#include "write_dialog.h"
#include "settings.h"
#include "../res/resource.h"

namespace {

struct DialogContext {
    const IsoFileInfo* isoInfo;
    const UsbDriveInfo* driveInfo;
    WriteDialogResult result;
};

int RadioForMode(BootWriteMode mode) {
    switch (mode) {
    case BootWriteMode::Uefi: return IDC_RADIO_BOOT_UEFI;
    case BootWriteMode::Legacy: return IDC_RADIO_BOOT_LEGACY;
    case BootWriteMode::Both: return IDC_RADIO_BOOT_BOTH;
    default: return IDC_RADIO_BOOT_IMAGE;
    }
}

BootWriteMode ModeFromRadio(HWND hDlg) {
    if (IsDlgButtonChecked(hDlg, IDC_RADIO_BOOT_UEFI) == BST_CHECKED) return BootWriteMode::Uefi;
    if (IsDlgButtonChecked(hDlg, IDC_RADIO_BOOT_LEGACY) == BST_CHECKED) return BootWriteMode::Legacy;
    if (IsDlgButtonChecked(hDlg, IDC_RADIO_BOOT_BOTH) == BST_CHECKED) return BootWriteMode::Both;
    return BootWriteMode::AsImage;
}

void UpdateBootDesc(HWND hDlg) {
    BootWriteMode mode = ModeFromRadio(hDlg);
    const wchar_t* text = L"";
    switch (mode) {
    case BootWriteMode::AsImage:
        text = L"Прямая побайтовая копия (DD). Флаги и структура образа переносятся без изменений.";
        break;
    case BootWriteMode::Uefi:
        text = L"Режим UEFI: образ пишется побайтно, флаг загрузки BIOS (0x80) в MBR сбрасывается.";
        break;
    case BootWriteMode::Legacy:
        text = L"Режим Legacy BIOS: образ пишется побайтно, основной раздел помечается активным (0x80) в MBR.";
        break;
    case BootWriteMode::Both:
        text = L"Режим UEFI и Legacy: образ пишется побайтно, раздел помечается активным, раздел EFI сохраняется.";
        break;
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

        // Always enable all boot modes and load default settings
        EnableWindow(GetDlgItem(hDlg, IDC_RADIO_BOOT_IMAGE), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_RADIO_BOOT_UEFI), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_RADIO_BOOT_LEGACY), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_RADIO_BOOT_BOTH), TRUE);

        PluginSettings s = SettingsManager::Load();
        CheckRadioButton(hDlg, IDC_RADIO_BOOT_IMAGE, IDC_RADIO_BOOT_BOTH, RadioForMode(s.bootMode));
        CheckDlgButton(hDlg, IDC_CHECK_VERIFY, s.verifyAfterWrite ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CHECK_CONFIRM, BST_UNCHECKED);
        EnableWindow(GetDlgItem(hDlg, IDOK), FALSE);

        UpdateBootDesc(hDlg);
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
            wmEvent == BN_CLICKED) {
            UpdateBootDesc(hDlg);
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
