#pragma once
#include <string>

namespace S3MM {

struct LoadedMod;

// PERSIST BRIDGE - Native driver for S3MM.PersistBridge (managed, in S3MM.Helper)
// PersistStatic.GameplayInitialize snapshots AppDomain.GetAssemblies() in StartupState; mods load later (StartupComplete detour), so their [PersistableStatic] fields never reach sPersistList
// Managed side appends by reflection, swaps entries on hot-reload (matched by field handle value), and fires Simulator.SignalAssembliesLoaded for [Tunable]
// No type-resolution remap is needed: mono prepends to domain_assemblies, so Persist.GetTypeByUnqualifiedName already resolves to the newest image
// This class only decides when, everything marshals to the sim thread
class PersistBridge {
  public:
    static PersistBridge& Get();

    // Resolve S3MM.PersistBridge in the helper image and run managed Install(). Inert on failure or when disabled by config
    bool Initialize(void* helperImage);
    bool IsReady() const { return m_ready; }

    // Register a loaded (or hot-reloaded) mod's fields and queue it for SignalAssembliesLoaded. Managed side detects reload by mod name
    void OnModLoaded(const LoadedMod& mod);

    // One Simulator.SignalAssembliesLoaded for everything queued by OnModLoaded
    void FlushSignal();

  private:
    PersistBridge() = default;
    ~PersistBridge() = default;
    PersistBridge(const PersistBridge&) = delete;
    PersistBridge& operator=(const PersistBridge&) = delete;

    bool Invoke(void* method, void** params, const std::string& what);
    static void* FindMethod(void* klass, const char* name);

    bool m_ready = false;
    void* m_onModLoaded = nullptr; // OnModLoaded(string assemblyName, string modName)
    void* m_flushSignal = nullptr; // FlushSignal()
};

} // namespace S3MM
