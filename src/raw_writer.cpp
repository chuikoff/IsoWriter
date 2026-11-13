#include "raw_writer.h"
#include <shlobj.h>
#include <winioctl.h>
#include <vector>
#include <algorithm>
#include <cstring>

std::wstring RawWriter::LogFilePath() {
    wchar_t temp[MAX_PATH] = {0};
    DWORD n = GetTempPathW(MAX_PATH, temp);
    if (n == 0 || n >= MAX_PATH) return L"isowriter_debug.log";
    return std::wstring(temp) + L"isowriter_debug.log";
}

void RawWriter::Log(const std::wstring& msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t timeBuf[64];
    swprintf_s(timeBuf, L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::wstring line = std::wstring(timeBuf) + msg + L"\r\n";

    int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), NULL, 0, NULL, NULL);
    if (bytes <= 0) return;
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), utf8.data(), bytes, NULL, NULL);

    HANDLE h = CreateFileW(RawWriter::LogFilePath().c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER size = {0};
    if (GetFileSizeEx(h, &size) && size.QuadPart == 0) {
        DWORD written = 0;
        const BYTE bom[] = {0xEF, 0xBB, 0xBF};
        WriteFile(h, bom, 3, &written, NULL);
    }
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, NULL);
    CloseHandle(h);
}

namespace {

std::wstring GetSystemErrorMessage(DWORD errorCode) {
    wchar_t* msgBuf = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&msgBuf, 0, NULL);
    if (len == 0 || !msgBuf) {
        return L"Error code: " + std::to_wstring(errorCode);
    }
    std::wstring result(msgBuf, len);
    LocalFree(msgBuf);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}

bool LockAndDismountHandle(HANDLE hVol, const std::wstring& name) {
    DWORD bytesRet = 0;
    DeviceIoControl(hVol, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &bytesRet, NULL);

    BOOL bLock = FALSE;
    DWORD lockErr = 0;
    for (int retry = 0; retry < 15; ++retry) {
        bLock = DeviceIoControl(hVol, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytesRet, NULL);
        if (bLock) break;
        lockErr = GetLastError();
        Sleep(50);
    }

    BOOL bDismount = FALSE;
    DWORD dismountErr = 0;
    if (bLock) {
        bDismount = DeviceIoControl(hVol, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &bytesRet, NULL);
        if (!bDismount) dismountErr = GetLastError();
    }

    RawWriter::Log(L"Volume " + name + L": Lock=" + std::to_wstring(bLock) +
                   L" (err=" + std::to_wstring(lockErr) + L"), Dismount=" +
                   std::to_wstring(bDismount) + L" (err=" + std::to_wstring(dismountErr) + L")");
    return bLock != FALSE;
}

bool VolumeBelongsToDisk(HANDLE hVol, int diskNumber) {
    BYTE buffer[4096] = {0};
    VOLUME_DISK_EXTENTS* extents = (VOLUME_DISK_EXTENTS*)buffer;
    DWORD bytesRet = 0;
    if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                        NULL, 0, buffer, sizeof(buffer), &bytesRet, NULL) &&
        bytesRet >= sizeof(VOLUME_DISK_EXTENTS)) {
        for (DWORD n = 0; n < extents->NumberOfDiskExtents; ++n) {
            if ((int)extents->Extents[n].DiskNumber == diskNumber) return true;
        }
        return false;
    }

    STORAGE_DEVICE_NUMBER sdn = {0};
    if (DeviceIoControl(hVol, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                        NULL, 0, &sdn, sizeof(sdn), &bytesRet, NULL)) {
        if (sdn.DeviceType == FILE_DEVICE_DISK && (int)sdn.DeviceNumber == diskNumber)
            return true;
    }
    return false;
}

