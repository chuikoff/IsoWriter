#include "raw_writer.h"
#include <shlobj.h>
#include <winioctl.h>
#include <ntddscsi.h>
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

void LogDebug(const std::wstring& msg) { RawWriter::Log(msg); }

bool IsTransientWriteError(DWORD err) {
    return err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION ||
           err == ERROR_LOCK_VIOLATION || err == ERROR_BUSY || err == ERROR_NOT_READY;
}

// Lock first, then dismount. A dismount without a lock does not keep the volume closed.
bool LockAndDismountVolume(HANDLE hVol, const std::wstring& name) {
    DWORD bytesRet = 0;
    DeviceIoControl(hVol, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &bytesRet, NULL);

    BOOL bLock = FALSE;
    DWORD lockErr = 0;
    for (int retry = 0; retry < 10; ++retry) {
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

    LogDebug(L"Volume " + name + L": Lock=" + std::to_wstring(bLock) +
             L" err=" + std::to_wstring(lockErr) +
             L", Dismount=" + std::to_wstring(bDismount) +
             L" err=" + std::to_wstring(dismountErr));
    return bLock && bDismount;
}

bool KeepLocked(HANDLE hVol, const std::wstring& name, std::vector<HANDLE>& outHandles) {
    if (LockAndDismountVolume(hVol, name)) {
        outHandles.push_back(hVol);
        return true;
    }
    CloseHandle(hVol);
    return false;
}

// Yes = this disk, No = another disk, Unknown = the query itself failed.
enum class VolumeMatch { No, Yes, Unknown };

VolumeMatch VolumeIsOnDisk(HANDLE hVol, int diskNumber, DWORD& err) {
    err = 0;
    BYTE buffer[4096] = {0};
    VOLUME_DISK_EXTENTS* extents = (VOLUME_DISK_EXTENTS*)buffer;
    DWORD bytesRet = 0;
    if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                        NULL, 0, buffer, sizeof(buffer), &bytesRet, NULL) &&
        bytesRet >= sizeof(VOLUME_DISK_EXTENTS)) {
        DWORD count = extents->NumberOfDiskExtents;
        DWORD maxFit = (DWORD)((sizeof(buffer) - sizeof(DWORD)) / sizeof(DISK_EXTENT));
        if (count > maxFit) count = maxFit;
        for (DWORD n = 0; n < count; ++n) {
            if (extents->Extents[n].DiskNumber == (DWORD)diskNumber) return VolumeMatch::Yes;
        }
        return VolumeMatch::No;
    }
    DWORD extErr = GetLastError();

    STORAGE_DEVICE_NUMBER sdn = {0};
    if (DeviceIoControl(hVol, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                        NULL, 0, &sdn, sizeof(sdn), &bytesRet, NULL)) {
        if (sdn.DeviceType == FILE_DEVICE_DISK && (int)sdn.DeviceNumber == diskNumber)
            return VolumeMatch::Yes;
        return VolumeMatch::No;
    }
    err = extErr ? extErr : GetLastError();
    return VolumeMatch::Unknown;
}

// One lock per volume, by GUID. A drive letter and a mount folder are the same volume.
// Returns false when a volume on this disk could not be locked.
bool LockAndDismountAllDiskVolumes(int diskNumber, std::vector<HANDLE>& outHandles) {
    bool failed = false;
    int scanned = 0;
    int queryFailed = 0;
    int found = 0;

    wchar_t volumeName[MAX_PATH];
    HANDLE hFind = FindFirstVolumeW(volumeName, MAX_PATH);
    if (hFind == INVALID_HANDLE_VALUE) {
        LogDebug(L"FindFirstVolume failed err=" + std::to_wstring(GetLastError()));
        return false;
    }

    do {
        ++scanned;
        std::wstring openPath = volumeName;
        if (!openPath.empty() && openPath.back() == L'\\') openPath.pop_back();

        HANDLE hQuery = CreateFileW(openPath.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, 0, NULL);
        if (hQuery == INVALID_HANDLE_VALUE) {
            LogDebug(L"Volume query open failed: " + openPath + L" err=" + std::to_wstring(GetLastError()));
            ++queryFailed;
            continue;
        }
        DWORD queryErr = 0;
        VolumeMatch match = VolumeIsOnDisk(hQuery, diskNumber, queryErr);
        CloseHandle(hQuery);
        if (match == VolumeMatch::Unknown) {
            LogDebug(L"Volume disk query failed: " + openPath + L" err=" + std::to_wstring(queryErr));
            ++queryFailed;
            continue;
        }
        if (match != VolumeMatch::Yes) continue;

        ++found;
        wchar_t mounts[512] = {0};
        DWORD needed = 0;
        GetVolumePathNamesForVolumeNameW(volumeName, mounts, 512, &needed);
        LogDebug(L"Volume on disk " + std::to_wstring(diskNumber) + L": " + openPath +
                 L" mount=" + (mounts[0] ? std::wstring(mounts) : L"(none)"));

        HANDLE hVol = INVALID_HANDLE_VALUE;
        DWORD openErr = 0;
        for (int retry = 0; retry < 5; ++retry) {
            hVol = CreateFileW(openPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
            if (hVol != INVALID_HANDLE_VALUE) break;
            openErr = GetLastError();
            Sleep(50);
        }
        if (hVol == INVALID_HANDLE_VALUE) {
            LogDebug(L"Volume open failed: " + openPath + L" err=" + std::to_wstring(openErr));
            failed = true;
            continue;
        }
        if (!KeepLocked(hVol, openPath, outHandles)) failed = true;
    } while (FindNextVolumeW(hFind, volumeName, MAX_PATH));
    FindVolumeClose(hFind);

    LogDebug(L"Volumes scanned=" + std::to_wstring(scanned) +
             L" queryFailed=" + std::to_wstring(queryFailed) +
             L" on target disk=" + std::to_wstring(found));
    return !failed;
}

void ReleaseVolumes(std::vector<HANDLE>& vols) {
    DWORD bytesRet = 0;
    for (HANDLE h : vols) {
        if (h == INVALID_HANDLE_VALUE) continue;
        DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &bytesRet, NULL);
        CloseHandle(h);
    }
    vols.clear();
}

