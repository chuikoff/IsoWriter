#pragma once
#include <windows.h>
#include <string>

inline constexpr const wchar_t* kPluginVersion = L"0.2";

enum class BootWriteMode {
    AsImage = 0,
    Uefi = 1,
    Legacy = 2,
    Both = 3
};

inline const wchar_t* BootWriteModeName(BootWriteMode mode) {
    switch (mode) {
    case BootWriteMode::Uefi: return L"UEFI";
    case BootWriteMode::Legacy: return L"Legacy";
    case BootWriteMode::Both: return L"UEFI и Legacy";
    default: return L"Как в образе (DD)";
    }
}

struct PluginSettings {
    bool verifyAfterWrite = true;
    BootWriteMode bootMode = BootWriteMode::AsImage;
    int bufferSizeKb = 1024;
};

class SettingsManager {
public:
    static std::wstring GetIniPath();
    static PluginSettings Load();
    static bool Save(const PluginSettings& settings);
    static bool ShowSettingsDialog(HWND hParent, HINSTANCE hInstance);
};