// Locks all volumes and partitions on the disk. Handles are added to outHandles
// and MUST be kept open during write to prevent Windows from denying access.
void LockAndDismountAllDiskVolumes(int diskNumber, std::vector<HANDLE>& outHandles) {
    // 1. Lock drive letters (e.g. \\.\F:)
    DWORD logicalDrives = GetLogicalDrives();
    std::vector<wchar_t> lockedLetters;
    for (int i = 0; i < 26; ++i) {
        if (logicalDrives & (1 << i)) {
            wchar_t driveLetter = L'A' + i;
            wchar_t volPath[] = L"\\\\.\\A:";
            volPath[4] = driveLetter;

            HANDLE hVol = CreateFileW(volPath, GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      NULL, OPEN_EXISTING, 0, NULL);
            if (hVol != INVALID_HANDLE_VALUE) {
                if (VolumeBelongsToDisk(hVol, diskNumber)) {
                    if (LockAndDismountHandle(hVol, volPath)) {
                        outHandles.push_back(hVol);
                        lockedLetters.push_back(driveLetter);
                    } else {
                        CloseHandle(hVol);
                    }
                } else {
                    CloseHandle(hVol);
                }
            }
        }
    }

    // 2. Lock volume GUIDs (FindFirstVolumeW) for partitions WITHOUT drive letters
    wchar_t volumeName[MAX_PATH];
    HANDLE hFind = FindFirstVolumeW(volumeName, MAX_PATH);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            // Check if this volume GUID already has a locked drive letter
            wchar_t pathNames[MAX_PATH] = {0};
            DWORD pathLen = 0;
            bool alreadyLocked = false;
            if (GetVolumePathNamesForVolumeNameW(volumeName, pathNames, MAX_PATH, &pathLen)) {
                for (wchar_t* p = pathNames; *p; p += wcslen(p) + 1) {
                    if (p[1] == L':') {
                        wchar_t letter = towupper(p[0]);
                        if (std::find(lockedLetters.begin(), lockedLetters.end(), letter) != lockedLetters.end()) {
                            alreadyLocked = true;
                            break;
                        }
                    }
                }
            }
            if (alreadyLocked) continue;

            std::wstring openPath = volumeName;
            if (!openPath.empty() && openPath.back() == L'\\') openPath.pop_back();

            HANDLE hQuery = CreateFileW(openPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        NULL, OPEN_EXISTING, 0, NULL);
            if (hQuery != INVALID_HANDLE_VALUE) {
                if (VolumeBelongsToDisk(hQuery, diskNumber)) {
                    if (LockAndDismountHandle(hQuery, openPath)) {
                        outHandles.push_back(hQuery);
                    } else {
                        CloseHandle(hQuery);
                    }
                } else {
                    CloseHandle(hQuery);
                }
            }
        } while (FindNextVolumeW(hFind, volumeName, MAX_PATH));
        FindVolumeClose(hFind);
    }

    // 3. If no volumes were locked yet (e.g. raw / Linux partitions), try locking partitions directly
    if (outHandles.empty()) {
        for (int p = 1; p <= 16; ++p) {
            std::wstring partPath = L"\\\\?\\GLOBALROOT\\Device\\Harddisk" +
                                    std::to_wstring(diskNumber) + L"\\Partition" + std::to_wstring(p);
            HANDLE hPart = CreateFileW(partPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       NULL, OPEN_EXISTING, 0, NULL);
            if (hPart != INVALID_HANDLE_VALUE) {
                if (LockAndDismountHandle(hPart, partPath)) {
                    outHandles.push_back(hPart);
                } else {
                    CloseHandle(hPart);
                }
            }
        }
    }

    RawWriter::Log(L"Total locked volume/partition handles for Disk " +
                   std::to_wstring(diskNumber) + L": " + std::to_wstring(outHandles.size()));
}

HANDLE OpenPhysicalForWrite(const std::wstring& path) {
    HANDLE hDisk = INVALID_HANDLE_VALUE;
    DWORD lastErr = 0;
    for (int retry = 0; retry < 15; ++retry) {
        hDisk = CreateFileW(path.c_str(),
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING,
                            NULL);
        if (hDisk != INVALID_HANDLE_VALUE) {
            RawWriter::Log(L"Opened physical drive " + path + L" successfully");
            return hDisk;
        }
        lastErr = GetLastError();
        Sleep(100);
    }
    SetLastError(lastErr);
    RawWriter::Log(L"Failed to open physical drive " + path + L" err=" + std::to_wstring(lastErr));
    return INVALID_HANDLE_VALUE;
}

