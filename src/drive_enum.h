#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

struct UsbDriveInfo {
    int physicalDriveIndex = -1;       // e.g. 2 for \\.\PhysicalDrive2
    std::wstring devicePath;           // L"\\\\.\\PhysicalDrive2"
    std::wstring vendor;               // e.g. L"Kingston"
    std::wstring product;              // e.g. L"DataTraveler 3.0"
    std::wstring revision;             // e.g. L"PMAP"
    std::wstring driveLetters;         // e.g. L"E:" or L"E:, F:"
    uint64_t totalSizeBytes = 0;       // Total disk size in bytes
    DWORD bytesPerSector = 512;        // Sector size
    bool isRemovable = false;
    std::wstring tcFolderName;         // Folder name displayed in Total Commander
};

class DriveEnumerator {
public:
    static std::vector<UsbDriveInfo> EnumerateUsbDrives();
    static int GetSystemDriveIndex();
    static std::wstring FormatSize(uint64_t bytes);
    static bool IsElevated();
};
