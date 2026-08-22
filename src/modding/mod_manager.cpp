#include "mod_manager.h"
#include "../core/vanilla_extractor.h"
#include "../util/logger.h"
#include "../resources/resource.h"
#include <Windows.h>
#include <fstream>

namespace S3MM {

ModManager& ModManager::Get() {
    static ModManager instance;
    return instance;
}

bool ModManager::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("[ModManager] ========================================");
    LOG_INFO("[ModManager] Initializing Mod System");
    LOG_INFO("[ModManager] ========================================");

    // Initialize compiler first (doesn't need Mono)
    if (!ModCompiler::Get().Initialize()) {
        m_status.errors.push_back("Failed to initialize mod compiler");
        LOG_ERROR("[ModManager] Compiler initialization failed");
        // Continue anyway - mods might be pre-compiled
    }

    // Extract vanilla libs from game packages before compilation
    // This reads gameplay.package, scripts.package, simcore.package directly
    ExtractVanillaLibrariesFromGameDir();

    // Extract embedded S3MM.Helper.dll to Libs/ so mods can reference it at compile time.
    // Re-extract only when missing or when the API version changed (tracked via a sidecar marker file).
    {
        std::filesystem::path libsPath = ModCompiler::Get().GetLibsPath();
        std::filesystem::path helperDst = libsPath / "S3MM.Helper.dll";
        std::filesystem::path versionMarker = libsPath / "S3MM.Helper.version";

        bool needsExtract = !std::filesystem::exists(helperDst);
        if (!needsExtract) {
            int diskVersion = -1;
            std::ifstream vf(versionMarker);
            if (vf) { vf >> diskVersion; }
            if (diskVersion != ModCompiler::API_VERSION) {
                LOG_INFO("[ModManager] Libs/S3MM.Helper.dll is API v" + std::to_string(diskVersion) + ", current is v" + std::to_string(ModCompiler::API_VERSION) + " - refreshing");
                needsExtract = true;
            }
        }

        if (needsExtract) {
            HMODULE hModule = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ModManager::Get, &hModule);
            if (hModule) {
                HRSRC hRes = FindResource(hModule, MAKEINTRESOURCE(IDR_S3MM_HELPER), RT_RCDATA);
                if (hRes) {
                    HGLOBAL hData = LoadResource(hModule, hRes);
                    DWORD resSize = SizeofResource(hModule, hRes);
                    const char* resPtr = hData ? (const char*)LockResource(hData) : nullptr;
                    if (resPtr && resSize > 0) {
                        std::ofstream out(helperDst, std::ios::binary);
                        if (out) {
                            out.write(resPtr, resSize);
                            out.close();
                            if (out.good()) {
                                // Stamp the API version we just wrote so future launches can detect drift.
                                std::ofstream vf(versionMarker, std::ios::trunc);
                                if (vf) { vf << ModCompiler::API_VERSION; }
                                LOG_INFO("[ModManager] Extracted S3MM.Helper.dll to Libs/ (API v" + std::to_string(ModCompiler::API_VERSION) + ")");
                            }
                        }
                    }
                }
            }
        }
    }

    // Discover mods and compile if needed
    m_discoveredMods = ModCompiler::Get().DiscoverMods();
    m_status.modsDiscovered = (int)m_discoveredMods.size();

    if (m_discoveredMods.empty()) {
        LOG_INFO("[ModManager] No mods found in " + ModCompiler::Get().GetModsPath().string());
    } else {
        // Compile mods that need it
        m_status.modsCompiled = ModCompiler::Get().CompileAllMods(m_discoveredMods);
    }

    m_initialized = true;
    LOG_INFO("[ModManager] Initialization complete");
    LOG_INFO("[ModManager] Discovered: " + std::to_string(m_status.modsDiscovered) + " mods");
    LOG_INFO("[ModManager] Compiled: " + std::to_string(m_status.modsCompiled) + " mods");

    return true;
}

