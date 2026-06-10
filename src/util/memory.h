#pragma once
#include <Windows.h>
#include <Psapi.h>
#include <cstdint>

namespace S3MM {

// MEMORY UTILITIES - Pattern scanning
namespace Memory {

// Pattern scanning with nibble-level wildcards. Pattern format: "55 8B EC ?? ?? 83".
// Supports: "FF" (exact), "??" (full wildcard), "?F" (low nibble), "F?" (high nibble), e.g. "0F 82 ?? ?? ?0 8B" matches 0F 82 <any> <any> <any ending in 0> 8B.
// Returns 0 if not found.
uintptr_t ScanPattern(BYTE* start, size_t size, const char* pattern);

// Convenience: scan main module
uintptr_t ScanPattern(const char* pattern);

// Get module info
struct ModuleInfo {
    uintptr_t base;
    size_t size;
};

inline ModuleInfo GetMainModule() {
    HMODULE hModule = GetModuleHandle(nullptr);
    MODULEINFO mi = {};
    GetModuleInformation(GetCurrentProcess(), hModule, &mi, sizeof(mi));
    return {(uintptr_t)mi.lpBaseOfDll, mi.SizeOfImage};
}

// Check if the host process is a known Sims 3 game executable (TS3.exe or TS3W.exe).
// Returns false if running inside a launcher or other non-game process.
inline bool IsGameProcess() {
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) { return false; }
    // Find the filename portion
    const char* fname = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') fname = p + 1;
    }
    return (_stricmp(fname, "TS3.exe") == 0 || _stricmp(fname, "TS3W.exe") == 0);
}

} // namespace Memory

} // namespace S3MM
