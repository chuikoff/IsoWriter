#include "settings.h"
#include "../res/resource.h"

extern HINSTANCE g_hInstance;

namespace {

INT_PTR CALLBACK SettingsDialogProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_INITDIALOG: {
        PluginSettings s = SettingsManager::Load();

        // Verify checkbox
        CheckDlgButton(hDlg, IDC_CHECK_DEFAULT_VERIFY, s.verifyAfterWrite ? BST_CHECKED : BST_UNCHECKED);

        HWND hBoot = GetDlgItem(hDlg, IDC_COMBO_DEFAULT_BOOT);
        SendMessageW(hBoot, CB_ADDSTRING, 0, (LPARAM)L"Как в образе (DD)");
        SendMessageW(hBoot, CB_ADDSTRING, 0, (LPARAM)L"UEFI");
        SendMessageW(hBoot, CB_ADDSTRING, 0, (LPARAM)L"Legacy");
        SendMessageW(hBoot, CB_ADDSTRING, 0, (LPARAM)L"UEFI и Legacy");
        int bootSel = (int)s.bootMode;
        if (bootSel < 0 || bootSel > 3) bootSel = 0;
        SendMessageW(hBoot, CB_SETCURSEL, bootSel, 0);

        return TRUE;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == IDOK) {
            PluginSettings s = SettingsManager::Load();

            // Verify
            s.verifyAfterWrite = (IsDlgButtonChecked(hDlg, IDC_CHECK_DEFAULT_VERIFY) == BST_CHECKED);
            int bootSel = (int)SendMessageW(GetDlgItem(hDlg, IDC_COMBO_DEFAULT_BOOT), CB_GETCURSEL, 0, 0);
            if (bootSel < 0 || bootSel > 3) bootSel = 0;
            s.bootMode = (BootWriteMode)bootSel;

            SettingsManager::Save(s);
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

std::wstring SettingsManager::GetIniPath() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(g_hInstance, path, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
    }
    return std::wstring(path) + L"IsoWriter.ini";
}

PluginSettings SettingsManager::Load() {
    PluginSettings s;
    std::wstring ini = GetIniPath();

    int verify = GetPrivateProfileIntW(L"Settings", L"DefaultVerify", 1, ini.c_str());
    s.verifyAfterWrite = (verify != 0);

    int boot = GetPrivateProfileIntW(L"Settings", L"BootMode", 0, ini.c_str());
    if (boot < 0 || boot > 3) boot = 0;
    s.bootMode = (BootWriteMode)boot;

    return s;
}

bool SettingsManager::Save(const PluginSettings& settings) {
    std::wstring ini = GetIniPath();

    WritePrivateProfileStringW(L"Settings", L"DefaultVerify",
                               settings.verifyAfterWrite ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Settings", L"BootMode",
                               std::to_wstring((int)settings.bootMode).c_str(), ini.c_str());

    return true;
}

bool SettingsManager::ShowSettingsDialog(HWND hParent, HINSTANCE hInstance) {
    INT_PTR res = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_SETTINGS_DIALOG),
                                  hParent, SettingsDialogProc, 0);
    return (res == IDOK);
}