bool ModManager::ProcessMods() {
    if (!m_initialized) {
        LOG_ERROR("[ModManager] Not initialized");
        return false;
    }

    if (m_modsProcessed) {
        LOG_WARN("[ModManager] Mods already processed");
        return true;
    }

    LOG_INFO("[ModManager] ========================================");
    LOG_INFO("[ModManager] Processing Mods (Loading & Hooking)");
    LOG_INFO("[ModManager] ========================================");

    // Initialize loader (needs Mono runtime ready)
    if (!ModLoader::Get().Initialize()) {
        m_status.errors.push_back("Failed to initialize mod loader");
        LOG_ERROR("[ModManager] Loader initialization failed");
        return false;
    }

    // Load S3MM.Helper.dll FIRST so S3MM.Native class exists before registering InternalCalls
    // Try embedded resource first (no external file needed), fall back to disk
    bool helperLoaded = false;
    {
        HMODULE hModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ModManager::Get, &hModule);
        if (hModule) {
            HRSRC hRes = FindResource(hModule, MAKEINTRESOURCE(IDR_S3MM_HELPER), RT_RCDATA);
            if (hRes) {
                HGLOBAL hData = LoadResource(hModule, hRes);
                DWORD resSize = SizeofResource(hModule, hRes);
                const char* resPtr = (const char*)LockResource(hData);
                if (resPtr && resSize > 0) {
                    void* helperAsm = ModLoader::Get().LoadAssemblyFromMemory(resPtr, resSize, "S3MM.Helper.dll");
                    if (helperAsm) {
                        LOG_INFO("[ModManager] Loaded S3MM.Helper.dll from embedded resource");
                        helperLoaded = true;
                    } else {
                        LOG_WARN("[ModManager] Failed to load embedded S3MM.Helper.dll");
                    }
                }
            }
        }
    }
    if (!helperLoaded) {
        // Fallback: load from disk
        std::filesystem::path helperDll = ModCompiler::Get().GetLibsPath() / "S3MM.Helper.dll";
        if (std::filesystem::exists(helperDll)) {
            void* helperAsm = ModLoader::Get().LoadAssemblyFromPath(helperDll);
            if (helperAsm) {
                LOG_INFO("[ModManager] Loaded S3MM.Helper.dll from disk (fallback)");
                helperLoaded = true;
            } else {
                LOG_WARN("[ModManager] Failed to load S3MM.Helper.dll - hooks won't work");
            }
        } else {
            LOG_WARN("[ModManager] S3MM.Helper.dll not found (embedded or on disk)");
        }
    }

    // Initialize hook manager (registers internal calls)
    // Must happen AFTER S3MM.Helper.dll is loaded
    if (!HookManager::Get().Initialize()) {
        m_status.errors.push_back("Failed to initialize hook manager");
        LOG_ERROR("[ModManager] HookManager initialization failed");
    }

    // Load all compiled mods
    m_status.modsLoaded = ModLoader::Get().LoadAllMods(m_discoveredMods);

    // Apply all hooks (Hook_/Watch_ naming + [S3MM.Hook]/[S3MM.Watcher] attributes)
    int hooksApplied = HookManager::Get().ApplyHooks();

    // Fire OnLoad callbacks for all loaded mods (after hooks are installed)
    for (const auto& mod : ModLoader::Get().GetLoadedMods()) { ModLoader::Get().FireOnLoadCallbacks(mod.name); }

    // OnLoad callbacks may register new hooks via Hook.RegisterByPointer().
    // Process any chains that need updating:
    //  - ReinstallDispatchStubs: existing chains where hooks were added (needsReinstall=true)
    //  - ApplyHooks: brand-new chains created during OnLoad (isPatched=false)
    int postOnLoadReinstalled = HookManager::Get().ReinstallDispatchStubs();
    int postOnLoadApplied = HookManager::Get().ApplyHooks();
    if (postOnLoadReinstalled > 0 || postOnLoadApplied > 0) {
        LOG_INFO("[ModManager] Post-OnLoad: reinstalled " + std::to_string(postOnLoadReinstalled) + " dispatch stubs, applied " + std::to_string(postOnLoadApplied) + " new chains");
    }

    // Per-mod hook install summary - requested / installed / REFUSED with reasons
    HookManager::Get().LogHookSummary();

    m_modsProcessed = true;

    LOG_INFO("[ModManager] ========================================");
    LOG_INFO("[ModManager] Mod Processing Complete");
    LOG_INFO("[ModManager] Loaded: " + std::to_string(m_status.modsLoaded) + " mods");
    LOG_INFO("[ModManager] Applied: " + std::to_string(hooksApplied + postOnLoadApplied) + " hook chains");
    LOG_INFO("[ModManager] ========================================");

    return true;
}

} // namespace S3MM
