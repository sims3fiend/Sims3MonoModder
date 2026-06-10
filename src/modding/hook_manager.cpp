#include "hook_manager.h"
#include "method_patcher.h"
#include "mod_loader.h"
#include "audit_api.h"
#include "../core/addresses.h"
#include "../core/mono_bridge.h"
#include "../cache/cache_api.h"
#include "../util/logger.h"
#include "../util/mod_log.h"
#include "../core/exception_helper.h"
#include <algorithm>
#include <Windows.h>

namespace S3MM {

// THREAD-LOCAL CHAIN STATE - Stack of active chain executions, supports nested/recursive hooks
thread_local std::vector<ChainExecutionState> t_chainStack;

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

    // Auto-prefixed log API (S3MM.Log class)
    MonoBridge::Get().AddInternalCall("S3MM.Log::Info", (void*)&HookManager::NativeLogInfo);
    MonoBridge::Get().AddInternalCall("S3MM.Log::Warn", (void*)&HookManager::NativeLogWarn);
    MonoBridge::Get().AddInternalCall("S3MM.Log::Error", (void*)&HookManager::NativeLogError);
    MonoBridge::Get().AddInternalCall("S3MM.Log::Debug", (void*)&HookManager::NativeLogDebug);

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

    // Hook by pointer
    MonoBridge::Get().AddInternalCall("S3MM.Hook::RegisterByPointer", (void*)&HookManager::NativeRegisterHookByPointer);

    // Audit API - hook/conflict introspection for managed diagnostic tools (e.g. NRaas ErrorTrap)
    MonoBridge::Get().AddInternalCall("S3MM.Audit::IsMethodHooked", (void*)&AuditAPI::NativeIsMethodHooked);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::IsAssemblyReplaced", (void*)&AuditAPI::NativeIsAssemblyReplaced);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetHookedMethodCount", (void*)&AuditAPI::NativeGetHookedMethodCount);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetAllHooks", (void*)&AuditAPI::NativeGetAllHooks);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetHookChain", (void*)&AuditAPI::NativeGetHookChain);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetReplacedAssemblies", (void*)&AuditAPI::NativeGetReplacedAssemblies);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetLoadedModCount", (void*)&AuditAPI::NativeGetLoadedModCount);
    MonoBridge::Get().AddInternalCall("S3MM.Audit::GetLoadedMods", (void*)&AuditAPI::NativeGetLoadedMods);

    m_initialized = true;
    LOG_INFO("[HookManager] Initialized");
    return true;
}

