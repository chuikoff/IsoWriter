#include "write_dialog.h"
#include "settings.h"
#include "../res/resource.h"

namespace {

struct DialogContext {
    const IsoFileInfo* isoInfo;
    const UsbDriveInfo* driveInfo;
    WriteDialogResult result;
};

bool PlainIso(const IsoFileInfo& iso) {
    return iso.hasIso9660 && !iso.hasMbr && !iso.hasGpt;
}

bool ModeAllowed(const IsoFileInfo& iso, BootWriteMode mode) {
    if (PlainIso(iso)) return true;
    bool uefi = iso.hasEfiPartition || iso.hasGpt;
    bool legacy = iso.hasLegacyPartition;
    if (mode == BootWriteMode::Uefi) return uefi;
    if (mode == BootWriteMode::Legacy) return legacy;
    if (mode == BootWriteMode::Both) return uefi && legacy;
    return true;
}

int RadioForMode(BootWriteMode mode) {
    if (mode == BootWriteMode::Uefi) return IDC_RADIO_BOOT_UEFI;
    if (mode == BootWriteMode::Legacy) return IDC_RADIO_BOOT_LEGACY;
    if (mode == BootWriteMode::Both) return IDC_RADIO_BOOT_BOTH;
    return IDC_RADIO_BOOT_IMAGE;
}

BootWriteMode ModeFromRadio(HWND hDlg) {
    if (IsDlgButtonChecked(hDlg, IDC_RADIO_BOOT_UEFI) == BST_CHECKED) return BootWriteMode::Uefi;
    if (IsDlgButtonChecked(hDlg, IDC_RADIO_BOOT_LEGACY) == BST_CHECKED) return BootWriteMode::Legacy;
    if (IsDlgButtonChecked(hDlg, IDC_RADIO_BOOT_BOTH) == BST_CHECKED) return BootWriteMode::Both;
    return BootWriteMode::AsImage;
}

void UpdateBootDesc(HWND hDlg, const IsoFileInfo& iso) {
    BootWriteMode mode = ModeFromRadio(hDlg);
    const wchar_t* text = L"Байты образа пишутся как есть. Флаги загрузки не меняются.";
    if (PlainIso(iso)) {
        if (mode == BootWriteMode::Uefi) {
            text = L"В ISO нет таблицы разделов, побайтовая запись даёт RAW. Файлы копируются на FAT32, UEFI берёт \\efi\\boot\\bootx64.efi.";
        } else if (mode == BootWriteMode::Legacy) {
            text = L"Файлы копируются на раздел. Если в образе есть файл больше 4 ГБ, раздел будет NTFS, иначе FAT32. BIOS ставит bootsect.exe из образа.";
        } else if (mode == BootWriteMode::Both) {
            text = L"Файлы на FAT32, раздел активный, bootsect для BIOS и \\efi\\boot для UEFI. install.wim больше 4 ГБ разрезается. Это не RAW и не побайтовая копия.";
        } else {
            text = L"В ISO нет таблицы разделов, побайтовая запись даёт RAW. «Как в образе» копирует файлы на FAT32 и включает UEFI и Legacy сразу. Образ с MBR по-прежнему пишется байт в байт.";
        }
    } else if (!iso.hasMbr && !iso.hasGpt) {
        text = L"В образе нет таблицы разделов. UEFI, Legacy и оба меняют только флаг активного раздела в MBR, поэтому здесь доступна побайтовая запись.";
    } else if (mode == BootWriteMode::Uefi) {
        text = L"После записи снимается флаг активной загрузки BIOS. Раздел EFI остаётся. Файлы из ISO отдельно не копируются.";
    } else if (mode == BootWriteMode::Legacy) {
        text = L"После записи первый обычный раздел помечается активным (0x80) для BIOS. Файлы из ISO отдельно не копируются.";
    } else if (mode == BootWriteMode::Both) {
        text = L"Обычный раздел помечается активным, раздел EFI сохраняется. Оба загрузчика должны уже быть в образе.";
    }
    SetDlgItemTextW(hDlg, IDC_STATIC_BOOT_DESC, text);
}

INT_PTR CALLBACK DialogProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_INITDIALOG: {
        DialogContext* ctx = (DialogContext*)lParam;
        SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)ctx);

        // Populate Image Info
        SetDlgItemTextW(hDlg, IDC_STATIC_IMAGE_NAME, ctx->isoInfo->fileName.c_str());
        SetDlgItemTextW(hDlg, IDC_STATIC_IMAGE_SIZE, DriveEnumerator::FormatSize(ctx->isoInfo->fileSize).c_str());
        SetDlgItemTextW(hDlg, IDC_STATIC_BOOT_TYPE, ctx->isoInfo->bootDescription.c_str());

        // Populate Target Drive Info
        std::wstring driveTitle = ctx->driveInfo->vendor;
        if (!driveTitle.empty() && !ctx->driveInfo->product.empty()) driveTitle += L" ";
        driveTitle += ctx->driveInfo->product;
        if (driveTitle.empty()) driveTitle = L"USB Drive " + std::to_wstring(ctx->driveInfo->physicalDriveIndex);
        driveTitle += L" (" + ctx->driveInfo->devicePath + L")";

        SetDlgItemTextW(hDlg, IDC_STATIC_TARGET_NAME, driveTitle.c_str());
        SetDlgItemTextW(hDlg, IDC_STATIC_TARGET_CAPACITY, DriveEnumerator::FormatSize(ctx->driveInfo->totalSizeBytes).c_str());

        std::wstring letters = ctx->driveInfo->driveLetters;
        if (letters.empty()) letters = L"(нет назначенных букв / RAW)";
        SetDlgItemTextW(hDlg, IDC_STATIC_TARGET_LETTERS, letters.c_str());

        PluginSettings s = SettingsManager::Load();
        EnableWindow(GetDlgItem(hDlg, IDC_RADIO_BOOT_UEFI), ModeAllowed(*ctx->isoInfo, BootWriteMode::Uefi));
        EnableWindow(GetDlgItem(hDlg, IDC_RADIO_BOOT_LEGACY), ModeAllowed(*ctx->isoInfo, BootWriteMode::Legacy));
        EnableWindow(GetDlgItem(hDlg, IDC_RADIO_BOOT_BOTH), ModeAllowed(*ctx->isoInfo, BootWriteMode::Both));
        BootWriteMode mode = s.bootMode;
        if (!ModeAllowed(*ctx->isoInfo, mode)) mode = BootWriteMode::AsImage;
        CheckRadioButton(hDlg, IDC_RADIO_BOOT_IMAGE, IDC_RADIO_BOOT_BOTH, RadioForMode(mode));
        if (PlainIso(*ctx->isoInfo)) {
            SetDlgItemTextW(hDlg, IDC_RADIO_BOOT_IMAGE, L"Как в образе (FAT32)");
            SetDlgItemTextW(hDlg, IDC_CHECK_VERIFY, L"Проверить, что файлы загрузки на месте");
        }
        UpdateBootDesc(hDlg, *ctx->isoInfo);

        CheckDlgButton(hDlg, IDC_CHECK_VERIFY, s.verifyAfterWrite ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CHECK_CONFIRM, BST_UNCHECKED);
        EnableWindow(GetDlgItem(hDlg, IDOK), FALSE);

        return TRUE;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hwndCtl = (HWND)lParam;
        if (GetDlgCtrlID(hwndCtl) == IDC_STATIC_WARNING) {
            SetTextColor(hdc, RGB(210, 30, 30));
            SetBkMode(hdc, TRANSPARENT);
            return (INT_PTR)GetStockObject(NULL_BRUSH);
        }
        break;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);

        DialogContext* ctx = (DialogContext*)GetWindowLongPtr(hDlg, DWLP_USER);

        if ((wmId == IDC_RADIO_BOOT_IMAGE || wmId == IDC_RADIO_BOOT_UEFI ||
             wmId == IDC_RADIO_BOOT_LEGACY || wmId == IDC_RADIO_BOOT_BOTH) &&
            wmEvent == BN_CLICKED && ctx && ctx->isoInfo) {
            UpdateBootDesc(hDlg, *ctx->isoInfo);
            return TRUE;
        }

        if (wmId == IDC_CHECK_CONFIRM && wmEvent == BN_CLICKED) {
            BOOL isChecked = (IsDlgButtonChecked(hDlg, IDC_CHECK_CONFIRM) == BST_CHECKED);
            EnableWindow(GetDlgItem(hDlg, IDOK), isChecked);
            return TRUE;
        }

        if (wmId == IDC_BTN_SAVE_DEFAULTS && wmEvent == BN_CLICKED) {
            PluginSettings s = SettingsManager::Load();
            s.verifyAfterWrite = (IsDlgButtonChecked(hDlg, IDC_CHECK_VERIFY) == BST_CHECKED);
            s.bootMode = ModeFromRadio(hDlg);
            SettingsManager::Save(s);
            MessageBoxW(hDlg, L"Текущие параметры сохранены как настройки по умолчанию.",
                        L"IsoWriter", MB_OK | MB_ICONINFORMATION);
            return TRUE;
        }

        if (wmId == IDOK) {
            if (ctx) {
                ctx->result.verify = (IsDlgButtonChecked(hDlg, IDC_CHECK_VERIFY) == BST_CHECKED);
                ctx->result.bootMode = ModeFromRadio(hDlg);
                ctx->result.confirmed = true;
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        if (wmId == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

} // namespace

bool WriteDialog::Show(HWND hParent, HINSTANCE hInstance,
                       const IsoFileInfo& isoInfo,
                       const UsbDriveInfo& driveInfo,
                       WriteDialogResult& outResult) {
    DialogContext ctx = {};
    ctx.isoInfo = &isoInfo;
    ctx.driveInfo = &driveInfo;
    ctx.result.verify = true;
    ctx.result.confirmed = false;

    INT_PTR res = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_WRITE_DIALOG),
                                  hParent, DialogProc, (LPARAM)&ctx);
    if (res == IDOK && ctx.result.confirmed) {
        outResult = ctx.result;
        return true;
    }
    return false;
}
