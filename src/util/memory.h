#pragma once
#include <Windows.h>
#include <Psapi.h>
#include <cstdint>
#include <string>

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

// CO-RESIDENT MODULE SNAPSHOT - read-only [base,end) list of loaded modules, taken once at mono-ready
// Attribution only: feeds the VEH crash log's "fault address in <OtherMod.dll>" line and the startup listing of other native mods sharing the process
void SnapshotModules();

// Resolve a code address to the owning module's base name (e.g. "SomeMod.dll"), or "" if it is the main image, MM itself, or not in any snapshotted module
// Reads the snapshot only, so it is safe to call from a post-crash context
std::string ResolveModuleForAddress(uintptr_t addr);

// Log modules loaded from the game directory (ASIs, proxy DLLs, other native patchers) - the "these are also running" record for conflict triage
void LogCoResidentModules();

} // namespace Memory

} // namespace S3MM
