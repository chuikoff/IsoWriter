#include "drive_enum.h"
#include <winioctl.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace {

std::wstring TrimString(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return L"";
    size_t end = str.find_last_not_of(" \t\r\n");
    std::string trimmed = str.substr(start, end - start + 1);

    int len = MultiByteToWideChar(CP_ACP, 0, trimmed.c_str(), -1, NULL, 0);
    if (len <= 0) return L"";
    std::wstring result(len - 1, 0);
    MultiByteToWideChar(CP_ACP, 0, trimmed.c_str(), -1, &result[0], len);
    return result;
}

// Clean folder name by removing illegal filename characters (\ / : * ? " < > |)
std::wstring SanitizeFolderName(const std::wstring& name) {
    std::wstring clean = name;
    const wchar_t illegal[] = L"\\/:*?\"<>|";
    for (wchar_t& c : clean) {
        for (int i = 0; illegal[i]; ++i) {
            if (c == illegal[i]) {
                c = L'_';
                break;
            }
        }
    }
    return clean;
}

} // namespace

int DriveEnumerator::GetSystemDriveIndex() {
    wchar_t sysDir[MAX_PATH] = {0};
    if (GetSystemDirectoryW(sysDir, MAX_PATH) == 0) {
        return -1;
    }

    wchar_t volumePath[] = L"\\\\.\\C:";
    volumePath[4] = sysDir[0]; // Drive letter of system dir

    HANDLE hVol = CreateFileW(volumePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (hVol == INVALID_HANDLE_VALUE) {
        return -1;
    }

    STORAGE_DEVICE_NUMBER sdn = {0};
    DWORD bytesReturned = 0;
    int sysDriveIndex = -1;
    if (DeviceIoControl(hVol, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                        NULL, 0, &sdn, sizeof(sdn), &bytesReturned, NULL)) {
        sysDriveIndex = (int)sdn.DeviceNumber;
    }
    CloseHandle(hVol);
    return sysDriveIndex;
}

std::wstring DriveEnumerator::FormatSize(uint64_t bytes) {
    const double KB = 1024.0;
    const double MB = KB * 1024.0;
    const double GB = MB * 1024.0;
    const double TB = GB * 1024.0;

    std::wstringstream ss;
    ss << std::fixed << std::setprecision(1);

    if (bytes >= TB) {
        ss << (bytes / TB) << L" TB";
    } else if (bytes >= GB) {
        ss << (bytes / GB) << L" GB";
    } else if (bytes >= MB) {
        ss << (bytes / MB) << L" MB";
    } else if (bytes >= KB) {
        ss << (bytes / KB) << L" KB";
    } else {
        ss << bytes << L" B";
    }
    return ss.str();
}

bool DriveEnumerator::IsElevated() {
    bool elevated = false;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation = {0};
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &cbSize)) {
            elevated = (elevation.TokenIsElevated != 0);
        }
        CloseHandle(hToken);
    }
    return elevated;
}