// FILE_ATTRIBUTE_NORMAL, no FILE_SHARE_WRITE, then share write after a few denials.
HANDLE OpenPhysicalForWrite(const std::wstring& path) {
    const int retries = 12;
    bool writeShare = false;
    DWORD err = ERROR_ACCESS_DENIED;
    for (int i = 0; i < retries; ++i) {
        if (!writeShare && i > retries / 3) {
            writeShare = true;
            LogDebug(L"Exclusive open failed, retrying with FILE_SHARE_WRITE");
        }
        DWORD share = FILE_SHARE_READ | (writeShare ? FILE_SHARE_WRITE : 0);
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, share,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            LogDebug(std::wstring(L"Opened ") + path + (writeShare ? L" shared" : L" exclusive"));
            return h;
        }
        err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) break;
        Sleep(100);
    }
    SetLastError(err);
    LogDebug(L"Open physical failed err=" + std::to_wstring(err));
    return INVALID_HANDLE_VALUE;
}

// Lock the physical handle before an image write.
// shortcut: no IOCTL_DISK_CREATE_DISK. On KDI-MSFT it returns success, the FAT16 table stays, and the stick then goes not-ready.
void PreparePhysicalForImage(HANDLE hDisk) {
    DWORD bytesRet = 0;
    BOOL dasd = DeviceIoControl(hDisk, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &bytesRet, NULL);
    LogDebug(L"ALLOW_EXTENDED_DASD_IO=" + std::to_wstring(dasd) +
             L" err=" + std::to_wstring(dasd ? 0 : GetLastError()));

    BOOL locked = FALSE;
    DWORD lockErr = 0;
    for (int i = 0; i < 20 && !locked; ++i) {
        locked = DeviceIoControl(hDisk, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytesRet, NULL);
        if (!locked) {
            lockErr = GetLastError();
            Sleep(50);
        }
    }
    BOOL dismount = FALSE;
    DWORD dismountErr = 0;
    if (locked) {
        dismount = DeviceIoControl(hDisk, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &bytesRet, NULL);
        if (!dismount) dismountErr = GetLastError();
    }
    LogDebug(L"Physical Lock=" + std::to_wstring(locked) + L" err=" + std::to_wstring(lockErr) +
             L" Dismount=" + std::to_wstring(dismount) + L" err=" + std::to_wstring(dismountErr));
}

struct ScsiWriteBuf {
    SCSI_PASS_THROUGH_DIRECT sptd;
    ULONG filler;
    UCHAR sense[32];
};