// HOOK REGISTRATION - Register a hook or watcher by class/method name
bool HookManager::RegisterHook(const std::string& targetClass, const std::string& targetMethod, void* hookMethod, HookType type, const std::string& modName, int priority, const std::string& signature) {
    if (!hookMethod) {
        LOG_ERROR("[HookManager] RegisterHook: null hook method");
        return false;
    }

    std::string sigInfo = signature.empty() ? "" : signature;
    LOG_INFO("[HookManager] Registering " + std::string(type == HookType::Hook ? "hook" : "watcher") + ": " + targetClass + ":" + targetMethod + sigInfo + " from mod: " + modName);

    // Find or create the hook chain
    void* originalMethod = FindTargetMethod(targetClass, targetMethod, signature);
    if (!originalMethod) {
        LOG_ERROR("[HookManager] Target method not found: " + targetClass + ":" + targetMethod + sigInfo);
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

    // Track token -> method mapping for dispatch.
    // Use chain.originalToken (backed up before patching) for consistency; the live MonoMethodLayout is now identity-preserving but the backup is the canonical source.
    uint32_t origToken = chain.originalToken;
    m_tokenToMethod[origToken] = originalMethod;

    // Track hook method -> token for auto-token resolution via GetCallerHookToken frame walk
    m_hookMethodToToken[hookMethod] = origToken;

    // Also store by (image, hookToken) composite key, more reliable since MonoMethod* can differ at runtime.
    // Using a composite key because hook tokens can collide across different mod assemblies.
    auto* hookLayout = reinterpret_cast<MonoMethodLayout*>(hookMethod);
    void* hookImage = hookLayout->klass ? *(void**)((uintptr_t)hookLayout->klass + Offsets::CLASS_IMAGE) : nullptr;
    uint64_t compositeKey = MakeHookTokenKey(hookImage, hookLayout->token);
    m_hookTokenToTargetToken[compositeKey] = origToken;
    LOG_DEBUG("[HookManager] Stored hook method @ " + std::to_string((uintptr_t)hookMethod) + " (hookToken=" + std::to_string(hookLayout->token) + ", image=" + std::to_string((uintptr_t)hookImage) + ")" +
              " -> targetToken " + std::to_string(origToken));

    LOG_INFO("[HookManager] Hook registered. Chain now has " + std::to_string(chain.hooks.size()) + " hooks (" + std::to_string(chain.watchers.size()) + " watchers)");

    return true;
}

// HOOK REGISTRATION BY POINTER - Register using raw MonoMethod* directly
bool HookManager::RegisterHookByPointer(void* targetMethod, void* hookMethod, HookType type, const std::string& modName, int priority) {
    if (!targetMethod || !hookMethod) {
        LOG_ERROR("[HookManager] RegisterHookByPointer: null target or hook method");
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
        targetMethodName = targetLayout->name ? targetLayout->name : "???";
        if (targetLayout->klass) {
            const char* ns = *(const char**)((uintptr_t)targetLayout->klass + Offsets::CLASS_NAMESPACE);
            const char* cls = *(const char**)((uintptr_t)targetLayout->klass + Offsets::CLASS_NAME);
            if (ns && ns[0])
                targetClassName = std::string(ns) + "." + (cls ? cls : "???");
            else
                targetClassName = cls ? cls : "???";
        }
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

    // Track token -> method mapping for dispatch (use ORIGINAL token, not patched)
    m_tokenToMethod[originalToken] = targetMethod;

    // Track hook method -> token for auto-token resolution via GetCallerHookToken
    m_hookMethodToToken[hookMethod] = originalToken;

    // Also store by (image, hookToken) composite key
    auto* hookLayout = reinterpret_cast<MonoMethodLayout*>(hookMethod);
    void* hookImage = hookLayout->klass ? *(void**)((uintptr_t)hookLayout->klass + Offsets::CLASS_IMAGE) : nullptr;
    uint64_t compositeKey = MakeHookTokenKey(hookImage, hookLayout->token);
    m_hookTokenToTargetToken[compositeKey] = originalToken;

    LOG_INFO("[HookManager] Hook registered by pointer. Chain now has " + std::to_string(chain.hooks.size()) + " hooks (" + std::to_string(chain.watchers.size()) + " watchers)" +
             ", originalToken=" + std::to_string(originalToken));

    return true;
}

// NativeRegisterHookByPointer - internal call entry point from managed code
void __cdecl HookManager::NativeRegisterHookByPointer(void* targetMethod, void* hookMethod, int32_t hookType, int32_t priority) {
    // Derive mod name from the hook method's image.
    std::string modName = "unknown";
    if (hookMethod) {
        auto* hookLayout = reinterpret_cast<MonoMethodLayout*>(hookMethod);
        if (hookLayout->klass) {
            void* image = *(void**)((uintptr_t)hookLayout->klass + Offsets::CLASS_IMAGE);
            if (image) {
                // Prefer the logical mod name (stable across hot-reloads). Fall back to the assembly name for non-S3MM/standalone images.
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

    HookType type = (hookType == 1) ? HookType::Watcher : HookType::Hook;
    HookManager::Get().RegisterHookByPointer(targetMethod, hookMethod, type, modName, priority);
}

// REMOVE HOOKS FOR MOD - Unregister all hooks belonging to a given mod name
int HookManager::RemoveHooksForMod(const std::string& modName) {
    int removed = 0;

    int staleMapEntries = 0;

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

        if (InstallDispatchStub(chain)) { applied++; }
    }

    LOG_INFO("[HookManager] Applied " + std::to_string(applied) + " hook chains");
    return applied;
}

// REINSTALL DISPATCH STUBS - Update stubs after hot-reload or chain changes
int HookManager::ReinstallDispatchStubs() {
    int reinstalled = 0;

    for (auto& [method, chain] : m_chains) {
        if (!chain.needsReinstall) continue;

        // Clear the flag first
        chain.needsReinstall = false;

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
                layout->runtimeMethod = nullptr; // Force recompilation with original IL
                VirtualProtect(chain.originalMethod, sizeof(MonoMethodLayout), oldProtect, &oldProtect);

                chain.isPatched = false;
                chain.installedRTM = nullptr; // RTM no longer referenced by the original; a future re-hook does a fresh transplant
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
        if (MethodPatcher::Get().ForcePatchMethod(chain.originalMethod, newEntryHook, &deferred, &chain.installedRTM)) {
            chain.needsIdentityRestore = deferred;
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
    for (auto& [method, chain] : m_chains) {
        if (chain.targetClass == targetClass && chain.targetMethod == targetMethod) { return &chain; }
    }
    return nullptr;
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
void* HookManager::FindTargetMethod(const std::string& targetClass, const std::string& targetMethod, const std::string& signature) {
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

        // Fallback: try the old type-code-based matching for simple cases where the user wrote "int,int" and Mono returns "int,int".
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

// BACKUP ORIGINAL METHOD - Snapshot original MonoMethod fields and create trampoline
void HookManager::BackupOriginalMethod(HookChain& chain) {
    auto* layout = reinterpret_cast<MonoMethodLayout*>(chain.originalMethod);

    chain.originalKlass = layout->klass;
    chain.originalToken = layout->token;
    chain.originalRuntimeMethod = layout->runtimeMethod;

    // Create a heap-allocated trampoline copy of the MonoMethod that retains the original values and can be used to invoke the original.
    auto* trampoline = new MonoMethodLayout(*layout);

    // CRITICAL: Clear runtimeMethod on the trampoline to force fresh compilation.
    // The original runtimeMethod contains RuntimeMethod->method which points back to the ORIGINAL MonoMethod.
    // After we patch the original, that back-pointer leads to the patched (hook) token/klass, causing crashes or wrong behavior.
    // By clearing runtimeMethod, Mono will compile fresh for the trampoline, with RuntimeMethod->method pointing to our trampoline copy.
    trampoline->runtimeMethod = nullptr;

    chain.trampolineMethod = trampoline;

    LOG_DEBUG("[HookManager] Backed up original method: token=" + std::to_string(chain.originalToken) + ", trampoline=" + std::to_string((uintptr_t)trampoline) + " (runtimeMethod cleared for fresh compilation)");

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

    auto* layout = reinterpret_cast<MonoMethodLayout*>(chain.originalMethod);
    void* sig = layout->signature;

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

    for (uint16_t i = 0; i < chain.cachedParamCount; i++) {
        void* paramType = *(void**)((uintptr_t)sig + Offsets::SIG_PARAMS + i * sizeof(void*));
        if (paramType) {
            MonoTypeCode code = (MonoTypeCode)(*(uint8_t*)((uintptr_t)paramType + Offsets::TYPE_CODE));
            chain.cachedParamTypes[i] = code;
            // Value types need unboxing (primitives and ValueType structs)
            chain.cachedParamIsValueType[i] = IsPrimitiveType(code) || code == MonoTypeCode::ValueType;
        } else {
            chain.cachedParamTypes[i] = MonoTypeCode::Object;
            chain.cachedParamIsValueType[i] = false;
        }
    }

    chain.signatureCached = true;
    LOG_DEBUG("[HookManager] Cached signature: returnType=" + std::to_string((int)chain.cachedReturnType) + ", params=" + std::to_string(chain.cachedParamCount) +
              ", isPrimitive=" + std::to_string(chain.returnIsPrimitive) + ", isVoid=" + std::to_string(chain.returnIsVoid));
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

    // ForcePatchMethod compiles hook IL via a heap proxy and sets the fresh RTM on the original.
    // Bypasses stale domain hash entries and preserves the original's identity fields.
    // If proxy compilation fails (never-compiled method), falls back to clobbering token/klass for lazy first-call compilation. deferred identity restoration happens in NativeGetCallerHookToken.
    bool deferred = false;
    if (!MethodPatcher::Get().ForcePatchMethod(chain.originalMethod, entryHook, &deferred, &chain.installedRTM)) {
        LOG_ERROR("[HookManager] Failed to patch method: " + chain.targetClass + "::" + chain.targetMethod);
        return false;
    }

    chain.isPatched = true;
    chain.needsIdentityRestore = deferred;
    LOG_INFO("[HookManager] Installed dispatch stub: " + chain.targetClass + "::" + chain.targetMethod);

    return true;
}

// INVOKE ORIGINAL - Call the trampoline to execute unhooked original IL
void* HookManager::InvokeOriginal(void* originalMethod, void* thisPtr, void** params) {
    LOG_DEBUG("[HookManager] InvokeOriginal: method=" + std::to_string((uintptr_t)originalMethod));

    auto* chain = GetChain(originalMethod);
    if (!chain) {
        LOG_ERROR("[HookManager] InvokeOriginal: no chain for method");
        return nullptr;
    }

    LOG_DEBUG("[HookManager] InvokeOriginal: found chain for " + chain->targetClass + "::" + chain->targetMethod);

    // Use the heap-allocated trampoline that was created during backup (it has the original token/klass/runtimeMethod values).
    if (!chain->trampolineMethod) {
        LOG_ERROR("[HookManager] InvokeOriginal: no trampoline method available");
        return nullptr;
    }

    auto* trampoline = reinterpret_cast<MonoMethodLayout*>(chain->trampolineMethod);
    bool needsCompile = (trampoline->runtimeMethod == nullptr);

    LOG_DEBUG("[HookManager] InvokeOriginal: using trampoline=" + std::to_string((uintptr_t)trampoline) + ", token=" + std::to_string(trampoline->token) +
              ", runtime=" + std::to_string((uintptr_t)trampoline->runtimeMethod) + (needsCompile ? " (first-time compile)" : ""));

    // Ensure TLS is set up for Mono operations
    MonoBridge::Get().SetupTLS();

    // Invoke using the trampoline
    MonoInvokeState state = {};
    bool invokeResult = MonoBridge::Get().InvokeMethod(chain->trampolineMethod, thisPtr, params, &state);

    LOG_DEBUG("[HookManager] InvokeOriginal: invoke returned " + std::to_string(invokeResult) + ", pRet=" + std::to_string((uintptr_t)state.pRet) + ", pRetVal=" + std::to_string((uintptr_t)state.pRetVal));

    // If the trampoline was compiled during this call, update it
    if (trampoline->runtimeMethod && !chain->originalRuntimeMethod) {
        chain->originalRuntimeMethod = trampoline->runtimeMethod;
        LOG_DEBUG("[HookManager] InvokeOriginal: captured new runtimeMethod=" + std::to_string((uintptr_t)trampoline->runtimeMethod));
    }

    // WORKAROUND: Mono 1.2 interpreter has a bug where first-time compilation + execution of a trampoline can fail, but the compilation side-effect persists.
    // If we needed compilation (runtimeMethod was null) and the invoke failed but now runtimeMethod is set, retry the invocation - it should succeed with the cached RuntimeMethod.
    if (!invokeResult && needsCompile && trampoline->runtimeMethod != nullptr) {
        LOG_WARN("[HookManager] InvokeOriginal: first-time compile failed, retrying with cached RuntimeMethod");

        // Clear state and retry
        memset(&state, 0, sizeof(state));
        invokeResult = MonoBridge::Get().InvokeMethod(chain->trampolineMethod, thisPtr, params, &state);

        LOG_DEBUG("[HookManager] InvokeOriginal: retry returned " + std::to_string(invokeResult) + ", pRet=" + std::to_string((uintptr_t)state.pRet) + ", pRetVal=" + std::to_string((uintptr_t)state.pRetVal));
    }

    // Use cached signature info to avoid re-reading signature every call. Ensure signature is cached (should already be from BackupOriginalMethod).
    if (!chain->signatureCached) {
        // Fallback: cache now if somehow missed
        CacheSignatureInfo(*chain);
    }

    MonoTypeCode typeCode = chain->cachedReturnType;

    // Enhanced diagnostics for return value handling
    LOG_DEBUG("[HookManager] InvokeOriginal: typeCode=" + std::to_string((int)typeCode) + ", isObject=" + std::to_string(state.isObject) + ", pRet=" + std::to_string((uintptr_t)state.pRet) +
              ", pRetVal=" + std::to_string((uintptr_t)state.pRetVal));

    // FAST PATH: void return - nothing to return
    if (chain->returnIsVoid) { return nullptr; }

    // Use isObject as the authoritative check for where the return value lives.
    // The interpreter sets isObject=1 for reference types, isObject=0 for value types.
    // This is more reliable than type code matching, especially for private nested classes, generic instantiations, and edge cases in type resolution.
    if (state.isObject) {
        LOG_DEBUG("[HookManager] InvokeOriginal: isObject=1, returning pRetVal=" + std::to_string((uintptr_t)state.pRetVal));
        return state.pRetVal;
    }

    // Diagnostic: isObject should be authoritative, but warn if typeCode disagrees
    if (typeCode == MonoTypeCode::Class || typeCode == MonoTypeCode::Object || typeCode == MonoTypeCode::String || typeCode == MonoTypeCode::SzArray || typeCode == MonoTypeCode::GenericInst) {
        LOG_WARN("[HookManager] InvokeOriginal: isObject=0 but typeCode=" + std::to_string((int)typeCode) + " suggests reference type. " + "pRetVal=" + std::to_string((uintptr_t)state.pRetVal) +
                 ", pRet=" + std::to_string((uintptr_t)state.pRet));
    }

    // For value types (int, bool, float, etc.), we need to box the value. Use cached klass if available, otherwise look up from mono_defaults.
    void* valueKlass = chain->cachedReturnKlass;
    if (!valueKlass) {
        // For primitive types, look up the class from mono_defaults (cached at known addresses in the Sims 3 executable).
        valueKlass = GetPrimitiveKlass(typeCode);
        if (!valueKlass) {
            LOG_WARN("[HookManager] InvokeOriginal: unknown primitive type " + std::to_string((int)typeCode));
            return state.pRet;
        }
        // Cache it for next time
        chain->cachedReturnKlass = valueKlass;
        LOG_DEBUG("[HookManager] InvokeOriginal: cached primitive klass=" + std::to_string((uintptr_t)valueKlass));
    }

    // pRet points to the raw value bytes
    if (!state.pRet) {
        LOG_WARN("[HookManager] InvokeOriginal: pRet is null for value type");
        return nullptr;
    }

    // Box the value using pRet (where the actual value is stored)
    void* boxedValue = MonoBridge::Get().BoxValue(valueKlass, state.pRet);
    LOG_DEBUG("[HookManager] InvokeOriginal: boxed value = " + std::to_string((uintptr_t)boxedValue));

    return boxedValue;
}

// INVOKE BASE - Skip all hooks, same as InvokeOriginal for now
void* HookManager::InvokeBase(void* originalMethod, void* thisPtr, void** params) {
    // Same as InvokeOriginal for now - both skip all hooks
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

    if (!argsArray) return unboxedParams;

    int32_t arrayLen = MonoBridge::Get().GetArrayLength(argsArray);
    if (arrayLen <= 0) return unboxedParams;

    unboxedParams.resize(arrayLen);
    void** arrayElements = (void**)((uintptr_t)argsArray + 0x10);

    // Use cached param info if available and matches
    bool useCached = chain && chain->signatureCached && (size_t)arrayLen == chain->cachedParamIsValueType.size();

    for (int32_t i = 0; i < arrayLen; i++) {
        void* boxedValue = arrayElements[i];

        if (!boxedValue) {
            unboxedParams[i] = nullptr;
            continue;
        }

        // Check for Hook.Ref<T> byref wrapper pattern (runtime check).
        // This heuristic treats ANY single-element array as a byref wrapper, passing &array[0] instead of the array object.
        // Hook.Ref<T> has a struct constraint on the managed side to prevent accidental byref treatment of legitimate single-element arrays.
        // For ref-type byrefs (e.g. ref uint[]), modders must manually create T[1] wrappers.
        // TODO: Could use a distinct marker type instead of the length-1 heuristic.
        void* klass = MonoBridge::Get().GetObjectClass(boxedValue);
        if (klass) {
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

    // Check if we have an active chain state for this token
    ChainExecutionState* state = FindChainState(methodToken);
    bool isNewChain = (state == nullptr);

    // FAST PATH: Single-entry chain (the lone entry may be a hook OR a watcher - watchers live in chain.hooks too), first call.
    // That entry is hook[0], already invoked by the dispatch stub before it called us, so nothing is skipped - skip chain state, go straight to trampoline.
    if (isNewChain && chain->hooks.size() == 1) {
        LOG_DEBUG("[HookManager] NativeCallNext: FAST PATH for single-entry chain " + chain->targetClass + "::" + chain->targetMethod);

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

        LOG_DEBUG("[HookManager] NativeCallNext: started chain for " + chain->targetClass + "::" + chain->targetMethod + " (" + std::to_string(chain->hooks.size()) + " hooks, " + std::to_string(chain->watchers.size()) +
                  " watchers)");
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

        LOG_DEBUG("[HookManager] NativeCallNext: invoking hook[" + std::to_string(nextIdx) + "] from " + nextHook.modName);

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
        bool success = MonoBridge::Get().InvokeMethod(nextHook.hookMethod, invokeThisPtr, invokeParams, &invokeState);

        if (!success) {
            std::string excMsg = ExtractExceptionInfo(invokeState.pThrowExc);
            LOG_ERROR("[HookManager] Exception in hook[" + std::to_string(nextIdx) + "] (" + nextHook.modName + ") for " + chain->targetClass + "::" + chain->targetMethod + ": " + excMsg);
            ModLog::Get().Log(nextHook.modName, "EXCEPTION in " + chain->targetClass + "::" + chain->targetMethod + ": " + excMsg, LogLevel::Error);
        }

        result = invokeState.pRetVal ? invokeState.pRetVal : invokeState.pRet;
    } else {
        // No more hooks - invoke the actual original (trampoline)
        LOG_DEBUG("[HookManager] NativeCallNext: invoking trampoline");
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
        LOG_DEBUG("[HookManager] NativeCallNext: chain complete, returning result");
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
                return chainIt->second.originalToken;
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

    LOG_DEBUG("[HookManager] NativeDispatch: starting chain for " + chain->targetClass + "::" + chain->targetMethod + " (" + std::to_string(chain->hooks.size()) + " hooks)");

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
    bool success = MonoBridge::Get().InvokeMethod(firstHook.hookMethod,
        nullptr, // hooks are static, no 'this'
        invokeParams, &invokeState);

    if (!success) {
        std::string excMsg = ExtractExceptionInfo(invokeState.pThrowExc);
        LOG_ERROR("[HookManager] Exception in dispatch hook (" + firstHook.modName + ") for " + chain->targetClass + "::" + chain->targetMethod + ": " + excMsg);
        ModLog::Get().Log(firstHook.modName, "EXCEPTION in " + chain->targetClass + "::" + chain->targetMethod + ": " + excMsg, LogLevel::Error);
    }

    void* result = invokeState.pRetVal ? invokeState.pRetVal : invokeState.pRet;

    // Clean up chain state
    if (!t_chainStack.empty() && t_chainStack.back().methodToken == methodToken) { t_chainStack.pop_back(); }

    LOG_DEBUG("[HookManager] NativeDispatch: chain complete");
    return result;
}

// NATIVE SET CURRENT TOKEN - Sets method token in managed TLS for dispatch stub routing
void __cdecl HookManager::NativeSetCurrentToken(uint32_t token) {
    // This is called from managed code (InstanceDispatch.SetCurrentToken).
    // The token is stored in managed TLS, so nothing to do on native side. The function exists just to provide the internal call binding.
    LOG_DEBUG("[HookManager] NativeSetCurrentToken: " + std::to_string(token));
}

} // namespace S3MM
