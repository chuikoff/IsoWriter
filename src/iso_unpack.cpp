#include "raw_writer.h"
#include "drive_enum.h"

#include <windows.h>
#include <virtdisk.h>
#include <winioctl.h>
#include <shlobj.h>

#include <algorithm>
#include <string>
#include <vector>
#include <functional>

// A Windows install ISO has no MBR. A byte copy leaves sector 0 empty, so Disk Management shows RAW.
// Mount the ISO, partition the USB, and copy the files. Images that already have an MBR stay on BurnImage.

namespace {

const uint64_t kFat32MaxFile = 1ull << 32;
const uint64_t kFat32DiskMax = 32ull << 30;
const GUID kVendorMicrosoft =
    {0xec984aec, 0xa0f9, 0x47e9, {0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b}};

enum class CopyFs { Fat32, Ntfs };

struct CopyPlan {
    BootWriteMode effective = BootWriteMode::AsImage;
    CopyFs fs = CopyFs::Fat32;
    bool bootsect = false;
    bool refuse = false;
};

struct BigFile {
    std::wstring full;
    std::wstring name;
    uint64_t size = 0;
    bool split = false;
};

enum class RunKind { Done, Cancelled, StartFailed };

std::wstring ErrText(DWORD code) {
    wchar_t* buf = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               NULL, code, 0, (LPWSTR)&buf, 0, NULL);
    std::wstring text = L"код " + std::to_wstring(code);
    if (len && buf) {
        text += L": ";
        text.append(buf, len);
        LocalFree(buf);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    }
    return text;
}

WriteResult Fail(std::wstring& out, const std::wstring& msg, WriteResult result) {
    out = msg;
    RawWriter::Log(msg);
    return result;
}

bool Step(const ProgressFn& progress, int percent, const std::wstring& status) {
    if (!progress) return true;
    return progress(percent, status);
}

bool SplitName(const std::wstring& name) {
    return _wcsicmp(name.c_str(), L"install.wim") == 0 || _wcsicmp(name.c_str(), L"install.esd") == 0;
}

CopyPlan MakePlan(BootWriteMode mode, bool anyBig, bool nonSplit) {
    CopyPlan plan;
    plan.effective = mode == BootWriteMode::AsImage ? BootWriteMode::Both : mode;
    plan.bootsect = plan.effective == BootWriteMode::Legacy || plan.effective == BootWriteMode::Both;
    bool ntfs = plan.effective == BootWriteMode::Legacy && anyBig;
    plan.fs = ntfs ? CopyFs::Ntfs : CopyFs::Fat32;
    plan.refuse = plan.fs == CopyFs::Fat32 && nonSplit;
    return plan;
}

size_t CountOf(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string SanitizeVolumeLabel(const std::wstring& wlabel, CopyFs fs) {
    if (wlabel.empty()) return "WINISO";
    std::string s;
    for (wchar_t wc : wlabel) {
        if (wc > 32 && wc < 127 && wc != '/' && wc != '\\' && wc != ':' && wc != '*' &&
            wc != '?' && wc != '"' && wc != '<' && wc != '>' && wc != '|' && wc != ' ') {
            s += (char)toupper(wc);
        }
    }
    if (s.empty()) s = "WINISO";
    size_t maxLen = (fs == CopyFs::Fat32) ? 11 : 32;
    if (s.size() > maxLen) s.resize(maxLen);
    return s;
}

std::string BuildScript(int disk, CopyFs fs, bool active, bool cap32, const std::string& volumeLabel = "WINISO") {
    std::string script;
    script += "select disk " + std::to_string(disk) + "\r\n";
    script += "clean\r\n";
    script += "convert mbr\r\n";
    // shortcut: diskpart refuses FAT32 above 32GB, so a larger stick gets a 32000 MB partition
    script += cap32 ? "create partition primary size=32000\r\n" : "create partition primary\r\n";
    std::string lbl = volumeLabel.empty() ? "WINISO" : volumeLabel;
    script += fs == CopyFs::Ntfs ? ("format fs=ntfs quick label=" + lbl + "\r\n")
                                 : ("format fs=fat32 quick label=" + lbl + "\r\n");
    script += "assign\r\n";
    if (active) script += "active\r\n";
    return script;
}

bool CopyExitOk(DWORD code) { return code <= 7; }

bool PlanOk(BootWriteMode mode, bool anyBig, bool nonSplit,
            BootWriteMode wantMode, CopyFs wantFs, bool wantBoot, bool wantRefuse) {
    CopyPlan plan = MakePlan(mode, anyBig, nonSplit);
    return plan.effective == wantMode && plan.fs == wantFs && plan.bootsect == wantBoot && plan.refuse == wantRefuse;
}

bool UnpackPlanSelfCheck() {
    if (!PlanOk(BootWriteMode::AsImage, false, false, BootWriteMode::Both, CopyFs::Fat32, true, false)) return false;
    if (!PlanOk(BootWriteMode::AsImage, true, false, BootWriteMode::Both, CopyFs::Fat32, true, false)) return false;
    if (!PlanOk(BootWriteMode::AsImage, true, true, BootWriteMode::Both, CopyFs::Fat32, true, true)) return false;
    if (!PlanOk(BootWriteMode::Legacy, true, true, BootWriteMode::Legacy, CopyFs::Ntfs, true, false)) return false;
    if (!PlanOk(BootWriteMode::Legacy, false, false, BootWriteMode::Legacy, CopyFs::Fat32, true, false)) return false;
    if (!PlanOk(BootWriteMode::Uefi, true, false, BootWriteMode::Uefi, CopyFs::Fat32, false, false)) return false;
    if (!PlanOk(BootWriteMode::Both, true, true, BootWriteMode::Both, CopyFs::Fat32, true, true)) return false;
    if (!PlanOk(BootWriteMode::Uefi, true, true, BootWriteMode::Uefi, CopyFs::Fat32, false, true)) return false;

    std::string fat = BuildScript(4, CopyFs::Fat32, true, false);
    if (fat.find("select disk 4\r\n") == std::string::npos) return false;
    if (CountOf(fat, "select disk ") != 1 || CountOf(fat, "clean\r\n") != 1) return false;
    if (fat.find("convert mbr\r\n") == std::string::npos) return false;
    if (fat.find("format fs=fat32 quick label=WINISO\r\n") == std::string::npos) return false;
    if (fat.find("ntfs") != std::string::npos || fat.find("size=") != std::string::npos) return false;
    if (fat.find("active\r\n") == std::string::npos) return false;

    std::string ntfs = BuildScript(4, CopyFs::Ntfs, true, false);
    if (ntfs.find("format fs=ntfs quick label=WINISO\r\n") == std::string::npos) return false;
    if (ntfs.find("fat32") != std::string::npos) return false;

    std::string uefi = BuildScript(4, CopyFs::Fat32, false, true);
    if (uefi.find("create partition primary size=32000\r\n") == std::string::npos) return false;
    if (uefi.find("active") != std::string::npos) return false;
    if (CountOf(uefi, "select disk ") != 1) return false;

    return CopyExitOk(0) && CopyExitOk(1) && CopyExitOk(7) && !CopyExitOk(8) && !CopyExitOk(16);
}

std::wstring SystemExe(const wchar_t* name) {
    BOOL wow = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow) && wow) {
        wchar_t win[MAX_PATH] = {};
        UINT n = GetWindowsDirectoryW(win, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring path = std::wstring(win) + L"\\Sysnative\\" + name;
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
        }
    }
    wchar_t dir[MAX_PATH] = {};
    GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir) + L"\\" + name;
}

