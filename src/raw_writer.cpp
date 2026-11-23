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

// Returns false when path is a volume of diskNumber and it could not be locked.
// shortcut: a volume that cannot be opened at all is treated as not ours, upgrade if writes still land on a busy stick
bool TryLockVolume(const std::wstring& path, int diskNumber, std::vector<HANDLE>& outHandles) {
    HANDLE hVol = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (hVol == INVALID_HANDLE_VALUE) {
        HANDLE probe = CreateFileW(path.c_str(), 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   NULL, OPEN_EXISTING, 0, NULL);
        if (probe == INVALID_HANDLE_VALUE) return true;
        bool ours = VolumeBelongsToDisk(probe, diskNumber);
        CloseHandle(probe);
        return !ours;
    }
    if (!VolumeBelongsToDisk(hVol, diskNumber)) {
        CloseHandle(hVol);
        return true;
    }
    if (!LockAndDismountHandle(hVol, path)) {
        CloseHandle(hVol);
        return false;
    }
    outHandles.push_back(hVol);
    return true;
}

// Locks all volumes and partitions on the disk. Handles are added to outHandles
// and MUST be kept open during write to prevent Windows from denying access.
// Returns false if a volume of this disk could not be locked.
bool LockAndDismountAllDiskVolumes(int diskNumber, std::vector<HANDLE>& outHandles) {
    bool lockFailed = false;
    // 1. Lock drive letters (e.g. \\.\F:)
    DWORD logicalDrives = GetLogicalDrives();
    std::vector<wchar_t> lockedLetters;
    for (int i = 0; i < 26; ++i) {
        if (logicalDrives & (1 << i)) {
            wchar_t driveLetter = L'A' + i;
            wchar_t volPath[] = L"\\\\.\\A:";
            volPath[4] = driveLetter;

            size_t before = outHandles.size();
            if (!TryLockVolume(volPath, diskNumber, outHandles)) {
                lockFailed = true;
            } else if (outHandles.size() > before) {
                lockedLetters.push_back(driveLetter);
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
            if (!TryLockVolume(openPath, diskNumber, outHandles)) lockFailed = true;
        } while (FindNextVolumeW(hFind, volumeName, MAX_PATH));
        FindVolumeClose(hFind);
    }

    // 3. If no volumes were found (e.g. raw / Linux partitions), try locking partitions directly.
    // The path already names this disk, so an open handle that will not lock is a failure.
    if (!lockFailed && outHandles.empty()) {
        for (int p = 1; p <= 16; ++p) {
            std::wstring partPath = L"\\\\?\\GLOBALROOT\\Device\\Harddisk" +
                                    std::to_wstring(diskNumber) + L"\\Partition" + std::to_wstring(p);
            HANDLE hPart = CreateFileW(partPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       NULL, OPEN_EXISTING, 0, NULL);
            if (hPart == INVALID_HANDLE_VALUE) continue;
            if (!LockAndDismountHandle(hPart, partPath)) {
                CloseHandle(hPart);
                lockFailed = true;
            } else {
                outHandles.push_back(hPart);
            }
        }
    }

    RawWriter::Log(L"Total locked volume/partition handles for Disk " +
                   std::to_wstring(diskNumber) + L": " + std::to_wstring(outHandles.size()) +
                   (lockFailed ? L" (lock failed)" : L""));
    return !lockFailed;
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

    for (int attempt = 0; attempt < 10; ++attempt) {
        bool labeled = false;
        DWORD logicalDrives = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if ((logicalDrives & (1 << i)) == 0) continue;
            wchar_t driveLetter = L'A' + i;
            wchar_t volPath[] = L"\\\\.\\A:";
            volPath[4] = driveLetter;

            HANDLE hVol = CreateFileW(volPath, GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      NULL, OPEN_EXISTING, 0, NULL);
            if (hVol == INVALID_HANDLE_VALUE) continue;
            bool belongs = VolumeBelongsToDisk(hVol, diskNumber);
            CloseHandle(hVol);
            if (!belongs) continue;
            wchar_t root[] = L"A:\\";
            root[0] = driveLetter;
            BOOL ok = SetVolumeLabelW(root, newLabel.c_str());
            RawWriter::Log(L"SetVolumeLabelW on " + std::wstring(root) +
                           L" to '" + newLabel + L"': " + (ok ? L"success" : L"failed err=" + std::to_wstring(GetLastError())));
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, root, NULL);
            labeled = true;
        }
        if (labeled) return;
        Sleep(50);
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
    bool commitLabel = false;

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
            if (commitLabel && !volumeLabel.empty() && diskNumber >= 0) {
                ApplyWindowsVolumeLabels(diskNumber, volumeLabel);
            }
        }
    }
};

