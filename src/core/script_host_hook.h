#pragma once
#include <functional>
#include <atomic>

namespace S3MM {

// INIT PHASE TRACKING - Exposes current initialization stage to UI
enum class InitPhase : int { WaitingForHook = 0, WaitingForScriptVM, WaitingForStartup, InitializingBridge, Ready };

// SCRIPT HOST HOOK - Hooks Script_InitHeap and GameUtils_StartupComplete to detect Mono runtime creation and game startup
class ScriptHostHook {
  public:
    static ScriptHostHook& Get();

    // Initialize hooks, call early in DllMain thread
    bool Initialize();

    // Set callback for when Mono + game assemblies are ready.
    // Must be called after all DLL-side init is complete, callback may fire on the game thread.
    void SetOnMonoReady(std::function<void()> callback);

    // Called internally by the Script_InitHeap detour
    void OnScriptInitHeapComplete();

    // Init phase tracking, thread-safe, polled by UI
    InitPhase GetPhase() const { return m_phase.load(std::memory_order_relaxed); }
    void SetPhase(InitPhase p) { m_phase.store(p, std::memory_order_relaxed); }

  private:
    ScriptHostHook() = default;
    ~ScriptHostHook() = default;
    ScriptHostHook(const ScriptHostHook&) = delete;
    ScriptHostHook& operator=(const ScriptHostHook&) = delete;

    bool m_initialized = false;
    bool m_monoReady = false; // Script_InitHeap has returned
    std::function<void()> m_onMonoReady;

    std::atomic<InitPhase> m_phase{InitPhase::WaitingForHook};
};

} // namespace S3MM