// shortcut: WRITE(10) only, so the disk must be under 2 TiB and the chunk under 65535 sectors.
bool ScsiWriteAt(HANDLE hDisk, uint64_t offset, BYTE* data, DWORD size, DWORD sectorSize) {
    if (sectorSize == 0) sectorSize = 512;
    if (size == 0 || offset % sectorSize != 0 || size % sectorSize != 0) return false;
    uint64_t lba = offset / sectorSize;
    uint64_t blocks = (uint64_t)size / sectorSize;
    if (lba > 0xFFFFFFFFull || blocks == 0 || blocks > 0xFFFF) return false;

    ScsiWriteBuf buf = {};
    buf.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    buf.sptd.CdbLength = 10;
    buf.sptd.SenseInfoLength = sizeof(buf.sense);
    buf.sptd.DataIn = SCSI_IOCTL_DATA_OUT;
    buf.sptd.DataTransferLength = size;
    buf.sptd.TimeOutValue = 20;
    buf.sptd.DataBuffer = data;
    buf.sptd.SenseInfoOffset = offsetof(ScsiWriteBuf, sense);
    buf.sptd.Cdb[0] = 0x2A;
    buf.sptd.Cdb[2] = (UCHAR)(lba >> 24);
    buf.sptd.Cdb[3] = (UCHAR)(lba >> 16);
    buf.sptd.Cdb[4] = (UCHAR)(lba >> 8);
    buf.sptd.Cdb[5] = (UCHAR)lba;
    buf.sptd.Cdb[7] = (UCHAR)(blocks >> 8);
    buf.sptd.Cdb[8] = (UCHAR)blocks;

    DWORD bytesRet = 0;
    BOOL ok = DeviceIoControl(hDisk, IOCTL_SCSI_PASS_THROUGH_DIRECT,
                              &buf, sizeof(buf), &buf, sizeof(buf), &bytesRet, NULL);
    if (ok && buf.sptd.ScsiStatus == 0) return true;

    DWORD key = buf.sense[2] & 0x0F;
    DWORD err = GetLastError();
    if (ok) {
        if (key == 0x02) err = ERROR_NOT_READY;
        else if (key == 0x07) err = ERROR_WRITE_PROTECT;
        else err = ERROR_WRITE_FAULT;
    }
    LogDebug(L"SCSI WRITE(10) offset=" + std::to_wstring(offset) +
             L" size=" + std::to_wstring(size) +
             L" ok=" + std::to_wstring(ok) +
             L" status=" + std::to_wstring(buf.sptd.ScsiStatus) +
             L" sense=" + std::to_wstring(key) +
             L"/" + std::to_wstring(buf.sense[12]) +
             L"/" + std::to_wstring(buf.sense[13]) +
             L" err=" + std::to_wstring(err));
    SetLastError(err);
    return false;
}

bool WriteAt(HANDLE hDisk, uint64_t offset, BYTE* data, DWORD size, DWORD sectorSize, bool& useScsi) {
    DWORD lastErr = 0;
    if (!useScsi) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)offset;
        for (int retry = 0; retry < 3; ++retry) {
            if (!SetFilePointerEx(hDisk, li, NULL, FILE_BEGIN)) {
                lastErr = GetLastError();
                LogDebug(L"SetFilePointerEx failed offset=" + std::to_wstring(offset) +
                         L" err=" + std::to_wstring(lastErr));
            } else {
                DWORD bytesWritten = 0;
                BOOL wrote = WriteFile(hDisk, data, size, &bytesWritten, NULL);
                if (wrote && bytesWritten == size) return true;
                lastErr = wrote ? ERROR_WRITE_FAULT : GetLastError();
                LogDebug(L"WriteFile offset=" + std::to_wstring(offset) +
                         L" requested=" + std::to_wstring(size) +
                         L" written=" + std::to_wstring(bytesWritten) +
                         L" ok=" + std::to_wstring(wrote) +
                         L" err=" + std::to_wstring(lastErr) +
                         L" retry=" + std::to_wstring(retry + 1));
            }
            // Error 1 (invalid function) is how this stick rejects WriteFile after a few sectors.
            if (lastErr == ERROR_ACCESS_DENIED || lastErr == ERROR_NOT_READY ||
                lastErr == ERROR_WRITE_PROTECT || lastErr == ERROR_INVALID_FUNCTION)
                break;
            if (!IsTransientWriteError(lastErr)) break;
            Sleep(200);
        }
    }

    if (useScsi || lastErr == ERROR_ACCESS_DENIED || lastErr == ERROR_NOT_READY ||
        lastErr == ERROR_WRITE_PROTECT || lastErr == ERROR_INVALID_FUNCTION) {
        if (ScsiWriteAt(hDisk, offset, data, size, sectorSize)) {
            if (!useScsi)
                LogDebug(L"WriteFile err=" + std::to_wstring(lastErr) + L", switched to SCSI WRITE(10)");
            useScsi = true;
            return true;
        }
    }
    SetLastError(lastErr ? lastErr : GetLastError());
    return false;
}