uint32_t Crc32(const void* data, size_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const BYTE* p = static_cast<const BYTE*>(data);
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t ReadLe32(const BYTE* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void WriteLe32(BYTE* p, uint32_t v) {
    p[0] = (BYTE)v;
    p[1] = (BYTE)(v >> 8);
    p[2] = (BYTE)(v >> 16);
    p[3] = (BYTE)(v >> 24);
}

uint64_t ReadLe64(const BYTE* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

void WriteLe64(BYTE* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = (BYTE)(v & 0xFF);
        v >>= 8;
    }
}

void FillVolumeId(BYTE out[32], const std::wstring& label, bool joliet) {
    if (joliet) {
        for (int i = 0; i < 32; i += 2) {
            out[i] = 0x00;
            out[i + 1] = 0x20;
        }
        size_t n = 0;
        for (wchar_t wc : label) {
            if (n >= 16) break;
            unsigned ch = (wc < 128) ? (unsigned)toupper(wc) : (unsigned)'_';
            out[n * 2] = (BYTE)((ch >> 8) & 0xFF);
            out[n * 2 + 1] = (BYTE)(ch & 0xFF);
            ++n;
        }
        return;
    }
    memset(out, ' ', 32);
    size_t n = 0;
    for (wchar_t wc : label) {
        if (n >= 32) break;
        out[n++] = (wc < 128) ? (BYTE)toupper(wc) : (BYTE)'_';
    }
}

bool IsJoliet(const BYTE* vd) {
    return vd[0] == 2 && memcmp(vd + 1, "CD001", 5) == 0 &&
           vd[88] == 0x25 && vd[89] == 0x2F &&
           (vd[90] == 0x40 || vd[90] == 0x43 || vd[90] == 0x45);
}

bool TailSelfCheck() {
    if (Crc32("123456789", 9) != 0xCBF43926u) return false;
    BYTE le[8] = {};
    WriteLe64(le, 0x1122334455667788ull);
    if (ReadLe64(le) != 0x1122334455667788ull) return false;
    BYTE id[32];
    FillVolumeId(id, L"Ab", false);
    if (id[0] != 'A' || id[1] != 'B' || id[2] != ' ') return false;
    FillVolumeId(id, L"Ab", true);
    if (id[0] != 0x00 || id[1] != 'A' || id[2] != 0x00 || id[3] != 'B') return false;
    if (id[4] != 0x00 || id[5] != 0x20) return false;
    return true;
}

bool ReadSectors(HANDLE h, uint64_t offset, BYTE* data, DWORD size) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    DWORD n = 0;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) && ReadFile(h, data, size, &n, NULL) && n == size;
}

bool WriteSectors(HANDLE h, uint64_t offset, const BYTE* data, DWORD size) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    DWORD n = 0;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) && WriteFile(h, data, size, &n, NULL) && n == size;
}