void PreparePhysicalDrive(HANDLE hDisk) {
    DWORD bytesRet = 0;
    DeviceIoControl(hDisk, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &bytesRet, NULL);
}

void NotifyDriveLetters(const std::wstring& driveLetters) {
    for (wchar_t c : driveLetters) {
        if ((c < L'A' || c > L'Z') && (c < L'a' || c > L'z')) continue;
        wchar_t root[] = L"A:\\";
        root[0] = towupper(c);
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, root, NULL);
    }
}

void ApplyWindowsVolumeLabels(int diskNumber, const std::wstring& newLabel) {
    if (newLabel.empty() || diskNumber < 0) return;

    Sleep(500);

    DWORD logicalDrives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (logicalDrives & (1 << i)) {
            wchar_t driveLetter = L'A' + i;
            wchar_t volPath[] = L"\\\\.\\A:";
            volPath[4] = driveLetter;

            HANDLE hVol = CreateFileW(volPath, GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      NULL, OPEN_EXISTING, 0, NULL);
            if (hVol != INVALID_HANDLE_VALUE) {
                bool belongs = VolumeBelongsToDisk(hVol, diskNumber);
                CloseHandle(hVol);
                if (belongs) {
                    wchar_t root[] = L"A:\\";
                    root[0] = driveLetter;
                    BOOL ok = SetVolumeLabelW(root, newLabel.c_str());
                    RawWriter::Log(L"SetVolumeLabelW on " + std::wstring(root) +
                                   L" to '" + newLabel + L"': " + (ok ? L"success" : L"failed err=" + std::to_wstring(GetLastError())));
                    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, root, NULL);
                }
            }
        }
    }
}

struct BurnCleanup {
    HANDLE hIso = INVALID_HANDLE_VALUE;
    HANDLE hDisk = INVALID_HANDLE_VALUE;
    std::vector<HANDLE> vols;
    BYTE* buffer = nullptr;
    BYTE* verifyBuffer = nullptr;
    std::wstring devicePath;
    std::wstring driveLetters;
    int diskNumber = -1;
    std::wstring volumeLabel;

    ~BurnCleanup() {
        if (buffer) VirtualFree(buffer, 0, MEM_RELEASE);
        if (verifyBuffer) VirtualFree(verifyBuffer, 0, MEM_RELEASE);

        if (hDisk != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(hDisk);
            CloseHandle(hDisk);
            hDisk = INVALID_HANDLE_VALUE;
        }

        if (hIso != INVALID_HANDLE_VALUE) {
            CloseHandle(hIso);
            hIso = INVALID_HANDLE_VALUE;
        }

        // Unlock and close volume handles
        DWORD bytesRet = 0;
        for (HANDLE h : vols) {
            if (h != INVALID_HANDLE_VALUE) {
                DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &bytesRet, NULL);
                CloseHandle(h);
            }
        }
        vols.clear();

        // Refresh disk properties so Windows immediately recognizes the new partition layout
        if (!devicePath.empty()) {
            HANDLE hRefresh = CreateFileW(devicePath.c_str(), 0,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          NULL, OPEN_EXISTING, 0, NULL);
            if (hRefresh != INVALID_HANDLE_VALUE) {
                DeviceIoControl(hRefresh, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &bytesRet, NULL);
                CloseHandle(hRefresh);
            }
            NotifyDriveLetters(driveLetters);
            if (!volumeLabel.empty() && diskNumber >= 0) {
                ApplyWindowsVolumeLabels(diskNumber, volumeLabel);
            }
        }
    }
};