bool SafeWriteChunk(HANDLE hDisk, uint64_t offset, BYTE* buffer, DWORD sizeToWrite, DWORD sectorSize, bool& useScsi) {
    if (WriteAt(hDisk, offset, buffer, sizeToWrite, sectorSize, useScsi)) {
        return true;
    }

    DWORD firstErr = GetLastError();
    LogDebug(L"Full chunk WriteFile failed at offset=" + std::to_wstring(offset) +
             L", size=" + std::to_wstring(sizeToWrite) +
             L", err=" + std::to_wstring(firstErr) + L" (" + GetSystemErrorMessage(firstErr) + L")");
    if (firstErr == ERROR_ACCESS_DENIED) {
        SetLastError(firstErr);
        return false;
    }

    DWORD subChunk = 4096;
    if (subChunk < sectorSize || (sectorSize != 0 && subChunk % sectorSize != 0)) {
        subChunk = sectorSize;
    }

    DWORD pos = 0;
    while (pos < sizeToWrite) {
        DWORD curWrite = std::min(subChunk, sizeToWrite - pos);
        if (!WriteAt(hDisk, offset + pos, buffer + pos, curWrite, sectorSize, useScsi)) {
            DWORD subErr = GetLastError();
            LogDebug(L"Sub-write failed at offset " + std::to_wstring(offset + pos) +
                     L", size=" + std::to_wstring(curWrite) +
                     L", err=" + std::to_wstring(subErr) + L" (" + GetSystemErrorMessage(subErr) + L")");
            SetLastError(subErr);
            return false;
        }
        pos += curWrite;
    }
    return true;
}

void NotifyDriveLetters(const std::wstring& driveLetters) {
    for (wchar_t c : driveLetters) {
        if ((c < L'A' || c > L'Z') && (c < L'a' || c > L'z')) continue;
        wchar_t root[] = L"A:\\";
        root[0] = towupper(c);
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, root, NULL);
    }
}

struct BurnCleanup {
    HANDLE hIso = INVALID_HANDLE_VALUE;
    HANDLE hDisk = INVALID_HANDLE_VALUE;
    std::vector<HANDLE> vols;
    BYTE* buffer = nullptr;
    BYTE* verifyBuffer = nullptr;
    bool refresh = false;
    std::wstring devicePath;
    std::wstring driveLetters;

    BurnCleanup() = default;
    BurnCleanup(const BurnCleanup&) = delete;
    BurnCleanup& operator=(const BurnCleanup&) = delete;

    ~BurnCleanup() {
        if (buffer) VirtualFree(buffer, 0, MEM_RELEASE);
        if (verifyBuffer) VirtualFree(verifyBuffer, 0, MEM_RELEASE);

        DWORD bytesRet = 0;
        for (HANDLE h : vols) {
            if (h != INVALID_HANDLE_VALUE) {
                DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &bytesRet, NULL);
                CloseHandle(h);
            }
        }
        vols.clear();

        if (refresh) {
            HANDLE hRefresh = hDisk;
            bool closeTemp = false;
            if (hRefresh == INVALID_HANDLE_VALUE && !devicePath.empty()) {
                hRefresh = CreateFileW(devicePath.c_str(), 0,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       NULL, OPEN_EXISTING, 0, NULL);
                closeTemp = hRefresh != INVALID_HANDLE_VALUE;
            }
            if (hRefresh != INVALID_HANDLE_VALUE) {
                DeviceIoControl(hRefresh, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &bytesRet, NULL);
                if (closeTemp) CloseHandle(hRefresh);
            }
            NotifyDriveLetters(driveLetters);
        }

        if (hDisk != INVALID_HANDLE_VALUE) CloseHandle(hDisk);
        if (hIso != INVALID_HANDLE_VALUE) CloseHandle(hIso);
    }
};

bool ReadDiskAt(HANDLE hDisk, uint64_t offset, BYTE* data, DWORD size, DWORD& bytesRead) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    bytesRead = 0;
    if (!SetFilePointerEx(hDisk, li, NULL, FILE_BEGIN)) return false;
    if (ReadFile(hDisk, data, size, &bytesRead, NULL) && bytesRead >= size) return true;

    bytesRead = 0;
    if (!SetFilePointerEx(hDisk, li, NULL, FILE_BEGIN)) return false;
    return ReadFile(hDisk, data, size, &bytesRead, NULL) && bytesRead >= size;
}

// shortcut: UEFI/Legacy only changes the MBR active flag. It does not format the stick or copy files out of the ISO.
bool ApplyBootFlags(BYTE* sector, BootWriteMode mode, bool diskHasGpt, std::wstring& error) {
    if (mode == BootWriteMode::AsImage) return true;
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        error = L"В образе нет подписи MBR (55 AA). Режимы UEFI, Legacy и оба для него недоступны. Запись байтов образа уже выполнена.";
        return false;
    }

    int legacy = -1;
    bool efi = diskHasGpt;
    for (int i = 0; i < 4; ++i) {
        BYTE type = sector[446 + i * 16 + 4];
        if (type == 0x00 || type == 0xEE) continue;
        if (type == 0xEF) {
            efi = true;
            continue;
        }
        if (legacy < 0) legacy = i;
    }

    bool wantUefi = mode == BootWriteMode::Uefi || mode == BootWriteMode::Both;
    bool wantLegacy = mode == BootWriteMode::Legacy || mode == BootWriteMode::Both;
    if (wantUefi && !efi) {
        error = L"В образе нет раздела EFI и нет GPT. Режим UEFI не применён. Байты образа уже записаны.";
        return false;
    }
    if (wantLegacy && legacy < 0) {
        error = L"В образе нет обычного раздела для Legacy BIOS. Режим Legacy не применён. Байты образа уже записаны.";
        return false;
    }

    for (int i = 0; i < 4; ++i) sector[446 + i * 16] = 0;
    if (wantLegacy) sector[446 + legacy * 16] = 0x80;
    return true;
}

