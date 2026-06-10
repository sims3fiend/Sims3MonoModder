#pragma once
#include "mod_compiler.h"
#include "mod_loader.h"
#include "hook_manager.h"
#include <vector>
#include <string>

namespace S3MM {

// MOD MANAGER - High-level API for the modding system, coordinates compilation, loading, and patching
class ModManager {
  public:
    static ModManager& Get();

    // Initialize the mod manager (call early, before Mono ready)
    bool Initialize();

    // Process mods - compile, load, and apply. Call this after Mono runtime is ready.
    bool ProcessMods();

    // Get status information
    struct Status {
        int modsDiscovered = 0;
        int modsCompiled = 0;
        int modsLoaded = 0;
        std::vector<std::string> errors;
    };
    const Status& GetStatus() const { return m_status; }

    // Discovered mods (includes ones that failed to compile, for error display)
    const std::vector<ModInfo>& GetDiscoveredMods() const { return m_discoveredMods; }

    // Get subsystems
    ModCompiler& GetCompiler() { return ModCompiler::Get(); }
    ModLoader& GetLoader() { return ModLoader::Get(); }
    HookManager& GetHookManager() { return HookManager::Get(); }

  private:
    ModManager() = default;
    ~ModManager() = default;
    ModManager(const ModManager&) = delete;
    ModManager& operator=(const ModManager&) = delete;

    bool m_initialized = false;
    bool m_modsProcessed = false;
    Status m_status;
    std::vector<ModInfo> m_discoveredMods;
};

} // namespace S3MM
