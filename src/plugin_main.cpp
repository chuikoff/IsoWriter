#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include "../include/fsplugin.h"
#include "../res/resource.h"
#include "drive_enum.h"
#include "iso_detect.h"
#include "raw_writer.h"
#include "write_dialog.h"
#include "settings.h"

// Global plugin state
HINSTANCE g_hInstance = NULL;
HWND g_hTcWnd = NULL;
DWORD g_mainThreadId = 0;
int g_PluginNr = 0;
tProgressProcW g_pProgressProcW = NULL;
tLogProcW g_pLogProcW = NULL;
tRequestProcW g_pRequestProcW = NULL;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hInstance = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

namespace {

WriteResult RunWrite(const WriteParams& params, const ProgressFn& progress, std::wstring& error) {
    if (params.unpackPlainIso) return RawWriter::UnpackIso(params, progress, error);
    return RawWriter::BurnImage(params, progress, error);
}

std::wstring ResultText(const WriteParams& params, WriteResult result, const std::wstring& error) {
    if (result == WriteResult::Success) {
        if (params.unpackPlainIso) {
            BootWriteMode shown = params.bootMode == BootWriteMode::AsImage ? BootWriteMode::Both : params.bootMode;
            std::wstring text = L"Файлы образа скопированы на накопитель " + params.targetDrive.tcFolderName +
                                L".\nWindows должен видеть раздел, а не RAW.";
            text += L"\nРежим загрузки: ";
            text += BootWriteModeName(shown);
            text += L".";
            if (params.verifyAfterWrite) text += L"\nФайлы загрузки на месте.";
            if (!error.empty()) text += L"\n" + error;
            return text;
        }
        std::wstring text = L"Образ записан на накопитель " + params.targetDrive.tcFolderName + L".";
        if (params.verifyAfterWrite) text += L"\nПроверка совпала с файлом образа.";
        if (params.bootMode != BootWriteMode::AsImage) {
            text += L"\nФлаги загрузки: ";
            text += BootWriteModeName(params.bootMode);
            text += L".";
        }
        return text;
    }
    if (result == WriteResult::Cancelled) {
        return error.empty() ? L"Запись отменена." : error;
    }
    std::wstring text = error.empty() ? L"Ошибка записи." : error;
    text += L"\n\nЛог: " + RawWriter::LogFilePath();
    return text;
}

HWND FindTcMainWindow() {
    HWND hWnd = FindWindowW(L"TTOTAL_CMD", NULL);
    if (!hWnd) {
        hWnd = GetActiveWindow();
    }
    return hWnd;
}

struct SearchContext {
    std::wstring path;
    std::vector<WIN32_FIND_DATAW> entries;
    size_t currentIndex = 0;
};

// Check if file has an image extension
bool IsImageExtension(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (wchar_t& c : ext) c = towlower(c);
    return (ext == L".iso" || ext == L".img" || ext == L".raw" || ext == L".bin");
}

// Extract physical drive index from remote path e.g. "\[USB 2] Kingston..."
int ExtractDriveIndexFromPath(const std::wstring& remotePath) {
    size_t pos = remotePath.find(L"[USB ");
    if (pos == std::wstring::npos) {
        pos = remotePath.find(L"USB ");
        if (pos != std::wstring::npos) pos += 4;
    } else {
        pos += 5;
    }

    if (pos != std::wstring::npos && pos < remotePath.size()) {
        try {
            return std::stoi(remotePath.substr(pos));
        } catch (...) {}
    }
    return -1;
}

} // namespace

// ============================================================================
// Total Commander WFX Plugin Exports
// ============================================================================