bool BootFlagsSelfCheck() {
    BYTE sector[512] = {};
    sector[510] = 0x55;
    sector[511] = 0xAA;
    sector[446 + 4] = 0x07;
    sector[446] = 0x00;
    std::wstring err;
    if (!ApplyBootFlags(sector, BootWriteMode::Legacy, false, err) || sector[446] != 0x80) return false;

    BYTE hybrid[512] = {};
    hybrid[510] = 0x55;
    hybrid[511] = 0xAA;
    hybrid[446 + 4] = 0xEF;
    hybrid[446 + 16 + 4] = 0x07;
    hybrid[446 + 16] = 0x80;
    if (!ApplyBootFlags(hybrid, BootWriteMode::Both, false, err)) return false;
    if (hybrid[446] != 0x00 || hybrid[446 + 16] != 0x80) return false;

    BYTE efiOnly[512] = {};
    efiOnly[510] = 0x55;
    efiOnly[511] = 0xAA;
    efiOnly[446 + 4] = 0xEF;
    efiOnly[446] = 0x80;
    if (!ApplyBootFlags(efiOnly, BootWriteMode::Uefi, false, err) || efiOnly[446] != 0x00) return false;
    if (ApplyBootFlags(efiOnly, BootWriteMode::Legacy, false, err)) return false;

    BYTE raw[512] = {};
    if (ApplyBootFlags(raw, BootWriteMode::Uefi, false, err)) return false;
    raw[446] = 0x80;
    if (!ApplyBootFlags(raw, BootWriteMode::AsImage, false, err) || raw[446] != 0x80) return false;
    return true;
}

bool PatchBootSector(HANDLE hDisk, BootWriteMode mode, DWORD sectorSize, bool& useScsi, std::wstring& error) {
    if (mode == BootWriteMode::AsImage) return true;
    if (!BootFlagsSelfCheck()) {
        error = L"Внутренняя проверка режима загрузки не прошла.";
        return false;
    }
    DWORD io = sectorSize < 512 ? 512 : sectorSize;
    std::vector<BYTE> first(io, 0), second(io, 0);
    DWORD n1 = 0, n2 = 0;
    if (!ReadDiskAt(hDisk, 0, first.data(), io, n1) || n1 < 512) {
        error = L"Образ записан, но не удалось прочитать первый сектор для смены режима загрузки.";
        return false;
    }
    bool gpt = ReadDiskAt(hDisk, io, second.data(), io, n2) && n2 >= 8 &&
               memcmp(second.data(), "EFI PART", 8) == 0;
    if (!ApplyBootFlags(first.data(), mode, gpt, error)) return false;
    if (!WriteAt(hDisk, 0, first.data(), io, sectorSize == 0 ? 512 : sectorSize, useScsi)) {
        error = L"Образ записан, но не удалось записать флаг загрузки: " + GetSystemErrorMessage(GetLastError());
        return false;
    }
    LogDebug(std::wstring(L"Boot flags applied: ") + BootWriteModeName(mode));
    return true;
}

} // namespace

// Windows mounts an EFI partition as soon as sector 0 names it, then denies writes into that range.
// Returns how many leading bytes were saved and zeroed in the buffer.
DWORD HoldLeadingSectors(BYTE* buffer, DWORD size, DWORD sectorSize, std::vector<BYTE>& saved) {
    saved.clear();
    if (buffer == nullptr || sectorSize == 0 || size < sectorSize) return 0;
    DWORD hold = sectorSize;
    if (sectorSize == 512 && size >= 1024 && memcmp(buffer + 512, "EFI PART", 8) == 0)
        hold = 1024;
    saved.assign(buffer, buffer + hold);
    memset(buffer, 0, hold);
    return hold;
}

