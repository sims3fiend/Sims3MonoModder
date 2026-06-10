#include "script_host_hook.h"
#include "addresses.h"
#include "assembly_hooks.h"
#include "mono_bridge.h"
#include "../util/logger.h"
#include <Windows.h>
#include <detours/detours.h>

namespace S3MM {

// ORIGINAL FUNCTION POINTERS - Trampolines for hooked functions
using Script_InitHeap_t = int(__cdecl*)(int, int, int);
static Script_InitHeap_t s_originalScriptInitHeap = nullptr;

using StartupComplete_t = void(__cdecl*)();
static StartupComplete_t s_originalStartupComplete = nullptr;

// MonoScriptHost::ProcessTasks - __fastcall(pHost in ECX), per-frame task pump inside Simulate
using ProcessTasks_t = void(__fastcall*)(void* pHost);
static ProcessTasks_t s_originalProcessTasks = nullptr;

static std::function<void()>* s_readyCallback = nullptr;

// STARTUP COMPLETE HOOK - Detour for GameUtils_StartupComplete (0x00EC0EF0), icall fired by managed code when startup finishes. Runs on game thread with valid GC TLS.
static void __cdecl DetourStartupComplete() {
    // Let the game finish its startup event dispatch first
    s_originalStartupComplete();

    ScriptHostHook::Get().SetPhase(InitPhase::InitializingBridge);

    LOG_INFO("[ScriptHostHook] GameUtils_StartupComplete - firing OnMonoReady (ImageLoadCount=" + std::to_string(GetImageLoadCount()) + ")");

    if (s_readyCallback && *s_readyCallback) { (*s_readyCallback)(); }
}

// PROCESSTASKS DETOUR - Runs once per simulation frame on the sim thread, with valid interpreter + GC context.
// Let the game flush its tasks first, then drain S3MM's cross-thread invoke queue here so UI-originated invokes execute on the thread that owns interp_exec_state
static void __fastcall DetourProcessTasks(void* pHost) {
    s_originalProcessTasks(pHost);
    MonoBridge::Get().DrainSimTasks();
}

// SCRIPT_INITHEAP DETOUR - Intercepts heap initialization to detect Mono readiness
static int __cdecl DetourScriptInitHeap(int param1, int param2, int param3) {
    LOG_INFO("[ScriptHostHook] Script_InitHeap called");

    int result = s_originalScriptInitHeap(param1, param2, param3);

    LOG_INFO("[ScriptHostHook] Script_InitHeap returned: " + std::to_string(result));

    ScriptHostHook::Get().OnScriptInitHeapComplete();

    return result;
}

// SINGLETON - Thread-safe lazy instance
ScriptHostHook& ScriptHostHook::Get() {
    static ScriptHostHook instance;
    return instance;
}

// INITIALIZATION - Install Detours hooks on script host functions
bool ScriptHostHook::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("[ScriptHostHook] Installing hooks: Script_InitHeap @ " + ToHex(Addresses::ScriptHost.ScriptHost_InitHeap) + ", GameUtils_StartupComplete @ " + ToHex(Addresses::Misc.GameUtils_StartupComplete));

    s_originalScriptInitHeap = (Script_InitHeap_t)Addresses::ScriptHost.ScriptHost_InitHeap;
    s_originalStartupComplete = (StartupComplete_t)Addresses::Misc.GameUtils_StartupComplete;
    s_originalProcessTasks = (ProcessTasks_t)Addresses::ScriptHost.MonoScriptHost_ProcessTasks;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_originalScriptInitHeap, DetourScriptInitHeap);
    DetourAttach(&(PVOID&)s_originalStartupComplete, DetourStartupComplete);
    if (s_originalProcessTasks) { DetourAttach(&(PVOID&)s_originalProcessTasks, DetourProcessTasks); }

    LONG error = DetourTransactionCommit();
    if (error != NO_ERROR) {
        LOG_ERROR("[ScriptHostHook] Failed to install hooks, error: " + std::to_string(error));
        return false;
    }

    m_initialized = true;
    LOG_INFO("[ScriptHostHook] Hooks installed successfully");

    // Check if Script_InitHeap was already called before our hook was installed.
    void* scriptVM = *(void**)Addresses::Globals.g_ScriptVM;
    if (scriptVM != nullptr) {
        LOG_WARN("[ScriptHostHook] g_ScriptVM already set (" + ToHex((uintptr_t)scriptVM) + ") - Script_InitHeap completed before hook");
        m_monoReady = true;
        SetPhase(InitPhase::WaitingForStartup);
    } else {
        SetPhase(InitPhase::WaitingForScriptVM);
    }

    return true;
}

// CALLBACK REGISTRATION - Store the OnMonoReady handler
void ScriptHostHook::SetOnMonoReady(std::function<void()> callback) {
    m_onMonoReady = std::move(callback);
    s_readyCallback = &m_onMonoReady;
    LOG_INFO("[ScriptHostHook] OnMonoReady callback registered - waiting for GameUtils_StartupComplete");
}

// SCRIPT_INITHEAP COMPLETE - Mark Mono as ready and advance phase
void ScriptHostHook::OnScriptInitHeapComplete() {
    if (m_monoReady) return;

    m_monoReady = true;
    SetPhase(InitPhase::WaitingForStartup);

    LOG_INFO("[ScriptHostHook] Script_InitHeap completed - waiting for GameUtils_StartupComplete");
}

} // namespace S3MM
