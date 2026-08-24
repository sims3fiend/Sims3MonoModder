#include "hook_manager.h"
#include "method_patcher.h"
#include "mod_loader.h"
#include "audit_api.h"
#include "../core/addresses.h"
#include "../core/mono_bridge.h"
#include "../cache/cache_api.h"
#include "../util/logger.h"
#include "../util/mod_log.h"
#include "../util/config_paths.h"
#include "../core/exception_helper.h"
#include "../core/exception_hook.h"
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <unordered_set>
#include <Windows.h>

namespace S3MM {

// THREAD-LOCAL CHAIN STATE - Stack of active chain executions, supports nested/recursive hooks
thread_local std::vector<ChainExecutionState> t_chainStack;

// INVOKE BRIDGES - a runtime-invoked method executes in a frame whose parent is NULL (the invoke's own base frame is separate and never chained)
// The interpreter's parent links stop at every hook[1..N] / trampoline entry; every runtime-invoke in the dispatch path records the frame it was made from
// ResolveCallerFrame steps across a NULL parent onto the innermost unconsumed entry (LIFO matches nesting since all entries sit on this thread's native stack)
thread_local std::vector<InterpFrame*> t_invokeBridges;

static InterpFrame* CurrentInterpFrame() {
    if (!Addresses::Profiler.g_thread_context_tls_index) return nullptr;
    auto* tc = reinterpret_cast<ThreadContext*>(TlsGetValue(*(DWORD*)Addresses::Profiler.g_thread_context_tls_index));
    return tc ? tc->current_frame : nullptr;
}

struct InvokeBridge {
    InvokeBridge() { t_invokeBridges.push_back(CurrentInterpFrame()); }
    ~InvokeBridge() { t_invokeBridges.pop_back(); }
};

// Helper to find active chain state for a given token
static ChainExecutionState* FindChainState(uint32_t methodToken) {
    for (auto it = t_chainStack.rbegin(); it != t_chainStack.rend(); ++it) {
        if (it->methodToken == methodToken) { return &(*it); }
    }
    return nullptr;
}

// PRIMITIVE KLASS LOOKUP - Resolve from Addresses::Defaults
static void* GetPrimitiveKlass(MonoTypeCode typeCode) {
    using namespace Addresses;
    uintptr_t addr = 0;
    switch (typeCode) {
    case MonoTypeCode::Boolean:
        addr = Defaults.boolean_class;
        break;
    case MonoTypeCode::Char:
        addr = Defaults.char_class;
        break;
    case MonoTypeCode::I1:
        addr = Defaults.sbyte_class;
        break;
    case MonoTypeCode::U1:
        addr = Defaults.byte_class;
        break;
    case MonoTypeCode::I2:
        addr = Defaults.int16_class;
        break;
    case MonoTypeCode::U2:
        addr = Defaults.uint16_class;
        break;
    case MonoTypeCode::I4:
        addr = Defaults.int32_class;
        break;
    case MonoTypeCode::U4:
        addr = Defaults.uint32_class;
        break;
    case MonoTypeCode::I8:
        addr = Defaults.int64_class;
        break;
    case MonoTypeCode::U8:
        addr = Defaults.uint64_class;
        break;
    case MonoTypeCode::R4:
        addr = Defaults.single_class;
        break;
    case MonoTypeCode::R8:
        addr = Defaults.double_class;
        break;
    default:
        return nullptr;
    }
    return addr ? *(void**)addr : nullptr;
}

// RAII guard for chain stack cleanup, ensures entry is popped even on exception
class ChainStackGuard {
  public:
    ChainStackGuard(uint32_t token, bool isNewChain) : m_token(token), m_shouldPop(isNewChain) {}

    // Call this when chain completes normally (resets so destructor doesn't double-pop)
    void MarkComplete() { m_shouldPop = false; }

    ~ChainStackGuard() {
        if (m_shouldPop) {
            // Exception path - pop unconditionally if we pushed
            if (!t_chainStack.empty() && t_chainStack.back().methodToken == m_token) {
                LOG_DEBUG("[ChainStackGuard] Exception cleanup for token " + std::to_string(m_token));
                t_chainStack.pop_back();
            }
        }
    }

  private:
    uint32_t m_token;
    bool m_shouldPop;
};

// SINGLETON - Global access
HookManager& HookManager::Get() {
    static HookManager instance;
    return instance;
}

// INITIALIZATION - Register internal calls for managed-to-native bridge
bool HookManager::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("[HookManager] Initializing...");

    // Register internal calls for managed code to call back into native
    MonoBridge::Get().AddInternalCall("S3MM.Native::CallNext", (void*)&HookManager::NativeCallNext);
    MonoBridge::Get().AddInternalCall("S3MM.Native::CallOriginal", (void*)&HookManager::NativeCallOriginal);
    MonoBridge::Get().AddInternalCall("S3MM.Native::GetCallerHookToken", (void*)&HookManager::NativeGetCallerHookToken);
    MonoBridge::Get().AddInternalCall("S3MM.Native::TargetIsInstance", (void*)&HookManager::NativeTargetIsInstance);

    // Caller introspection (S3MM.Hook.Caller*)
    MonoBridge::Get().AddInternalCall("S3MM.Native::CallerMethod", (void*)&HookManager::NativeCallerMethod);
    MonoBridge::Get().AddInternalCall("S3MM.Native::CallerName", (void*)&HookManager::NativeCallerName);
    MonoBridge::Get().AddInternalCall("S3MM.Native::CallerIs", (void*)&HookManager::NativeCallerIs);
    MonoBridge::Get().AddInternalCall("S3MM.Native::CallerInstance", (void*)&HookManager::NativeCallerInstance);
    MonoBridge::Get().AddInternalCall("S3MM.Native::CallerArgs", (void*)&HookManager::NativeCallerArgs);
    MonoBridge::Get().AddInternalCall("S3MM.Native::CallerMintOffset", (void*)&HookManager::NativeCallerMintOffset);

    // Auto-prefixed log API (S3MM.Log class)
    MonoBridge::Get().AddInternalCall("S3MM.Log::Info", (void*)&HookManager::NativeLogInfo);
    MonoBridge::Get().AddInternalCall("S3MM.Log::Warn", (void*)&HookManager::NativeLogWarn);
    MonoBridge::Get().AddInternalCall("S3MM.Log::Error", (void*)&HookManager::NativeLogError);
    MonoBridge::Get().AddInternalCall("S3MM.Log::Debug", (void*)&HookManager::NativeLogDebug);
    MonoBridge::Get().AddInternalCall("S3MM.Log::Raw", (void*)&HookManager::NativeLogRaw);

    MonoBridge::Get().AddInternalCall("S3MM.Native::Dispatch", (void*)&HookManager::NativeDispatch);
    MonoBridge::Get().AddInternalCall("S3MM.InstanceDispatch::SetCurrentToken", (void*)&HookManager::NativeSetCurrentToken);

    // Cache API - fast native lookup and field access for mods
    MonoBridge::Get().AddInternalCall("S3MM.Cache::FindClass", (void*)&CacheAPI::NativeFindClass);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::FindField", (void*)&CacheAPI::NativeFindField);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::FindMethod", (void*)&CacheAPI::NativeFindMethod);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetField", (void*)&CacheAPI::NativeGetField);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetField", (void*)&CacheAPI::NativeSetField);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticField", (void*)&CacheAPI::NativeGetStaticField);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticField", (void*)&CacheAPI::NativeSetStaticField);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::Invoke", (void*)&CacheAPI::NativeInvoke);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::FindNestedClass", (void*)&CacheAPI::NativeFindNestedClass);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetObjectClass", (void*)&CacheAPI::NativeGetObjectClass);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetClassName", (void*)&CacheAPI::NativeGetClassName);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetClassNamespace", (void*)&CacheAPI::NativeGetClassNamespace);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetParentClass", (void*)&CacheAPI::NativeGetParentClass);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::IsEnum", (void*)&CacheAPI::NativeIsEnum);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetMethodName", (void*)&CacheAPI::NativeGetMethodName);

    // Typed field access - no boxing overhead
    // Instance get/set
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetFieldInt32", (void*)&CacheAPI::NativeGetFieldInt32);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetFieldInt32", (void*)&CacheAPI::NativeSetFieldInt32);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetFieldUInt32", (void*)&CacheAPI::NativeGetFieldUInt32);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetFieldUInt32", (void*)&CacheAPI::NativeSetFieldUInt32);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetFieldInt64", (void*)&CacheAPI::NativeGetFieldInt64);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetFieldInt64", (void*)&CacheAPI::NativeSetFieldInt64);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetFieldUInt64", (void*)&CacheAPI::NativeGetFieldUInt64);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetFieldUInt64", (void*)&CacheAPI::NativeSetFieldUInt64);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetFieldFloat", (void*)&CacheAPI::NativeGetFieldFloat);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetFieldFloat", (void*)&CacheAPI::NativeSetFieldFloat);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetFieldDouble", (void*)&CacheAPI::NativeGetFieldDouble);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetFieldDouble", (void*)&CacheAPI::NativeSetFieldDouble);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetFieldBool", (void*)&CacheAPI::NativeGetFieldBool);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetFieldBool", (void*)&CacheAPI::NativeSetFieldBool);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetFieldByte", (void*)&CacheAPI::NativeGetFieldByte);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetFieldByte", (void*)&CacheAPI::NativeSetFieldByte);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetFieldSByte", (void*)&CacheAPI::NativeGetFieldSByte);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetFieldSByte", (void*)&CacheAPI::NativeSetFieldSByte);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetFieldInt16", (void*)&CacheAPI::NativeGetFieldInt16);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetFieldInt16", (void*)&CacheAPI::NativeSetFieldInt16);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetFieldUInt16", (void*)&CacheAPI::NativeGetFieldUInt16);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetFieldUInt16", (void*)&CacheAPI::NativeSetFieldUInt16);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetFieldChar", (void*)&CacheAPI::NativeGetFieldChar);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetFieldChar", (void*)&CacheAPI::NativeSetFieldChar);
    // Static get/set
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticFieldInt32", (void*)&CacheAPI::NativeGetStaticFieldInt32);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticFieldInt32", (void*)&CacheAPI::NativeSetStaticFieldInt32);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticFieldUInt32", (void*)&CacheAPI::NativeGetStaticFieldUInt32);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticFieldUInt32", (void*)&CacheAPI::NativeSetStaticFieldUInt32);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticFieldInt64", (void*)&CacheAPI::NativeGetStaticFieldInt64);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticFieldInt64", (void*)&CacheAPI::NativeSetStaticFieldInt64);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticFieldUInt64", (void*)&CacheAPI::NativeGetStaticFieldUInt64);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticFieldUInt64", (void*)&CacheAPI::NativeSetStaticFieldUInt64);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticFieldFloat", (void*)&CacheAPI::NativeGetStaticFieldFloat);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticFieldFloat", (void*)&CacheAPI::NativeSetStaticFieldFloat);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticFieldDouble", (void*)&CacheAPI::NativeGetStaticFieldDouble);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticFieldDouble", (void*)&CacheAPI::NativeSetStaticFieldDouble);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticFieldBool", (void*)&CacheAPI::NativeGetStaticFieldBool);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticFieldBool", (void*)&CacheAPI::NativeSetStaticFieldBool);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticFieldByte", (void*)&CacheAPI::NativeGetStaticFieldByte);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticFieldByte", (void*)&CacheAPI::NativeSetStaticFieldByte);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticFieldSByte", (void*)&CacheAPI::NativeGetStaticFieldSByte);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticFieldSByte", (void*)&CacheAPI::NativeSetStaticFieldSByte);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticFieldInt16", (void*)&CacheAPI::NativeGetStaticFieldInt16);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticFieldInt16", (void*)&CacheAPI::NativeSetStaticFieldInt16);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticFieldUInt16", (void*)&CacheAPI::NativeGetStaticFieldUInt16);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticFieldUInt16", (void*)&CacheAPI::NativeSetStaticFieldUInt16);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::GetStaticFieldChar", (void*)&CacheAPI::NativeGetStaticFieldChar);
    MonoBridge::Get().AddInternalCall("S3MM.Cache::SetStaticFieldChar", (void*)&CacheAPI::NativeSetStaticFieldChar);

    // Hook by pointer (baking is inferred from the handler's IL - no separate typed entry point)
    MonoBridge::Get().AddInternalCall("S3MM.Hook::RegisterByPointer", (void*)&HookManager::NativeRegisterHookByPointer);
    // Apply pending hook changes on demand (reinstall + apply) - lets a mod commit a runtime registration
    MonoBridge::Get().AddInternalCall("S3MM.Hook::ApplyPending", (void*)&HookManager::NativeApplyPendingHooks);

    // Audit API - hook/conflict introspection for managed diagnostic tools (e.g. NRaas ErrorTrap)
    MonoBridge::Get().AddInternalCall("S3MM.Audit::IsMethodHooked", (void*)&AuditAPI::NativeIsMethodHooked);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::IsMethodBaked", (void*)&AuditAPI::NativeIsMethodBaked);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetBakeRejectReason", (void*)&AuditAPI::NativeGetBakeRejectReason);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::IsAssemblyReplaced", (void*)&AuditAPI::NativeIsAssemblyReplaced);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetHookedMethodCount", (void*)&AuditAPI::NativeGetHookedMethodCount);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetAllHooks", (void*)&AuditAPI::NativeGetAllHooks);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetHookChain", (void*)&AuditAPI::NativeGetHookChain);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetReplacedAssemblies", (void*)&AuditAPI::NativeGetReplacedAssemblies);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetLoadedModCount", (void*)&AuditAPI::NativeGetLoadedModCount);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetLoadedMods", (void*)&AuditAPI::NativeGetLoadedMods);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetRefusedHooks", (void*)&AuditAPI::NativeGetRefusedHooks);

    m_initialized = true;
    LOG_INFO("[HookManager] Initialized");
    return true;
}

// DISPATCH TOKEN ALLOCATION - see HookChain::dispatchToken
// Prefer the target's real MethodDef token; on a cross-assembly collision probe upward in 0x01000000 steps, leaving the 0x06xxxxxx MethodDef space (the value is opaque to managed code, so any unique u32 works)
uint32_t HookManager::AllocateDispatchToken(uint32_t preferredToken, void* targetMethod) {
    uint32_t key = preferredToken;
    for (;;) {
        auto it = m_tokenToMethod.find(key);
        if (it == m_tokenToMethod.end() || it->second == targetMethod) return key; // free, or already this chain's
        key += 0x01000000;
    }
}

// HOOK REGISTRATION - Register a hook or watcher by class/method name
bool HookManager::RegisterHook(const std::string& targetClass, const std::string& targetMethod, void* hookMethod, HookType type, const std::string& modName, int priority, const std::string& signature) {
    RecordHookRequest(modName);
    if (!hookMethod) {
        LOG_ERROR("[HookManager] RegisterHook: null hook method");
        RecordHookRefusal(modName, targetClass + ":" + targetMethod, "null hook method pointer passed to Register");
        return false;
    }

    std::string sigInfo = signature.empty() ? "" : signature;
    LOG_INFO("[HookManager] Registering " + std::string(type == HookType::Hook ? "hook" : "watcher") + ": " + targetClass + ":" + targetMethod + sigInfo + " from mod: " + modName);

    // Derive the registering hook's image so a target in a mod assembly (self-targeting) can resolve even though ScriptHost::FindClass only searches game assemblies
    void* resolveImage = nullptr;
    if (auto* hl = reinterpret_cast<MonoMethodLayout*>(hookMethod); hl && hl->klass) { resolveImage = *(void**)((uintptr_t)hl->klass + Offsets::CLASS_IMAGE); }

    // Find or create the hook chain
    void* originalMethod = FindTargetMethod(targetClass, targetMethod, signature, resolveImage);
    if (!originalMethod) {
        LOG_ERROR("[HookManager] Target method not found: " + targetClass + ":" + targetMethod + sigInfo);
        RecordHookRefusal(modName, targetClass + ":" + targetMethod, "target method not found (typo, renamed game method, or an unloaded assembly)");
        return false;
    }

    // Same refusal as the by-pointer path - a nested class inside a generic type, or an inherited member reached through one, can land here inflated even though the attribute named a plain type
    // Inflated targets are supported (full-size trampoline copy); only open generic definitions and corrupt shapes refuse
    if (std::string hazard = DescribeTrampolineHazard(originalMethod); !hazard.empty()) {
        LOG_ERROR("[HookManager] RegisterHook: refusing target " + targetClass + ":" + targetMethod + sigInfo + " from mod '" + modName + "' - " + hazard + ". See the generic-target notes in the modding docs.");
        RecordHookRefusal(modName, targetClass + ":" + targetMethod, "unhookable target: " + hazard);
        return false;
    }

    // Get or create chain
    auto& chain = m_chains[originalMethod];
    if (!chain.originalMethod) {
        // New chain - initialize it
        chain.originalMethod = originalMethod;
        chain.targetClass = targetClass;
        chain.targetMethod = targetMethod;
        chain.signature = signature;
        BackupOriginalMethod(chain);
    }

    // Create hook entry
    HookEntry entry;
    entry.modName = modName;
    entry.hookMethod = hookMethod;
    entry.type = type;
    entry.priority = priority;
    entry.usesChain = ChainCompiler::HookUsesChain(hookMethod); // inferred bake candidacy (no opt-in flag)

    // Add to appropriate list
    if (type == HookType::Hook) {
        chain.hooks.push_back(entry);
        // Sort hooks: higher priority first
        std::sort(chain.hooks.begin(), chain.hooks.end(), [](const HookEntry& a, const HookEntry& b) { return a.priority > b.priority; });
    } else if (type == HookType::Watcher) {
        // Track in watchers list for reporting
        chain.watchers.push_back(entry);

        // Add to hooks with VERY HIGH priority so they execute FIRST.
        // This ensures watchers run before any other hooks.
        // Watchers should call CallNext to continue the chain - this way they observe the call even if later hooks don't call CallNext.
        HookEntry watcherAsHook = entry;
        watcherAsHook.priority = 1000000 + priority; // Very high, but respect relative priority
        chain.hooks.push_back(watcherAsHook);
        std::sort(chain.hooks.begin(), chain.hooks.end(), [](const HookEntry& a, const HookEntry& b) { return a.priority > b.priority; });
    }

    // If this chain already has a dispatch stub installed, adding a new hook may change which hook is at position [0] (the entry point).
    // Mark for reinstall so ReinstallDispatchStubs will update the stub to the new hook[0].
    if (chain.isPatched) { chain.needsReinstall = true; }

    // Track token -> method mapping for dispatch, keyed by the chain's unique dispatch token (see HookChain::dispatchToken)
    if (chain.dispatchToken == 0) {
        chain.dispatchToken = AllocateDispatchToken(chain.originalToken, originalMethod);
        if (chain.dispatchToken != chain.originalToken)
            LOG_INFO("[HookManager] MethodDef token " + std::to_string(chain.originalToken) + " already claimed by another assembly's target; " + targetClass + "::" + targetMethod + " dispatches via synthetic token " +
                     std::to_string(chain.dispatchToken));
    }
    uint32_t dispToken = chain.dispatchToken;
    m_tokenToMethod[dispToken] = originalMethod;

    // Track hook method -> token for auto-token resolution via GetCallerHookToken frame walk
    m_hookMethodToToken[hookMethod] = dispToken;

    // Also store by (image, hookToken) composite key, more reliable since MonoMethod* can differ at runtime.
    // Using a composite key because hook tokens can collide across different mod assemblies.
    auto* hookLayout = reinterpret_cast<MonoMethodLayout*>(hookMethod);
    void* hookImage = hookLayout->klass ? *(void**)((uintptr_t)hookLayout->klass + Offsets::CLASS_IMAGE) : nullptr;
    uint64_t compositeKey = MakeHookTokenKey(hookImage, hookLayout->token);
    m_hookTokenToTargetToken[compositeKey] = dispToken;
    LOG_DEBUG("[HookManager] Stored hook method @ " + std::to_string((uintptr_t)hookMethod) + " (hookToken=" + std::to_string(hookLayout->token) + ", image=" + std::to_string((uintptr_t)hookImage) + ")" +
              " -> dispatchToken " + std::to_string(dispToken));

    LOG_INFO("[HookManager] Hook registered. Chain now has " + std::to_string(chain.hooks.size()) + " hooks (" + std::to_string(chain.watchers.size()) + " watchers)");

    return true;
}