bool HoldLeadingSelfCheck() {
    BYTE mbr[1024];
    memset(mbr, 0xAB, sizeof(mbr));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    std::vector<BYTE> saved;
    if (HoldLeadingSectors(mbr, 1024, 512, saved) != 512) return false;
    if (saved.size() != 512 || saved[0] != 0xAB || saved[510] != 0x55) return false;
    if (mbr[0] != 0 || mbr[511] != 0 || mbr[512] != 0xAB) return false;

    BYTE gpt[1024];
    memset(gpt, 0xCD, sizeof(gpt));
    memcpy(gpt + 512, "EFI PART", 8);
    if (HoldLeadingSectors(gpt, 1024, 512, saved) != 1024) return false;
    if (saved.size() != 1024 || memcmp(saved.data() + 512, "EFI PART", 8) != 0) return false;
    if (gpt[0] != 0 || gpt[512] != 0 || gpt[1023] != 0) return false;

    BYTE big[4096];
    memset(big, 0x11, sizeof(big));
    memcpy(big + 512, "EFI PART", 8);
    if (HoldLeadingSectors(big, 4096, 4096, saved) != 4096) return false;
    if (big[0] != 0 || big[4095] != 0) return false;

    BYTE tiny[256];
    memset(tiny, 0x22, sizeof(tiny));
    if (HoldLeadingSectors(tiny, 256, 512, saved) != 0 || !saved.empty()) return false;
    return tiny[0] == 0x22;
}

