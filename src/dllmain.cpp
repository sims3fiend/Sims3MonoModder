// SIMS 3 MONO MODDER - Main entry point

#include "util/logger.h"
#include "util/mod_log.h"
#include "util/config.h"
#include "util/config_paths.h"
#include "core/mono_bridge.h"
#include "core/addresses.h"
#include "util/memory.h"
#include "core/assembly_hooks.h"
#include "core/script_host_hook.h"
#include "core/exception_hook.h"
#include "cache/mono_cache.h"
#include "modding/chain_compiler.h"
#include "modding/mod_manager.h"
#include "ui/imgui_manager.h"
#include "ui/main_window.h"
#include <Windows.h>

namespace S3MM {

// Callback when Mono runtime becomes ready
static void OnMonoRuntimeReady() {
    LOG_INFO("===========================================");
    LOG_INFO("Mono runtime ready - processing mods");
    LOG_INFO("===========================================");

    // Initialize Mono bridge
    if (!MonoBridge::Get().Initialize()) {
        LOG_ERROR("Failed to initialize MonoBridge");
        return;
    }
    LOG_INFO("MonoBridge initialized");

    // CO-RESIDENT MODULE SNAPSHOT - record the other native mod binaries sharing the process (ASIs, proxy DLLs).
    // Attribution only, no probing: the VEH crash path resolves fault addresses against this snapshot so a conflict crash names the real culprit instead of blaming MM.
    Memory::SnapshotModules();
    Memory::LogCoResidentModules();

    // Install global exception hook (mono_raise_exception) that logs all managed exceptions with source assembly attribution; requires Addresses.
    if (!ExceptionHook::Get().Initialize()) { LOG_WARN("Failed to install exception hook - exception logging unavailable"); }

    // Install native Mono lookup cache (detour hooks on 6 lookup functions); requires Addresses and MonoBridge, and stored function pointers route through hooks.
    MonoLookupCache::Get().Install();

    // Install the chain-compiler instrument (detour on SPEC_interp_add_data_item) used to locate CallNext data_items slots during hook link transforms
    ChainCompiler::Get().Install();

    // Load and patch mods
    ModManager::Get().ProcessMods();

    ScriptHostHook::Get().SetPhase(InitPhase::Ready);
}

DWORD WINAPI MainThread(LPVOID /*lpParam*/) {
    // Bail if injected into a non-game process (e.g. launcher)
    if (!Memory::IsGameProcess()) { return 0; }

    // Ensure Documents/.../S3MM/ directory exists before any file I/O
    ConfigPaths::EnsureDirectoryExists();

    // Initialize logger to Documents path
    Logger::Get().Initialize(ConfigPaths::GetLogPath());
    ModLog::Get().Initialize();
    LOG_INFO("===========================================");
    LOG_INFO("Sims 3 Mono Modder - Starting up");
    LOG_INFO("===========================================");

    // ADDRESS RESOLUTION - Pattern scan for all Mono/game addresses
    if (!Addresses::Initialize()) { LOG_ERROR("Failed to initialize addresses - critical addresses unresolved"); }
    // Validate() compares resolved addresses against the hardcoded Steam fallbacks, when I originaly wrote this I did mostly direct addresses which was a mistake :)
    // You would think I'd earn my lesson from S3SS but no
    // int mismatches = Addresses::Validate();

    // ASSEMBLY LOAD HOOKS - Must be installed before any assemblies load.
    // Captures mono_image_open_full (file loads) and mono_image_open_from_data_full (memory loads).
    if (!InstallImageLoadHooks()) { LOG_WARN("Failed to install image load hooks - path tracking unavailable"); }

    // Initialize config (loads settings from INI)
    Config::Get().Initialize();

    // SCRIPT HOST HOOK - Hook Script_InitHeap to fire OnMonoRuntimeReady when Mono initializes
    if (!ScriptHostHook::Get().Initialize()) {
        LOG_ERROR("Failed to initialize ScriptHostHook");
        return 0;
    }

    // MODDING SYSTEM - Compile mods early, before Mono is needed
    ModManager::Get().Initialize();

    // Initialize ImGui/D3D9 hook
    if (!ImGuiManager::Get().Initialize()) {
        LOG_ERROR("Failed to initialize ImGuiManager");
        return 0;
    }
    LOG_INFO("ImGuiManager initialized");

    // Initialize main window
    MainWindow::Get().Initialize();
    LOG_INFO("MainWindow initialized");

    // Set up render callback
    ImGuiManager::Get().SetRenderCallback([]() { MainWindow::Get().Render(); });

    std::string toggleKeyName = Config::GetKeyName(Config::Get().GetToggleKey());
    LOG_INFO("Initialization complete - press " + toggleKeyName + " to toggle UI");

    // MONO READY Must be last; fires immediately if Script_InitHeap already completed, and everything above must be initialized before the callback runs.
    ScriptHostHook::Get().SetOnMonoReady(OnMonoRuntimeReady);
    return 0;
}

} // namespace S3MM

// DLL ENTRY POINT - Process attach spawns main thread
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        HANDLE hThread = CreateThread(nullptr, 0, S3MM::MainThread, nullptr, 0, nullptr);
        if (hThread) { CloseHandle(hThread); }
    }

    return TRUE;
}