// NESTED-AWARE CLASS PATH - "Ns.Outer/Inner/Definition" for by-pointer registrations: the bare name collides across mods in Audit's classMethod index and the quarantine key
// Walks CLASS_NESTED_IN innermost->outermost and prepends the outermost namespace; a non-nested class gets plain "Ns.Class"
// An INFLATED class carries its generic definition's name ("GenBox`1"), so instantiations share Audit/quarantine strings (chains stay distinct, keyed by MonoMethod*)
static std::string DeriveFullClassName(void* klass) {
    if (!klass || !IsReadablePointer(klass, Offsets::CLASS_NESTED_IN + sizeof(void*))) return "???";
    std::string path;
    void* k = klass;
    for (int depth = 0; k && depth < 8; ++depth) {
        const char* cls = *(const char**)((uintptr_t)k + Offsets::CLASS_NAME);
        path = std::string((cls && IsReadablePointer((void*)cls, 2)) ? cls : "???") + (path.empty() ? "" : "/" + path);
        void* outer = *(void**)((uintptr_t)k + Offsets::CLASS_NESTED_IN);
        if (!outer || !IsReadablePointer(outer, Offsets::CLASS_NESTED_IN + sizeof(void*))) {
            const char* ns = *(const char**)((uintptr_t)k + Offsets::CLASS_NAMESPACE);
            if (ns && IsReadablePointer((void*)ns, 2) && ns[0]) path = std::string(ns) + "." + path;
            return path;
        }
        k = outer;
    }
    return path;
}

// HOOK REGISTRATION BY POINTER - Register using raw MonoMethod* directly
bool HookManager::RegisterHookByPointer(void* targetMethod, void* hookMethod, HookType type, const std::string& modName, int priority) {
    RecordHookRequest(modName);
    if (!targetMethod || !hookMethod) {
        LOG_ERROR("[HookManager] RegisterHookByPointer: null target or hook method");
        RecordHookRefusal(modName, "??:??", "null target or hook method pointer passed to Register");
        return false;
    }

    // Refuse unsound targets BEFORE anything reads their metadata - registration is the last recoverable point (downstream the failure mode is abort())
    // Inflated targets are supported via the full-size trampoline copy; only open generic definitions and corrupt shapes refuse
    if (std::string hazard = DescribeTrampolineHazard(targetMethod); !hazard.empty()) {
        LOG_ERROR("[HookManager] RegisterHookByPointer: refusing target @ " + ToHex((uintptr_t)targetMethod) + " from mod '" + modName + "' - " + hazard + ". See the generic-target notes in the modding docs.");
        auto* hazardLayout = reinterpret_cast<MonoMethodLayout*>(targetMethod);
        const char* hazardName = (hazardLayout->name && IsReadablePointer((void*)hazardLayout->name, 2)) ? hazardLayout->name : "???";
        RecordHookRefusal(modName, DeriveFullClassName(hazardLayout->klass) + ":" + hazardName, "unhookable target: " + hazard);
        return false;
    }

    // Check if a chain already exists for this method (e.g., from attribute-based registration), and use the chain's backup data for consistency with the canonical token/klass source.
    auto existingIt = m_chains.find(targetMethod);
    bool chainExists = (existingIt != m_chains.end() && existingIt->second.originalMethod != nullptr);

    // Derive class/method names and token from the correct source
    auto* targetLayout = reinterpret_cast<MonoMethodLayout*>(targetMethod);
    std::string targetMethodName;
    std::string targetClassName;
    uint32_t originalToken = 0;

    if (chainExists) {
        // Chain already exists - use backed-up metadata (safe from patching)
        targetMethodName = existingIt->second.targetMethod;
        targetClassName = existingIt->second.targetClass;
        originalToken = existingIt->second.originalToken;
    } else {
        // New chain - MonoMethodLayout is unpatched, safe to read directly
        // Nested-qualified path (Ns.Outer/Inner) so Audit lookups and quarantine keys are unambiguous
        targetMethodName = targetLayout->name ? targetLayout->name : "???";
        targetClassName = DeriveFullClassName(targetLayout->klass);
        originalToken = targetLayout->token;
    }

    LOG_INFO("[HookManager] RegisterHookByPointer: " + std::string(type == HookType::Hook ? "hook" : "watcher") + " -> " + targetClassName + ":" + targetMethodName + " (token=" + std::to_string(originalToken) + ")" +
             " @ " + ToHex((uintptr_t)targetMethod) + " from mod: " + modName + (chainExists ? " [existing chain]" : " [new chain]"));

    // Get or create chain, keyed on the raw MonoMethod*
    auto& chain = m_chains[targetMethod];
    if (!chain.originalMethod) {
        chain.originalMethod = targetMethod;
        chain.targetClass = targetClassName;
        chain.targetMethod = targetMethodName;
        BackupOriginalMethod(chain);
        // Re-read originalToken from backup (BackupOriginalMethod stores it)
        originalToken = chain.originalToken;
    }

    // Create hook entry
    HookEntry entry;
    entry.modName = modName;
    entry.hookMethod = hookMethod;
    entry.type = type;
    entry.priority = priority;
    entry.usesChain = ChainCompiler::HookUsesChain(hookMethod); // inferred bake candidacy (no opt-in flag)

    // Add to appropriate list (identical logic to RegisterHook)
    if (type == HookType::Hook) {
        chain.hooks.push_back(entry);
        std::sort(chain.hooks.begin(), chain.hooks.end(), [](const HookEntry& a, const HookEntry& b) { return a.priority > b.priority; });
    } else if (type == HookType::Watcher) {
        chain.watchers.push_back(entry);
        HookEntry watcherAsHook = entry;
        watcherAsHook.priority = 1000000 + priority;
        chain.hooks.push_back(watcherAsHook);
        std::sort(chain.hooks.begin(), chain.hooks.end(), [](const HookEntry& a, const HookEntry& b) { return a.priority > b.priority; });
    }

    if (chain.isPatched) { chain.needsReinstall = true; }

    // Track token -> method mapping for dispatch, keyed by the chain's unique dispatch token (see HookChain::dispatchToken)
    if (chain.dispatchToken == 0) {
        chain.dispatchToken = AllocateDispatchToken(originalToken, targetMethod);
        if (chain.dispatchToken != originalToken)
            LOG_INFO("[HookManager] MethodDef token " + std::to_string(originalToken) + " already claimed by another assembly's target; " + targetClassName + "::" + targetMethodName + " dispatches via synthetic token " +
                     std::to_string(chain.dispatchToken));
    }
    uint32_t dispToken = chain.dispatchToken;
    m_tokenToMethod[dispToken] = targetMethod;

    // Track hook method -> token for auto-token resolution via GetCallerHookToken
    m_hookMethodToToken[hookMethod] = dispToken;

    // Also store by (image, hookToken) composite key
    auto* hookLayout = reinterpret_cast<MonoMethodLayout*>(hookMethod);
    void* hookImage = hookLayout->klass ? *(void**)((uintptr_t)hookLayout->klass + Offsets::CLASS_IMAGE) : nullptr;
    uint64_t compositeKey = MakeHookTokenKey(hookImage, hookLayout->token);
    m_hookTokenToTargetToken[compositeKey] = dispToken;

    LOG_INFO("[HookManager] Hook registered by pointer. Chain now has " + std::to_string(chain.hooks.size()) + " hooks (" + std::to_string(chain.watchers.size()) + " watchers)" +
             ", originalToken=" + std::to_string(originalToken) + (dispToken != originalToken ? " (dispatchToken=" + std::to_string(dispToken) + ")" : ""));

    return true;
}

// Resolve the logical mod name from a hook method's image (stable across hot-reloads)
static std::string ResolveModNameForHook(void* hookMethod) {
    std::string modName = "unknown";
    if (hookMethod) {
        auto* hookLayout = reinterpret_cast<MonoMethodLayout*>(hookMethod);
        if (hookLayout->klass) {
            void* image = *(void**)((uintptr_t)hookLayout->klass + Offsets::CLASS_IMAGE);
            if (image) {
                std::string logical = ModLoader::Get().GetModNameForImage(image);
                if (!logical.empty()) {
                    modName = logical;
                } else {
                    const char* asmName = *(const char**)((uintptr_t)image + Offsets::IMAGE_ASSEMBLY_NAME);
                    if (asmName) modName = asmName;
                }
            }
        }
    }
    return modName;
}

// Baking is inferred from the handler's IL (HookUsesChain) at registration; there is no separate typed entry point
void __cdecl HookManager::NativeRegisterHookByPointer(void* targetMethod, void* hookMethod, int32_t hookType, int32_t priority) {
    HookType type = (hookType == 1) ? HookType::Watcher : HookType::Hook;
    HookManager::Get().RegisterHookByPointer(targetMethod, hookMethod, type, ResolveModNameForHook(hookMethod), priority);
}

// Commit pending hook changes without a mod reload: recompile chains marked needsReinstall (baked chains in place), then install any not-yet-patched chains
void __cdecl HookManager::NativeApplyPendingHooks() {
    HookManager::Get().ReinstallDispatchStubs();
    HookManager::Get().ApplyHooks();
    // Runtime-registered hooks: surface new refusals without re-printing the full startup summary
    HookManager::Get().LogHookSummary(/*onlyIfNewRefusals=*/true);
}

// Is the target behind this token an instance method? Used by the un-baked Chain.CallNext boxing fallback to split a leading-self arg from the params (managed can't read the target's hasThis); 0 for an unknown token
int32_t __cdecl HookManager::NativeTargetIsInstance(uint32_t methodToken) {
    auto& mgr = HookManager::Get();
    auto it = mgr.m_tokenToMethod.find(methodToken);
    if (it == mgr.m_tokenToMethod.end()) return 0;
    HookChain* chain = mgr.GetChain(it->second);
    return (chain && chain->originalIsInstance) ? 1 : 0;
}

// REMOVE HOOKS FOR MOD - Unregister all hooks belonging to a given mod name
int HookManager::RemoveHooksForMod(const std::string& modName) {
    int removed = 0;

    int staleMapEntries = 0;

    // Reset install diagnostics so a hot-reloaded mod recounts from zero instead of double-counting its re-registrations
    m_modStats.erase(modName);

    for (auto& [method, chain] : m_chains) {
        size_t hooksBefore = chain.hooks.size();

        // Clean up map entries for hooks being removed BEFORE erasing them
        for (auto it = chain.hooks.begin(); it != chain.hooks.end(); ++it) {
            if (it->modName != modName) continue;

            // Erase from m_hookMethodToToken
            auto ptrIt = m_hookMethodToToken.find(it->hookMethod);
            if (ptrIt != m_hookMethodToToken.end()) {
                m_hookMethodToToken.erase(ptrIt);
                staleMapEntries++;
            }

            // Erase from m_hookTokenToTargetToken (composite key: image|token)
            auto* hookLayout = reinterpret_cast<MonoMethodLayout*>(it->hookMethod);
            if (hookLayout->klass) {
                void* hookImage = *(void**)((uintptr_t)hookLayout->klass + Offsets::CLASS_IMAGE);
                uint64_t compositeKey = MakeHookTokenKey(hookImage, hookLayout->token);
                auto compositeIt = m_hookTokenToTargetToken.find(compositeKey);
                if (compositeIt != m_hookTokenToTargetToken.end()) {
                    m_hookTokenToTargetToken.erase(compositeIt);
                    staleMapEntries++;
                }
            }
        }

        // Remove from hooks
        auto hookIt = std::remove_if(chain.hooks.begin(), chain.hooks.end(), [&modName](const HookEntry& e) { return e.modName == modName; });
        removed += std::distance(hookIt, chain.hooks.end());
        chain.hooks.erase(hookIt, chain.hooks.end());

        // Remove from watchers (tracking list only, they're also in hooks)
        auto watcherIt = std::remove_if(chain.watchers.begin(), chain.watchers.end(), [&modName](const HookEntry& e) { return e.modName == modName; });
        chain.watchers.erase(watcherIt, chain.watchers.end());

        // If we removed any hooks, the dispatch stub needs updating
        if (chain.hooks.size() != hooksBefore) {
            chain.needsReinstall = true;
            LOG_DEBUG("[HookManager] Chain " + chain.targetClass + "::" + chain.targetMethod + " marked for reinstall (hooks changed: " + std::to_string(hooksBefore) + " -> " + std::to_string(chain.hooks.size()) + ")");
        }
    }

    if (staleMapEntries > 0) { LOG_INFO("[HookManager] Cleaned up " + std::to_string(staleMapEntries) + " stale map entries"); }
    LOG_INFO("[HookManager] Removed " + std::to_string(removed) + " hooks for mod: " + modName);
    return removed;
}

// APPLY HOOKS - Install dispatch stubs for all unpatched chains
int HookManager::ApplyHooks() {
    int applied = 0;

    for (auto& [method, chain] : m_chains) {
        if (chain.isPatched) continue;

        // Prefer the zero-overhead baked chain when every entry opts in and the signatures line up
        if (ChainBakeEligible(chain) && TryInstallBakedChain(chain)) {
            applied++;
            continue;
        }

        // Not baked (ineligible, or compile/install failed) - demote-or-reject the typed entries
        DemoteIneligibleTypedHooks(chain);
        if (chain.hooks.empty()) continue; // nothing installable left

        if (InstallDispatchStub(chain)) { applied++; }
    }

    LOG_INFO("[HookManager] Applied " + std::to_string(applied) + " hook chains");
    return applied;
}

// PER-MOD INSTALL DIAGNOSTICS - one request per RegisterHook/RegisterHookByPointer call
void HookManager::RecordHookRequest(const std::string& modName) {
    m_modStats[modName].requested++;
}

// Deduped on (mod, target, reason) - install retries would otherwise re-record the same refusal
void HookManager::RecordHookRefusal(const std::string& modName, const std::string& target, const std::string& reason) {
    auto& refusals = m_modStats[modName].refusals;
    for (const auto& r : refusals) {
        if (r.target == target && r.reason == reason) return;
    }
    refusals.push_back({target, reason});
}

// HOOK INSTALL SUMMARY - per-mod "N requested, M installed, K REFUSED (reason)" block; the loud counterpart to the scattered per-refusal ERROR lines
void HookManager::LogHookSummary(bool onlyIfNewRefusals) {
    size_t totalRefusals = 0;
    for (const auto& [mod, stats] : m_modStats) { totalRefusals += stats.refusals.size(); }
    if (onlyIfNewRefusals && totalRefusals == m_summarizedRefusals) return;

    // Installed = entries on a patched chain (watchers ride in chain.hooks); entries stranded on another mod's refused chain count as not-installed
    std::unordered_map<std::string, int> installed;
    for (const auto& [method, chain] : m_chains) {
        if (!chain.isPatched) continue;
        for (const auto& e : chain.hooks) { installed[e.modName]++; }
    }

    LOG_INFO("[HookManager] ===== Hook install summary =====");
    for (const auto& [mod, stats] : m_modStats) {
        if (stats.requested == 0) continue;
        if (onlyIfNewRefusals && stats.refusals.empty()) continue; // delta mode: only mods with refusals

        int inst = 0;
        auto it = installed.find(mod);
        if (it != installed.end()) inst = it->second;

        std::string line = "[HookManager]   " + mod + ": " + std::to_string(stats.requested) + " hooks requested, " + std::to_string(inst) + " installed";
        if (stats.refusals.empty()) {
            LOG_INFO(line);
        } else {
            LOG_WARN(line + ", " + std::to_string(stats.refusals.size()) + " REFUSED");
            for (const auto& r : stats.refusals) { LOG_WARN("[HookManager]     REFUSED " + r.target + " - " + r.reason); }
        }
    }
    LOG_INFO("[HookManager] =================================");

    m_summarizedRefusals = totalRefusals;
}

