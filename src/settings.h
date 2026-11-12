#pragma once
#include <windows.h>
#include <string>

enum class BootWriteMode : int {
    AsImage = 0,
    Uefi = 1,
    Legacy = 2,
    Both = 3
};

inline const wchar_t* BootWriteModeName(BootWriteMode mode) {
    switch (mode) {
    case BootWriteMode::Uefi: return L"UEFI";
    case BootWriteMode::Legacy: return L"Legacy";
    case BootWriteMode::Both: return L"UEFI + Legacy";
    default: return L"как в образе";
    }
}

struct PluginSettings {
    bool verifyAfterWrite = true;
    bool autoConfirm = false;
    BootWriteMode bootMode = BootWriteMode::AsImage;
};

class SettingsManager {
public:
    static std::wstring GetIniPath();
    static PluginSettings Load();
    static bool Save(const PluginSettings& settings);
    static bool ShowSettingsDialog(HWND hParent, HINSTANCE hInstance);
};