bool PatchIso9660VolumeLabel(HANDLE hDisk, const std::wstring& newLabel, DWORD sectorSize, bool& patchedOut) {
    patchedOut = false;
    if (newLabel.empty()) return true;

    // ISO 9660 descriptors start at sector 16 (offset 32768). A short read must not be written back:
    // the rest of an aligned buffer is zeros and would wipe the next descriptor.
    DWORD ioSize = (sectorSize < 2048) ? 2048 : sectorSize;
    if (ioSize % sectorSize != 0) ioSize = ((ioSize + sectorSize - 1) / sectorSize) * sectorSize;

    BYTE* buf = (BYTE*)VirtualAlloc(NULL, ioSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return false;

    bool ok = true;
    bool patched = false;
    for (DWORD base = 32768; ok && base < 32768 + 32 * 2048; base += ioSize) {
        DWORD bytesRead = 0;
        LARGE_INTEGER li;
        li.QuadPart = base;
        if (!SetFilePointerEx(hDisk, li, NULL, FILE_BEGIN) ||
            !ReadFile(hDisk, buf, ioSize, &bytesRead, NULL) || bytesRead != ioSize) {
            RawWriter::Log(L"Volume label patch stopped: short read at " + std::to_wstring(base));
            break;
        }

        bool changed = false;
        bool done = false;
        for (DWORD off = 0; off + 2048 <= ioSize; off += 2048) {
            BYTE* vd = buf + off;
            if (vd[0] == 0xFF) {
                done = true;
                break;
            }
            bool joliet = IsJoliet(vd);
            bool primary = vd[0] == 1 && memcmp(vd + 1, "CD001", 5) == 0;
            bool asciiSvd = vd[0] == 2 && memcmp(vd + 1, "CD001", 5) == 0 && !joliet;
            if (!primary && !joliet && !asciiSvd) continue;
            BYTE id[32];
            FillVolumeId(id, newLabel, joliet);
            memcpy(vd + 40, id, 32);
            changed = true;
        }

        if (changed) {
            DWORD bytesWritten = 0;
            if (!SetFilePointerEx(hDisk, li, NULL, FILE_BEGIN) ||
                !WriteFile(hDisk, buf, ioSize, &bytesWritten, NULL) || bytesWritten != ioSize) {
                ok = false;
                break;
            }
            patched = true;
        }
        if (done) break;
    }

    if (ok && patched) {
        FlushFileBuffers(hDisk);
        RawWriter::Log(L"Patched ISO 9660 / Joliet volume label: " + newLabel);
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    patchedOut = patched;
    return ok;
}

bool PatchBootSector(HANDLE hDisk, BootWriteMode mode, DWORD sectorSize, std::wstring& error) {
    if (mode == BootWriteMode::AsImage) return true;

    DWORD io = (sectorSize < 512) ? 512 : sectorSize;
    std::vector<BYTE> sector(io, 0);

    LARGE_INTEGER zero = {0};
    SetFilePointerEx(hDisk, zero, NULL, FILE_BEGIN);
    DWORD bytesRead = 0;
    if (!ReadFile(hDisk, sector.data(), io, &bytesRead, NULL) || bytesRead != io) {
        error = L"Не удалось прочитать загрузочный сектор целиком. Флаги не записаны, чтобы не затереть остаток сектора.";
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

// The image's backup GPT sits at the end of the file. On a larger stick the previous
// backup still occupies the physical end and Windows may resurrect the old layout.
bool FixDiskTail(HANDLE hDisk, uint64_t writtenBytes, uint64_t diskBytes, DWORD sectorSize, std::wstring& note) {
    if (sectorSize < 512 || sectorSize > 1024 * 1024 || diskBytes <= writtenBytes || writtenBytes % sectorSize != 0)
        return true;
    uint64_t usable = diskBytes / sectorSize * sectorSize;
    if (usable <= writtenBytes) return true;
    uint64_t lastLba = usable / sectorSize - 1;

    const DWORD chunk = 1024 * 1024;
    BYTE* zeros = (BYTE*)VirtualAlloc(NULL, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!zeros) return false;
    uint64_t tail = chunk - (chunk % sectorSize);
    uint64_t start = (usable > tail) ? (usable - tail) : 0;
    if (start < writtenBytes) start = writtenBytes;
    bool cleared = false;
    bool ok = true;
    for (uint64_t off = start; ok && off < usable; ) {
        DWORD n = (DWORD)std::min<uint64_t>(chunk, usable - off);
        n = n / sectorSize * sectorSize;
        if (n == 0) break;
        ok = WriteSectors(hDisk, off, zeros, n);
        off += n;
        cleared = cleared || ok;
    }
    VirtualFree(zeros, 0, MEM_RELEASE);
    if (!ok) return false;
    if (cleared) note += L"Последний мегабайт за образом очищен. ";

    BYTE* header = (BYTE*)VirtualAlloc(NULL, sectorSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!header) return false;
    if (!ReadSectors(hDisk, sectorSize, header, sectorSize)) {
        VirtualFree(header, 0, MEM_RELEASE);
        return false;
    }
    if (memcmp(header, "EFI PART", 8) != 0) {
        VirtualFree(header, 0, MEM_RELEASE);
        return true;
    }

    uint32_t headerSize = ReadLe32(header + 12);
    uint64_t backupLba = ReadLe64(header + 32);
    uint64_t entryLba = ReadLe64(header + 72);
    uint32_t entryCount = ReadLe32(header + 80);
    uint32_t entrySize = ReadLe32(header + 84);
    auto finish = [&](bool success) {
        VirtualFree(header, 0, MEM_RELEASE);
        return success;
    };
    if (headerSize < 92 || headerSize > sectorSize || backupLba == lastLba ||
        entryCount == 0 || entrySize == 0 || entrySize > 4096) {
        return finish(true);
    }
    uint64_t entryBytes = (uint64_t)entryCount * entrySize;
    if (entryBytes == 0 || entryBytes > chunk) return finish(true);
    uint64_t entrySectors = (entryBytes + sectorSize - 1) / sectorSize;
    if (lastLba <= entrySectors + 2) return finish(true);
    uint64_t backupEntriesLba = lastLba - entrySectors;
    if (backupEntriesLba * sectorSize < writtenBytes) return finish(true);
    uint64_t lastUsable = backupEntriesLba - 1;
    if (lastUsable < ReadLe64(header + 48)) return finish(true);

    DWORD entryIo = (DWORD)(entrySectors * sectorSize);
    BYTE* entries = (BYTE*)VirtualAlloc(NULL, entryIo, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BYTE* backup = (BYTE*)VirtualAlloc(NULL, sectorSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!entries || !backup) {
        if (entries) VirtualFree(entries, 0, MEM_RELEASE);
        if (backup) VirtualFree(backup, 0, MEM_RELEASE);
        return finish(false);
    }
    if (!ReadSectors(hDisk, entryLba * sectorSize, entries, entryIo)) {
        VirtualFree(entries, 0, MEM_RELEASE);
        VirtualFree(backup, 0, MEM_RELEASE);
        return finish(false);
    }

    memcpy(backup, header, sectorSize);
    uint64_t primaryLba = ReadLe64(header + 24);
    WriteLe64(backup + 24, lastLba);
    WriteLe64(backup + 32, primaryLba);
    WriteLe64(backup + 48, lastUsable);
    WriteLe64(backup + 72, backupEntriesLba);
    WriteLe32(backup + 16, 0);
    WriteLe32(backup + 16, Crc32(backup, headerSize));

    WriteLe64(header + 32, lastLba);
    WriteLe64(header + 48, lastUsable);
    WriteLe32(header + 16, 0);
    WriteLe32(header + 16, Crc32(header, headerSize));

    ok = WriteSectors(hDisk, backupEntriesLba * sectorSize, entries, entryIo) &&
         WriteSectors(hDisk, lastLba * sectorSize, backup, sectorSize) &&
         WriteSectors(hDisk, sectorSize, header, sectorSize);
    VirtualFree(entries, 0, MEM_RELEASE);
    VirtualFree(backup, 0, MEM_RELEASE);
    if (!ok) return finish(false);
    note += L"Запасная GPT перенесена в конец накопителя. ";
    RawWriter::Log(L"Relocated backup GPT to LBA " + std::to_wstring(lastLba));
    return finish(true);
}

} // namespace

bool RawWriter::ConfirmTarget(const WriteParams& params, std::wstring& error) {
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
        error = L"Не удалось открыть диск для проверки: " + GetSystemErrorMessage(GetLastError());
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

WriteResult RawWriter::BurnImage(const WriteParams& params, ProgressFn progressCallback, std::wstring& outErrorMessage) {
    outErrorMessage.clear();
    Log(L"==================================================");
    Log(L"Starting BurnImage for " + params.targetDrive.devicePath +
        L" (Disk " + std::to_wstring(params.targetDrive.physicalDriveIndex) + L")");
    Log(L"Source: " + params.sourceIsoPath);

    if (!TailSelfCheck()) {
        outErrorMessage = L"Внутренняя проверка CRC и метки тома не прошла. Запись отменена.";
        Log(outErrorMessage);
        return WriteResult::WriteError;
    }

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

    // Re-check the same stick immediately before anything destructive.
    std::wstring guardError;
    if (!ConfirmTarget(params, guardError)) {
        outErrorMessage = guardError;
        Log(outErrorMessage);
        return WriteResult::DeviceOpenFailed;
    }

    // 1. Lock and dismount ALL volumes and partitions on target disk.
    // Handles are kept in st.vols and stay open throughout the write!
    if (!LockAndDismountAllDiskVolumes(params.targetDrive.physicalDriveIndex, st.vols)) {
        outErrorMessage = L"Не удалось заблокировать тома накопителя. Закройте все окна с этой флешкой и повторите.";
        Log(outErrorMessage);
        return WriteResult::LockVolumeFailed;
    }

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

        DWORD bytesWritten = 0;
        BOOL ok = FALSE;
        DWORD err = 0;
        for (int retry = 0; retry < 5; ++retry) {
            // A failed or short write leaves the pointer past diskOffset. Rewind every attempt.
            if (!SetFilePointerEx(st.hDisk, li, NULL, FILE_BEGIN)) {
                err = GetLastError();
                ok = FALSE;
                break;
            }
            ok = WriteFile(st.hDisk, st.buffer, writeSize, &bytesWritten, NULL);
            if (ok && bytesWritten == writeSize) break;
            err = ok ? ERROR_WRITE_FAULT : GetLastError();
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
        outErrorMessage = L"Запись отменена. Накопитель уже частично перезаписан, запишите образ ещё раз.";
        Log(outErrorMessage);
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
            progressCallback(100, L"Проверка: образ совпадает с файлом.");
            Sleep(400);
        }
        Log(L"Verification passed successfully: " + std::to_wstring(totalVerified) + L" bytes matched the file.");
    }

    std::wstring adjustNote;
    // Edits below happen after verification, so the final disk is no longer a raw copy.
    if (params.bootMode != BootWriteMode::AsImage) {
        if (progressCallback) progressCallback(100, L"Настройка режима загрузки...");
        if (!PatchBootSector(st.hDisk, params.bootMode, sectorSize, outErrorMessage)) {
            Log(outErrorMessage);
            return WriteResult::WriteError;
        }
        adjustNote += L"Флаги загрузки записаны после проверки образа. ";
    }

    if (!params.volumeLabel.empty()) {
        bool labelPatched = false;
        if (!PatchIso9660VolumeLabel(st.hDisk, params.volumeLabel, sectorSize, labelPatched)) {
            outErrorMessage = L"Не удалось записать метку тома. Дескрипторы ISO не перезаписаны укороченным буфером.";
            Log(outErrorMessage);
            return WriteResult::WriteError;
        }
        if (labelPatched) adjustNote += L"Метка записана в дескрипторы ISO 9660 и Joliet. ";
    }

    if (!FixDiskTail(st.hDisk, diskOffset, diskBytes, sectorSize, adjustNote)) {
        outErrorMessage = L"Не удалось очистить хвост накопителя или перенести запасную GPT.";
        Log(outErrorMessage);
        return WriteResult::WriteError;
    }
    if (!adjustNote.empty()) outErrorMessage = adjustNote;

    st.commitLabel = true;
    Log(L"BurnImage completed successfully!");
    return WriteResult::Success;
}