std::vector<UsbDriveInfo> DriveEnumerator::EnumerateUsbDrives() {
    std::vector<UsbDriveInfo> drives;
    int systemDriveIdx = GetSystemDriveIndex();

    // Map drive letters to PhysicalDrive indices
    std::vector<std::pair<int, wchar_t>> letterToPhysicalDrive;
    DWORD logicalDrives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (logicalDrives & (1 << i)) {
            wchar_t driveLetter = L'A' + i;
            wchar_t volPath[] = L"\\\\.\\A:";
            volPath[4] = driveLetter;

            HANDLE hVol = CreateFileW(volPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      NULL, OPEN_EXISTING, 0, NULL);
            if (hVol != INVALID_HANDLE_VALUE) {
                STORAGE_DEVICE_NUMBER sdn = {0};
                DWORD bytesRet = 0;
                if (DeviceIoControl(hVol, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                    NULL, 0, &sdn, sizeof(sdn), &bytesRet, NULL)) {
                    if (sdn.DeviceType == FILE_DEVICE_DISK) {
                        letterToPhysicalDrive.push_back({(int)sdn.DeviceNumber, driveLetter});
                    }
                }
                CloseHandle(hVol);
            }
        }
    }

    // Inspect physical drives 0..32
    for (int idx = 0; idx < 32; ++idx) {
        if (systemDriveIdx >= 0 && idx == systemDriveIdx) {
            continue;
        }

        std::wstring devPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(idx);
        HANDLE hDisk = CreateFileW(devPath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
        if (hDisk == INVALID_HANDLE_VALUE) {
            continue;
        }

        STORAGE_PROPERTY_QUERY query = {};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;

        BYTE buffer[1024] = {0};
        DWORD bytesReturned = 0;

        if (DeviceIoControl(hDisk, IOCTL_STORAGE_QUERY_PROPERTY,
                            &query, sizeof(query),
                            buffer, sizeof(buffer),
                            &bytesReturned, NULL)) {
            STORAGE_DEVICE_DESCRIPTOR* pDesc = (STORAGE_DEVICE_DESCRIPTOR*)buffer;

            // shortcut: USB HDD stays listed; many flash drives report BusTypeUsb with RemovableMedia=0, so that bit cannot exclude them
            bool isTargetBus = (pDesc->BusType == BusTypeUsb ||
                                pDesc->BusType == BusTypeSd ||
                                pDesc->BusType == BusTypeMmc);

            if (isTargetBus) {
                UsbDriveInfo info;
                info.physicalDriveIndex = idx;
                info.devicePath = devPath;
                info.isRemovable = (pDesc->RemovableMedia != FALSE);

                if (pDesc->VendorIdOffset != 0 && pDesc->VendorIdOffset < bytesReturned) {
                    info.vendor = TrimString((char*)(buffer + pDesc->VendorIdOffset));
                }
                if (pDesc->ProductIdOffset != 0 && pDesc->ProductIdOffset < bytesReturned) {
                    info.product = TrimString((char*)(buffer + pDesc->ProductIdOffset));
                }
                if (pDesc->ProductRevisionOffset != 0 && pDesc->ProductRevisionOffset < bytesReturned) {
                    info.revision = TrimString((char*)(buffer + pDesc->ProductRevisionOffset));
                }

                // Get capacity and sector size
                DISK_GEOMETRY_EX geomEx = {0};
                if (DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                                    NULL, 0, &geomEx, sizeof(geomEx), &bytesReturned, NULL)) {
                    info.totalSizeBytes = (uint64_t)geomEx.DiskSize.QuadPart;
                    info.bytesPerSector = geomEx.Geometry.BytesPerSector;
                } else {
                    DISK_GEOMETRY geom = {0};
                    if (DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                        NULL, 0, &geom, sizeof(geom), &bytesReturned, NULL)) {
                        info.totalSizeBytes = (uint64_t)geom.Cylinders.QuadPart *
                                              geom.TracksPerCylinder *
                                              geom.SectorsPerTrack *
                                              geom.BytesPerSector;
                        info.bytesPerSector = geom.BytesPerSector;
                    }
                }

                if (info.bytesPerSector == 0) {
                    info.bytesPerSector = 512;
                }

                // Gather associated drive letters
                std::wstring letters;
                for (const auto& pair : letterToPhysicalDrive) {
                    if (pair.first == idx) {
                        if (!letters.empty()) letters += L", ";
                        letters += pair.second;
                        letters += L":";
                    }
                }
                info.driveLetters = letters;

                // Build a nice display folder name for Total Commander
                // e.g.: [USB 2] Kingston DataTraveler 3.0 (29.8 GB) [E]
                std::wstring title;
                if (!info.vendor.empty() && !info.product.empty()) {
                    title = info.vendor + L" " + info.product;
                } else if (!info.product.empty()) {
                    title = info.product;
                } else {
                    title = L"USB Drive " + std::to_wstring(idx);
                }

                std::wstring folderName = L"[USB " + std::to_wstring(idx) + L"] " + title;
                if (info.totalSizeBytes > 0) {
                    folderName += L" (" + FormatSize(info.totalSizeBytes) + L")";
                }
                if (!info.driveLetters.empty()) {
                    folderName += L" [" + info.driveLetters + L"]";
                }

                // Remove colons or illegal chars for TC folder name
                std::wstring safeLetters = info.driveLetters;
                std::replace(safeLetters.begin(), safeLetters.end(), L':', L' ');
                
                info.tcFolderName = SanitizeFolderName(L"[USB " + std::to_wstring(idx) + L"] " + title + 
                    (info.totalSizeBytes > 0 ? (L" (" + FormatSize(info.totalSizeBytes) + L")") : L"") +
                    (!safeLetters.empty() ? (L" [" + safeLetters + L"]") : L""));

                drives.push_back(info);
            }
        }

        CloseHandle(hDisk);
    }

    return drives;
}