std::wstring Quote(const std::wstring& text) { return L"\"" + text + L"\""; }

std::wstring OemToWide(const std::string& text) {
    if (text.empty()) return L"";
    int n = MultiByteToWideChar(CP_OEMCP, 0, text.data(), (int)text.size(), NULL, 0);
    if (n <= 0) return L"";
    std::wstring wide(n, 0);
    MultiByteToWideChar(CP_OEMCP, 0, text.data(), (int)text.size(), wide.data(), n);
    return wide;
}

std::wstring LastLines(const std::wstring& text) {
    if (text.size() <= 800) return text;
    return text.substr(text.size() - 800);
}

RunKind RunProcess(const std::wstring& exe, const std::wstring& args, const std::wstring& workDir,
                   const ProgressFn& progress, const std::function<int()>& percent,
                   const std::wstring& status, std::wstring& output, DWORD& exitCode,
                   const std::function<std::wstring()>& statusFn = nullptr) {
    struct Handles {
        HANDLE job = NULL;
        HANDLE process = NULL;
        HANDLE thread = NULL;
        HANDLE readPipe = NULL;
        HANDLE writePipe = NULL;
        HANDLE nul = NULL;
        ~Handles() {
            if (writePipe) CloseHandle(writePipe);
            if (nul) CloseHandle(nul);
            if (readPipe) CloseHandle(readPipe);
            if (thread) CloseHandle(thread);
            if (process) CloseHandle(process);
            // shortcut: the job ends dismhost only when AssignProcessToJobObject succeeded
            if (job) CloseHandle(job);
        }
    } handles;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&handles.readPipe, &handles.writePipe, &sa, 0)) {
        output = L"Не удалось создать канал для " + exe + L": " + ErrText(GetLastError());
        return RunKind::StartFailed;
    }
    SetHandleInformation(handles.readPipe, HANDLE_FLAG_INHERIT, 0);
    handles.nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, NULL);

    handles.job = CreateJobObjectW(NULL, NULL);
    if (handles.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(handles.job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = handles.nul;
    si.hStdOutput = handles.writePipe;
    si.hStdError = handles.writePipe;
    std::wstring cmd = Quote(exe) + L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    PROCESS_INFORMATION pi = {};
    const wchar_t* dir = workDir.empty() ? NULL : workDir.c_str();
    if (!CreateProcessW(exe.c_str(), buf.data(), NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
        output = L"Не удалось запустить " + exe + L": " + ErrText(GetLastError());
        return RunKind::StartFailed;
    }
    CloseHandle(handles.writePipe);
    handles.writePipe = NULL;
    if (handles.nul) {
        CloseHandle(handles.nul);
        handles.nul = NULL;
    }
    if (handles.job && !AssignProcessToJobObject(handles.job, pi.hProcess)) {
        RawWriter::Log(L"Job assign failed err=" + std::to_wstring(GetLastError()));
    }
    ResumeThread(pi.hThread);
    handles.process = pi.hProcess;
    handles.thread = pi.hThread;

    std::string bytes;
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(handles.readPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            char chunk[1024];
            DWORD got = 0;
            DWORD take = avail > sizeof(chunk) ? (DWORD)sizeof(chunk) : avail;
            if (!ReadFile(handles.readPipe, chunk, take, &got, NULL) || got == 0) break;
            bytes.append(chunk, got);
            if (bytes.size() > 16000) bytes.erase(0, bytes.size() - 8000);
        }
        DWORD wait = WaitForSingleObject(handles.process, 200);
        if (wait == WAIT_FAILED) {
            output = L"Ожидание процесса сорвалось: " + ErrText(GetLastError());
            TerminateProcess(handles.process, 1);
            return RunKind::StartFailed;
        }
        if (wait == WAIT_OBJECT_0) {
            DWORD avail2 = 0;
            while (PeekNamedPipe(handles.readPipe, NULL, 0, NULL, &avail2, NULL) && avail2 > 0) {
                char chunk[1024];
                DWORD got = 0;
                DWORD take = avail2 > sizeof(chunk) ? (DWORD)sizeof(chunk) : avail2;
                if (!ReadFile(handles.readPipe, chunk, take, &got, NULL) || got == 0) break;
                bytes.append(chunk, got);
                if (bytes.size() > 16000) bytes.erase(0, bytes.size() - 8000);
            }
            exitCode = 1;
            GetExitCodeProcess(handles.process, &exitCode);
            output = OemToWide(bytes);
            return RunKind::Done;
        }
        int pct = percent ? percent() : 0;
        std::wstring curStatus = statusFn ? statusFn() : status;
        if (progress && !progress(pct, curStatus)) {
            TerminateProcess(handles.process, 1);
            WaitForSingleObject(handles.process, 3000);
            output = OemToWide(bytes);
            return RunKind::Cancelled;
        }
    }
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ConfirmTarget(const WriteParams& params, std::wstring& error) {
    if (params.targetDrive.physicalDriveIndex < 0) {
        error = L"Не выбран физический диск.";
        return false;
    }
    std::wstring expect = L"\\\\.\\PhysicalDrive" + std::to_wstring(params.targetDrive.physicalDriveIndex);
    if (_wcsicmp(params.targetDrive.devicePath.c_str(), expect.c_str()) != 0) {
        error = L"Путь накопителя не совпадает с номером диска. Запись отменена.";
        return false;
    }
    if (params.targetDrive.totalSizeBytes == 0) {
        error = L"Не удалось определить ёмкость накопителя. Запись отменена.";
        return false;
    }
    int sys = DriveEnumerator::GetSystemDriveIndex();
    if (sys < 0) {
        error = L"Не удалось определить системный диск. Запись отменена.";
        return false;
    }
    if (params.targetDrive.physicalDriveIndex == sys) {
        error = L"Критическая ошибка безопасности: попытка записи на системный диск!";
        return false;
    }

    HANDLE disk = CreateFileW(params.targetDrive.devicePath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (disk == INVALID_HANDLE_VALUE) {
        error = L"Не удалось открыть диск для проверки: " + ErrText(GetLastError());
        return false;
    }
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    BYTE prop[1024] = {};
    DWORD n = 0;
    BOOL busOk = DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), prop, sizeof(prop), &n, NULL);
    STORAGE_DEVICE_DESCRIPTOR* desc = (STORAGE_DEVICE_DESCRIPTOR*)prop;
    bool usb = busOk && (desc->BusType == BusTypeUsb || desc->BusType == BusTypeSd || desc->BusType == BusTypeMmc);

    GET_LENGTH_INFORMATION len = {};
    BOOL lenOk = DeviceIoControl(disk, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &len, sizeof(len), &n, NULL);
    DISK_GEOMETRY_EX geom = {};
    BOOL geomOk = DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geom, sizeof(geom), &n, NULL);
    CloseHandle(disk);

    uint64_t length = lenOk ? (uint64_t)len.Length.QuadPart : 0;
    uint64_t geometry = geomOk ? (uint64_t)geom.DiskSize.QuadPart : 0;
    RawWriter::Log(L"Recheck disk " + std::to_wstring(params.targetDrive.physicalDriveIndex) +
                   L" length=" + std::to_wstring(length) +
                   L" geometry=" + std::to_wstring(geometry) +
                   L" expected=" + std::to_wstring(params.targetDrive.totalSizeBytes) +
                   L" usb=" + std::to_wstring(usb));
    if (!usb) {
        error = L"Накопитель больше не USB, SD или MMC. Запись отменена.";
        return false;
    }
    if (lenOk && geomOk && length != geometry) {
        error = L"Два запроса размера диска разошлись. Запись отменена.";
        return false;
    }
    bool sizeOk = (lenOk && length == params.targetDrive.totalSizeBytes) ||
                  (!lenOk && geomOk && geometry == params.targetDrive.totalSizeBytes);
    if (!sizeOk) {
        error = L"Размер диска изменился. Запись отменена, чтобы не стереть другой накопитель.";
        return false;
    }
    return true;
}