extern "C" {

__declspec(dllexport) int __stdcall FsInitW(int PluginNr,
                                           tProgressProcW pProgressProc,
                                           tLogProcW pLogProc,
                                           tRequestProcW pRequestProc) {
    g_PluginNr = PluginNr;
    g_pProgressProcW = pProgressProc;
    g_pLogProcW = pLogProc;
    g_pRequestProcW = pRequestProc;
    g_mainThreadId = GetCurrentThreadId();
    g_hTcWnd = FindTcMainWindow();
    return 0;
}

// BG_UPLOAD: F5's "В фоне" runs FsPutFileW on TC's background thread and shows its own transfer list.
__declspec(dllexport) int __stdcall FsGetBackgroundFlags(void) {
    return BG_UPLOAD;
}

__declspec(dllexport) int __stdcall FsInit(int PluginNr,
                                          tProgressProc pProgressProc,
                                          tLogProc pLogProc,
                                          tRequestProc pRequestProc) {
    g_PluginNr = PluginNr;
    g_mainThreadId = GetCurrentThreadId();
    g_hTcWnd = FindTcMainWindow();
    return 0;
}

__declspec(dllexport) void __stdcall FsGetDefRootName(char* DefRootName, int maxlen) {
    if (DefRootName && maxlen > 0) {
        strncpy_s(DefRootName, maxlen, "ISO USB Writer", _TRUNCATE);
    }
}

__declspec(dllexport) HANDLE __stdcall FsFindFirstW(WCHAR* Path, WIN32_FIND_DATAW* FindData) {
    if (!Path || !FindData) return INVALID_HANDLE_VALUE;

    std::wstring p = Path;
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) {
        p.pop_back();
    }

    auto ctx = std::make_unique<SearchContext>();
    ctx->path = p;

    if (p.empty() || p == L"\\" || p == L"/") {
        // Root directory: enumerate USB drives as folders
        auto drives = DriveEnumerator::EnumerateUsbDrives();

        for (const auto& d : drives) {
            WIN32_FIND_DATAW fd = {0};
            fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            wcsncpy_s(fd.cFileName, MAX_PATH, d.tcFolderName.c_str(), _TRUNCATE);
            ctx->entries.push_back(fd);
        }

        // Add special settings item
        WIN32_FIND_DATAW fdSet = {0};
        fdSet.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        wcsncpy_s(fdSet.cFileName, MAX_PATH, L"[Настройки по умолчанию (Enter для открытия)]", _TRUNCATE);
        ctx->entries.push_back(fdSet);

        // If no drives found, show a helpful dummy item
        if (drives.empty()) {
            WIN32_FIND_DATAW fd = {0};
            fd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_READONLY;
            wcsncpy_s(fd.cFileName, MAX_PATH, L"[USB накопители не обнаружены — подключите флешку и нажмите Ctrl+R].txt", _TRUNCATE);
            ctx->entries.push_back(fd);
        }
    } else {
        // Inside a drive directory: show helpful drop target hint
        WIN32_FIND_DATAW fd = {0};
        fd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_READONLY;
        wcsncpy_s(fd.cFileName, MAX_PATH, L"[Скопируйте сюда .ISO файл для записи на этот USB-диск].txt", _TRUNCATE);
        ctx->entries.push_back(fd);
    }

    if (ctx->entries.empty()) {
        return INVALID_HANDLE_VALUE;
    }

    *FindData = ctx->entries[0];
    ctx->currentIndex = 1;
    return (HANDLE)ctx.release();
}

__declspec(dllexport) BOOL __stdcall FsFindNextW(HANDLE Hdl, WIN32_FIND_DATAW* FindData) {
    if (!Hdl || Hdl == INVALID_HANDLE_VALUE || !FindData) return FALSE;

    SearchContext* ctx = (SearchContext*)Hdl;
    if (ctx->currentIndex >= ctx->entries.size()) {
        return FALSE;
    }

    *FindData = ctx->entries[ctx->currentIndex++];
    return TRUE;
}

__declspec(dllexport) int __stdcall FsFindClose(HANDLE Hdl) {
    if (Hdl && Hdl != INVALID_HANDLE_VALUE) {
        SearchContext* ctx = (SearchContext*)Hdl;
        delete ctx;
    }
    return 0;
}

