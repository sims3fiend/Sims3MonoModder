#include "assembly_hooks.h"
#include "mono_bridge.h"
#include "resource_system.h"
#include "../modding/package_reader.h"
#include "addresses.h"
#include "../modding/conflict_detector.h"
#include "vanilla_extractor.h"
#include "../util/logger.h"
#include "../util/config_paths.h"
#include <Windows.h>
#include <detours/detours.h>
#include <map>
#include <mutex>

namespace S3MM {

// THREAD-LOCAL STATE - Per-thread context for nested/recursive load tracking
thread_local bool t_insideFileLoad = false;         // true while inside a file-based load
thread_local void* t_currentLoadingImage = nullptr; // MonoImage* currently being processed

// LOAD PATH TRACKING - Maps images to their load paths and source packages
static std::map<void*, std::string> s_imageLoadPaths;
static std::mutex s_imageLoadPathsMutex;
static std::atomic<int> s_imageLoadCount(0);                        // total image opens (file + memory), for quiescence detection
static std::map<std::string, std::string> s_assemblyPackageSources; // assembly name -> source package path
static std::mutex s_assemblyPackageSourcesMutex;
thread_local std::string t_currentWinningPackage; // set by LoadImage hook, read by mono hooks

// ASSEMBLY NAME COMPARISON HOOK - Detects same-version conflicts during assembly resolution
using assembly_name_compare_t = bool(__cdecl*)(void* asmName1, void* asmName2);
static assembly_name_compare_t s_originalAssemblyNameCompare = nullptr;

// Extract version from AssemblyName structure
static void ExtractAssemblyVersion(void* asmName, uint16_t& major, uint16_t& minor, uint16_t& build, uint16_t& revision) {
    major = minor = build = revision = 0;
    if (!asmName || !IsValidPointer(asmName)) return;
    uintptr_t base = (uintptr_t)asmName;
    major = *(uint16_t*)(base + 0x30);
    minor = *(uint16_t*)(base + 0x32);
    build = *(uint16_t*)(base + 0x34);
    revision = *(uint16_t*)(base + 0x36);
}

static const char* ExtractAssemblyName(void* asmName) {
    if (!asmName || !IsValidPointer(asmName)) return nullptr;
    const char* name = *(const char**)asmName;
    if (!name || !IsValidPointer((void*)name)) return nullptr;
    return name;
}

static bool __cdecl DetourAssemblyNameCompare(void* asmName1, void* asmName2) {
    const char* name1 = ExtractAssemblyName(asmName1);

    uint16_t maj1, min1, bld1, rev1;
    uint16_t maj2, min2, bld2, rev2;
    ExtractAssemblyVersion(asmName1, maj1, min1, bld1, rev1);
    ExtractAssemblyVersion(asmName2, maj2, min2, bld2, rev2);

    bool result = s_originalAssemblyNameCompare(asmName1, asmName2);

    // If assemblies match by name with identical versions, likely a mod replacement
    if (result && name1 && IsCoreAssembly(name1)) {
        if (maj1 == maj2 && min1 == min2 && bld1 == bld2 && rev1 == rev2) {
            char verStr[64];
            snprintf(verStr, sizeof(verStr), "%d.%d.%d.%d", maj1, min1, bld1, rev1);

            // t_currentWinningPackage is often empty here because comparison fires during the second/vanilla load, not inside LoadImage.
            // Fall back to the package source tracked during the first/winning load.
            std::string pkg = t_currentWinningPackage;
            if (pkg.empty()) {
                std::lock_guard<std::mutex> lock(s_assemblyPackageSourcesMutex);
                auto it = s_assemblyPackageSources.find(name1);
                if (it != s_assemblyPackageSources.end()) { pkg = it->second; }
            }
            RecordSameVersionConflict(name1, verStr, !t_insideFileLoad, 0, pkg);
        }
    }

    return result;
}

// ASSEMBLY LOAD FROM FULL HOOK - Tracks which image is currently being loaded
using assembly_load_from_full_t = void*(__cdecl*)(void* image, const char* path, int* status, int refonly);
static assembly_load_from_full_t s_originalAssemblyLoadFromFull = nullptr;

static void* __cdecl DetourAssemblyLoadFromFull(void* newImage, const char* path, int* status, int refonly) {
    t_currentLoadingImage = newImage;
    void* result = s_originalAssemblyLoadFromFull(newImage, path, status, refonly);
    t_currentLoadingImage = nullptr;
    return result;
}

// MONO IMAGE OPEN FULL HOOK - Intercepts file-based assembly loads
using mono_image_open_full_t = void*(__cdecl*)(const char* fname, int* status, int refonly);
static mono_image_open_full_t s_originalMonoImageOpenFull = nullptr;

static void* __cdecl DetourMonoImageOpenFull(const char* fname, int* status, int refonly) {
    std::string path = fname ? fname : "<null>";
    LOG_INFO("[AssemblyLoad] Loading: " + path);

    t_insideFileLoad = true;
    void* result = s_originalMonoImageOpenFull(fname, status, refonly);
    t_insideFileLoad = false;

    if (result) s_imageLoadCount.fetch_add(1, std::memory_order_relaxed);

    if (result && fname) {
        std::lock_guard<std::mutex> lock(s_imageLoadPathsMutex);
        s_imageLoadPaths[result] = path;
    }

    return result;
}

// MONO IMAGE OPEN FROM DATA FULL HOOK - Intercepts memory-based assembly loads
using mono_image_open_from_data_full_t = void*(__cdecl*)(void* data, uint32_t data_len, int need_copy, int* status, int refonly);
static mono_image_open_from_data_full_t s_originalMonoImageOpenFromDataFull = nullptr;

static void* __cdecl DetourMonoImageOpenFromDataFull(void* data, uint32_t data_len, int need_copy, int* status, int refonly) {
    void* result = s_originalMonoImageOpenFromDataFull(data, data_len, need_copy, status, refonly);

    if (result) {
        s_imageLoadCount.fetch_add(1, std::memory_order_relaxed);
        const char* name = MonoBridge::GetImageName(result);
        std::string nameStr = name ? name : "<unknown>";

        if (IsCoreAssembly(nameStr)) {
            bool fromPackage = !t_insideFileLoad;
            RecordCoreAssemblySource(nameStr, fromPackage);

            if (!fromPackage) {
                LOG_INFO("[AssemblyLoad] Core assembly '" + nameStr + "' loaded from file (vanilla)");
            } else {
                LOG_INFO("[AssemblyLoad] Core assembly '" + nameStr + "' loaded from .package (potential mod)");

                // If inside a ScriptHost::LoadImage call, we know which package
                if (!t_currentWinningPackage.empty()) {
                    std::lock_guard<std::mutex> lock(s_assemblyPackageSourcesMutex);
                    s_assemblyPackageSources[nameStr] = t_currentWinningPackage;
                    LOG_INFO("[AssemblyLoad] Tracked core: " + nameStr + " <- " + t_currentWinningPackage);
                } else {
                    LOG_WARN("[AssemblyLoad] Core assembly '" + nameStr + "' loaded outside LoadImage - cannot determine source package");
                }
            }
        }
    }

    return result;
}

// MONO IMAGE OPEN FROM DATA FULL WRAPPER HOOK - Hooks 00e84860 wrapper around mono_image_open_from_data_full

using ImageOpenWrapper_t = void*(__cdecl*)(void* data, uint32_t dataLen, int needCopy, int* status);
static ImageOpenWrapper_t s_originalImageOpenWrapper = nullptr;

static void* __cdecl DetourImageOpenWrapper(void* data, uint32_t dataLen, int needCopy, int* status) {
    LOG_INFO("[ImageOpenWrapper] Called! data=" + ToHex((uintptr_t)data) + ", len=" + std::to_string(dataLen) + ", t_currentWinningPackage=" + (t_currentWinningPackage.empty() ? "<empty>" : t_currentWinningPackage));

    void* result = s_originalImageOpenWrapper(data, dataLen, needCopy, status);

    if (result) {
        const char* name = *(const char**)((uintptr_t)result + 0x18);
        if (name && IsValidPointer((void*)name)) { LOG_INFO("[ImageOpenWrapper] Result image: " + std::string(name)); }
    }

    return result;
}

// APPDOMAIN LOADASSEMBLYRAW ICALL HOOK - Catches managed Assembly.Load(byte[])
// cdecl btw, thank you Ghidra for causing me immense pain!
using LoadAssemblyRaw_t = uint32_t(__cdecl*)(void* ad, void* rawAssembly, void* rawSymbolStore, void* evidence, uint8_t refonly);
static LoadAssemblyRaw_t s_originalLoadAssemblyRaw = nullptr;

static uint32_t __cdecl DetourLoadAssemblyRaw(void* ad, void* rawAssembly, void* rawSymbolStore, void* evidence, uint8_t refonly) {
    // rawAssembly is a MonoArray*, data at +0x10 (ARRAY_DATA), length at +0x08 (ARRAY_LENGTH)
    uint32_t dataSize = 0;
    if (rawAssembly && IsValidPointer(rawAssembly)) { dataSize = *(uint32_t*)((uintptr_t)rawAssembly + 0x08); }

    LOG_INFO("[LoadAssemblyRaw] ICALL fired! rawAssembly=" + ToHex((uintptr_t)rawAssembly) + ", size=" + std::to_string(dataSize) + " bytes");

    uint32_t result = s_originalLoadAssemblyRaw(ad, rawAssembly, rawSymbolStore, evidence, refonly);

    // Try to get the assembly name from the result
    if (result != 0) {
        // MonoReflectionAssembly, assembly at +0x08
        void* assembly = *(void**)((uintptr_t)result + 0x08);
        if (assembly && IsValidPointer(assembly)) {
            // MonoAssembly has image at +0x00
            void* image = *(void**)assembly;
            if (image && IsValidPointer(image)) {
                const char* name = *(const char**)((uintptr_t)image + 0x18);
                if (name && IsValidPointer((void*)name)) { LOG_INFO("[LoadAssemblyRaw] Loaded assembly: " + std::string(name)); }
            }
        }
    }

    return result;
}

// SCRIPTHOST LOADIMAGE HOOK - Enumerates DBArray at game level to track package sources

// ResourceManager offsets
constexpr uint32_t RESMAN_DBARRAY_START = 0x30;
constexpr uint32_t RESMAN_DBARRAY_END = 0x34;

// Package vtable offsets
constexpr uint32_t PKG_VTABLE_GETPATH = 0x28;     // vtable[0x0A] -> wchar_t*
constexpr uint32_t PKG_VTABLE_HASRESOURCE = 0x34; // vtable[0x0D] -> bool

// Current resource key being loaded, for correlation after load.
// Uses ResourceKey from resource_system.h, confirmed layout: {instance_lo, instance_hi, type, group}.
// Previously SPEC_ResourceKey had wrong layout but was only passed as void*, so no existing bugs.
thread_local ResourceKey t_currentResourceKey = {0, 0, 0, 0};

// Function pointer types for resource manager interaction
using GetResourceManager_t = void*(__cdecl*)();
using ScriptHostLoadImage_t = void*(__thiscall*)(void* thisPtr, void* resourceKey);
using PackageHasResource_t = bool(__thiscall*)(void* pkg, void* key, int, int, int, int, int);
using PackageGetPath_t = const wchar_t*(__thiscall*)(void* pkg);

static GetResourceManager_t s_getResourceManager = nullptr;
static ScriptHostLoadImage_t s_originalScriptHostLoadImage = nullptr;

// Safely call package GetPath (SEH-wrapped, no C++ objects)
static const wchar_t* SafeGetPackagePath(PackageGetPath_t getPath, void* pkg) {
    __try {
        return getPath(pkg);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Safely call HasResource (SEH-wrapped, no C++ objects)
static bool SafeHasResource(PackageHasResource_t hasResource, void* pkg, void* resourceKey) {
    __try {
        return hasResource(pkg, resourceKey, 0, 1, 6, 1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Walk DBArray to find the highest-priority package that has this resource
static std::string FindWinningPackage(void* resourceKey) {
    if (!s_getResourceManager) { s_getResourceManager = (GetResourceManager_t)Addresses::ScriptHost.GetResourceManager; }

    void* manager = s_getResourceManager();
    if (!manager || !IsValidPointer(manager)) { return ""; }

    // DBArray at manager+0x30..+0x34, each entry is 8 bytes: {PackageSource* pkg, int priority}
    uintptr_t* dbStart = *(uintptr_t**)((uintptr_t)manager + RESMAN_DBARRAY_START);
    uintptr_t* dbEnd = *(uintptr_t**)((uintptr_t)manager + RESMAN_DBARRAY_END);

    if (!dbStart || !dbEnd || !IsValidPointer(dbStart) || !IsValidPointer(dbEnd)) { return ""; }

    // Sorted by priority descending, first match wins
    for (uintptr_t* entry = dbStart; entry < dbEnd; entry += 2) {
        void* pkg = (void*)*entry;
        // int priority = (int)*(entry + 1); // not needed, first match wins

        if (!pkg || !IsValidPointer(pkg)) continue;

        // Get vtable for virtual calls
        void* vtable = *(void**)pkg;
        if (!vtable || !IsValidPointer(vtable)) continue;

        // Path via vtable[0x0A]
        void* getPathFn = *(void**)((uintptr_t)vtable + PKG_VTABLE_GETPATH);
        if (!getPathFn || !IsValidPointer(getPathFn)) continue;

        PackageGetPath_t getPath = (PackageGetPath_t)getPathFn;
        const wchar_t* pathW = SafeGetPackagePath(getPath, pkg);

        if (!pathW || !IsValidPointer((void*)pathW)) continue;

        std::string path = ConfigPaths::WideToUtf8(pathW);

        // Check if this package owns the resource
        void* hasResourceFn = *(void**)((uintptr_t)vtable + PKG_VTABLE_HASRESOURCE);
        if (!hasResourceFn || !IsValidPointer(hasResourceFn)) continue;

        PackageHasResource_t hasResource = (PackageHasResource_t)hasResourceFn;

        if (SafeHasResource(hasResource, pkg, resourceKey)) {
            // First match wins (highest priority)
            return path;
        }
    }

    return "";
}

// Safely read image name (SEH-wrapped, no C++ objects)
static const char* SafeGetImageName(void* image) {
    if (!image || !IsValidPointer(image)) return nullptr;
    __try {
        const char* name = *(const char**)((uintptr_t)image + 0x18);
        if (name && IsValidPointer((void*)name)) { return name; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Fall through
    }
    return nullptr;
}

static void* __fastcall DetourScriptHostLoadImage(void* thisPtr, void* edx, void* resourceKey) {
    (void)edx; // thiscall via fastcall shim

    LOG_INFO("[LoadImage] DetourScriptHostLoadImage called, resourceKey=" + ToHex((uintptr_t)resourceKey));

    // Save previous value for recursive/nested LoadImage calls
    std::string previousWinningPackage = t_currentWinningPackage;

    // Find which package will provide this resource
    t_currentWinningPackage = "";
    if (resourceKey && IsValidPointer(resourceKey)) {
        t_currentWinningPackage = FindWinningPackage(resourceKey);

        if (!t_currentWinningPackage.empty()) {
            // Extract filename for logging
            size_t lastSlash = t_currentWinningPackage.find_last_of("\\/");
            std::string filename = (lastSlash != std::string::npos) ? t_currentWinningPackage.substr(lastSlash + 1) : t_currentWinningPackage;
            LOG_INFO("[LoadImage] Resource will load from: " + filename);
        }
    }

    // Forward to original
    void* result = s_originalScriptHostLoadImage(thisPtr, resourceKey);

    // Correlate assembly name with package source
    if (result && !t_currentWinningPackage.empty()) {
        const char* name = SafeGetImageName(result);

        if (name) {
            std::string assemblyName = name;

            // Only track non-vanilla packages (Mods folder, not GameData)
            bool isMod = (t_currentWinningPackage.find("\\Mods\\") != std::string::npos || t_currentWinningPackage.find("/Mods/") != std::string::npos);

            if (isMod) {
                std::lock_guard<std::mutex> lock(s_assemblyPackageSourcesMutex);
                s_assemblyPackageSources[assemblyName] = t_currentWinningPackage;

                LOG_INFO("[LoadImage] Tracked: " + assemblyName + " <- " + t_currentWinningPackage);
            } else {
                RecordVanillaPackage(t_currentWinningPackage);
            }
        }
    }

    // Restore previous value for nested calls
    t_currentWinningPackage = previousWinningPackage;
    return result;
}

// PUBLIC API - Hook installation and query functions
bool InstallImageLoadHooks() {
    LOG_INFO("[AssemblyHooks] Installing all assembly load hooks...");

    s_originalMonoImageOpenFull = (mono_image_open_full_t)Addresses::Assemblies.mono_image_open_full;
    s_originalMonoImageOpenFromDataFull = (mono_image_open_from_data_full_t)Addresses::Assemblies.mono_image_open_from_data_full;
    s_originalAssemblyLoadFromFull = (assembly_load_from_full_t)Addresses::Assemblies.mono_assembly_load_from_full;
    s_originalAssemblyNameCompare = (assembly_name_compare_t)Addresses::Assemblies.mono_assembly_names_equal;
    s_originalScriptHostLoadImage = (ScriptHostLoadImage_t)Addresses::ScriptHost.ScriptHost_LoadImage;
    s_originalLoadAssemblyRaw = (LoadAssemblyRaw_t)Addresses::Assemblies.SPEC_ves_icall_AppDomain_LoadAssemblyRaw;
    s_originalImageOpenWrapper = (ImageOpenWrapper_t)Addresses::Assemblies.mono_image_open_from_data;
    s_getResourceManager = (GetResourceManager_t)Addresses::ScriptHost.GetResourceManager;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_originalMonoImageOpenFull, DetourMonoImageOpenFull);
    DetourAttach(&(PVOID&)s_originalMonoImageOpenFromDataFull, DetourMonoImageOpenFromDataFull);
    DetourAttach(&(PVOID&)s_originalAssemblyLoadFromFull, DetourAssemblyLoadFromFull);
    DetourAttach(&(PVOID&)s_originalAssemblyNameCompare, DetourAssemblyNameCompare);
    DetourAttach(&(PVOID&)s_originalScriptHostLoadImage, DetourScriptHostLoadImage);
    DetourAttach(&(PVOID&)s_originalLoadAssemblyRaw, DetourLoadAssemblyRaw);
    DetourAttach(&(PVOID&)s_originalImageOpenWrapper, DetourImageOpenWrapper);
    LONG error = DetourTransactionCommit();

    if (error == NO_ERROR) {
        LOG_INFO("[AssemblyHooks] All hooks installed successfully (including ScriptHost::LoadImage @ " + ToHex(Addresses::ScriptHost.ScriptHost_LoadImage) + ")");
        return true;
    } else {
        LOG_ERROR("[AssemblyHooks] Failed to install hooks: " + std::to_string(error));
        return false;
    }
}

std::string GetImageLoadPath(void* image) {
    std::lock_guard<std::mutex> lock(s_imageLoadPathsMutex);
    auto it = s_imageLoadPaths.find(image);
    if (it != s_imageLoadPaths.end()) { return it->second; }
    return "";
}

std::map<std::string, std::string> GetAllAssemblyPackageSources() {
    std::lock_guard<std::mutex> lock(s_assemblyPackageSourcesMutex);
    return s_assemblyPackageSources; // Return copy
}

int GetImageLoadCount() {
    return s_imageLoadCount.load(std::memory_order_relaxed);
}

} // namespace S3MM