bool PatchIso9660VolumeLabel(HANDLE hDisk, const std::wstring& newLabel, DWORD sectorSize) {
    if (newLabel.empty()) return true;

    // ISO 9660 Primary Volume Descriptor is at sector 16 (offset 32768 = 16 * 2048)
    DWORD ioSize = (sectorSize < 2048) ? 2048 : sectorSize;
    if (ioSize % sectorSize != 0) ioSize = ((ioSize + sectorSize - 1) / sectorSize) * sectorSize;

    BYTE* pvdBuf = (BYTE*)VirtualAlloc(NULL, ioSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pvdBuf) return false;

    LARGE_INTEGER li;
    li.QuadPart = 32768; // 16 * 2048
    if (!SetFilePointerEx(hDisk, li, NULL, FILE_BEGIN)) {
        VirtualFree(pvdBuf, 0, MEM_RELEASE);
        return false;
    }

    DWORD bytesRead = 0;
    if (!ReadFile(hDisk, pvdBuf, ioSize, &bytesRead, NULL) || bytesRead < 2048) {
        VirtualFree(pvdBuf, 0, MEM_RELEASE);
        return false;
    }

    // Check PVD signature: byte 0 is 1 (PVD), bytes 1..5 are "CD001"
    if (pvdBuf[0] == 1 && memcmp(pvdBuf + 1, "CD001", 5) == 0) {
        char asciiLabel[32];
        memset(asciiLabel, ' ', sizeof(asciiLabel));
        for (size_t i = 0; i < newLabel.size() && i < 32; ++i) {
            wchar_t wc = newLabel[i];
            if (wc < 128) asciiLabel[i] = (char)toupper(wc);
            else asciiLabel[i] = '_';
        }

        // PVD Volume Identifier is at bytes 40..71 (32 bytes)
        memcpy(pvdBuf + 40, asciiLabel, 32);

        SetFilePointerEx(hDisk, li, NULL, FILE_BEGIN);
        DWORD bytesWritten = 0;
        WriteFile(hDisk, pvdBuf, ioSize, &bytesWritten, NULL);
        FlushFileBuffers(hDisk);
        RawWriter::Log(L"Patched ISO 9660 PVD volume label: " + newLabel);
    }

    VirtualFree(pvdBuf, 0, MEM_RELEASE);
    return true;
}

bool PatchBootSector(HANDLE hDisk, BootWriteMode mode, DWORD sectorSize, std::wstring& error) {
    if (mode == BootWriteMode::AsImage) return true;

    DWORD io = (sectorSize < 512) ? 512 : sectorSize;
    std::vector<BYTE> sector(io, 0);

    LARGE_INTEGER zero = {0};
    SetFilePointerEx(hDisk, zero, NULL, FILE_BEGIN);
    DWORD bytesRead = 0;
    if (!ReadFile(hDisk, sector.data(), io, &bytesRead, NULL) || bytesRead < 512) {
        error = L"Не удалось прочитать загрузочный сектор для настройки режима.";
        return false;
    }

    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        // No MBR signature, nothing to patch
        return true;
    }

    int legacyIdx = -1;
    for (int i = 0; i < 4; ++i) {
        BYTE type = sector[446 + i * 16 + 4];
        if (type != 0x00 && type != 0xEE && type != 0xEF && legacyIdx < 0) {
            legacyIdx = i;
        }
    }

    if (mode == BootWriteMode::Legacy || mode == BootWriteMode::Both) {
        if (legacyIdx >= 0) {
            for (int i = 0; i < 4; ++i) sector[446 + i * 16] = 0;
            sector[446 + legacyIdx * 16] = 0x80; // Active boot flag
        }
    } else if (mode == BootWriteMode::Uefi) {
        // Pure UEFI: clear active flags from MBR
        for (int i = 0; i < 4; ++i) sector[446 + i * 16] = 0;
    }

    SetFilePointerEx(hDisk, zero, NULL, FILE_BEGIN);
    DWORD bytesWritten = 0;
    if (!WriteFile(hDisk, sector.data(), io, &bytesWritten, NULL) || bytesWritten != io) {
        DWORD err = GetLastError();
        error = L"Не удалось записать флаги загрузки: " + GetSystemErrorMessage(err);
        return false;
    }
    FlushFileBuffers(hDisk);
    RawWriter::Log(L"Boot flags applied for mode: " + std::wstring(BootWriteModeName(mode)));
    return true;
}

} // namespace