std::wstring ReadIsoLabel(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER pos = {};
    pos.QuadPart = 32768 + 40;
    std::wstring label;
    char buf[32] = {};
    DWORD n = 0;
    if (SetFilePointerEx(file, pos, NULL, FILE_BEGIN) && ReadFile(file, buf, 32, &n, NULL) && n == 32) {
        int end = 31;
        while (end >= 0 && (buf[end] == ' ' || buf[end] == '\0')) --end;
        if (end >= 0) {
            int wlen = MultiByteToWideChar(CP_ACP, 0, buf, end + 1, NULL, 0);
            if (wlen > 0) {
                label.resize(wlen);
                MultiByteToWideChar(CP_ACP, 0, buf, end + 1, label.data(), wlen);
            }
        }
    }
    CloseHandle(file);
    return label;
}

wchar_t PickAddedLetter(DWORD before, DWORD after) {
    DWORD added = after & ~before;
    wchar_t any = 0;
    for (int i = 0; i < 26; ++i) {
        if ((added & (1u << i)) == 0) continue;
        wchar_t letter = (wchar_t)(L'A' + i);
        std::wstring root = std::wstring(1, letter) + L":\\";
        if (GetDriveTypeW(root.c_str()) == DRIVE_CDROM) return letter;
        if (!any) any = letter;
    }
    return any;
}

