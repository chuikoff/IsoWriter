#pragma once
#include <windows.h>
#include <string>
#include <cstdint>

enum class IsoBootType {
    Unknown,
    StandardIso,
    LegacyMbr,
    UefiGpt,
    HybridUefiLegacy
};

struct IsoFileInfo {
    std::wstring filePath;
    std::wstring fileName;
    uint64_t fileSize = 0;
    std::wstring volumeLabel;
    IsoBootType bootType = IsoBootType::Unknown;
    std::wstring bootDescription;
    bool hasMbr = false;
    bool hasGpt = false;
    bool hasEfiPartition = false;
    bool hasLegacyPartition = false;
    bool hasIso9660 = false;
    bool hasElTorito = false;
};

class IsoDetector {
public:
    static bool Analyze(const std::wstring& filePath, IsoFileInfo& outInfo);
    static bool SelfCheck();
};
