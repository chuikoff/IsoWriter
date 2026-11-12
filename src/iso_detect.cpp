#include "iso_detect.h"
#include <vector>
#include <cstring>
#include <algorithm>

namespace {

std::wstring TrimRight(const std::string& str) {
    size_t end = str.find_last_not_of(" \t\r\n\0");
    if (end == std::string::npos) return L"";
    std::string trimmed = str.substr(0, end + 1);

    int len = MultiByteToWideChar(CP_ACP, 0, trimmed.c_str(), -1, NULL, 0);
    if (len <= 0) return L"";
    std::wstring result(len - 1, 0);
    MultiByteToWideChar(CP_ACP, 0, trimmed.c_str(), -1, &result[0], len);
    return result;
}

struct BootFlags {
    bool mbr = false;
    bool gpt = false;
    bool efi = false;
    bool iso = false;
    bool elTorito = false;
    bool bootable = false;
};

IsoBootType ClassifyBoot(BootFlags f) {
    bool uefi = f.efi || f.gpt;
    bool legacy = f.elTorito || f.bootable;
    if (uefi && legacy) return IsoBootType::HybridUefiLegacy;
    if (uefi) return IsoBootType::UefiGpt;
    if (legacy) return IsoBootType::LegacyMbr;
    if (f.iso) return IsoBootType::StandardIso;
    return IsoBootType::Unknown;
}

} // namespace

bool IsoDetector::SelfCheck() {
    const struct {
        BootFlags flags;
        IsoBootType type;
    } cases[] = {
        {{true, true, false, false, false, false}, IsoBootType::UefiGpt},
        {{true, false, false, true, true, false}, IsoBootType::LegacyMbr},
        {{true, false, true, true, true, true}, IsoBootType::HybridUefiLegacy},
        {{false, false, false, true, false, false}, IsoBootType::StandardIso},
        {{}, IsoBootType::Unknown},
        {{true, true, true, true, true, false}, IsoBootType::HybridUefiLegacy},
        {{true, false, false, false, false, true}, IsoBootType::LegacyMbr},
        {{true, false, false, false, false, false}, IsoBootType::Unknown},
    };
    for (const auto& c : cases) {
        if (ClassifyBoot(c.flags) != c.type) return false;
    }
    return true;
}

bool IsoDetector::Analyze(const std::wstring& filePath, IsoFileInfo& outInfo) {
    outInfo = IsoFileInfo();
    if (!SelfCheck()) {
        outInfo.bootDescription = L"Ошибка классификации образа";
        return false;
    }
    outInfo.filePath = filePath;

    size_t lastSlash = filePath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        outInfo.fileName = filePath.substr(lastSlash + 1);
    } else {
        outInfo.fileName = filePath;
    }

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER liSize;
    if (GetFileSizeEx(hFile, &liSize)) {
        outInfo.fileSize = (uint64_t)liSize.QuadPart;
    }

    // Read initial 64 KB of the image
    const DWORD HEADER_BUFFER_SIZE = 65536;
    std::vector<BYTE> buffer(HEADER_BUFFER_SIZE, 0);
    DWORD bytesRead = 0;
    ReadFile(hFile, buffer.data(), HEADER_BUFFER_SIZE, &bytesRead, NULL);
    CloseHandle(hFile);

    if (bytesRead < 512) {
        return false;
    }

    // 1. Check MBR (Sector 0: 512 bytes)
    bool hasBootableMbr = false;
    if (buffer[510] == 0x55 && buffer[511] == 0xAA) {
        outInfo.hasMbr = true;

        // Inspect 4 partition entries (starting at 446)
        for (int p = 0; p < 4; ++p) {
            size_t entryOffset = 446 + p * 16;
            BYTE type = buffer[entryOffset + 4];
            BYTE status = buffer[entryOffset];

            if (type == 0xEF) {
                outInfo.hasEfiPartition = true;
            } else if (type != 0x00 && type != 0xEE) {
                outInfo.hasLegacyPartition = true;
            }
            if (type == 0xEE) {
                outInfo.hasGpt = true; // Protective MBR for GPT
            }
            if (status == 0x80 && type != 0x00 && type != 0xEE) {
                hasBootableMbr = true;
            }
        }
    }

    // 2. Check GPT Header at Sector 1 (offset 512)
    if (bytesRead >= 1024) {
        if (memcmp(buffer.data() + 512, "EFI PART", 8) == 0) {
            outInfo.hasGpt = true;
        }
    }

    // 3. ISO 9660 descriptors start at sector 16 (2048-byte sectors). El Torito
    //    may sit after Joliet, so walk the descriptor chain instead of sector 17 only.
    if (bytesRead >= 32768 + 2048) {
        for (DWORD off = 32768; off + 2048 <= bytesRead; off += 2048) {
            const BYTE* vd = buffer.data() + off;
            if (vd[0] == 0xFF) break;
            if (off == 32768 && vd[0] == 0x01 && memcmp(vd + 1, "CD001", 5) == 0) {
                outInfo.hasIso9660 = true;
                std::string volId((const char*)(vd + 40), 32);
                outInfo.volumeLabel = TrimRight(volId);
            }
            if (vd[0] == 0x00 && memcmp(vd + 1, "CD001", 5) == 0 &&
                memcmp(vd + 7, "EL TORITO SPECIFICATION", 23) == 0) {
                outInfo.hasElTorito = true;
            }
        }
    }

    BootFlags flags;
    flags.mbr = outInfo.hasMbr;
    flags.gpt = outInfo.hasGpt;
    flags.efi = outInfo.hasEfiPartition;
    flags.iso = outInfo.hasIso9660;
    flags.elTorito = outInfo.hasElTorito;
    flags.bootable = hasBootableMbr;
    outInfo.bootType = ClassifyBoot(flags);
    switch (outInfo.bootType) {
    case IsoBootType::HybridUefiLegacy:
        outInfo.bootDescription = L"Hybrid ISO: UEFI + Legacy BIOS (MBR)";
        break;
    case IsoBootType::UefiGpt:
        outInfo.bootDescription = L"UEFI Bootable (GPT / EFI)";
        break;
    case IsoBootType::LegacyMbr:
        outInfo.bootDescription = L"Legacy BIOS Bootable (MBR / El Torito)";
        break;
    case IsoBootType::StandardIso:
        outInfo.bootDescription = L"Standard ISO 9660 Image";
        break;
    default:
        outInfo.bootDescription = L"Raw Disk / Binary Image";
        break;
    }
    if (!outInfo.hasMbr && !outInfo.hasGpt && outInfo.hasIso9660) {
        outInfo.bootDescription = L"ISO 9660 без таблицы разделов";
    }

    return true;
}