__declspec(dllexport) int __stdcall FsExtractCustomIconW(WCHAR* RemoteName, int ExtractFlags, HICON* TheIcon) {
    if (!TheIcon) return FS_ICON_USEDEFAULT;

    int size = (ExtractFlags & FS_ICONFLAG_SMALL) ? 16 : 32;
    *TheIcon = (HICON)LoadImageW(g_hInstance, MAKEINTRESOURCEW(IDI_PLUGIN_ICON),
                                IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
    if (*TheIcon) {
        return FS_ICON_EXTRACTED_DESTROY;
    }
    return FS_ICON_USEDEFAULT;
}

__declspec(dllexport) int __stdcall FsExtractCustomIcon(char* RemoteName, int ExtractFlags, HICON* TheIcon) {
    if (!TheIcon) return FS_ICON_USEDEFAULT;

    int size = (ExtractFlags & FS_ICONFLAG_SMALL) ? 16 : 32;
    *TheIcon = (HICON)LoadImageW(g_hInstance, MAKEINTRESOURCEW(IDI_PLUGIN_ICON),
                                IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
    if (*TheIcon) {
        return FS_ICON_EXTRACTED_DESTROY;
    }
    return FS_ICON_USEDEFAULT;
}

__declspec(dllexport) int __stdcall FsPutFileW(WCHAR* LocalName, WCHAR* RemoteName, int CopyFlags) {
    if (!LocalName || !RemoteName) {
        return FS_FILE_WRITEERROR;
    }

    bool inBackground = g_mainThreadId != 0 && GetCurrentThreadId() != g_mainThreadId;
    HWND hParent = inBackground ? NULL : (g_hTcWnd ? g_hTcWnd : FindTcMainWindow());

    // 1. Verify extension
    std::wstring localPath = LocalName;
    if (!IsImageExtension(localPath)) {
        int choice = MessageBoxW(
            hParent,
            L"Выбранный файл не имеет стандартного расширения дискового образа (.iso, .img, .raw, .bin).\n\n"
            L"Вы уверены, что хотите продолжить запись этого файла как побайтового образа?",
            L"Предупреждение (IsoWriter)",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (choice != IDYES) {
            return FS_FILE_USERABORT;
        }
    }

    // 2. Identify target USB drive
    std::wstring remotePath = RemoteName;
    int targetIdx = ExtractDriveIndexFromPath(remotePath);

    auto drives = DriveEnumerator::EnumerateUsbDrives();
    UsbDriveInfo targetDrive;
    bool found = false;

    for (const auto& d : drives) {
        if (targetIdx >= 0 && d.physicalDriveIndex == targetIdx) {
            targetDrive = d;
            found = true;
            break;
        }
        if (remotePath.find(d.tcFolderName) != std::wstring::npos) {
            targetDrive = d;
            found = true;
            break;
        }
    }

    if (!found) {
        MessageBoxW(hParent,
                    L"Не удалось определить целевой USB накопитель для записи.\n"
                    L"Пожалуйста, обновите список дисков (Ctrl+R) и повторите попытку.",
                    L"Ошибка (IsoWriter)",
                    MB_OK | MB_ICONERROR);
        return FS_FILE_WRITEERROR;
    }

    // 3. Check Administrator elevation
    if (!DriveEnumerator::IsElevated()) {
        MessageBoxW(
            hParent,
            L"Для прямой побайтовой записи на физический накопитель требуются права Администратора.\n\n"
            L"Пожалуйста, перезапустите Total Commander от имени Администратора (Запуск от имени администратора).",
            L"Требуются права Администратора (IsoWriter)",
            MB_OK | MB_ICONEXCLAMATION);
        return FS_FILE_USERABORT;
    }

    // 4. Analyze ISO Image (MBR / GPT / UEFI / Hybrid)
    IsoFileInfo isoInfo;
    if (!IsoDetector::Analyze(localPath, isoInfo)) {
        MessageBoxW(hParent,
                    L"Не удалось прочитать или проанализировать исходный файл образа.",
                    L"Ошибка (IsoWriter)",
                    MB_OK | MB_ICONERROR);
        return FS_FILE_READERROR;
    }

    // 5. Show Confirmation & Configuration Dialog
    WriteDialogResult dlgResult;
    if (!WriteDialog::Show(hParent, g_hInstance, isoInfo, targetDrive, dlgResult)) {
        return FS_FILE_USERABORT;
    }

    // 6. Execute Raw Burn
    WriteParams params;
    params.sourceIsoPath = localPath;
    params.targetDrive = targetDrive;
    params.verifyAfterWrite = dlgResult.verify;
    params.bootMode = dlgResult.bootMode;
    params.unpackPlainIso = isoInfo.hasIso9660 && !isoInfo.hasMbr && !isoInfo.hasGpt;

    auto progressCallback = [&](int percentDone, const std::wstring& statusText) -> bool {
        if (g_pProgressProcW) {
            std::wstring displayTarget = targetDrive.tcFolderName + L" (" + statusText + L")";
            int tcResult = g_pProgressProcW(g_PluginNr, LocalName, (WCHAR*)displayTarget.c_str(), percentDone);
            if (tcResult != 0) {
                // User pressed Cancel in Total Commander progress dialog
                return false;
            }
        }
        return true;
    };

    std::wstring errorMsg;
    WriteResult result = RunWrite(params, progressCallback, errorMsg);
    if (result != WriteResult::Success && !errorMsg.empty()) {
        errorMsg += L"\n\nЛог: " + RawWriter::LogFilePath();
    }

    if (result == WriteResult::Success) {
        if (!inBackground) {
            std::wstring successMsg = ResultText(params, result, errorMsg);
            MessageBoxW(hParent, successMsg.c_str(), L"Запись завершена (IsoWriter)", MB_OK | MB_ICONINFORMATION);
        }
        return FS_FILE_OK;
    } else if (result == WriteResult::Cancelled) {
        if (!errorMsg.empty()) {
            MessageBoxW(hParent, errorMsg.c_str(), L"IsoWriter", MB_OK | MB_ICONWARNING);
        }
        return FS_FILE_USERABORT;
    } else {
        MessageBoxW(hParent, errorMsg.c_str(), L"Ошибка записи (IsoWriter)", MB_OK | MB_ICONERROR);
        return FS_FILE_WRITEERROR;
    }
}

__declspec(dllexport) int __stdcall FsGetFileW(WCHAR* RemoteName, WCHAR* LocalName, int CopyFlags, RemoteInfoStruct* ri) {
    return FS_FILE_NOTSUPPORTED;
}

__declspec(dllexport) BOOL __stdcall FsDeleteFileW(WCHAR* RemoteName) {
    return FALSE;
}

__declspec(dllexport) BOOL __stdcall FsRemoveDirW(WCHAR* RemoteName) {
    return FALSE;
}

__declspec(dllexport) BOOL __stdcall FsMkDirW(WCHAR* Path) {
    return FALSE;
}

__declspec(dllexport) int __stdcall FsExecuteFileW(HWND MainWin, WCHAR* RemoteName, WCHAR* Verb) {
    if (RemoteName && wcsstr(RemoteName, L"[Настройки") != NULL) {
        SettingsManager::ShowSettingsDialog(MainWin ? MainWin : g_hTcWnd, g_hInstance);
        return FS_EXEC_OK;
    }
    return FS_EXEC_OK;
}

} // extern "C"