// REINSTALL DISPATCH STUBS - Update stubs after hot-reload or chain changes
int HookManager::ReinstallDispatchStubs() {
    int reinstalled = 0;

    for (auto& [method, chain] : m_chains) {
        if (!chain.needsReinstall) continue;

        // Clear the flag first
        chain.needsReinstall = false;

        // BAKED CHAIN - recompile the linked-RTM dispatch in place (or tear down if emptied)
        if (chain.bakedActive) {
            if (chain.hooks.empty()) {
                // No hooks left - point the target back at its original RTM (or null to force a fresh transform of the original IL); identity was never clobbered, so this fully restores it
                auto* layout = reinterpret_cast<MonoMethodLayout*>(chain.originalMethod);
                DWORD oldProtect;
                if (VirtualProtect(chain.originalMethod, sizeof(MonoMethodLayout), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    auto compIt = m_compiledChains.find(chain.originalMethod);
                    if (compIt != m_compiledChains.end() && compIt->second.overwroteOriginal) {
                        // Head lives in the target's own RTM (callers bound that pointer): put the original bytes back under it
                        MethodPatcher::OverwriteRTM(reinterpret_cast<RuntimeMethod*>(compIt->second.headRTM), &compIt->second.savedOriginalRTM, chain.originalMethod);
                        layout->runtimeMethod = compIt->second.headRTM;
                    } else {
                        layout->runtimeMethod = chain.originalRuntimeMethod; // backup captured pre-patch (may be null)
                    }
                    VirtualProtect(chain.originalMethod, sizeof(MonoMethodLayout), oldProtect, &oldProtect);
                    chain.bakedActive = false;
                    chain.isPatched = false;
                    m_compiledChains.erase(chain.originalMethod);
                    reinstalled++;
                    LOG_INFO("[HookManager] Restored original method (baked chain emptied): " + chain.targetClass + "::" + chain.targetMethod);
                } else {
                    LOG_ERROR("[HookManager] Failed to restore original method (baked, VirtualProtect failed): " + chain.targetClass + "::" + chain.targetMethod);
                }
            } else if (RecompileBakedChain(chain)) {
                reinstalled++;
                LOG_INFO("[HookManager] Recompiled baked chain: " + chain.targetClass + "::" + chain.targetMethod);
            } else {
                LOG_ERROR("[HookManager] Failed to recompile baked chain: " + chain.targetClass + "::" + chain.targetMethod);
            }
            continue;
        }

        // RUNTIME-REGISTERED TYPED HOOKS - a Chain.* hook added to an already-boxed chain gets the same demote/reject pass as first install (a Chain.ByRef hook is REJECTED rather than boxed into per-call NotRewritten throws)
        DemoteIneligibleTypedHooks(chain);

        if (chain.hooks.empty()) {
            // No hooks left - restore original method fields so the game method runs its own IL again instead of dispatching through a stale stub.
            auto* layout = reinterpret_cast<MonoMethodLayout*>(chain.originalMethod);

            DWORD oldProtect;
            if (VirtualProtect(chain.originalMethod, sizeof(MonoMethodLayout), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                // Restore runtimeMethod from the trampoline.
                // Identity fields were never modified by identity-preserving patching, so this mainly clears the hook's RTM.
                // Full struct copy for safety in case of future changes.
                auto* trampoline = reinterpret_cast<MonoMethodLayout*>(chain.trampolineMethod);
                *layout = *trampoline;
                if (chain.originalRTMOverwritten) {
                    // The stub lives in the original's own RTM (callers bound that pointer): put the original bytes back under it
                    MethodPatcher::OverwriteRTM(reinterpret_cast<RuntimeMethod*>(chain.installedRTM), &chain.savedOriginalRTM, chain.originalMethod);
                    layout->runtimeMethod = chain.installedRTM;
                    chain.originalRTMOverwritten = false;
                } else {
                    layout->runtimeMethod = nullptr; // Force recompilation with original IL
                }
                VirtualProtect(chain.originalMethod, sizeof(MonoMethodLayout), oldProtect, &oldProtect);

                chain.isPatched = false;
                chain.installedRTM = nullptr; // a future re-hook installs afresh (in place again if the RTM still exists)
                reinstalled++;
                LOG_INFO("[HookManager] Restored original method: " + chain.targetClass + "::" + chain.targetMethod + " (chain emptied, no hooks remain)");
            } else {
                LOG_ERROR("[HookManager] Failed to restore original method: " + chain.targetClass + "::" + chain.targetMethod + " (VirtualProtect failed)");
            }
            continue;
        }

        // Update the dispatch stub to point to the new first hook
        void* newEntryHook = chain.hooks[0].hookMethod;

        LOG_DEBUG("[HookManager] Reinstalling dispatch stub for " + chain.targetClass + "::" + chain.targetMethod);

        // Use ForcePatchMethod to update even if already patched. Pass installedRTM so a reload oerwrites the existing dispatch RTM in place (callers cached its pointer) instead of swapping in a new RTM they'd never see
        bool deferred = false;
        RuntimeMethod saved = {};
        void* preInstallRTM = reinterpret_cast<MonoMethodLayout*>(chain.originalMethod)->runtimeMethod;
        if (MethodPatcher::Get().ForcePatchMethod(chain.originalMethod, newEntryHook, &deferred, &chain.installedRTM, &saved)) {
            // A previously-restored chain re-hooking an existing RTM is a first install again: keep the pre-overwrite bytes (a clobber-compiled RTM on an already-patched chain is NOT original bytes)
            if (!chain.isPatched && preInstallRTM && chain.installedRTM == preInstallRTM) {
                chain.savedOriginalRTM = saved;
                chain.originalRTMOverwritten = true;
            }
            chain.needsIdentityRestore = deferred;
            chain.isPatched = true;
            reinstalled++;
            LOG_INFO("[HookManager] Reinstalled dispatch stub: " + chain.targetClass + "::" + chain.targetMethod);
        } else {
            LOG_ERROR("[HookManager] Failed to reinstall dispatch stub: " + chain.targetClass + "::" + chain.targetMethod);
        }
    }

    if (reinstalled > 0) { LOG_INFO("[HookManager] Reinstalled " + std::to_string(reinstalled) + " dispatch stubs"); }
    return reinstalled;
}

// GET CHAIN - Look up a hook chain by original method pointer
HookChain* HookManager::GetChain(void* originalMethod) {
    auto it = m_chains.find(originalMethod);
    if (it != m_chains.end()) { return &it->second; }
    return nullptr;
}

HookChain* HookManager::GetChainByName(const std::string& targetClass, const std::string& targetMethod) {
    // A hot-reload empties the old chain but retains it in m_chains, so a first-match name lookup can hash-order-randomly return the stale duplicate and read dead state
    // Prefer a chain with LIVE entries (same liveness test as RebuildClassMethodIndex); fall back to a stale match only if none is live
    HookChain* stale = nullptr;
    for (auto& [method, chain] : m_chains) {
        if (chain.targetClass == targetClass && chain.targetMethod == targetMethod) {
            if (!chain.hooks.empty() || !chain.watchers.empty()) { return &chain; }
            stale = &chain;
        }
    }
    return stale;
}

// SIGNATURE PARSING AND MATCHING - Normalize user type names to Mono canonical format
static std::string NormalizeTypeName(const std::string& typeName) {
    std::string result = typeName;

    // Check for byref suffix
    bool isByRef = false;
    if (!result.empty() && result.back() == '&') {
        isByRef = true;
        result.pop_back();
    }

    // Trim whitespace
    size_t first = result.find_first_not_of(" \t");
    size_t last = result.find_last_not_of(" \t");
    if (first != std::string::npos) { result = result.substr(first, last - first + 1); }

    // Lowercase for comparison
    std::string lower = result;
    for (char& c : lower) c = (char)tolower(c);

    // Map common C# aliases to Mono's canonical names (void, bool, char, sbyte, byte, int16, uint16, int, uint, long, ulong, single, double, string, object).
    if (lower == "int" || lower == "int32" || lower == "system.int32")
        result = "int";
    else if (lower == "uint" || lower == "uint32" || lower == "system.uint32")
        result = "uint";
    else if (lower == "long" || lower == "int64" || lower == "system.int64")
        result = "long";
    else if (lower == "ulong" || lower == "uint64" || lower == "system.uint64")
        result = "ulong";
    else if (lower == "short" || lower == "int16" || lower == "system.int16")
        result = "int16";
    else if (lower == "ushort" || lower == "uint16" || lower == "system.uint16")
        result = "uint16";
    else if (lower == "byte" || lower == "system.byte")
        result = "byte";
    else if (lower == "sbyte" || lower == "system.sbyte")
        result = "sbyte";
    else if (lower == "float" || lower == "single" || lower == "system.single")
        result = "single";
    else if (lower == "double" || lower == "system.double")
        result = "double";
    else if (lower == "bool" || lower == "boolean" || lower == "system.boolean")
        result = "bool";
    else if (lower == "char" || lower == "system.char")
        result = "char";
    else if (lower == "string" || lower == "system.string")
        result = "string";
    else if (lower == "object" || lower == "system.object")
        result = "object";
    else if (lower == "void" || lower == "system.void")
        result = "void";
    else if (lower == "intptr" || lower == "system.intptr")
        result = "intptr";
    else if (lower == "uintptr" || lower == "system.uintptr")
        result = "uintptr";
    // For complex types (generics, classes), strip namespace and normalize nested type separator.
    // mono_signature_get_desc with include_namespace=0 strips namespaces and uses '/' for nested types.
    else {
        // Check for generic type: "Dictionary<KeyType, ValueType>"
        size_t angleStart = result.find('<');
        if (angleStart != std::string::npos && result.back() == '>') {
            // Extract base type and generic arguments
            std::string baseType = result.substr(0, angleStart);
            std::string genericArgs = result.substr(angleStart + 1, result.size() - angleStart - 2);

            // Normalize the base type (strip namespace, convert +)
            std::string normalizedBase = NormalizeTypeName(baseType);

            // Normalize each generic argument (recursive, respects nested <>)
            std::string normalizedArgs;
            size_t start = 0;
            int depth = 0;
            for (size_t i = 0; i < genericArgs.size(); i++) {
                char c = genericArgs[i];
                if (c == '<')
                    depth++;
                else if (c == '>')
                    depth--;
                else if (c == ',' && depth == 0) {
                    std::string arg = genericArgs.substr(start, i - start);
                    if (!normalizedArgs.empty()) normalizedArgs += ",";
                    normalizedArgs += NormalizeTypeName(arg);
                    start = i + 1;
                }
            }
            // Last argument
            std::string arg = genericArgs.substr(start);
            if (!normalizedArgs.empty()) normalizedArgs += ",";
            normalizedArgs += NormalizeTypeName(arg);

            result = normalizedBase + "<" + normalizedArgs + ">";
        } else {
            // Non-generic: strip namespace prefix, but be careful with nested types: "Ns.Outer+Inner" -> "Outer+Inner" -> "Outer/Inner".
            size_t lastDot = result.rfind('.');
            if (lastDot != std::string::npos) {
                // Check if there's a '+' after the dot (nested type with namespace)
                // e.g., "Ns.Outer+Inner" - we want "Outer/Inner", not just "Inner"
                size_t plusPos = result.find('+');
                if (plusPos != std::string::npos && plusPos > lastDot) {
                    // Nested type: strip namespace only (before the outer class)
                    result = result.substr(lastDot + 1);
                } else if (plusPos != std::string::npos && plusPos < lastDot) {
                    // Edge case: "Ns.Sub.Outer+Inner" - find the dot before the '+' part
                    size_t dotBeforePlus = result.rfind('.', plusPos);
                    if (dotBeforePlus != std::string::npos) { result = result.substr(dotBeforePlus + 1); }
                } else {
                    // Simple class with namespace: strip it
                    result = result.substr(lastDot + 1);
                }
            }

            // Convert C# nested type separator '+' to Mono's '/'
            for (char& c : result) {
                if (c == '+') c = '/';
            }
        }
    }

    // Re-add byref suffix
    if (isByRef) { result += "&"; }

    return result;
}

// Normalize a full signature string "(Type1, Type2)" to Mono format "Type1,Type2"
static std::string NormalizeSignature(const std::string& signature) {
    if (signature.empty()) return "";

    // Remove parentheses
    std::string inner = signature;
    if (inner.front() == '(') inner = inner.substr(1);
    if (!inner.empty() && inner.back() == ')') inner.pop_back();

    if (inner.empty()) return ""; // "()" means zero params

    // Split by comma but respect generic angle brackets
    std::string result;
    size_t start = 0;
    int angleBracketDepth = 0;

    for (size_t i = 0; i < inner.size(); i++) {
        char c = inner[i];
        if (c == '<') {
            angleBracketDepth++;
        } else if (c == '>') {
            angleBracketDepth--;
        } else if (c == ',' && angleBracketDepth == 0) {
            // Split here - at top level comma
            std::string param = inner.substr(start, i - start);
            if (!result.empty()) result += ",";
            result += NormalizeTypeName(param);
            start = i + 1;
        }
    }

    // Last parameter
    std::string param = inner.substr(start);
    if (!result.empty()) result += ",";
    result += NormalizeTypeName(param);

    return result;
}

// Parse "(Type1,Type2)" into vector of type names
std::vector<std::string> HookManager::ParseSignature(const std::string& signature) {
    std::vector<std::string> result;
    if (signature.empty()) return result;

    // Remove parentheses
    std::string inner = signature;
    if (inner.front() == '(') inner = inner.substr(1);
    if (!inner.empty() && inner.back() == ')') inner.pop_back();

    if (inner.empty()) return result; // "()" means zero params

    // Split by comma
    size_t start = 0;
    size_t pos = 0;
    while ((pos = inner.find(',', start)) != std::string::npos) {
        std::string param = inner.substr(start, pos - start);
        // Trim whitespace
        size_t first = param.find_first_not_of(" \t");
        size_t last = param.find_last_not_of(" \t");
        if (first != std::string::npos) { result.push_back(param.substr(first, last - first + 1)); }
        start = pos + 1;
    }
    // Last parameter
    std::string param = inner.substr(start);
    size_t first = param.find_first_not_of(" \t");
    size_t last = param.find_last_not_of(" \t");
    if (first != std::string::npos) { result.push_back(param.substr(first, last - first + 1)); }

    return result;
}

// Convert type name string to MonoTypeCode via NormalizeTypeName then canonical mapping
MonoTypeCode HookManager::TypeNameToCode(const std::string& typeName) {
    // Normalize to Mono canonical form (handles all aliases: int32->int, float->single, etc.)
    std::string normalized = NormalizeTypeName(typeName);

    // Strip byref suffix for type code matching
    if (!normalized.empty() && normalized.back() == '&') { normalized.pop_back(); }

    // Map Mono canonical names to type codes
    if (normalized == "int") return MonoTypeCode::I4;
    if (normalized == "uint") return MonoTypeCode::U4;
    if (normalized == "long") return MonoTypeCode::I8;
    if (normalized == "ulong") return MonoTypeCode::U8;
    if (normalized == "int16") return MonoTypeCode::I2;
    if (normalized == "uint16") return MonoTypeCode::U2;
    if (normalized == "byte") return MonoTypeCode::U1;
    if (normalized == "sbyte") return MonoTypeCode::I1;
    if (normalized == "single") return MonoTypeCode::R4;
    if (normalized == "double") return MonoTypeCode::R8;
    if (normalized == "bool") return MonoTypeCode::Boolean;
    if (normalized == "char") return MonoTypeCode::Char;
    if (normalized == "string") return MonoTypeCode::String;
    if (normalized == "object") return MonoTypeCode::Object;
    if (normalized == "void") return MonoTypeCode::Void;

    // Array types
    if (normalized.size() > 2 && normalized.substr(normalized.size() - 2) == "[]") return MonoTypeCode::SzArray;

    // Unknown, could be a class type
    return MonoTypeCode::Class;
}

// Check if method signature matches expected param types
bool HookManager::MethodMatchesSignature(void* method, const std::vector<std::string>& paramTypes) {
    if (!method) return false;

    void* sig = MonoBridge::Get().GetMethodSignature(method);
    if (!sig) return paramTypes.empty();

    // Get param count from signature
    uint16_t paramCount = *(uint16_t*)((uintptr_t)sig + Offsets::SIG_PARAM_COUNT);

    // Quick check: param count must match
    if (paramCount != paramTypes.size()) { return false; }

    // If no params expected and method has no params, it's a match
    if (paramTypes.empty()) { return true; }

    // Check each parameter type
    for (size_t i = 0; i < paramTypes.size(); i++) {
        void* paramType = *(void**)((uintptr_t)sig + Offsets::SIG_PARAMS + i * sizeof(void*));
        if (!paramType) return false;

        MonoTypeCode actualCode = (MonoTypeCode)(*(uint8_t*)((uintptr_t)paramType + Offsets::TYPE_CODE));
        MonoTypeCode expectedCode = TypeNameToCode(paramTypes[i]);

        // For Class type, we need deeper comparison (check class name)
        if (expectedCode == MonoTypeCode::Class && actualCode == MonoTypeCode::Class) {
            // Get the class from the MonoType
            void* paramKlass = *(void**)((uintptr_t)paramType);
            if (paramKlass) {
                const char* paramClassName = MonoBridge::Get().GetClassName(paramKlass);
                const char* paramNs = MonoBridge::Get().GetClassNamespace(paramKlass);

                // Build full name for comparison
                std::string fullName;
                if (paramNs && paramNs[0]) {
                    fullName = std::string(paramNs) + "." + (paramClassName ? paramClassName : "");
                } else {
                    fullName = paramClassName ? paramClassName : "";
                }

                // Compare (case-insensitive for flexibility)
                std::string expectedLower = paramTypes[i];
                std::string actualLower = fullName;
                for (char& c : expectedLower) c = (char)tolower(c);
                for (char& c : actualLower) c = (char)tolower(c);

                // Check if expected matches (could be short name or full name)
                if (actualLower != expectedLower) {
                    // Try short name only
                    std::string shortName = paramClassName ? paramClassName : "";
                    for (char& c : shortName) c = (char)tolower(c);
                    if (shortName != expectedLower) { return false; }
                }
            }
            continue;
        }

        // For ValueType, also check class name
        if (expectedCode == MonoTypeCode::Class && actualCode == MonoTypeCode::ValueType) {
            void* paramKlass = *(void**)((uintptr_t)paramType);
            if (paramKlass) {
                const char* paramClassName = MonoBridge::Get().GetClassName(paramKlass);
                std::string shortName = paramClassName ? paramClassName : "";
                std::string expectedLower = paramTypes[i];
                for (char& c : shortName) c = (char)tolower(c);
                for (char& c : expectedLower) c = (char)tolower(c);
                if (shortName != expectedLower) { return false; }
            }
            continue;
        }

        // For primitive types, direct comparison
        if (actualCode != expectedCode) { return false; }
    }

    return true;
}

// FIND TARGET METHOD - Resolve a class/method/signature to a MonoMethod*
void* HookManager::FindTargetMethod(const std::string& targetClass, const std::string& targetMethod, const std::string& signature, void* preferImage) {
    // Parse class name into namespace and class, handling nested types with '+' separator
    size_t lastDot = targetClass.rfind('.');
    std::string ns = (lastDot != std::string::npos) ? targetClass.substr(0, lastDot) : "";
    std::string className = (lastDot != std::string::npos) ? targetClass.substr(lastDot + 1) : targetClass;

    // Check for nested class notation ('+' separator)
    void* klass = nullptr;
    size_t plusPos = className.find('+');
    if (plusPos != std::string::npos) {
        // Nested class: split into outer and nested parts
        std::string outerClassName = className.substr(0, plusPos);
        std::string nestedClassName = className.substr(plusPos + 1);

        // Handle multi-level nesting (e.g., Outer+Middle+Inner)
        // First resolve the outer class
        void* outerKlass = MonoBridge::Get().GetClassByName(ns.c_str(), outerClassName.c_str());
        if (!outerKlass) {
            LOG_ERROR("[HookManager] Outer class not found: " + ns + "." + outerClassName);
            return nullptr;
        }

        // Now resolve each nested level
        klass = outerKlass;
        size_t nestedStart = 0;
        while (nestedStart < nestedClassName.length()) {
            size_t nextPlus = nestedClassName.find('+', nestedStart);
            std::string currentNested;
            if (nextPlus != std::string::npos) {
                currentNested = nestedClassName.substr(nestedStart, nextPlus - nestedStart);
                nestedStart = nextPlus + 1;
            } else {
                currentNested = nestedClassName.substr(nestedStart);
                nestedStart = nestedClassName.length();
            }

            klass = MonoBridge::Get().GetNestedClass(klass, currentNested.c_str());
            if (!klass) {
                LOG_ERROR("[HookManager] Nested class not found: " + currentNested + " in " + targetClass);
                return nullptr;
            }
        }

        LOG_DEBUG("[HookManager] Resolved nested class: " + targetClass);
    } else {
        // Regular (non-nested) class lookup
        klass = MonoBridge::Get().GetClassByName(ns.c_str(), className.c_str());
    }

    if (!klass && preferImage) {
        // ScriptHost::FindClass only searches game assemblies; for a target in a mod assembly (self-targeting / mod-to-mod) fall back to the registering hook's own image
        klass = MonoBridge::Get().GetClassFromNameInImage(preferImage, ns.c_str(), className.c_str());
        if (klass) LOG_DEBUG("[HookManager] Resolved '" + targetClass + "' via hook-image fallback");
    }

    if (!klass) {
        LOG_ERROR("[HookManager] Class not found: " + targetClass);
        return nullptr;
    }

    // Normalize signature if provided for string-based matching via mono_signature_get_desc (handles generics, byref params, nested types, and primitive aliases).
    bool hasSignature = !signature.empty();
    std::string normalizedExpected;
    if (hasSignature) {
        normalizedExpected = NormalizeSignature(signature);
        LOG_DEBUG("[HookManager] Looking for signature: " + normalizedExpected);
    }

    // Find the method, if signature provided match against it
    void* firstMatch = nullptr;
    int matchCount = 0;

    int iter = 0;
    while (void* method = MonoBridge::Get().GetNextMethod(klass, &iter)) {
        auto* layout = reinterpret_cast<MonoMethodLayout*>(method);
        if (!layout->name || strcmp(layout->name, targetMethod.c_str()) != 0) { continue; }

        // Found a method with matching name
        matchCount++;

        // Get canonical signature from Mono
        void* sig = MonoBridge::Get().GetMethodSignature(method);
        std::string canonicalSig;
        if (sig) {
            canonicalSig = MonoBridge::Get().GetSignatureDescription(sig, false);
            LOG_DEBUG("[HookManager] Overload " + std::to_string(matchCount) + ": " + targetMethod + "(" + canonicalSig + ")");
        }

        if (!hasSignature) {
            // No signature specified, return first match
            return method;
        }

        // String-based signature matching
        // Compare normalized user signature against Mono's canonical signature
        if (canonicalSig == normalizedExpected) {
            LOG_DEBUG("[HookManager] Found matching overload (string match)");
            return method;
        }

        // Fallback: try the type-code-based matching for simple cases where the user wrote "int,int" and Mono returns "int,int"
        std::vector<std::string> expectedParams = ParseSignature(signature);
        if (MethodMatchesSignature(method, expectedParams)) {
            LOG_DEBUG("[HookManager] Found matching overload (type-code match)");
            return method;
        }

        // Keep first match as fallback info
        if (!firstMatch) firstMatch = method;
    }

    if (hasSignature && matchCount > 0) {
        LOG_ERROR("[HookManager] Method " + targetClass + ":" + targetMethod + " found (" + std::to_string(matchCount) + " overload(s)) but none match signature " + signature);
        LOG_ERROR("[HookManager]   Expected (normalized): " + normalizedExpected);
    } else {
        LOG_ERROR("[HookManager] Method not found: " + targetClass + ":" + targetMethod);
    }
    return nullptr;
}

// GENERIC-TARGET SAFETY - see hook_manager.h
// INFLATED targets are SUPPORTED: CloneMethod copies the full 0x30-byte MonoMethodInflated (context @+0x28, declaring @+0x2C), so the forced re-transform resolves like the original's
// Refused: open generic method definitions (no context to inflate with), inflated methods with unreadable context/declaring, and the corrupt generic_class-without-is_inflated shape
std::string HookManager::DescribeTrampolineHazard(void* method) {
    if (!method || !IsReadablePointer(method, sizeof(MonoMethodLayout))) return "method pointer is not readable";

    auto* layout = reinterpret_cast<MonoMethodLayout*>(method);

    // Open generic method definition - no context to inflate with
    // Checked before the inflated probe: a MethodSpec instantiation of a generic METHOD has both container-on-declaring and is_inflated
    if (!(layout->slotAndFlags & Offsets::METHOD_IS_INFLATED_FLAG) && layout->genericContainer) {
        return "it is an open generic method definition (generic_container set at +0x10); "
               "hook a constructed instantiation instead";
    }

    if (layout->slotAndFlags & Offsets::METHOD_IS_INFLATED_FLAG) {
        // Inflated method - trampolinable via the full-size copy, provided the extension is intact
        if (!IsReadablePointer(method, sizeof(MonoMethodInflatedLayout))) { return "it is an inflated generic method but the 0x30-byte MonoMethodInflated span is not readable"; }
        auto* inflated = reinterpret_cast<MonoMethodInflatedLayout*>(method);
        if (!inflated->declaring || !IsReadablePointer(inflated->declaring, sizeof(MonoMethodLayout))) { return "it is an inflated generic method with an unreadable declaring method (+0x2C)"; }
        // context is never null on a runtime-built inflated method (the inflation path always allocates it);
        // transform_method hands it to token resolution unconditionally when the sig's inflated bit is set
        if (!inflated->context || !IsReadablePointer(inflated->context, 8)) { return "it is an inflated generic method with an unreadable generic context (+0x28)"; }
        return {};
    }

    // Contradictory shape: constructed-generic declaring class but is_inflated clear
    if (layout->klass && IsReadablePointer(layout->klass, Offsets::CLASS_GENERIC_CLASS + sizeof(void*))) {
        void* genericClass = *(void**)((uintptr_t)layout->klass + Offsets::CLASS_GENERIC_CLASS);
        if (genericClass) {
            return "its declaring type is a generic instantiation (klass->generic_class set at +0xA8) but "
                   "is_inflated is clear - the loader never produces this shape, refusing as corrupt";
        }
    }

    return {};
}

// BACKUP ORIGINAL METHOD - Snapshot original MonoMethod fields and create trampoline
void HookManager::BackupOriginalMethod(HookChain& chain) {
    auto* layout = reinterpret_cast<MonoMethodLayout*>(chain.originalMethod);

    chain.originalKlass = layout->klass;
    chain.originalToken = layout->token;
    chain.originalRuntimeMethod = layout->runtimeMethod;

    // Create a heap-allocated trampoline copy of the MonoMethod that retains the original values and can be used to invoke the original.
    // CloneMethod is SIZE-AWARE: an inflated target is copied as the full 0x30-byte MonoMethodInflated, so context (+0x28) / declaring (+0x2C) survive the re-transform
    // It also clears runtimeMethod: the original RTM's ->method back-pointer leads to the patched identity after install, so the trampoline compiles fresh with its own
    void* trampoline = ChainCompiler::CloneMethod(chain.originalMethod);
    if (!trampoline) {
        // Registration already ran DescribeTrampolineHazard, so this is an unexpected readability failure
        LOG_ERROR("[HookManager] BackupOriginalMethod: clone failed for token=" + std::to_string(chain.originalToken) + " - chain left without a trampoline (CallNext to the original will fail)");
        return;
    }

    chain.trampolineMethod = trampoline;
    m_trampolineToOriginal[trampoline] = chain.originalMethod;

    bool inflated = (layout->slotAndFlags & Offsets::METHOD_IS_INFLATED_FLAG) != 0;
    LOG_DEBUG("[HookManager] Backed up original method: token=" + std::to_string(chain.originalToken) + ", trampoline=" + std::to_string((uintptr_t)trampoline) +
              (inflated ? " (inflated - full 0x30 copy w/ context+declaring)" : "") + " (runtimeMethod cleared for fresh compilation)");

    // Determine if original is instance method (check STATIC flag)
    auto* origLayout = reinterpret_cast<MonoMethodLayout*>(chain.originalMethod);
    chain.originalIsInstance = (origLayout->flagsPacked & MethodFlags::STATIC) == 0;
    LOG_DEBUG("[HookManager] Original method isInstance=" + std::to_string(chain.originalIsInstance));

    // Cache signature info for faster dispatch
    CacheSignatureInfo(chain);
}

// CACHE SIGNATURE INFO - Pre-compute signature details to avoid re-reading every call
void HookManager::CacheSignatureInfo(HookChain& chain) {
    if (chain.signatureCached) return;

    // Resolve via the bridge, NOT the raw layout->signature field: Mono populates method->signature LAZILY on first mono_method_signature() call
    // A freshly-compiled target that gameplay never invoked still has a null signature field, and a raw read would latch returnIsVoid=true and silently drop the return value
    // GetMethodSignature forces the parse
    void* sig = MonoBridge::Get().GetMethodSignature(chain.originalMethod);

    if (!sig) {
        chain.signatureCached = true;
        chain.returnIsVoid = true;
        return;
    }

    // Cache return type info
    void* retType = *(void**)((uintptr_t)sig + Offsets::SIG_RET_TYPE);
    if (retType) {
        chain.cachedReturnType = (MonoTypeCode)(*(uint8_t*)((uintptr_t)retType + Offsets::TYPE_CODE));
        chain.returnIsVoid = (chain.cachedReturnType == MonoTypeCode::Void);
        chain.returnIsPrimitive = IsPrimitiveType(chain.cachedReturnType);

        // Cache return klass for value type boxing
        if (!chain.returnIsVoid && !chain.returnIsPrimitive) { chain.cachedReturnKlass = *(void**)((uintptr_t)retType); }
    } else {
        chain.returnIsVoid = true;
    }

    // Cache param count and types
    chain.cachedParamCount = *(uint16_t*)((uintptr_t)sig + Offsets::SIG_PARAM_COUNT);
    chain.cachedParamTypes.resize(chain.cachedParamCount);
    chain.cachedParamIsValueType.resize(chain.cachedParamCount);
    chain.cachedParamIsByRef.resize(chain.cachedParamCount);

    for (uint16_t i = 0; i < chain.cachedParamCount; i++) {
        void* paramType = *(void**)((uintptr_t)sig + Offsets::SIG_PARAMS + i * sizeof(void*));
        if (paramType) {
            MonoTypeCode code = (MonoTypeCode)(*(uint8_t*)((uintptr_t)paramType + Offsets::TYPE_CODE));
            chain.cachedParamTypes[i] = code;
            // Value types need unboxing (primitives and ValueType structs)
            chain.cachedParamIsValueType[i] = IsPrimitiveType(code) || code == MonoTypeCode::ValueType;
            // By-ref (ref/out) positions - the ONLY positions where UnboxParametersFast may treat a single-element array as a Hook.Ref wrapper (same +0x07 & 0x40 read as SignatureInfo)
            chain.cachedParamIsByRef[i] = ChainCompiler::TypeIsByRef(paramType);
        } else {
            chain.cachedParamTypes[i] = MonoTypeCode::Object;
            chain.cachedParamIsValueType[i] = false;
            chain.cachedParamIsByRef[i] = false;
        }
    }

    chain.signatureCached = true;
    LOG_DEBUG("[HookManager] Cached signature: returnType=" + std::to_string((int)chain.cachedReturnType) + ", params=" + std::to_string(chain.cachedParamCount) +
              ", isPrimitive=" + std::to_string(chain.returnIsPrimitive) + ", isVoid=" + std::to_string(chain.returnIsVoid));
}

// ENTRY-HOOK ABI GUARD - the entry hook (hooks[0]) is compiled UNDER THE TARGET'S SIGNATURE, so its declared C# shape IS the ABI (ldarg.N typed by the target's params, `ret` writes the caller's return slot)
// Failure mode: an `object`-declared hook over an enum/struct-returning target stores a boxed-object POINTER into the caller's value slot - every caller sees a ~0x4xxxxxxx "enum"; params corrupt the same way in reverse
// Interior hooks (position > 0) are exempt - they're runtime-invoked with boxed args, so loosely-typed catch-all handlers stay legal there
// Compatibility is SLOT-SHAPE, deliberately weaker than SignatureInfo::ExactMatch:
//   - reference kinds interchange freely (an `object` return over a `Sim` target is one O slot either way)
//   - value types (and enums), by-ref positions, and generic instantiations must match IDENTICALLY (slot encoding depends on the exact type)
// Returns "" when compatible, else a one-line reason for the install-refusal ERROR
static bool IsRefSlotCode(MonoTypeCode c) {
    return c == MonoTypeCode::String || c == MonoTypeCode::Class || c == MonoTypeCode::Object || c == MonoTypeCode::SzArray;
}

static std::string AggregateTypeName(MonoTypeCode code, void* data); // defined with the bake diagnostics below

static std::string DescribeEntryAbiMismatch(const SignatureInfo& target, const SignatureInfo& hook) {
    if (!target.valid) return "target signature could not be read";
    if (!hook.valid) return "entry hook signature could not be read";

    // Return slot
    bool retOk;
    if (target.ret == MonoTypeCode::Void)
        retOk = (hook.ret == MonoTypeCode::Void);
    else if (IsRefSlotCode(target.ret) && IsRefSlotCode(hook.ret))
        retOk = true; // any ref-to-ref
    else
        retOk = (hook.ret == target.ret && hook.retTypeData == target.retTypeData);
    if (!retOk) {
        return "return ABI mismatch: target returns " + AggregateTypeName(target.ret, target.retTypeData) + " (type-code " + std::to_string((int)target.ret) + ", a value slot) but the entry hook declares " +
               AggregateTypeName(hook.ret, hook.retTypeData) + " (type-code " + std::to_string((int)hook.ret) + ") - the hook's boxed return pointer would land raw in the caller's value slot. " +
               "Declare the target's EXACT return type on the hook";
    }

    // Param window: same-shape, or the leading-self convention for instance targets
    uint16_t selfShift = 0;
    if (target.hasThis && !hook.hasThis) {
        if (hook.paramCount != (uint16_t)(target.paramCount + 1))
            return "instance target: entry hook must declare a leading receiver param plus the target's " + std::to_string(target.paramCount) + " param(s), but declares " + std::to_string(hook.paramCount);
        if (hook.paramCount == 0 || hook.paramIsByRef[0] || hook.paramIsValueType[0]) return "instance target: entry hook's leading receiver param must be a plain reference type";
        selfShift = 1;
    } else if (hook.hasThis != target.hasThis) {
        return std::string("static/instance mismatch: target is ") + (target.hasThis ? "instance" : "static") + " but the entry hook is " + (hook.hasThis ? "an instance method" : "static");
    } else if (hook.paramCount != target.paramCount) {
        return "parameter count differs: target " + std::to_string(target.paramCount) + " vs entry hook " + std::to_string(hook.paramCount);
    }

    for (uint16_t i = 0; i < target.paramCount; ++i) {
        uint16_t h = (uint16_t)(i + selfShift);
        if (hook.paramIsByRef[h] != target.paramIsByRef[i]) return "param[" + std::to_string(i) + "] by-ref mismatch (byref positions are managed-pointer slots; both sides must agree)";
        bool ok;
        if (target.paramIsByRef[i])
            ok = (hook.paramTypes[h] == target.paramTypes[i] && hook.paramTypeData[h] == target.paramTypeData[i]); // byref: pointee must match exactly
        else if (IsRefSlotCode(target.paramTypes[i]) && IsRefSlotCode(hook.paramTypes[h]))
            ok = true;
        else
            ok = (hook.paramTypes[h] == target.paramTypes[i] && hook.paramTypeData[h] == target.paramTypeData[i]);
        if (!ok) {
            return "param[" + std::to_string(i) + "] ABI mismatch: target takes " + AggregateTypeName(target.paramTypes[i], target.paramTypeData[i]) + " (type-code " + std::to_string((int)target.paramTypes[i]) +
                   ") but the entry hook declares " + AggregateTypeName(hook.paramTypes[h], hook.paramTypeData[h]) + " (type-code " + std::to_string((int)hook.paramTypes[h]) + ")";
        }
    }

    return {};
}

// INSTALL DISPATCH STUB - Patch original method to execute the first hook in chain
bool HookManager::InstallDispatchStub(HookChain& chain) {
    // Identity-preserving: ForcePatchMethod compiles hook IL via a heap proxy, then transplants the resulting RTM onto the original MonoMethod.
    // The original's token/klass/name/signature are NEVER modified, so external code (stack traces, reflection, ErrorTrap) sees the original EA identity.
    //
    // For instance methods, the compile proxy uses the original's signature/flagsPacked (hasthis=1) so MINT maps ldarg.0 to this, ldarg.1 to param[0], etc.
    // Result: 'this' arrives as the hook's first declared C# parameter.

    if (chain.hooks.empty()) {
        LOG_WARN("[HookManager] No hooks in chain for: " + chain.targetClass + "::" + chain.targetMethod);
        return false;
    }

    // Use the first (highest priority) hook as entry point
    void* entryHook = chain.hooks[0].hookMethod;
    LOG_DEBUG("[HookManager] Using hook as entry point");

    // ENTRY-HOOK ABI GUARD (see DescribeEntryAbiMismatch) - refuse the install rather than let a mismatched entry hook write pointer-shaped garbage into callers' value slots
    // Target signature is read from the TRAMPOLINE copy (pristine after a prior clobber-path patch); generated dispatch wrappers match by construction and skip the check
    if (!chain.hooks[0].hasWrapper) {
        SignatureInfo targetSig = ChainCompiler::ReadSignature(chain.trampolineMethod ? chain.trampolineMethod : chain.originalMethod);
        SignatureInfo entrySig = ChainCompiler::ReadSignature(entryHook);
        if (std::string abi = DescribeEntryAbiMismatch(targetSig, entrySig); !abi.empty()) {
            LOG_ERROR("[HookManager] REFUSING to install hook chain on " + chain.targetClass + "::" + chain.targetMethod + " - entry hook from mod '" + chain.hooks[0].modName + "' is ABI-incompatible: " + abi +
                      ". The entry hook is compiled under the target's signature, so its declared parameter/return types are the call ABI. " +
                      "The target stays UNHOOKED until the hook's declaration matches (loosely-typed handlers are only legal at lower priority behind a shape-matched entry hook).");
            RecordHookRefusal(chain.hooks[0].modName, chain.targetClass + ":" + chain.targetMethod, "entry hook ABI-incompatible: " + abi);
            return false;
        }
    }

    // ForcePatchMethod compiles hook IL via a heap proxy and sets the fresh RTM on the original.
    // Sidesteps the can't-re-transform-a-transformed-method problem and preserves the original's identity fields.
    // If proxy compilation fails (never-compiled method), falls back to clobbering token/klass for lazy first-call compilation. deferred identity restoration happens in NativeGetCallerHookToken.
    bool deferred = false;
    RuntimeMethod saved = {};
    void* preInstallRTM = reinterpret_cast<MonoMethodLayout*>(chain.originalMethod)->runtimeMethod;
    if (!MethodPatcher::Get().ForcePatchMethod(chain.originalMethod, entryHook, &deferred, &chain.installedRTM, &saved)) {
        LOG_ERROR("[HookManager] Failed to patch method: " + chain.targetClass + "::" + chain.targetMethod);
        return false;
    }
    // In-place install on the original's own RTM: keep the pre-overwrite bytes for unhook
    if (!chain.isPatched && preInstallRTM && chain.installedRTM == preInstallRTM) {
        chain.savedOriginalRTM = saved;
        chain.originalRTMOverwritten = true;
    }

    chain.isPatched = true;
    chain.needsIdentityRestore = deferred;
    LOG_INFO("[HookManager] Installed dispatch stub: " + chain.targetClass + "::" + chain.targetMethod);

    return true;
}

// BAKED CHAIN PATH - zero-overhead dispatch via a linked list of RuntimeMethods (ChainCompiler), downfall of my sanity, commence
// DIAG - dump a SignatureInfo field-by-field (used when a typed hook is rejected by ExactMatch)
static std::string DumpSig(const char* tag, const SignatureInfo& s) {
    std::string out = std::string(tag) + ": valid=" + std::to_string(s.valid) + " hasThis=" + std::to_string(s.hasThis) + " ret=" + std::to_string((int)s.ret) + " retVT=" + std::to_string(s.retIsValueType) +
                      " paramCount=" + std::to_string(s.paramCount) + " [";
    for (uint16_t i = 0; i < s.paramCount; ++i) { out += "{t=" + std::to_string((int)s.paramTypes[i]) + " byref=" + std::to_string((int)s.paramIsByRef[i]) + " vt=" + std::to_string((int)s.paramIsValueType[i]) + "}"; }
    out += "]";
    return out;
}

// DIAG - render an aggregate identity word (SignatureInfo::paramTypeData) for a reject message: class name for Class/ValueType/SzArray, raw pointer for GenericInst (no direct name to read)
static std::string AggregateTypeName(MonoTypeCode code, void* data) {
    if (!data) return "?";
    if (code == MonoTypeCode::Class || code == MonoTypeCode::ValueType || code == MonoTypeCode::SzArray) {
        const char* n = MonoBridge::GetClassName(data);
        if (n) return std::string(n) + (code == MonoTypeCode::SzArray ? "[]" : "");
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%p", data);
    return buf;
}

// DIAG - one-line description of the FIRST structural difference between a same-shape (static) hook and the target, so a reject names exactly what to fix
// Only reached on the static same-shape branch (the instance leading-self case is classified before this)
static std::string DescribeSigMismatch(const SignatureInfo& target, const SignatureInfo& hook) {
    if (!hook.valid) return "handler signature could not be read";
    if (hook.ret != target.ret || hook.retIsValueType != target.retIsValueType) return "return type differs (target type-code " + std::to_string((int)target.ret) + " vs handler " + std::to_string((int)hook.ret) + ")";
    if (hook.retTypeData != target.retTypeData)
        return "return type differs (same type-code, different class/instantiation: target " + AggregateTypeName(target.ret, target.retTypeData) + " vs handler " + AggregateTypeName(hook.ret, hook.retTypeData) + ")";
    if (hook.hasThis != target.hasThis) return std::string("static/instance mismatch (target is ") + (target.hasThis ? "instance" : "static") + ", handler is " + (hook.hasThis ? "instance" : "static") + ")";
    if (hook.paramCount != target.paramCount) return "parameter count differs (target " + std::to_string(target.paramCount) + " vs handler " + std::to_string(hook.paramCount) + ")";
    for (uint16_t i = 0; i < target.paramCount; ++i) {
        if (hook.paramTypes[i] != target.paramTypes[i])
            return "param[" + std::to_string(i) + "] type differs (target type-code " + std::to_string((int)target.paramTypes[i]) + " vs handler " + std::to_string((int)hook.paramTypes[i]) + ")";
        if (hook.paramIsByRef[i] != target.paramIsByRef[i])
            return "param[" + std::to_string(i) + "] by-ref differs (target " + std::to_string((int)target.paramIsByRef[i]) + " vs handler " + std::to_string((int)hook.paramIsByRef[i]) + ")";
        if (hook.paramIsValueType[i] != target.paramIsValueType[i])
            return "param[" + std::to_string(i) + "] value-type differs (target " + std::to_string((int)target.paramIsValueType[i]) + " vs handler " + std::to_string((int)hook.paramIsValueType[i]) + ")";
        if (hook.paramTypeData[i] != target.paramTypeData[i])
            return "param[" + std::to_string(i) + "] class differs (target " + AggregateTypeName(target.paramTypes[i], target.paramTypeData[i]) + " vs handler " +
                   AggregateTypeName(hook.paramTypes[i], hook.paramTypeData[i]) + ")";
    }
    return "signatures differ but no single field could be isolated";
}

// CRASH QUARANTINE - persisted at S3MM/baked_quarantine.txt, see header

static std::wstring BakeQuarantineFilePath() {
    return ConfigPaths::GetS3MMDirectory() + L"baked_quarantine.txt";
}

std::string HookManager::BakeQuarantineKey(const HookChain& chain) {
    // Signature-qualified so two hooked OVERLOADS don't share strikes (a bare Class::Method key let one crashing overload quarantine its siblings)
    // chain.signature is the "(int,int)" disambiguator when registration provided one; "" degrades to a bare Class::Method key
    return chain.targetClass + "::" + chain.targetMethod + chain.signature;
}

void HookManager::EnsureBakeQuarantineLoaded() {
    if (m_quarantineLoaded) return;
    m_quarantineLoaded = true; // mark first: a read failure shouldn't retry every chain

    std::ifstream in(BakeQuarantineFilePath().c_str());
    if (!in) return; // no file => nothing quarantined (the common case)

    int refused = 0;
    std::string line;
    while (std::getline(in, line)) {
        // Strip an inline " # reason" comment and trim trailing whitespace/CR
        size_t hash = line.find(" #");
        if (hash != std::string::npos) line.erase(hash);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r' || line.back() == '\n')) line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue; // blank or whole-line comment
        // Append-only: each line is one attributed crash, so a repeated key accumulates its count
        int count = ++m_bakeQuarantine[line.substr(start)];
        if (count == kBakeQuarantineThreshold) ++refused; // crossed the threshold on this entry
    }
    if (refused > 0) {
        LOG_WARN("[HookManager] " + std::to_string(refused) + " baked target(s) reached the crash-quarantine threshold (" + std::to_string(kBakeQuarantineThreshold) +
                 " strikes); they will NOT bake this run (typed hooks run boxed instead). Delete baked_quarantine.txt to retry.");
    } else if (!m_bakeQuarantine.empty()) {
        LOG_INFO("[HookManager] " + std::to_string(m_bakeQuarantine.size()) + " baked target(s) have prior crash strikes (below the quarantine threshold); still baking this run");
    }
}

bool HookManager::IsBakeQuarantined(const HookChain& chain) {
    EnsureBakeQuarantineLoaded();
    if (m_bakeQuarantine.empty()) return false;
    auto it = m_bakeQuarantine.find(BakeQuarantineKey(chain));
    return it != m_bakeQuarantine.end() && it->second >= kBakeQuarantineThreshold;
}

static std::wstring BakedCrashLogPath() {
    return ConfigPaths::GetS3MMDirectory() + L"baked_crashes.log";
}

// CRASH-TIME FILE WRITES (post-AV / VEH context) - allocation-free and lock-free end to end
// Handles are pre-opened in a HEALTHY context (the first baked-chain install); records are snprintf-formatted into fixed stack buffers and written with a single WriteFile
// No CRT heap and no stdio locks, so a crash that fired while the faulting thread HELD the CRT heap lock can't deadlock the logging
static HANDLE s_quarantineFile = INVALID_HANDLE_VALUE;
static HANDLE s_crashLogFile = INVALID_HANDLE_VALUE;

static HANDLE OpenAppendHandle(const std::wstring& path) {
    // FILE_APPEND_DATA => every write lands at EOF (append-only logs)
    return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Called from a healthy context when the first baked chain goes live, so the VEH path never has to CreateFile
static void EnsureCrashLogHandles() {
    if (s_quarantineFile == INVALID_HANDLE_VALUE) s_quarantineFile = OpenAppendHandle(BakeQuarantineFilePath());
    if (s_crashLogFile == INVALID_HANDLE_VALUE) s_crashLogFile = OpenAppendHandle(BakedCrashLogPath());
}

// One allocation-free append; false when the handle was never opened (no baked chain ever went live, or the open failed)
static bool CrashAppend(HANDLE h, const char* text, size_t len) {
    if (h == INVALID_HANDLE_VALUE || !text || len == 0) return false;
    DWORD written = 0;
    return WriteFile(h, text, (DWORD)len, &written, nullptr) && written == len;
}

bool HookManager::QuarantineBakedChain(void* method, const char* reason) {
    auto* chain = GetChain(method);
    if (!chain || !chain->bakedActive) return false; // not an active baked target -> not the culprit frame

    // POST-AV CONTEXT - the key is formatted straight into a stack buffer (exact "Class::Method(sig)" layout the loader parses); .c_str() only READS strings built in a healthy context
    char key[512];
    _snprintf_s(key, sizeof(key), _TRUNCATE, "%s::%s%s", chain->targetClass.c_str(), chain->targetMethod.c_str(), chain->signature.c_str());
    key[sizeof(key) - 1] = '\0';

    // APPEND one crash record - a single allocation-free WriteFile; the per-key count is tallied from the file on next launch (the in-memory map is NOT updated - a std::string key allocates, and this process is on its way down)
    char line[768];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s%s\n", key, (reason && reason[0]) ? " # " : "", (reason && reason[0]) ? reason : "");
    size_t len = (n >= 0) ? (size_t)n : sizeof(line) - 1;
    if (!CrashAppend(s_quarantineFile, line, len)) { LOG_ERROR("[HookManager] Could not record a quarantine strike for " + std::string(key) + " (crash-log handle not open)"); }

    LOG_ERROR("[HookManager] Crash attributed to baked chain: " + std::string(key) + (reason ? std::string(" (") + reason + ")" : "") + ". Strike recorded; the bake is refused once it reaches " +
              std::to_string(kBakeQuarantineThreshold) + " strike(s).");
    return true;
}

// BAKED CRASH FORENSICS - typed record of a baked-chain fault (see header)
// Everything below runs in a post-AV context under the VEH caller's __try: readability-guard every deref; file output ONLY via the allocation-free CrashAppend path

// "Namespace.Class @ 0xPTR" for a managed object pointer; "null" / "?unreadable" when it isn't one - writes into the caller's buffer (no heap)
static void DescribeManagedObject(const void* obj, char* buf, size_t bufSize) {
    if (!obj) {
        _snprintf_s(buf, bufSize, _TRUNCATE, "null");
        return;
    }
    if (!IsReadablePointer(const_cast<void*>(obj), 8)) {
        _snprintf_s(buf, bufSize, _TRUNCATE, "?unreadable @ 0x%08X", (unsigned)(uintptr_t)obj);
        return;
    }
    void* klass = MonoBridge::GetObjectClass(const_cast<void*>(obj));
    const char* cn = klass ? MonoBridge::GetClassName(klass) : nullptr;
    const char* ns = klass ? MonoBridge::GetClassNamespace(klass) : nullptr;
    if (!cn || !IsReadablePointer((void*)cn, 2)) cn = "?";
    if (!ns || !IsReadablePointer((void*)ns, 2)) ns = "";
    _snprintf_s(buf, bufSize, _TRUNCATE, "%s%s%s @ 0x%08X", ns, (ns[0] ? "." : ""), cn, (unsigned)(uintptr_t)obj);
}

// Render one argument's value storage (at `p`, inside the frame args buffer) by its cached type code
// By-ref params store a managed pointer regardless of pointee; ValueType payloads print as leading hex (size unknown from the cache)
static void DescribeArgValue(const uint8_t* p, MonoTypeCode code, bool isByRef, char* buf, size_t bufSize) {
    if (isByRef) {
        _snprintf_s(buf, bufSize, _TRUNCATE, "byref -> 0x%08X", *(const uint32_t*)p);
        return;
    }
    switch (code) {
    case MonoTypeCode::Boolean:
        _snprintf_s(buf, bufSize, _TRUNCATE, "bool = %s", *p ? "true" : "false");
        break;
    case MonoTypeCode::I1:
        _snprintf_s(buf, bufSize, _TRUNCATE, "i1 = %d", (int)*(const int8_t*)p);
        break;
    case MonoTypeCode::U1:
        _snprintf_s(buf, bufSize, _TRUNCATE, "u1 = %u", (unsigned)*p);
        break;
    case MonoTypeCode::Char:
        _snprintf_s(buf, bufSize, _TRUNCATE, "char = 0x%04X", (unsigned)*(const uint16_t*)p);
        break;
    case MonoTypeCode::I2:
        _snprintf_s(buf, bufSize, _TRUNCATE, "i2 = %d", (int)*(const int16_t*)p);
        break;
    case MonoTypeCode::U2:
        _snprintf_s(buf, bufSize, _TRUNCATE, "u2 = %u", (unsigned)*(const uint16_t*)p);
        break;
    case MonoTypeCode::I4:
        _snprintf_s(buf, bufSize, _TRUNCATE, "i4 = %d (0x%X)", *(const int32_t*)p, *(const uint32_t*)p);
        break;
    case MonoTypeCode::U4:
        _snprintf_s(buf, bufSize, _TRUNCATE, "u4 = %u (0x%X)", *(const uint32_t*)p, *(const uint32_t*)p);
        break;
    case MonoTypeCode::I8:
        _snprintf_s(buf, bufSize, _TRUNCATE, "i8 = %lld", (long long)*(const int64_t*)p);
        break;
    case MonoTypeCode::U8:
        _snprintf_s(buf, bufSize, _TRUNCATE, "u8 = %llu", (unsigned long long)*(const uint64_t*)p);
        break;
    case MonoTypeCode::R4:
        _snprintf_s(buf, bufSize, _TRUNCATE, "r4 = %g", (double)*(const float*)p);
        break;
    case MonoTypeCode::R8:
        _snprintf_s(buf, bufSize, _TRUNCATE, "r8 = %g", *(const double*)p);
        break;
    case MonoTypeCode::ValueType:
        _snprintf_s(buf, bufSize, _TRUNCATE, "VT = %02X %02X %02X %02X %02X %02X %02X %02X ...", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        break;
    case MonoTypeCode::String:
    case MonoTypeCode::Class:
    case MonoTypeCode::Object:
    case MonoTypeCode::SzArray:
    case MonoTypeCode::GenericInst:
        DescribeManagedObject(*(void* const*)p, buf, bufSize);
        return;
    default:
        _snprintf_s(buf, bufSize, _TRUNCATE, "?type(0x%02X) raw 0x%08X", (unsigned)code, *(const uint32_t*)p);
        break;
    }
}

void HookManager::DumpBakedCrashRecord(void* method, void* frameV, const char* reason) {
    auto* chain = GetChain(method);
    if (!chain || !chain->bakedActive) return;
    auto* frame = reinterpret_cast<InterpFrame*>(frameV);
    if (!IsReadablePointer(frame, sizeof(InterpFrame))) return;

    // POST-AV CONTEXT - build the record in a fixed stack buffer (snprintf-append, NO heap), then one WriteFile via the pre-opened handle
    char rec[16384];
    size_t off = 0;
    auto addf = [&rec, &off](const char* fmt, ...) {
        if (off >= sizeof(rec)) return;
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(rec + off, sizeof(rec) - off, _TRUNCATE, fmt, ap);
        va_end(ap);
        off += (n >= 0) ? (size_t)n : (sizeof(rec) - off - 1);
    };
    auto adds = [&rec, &off](const char* s) {
        size_t n = strlen(s);
        if (off + n >= sizeof(rec)) n = sizeof(rec) - 1 - off;
        memcpy(rec + off, s, n);
        off += n;
    };

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[256];
    addf("=== baked crash %04u-%02u-%02u %02u:%02u:%02u ===\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    addf("target : %s::%s\n", chain->targetClass.c_str(), chain->targetMethod.c_str());
    addf("reason : %s\n", (reason && reason[0]) ? reason : "(none)");

    // FAULTING LINK - match the frame's runtime_method against the compiled chain
    // Post-recompile, link 0 executes as the in-place-overwritten headRTM (links[0].linkRTM is the staged copy); a frame matching trampolineRTM means the fault was in the ORIGINAL body, not a hook
    RuntimeMethod* rtm = frame->runtime_method;
    auto it = m_compiledChains.find(chain->originalMethod);
    if (it != m_compiledChains.end()) {
        CompiledChain& c = it->second;
        adds("chain  : ");
        for (size_t i = 0; i < c.links.size(); ++i) { addf("[%u %s %s] ", (unsigned)i, c.links[i].kind == LinkKind::ShimHook ? "shim" : "typed", c.links[i].modName.c_str()); }
        addf("(v%u)\n", c.version);

        const char* blame = "?unmatched RTM";
        if (rtm == c.trampolineRTM) {
            blame = "trampoline (original method body)";
        } else {
            for (size_t i = 0; i < c.links.size(); ++i) {
                if (rtm == c.links[i].linkRTM || (i == 0 && rtm == c.headRTM)) {
                    _snprintf_s(line, sizeof(line), _TRUNCATE, "link %u (%s, %s)", (unsigned)i, c.links[i].kind == LinkKind::ShimHook ? "shim" : "typed", c.links[i].modName.c_str());
                    blame = line;
                    break;
                }
            }
        }
        addf("frame  : %s\n", blame);
    }

    // IP - byte offset into the faulting RTM's bytecode
    if (IsReadablePointer(rtm, sizeof(RuntimeMethod)) && IsValidPointer(rtm->code) && frame->ip && (uintptr_t)frame->ip >= (uintptr_t)rtm->code) {
        addf("ip     : 0x%08X (code+0x%X)\n", (unsigned)(uintptr_t)frame->ip, (unsigned)((uintptr_t)frame->ip - (uintptr_t)rtm->code));
    }

    // THIS + TYPED ARGS - `this` lives at frame->obj (+0x0C), NOT in the args buffer; arg_offsets[i] is explicit param i on instance and static targets alike
    if (chain->originalIsInstance) {
        DescribeManagedObject(frame->obj, line, sizeof(line));
        addf("this   : %s\n", line);
    }

    uint16_t nParams = chain->signatureCached ? chain->cachedParamCount : 0;
    bool rtmReadable = IsReadablePointer(rtm, sizeof(RuntimeMethod));
    if (rtmReadable && rtm->param_count < nParams) nParams = rtm->param_count; // trust the smaller claim
    const uint8_t* argsBuf = reinterpret_cast<const uint8_t*>(frame->args);
    uint16_t argsSize = rtmReadable ? rtm->args_size : 0;
    bool argsReadable = argsBuf && argsSize > 0 && IsReadablePointer(const_cast<uint8_t*>(argsBuf), argsSize);
    bool offsetsReadable = rtmReadable && nParams > 0 && IsReadablePointer(rtm->arg_offsets, nParams * sizeof(uint32_t));

    if (argsReadable && offsetsReadable) {
        for (uint16_t i = 0; i < nParams; ++i) {
            uint32_t argOff = rtm->arg_offsets[i];
            if (argOff + 8 > argsSize) { // slot must fit inside the arg area (8 = widest primitive read)
                addf("arg[%u] : ?offset 0x%X outside args_size 0x%X\n", (unsigned)i, argOff, (unsigned)argsSize);
                continue;
            }
            DescribeArgValue(argsBuf + argOff, chain->cachedParamTypes[i], chain->cachedParamIsByRef[i], line, sizeof(line));
            addf("arg[%u] : %s\n", (unsigned)i, line);
        }
    }

    // RAW ARGS BUFFER - ground truth if the typed decode is ever suspect; capped at 64 bytes
    if (argsReadable) {
        uint16_t dump = argsSize < 64 ? argsSize : 64;
        adds("raw    : ");
        for (uint16_t i = 0; i < dump; ++i) { addf("%02X ", argsBuf[i]); }
        if (dump < argsSize) adds("...");
        adds("\n");
    }

    // One allocation-free WriteFile via the pre-opened handle; the buffer stays stack-resident throughout
    if (!CrashAppend(s_crashLogFile, rec, off)) {
        rec[off] = '\0';
        LOG_ERROR("[HookManager] Could not append to baked_crashes.log (crash-log handle not open); record follows:\n" + std::string(rec, off));
    }
}

// ELIGIBILITY - the typed path requires a structurally-identical call all the way down: every entry must ExactMatch the target signature
// INSTANCE targets bake via the leading-self convention (see hook_manager.h); value-type (struct) receivers are refused inside CompileChain (object-self can't carry a struct's by-ref this)
bool HookManager::ChainBakeEligible(HookChain& chain) {
    if (chain.hooks.empty()) return false;

    // INTERPRETER INTEGRITY: the interp_exec_state recovery-site bytes must match this build - baked dispatch and the VEH crash-quarantine assume a pristine interpreter, else fall back to boxing
    // Live read-only probe (not a startup snapshot) so a late-installing patcher is still caught; typed entries get demoted (they run boxed, not dropped)
    if (!ExceptionHook::InterpRecoverySignatureIntact(Addresses::Methods.interp_exec_state)) {
        LOG_WARN("[HookManager] Bake refused (interpreter recovery signature mismatch): " + chain.targetClass + "::" + chain.targetMethod);
        return false;
    }

    // CRASH QUARANTINE: a target with >= kBakeQuarantineThreshold attributed crashes is refused; typed entries demote to boxing (delete baked_quarantine.txt to clear)
    if (IsBakeQuarantined(chain)) {
        LOG_WARN("[HookManager] Bake refused (reached crash-quarantine threshold): " + chain.targetClass + "::" + chain.targetMethod);
        return false;
    }

    // INFLATED TARGET: boxing dispatch only - the baked recipe re-signs link clones under the target's signature, and the context-sensitive transform path isn't proven for an inflated signature
    // Typed hooks demote to boxing instead of risking a bad bake
    if (chain.originalMethod && (reinterpret_cast<MonoMethodLayout*>(chain.originalMethod)->slotAndFlags & Offsets::METHOD_IS_INFLATED_FLAG)) {
        LOG_DEBUG("[HookManager] Bake skipped (inflated generic target - boxing dispatch only): " + chain.targetClass + "::" + chain.targetMethod);
        return false;
    }

    // The add_data_item instrument must be live or no link would ever capture its CallNext slots (every typed link would silently become replace-only); fall back rather than mis-dispatch
    if (!ChainCompiler::Get().IsInstalled()) {
        LOG_DEBUG("[HookManager] Bake skipped (instrument not installed): " + chain.targetClass + "::" + chain.targetMethod);
        return false;
    }

    SignatureInfo target = ChainCompiler::ReadSignature(chain.originalMethod);
    if (!target.valid) return false;

    // MIXED CHAINS bake: a boxing/watcher hook (usesChain=false) rides as a ShimHook link, continued through ContinueBakedChain - two requirements remain:
    //   - EVERY entry must ExactMatch the target: a shim link is entered with RAW arg slots (not boxed), so a loosely-typed boxing handler can't ride baked
    //   - At least ONE entry must forward via Chain.* (usesChain): an all-boxing chain gains nothing from baking and stays on the cheaper dispatch stub
    bool anyChain = false;
    for (const HookEntry& e : chain.hooks) {
        if (e.hasWrapper) return false; // generated dispatch wrapper - an instance-path artifact, not a bakeable link
        if (e.usesChain) anyChain = true;
        SignatureInfo h = ChainCompiler::ReadSignature(e.hookMethod);
        if (!h.ExactMatch(target)) {
            // Ineligible: the attributed log line is emitted by DemoteIneligibleTypedHooks (the caller routes here on a false return)
            // Keep the structural dump at DEBUG for diagnosis
            LOG_DEBUG("[HookManager]   " + DumpSig("target", target));
            LOG_DEBUG("[HookManager]   " + DumpSig("hook  ", h));
            return false;
        }
    }
    return anyChain;
}

// UNBAKEABLE CHAIN - demote-or-reject (see hook_manager.h): a boxing-safe Chain.* entry is KEPT and runs boxed
// A DEFECTIVE entry (signature mismatch, incoherent Chain.CallNext forward, Chain.ByRef) breaks both paths - reject it with an install-time error rather than box it into per-call runtime spam
// First reason -> bakeRejectReason (Audit.GetBakeRejectReason)
void HookManager::DemoteIneligibleTypedHooks(HookChain& chain) {
    bool anyChain = false;
    for (const HookEntry& e : chain.hooks) {
        if (e.usesChain) {
            anyChain = true;
            break;
        }
    }
    if (!anyChain) return; // pure boxing chain - never attempted a bake, nothing to explain

    SignatureInfo target = ChainCompiler::ReadSignature(chain.originalMethod);

    // The external-block reason (used only when the hook is itself boxing-safe): why couldn't the CHAIN bake?
    // Mixed chains bake, so the blocker is a sibling whose signature doesn't ExactMatch, quarantine, a downed instrument, or a compile/install failure
    auto externalReason = [&]() -> std::string {
        if (!ExceptionHook::InterpRecoverySignatureIntact(Addresses::Methods.interp_exec_state))
            return "the game's script interpreter doesn't match this build (another native patcher is present, or a game build mismatch), so S3MM disabled baking for safety and runs this hook boxed";
        if (IsBakeQuarantined(chain)) return "crashes on previous runs were attributed to this baked target (reached the quarantine threshold); delete baked_quarantine.txt to retry";
        if (!ChainCompiler::Get().IsInstalled()) return "baked-dispatch instrument unavailable";
        for (const HookEntry& o : chain.hooks) {
            SignatureInfo oh = ChainCompiler::ReadSignature(o.hookMethod);
            if (!target.valid || !oh.ExactMatch(target)) { return "another hook on this method (mod '" + o.modName + "') doesn't exactly match the target signature, so the whole chain can't bake"; }
        }
        // TryInstallBakedChain seeds bakeRejectReason with CompileChain's specific failure - prefer it
        return chain.bakeRejectReason.empty() ? "the baked chain could not be compiled or installed (see preceding warnings)" : chain.bakeRejectReason;
    };

    std::vector<void*> rejected;
    for (HookEntry& e : chain.hooks) {
        if (!e.usesChain) continue; // boxing/replacement hook - runs boxed as written, no message needed

        // Reject a DEFECT (breaks bake AND box); box a boxing-safe hook that was only blocked externally
        SignatureInfo h = ChainCompiler::ReadSignature(e.hookMethod);
        std::string defect, fwdReason;
        if (!target.valid) {
            defect = "could not read the target method signature";
        } else if (!h.ExactMatch(target)) {
            defect = "handler signature does not exactly match the target - " + DescribeSigMismatch(target, h);
        } else if (!ChainCompiler::VerifyHookForward(e.hookMethod, target, target.hasThis, fwdReason)) {
            defect = "incoherent Chain.CallNext forward (" + fwdReason + ") - can neither bake nor box";
        } else if (ChainCompiler::HookUsesByRef(e.hookMethod)) {
            defect = "Chain.ByRef can't fall back to boxing (ref/out forwarding only works on the baked path)";
        }

        std::string reason = defect.empty() ? externalReason() : defect;
        if (chain.bakeRejectReason.empty()) chain.bakeRejectReason = reason; // surfaced via Audit.GetBakeRejectReason

        if (!defect.empty()) {
            LOG_ERROR(
                "[HookManager] Chain hook REJECTED (" + reason + "): " + chain.targetClass + "::" + chain.targetMethod + " <- mod '" + e.modName + "'. Fix the handler, or use the boxing Hook.CallNext / Hook.Ref API.");
            RecordHookRefusal(e.modName, chain.targetClass + ":" + chain.targetMethod, "rejected typed hook: " + reason);
            rejected.push_back(e.hookMethod);
        } else {
            LOG_WARN("[HookManager] Chain hook not baked, running BOXED (" + reason + "): " + chain.targetClass + "::" + chain.targetMethod + " <- mod '" + e.modName + "'.");
        }
    }

    if (rejected.empty()) return;
    auto isRejected = [&](const HookEntry& e) { return std::find(rejected.begin(), rejected.end(), e.hookMethod) != rejected.end(); };
    chain.hooks.erase(std::remove_if(chain.hooks.begin(), chain.hooks.end(), isRejected), chain.hooks.end());
    chain.watchers.erase(std::remove_if(chain.watchers.begin(), chain.watchers.end(), isRejected), chain.watchers.end());
}

// BUILD - populate links from the priority-sorted hook list and ensure the trampoline RTM exists
bool HookManager::BuildCompiledChain(HookChain& chain, CompiledChain& c) {
    c.targetMethod = chain.originalMethod;

    // Tail target = the original IL: trampolineMethod is a BackupOriginalMethod copy (cleared RTM, original token/klass); transform it into an RTM that runs the original body with original identity
    auto* trampoline = reinterpret_cast<MonoMethodLayout*>(chain.trampolineMethod);
    if (!trampoline) {
        LOG_ERROR("[HookManager] BuildCompiledChain: no trampoline method");
        return false;
    }
    if (!trampoline->runtimeMethod) {
        MonoBridge::Get().SetupTLS();
        if (!MonoBridge::Get().TransformMethod(chain.trampolineMethod)) {
            LOG_ERROR("[HookManager] BuildCompiledChain: failed to transform trampoline (original IL)");
            return false;
        }
    }
    c.trampolineRTM = trampoline->runtimeMethod;
    if (!c.trampolineRTM) {
        LOG_ERROR("[HookManager] BuildCompiledChain: null trampoline RTM after transform");
        return false;
    }

    // One typed link per chain entry, in execution order (watchers merged in by priority already)
    c.links.clear();
    c.links.reserve(chain.hooks.size());
    for (const HookEntry& e : chain.hooks) {
        ChainLink link;
        // usesChain forwards via S3MM.Chain.* (baked slot); a boxing/watcher handler rides as a ShimHook link (see hook_manager.h)
        link.kind = e.usesChain ? LinkKind::TypedHook : LinkKind::ShimHook;
        link.modName = e.modName;
        link.userMethod = e.hookMethod;
        c.links.push_back(std::move(link));
    }
    return true;
}

// FIRST INSTALL - compile then transplant the head RTM onto the target
bool HookManager::TryInstallBakedChain(HookChain& chain) {
    CompiledChain c;
    if (!BuildCompiledChain(chain, c)) return false;

    if (!ChainCompiler::Get().CompileChain(c)) {
        // Seed the specific compile failure for Audit.GetBakeRejectReason; DemoteIneligibleTypedHooks keeps the first non-empty reason and its externalReason() falls back to this over the generic message
        chain.bakeRejectReason = c.failReason.empty() ? "the baked chain could not be compiled (see preceding warnings)" : c.failReason;
        LOG_WARN("[HookManager] Baked compile failed, falling back to boxing: " + chain.targetClass + "::" + chain.targetMethod);
        return false;
    }
    if (!ChainCompiler::Get().InstallChain(c)) {
        chain.bakeRejectReason = "the baked chain compiled but could not be installed (see preceding warnings)";
        LOG_WARN("[HookManager] Baked install failed, falling back to boxing: " + chain.targetClass + "::" + chain.targetMethod);
        return false;
    }

    m_compiledChains[chain.originalMethod] = std::move(c);
    chain.bakedActive = true;
    EnsureCrashLogHandles();        // pre-open the quarantine/crash-log files in a HEALTHY context so the VEH crash path never CreateFiles
    chain.bakeRejectReason.clear(); // baked cleanly - drop any stale reason from a prior failed attempt
    chain.isPatched = true;
    LOG_INFO("[HookManager] Installed baked chain: " + chain.targetClass + "::" + chain.targetMethod + " (" + std::to_string(chain.hooks.size()) + " links)");
    return true;
}

// RECOMPILE - rebuild links and re-install IN PLACE (preserve headRTM so game-cached pointers stay live)
bool HookManager::RecompileBakedChain(HookChain& chain) {
    // A runtime registration may have added an incompatible hook since the bake; rebuilding would compile a corrupt dispatch, so keep the old chain
    if (!ChainBakeEligible(chain)) {
        LOG_ERROR("[HookManager] Cannot recompile baked chain - a newly added hook made it ineligible "
                  "(incompatible signature, boxing hook, or instance target). Keeping the previous baked "
                  "chain: " +
                  chain.targetClass + "::" + chain.targetMethod);
        return false;
    }

    auto it = m_compiledChains.find(chain.originalMethod);
    if (it == m_compiledChains.end()) {
        // Lost the compiled record (shouldn't happen) - treat as first install
        return TryInstallBakedChain(chain);
    }
    // STAGE AND COMMIT - build/compile into a temp so a FAILED recompile leaves the live record untouched (the target still executes the old head, and the map's linkRTMs still match what frames execute)
    void* headRTM = it->second.headRTM;
    CompiledChain staged;
    staged.headRTM = headRTM; // non-null -> InstallChain overwrites this RTM's CONTENTS in place (game-cached pointers stay live)
    staged.version = it->second.version;
    if (!BuildCompiledChain(chain, staged)) return false;

    if (!ChainCompiler::Get().CompileChain(staged)) return false;
    if (!ChainCompiler::Get().InstallChain(staged)) return false;
    it->second = std::move(staged); // commit only on success
    return true;
}

// Forward declaration - defined below, near the boxing dispatch path it shares with NativeCallNext
static std::vector<void*> UnboxParametersFast(void* argsArray, const HookChain* chain);

// FIND CALLER BAKED LINK - frame-walk to the innermost link RTM of originalMethod's baked chain
// A ShimHook link's clone RTM is the current interpreter frame's runtime_method when its handler calls Hook.CallNext (the ICALL runs native, so the frame is still the link's), so a shallow walk resolves it
int HookManager::FindCallerBakedLinkIndex(void* originalMethod) {
    auto compIt = m_compiledChains.find(originalMethod);
    if (compIt == m_compiledChains.end()) return -1;
    CompiledChain& c = compIt->second;

    DWORD tlsIndex = *(DWORD*)Addresses::Profiler.g_thread_context_tls_index;
    void* ctx = TlsGetValue(tlsIndex);
    if (!ctx) return -1;

    InterpFrame* current = reinterpret_cast<ThreadContext*>(ctx)->current_frame;
    int frameCount = 0;
    while (current && frameCount < 10) { // innermost-first; safety-limited like NativeGetCallerHookToken
        RuntimeMethod* rtm = current->runtime_method;
        if (rtm) {
            for (size_t i = 0; i < c.links.size(); ++i) {
                // Post-recompile, link 0 executes as the in-place-overwritten headRTM (links[0].linkRTM holds the staged copy) - same special case as DumpBakedCrashRecord
                if (c.links[i].linkRTM == rtm || (i == 0 && rtm == c.headRTM)) return static_cast<int>(i);
            }
        }
        current = current->parent;
        frameCount++;
    }
    return -1;
}

// CONTINUE BAKED CHAIN - a ShimHook link's boxing Hook.CallNext lands here via NativeCallNext (see hook_manager.h)
void* HookManager::ContinueBakedChain(void* originalMethod, size_t fromLinkIndex, void* thisPtr, void* argsArray) {
    auto compIt = m_compiledChains.find(originalMethod);
    if (compIt == m_compiledChains.end()) {
        LOG_ERROR("[HookManager] ContinueBakedChain: no compiled chain for method");
        return nullptr;
    }
    CompiledChain& c = compIt->second;
    HookChain* chain = GetChain(originalMethod);

    std::vector<void*> unboxedParams = UnboxParametersFast(argsArray, chain);
    void** params = unboxedParams.empty() ? nullptr : unboxedParams.data();

    size_t next = fromLinkIndex + 1;
    if (next >= c.links.size()) {
        // Past the last link - fall through to the true original (trampoline)
        return InvokeOriginal(originalMethod, thisPtr, params);
    }

    void* nextMethod = c.links[next].linkMethod; // clone whose runtimeMethod == linkRTM (baked slots wired)
    if (!nextMethod) {
        LOG_ERROR("[HookManager] ContinueBakedChain: link " + std::to_string(next) + " has no clone method");
        return nullptr;
    }

    // Instance chains: linkMethod was re-signed instance (STATIC cleared), so runtime-invoke delivers thisPtr as the receiver - the leading-self convention
    MonoBridge::Get().SetupTLS();
    MonoInvokeState st = {};
    InvokeBridge bridge;
    bool ok = MonoBridge::Get().InvokeMethod(nextMethod, thisPtr, params, &st);
    if (!ok) {
        std::string excMsg = ExtractExceptionInfo(st.pThrowExc);
        LOG_ERROR("[HookManager] ContinueBakedChain: exception in link " + std::to_string(next) + " (" + c.links[next].modName + ") for " + (chain ? chain->targetClass + "::" + chain->targetMethod : std::string("?")) +
                  ": " + excMsg);
        if (chain) { ModLog::Get().Log(c.links[next].modName, "EXCEPTION in " + chain->targetClass + "::" + chain->targetMethod + ": " + excMsg, LogLevel::Error); }
    }

    // Boxed result out (see header) - a value-type-returning link must not hand managed Hook.CallNext a raw stackval pointer
    return BoxInvokeResult(chain, st, "ContinueBakedChain");
}

// INVOKE ORIGINAL - Call the trampoline to execute unhooked original IL
void* HookManager::InvokeOriginal(void* originalMethod, void* thisPtr, void** params) {
    LOG_HOOK_TRACE("[HookManager] InvokeOriginal: method=" + std::to_string((uintptr_t)originalMethod));

    auto* chain = GetChain(originalMethod);
    if (!chain) {
        LOG_ERROR("[HookManager] InvokeOriginal: no chain for method");
        return nullptr;
    }

    LOG_HOOK_TRACE("[HookManager] InvokeOriginal: found chain for " + chain->targetClass + "::" + chain->targetMethod);

    // Use the heap-allocated trampoline that was created during backup (it has the original token/klass/runtimeMethod values).
    if (!chain->trampolineMethod) {
        LOG_ERROR("[HookManager] InvokeOriginal: no trampoline method available");
        return nullptr;
    }

    auto* trampoline = reinterpret_cast<MonoMethodLayout*>(chain->trampolineMethod);
    bool needsCompile = (trampoline->runtimeMethod == nullptr);

    LOG_HOOK_TRACE("[HookManager] InvokeOriginal: using trampoline=" + std::to_string((uintptr_t)trampoline) + ", token=" + std::to_string(trampoline->token) +
                   ", runtime=" + std::to_string((uintptr_t)trampoline->runtimeMethod) + (needsCompile ? " (first-time compile)" : ""));

    // Ensure TLS is set up for Mono operations
    MonoBridge::Get().SetupTLS();

    // Invoke using the trampoline
    InvokeBridge bridge;
    MonoInvokeState state = {};
    bool invokeResult = MonoBridge::Get().InvokeMethod(chain->trampolineMethod, thisPtr, params, &state);

    LOG_HOOK_TRACE("[HookManager] InvokeOriginal: invoke returned " + std::to_string(invokeResult) + ", pRet=" + std::to_string((uintptr_t)state.pRet) + ", pRetVal=" + std::to_string((uintptr_t)state.pRetVal));

    // If the trampoline was compiled during this call, update it
    if (trampoline->runtimeMethod && !chain->originalRuntimeMethod) {
        chain->originalRuntimeMethod = trampoline->runtimeMethod;
        LOG_HOOK_TRACE("[HookManager] InvokeOriginal: captured new runtimeMethod=" + std::to_string((uintptr_t)trampoline->runtimeMethod));
    }

    // WORKAROUND: Mono 1.2 interpreter has a bug where first-time compilation + execution of a trampoline can fail, but the compilation side-effect persists.
    // If we needed compilation (runtimeMethod was null) and the invoke failed but now runtimeMethod is set, retry the invocation - it should succeed with the cached RuntimeMethod.
    if (!invokeResult && needsCompile && trampoline->runtimeMethod != nullptr) {
        LOG_WARN("[HookManager] InvokeOriginal: first-time compile failed, retrying with cached RuntimeMethod");

        // Clear state and retry
        memset(&state, 0, sizeof(state));
        invokeResult = MonoBridge::Get().InvokeMethod(chain->trampolineMethod, thisPtr, params, &state);

        LOG_HOOK_TRACE("[HookManager] InvokeOriginal: retry returned " + std::to_string(invokeResult) + ", pRet=" + std::to_string((uintptr_t)state.pRet) + ", pRetVal=" + std::to_string((uintptr_t)state.pRetVal));
    }

    // Use cached signature info to avoid re-reading signature every call. Ensure signature is cached (should already be from BackupOriginalMethod).
    if (!chain->signatureCached) {
        // Fallback: cache now if somehow missed
        CacheSignatureInfo(*chain);
    }

    return BoxInvokeResult(chain, state, "InvokeOriginal");
}

// SHARED RETURN TERMINAL - see hook_manager.h; every boxing return path funnels through here
void* HookManager::BoxInvokeResult(HookChain* chain, MonoInvokeState& state, const char* site) {
    const std::string tag = std::string("[HookManager] ") + (site ? site : "?") + ": ";
    MonoTypeCode typeCode = chain ? chain->cachedReturnType : MonoTypeCode::Object;

    LOG_HOOK_TRACE(
        tag + "typeCode=" + std::to_string((int)typeCode) + ", isObject=" + std::to_string(state.isObject) + ", pRet=" + std::to_string((uintptr_t)state.pRet) + ", pRetVal=" + std::to_string((uintptr_t)state.pRetVal));

    // FAST PATH: void return - nothing to return
    if (chain && chain->returnIsVoid) { return nullptr; }

    // Use isObject as the authoritative check for where the return value lives.
    // The interpreter sets isObject=1 for reference types, isObject=0 for value types.
    // This is more reliable than type code matching, especially for private nested classes, generic instantiations, and edge cases in type resolution.
    if (state.isObject) {
        LOG_HOOK_TRACE(tag + "isObject=1, returning pRetVal=" + std::to_string((uintptr_t)state.pRetVal));
        return state.pRetVal;
    }

    // Diagnostic: isObject should be authoritative, but warn if typeCode disagrees
    if (typeCode == MonoTypeCode::Class || typeCode == MonoTypeCode::Object || typeCode == MonoTypeCode::String || typeCode == MonoTypeCode::SzArray) {
        LOG_WARN(tag + "isObject=0 but typeCode=" + std::to_string((int)typeCode) + " suggests reference type. " + "pRetVal=" + std::to_string((uintptr_t)state.pRetVal) + ", pRet=" + std::to_string((uintptr_t)state.pRet));
    }

    // VALUE-TYPE RETURN, PRE-BOXED BY THE RUNTIME - the invoke setup pre-allocates the box and points pRet at its payload (box+0x4)
    // => pRetVal is the authoritative, already-filled boxed result, re-boxing from pRet is wrong for >8-byte structs
    if (state.pRetVal) {
        LOG_HOOK_TRACE(tag + "value-type return: using the runtime's pre-boxed pRetVal=" + std::to_string((uintptr_t)state.pRetVal));
        return state.pRetVal;
    }

    // FALLBACK - pRetVal null means the runtime didn't pre-box (e.g. a retried invoke with a partially-cleared state) box manually where safe
    LOG_WARN(tag + "pRetVal is null on a value-type return (setup helper didn't pre-box?) - falling back to manual boxing from pRet");

    // Without a chain we cannot name the value's class, so there is nothing safe to box, the raw pRet is a stackval pointer, NOT an object, and handing it back is exactly the pointer-shaped-garbage bug this helper exists to prevent
    if (!chain) {
        LOG_WARN(tag + "value-type return with no chain context - cannot determine return class, returning null rather than a raw stackval pointer");
        return nullptr;
    }

    // For value types (int, bool, float, etc.), we need to box the value. Use cached klass if available, otherwise look up from mono_defaults.
    void* valueKlass = chain->cachedReturnKlass;
    if (!valueKlass) {
        // For primitive types, look up the class from mono_defaults (cached at known addresses in the Sims 3 executable).
        valueKlass = GetPrimitiveKlass(typeCode);
        if (!valueKlass) {
            LOG_WARN(tag + "unknown primitive type " + std::to_string((int)typeCode) + " - returning null rather than a raw stackval pointer");
            return nullptr;
        }
        // Cache it for next time
        chain->cachedReturnKlass = valueKlass;
        LOG_HOOK_TRACE(tag + "cached primitive klass=" + std::to_string((uintptr_t)valueKlass));
    }

    // pRet points to the raw value bytes
    if (!state.pRet) {
        LOG_WARN(tag + "pRet is null for value type");
        return nullptr;
    }

    // The stackval-slot size guard applies ONLY to non-primitive value types (structs)
    // A primitive's payload is <= 8 bytes (one slot), but alignment padding can inflate its boxed instance_size (System.Double: [4 header][4 pad][8 data] = 16, so instance_size-4 = 12 would falsely trip a flat size guard)
    // The guard is reserved for genuinely wide structs (e.g. a 16-byte ResourceKey), whose payload lives on the interpreter vt side-stack, not in a single slot
    void* boxedValue = chain->returnIsPrimitive ? MonoBridge::Get().BoxValue(valueKlass, state.pRet) : MonoBridge::Get().BoxValueChecked(valueKlass, state.pRet, MonoBridge::STACKVAL_SLOT_BYTES, site);
    LOG_HOOK_TRACE(tag + "boxed value = " + std::to_string((uintptr_t)boxedValue));

    return boxedValue;
}

// INVOKE BASE - Skip all hooks (same as InvokeOriginal)
void* HookManager::InvokeBase(void* originalMethod, void* thisPtr, void** params) {
    return InvokeOriginal(originalMethod, thisPtr, params);
}

// VALUE TYPE CHECK - Cached class pointers for fast comparison, avoids string ops per call
static void* s_cachedValueTypeKlass = nullptr;
static void* s_cachedEnumKlass = nullptr;
static bool s_valueTypeKlassInitAttempted = false;

static bool IsValueTypeClass(void* klass) {
    if (!klass) return false;
    void* parentClass = MonoBridge::Get().GetParentClass(klass);
    if (!parentClass) return false;

    // Lazy init cached class pointers (once per session)
    if (!s_valueTypeKlassInitAttempted) {
        s_valueTypeKlassInitAttempted = true;
        s_cachedValueTypeKlass = MonoBridge::Get().GetClassByName("System", "ValueType");
        s_cachedEnumKlass = MonoBridge::Get().GetClassByName("System", "Enum");
    }

    // Fast path: pointer comparison
    if (s_cachedValueTypeKlass && s_cachedEnumKlass) { return (parentClass == s_cachedValueTypeKlass || parentClass == s_cachedEnumKlass); }

    // Fallback: string comparison (if class lookup failed)
    const char* parentName = MonoBridge::Get().GetClassName(parentClass);
    const char* parentNs = MonoBridge::Get().GetClassNamespace(parentClass);
    if (!parentNs || !parentName) return false;
    return (strcmp(parentNs, "System") == 0) && (strcmp(parentName, "ValueType") == 0 || strcmp(parentName, "Enum") == 0);
}

// FAST UNBOX PARAMETERS - Use cached type info to avoid expensive IsValueTypeClass() checks
static std::vector<void*> UnboxParametersFast(void* argsArray, const HookChain* chain) {
    std::vector<void*> unboxedParams;

    int32_t arrayLen = argsArray ? MonoBridge::Get().GetArrayLength(argsArray) : 0;
    if (arrayLen < 0) arrayLen = 0;

    // ARG COUNT GUARD - the invoke marshals sig->param_count entries off ppParams regardless of what we hand it, so a short array is an AV inside interp_mono_invoke_with_state
    // Pad to the signature (null for ref types, a zeroed slot for value types) and say why once per chain; the usual cause is llikely C# `params` swallowing an object[] argument
    static const uint8_t s_zeroSlot[64] = {0};
    static std::unordered_set<const HookChain*> s_countWarned;
    int32_t sigCount = (chain && chain->signatureCached) ? chain->cachedParamCount : arrayLen;
    if (arrayLen != sigCount && s_countWarned.insert(chain).second) {
        LOG_ERROR("[HookManager] " + chain->targetClass + "::" + chain->targetMethod + " called through CallNext/CallOriginal with " + std::to_string(arrayLen) + " arg(s), signature has " + std::to_string(sigCount) +
                  (arrayLen < sigCount ? " - padding with defaults. If the target takes a single object[], wrap it: CallNext(self, new object[] { arr })" : " - extra args ignored"));
    }
    if (arrayLen == 0 && sigCount == 0) return unboxedParams;

    unboxedParams.resize(sigCount);
    void** arrayElements = argsArray ? (void**)((uintptr_t)argsArray + 0x10) : nullptr;
    for (int32_t i = arrayLen; i < sigCount; i++) { unboxedParams[i] = chain->cachedParamIsValueType[i] ? (void*)s_zeroSlot : nullptr; }
    if (arrayLen > sigCount) arrayLen = sigCount;

    // Use cached param info if available and matches
    bool useCached = chain && chain->signatureCached && (size_t)sigCount == chain->cachedParamIsValueType.size();

    for (int32_t i = 0; i < arrayLen; i++) {
        void* boxedValue = arrayElements[i];

        if (!boxedValue) {
            unboxedParams[i] = nullptr;
            continue;
        }

        // Hook.Ref<T> byref wrapper (runtime check): a single-element array passed at a position whose TARGET param is ref/out is the by-ref smuggle - pass &array[0]
        // GATED on cachedParamIsByRef so a legitimate length-1 array argument to a by-value array param isn't mis-dereffed as a by-ref smuggle (type confusion)
        // Without cached info fall back to the legacy any-position heuristic; for ref-type byrefs (e.g. ref uint[]) modders still create T[1] wrappers manually
        void* klass = MonoBridge::Get().GetObjectClass(boxedValue);
        bool byrefPosition = !useCached || chain->cachedParamIsByRef[i];
        if (klass && byrefPosition) {
            // Quick check: arrays have SzArray type code at offset 0xA6 in class
            uint8_t typeEnum = *(uint8_t*)((uintptr_t)klass + Offsets::CLASS_ELEMENT_TYPE);
            if (typeEnum == (uint8_t)MonoTypeCode::SzArray) {
                int32_t innerArrayLen = MonoBridge::Get().GetArrayLength(boxedValue);
                if (innerArrayLen == 1) {
                    // Single-element array - byref pattern: pass pointer into array data
                    unboxedParams[i] = (void*)((uintptr_t)boxedValue + Offsets::ARRAY_DATA);
                    continue;
                }
            }
        }

        // FAST PATH: Use cached type info to decide unboxing
        if (useCached) {
            if (chain->cachedParamIsValueType[i]) {
                // Cached: this param is a value type - unbox directly
                unboxedParams[i] = (void*)((uintptr_t)boxedValue + 0x04);
            } else {
                // Cached: this param is a reference type - pass as-is
                unboxedParams[i] = boxedValue;
            }
        } else {
            // SLOW PATH: No cached info, check at runtime
            if (klass && IsValueTypeClass(klass)) {
                unboxedParams[i] = (void*)((uintptr_t)boxedValue + 0x04);
            } else {
                unboxedParams[i] = boxedValue;
            }
        }
    }

    return unboxedParams;
}

// NATIVE CALL NEXT - Chain-aware dispatch, invokes next hook or falls through to trampoline
void* __cdecl HookManager::NativeCallNext(uint32_t methodToken, void* thisPtr, void* argsArray) {
    auto& mgr = HookManager::Get();

    // Look up the chain by token
    auto it = mgr.m_tokenToMethod.find(methodToken);
    if (it == mgr.m_tokenToMethod.end()) {
        LOG_ERROR("[HookManager] NativeCallNext: unknown method token " + std::to_string(methodToken));
        return nullptr;
    }

    void* originalMethod = it->second;
    HookChain* chain = mgr.GetChain(originalMethod);
    if (!chain) {
        LOG_ERROR("[HookManager] NativeCallNext: no chain for method");
        return nullptr;
    }

    // BAKED CHAIN: a ShimHook link forwarding via Hook.CallNext - the chain dispatches through the compiled RTM links, so continue natively from the calling link (see hook_manager.h)
    if (chain->bakedActive) {
        int linkIdx = mgr.FindCallerBakedLinkIndex(originalMethod);
        if (linkIdx >= 0) { return mgr.ContinueBakedChain(originalMethod, static_cast<size_t>(linkIdx), thisPtr, argsArray); }
        // A baked chain is always entered through one of its links; not finding the caller means the frame walk missed it
        // Fail safe to the original rather than mis-dispatch through the unused hook list
        LOG_ERROR("[HookManager] NativeCallNext: baked chain but caller link not found for " + chain->targetClass + "::" + chain->targetMethod + "; invoking original");
        std::vector<void*> p = UnboxParametersFast(argsArray, chain);
        return mgr.InvokeOriginal(originalMethod, thisPtr, p.empty() ? nullptr : p.data());
    }

    // Check if we have an active chain state for this token
    ChainExecutionState* state = FindChainState(methodToken);
    bool isNewChain = (state == nullptr);

    // FAST PATH: Single-entry chain (the lone entry may be a hook OR a watcher - watchers live in chain.hooks too), first call.
    // That entry is hook[0], already invoked by the dispatch stub before it called us, so nothing is skipped - skip chain state, go straight to trampoline.
    if (isNewChain && chain->hooks.size() == 1) {
        LOG_HOOK_TRACE("[HookManager] NativeCallNext: FAST PATH for single-entry chain " + chain->targetClass + "::" + chain->targetMethod);

        // Unbox params and invoke trampoline directly
        std::vector<void*> unboxedParams = UnboxParametersFast(argsArray, chain);
        void** params = unboxedParams.empty() ? nullptr : unboxedParams.data();

        return mgr.InvokeOriginal(originalMethod, thisPtr, params);
    }

    // RAII guard for cleanup on exception, only for new chains
    ChainStackGuard guard(methodToken, isNewChain);

    // Cache values before nested calls that could invalidate state pointer
    void* cachedThisPtr = thisPtr;
    void** cachedParams = nullptr;
    size_t cachedParamCount = 0;
    size_t nextIdx = 0;

    if (isNewChain) {
        // First CallNext for this chain - create state. nextHookIndex starts at 1 because hook[0] is already running (it called us).
        ChainExecutionState newState;
        newState.methodToken = methodToken;
        newState.chain = chain;
        newState.nextHookIndex = 1;
        newState.thisPtr = thisPtr;
        newState.paramStorage = UnboxParametersFast(argsArray, chain);
        newState.params = newState.paramStorage.empty() ? nullptr : newState.paramStorage.data();
        newState.callDepth = 1; // First call

        t_chainStack.push_back(std::move(newState));
        // Don't store pointer, vector may reallocate during nested calls
        // Re-fetch state after push to get current values
        state = FindChainState(methodToken);

        LOG_HOOK_TRACE("[HookManager] NativeCallNext: started chain for " + chain->targetClass + "::" + chain->targetMethod + " (" + std::to_string(chain->hooks.size()) + " hooks, " +
                       std::to_string(chain->watchers.size()) + " watchers)");
    } else {
        // Existing chain, nested CallNext call
        state->callDepth++; // Track nesting
        if (argsArray) {
            state->paramStorage = UnboxParametersFast(argsArray, chain);
            state->params = state->paramStorage.empty() ? nullptr : state->paramStorage.data();
        }
        state->thisPtr = thisPtr;
    }

    // Cache values for invocation, these survive even if state pointer becomes invalid
    if (state) {
        cachedThisPtr = state->thisPtr;
        cachedParams = state->params;
        cachedParamCount = state->paramStorage.size();
        nextIdx = state->nextHookIndex;
    }

    void* result = nullptr;

    // Are there more hooks to invoke?
    if (nextIdx < chain->hooks.size()) {
        // Increment nextHookIndex before invocation (state may be invalidated after)
        state->nextHookIndex++;
        const HookEntry& nextHook = chain->hooks[nextIdx];

        LOG_HOOK_TRACE("[HookManager] NativeCallNext: invoking hook[" + std::to_string(nextIdx) + "] from " + nextHook.modName);

        // Ensure TLS is set up for Mono operations
        MonoBridge::Get().SetupTLS();

        // For instance method chains, hooks are static but follow the S3MM convention where 'this' is the first declared C# parameter (e.g. MyHook(object this_, ...)).
        // mono_runtime_invoke ignores the obj parameter for static methods, so we must prepend thisPtr to the params array so it arrives as the hook's first argument.
        void** invokeParams = cachedParams;
        void* invokeThisPtr = cachedThisPtr;
        std::vector<void*> extendedParams;

        if (chain->originalIsInstance) {
            extendedParams.reserve(cachedParamCount + 1);
            extendedParams.push_back(cachedThisPtr); // this -> hook's first param
            for (size_t i = 0; i < cachedParamCount; i++) { extendedParams.push_back(cachedParams[i]); }
            invokeParams = extendedParams.data();
            invokeThisPtr = nullptr; // static method, obj unused
        }

        // Invoke next hook; it may call CallNext recursively to continue chain.
        // WARNING: after this call, 'state' pointer may be invalid due to vector reallocation.
        MonoInvokeState invokeState = {};
        bool success;
        {
            InvokeBridge bridge;
            success = MonoBridge::Get().InvokeMethod(nextHook.hookMethod, invokeThisPtr, invokeParams, &invokeState);
        }

        if (!success) {
            std::string excMsg = ExtractExceptionInfo(invokeState.pThrowExc);
            LOG_ERROR("[HookManager] Exception in hook[" + std::to_string(nextIdx) + "] (" + nextHook.modName + ") for " + chain->targetClass + "::" + chain->targetMethod + ": " + excMsg);
            ModLog::Get().Log(nextHook.modName, "EXCEPTION in " + chain->targetClass + "::" + chain->targetMethod + ": " + excMsg, LogLevel::Error);
        }

        result = invokeState.pRetVal ? invokeState.pRetVal : invokeState.pRet;
    } else {
        // No more hooks - invoke the actual original (trampoline)
        LOG_HOOK_TRACE("[HookManager] NativeCallNext: invoking trampoline");
        // WARNING WEEE WOOO WEE WOO: after this call, 'state' pointer may be invalid due to vector reallocation
        result = mgr.InvokeOriginal(originalMethod, cachedThisPtr, cachedParams);
    }

    // Re-fetch state pointer after invocation, may have been invalidated by nested calls
    state = FindChainState(methodToken);
    if (!state) {
        // State was somehow removed during invocation, this shouldn't happen normally
        LOG_ERROR("[HookManager] NativeCallNext: state lost after invocation!");
        guard.MarkComplete(); // don't try to pop, already gone
        return result;
    }

    // Decrement call depth and clean up if we're back to the root
    state->callDepth--;
    if (state->callDepth <= 0) {
        if (!t_chainStack.empty() && t_chainStack.back().methodToken == methodToken) { t_chainStack.pop_back(); }
        // Mark guard complete so destructor doesn't double-pop
        guard.MarkComplete();
        LOG_HOOK_TRACE("[HookManager] NativeCallNext: chain complete, returning result");
    }

    return result;
}

// NATIVE CALL ORIGINAL - Bypass all hooks, invoke the true original trampoline directly
void* __cdecl HookManager::NativeCallOriginal(uint32_t methodToken, void* thisPtr, void* argsArray) {
    auto& mgr = HookManager::Get();

    auto it = mgr.m_tokenToMethod.find(methodToken);
    if (it == mgr.m_tokenToMethod.end()) {
        LOG_ERROR("[HookManager] NativeCallOriginal: unknown method token");
        return nullptr;
    }

    // Get chain for cached type info
    HookChain* chain = mgr.GetChain(it->second);

    // Use fast unboxing with cached type info
    std::vector<void*> unboxedParams = UnboxParametersFast(argsArray, chain);
    void** params = unboxedParams.empty() ? nullptr : unboxedParams.data();

    return mgr.InvokeBase(it->second, thisPtr, params);
}

// NATIVE GET CALLER HOOK TOKEN - Walk interpreter frames to find calling hook method
uint32_t __cdecl HookManager::NativeGetCallerHookToken() {
    auto& mgr = HookManager::Get();

    DWORD tlsIndex = *(DWORD*)Addresses::Profiler.g_thread_context_tls_index;

    void* ctx = TlsGetValue(tlsIndex);
    if (!ctx) { return 0; }

    // Get current frame from ThreadContext->current_frame (offset 0x08)
    ThreadContext* threadCtx = reinterpret_cast<ThreadContext*>(ctx);
    InterpFrame* frame = threadCtx->current_frame;

    if (!frame) { return 0; }

    // Walk interpreter frames to find the calling hook method.
    // Dispatch stub frames: RTM->method points to the original MonoMethod* (identity-preserving patch, token/klass never clobbered), detected via m_chains.
    // Hook[1..N] frames: invoked via mono_runtime_invoke on their own MonoMethod* (in mod assemblies), detected via m_hookMethodToToken or composite key.
    int frameCount = 0;
    InterpFrame* current = frame;

    while (current && frameCount < 10) { // Safety limit
        RuntimeMethod* rtm = current->runtime_method;

        if (rtm && rtm->method) {
            void* monoMethod = rtm->method;
            auto* layout = reinterpret_cast<MonoMethodLayout*>(monoMethod);

            // Check if this is a dispatch stub frame (RTM->method = original method).
            // After identity-preserving patching, the original retains its EA identity but executes hook IL via the transplanted RTM.
            auto chainIt = mgr.m_chains.find(monoMethod);
            if (chainIt != mgr.m_chains.end() && chainIt->second.isPatched) {
                // Deferred identity restoration: if proxy compilation failed during hook installation, token/klass were temporarily clobbered.
                // Now that the method has been compiled (we're executing it), restore identity.
                if (chainIt->second.needsIdentityRestore) {
                    auto* origLayout = reinterpret_cast<MonoMethodLayout*>(chainIt->second.originalMethod);
                    auto* trampoline = reinterpret_cast<MonoMethodLayout*>(chainIt->second.trampolineMethod);
                    if (trampoline && origLayout->runtimeMethod) {
                        void* hookRTM = origLayout->runtimeMethod;
                        DWORD op;
                        if (VirtualProtect(chainIt->second.originalMethod, sizeof(MonoMethodLayout), PAGE_EXECUTE_READWRITE, &op)) {
                            *origLayout = *trampoline;           // Restore all original identity
                            origLayout->runtimeMethod = hookRTM; // Keep hook's compiled RTM
                            VirtualProtect(chainIt->second.originalMethod, sizeof(MonoMethodLayout), op, &op);
                        }
                        chainIt->second.needsIdentityRestore = false;
                        LOG_DEBUG("[HookManager] Deferred identity restore complete: " + chainIt->second.targetClass + "::" + chainIt->second.targetMethod);
                    }
                }
                // Return the chain's UNIQUE dispatch token (m_tokenToMethod key), not the raw MethodDef token - a cross-assembly row collision would resolve CallNext to the WRONG chain
                return chainIt->second.dispatchToken ? chainIt->second.dispatchToken : chainIt->second.originalToken;
            }

            // Check if this method is in our hook map (by pointer) - hook[1..N]
            auto it = mgr.m_hookMethodToToken.find(monoMethod);
            if (it != mgr.m_hookMethodToToken.end()) { return it->second; }

            // Check if this (image, token) matches a hook token using composite key.
            // Authoritative check for hook methods; composite key uniquely identifies a hook because it includes both the image pointer and the token.
            if (layout->klass) {
                void* frameImage = *(void**)((uintptr_t)layout->klass + Offsets::CLASS_IMAGE);
                uint64_t compositeKey = MakeHookTokenKey(frameImage, layout->token);
                auto hookTokenIt = mgr.m_hookTokenToTargetToken.find(compositeKey);
                if (hookTokenIt != mgr.m_hookTokenToTargetToken.end()) { return hookTokenIt->second; }
            }
        }

        current = current->parent;
        frameCount++;
    }

    LOG_WARN("[HookManager] NativeGetCallerHookToken: no hook found in call stack");
    return 0;
}

// CALLER FRAME - the game frame that called the innermost hooked method
// Every chain frame back-points RTM->method to the hooked target: the boxing dispatch stub, the baked head/links (ChainCompiler sets it) and the trampoline (MethodPatcher fixes it)
// So the walk is: skip everything until a frame resolves to a chain (our ICALL, handlers, S3MM.Chain helpers), skip the rest of that chain, the next frame is the caller
// parent == NULL is the entry frame of a fresh-context runtime-invoke, i.e. a native/engine caller - reported as "no caller", not "fixed"
static void* FrameMethod(InterpFrame* frame) {
    RuntimeMethod* rtm = frame->runtime_method;
    if (!IsValidPointer(rtm) || !IsValidPointer(rtm->method)) return nullptr;
    return rtm->method;
}

// The original body runs in the trampoline clone (its own RTM, back-pointer to the clone), so those frames read as the clone unless mapped back
void* HookManager::CanonicalFrameMethod(InterpFrame* frame) {
    void* method = FrameMethod(frame);
    if (!method) return nullptr;
    auto it = m_trampolineToOriginal.find(method);
    return it == m_trampolineToOriginal.end() ? method : it->second;
}

// mono_interp_transform_method substitutes mono_marshal_get_delegate_invoke for <Delegate>.Invoke, so exactly one wrapper frame sits between a delegate caller and the target
static bool IsDelegateInvokeWrapper(void* method) {
    uintptr_t mdAddr = Addresses::Defaults.multicastdelegate_class;
    if (!mdAddr || !IsValidPointer(method)) return false;
    void* multicast = *(void**)mdAddr;
    auto* layout = reinterpret_cast<MonoMethodLayout*>(method);
    if (!multicast || !IsValidPointer(layout->klass) || !IsValidPointer((void*)layout->name)) return false;
    return *(void**)((uintptr_t)layout->klass + Offsets::CLASS_PARENT) == multicast && strcmp(layout->name, "Invoke") == 0;
}

// DIAG - first few failed walks dump the chain they saw (method names innermost-first), so a "native caller" verdict can be checked against reality
static void LogFailedCallerWalk(InterpFrame* start, const char* why) {
    static int budget = 5;
    if (budget <= 0) return;
    budget--;
    std::string chain;
    int n = 0;
    size_t bridge = t_invokeBridges.size();
    for (InterpFrame* f = start; f && n < 32 && IsValidPointer(f); ++n) {
        void* m = FrameMethod(f);
        chain += (n ? " <- " : "") + (m ? MonoBridge::GetMethodFullName(m) : std::string("<no method>"));
        f = f->parent;
        if (!f && bridge > 0) {
            f = t_invokeBridges[--bridge];
            chain += " <-[bridge]";
        }
    }
    LOG_DEBUG(std::string("[HookManager] GetCaller walk failed (") + why + "): " + chain);
}

bool HookManager::ResolveCallerFrameUnguarded(InterpFrame* frame, CallerFrame& out) {
    InterpFrame* start = frame;
    void* chainTarget = nullptr;
    size_t bridge = t_invokeBridges.size(); // consumed innermost-first as NULL parents are crossed
    for (int depth = 0; frame && depth < 32; ++depth) {
        if (!IsValidPointer(frame)) return false;
        void* method = CanonicalFrameMethod(frame);
        InterpFrame* next = frame->parent;
        if (!next && bridge > 0) next = t_invokeBridges[--bridge];

        if (!chainTarget) {
            if (method && m_chains.count(method)) chainTarget = method;
            frame = next;
            continue;
        }
        if (method == chainTarget || IsDelegateInvokeWrapper(method)) {
            frame = next;
            continue;
        }
        if (!method) {
            LogFailedCallerWalk(start, "frame without method after chain");
            return false;
        }

        // A frame of ANOTHER chain: a baked link running a handler reports that handler, the trampoline/stub reports the target itself
        auto compIt = m_compiledChains.find(method);
        if (compIt != m_compiledChains.end()) {
            RuntimeMethod* rtm = frame->runtime_method;
            for (size_t i = 0; i < compIt->second.links.size(); ++i) {
                if (compIt->second.links[i].linkRTM == rtm || (i == 0 && rtm == compIt->second.headRTM)) {
                    method = compIt->second.links[i].userMethod;
                    break;
                }
            }
        }
        out.frame = frame;
        out.method = method;
        return true;
    }
    LogFailedCallerWalk(start, chainTarget ? "chain is the entry frame (parent NULL)" : "no chain frame found");
    return false;
}

bool HookManager::ResolveCallerFrame(CallerFrame& out) {
    out = CallerFrame{};
    if (!Addresses::Profiler.g_thread_context_tls_index) return false;
    void* ctx = TlsGetValue(*(DWORD*)Addresses::Profiler.g_thread_context_tls_index);
    if (!ctx) return false;
    __try {
        return ResolveCallerFrameUnguarded(reinterpret_cast<ThreadContext*>(ctx)->current_frame, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Box one value stored at `src` as `monoType`; reference types are read as the object pointer, value types (primitives, structs, enums, generic structs) copy via mono_value_box
// availableBytes bounds the copy for args-area slots; 0 = unbounded (a by-ref pointee is real storage of that type)
static void* BoxTypedValue(void* monoType, const void* src, uint32_t availableBytes, const char* site) {
    using class_from_type_t = void*(__cdecl*)(void* monoType, void* context);
    auto classFromType = reinterpret_cast<class_from_type_t>(Addresses::Classes.SPEC_mono_class_from_mono_type);
    if (!classFromType || !IsValidPointer(monoType)) return nullptr;
    void* klass = classFromType(monoType, nullptr);
    if (!IsValidPointer(klass)) return nullptr;

    bool isValueType = (*(uint8_t*)((uintptr_t)klass + Offsets::CLASS_FLAGS_BYTE) & Offsets::CLASS_FLAG_VALUETYPE) != 0;
    if (!isValueType) return *(void**)src;
    if (availableBytes == 0) return MonoBridge::Get().BoxValue(klass, const_cast<void*>(src));
    return MonoBridge::Get().BoxValueChecked(klass, const_cast<void*>(src), availableBytes, site);
}

void* __cdecl HookManager::NativeCallerMethod() {
    CallerFrame cf;
    return HookManager::Get().ResolveCallerFrame(cf) ? cf.method : nullptr;
}

void* __cdecl HookManager::NativeCallerName() {
    CallerFrame cf;
    if (!HookManager::Get().ResolveCallerFrame(cf)) return nullptr;
    return MonoBridge::Get().CreateString(MonoBridge::GetMethodFullName(cf.method).c_str());
}

int32_t __cdecl HookManager::NativeCallerIs(void* nameStr) {
    CallerFrame cf;
    if (!nameStr || !HookManager::Get().ResolveCallerFrame(cf)) return 0;
    return MonoBridge::GetMethodFullName(cf.method) == MonoBridge::Get().ExtractString(nameStr) ? 1 : 0;
}

// Struct receivers arrive as a payload pointer (interior or box+4), so hand out a boxed copy rather than the pointer
void* __cdecl HookManager::NativeCallerInstance() {
    CallerFrame cf;
    if (!HookManager::Get().ResolveCallerFrame(cf) || !cf.frame->obj) return nullptr;
    void* klass = reinterpret_cast<MonoMethodLayout*>(cf.method)->klass;
    if (!IsValidPointer(klass)) return nullptr;
    if (*(uint8_t*)((uintptr_t)klass + Offsets::CLASS_FLAGS_BYTE) & Offsets::CLASS_FLAG_VALUETYPE) return MonoBridge::Get().BoxValue(klass, cf.frame->obj);
    return cf.frame->obj;
}

// Args live in the frame's private args area (frame->args + rtm->arg_offsets[i]) at native widths, value types inline - same recipe DumpBakedCrashRecord validated
// By-ref slots hold a managed pointer into a live ancestor frame: deref'd and copied here, never handed out
void* __cdecl HookManager::NativeCallerArgs() {
    CallerFrame cf;
    if (!HookManager::Get().ResolveCallerFrame(cf)) return nullptr;

    RuntimeMethod* rtm = cf.frame->runtime_method;
    void* sig = MonoBridge::Get().GetMethodSignature(cf.method);
    if (!IsReadablePointer(sig, Offsets::SIG_PARAMS) || !IsReadablePointer(rtm, sizeof(RuntimeMethod))) return nullptr;

    uint16_t nParams = *(uint16_t*)((uintptr_t)sig + Offsets::SIG_PARAM_COUNT);
    if (rtm->param_count < nParams) nParams = rtm->param_count;
    auto* args = reinterpret_cast<const uint8_t*>(cf.frame->args);
    uint16_t argsSize = rtm->args_size;
    if (nParams > 0 && (!IsReadablePointer(const_cast<uint8_t*>(args), argsSize) || !IsReadablePointer(rtm->arg_offsets, nParams * sizeof(uint32_t)))) return nullptr;

    void* objectKlass = MonoBridge::Get().GetClassByName("System", "Object");
    void* array = objectKlass ? MonoBridge::Get().CreateArray(objectKlass, nParams) : nullptr;
    if (!array) return nullptr;

    for (uint16_t i = 0; i < nParams; ++i) {
        void* paramType = *(void**)((uintptr_t)sig + Offsets::SIG_PARAMS + i * sizeof(void*));
        uint32_t off = rtm->arg_offsets[i];
        if (off + 4 > argsSize) continue; // slot outside the args area: leave null
        const uint8_t* src = args + off;

        void* boxed = nullptr;
        if (ChainCompiler::TypeIsByRef(paramType)) {
            void* pointee = *(void**)src;
            if (IsValidPointer(pointee)) boxed = BoxTypedValue(paramType, pointee, 0, "CallerArgs byref");
        } else {
            boxed = BoxTypedValue(paramType, src, argsSize - off, "CallerArgs");
        }
        if (void* elem = MonoBridge::Get().GetArrayElementPtr(array, i, sizeof(void*))) *(void**)elem = boxed;
    }
    return array;
}

uint32_t __cdecl HookManager::NativeCallerMintOffset() {
    CallerFrame cf;
    if (!HookManager::Get().ResolveCallerFrame(cf)) return 0;
    RuntimeMethod* rtm = cf.frame->runtime_method;
    if (!IsValidPointer(rtm) || !cf.frame->ip || (uintptr_t)cf.frame->ip < (uintptr_t)rtm->code) return 0;
    return (uint32_t)((uintptr_t)cf.frame->ip - (uintptr_t)rtm->code);
}

// AUTO-PREFIXED LOG - Walk frames to resolve calling mod's assembly name, skipping S3MM.Helper
static std::string ResolveCallerAssemblyName() {
    if (!Addresses::Profiler.g_thread_context_tls_index) return "Mod";

    DWORD tlsIndex = *(DWORD*)Addresses::Profiler.g_thread_context_tls_index;
    void* ctx = TlsGetValue(tlsIndex);
    if (!ctx) return "Mod";

    ThreadContext* threadCtx = reinterpret_cast<ThreadContext*>(ctx);
    InterpFrame* frame = threadCtx->current_frame;

    for (int i = 0; i < 15 && frame; i++, frame = frame->parent) {
        if (!IsValidPointer(frame)) break;

        RuntimeMethod* rtm = frame->runtime_method;
        if (!rtm || !IsValidPointer(rtm) || !rtm->method || !IsValidPointer(rtm->method)) continue;

        auto* layout = reinterpret_cast<MonoMethodLayout*>(rtm->method);
        if (!layout->klass || !IsValidPointer(layout->klass)) continue;

        void* image = *(void**)((uintptr_t)layout->klass + Offsets::CLASS_IMAGE);
        if (!image || !IsValidPointer(image)) continue;

        const char* name = *(const char**)((uintptr_t)image + Offsets::IMAGE_ASSEMBLY_NAME);
        if (!name || !IsValidPointer((void*)name)) continue;

        // Skip S3MM.Helper frames, we want the actual mod's assembly
        if (strcmp(name, "S3MM") == 0 || strcmp(name, "S3MM.Helper") == 0) continue;

        return std::string(name);
    }

    // Fallback: try chain stack (works when called from within a hook dispatch)
    if (!t_chainStack.empty()) {
        const auto& state = t_chainStack.back();
        if (state.chain && !state.chain->hooks.empty()) { return state.chain->hooks[0].modName; }
    }

    return "Mod";
}

void __cdecl HookManager::NativeLogInfo(void* messageStr) {
    if (!messageStr) return;
    std::string message = MonoBridge::Get().ExtractString(messageStr);
    std::string source = ResolveCallerAssemblyName();
    LOG_INFO("[" + source + "] " + message);
    ModLog::Get().Log(source, message, LogLevel::Info);
}

void __cdecl HookManager::NativeLogWarn(void* messageStr) {
    if (!messageStr) return;
    std::string message = MonoBridge::Get().ExtractString(messageStr);
    std::string source = ResolveCallerAssemblyName();
    LOG_WARN("[" + source + "] " + message);
    ModLog::Get().Log(source, message, LogLevel::Warn);
}

void __cdecl HookManager::NativeLogError(void* messageStr) {
    if (!messageStr) return;
    std::string message = MonoBridge::Get().ExtractString(messageStr);
    std::string source = ResolveCallerAssemblyName();
    LOG_ERROR("[" + source + "] " + message);
    ModLog::Get().Log(source, message, LogLevel::Error);
}

void __cdecl HookManager::NativeLogDebug(void* messageStr) {
    if (!messageStr) return;
    std::string message = MonoBridge::Get().ExtractString(messageStr);
    std::string source = ResolveCallerAssemblyName();
    LOG_DEBUG("[" + source + "] " + message);
    ModLog::Get().Log(source, message, LogLevel::Debug);
}

// RAW LOG - Data channel for mods (S3MM.Log.Raw). No length cap (1M sanity bound), no timestamp/level tag, payload verbatim,
// but each line is prefixed with [ModName] so payload lines are attributable and filterable in the shared S3MM.log.
// Deliberately NOT mirrored into the ModLog ring (it evicts at 1000 entries - lossy for bulk data)
void __cdecl HookManager::NativeLogRaw(void* messageStr) {
    if (!messageStr) return;
    std::string message = MonoBridge::Get().ExtractString(messageStr, 1 << 20);
    std::string source = ResolveCallerAssemblyName();
    S3MM::Logger::Get().LogRaw("[" + source + "] " + message);
}

// NATIVE DISPATCH - Entry point for instance method hooks, starts chain from hook[0]
void* __cdecl HookManager::NativeDispatch(uint32_t methodToken, void* thisPtr, void* argsArray) {
    auto& mgr = HookManager::Get();

    // Look up the chain by token
    auto it = mgr.m_tokenToMethod.find(methodToken);
    if (it == mgr.m_tokenToMethod.end()) {
        LOG_ERROR("[HookManager] NativeDispatch: unknown method token " + std::to_string(methodToken));
        return nullptr;
    }

    void* originalMethod = it->second;
    HookChain* chain = mgr.GetChain(originalMethod);
    if (!chain) {
        LOG_ERROR("[HookManager] NativeDispatch: no chain for method");
        return nullptr;
    }

    if (chain->hooks.empty()) {
        LOG_WARN("[HookManager] NativeDispatch: chain has no hooks, calling trampoline");
        std::vector<void*> unboxedParams = UnboxParametersFast(argsArray, chain);
        void** params = unboxedParams.empty() ? nullptr : unboxedParams.data();
        return mgr.InvokeOriginal(originalMethod, thisPtr, params);
    }

    // Create chain state starting at hook[0]
    ChainExecutionState newState;
    newState.methodToken = methodToken;
    newState.chain = chain;
    newState.nextHookIndex = 1; // Will invoke hook[0] below, next call goes to hook[1]
    newState.thisPtr = thisPtr;
    newState.paramStorage = UnboxParametersFast(argsArray, chain);
    newState.params = newState.paramStorage.empty() ? nullptr : newState.paramStorage.data();
    newState.callDepth = 1;

    t_chainStack.push_back(std::move(newState));
    // NOTE: newState is moved-from after this point, read from stack entry instead
    auto& stackEntry = t_chainStack.back();

    LOG_HOOK_TRACE("[HookManager] NativeDispatch: starting chain for " + chain->targetClass + "::" + chain->targetMethod + " (" + std::to_string(chain->hooks.size()) + " hooks)");

    // Invoke hook[0]
    const HookEntry& firstHook = chain->hooks[0];

    // For instance method chains, prepend thisPtr to params (same as NativeCallNext). Hooks are static but expect 'this' as first C# parameter.
    void** invokeParams = stackEntry.params;
    std::vector<void*> extendedParams;

    if (chain->originalIsInstance) {
        size_t paramCount = stackEntry.paramStorage.size();
        extendedParams.reserve(paramCount + 1);
        extendedParams.push_back(thisPtr); // this -> hook's first param
        for (size_t i = 0; i < paramCount; i++) { extendedParams.push_back(stackEntry.params[i]); }
        invokeParams = extendedParams.data();
    }

    MonoBridge::Get().SetupTLS();
    MonoInvokeState invokeState = {};
    bool success;
    {
        InvokeBridge bridge;
        success = MonoBridge::Get().InvokeMethod(firstHook.hookMethod,
            nullptr, // hooks are static, no 'this'
            invokeParams, &invokeState);
    }

    if (!success) {
        std::string excMsg = ExtractExceptionInfo(invokeState.pThrowExc);
        LOG_ERROR("[HookManager] Exception in dispatch hook (" + firstHook.modName + ") for " + chain->targetClass + "::" + chain->targetMethod + ": " + excMsg);
        ModLog::Get().Log(firstHook.modName, "EXCEPTION in " + chain->targetClass + "::" + chain->targetMethod + ": " + excMsg, LogLevel::Error);
    }

    // The invoked method here is the HOOK, not the original, so the value that came back is typed by the hook's signature
    // A correctly-declared hook returns the target's return type, and mismatches are already demoted by DemoteIneligibleTypedHooks, so the chain's cached return info is the right descriptor - and isObject is consulted first regardless
    void* result = BoxInvokeResult(chain, invokeState, "NativeDispatch");

    // Clean up chain state
    if (!t_chainStack.empty() && t_chainStack.back().methodToken == methodToken) { t_chainStack.pop_back(); }

    LOG_HOOK_TRACE("[HookManager] NativeDispatch: chain complete");
    return result;
}

// NATIVE SET CURRENT TOKEN - Sets method token in managed TLS for dispatch stub routing
void __cdecl HookManager::NativeSetCurrentToken(uint32_t token) {
    // This is called from managed code (InstanceDispatch.SetCurrentToken).
    // The token is stored in managed TLS, so nothing to do on native side. The function exists just to provide the internal call binding.
    LOG_HOOK_TRACE("[HookManager] NativeSetCurrentToken: " + std::to_string(token));
}

} // namespace S3MM