WriteResult RawWriter::BurnImage(const WriteParams& params, ProgressFn progressCallback, std::wstring& outErrorMessage) {
    outErrorMessage.clear();
    LogDebug(L"==================================================");
    LogDebug(L"Log file: " + LogFilePath());
    LogDebug(L"Starting BurnImage for " + params.targetDrive.devicePath +
             L" index=" + std::to_wstring(params.targetDrive.physicalDriveIndex) +
             L" letters=" + params.targetDrive.driveLetters +
             L" sector=" + std::to_wstring(params.targetDrive.bytesPerSector) +
             L" size=" + std::to_wstring(params.targetDrive.totalSizeBytes));
    LogDebug(L"Source ISO: " + params.sourceIsoPath);

    int sysDrive = DriveEnumerator::GetSystemDriveIndex();
    if (sysDrive < 0) {
        outErrorMessage = L"Не удалось определить системный диск. Запись отменена.";
        LogDebug(outErrorMessage);
        return WriteResult::DeviceOpenFailed;
    }
    if (params.targetDrive.physicalDriveIndex == sysDrive) {
        outErrorMessage = L"Критическая ошибка безопасности: попытка записи на системный диск!";
        LogDebug(outErrorMessage);
        return WriteResult::DeviceOpenFailed;
    }

    if (params.targetDrive.totalSizeBytes == 0) {
        outErrorMessage = L"Не удалось определить ёмкость накопителя. Запись отменена.";
        LogDebug(outErrorMessage);
        return WriteResult::DiskTooSmall;
    }

    BurnCleanup st;
    st.devicePath = params.targetDrive.devicePath;
    st.driveLetters = params.targetDrive.driveLetters;

    st.hIso = CreateFileW(params.sourceIsoPath.c_str(), GENERIC_READ,
                          FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (st.hIso == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        outErrorMessage = L"Не удалось открыть исходный файл ISO: " + GetSystemErrorMessage(err);
        LogDebug(outErrorMessage);
        return WriteResult::SourceOpenFailed;
    }

    LARGE_INTEGER isoSize = {0};
    if (!GetFileSizeEx(st.hIso, &isoSize) || isoSize.QuadPart <= 0) {
        outErrorMessage = L"Не удалось определить размер ISO файла.";
        LogDebug(outErrorMessage);
        return WriteResult::SourceOpenFailed;
    }
    LogDebug(L"ISO size: " + std::to_wstring(isoSize.QuadPart) + L" bytes");

    DWORD sectorSize = params.targetDrive.bytesPerSector;
    if (sectorSize == 0) sectorSize = 512;

    uint64_t isoBytes = (uint64_t)isoSize.QuadPart;
    uint64_t diskBytes = params.targetDrive.totalSizeBytes;
    if (isoBytes > diskBytes) {
        outErrorMessage = L"Размер ISO (" + DriveEnumerator::FormatSize(isoBytes) +
                          L") превышает емкость накопителя (" +
                          DriveEnumerator::FormatSize(diskBytes) + L")!";
        LogDebug(outErrorMessage);
        return WriteResult::DiskTooSmall;
    }
    uint64_t remainder = isoBytes % sectorSize;
    if (remainder != 0 && diskBytes - isoBytes < (sectorSize - remainder)) {
        outErrorMessage = L"На накопителе нет места для выравнивания последнего сектора.";
        LogDebug(outErrorMessage);
        return WriteResult::DiskTooSmall;
    }

    if (!HoldLeadingSelfCheck()) {
        outErrorMessage = L"Внутренняя проверка записи таблицы разделов не прошла.";
        LogDebug(outErrorMessage);
        return WriteResult::WriteError;
    }

    if (!LockAndDismountAllDiskVolumes(params.targetDrive.physicalDriveIndex, st.vols)) {
        st.refresh = true;
        outErrorMessage = L"Не удалось заблокировать тома накопителя. Закройте файлы на нём и повторите запись.";
        LogDebug(outErrorMessage + L" Locked handles: " + std::to_wstring(st.vols.size()));
        return WriteResult::LockVolumeFailed;
    }
    st.refresh = true;
    LogDebug(L"Locked volumes count: " + std::to_wstring(st.vols.size()));
    // Close them before the exclusive physical open. A held partition handle makes sector 0 return error 5.
    ReleaseVolumes(st.vols);

    st.hDisk = OpenPhysicalForWrite(params.targetDrive.devicePath);
    if (st.hDisk == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            outErrorMessage = L"Отказано в доступе к физическому диску.\n"
                              L"Для прямой записи на USB требуются права Администратора.\n"
                              L"Запустите Total Commander от имени Администратора.";
            LogDebug(outErrorMessage);
            return WriteResult::ElevationRequired;
        }
        outErrorMessage = L"Не удалось открыть накопитель " + params.targetDrive.devicePath +
                          L": " + GetSystemErrorMessage(err);
        LogDebug(outErrorMessage);
        return WriteResult::DeviceOpenFailed;
    }
    LogDebug(L"Opened " + params.targetDrive.devicePath + L" successfully");

    DWORD bytesRet = 0;
    BOOL writable = DeviceIoControl(st.hDisk, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, &bytesRet, NULL);
    LogDebug(L"IOCTL_DISK_IS_WRITABLE=" + std::to_wstring(writable) +
             L" err=" + std::to_wstring(writable ? 0 : GetLastError()));

    PreparePhysicalForImage(st.hDisk);

    const DWORD chunkSize = 64 * 1024;
    LogDebug(L"Sector size: " + std::to_wstring(sectorSize) + L", Chunk size: " + std::to_wstring(chunkSize));

    st.buffer = (BYTE*)VirtualAlloc(NULL, chunkSize + sectorSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!st.buffer) {
        outErrorMessage = L"Не удалось выделить память для буфера записи.";
        LogDebug(outErrorMessage);
        return WriteResult::WriteError;
    }

    uint64_t totalBytesWritten = 0;
    uint64_t diskOffset = 0;
    bool aborted = false;
    bool useScsi = false;
    std::vector<BYTE> leading;

    if (progressCallback) {
        progressCallback(0, L"Начало записи на USB...");
    }

    while (true) {
        DWORD bytesRead = 0;
        if (!ReadFile(st.hIso, st.buffer, chunkSize, &bytesRead, NULL)) {
            DWORD err = GetLastError();
            outErrorMessage = L"Ошибка чтения ISO (код " + std::to_wstring(err) + L"): " + GetSystemErrorMessage(err);
            LogDebug(outErrorMessage);
            return WriteResult::WriteError;
        }
        if (bytesRead == 0) break;

        DWORD writeSize = bytesRead;
        DWORD tail = writeSize % sectorSize;
        if (tail != 0) {
            DWORD padding = sectorSize - tail;
            memset(st.buffer + writeSize, 0, padding);
            writeSize += padding;
        }

        if (diskOffset == 0) {
            DWORD held = HoldLeadingSectors(st.buffer, writeSize, sectorSize, leading);
            if (held != 0)
                LogDebug(L"Holding leading " + std::to_wstring(held) + L" bytes until the image body is written");
        }

        if (!SafeWriteChunk(st.hDisk, diskOffset, st.buffer, writeSize, sectorSize, useScsi)) {
            DWORD err = GetLastError();
            LogDebug(L"Write failed at offset " + std::to_wstring(diskOffset) +
                     L", err=" + std::to_wstring(err) + L" (" + GetSystemErrorMessage(err) + L")");
            if (err == ERROR_ACCESS_DENIED || err == ERROR_NOT_READY ||
                err == ERROR_WRITE_PROTECT || err == ERROR_INVALID_FUNCTION) {
                outErrorMessage = L"Контроллер отклонил запись сектора (код " + std::to_wstring(err) +
                                  L": " + GetSystemErrorMessage(err) + L"). "
                                  L"Диск уже открыт монопольно, и Windows не считает его только для чтения.";
                if (params.targetDrive.vendor.find(L"KDI-MSFT") != std::wstring::npos) {
                    outErrorMessage += L"\n\nЭто флешка KDI-MSFT Windows 10, контроллер Phison PS2251-07 "
                                       L"(USB VID 0951, PID 16AA). Защита стоит в прошивке: "
                                       L"таблица разделов на носителе не меняется. "
                                       L"Её снимают утилитой Phison Format & Restore для PS2251-07, "
                                       L"затем флешку вставляют заново.";
                }
            } else {
                outErrorMessage = L"Ошибка записи на диск (код " + std::to_wstring(err) + L"): " + GetSystemErrorMessage(err);
            }
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
    LogDebug(L"Write loop finished, total bytes written: " + std::to_wstring(totalBytesWritten));

    if (aborted) {
        LogDebug(L"Запись отменена пользователем.");
        return WriteResult::Cancelled;
    }
    if (totalBytesWritten != isoBytes) {
        outErrorMessage = L"Записано " + DriveEnumerator::FormatSize(totalBytesWritten) +
                          L" из " + DriveEnumerator::FormatSize(isoBytes) + L". Образ на накопителе неполный.";
        LogDebug(outErrorMessage);
        return WriteResult::WriteError;
    }

    if (!leading.empty()) {
        if (!SafeWriteChunk(st.hDisk, 0, leading.data(), (DWORD)leading.size(), sectorSize, useScsi)) {
            DWORD err = GetLastError();
            outErrorMessage = L"Образ записан, но не удалось вернуть таблицу разделов: " + GetSystemErrorMessage(err);
            LogDebug(outErrorMessage);
            return WriteResult::WriteError;
        }
        FlushFileBuffers(st.hDisk);
        LogDebug(L"Leading bytes written last: " + std::to_wstring(leading.size()));
    }

    if (params.verifyAfterWrite) {
        if (progressCallback) {
            progressCallback(0, L"Верификация данных...");
        }

        st.verifyBuffer = (BYTE*)VirtualAlloc(NULL, chunkSize + sectorSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!st.verifyBuffer) {
            outErrorMessage = L"Запись завершена, но не удалось выделить память для проверки.";
            LogDebug(outErrorMessage);
            return WriteResult::VerificationError;
        }

        LARGE_INTEGER zeroOffset = {0};
        if (!SetFilePointerEx(st.hIso, zeroOffset, NULL, FILE_BEGIN)) {
            outErrorMessage = L"Не удалось вернуться к началу ISO для проверки.";
            LogDebug(outErrorMessage);
            return WriteResult::VerificationError;
        }

        uint64_t totalVerified = 0;
        uint64_t verifyDiskOffset = 0;
        while (true) {
            DWORD bytesFromIso = 0;
            if (!ReadFile(st.hIso, st.buffer, chunkSize, &bytesFromIso, NULL)) {
                DWORD err = GetLastError();
                outErrorMessage = L"Ошибка чтения ISO при проверке: " + GetSystemErrorMessage(err);
                LogDebug(outErrorMessage);
                return WriteResult::VerificationError;
            }
            if (bytesFromIso == 0) break;

            DWORD readSize = bytesFromIso;
            DWORD tail = readSize % sectorSize;
            if (tail != 0) readSize += sectorSize - tail;

            DWORD bytesFromDisk = 0;
            if (!ReadDiskAt(st.hDisk, verifyDiskOffset, st.verifyBuffer, readSize, bytesFromDisk) ||
                bytesFromDisk < bytesFromIso) {
                DWORD err = GetLastError();
                outErrorMessage = L"Ошибка чтения с диска при проверке: " + GetSystemErrorMessage(err);
                LogDebug(outErrorMessage);
                return WriteResult::VerificationError;
            }

            if (memcmp(st.buffer, st.verifyBuffer, bytesFromIso) != 0) {
                outErrorMessage = L"Несоответствие данных при проверке на смещении: " +
                                  std::to_wstring(totalVerified) + L" байт.";
                LogDebug(outErrorMessage);
                return WriteResult::VerificationError;
            }

            verifyDiskOffset += readSize;
            totalVerified += bytesFromIso;
            int percent = (int)((totalVerified * 100) / isoBytes);
            if (percent > 100) percent = 100;

            if (progressCallback) {
                std::wstring status = L"Проверка: " + DriveEnumerator::FormatSize(totalVerified) +
                                      L" из " + DriveEnumerator::FormatSize(isoBytes);
                if (!progressCallback(percent, status)) {
                    aborted = true;
                    break;
                }
            }
        }

        if (aborted) {
            outErrorMessage = L"Проверка отменена. Образ уже записан на накопитель.";
            LogDebug(outErrorMessage);
            return WriteResult::Cancelled;
        }
        if (totalVerified != isoBytes) {
            outErrorMessage = L"Проверка прочитала меньше данных, чем размер образа.";
            LogDebug(outErrorMessage);
            return WriteResult::VerificationError;
        }
    }

    if (params.bootMode != BootWriteMode::AsImage) {
        if (progressCallback) progressCallback(100, L"Режим загрузки...");
        if (!PatchBootSector(st.hDisk, params.bootMode, sectorSize, useScsi, outErrorMessage)) {
            LogDebug(outErrorMessage);
            return WriteResult::WriteError;
        }
    }

    LogDebug(L"BurnImage completed successfully!");
    return WriteResult::Success;
}
