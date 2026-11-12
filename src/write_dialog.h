#pragma once
#include <windows.h>
#include "iso_detect.h"
#include "drive_enum.h"
#include "settings.h"

struct WriteDialogResult {
    bool confirmed = false;
    bool verify = true;
    BootWriteMode bootMode = BootWriteMode::AsImage;
};

class WriteDialog {
public:
    static bool Show(HWND hParent, HINSTANCE hInstance,
                     const IsoFileInfo& isoInfo,
                     const UsbDriveInfo& driveInfo,
                     WriteDialogResult& outResult);
};