wchar_t FindCdByLabel(const std::wstring& label) {
    if (label.empty()) return 0;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        wchar_t letter = (wchar_t)(L'A' + i);
        std::wstring root = std::wstring(1, letter) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_CDROM) continue;
        wchar_t vol[64] = {};
        if (GetVolumeInformationW(root.c_str(), vol, 64, NULL, NULL, NULL, NULL, 0) &&
            _wcsicmp(vol, label.c_str()) == 0) {
            return letter;
        }
    }
    return 0;
}

struct IsoMount {
    HANDLE handle = NULL;
    ~IsoMount() {
        if (handle) CloseHandle(handle);
    }
};

wchar_t MountIso(const std::wstring& path, const ProgressFn& progress, IsoMount& mount, std::wstring& error) {
    DWORD before = GetLogicalDrives();
    VIRTUAL_STORAGE_TYPE type = {};
    type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;
    type.VendorId = kVendorMicrosoft;
    OPEN_VIRTUAL_DISK_PARAMETERS openParams = {};
    openParams.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    openParams.Version1.RWDepth = OPEN_VIRTUAL_DISK_RW_DEPTH_DEFAULT;
    HANDLE opened = NULL;
    DWORD status = OpenVirtualDisk(&type, path.c_str(), VIRTUAL_DISK_ACCESS_READ,
                                   OPEN_VIRTUAL_DISK_FLAG_NONE, &openParams, &opened);
    if (status == ERROR_SUCCESS && opened) {
        ATTACH_VIRTUAL_DISK_PARAMETERS attach = {};
        attach.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
        status = AttachVirtualDisk(opened, NULL, ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY, 0, &attach, NULL);
        if (status == ERROR_SUCCESS) {
            mount.handle = opened;
            for (int i = 0; i < 40; ++i) {
                wchar_t letter = PickAddedLetter(before, GetLogicalDrives());
                if (letter) return letter;
                if (!Step(progress, 5, L"Подключение ISO...")) {
                    error = L"Запись отменена.";
                    return 0;
                }
                Sleep(250);
            }
            error = L"ISO подключён, но буква диска не появилась.";
            return 0;
        }
        RawWriter::Log(L"AttachVirtualDisk failed " + ErrText(status));
        CloseHandle(opened);
    } else {
        RawWriter::Log(L"OpenVirtualDisk failed " + ErrText(status));
    }

    wchar_t existing = FindCdByLabel(ReadIsoLabel(path));
    if (existing) {
        RawWriter::Log(std::wstring(L"Using already mounted ISO ") + existing);
        return existing;
    }
    error = L"Не удалось подключить ISO. Если образ уже открыт в проводнике, извлеките его и повторите. " + ErrText(status);
    return 0;
}

bool WalkFiles(const std::wstring& dir, int depth, bool& anyBig, bool& nonSplit,
               std::vector<BigFile>& hits, std::wstring& error) {
    if (depth > 40) {
        error = L"Слишком глубокая папка в ISO.";
        return false;
    }
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        error = L"Не удалось прочитать папку образа " + dir;
        return false;
    }
    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!WalkFiles(full, depth + 1, anyBig, nonSplit, hits, error)) {
                ok = false;
                break;
            }
            continue;
        }
        uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (size < kFat32MaxFile) continue;
        anyBig = true;
        BigFile hit;
        hit.full = full;
        hit.name = fd.cFileName;
        hit.size = size;
        hit.split = SplitName(fd.cFileName);
        if (!hit.split) nonSplit = true;
        hits.push_back(hit);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return ok;
}

int DiskNumberOfLetter(wchar_t letter) {
    std::wstring path = L"\\\\.\\";
    path.push_back(letter);
    path += L":";
    HANDLE volume = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, 0, NULL);
    if (volume == INVALID_HANDLE_VALUE) return -1;
    BYTE buffer[256] = {};
    DWORD n = 0;
    VOLUME_DISK_EXTENTS* extents = (VOLUME_DISK_EXTENTS*)buffer;
    int disk = -1;
    if (DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                        NULL, 0, buffer, sizeof(buffer), &n, NULL) &&
        extents->NumberOfDiskExtents >= 1) {
        disk = (int)extents->Extents[0].DiskNumber;
    }
    CloseHandle(volume);
    return disk;
}

