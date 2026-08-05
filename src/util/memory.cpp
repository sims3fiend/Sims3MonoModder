#include "memory.h"
#include "logger.h"
#include <cctype>
#include <vector>

namespace S3MM {
namespace Memory {

// PATTERN SCANNER - Nibble-level wildcards for flexible matching

uintptr_t ScanPattern(BYTE* start, size_t size, const char* pattern) {
    std::vector<BYTE> bytes;
    std::vector<BYTE> masks; // 0xFF = exact, 0xF0 = high nibble, 0x0F = low nibble, 0x00 = wildcard

    // Helper to parse a single hex nibble
    auto parseNibble = [](char c) -> BYTE {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return 0;
    };

    const char* p = pattern;
    while (*p) {
        // Skip whitespace
        while (*p == ' ') p++;
        if (!*p) break;

        char hiChar = *p;
        char loChar = *(p + 1);

        // Need two characters for a byte
        if (!loChar || loChar == ' ') {
            // Invalid pattern format (incomplete byte)
            return 0;
        }

        bool hiWild = (hiChar == '?');
        bool loWild = (loChar == '?');
        bool hiHex = isxdigit(static_cast<unsigned char>(hiChar)) != 0;
        bool loHex = isxdigit(static_cast<unsigned char>(loChar)) != 0;

        // Each nibble must be hex or wildcard
        if ((!hiWild && !hiHex) || (!loWild && !loHex)) {
            // Invalid pattern format
            return 0;
        }

        // Build the byte and mask
        BYTE byteVal = 0;
        BYTE maskVal = 0;

        if (hiHex) {
            byteVal |= (parseNibble(hiChar) << 4);
            maskVal |= 0xF0;
        }
        if (loHex) {
            byteVal |= parseNibble(loChar);
            maskVal |= 0x0F;
        }

        bytes.push_back(byteVal);
        masks.push_back(maskVal);
        p += 2;
    }

    if (bytes.empty()) { return 0; }

    // Perform the scan with mask comparison
    for (size_t i = 0; i <= size - bytes.size(); i++) {
        bool found = true;
        for (size_t j = 0; j < bytes.size(); j++) {
            if ((start[i + j] & masks[j]) != (bytes[j] & masks[j])) {
                found = false;
                break;
            }
        }
        if (found) { return reinterpret_cast<uintptr_t>(start + i); }
    }

    return 0;
}

uintptr_t ScanPattern(const char* pattern) {
    ModuleInfo mod = GetMainModule();
    return ScanPattern(reinterpret_cast<BYTE*>(mod.base), mod.size, pattern);
}

// CO-RESIDENT MODULE SNAPSHOT

// SELF-ANCHOR - any address inside this module works with GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
static int s_selfAnchor = 0;

struct ModuleRange {
    uintptr_t base = 0;
    uintptr_t end = 0;
    std::string name;
    std::string path;
    bool isMain = false;
    bool isSelf = false;
};

static std::vector<ModuleRange> s_modules;

void SnapshotModules() {
    s_modules.clear();

    HMODULE mainH = GetModuleHandleA(nullptr);

    HMODULE selfH = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&s_selfAnchor), &selfH);

    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        LOG_WARN("[Modules] EnumProcessModules failed - crash-log module attribution unavailable");
        return;
    }

    size_t count = needed / sizeof(HMODULE);
    if (count > 512) count = 512;

    for (size_t i = 0; i < count; i++) {
        MODULEINFO mi = {};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;

        char nameBuf[MAX_PATH] = {};
        GetModuleBaseNameA(GetCurrentProcess(), mods[i], nameBuf, sizeof(nameBuf));
        char pathBuf[MAX_PATH] = {};
        GetModuleFileNameExA(GetCurrentProcess(), mods[i], pathBuf, sizeof(pathBuf));

        ModuleRange r;
        r.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        r.end = r.base + mi.SizeOfImage;
        r.name = nameBuf[0] ? nameBuf : "<unknown>";
        r.path = pathBuf;
        r.isMain = (mods[i] == mainH);
        r.isSelf = (selfH != nullptr && mods[i] == selfH);
        s_modules.push_back(r);
    }
}

std::string ResolveModuleForAddress(uintptr_t addr) {
    for (const auto& r : s_modules) {
        if (addr >= r.base && addr < r.end) { return (r.isMain || r.isSelf) ? "" : r.name; }
    }
    return "";
}

// Game-directory filter: system DLLs live under Windows, so anything loaded from the exe's directory is another native mod binary (ASI, proxy DLL, second patcher)
void LogCoResidentModules() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    size_t slash = dir.find_last_of("\\/");
    dir = (slash == std::string::npos) ? "" : dir.substr(0, slash + 1);

    int found = 0;
    for (const auto& r : s_modules) {
        if (r.isMain || r.isSelf) continue;
        if (dir.empty() || _strnicmp(r.path.c_str(), dir.c_str(), dir.size()) != 0) continue;
        LOG_INFO("[Modules] Co-resident native module: " + r.name + " @ " + ToHex(r.base));
        found++;
    }
    if (found == 0) { LOG_INFO("[Modules] No co-resident native modules in the game directory"); }
}

} // namespace Memory
} // namespace S3MM
