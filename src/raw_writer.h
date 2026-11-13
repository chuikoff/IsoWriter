#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <cstdint>
#include "drive_enum.h"
#include "settings.h"

enum class WriteResult {
    Success,
    Cancelled,
    ElevationRequired,
    DeviceOpenFailed,
    SourceOpenFailed,
    LockVolumeFailed,
    WriteError,
    VerificationError,
    DiskTooSmall
};

struct WriteParams {
    std::wstring sourceIsoPath;
    UsbDriveInfo targetDrive;
    bool verifyAfterWrite = true;
    BootWriteMode bootMode = BootWriteMode::AsImage;
    std::wstring volumeLabel;
};

// Callback: returns false if user requested cancellation
using ProgressFn = std::function<bool(int percentDone, const std::wstring& statusText)>;

class RawWriter {
public:
    static std::wstring LogFilePath();
    static void Log(const std::wstring& msg);
    static WriteResult BurnImage(const WriteParams& params, ProgressFn progressCallback, std::wstring& outErrorMessage);
};