WriteResult RawWriter::BurnImage(const WriteParams& params, ProgressFn progressCallback, std::wstring& outErrorMessage) {
    outErrorMessage.clear();
    Log(L"==================================================");
    Log(L"Starting BurnImage for " + params.targetDrive.devicePath +
        L" (Disk " + std::to_wstring(params.targetDrive.physicalDriveIndex) + L")");
    Log(L"Source: " + params.sourceIsoPath);

    int sysDrive = DriveEnumerator::GetSystemDriveIndex();
    if (sysDrive < 0) {
        outErrorMessage = L"Не удалось определить системный диск. Запись отменена для безопасности.";
        Log(outErrorMessage);
        return WriteResult::DeviceOpenFailed;
    }
    if (params.targetDrive.physicalDriveIndex == sysDrive) {
        outErrorMessage = L"Критическая ошибка безопасности: попытка записи на системный диск!";
        Log(outErrorMessage);
        return WriteResult::DeviceOpenFailed;
    }

    if (params.targetDrive.totalSizeBytes == 0) {
        outErrorMessage = L"Не удалось определить ёмкость накопителя. Запись отменена.";
        Log(outErrorMessage);
        return WriteResult::DiskTooSmall;
    }

    BurnCleanup st;
    st.devicePath = params.targetDrive.devicePath;
    st.driveLetters = params.targetDrive.driveLetters;
    st.diskNumber = params.targetDrive.physicalDriveIndex;
    st.volumeLabel = params.volumeLabel;

    st.hIso = CreateFileW(params.sourceIsoPath.c_str(), GENERIC_READ,
                          FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (st.hIso == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        outErrorMessage = L"Не удалось открыть исходный файл ISO: " + GetSystemErrorMessage(err);
        Log(outErrorMessage);
        return WriteResult::SourceOpenFailed;
    }

    LARGE_INTEGER isoSize = {0};
    if (!GetFileSizeEx(st.hIso, &isoSize) || isoSize.QuadPart <= 0) {
        outErrorMessage = L"Не удалось определить размер ISO файла.";
        Log(outErrorMessage);
        return WriteResult::SourceOpenFailed;
    }
    uint64_t isoBytes = (uint64_t)isoSize.QuadPart;
    uint64_t diskBytes = params.targetDrive.totalSizeBytes;

    Log(L"ISO size: " + std::to_wstring(isoBytes) + L" bytes (" + DriveEnumerator::FormatSize(isoBytes) + L")");
    Log(L"Disk size: " + std::to_wstring(diskBytes) + L" bytes (" + DriveEnumerator::FormatSize(diskBytes) + L")");

    DWORD sectorSize = params.targetDrive.bytesPerSector;
    if (sectorSize == 0) sectorSize = 512;

    if (isoBytes > diskBytes) {
        outErrorMessage = L"Размер ISO (" + DriveEnumerator::FormatSize(isoBytes) +
                          L") превышает емкость накопителя (" +
                          DriveEnumerator::FormatSize(diskBytes) + L")!";
        Log(outErrorMessage);
        return WriteResult::DiskTooSmall;
    }

    // 1. Lock and dismount ALL volumes and partitions on target disk.
    // Handles are kept in st.vols and stay open throughout the write!
    LockAndDismountAllDiskVolumes(params.targetDrive.physicalDriveIndex, st.vols);

    // 2. Open physical drive for raw writing
    st.hDisk = OpenPhysicalForWrite(params.targetDrive.devicePath);
    if (st.hDisk == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            outErrorMessage = L"Отказано в доступе к физическому диску.\n"
                              L"Для прямой записи на USB требуются права Администратора.\n"
                              L"Запустите Total Commander от имени Администратора.";
            Log(outErrorMessage);
            return WriteResult::ElevationRequired;
        }
        outErrorMessage = L"Не удалось открыть накопитель " + params.targetDrive.devicePath +
                          L": " + GetSystemErrorMessage(err);
        Log(outErrorMessage);
        return WriteResult::DeviceOpenFailed;
    }

    PreparePhysicalDrive(st.hDisk);

    // 3. Allocate aligned buffer (1 MB chunks for high throughput)
    PluginSettings settings = SettingsManager::Load();
    DWORD chunkSize = (DWORD)settings.bufferSizeKb * 1024;
    if (chunkSize < 64 * 1024) chunkSize = 1024 * 1024;
    Log(L"Using write chunk size: " + std::to_wstring(chunkSize / 1024) + L" KB");

    st.buffer = (BYTE*)VirtualAlloc(NULL, chunkSize + sectorSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!st.buffer) {
        outErrorMessage = L"Не удалось выделить память для буфера записи.";
        Log(outErrorMessage);
        return WriteResult::WriteError;
    }

    // 4. Sequential write loop from sector 0 to EOF
    uint64_t totalBytesWritten = 0;
    uint64_t diskOffset = 0;
    bool aborted = false;

    if (progressCallback) {
        progressCallback(0, L"Начало записи на USB...");
    }

    while (true) {
        DWORD bytesRead = 0;
        if (!ReadFile(st.hIso, st.buffer, chunkSize, &bytesRead, NULL)) {
            DWORD err = GetLastError();
            outErrorMessage = L"Ошибка чтения ISO (код " + std::to_wstring(err) + L"): " + GetSystemErrorMessage(err);
            Log(outErrorMessage);
            return WriteResult::WriteError;
        }
        if (bytesRead == 0) break; // EOF

        DWORD writeSize = bytesRead;
        DWORD tail = writeSize % sectorSize;
        if (tail != 0) {
            DWORD padding = sectorSize - tail;
            memset(st.buffer + writeSize, 0, padding);
            writeSize += padding;
        }

        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)diskOffset;
        if (!SetFilePointerEx(st.hDisk, li, NULL, FILE_BEGIN)) {
            DWORD err = GetLastError();
            outErrorMessage = L"Ошибка позиционирования на диске (код " + std::to_wstring(err) + L"): " + GetSystemErrorMessage(err);
            Log(outErrorMessage);
            return WriteResult::WriteError;
        }

        DWORD bytesWritten = 0;
        BOOL ok = FALSE;
        DWORD err = 0;
        for (int retry = 0; retry < 5; ++retry) {
            ok = WriteFile(st.hDisk, st.buffer, writeSize, &bytesWritten, NULL);
            if (ok && bytesWritten == writeSize) break;
            err = GetLastError();
            Sleep(100);
        }
        if (!ok || bytesWritten != writeSize) {
            outErrorMessage = L"Ошибка записи на диск (код " + std::to_wstring(err) + L"): " + GetSystemErrorMessage(err);
            Log(outErrorMessage + L" at offset " + std::to_wstring(diskOffset));
            return WriteResult::WriteError;
        }

        diskOffset += writeSize;
        totalBytesWritten += bytesRead;
        int percent = (int)((totalBytesWritten * 100) / isoBytes);
        if (percent > 100) percent = 100;

        if (progressCallback) {
            std::wstring status = L"Запись: " + DriveEnumerator::FormatSize(totalBytesWritten) +
                                  L" из " + DriveEnumerator::FormatSize(isoBytes);
            if (!progressCallback(percent, status)) {
                aborted = true;
                break;
            }
        }
    }

    FlushFileBuffers(st.hDisk);
    Log(L"Write loop completed. Total bytes written: " + std::to_wstring(totalBytesWritten));

    if (aborted) {
        Log(L"Запись отменена пользователем.");
        return WriteResult::Cancelled;
    }
    if (totalBytesWritten != isoBytes) {
        outErrorMessage = L"Записано " + DriveEnumerator::FormatSize(totalBytesWritten) +
                          L" из " + DriveEnumerator::FormatSize(isoBytes) + L". Образ на накопителе неполный.";
        Log(outErrorMessage);
        return WriteResult::WriteError;
    }

    // 5. Verify if requested
    if (params.verifyAfterWrite) {
        if (progressCallback) {
            progressCallback(0, L"Верификация данных...");
        }

        st.verifyBuffer = (BYTE*)VirtualAlloc(NULL, chunkSize + sectorSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!st.verifyBuffer) {
            outErrorMessage = L"Запись завершена, но не удалось выделить память для проверки.";
            Log(outErrorMessage);
            return WriteResult::VerificationError;
        }

        LARGE_INTEGER zero = {0};
        SetFilePointerEx(st.hIso, zero, NULL, FILE_BEGIN);
        SetFilePointerEx(st.hDisk, zero, NULL, FILE_BEGIN);

        uint64_t totalVerified = 0;
        uint64_t verifyOffset = 0;

        while (true) {
            DWORD bytesFromIso = 0;
            if (!ReadFile(st.hIso, st.buffer, chunkSize, &bytesFromIso, NULL)) {
                DWORD err = GetLastError();
                outErrorMessage = L"Ошибка чтения ISO при проверке: " + GetSystemErrorMessage(err);
                Log(outErrorMessage);
                return WriteResult::VerificationError;
            }
            if (bytesFromIso == 0) break;

            DWORD readSize = bytesFromIso;
            DWORD tail = readSize % sectorSize;
            if (tail != 0) readSize += sectorSize - tail;

            LARGE_INTEGER li;
            li.QuadPart = (LONGLONG)verifyOffset;
            SetFilePointerEx(st.hDisk, li, NULL, FILE_BEGIN);

            DWORD bytesFromDisk = 0;
            if (!ReadFile(st.hDisk, st.verifyBuffer, readSize, &bytesFromDisk, NULL) || bytesFromDisk < bytesFromIso) {
                DWORD err = GetLastError();
                outErrorMessage = L"Ошибка чтения с диска при проверке: " + GetSystemErrorMessage(err);
                Log(outErrorMessage);
                return WriteResult::VerificationError;
            }

            if (memcmp(st.buffer, st.verifyBuffer, bytesFromIso) != 0) {
                outErrorMessage = L"Несоответствие данных при проверке на смещении " +
                                  std::to_wstring(totalVerified) + L" байт.";
                Log(outErrorMessage);
                return WriteResult::VerificationError;
            }

            verifyOffset += readSize;
            totalVerified += bytesFromIso;
            int percent = (int)((totalVerified * 100) / isoBytes);
            if (percent > 100) percent = 100;

            if (progressCallback) {
                std::wstring status = L"Проверка: " + DriveEnumerator::FormatSize(totalVerified) +
                                      L" из " + DriveEnumerator::FormatSize(isoBytes);
                if (!progressCallback(percent, status)) {
                    outErrorMessage = L"Проверка отменена. Образ уже записан на накопитель.";
                    Log(outErrorMessage);
                    return WriteResult::Cancelled;
                }
            }
        }
        if (progressCallback) {
            progressCallback(100, L"Верификация: 100% — УСПЕШНО (данные полностью совпадают!)");
            Sleep(400);
        }
        Log(L"Verification passed successfully: " + std::to_wstring(totalVerified) + L" bytes (100% match).");
    }

    // 6. Apply boot mode flags if requested
    if (params.bootMode != BootWriteMode::AsImage) {
        if (progressCallback) progressCallback(100, L"Настройка режима загрузки...");
        if (!PatchBootSector(st.hDisk, params.bootMode, sectorSize, outErrorMessage)) {
            Log(outErrorMessage);
            return WriteResult::WriteError;
        }
    }

    // 7. Patch ISO 9660 volume label if specified
    if (!params.volumeLabel.empty()) {
        PatchIso9660VolumeLabel(st.hDisk, params.volumeLabel, sectorSize);
    }

    Log(L"BurnImage completed successfully!");
    return WriteResult::Success;
}