wchar_t WaitForDiskLetter(int disk, wchar_t skip, const ProgressFn& progress, bool& cancelled) {
    wchar_t skipUp = (wchar_t)towupper(skip);
    for (int attempt = 0; attempt < 30; ++attempt) {
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if ((mask & (1u << i)) == 0) continue;
            wchar_t letter = (wchar_t)(L'A' + i);
            if (letter == skipUp) continue;
            if (DiskNumberOfLetter(letter) == disk) return letter;
        }
        if (!Step(progress, 15, L"Ожидание буквы флешки...")) {
            cancelled = true;
            return 0;
        }
        Sleep(500);
    }
    return 0;
}

bool VolumeFs(wchar_t letter, std::wstring& fs) {
    wchar_t name[64] = {};
    std::wstring root = std::wstring(1, letter) + L":\\";
    if (!GetVolumeInformationW(root.c_str(), NULL, 0, NULL, NULL, NULL, name, 64)) return false;
    fs = name;
    return !fs.empty();
}

bool FsMatches(const std::wstring& fs, CopyFs want) {
    if (want == CopyFs::Ntfs) return _wcsicmp(fs.c_str(), L"NTFS") == 0;
    return _wcsicmp(fs.c_str(), L"FAT32") == 0 || _wcsicmp(fs.c_str(), L"FAT") == 0;
}

bool DiskpartFailed(const std::wstring& output) {
    return output.find(L"encountered an error") != std::wstring::npos ||
           output.find(L"Encountered an error") != std::wstring::npos ||
           output.find(L"обнаружила ошибку") != std::wstring::npos;
}

struct ScriptFile {
    std::wstring path;
    ~ScriptFile() {
        if (!path.empty()) DeleteFileW(path.c_str());
    }
};

bool WriteScript(const std::string& script, std::wstring& path, std::wstring& error) {
    wchar_t temp[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, temp);
    if (n == 0 || n >= MAX_PATH) {
        error = L"Не удалось найти папку TEMP.";
        return false;
    }
    path = std::wstring(temp) + L"isowriter_diskpart.txt";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Не удалось записать сценарий diskpart: " + ErrText(GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(file, script.data(), (DWORD)script.size(), &written, NULL);
    CloseHandle(file);
    if (!ok || written != script.size()) {
        error = L"Сценарий diskpart записан не полностью.";
        return false;
    }
    return true;
}

void EnsureDir(const std::wstring& dir) {
    if (dir.empty() || dir.size() <= 3) return;
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) EnsureDir(dir.substr(0, slash));
    CreateDirectoryW(dir.c_str(), NULL);
}

std::wstring SwmPath(const std::wstring& usbRoot, const std::wstring& isoRoot,
                     const BigFile& hit, std::vector<std::wstring>& used) {
    std::wstring rel = hit.full;
    if (rel.size() > isoRoot.size() && _wcsnicmp(rel.c_str(), isoRoot.c_str(), isoRoot.size()) == 0 &&
        rel[isoRoot.size()] == L'\\') {
        rel = rel.substr(isoRoot.size() + 1);
    } else {
        rel = hit.name;
    }
    std::wstring parent;
    size_t slash = rel.find_last_of(L"\\/");
    if (slash != std::wstring::npos) parent = rel.substr(0, slash);
    std::wstring base = hit.name;
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);
    std::wstring dir = parent.empty() ? (usbRoot + L"\\") : (usbRoot + L"\\" + parent);
    EnsureDir(dir);
    std::wstring path = dir + (dir.back() == L'\\' ? L"" : L"\\") + base + L".swm";
    int n = 2;
    while (std::find(used.begin(), used.end(), path) != used.end()) {
        path = dir + (dir.back() == L'\\' ? L"" : L"\\") + base + std::to_wstring(n) + L".swm";
        ++n;
    }
    used.push_back(path);
    return path;
}

bool HasLegacyBoot(const std::wstring& root) {
    return FileExists(root + L"\\bootmgr") || FileExists(root + L"\\boot\\bootmgr");
}

bool HasUefiBoot(const std::wstring& root) {
    return FileExists(root + L"\\efi\\boot\\bootx64.efi") || FileExists(root + L"\\efi\\boot\\bootia32.efi");
}

// Fail only when the ISO itself has the boot file and the stick does not. Hiren's has neither bootmgr nor bootx64.efi.
bool CopiedBootFiles(BootWriteMode mode, const std::wstring& isoRoot, const std::wstring& usbRoot, std::wstring& missing) {
    bool wantUefi = mode == BootWriteMode::Uefi || mode == BootWriteMode::Both;
    bool wantLegacy = mode == BootWriteMode::Legacy || mode == BootWriteMode::Both;
    if (wantLegacy && HasLegacyBoot(isoRoot) && !HasLegacyBoot(usbRoot)) {
        missing = L"bootmgr";
        return false;
    }
    if (wantUefi && HasUefiBoot(isoRoot) && !HasUefiBoot(usbRoot)) {
        missing = L"efi\\boot\\bootx64.efi";
        return false;
    }
    return true;
}

uint64_t FileSizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
}

uint64_t SumPatternSizes(const std::wstring& dir, const std::wstring& pattern) {
    std::wstring search = dir + (dir.empty() || dir.back() == L'\\' ? L"" : L"\\") + pattern;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint64_t total = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

} // namespace

WriteResult RawWriter::UnpackIso(const WriteParams& params, ProgressFn progressCallback, std::wstring& outErrorMessage) {
    outErrorMessage.clear();
    Log(L"==================================================");
    Log(L"Starting UnpackIso for " + params.targetDrive.devicePath +
        L" index=" + std::to_wstring(params.targetDrive.physicalDriveIndex) +
        L" mode=" + std::to_wstring((int)params.bootMode));
    Log(L"Source ISO: " + params.sourceIsoPath);

    if (!UnpackPlanSelfCheck()) {
        return Fail(outErrorMessage, L"Внутренняя проверка распаковки ISO не прошла.", WriteResult::WriteError);
    }
    std::wstring guardError;
    if (!ConfirmTarget(params, guardError)) return Fail(outErrorMessage, guardError, WriteResult::DeviceOpenFailed);

    uint64_t isoSize = FileSizeOf(params.sourceIsoPath);
    if (isoSize == 0) {
        return Fail(outErrorMessage, L"Не удалось определить размер ISO файла.", WriteResult::SourceOpenFailed);
    }
    if (isoSize > params.targetDrive.totalSizeBytes) {
        return Fail(outErrorMessage,
                    L"Размер ISO (" + DriveEnumerator::FormatSize(isoSize) +
                    L") превышает ёмкость накопителя (" +
                    DriveEnumerator::FormatSize(params.targetDrive.totalSizeBytes) + L").",
                    WriteResult::DiskTooSmall);
    }
    if (!Step(progressCallback, 2, L"Подключение ISO...")) {
        return Fail(outErrorMessage, L"Запись отменена.", WriteResult::Cancelled);
    }

    IsoMount mount;
    std::wstring mountError;
    wchar_t isoLetter = MountIso(params.sourceIsoPath, progressCallback, mount, mountError);
    if (!isoLetter) {
        WriteResult result = mountError == L"Запись отменена." ? WriteResult::Cancelled : WriteResult::SourceOpenFailed;
        return Fail(outErrorMessage, mountError.empty() ? L"Не удалось подключить ISO." : mountError, result);
    }
    Log(std::wstring(L"ISO letter ") + isoLetter);
    if (!Step(progressCallback, 8, L"Просмотр файлов образа...")) {
        return Fail(outErrorMessage, L"Запись отменена.", WriteResult::Cancelled);
    }

    bool anyBig = false;
    bool nonSplit = false;
    std::vector<BigFile> big;
    std::wstring walkError;
    std::wstring isoRoot = std::wstring(1, isoLetter) + L":";
    if (!WalkFiles(isoRoot, 0, anyBig, nonSplit, big, walkError)) {
        return Fail(outErrorMessage, walkError, WriteResult::SourceOpenFailed);
    }
    CopyPlan plan = MakePlan(params.bootMode, anyBig, nonSplit);
    Log(L"Unpack plan mode=" + std::to_wstring((int)plan.effective) +
        L" fs=" + (plan.fs == CopyFs::Ntfs ? std::wstring(L"ntfs") : std::wstring(L"fat32")) +
        L" big=" + std::to_wstring(big.size()) +
        L" bootsect=" + std::to_wstring(plan.bootsect));
    if (plan.refuse) {
        return Fail(outErrorMessage,
                    L"В образе есть файл больше 4 ГБ, и это не install.wim и не install.esd. "
                    L"FAT32 такой файл не хранит. Выберите режим Legacy: раздел будет NTFS.",
                    WriteResult::WriteError);
    }
    bool cap32 = plan.fs == CopyFs::Fat32 && params.targetDrive.totalSizeBytes > kFat32DiskMax;
    uint64_t budget = cap32 ? (32000ull << 20) : params.targetDrive.totalSizeBytes;
    if (isoSize > budget) {
        return Fail(outErrorMessage,
                    L"Для FAT32 на флешке больше 32 ГБ создаётся раздел 32000 МБ. Образ в него не помещается. "
                    L"Выберите Legacy, чтобы записать на NTFS.",
                    WriteResult::DiskTooSmall);
    }

    if (!ConfirmTarget(params, guardError)) return Fail(outErrorMessage, guardError, WriteResult::DeviceOpenFailed);
    std::string volLbl = SanitizeVolumeLabel(params.volumeLabel, plan.fs);
    std::string script = BuildScript(params.targetDrive.physicalDriveIndex, plan.fs, plan.bootsect, cap32, volLbl);
    std::string expect = "select disk " + std::to_string(params.targetDrive.physicalDriveIndex) + "\r\n";
    if (script.compare(0, expect.size(), expect) != 0 || CountOf(script, "select disk ") != 1 ||
        CountOf(script, "clean\r\n") != 1) {
        return Fail(outErrorMessage, L"Внутренняя ошибка сценария diskpart.", WriteResult::WriteError);
    }
    ScriptFile scriptFile;
    if (!WriteScript(script, scriptFile.path, guardError)) return Fail(outErrorMessage, guardError, WriteResult::WriteError);
    Log(L"diskpart script:\r\n" + OemToWide(script));
    if (!ConfirmTarget(params, guardError)) return Fail(outErrorMessage, guardError, WriteResult::DeviceOpenFailed);
    if (!Step(progressCallback, 12, L"Разметка и форматирование...")) {
        return Fail(outErrorMessage, L"Запись отменена.", WriteResult::Cancelled);
    }

    std::wstring diskpartOut;
    DWORD diskpartExit = 1;
    auto fixed = [](int pct) { return [pct]() { return pct; }; };
    RunKind ran = RunProcess(SystemExe(L"diskpart.exe"), L"/s " + Quote(scriptFile.path), L"",
                             progressCallback, fixed(14), L"Разметка и форматирование...",
                             diskpartOut, diskpartExit);
    Log(L"diskpart exit=" + std::to_wstring(diskpartExit) + L"\r\n" + diskpartOut);
    if (ran == RunKind::Cancelled) {
        return Fail(outErrorMessage,
                    L"Запись отменена. Если очистка уже началась, запишите образ ещё раз.",
                    WriteResult::Cancelled);
    }
    if (ran != RunKind::Done || diskpartExit != 0 || DiskpartFailed(diskpartOut)) {
        return Fail(outErrorMessage, L"Не удалось разметить флешку.\n" + LastLines(diskpartOut),
                    WriteResult::WriteError);
    }

    bool cancelled = false;
    wchar_t usbLetter = WaitForDiskLetter(params.targetDrive.physicalDriveIndex, isoLetter, progressCallback, cancelled);
    if (cancelled) {
        return Fail(outErrorMessage, L"Запись отменена. Флешка уже могла быть очищена, запишите образ ещё раз.",
                    WriteResult::Cancelled);
    }
    if (!usbLetter || (wchar_t)towupper(usbLetter) == (wchar_t)towupper(isoLetter)) {
        return Fail(outErrorMessage, L"Diskpart не назначил букву выбранной флешке.\n" + LastLines(diskpartOut),
                    WriteResult::WriteError);
    }
    if (DiskNumberOfLetter(usbLetter) != params.targetDrive.physicalDriveIndex) {
        return Fail(outErrorMessage, L"Буква диска не принадлежит выбранной флешке. Копирование отменено.",
                    WriteResult::WriteError);
    }
    std::wstring fs;
    if (!VolumeFs(usbLetter, fs) || !FsMatches(fs, plan.fs)) {
        return Fail(outErrorMessage,
                    L"После форматирования Windows не видит " +
                    std::wstring(plan.fs == CopyFs::Ntfs ? L"NTFS" : L"FAT32") +
                    L" на " + usbLetter + L":. Сейчас: " + (fs.empty() ? L"RAW" : fs) + L".\n" + LastLines(diskpartOut),
                    WriteResult::WriteError);
    }
    Log(std::wstring(L"USB letter ") + usbLetter + L" fs=" + fs);

    std::wstring copyArgs = isoRoot + L"\\ " + std::wstring(1, usbLetter) + L":\\ /E /R:2 /W:2 /XJ /NFL /NDL /NP";
    if (plan.fs == CopyFs::Fat32) {
        bool exclude = false;
        for (const BigFile& hit : big) {
            if (!hit.split) continue;
            if (!exclude) {
                copyArgs += L" /XF";
                exclude = true;
            }
            copyArgs += L" " + hit.name;
        }
    }
    std::wstring copyOut;
    DWORD copyExit = 16;
    auto copyPercent = [&]() -> int {
        ULARGE_INTEGER avail = {}, total = {}, freeTotal = {};
        std::wstring root = std::wstring(1, usbLetter) + L":\\";
        if (!GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &freeTotal) || isoSize == 0) return 40;
        uint64_t used = total.QuadPart > avail.QuadPart ? total.QuadPart - avail.QuadPart : 0;
        int pct = 20 + (int)((used * 65) / isoSize);
        if (pct < 20) pct = 20;
        if (pct > 85) pct = 85;
        return pct;
    };
    auto copyStatus = [&]() -> std::wstring {
        return L"Копирование файлов (" + std::to_wstring(copyPercent()) + L"%)...";
    };
    ran = RunProcess(SystemExe(L"robocopy.exe"), copyArgs, L"", progressCallback, copyPercent,
                     L"Копирование файлов...", copyOut, copyExit, copyStatus);
    Log(L"robocopy exit=" + std::to_wstring(copyExit) + L"\r\n" + copyOut);
    if (ran == RunKind::Cancelled) {
        return Fail(outErrorMessage, L"Копирование отменено. Запишите образ ещё раз.", WriteResult::Cancelled);
    }
    if (ran != RunKind::Done || !CopyExitOk(copyExit)) {
        return Fail(outErrorMessage,
                    L"Копирование остановилось (код robocopy " + std::to_wstring(copyExit) + L").\n" + LastLines(copyOut),
                    WriteResult::WriteError);
    }

    if (plan.fs == CopyFs::Fat32) {
        std::vector<std::wstring> usedSwm;
        for (const BigFile& hit : big) {
            if (!hit.split) continue;
            if (!Step(progressCallback, 90, L"Разрезание " + hit.name + L"...")) {
                return Fail(outErrorMessage, L"Запись отменена. Файлы уже частично скопированы.", WriteResult::Cancelled);
            }
            std::wstring swm = SwmPath(std::wstring(1, usbLetter) + L":", isoRoot, hit, usedSwm);
            std::wstring args = L"/Split-Image /ImageFile:" + Quote(hit.full) +
                                L" /SWMFile:" + Quote(swm) + L" /FileSize:4000";
            std::wstring splitOut;
            DWORD splitExit = 1;
            size_t slash = swm.find_last_of(L"\\/");
            std::wstring swmDir = (slash != std::wstring::npos) ? swm.substr(0, slash) : (std::wstring(1, usbLetter) + L":");
            std::wstring base = hit.name;
            size_t dot = base.find_last_of(L'.');
            if (dot != std::wstring::npos) base = base.substr(0, dot);

            auto splitPercent = [&]() -> int {
                uint64_t written = SumPatternSizes(swmDir, base + L"*.swm");
                if (hit.size == 0) return 90;
                int p = (int)((written * 100) / hit.size);
                if (p > 99) p = 99;
                return 85 + (p * 11) / 100; // 85% to 96%
            };
            auto splitStatus = [&]() -> std::wstring {
                uint64_t written = SumPatternSizes(swmDir, base + L"*.swm");
                int p = hit.size > 0 ? (int)((written * 100) / hit.size) : 0;
                if (p > 99) p = 99;
                return L"Разрезание " + hit.name + L" (" + std::to_wstring(p) + L"%)...";
            };
            ran = RunProcess(SystemExe(L"dism.exe"), args, L"", progressCallback, splitPercent,
                             L"Разрезание " + hit.name + L"...", splitOut, splitExit, splitStatus);
            Log(L"dism " + hit.name + L" exit=" + std::to_wstring(splitExit) + L"\r\n" + splitOut);
            if (ran == RunKind::Cancelled) {
                return Fail(outErrorMessage, L"Запись отменена. Файлы уже частично скопированы.", WriteResult::Cancelled);
            }
            if (ran != RunKind::Done || splitExit != 0) {
                return Fail(outErrorMessage,
                            L"Раздел " + std::wstring(1, usbLetter) + L": создан (" + fs +
                            L"), но не удалось разрезать " + hit.name +
                            L". Выберите Legacy, чтобы записать на NTFS без разрезания.\n" + LastLines(splitOut),
                            WriteResult::WriteError);
            }
        }
    }

    std::wstring bootNote;
    if (plan.bootsect) {
        std::wstring bootsect = isoRoot + L"\\boot\\bootsect.exe";
        if (!FileExists(bootsect)) {
            bootNote = L"В образе нет boot\\bootsect.exe, загрузчик BIOS не установлен.";
            Log(bootNote);
        } else if (!Step(progressCallback, 96, L"Загрузчик BIOS...")) {
            return Fail(outErrorMessage, L"Запись отменена. Файлы уже скопированы на " + std::wstring(1, usbLetter) + L":.",
                        WriteResult::Cancelled);
        } else {
            std::wstring args = L"/nt60 " + std::wstring(1, usbLetter) + L": /mbr";
            std::wstring bootOut;
            DWORD bootExit = 1;
            ran = RunProcess(bootsect, args, isoRoot + L"\\boot", progressCallback, fixed(96),
                             L"Загрузчик BIOS...", bootOut, bootExit);
            Log(L"bootsect exit=" + std::to_wstring(bootExit) + L"\r\n" + bootOut);
            if (ran == RunKind::Cancelled) {
                return Fail(outErrorMessage, L"Запись отменена. Файлы уже скопированы.", WriteResult::Cancelled);
            }
            if (ran != RunKind::Done || bootExit != 0) {
                bootNote = L"bootsect не записал загрузчик BIOS.";
                Log(bootNote);
            }
        }
    }

    if (params.verifyAfterWrite) {
        if (!Step(progressCallback, 98, L"Проверка файлов загрузки...")) {
            return Fail(outErrorMessage, L"Запись отменена. Файлы уже скопированы.", WriteResult::Cancelled);
        }
        std::wstring missing;
        if (!CopiedBootFiles(plan.effective, isoRoot, std::wstring(1, usbLetter) + L":", missing)) {
            return Fail(outErrorMessage,
                        L"Файлы скопированы, раздел " + std::wstring(1, usbLetter) + L": " + fs +
                        L". На флешку не попал файл загрузки из образа: " + missing + L".",
                        WriteResult::VerificationError);
        }
    }

    std::wstring note = L"Раздел " + std::wstring(1, usbLetter) + L": " + fs + L".";
    if (plan.fs == CopyFs::Fat32) {
        for (const BigFile& hit : big) {
            if (hit.split) {
                note += L" " + hit.name + L" разрезан на части по 4000 МБ.";
                break;
            }
        }
    }
    if (!bootNote.empty()) note += L" " + bootNote;
    wchar_t root[] = L"A:\\";
    root[0] = usbLetter;
    SHChangeNotify(SHCNE_MEDIAINSERTED, SHCNF_PATH | SHCNF_FLUSH, root, NULL);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH | SHCNF_FLUSH, root, NULL);
    Log(L"UnpackIso completed: " + note);
    outErrorMessage = note;
    return WriteResult::Success;
}
