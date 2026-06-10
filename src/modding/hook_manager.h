#pragma once
#include "../core/mono_types.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>

namespace S3MM {

// HOOK TYPE - Standard hook vs read-only watcher
enum class HookType {
    Hook,   // Standard hook - call CallNext() to continue chain
    Watcher // Read-only observer - should always call CallNext()
};

// HOOK ENTRY - A single hook registered by a mod
struct HookEntry {
    std::string modName;        // Which mod registered this hook
    void* hookMethod = nullptr; // MonoMethod* of the hook function (or wrapper for instance methods)
    HookType type = HookType::Hook;
    int priority = 0;        // Higher = runs first
    bool hasWrapper = false; // True if hookMethod is a generated dispatch wrapper
};

// HOOK CHAIN - All hooks targeting a single method
struct HookChain {
    void* originalMethod = nullptr;   // The original game MonoMethod*
    void* trampolineMethod = nullptr; // Copy for calling original (backed up values)

    // Backed-up original values for trampoline invocation
    void* originalKlass = nullptr;
    uint32_t originalToken = 0;
    void* originalRuntimeMethod = nullptr;

    std::string targetClass;  // e.g., "Sims3.Gameplay.Sim"
    std::string targetMethod; // e.g., "DoThing"
    std::string signature;    // e.g., "(int,int)" for overload matching

    std::vector<HookEntry> hooks;    // Sorted by priority (high to low)
    std::vector<HookEntry> watchers; // Read-only observers (very high priority)

    bool isPatched = false;      // Has the dispatch stub been installed?
    bool needsReinstall = false; // Does the dispatch stub need updating? (after hot-reload)

    // The RuntimeMethod* transplanted onto the original at first install
    void* installedRTM = nullptr;
    bool originalIsInstance = false;   // True if original method is instance (not static)
    bool needsIdentityRestore = false; // True if identity was clobbered (deferred proxy compilation failed)

    // CACHED SIGNATURE INFO - Avoid re-reading signature every call
    bool signatureCached = false; // Has signature been cached?
    MonoTypeCode cachedReturnType = MonoTypeCode::Void;
    void* cachedReturnKlass = nullptr; // For value types that need boxing
    std::vector<MonoTypeCode> cachedParamTypes;
    std::vector<bool> cachedParamIsValueType; // True if param needs unboxing
    uint16_t cachedParamCount = 0;
    bool returnIsPrimitive = false; // True if return is bool/char/i1-u8/r4/r8
    bool returnIsVoid = false;      // True if void return (skip boxing entirely)
};

// CHAIN EXECUTION STATE - Thread-local position in hook chain, supports nested/recursive chains via stack
struct ChainExecutionState {
    uint32_t methodToken;            // Which chain we're executing
    HookChain* chain;                // Pointer to the chain
    size_t nextHookIndex;            // Next hook to invoke (starts at 1, since 0 is already running)
    void* thisPtr;                   // Cached this pointer
    void** params;                   // Cached unboxed params
    std::vector<void*> paramStorage; // Storage for unboxed params (prevent dangling pointers)
    int callDepth;                   // Tracks nested CallNext calls; pop state when reaches 0
};

// HOOK MANAGER - Manages hook chains and dispatches calls
class HookManager {
  public:
    static HookManager& Get();

    // Initialize hook manager - registers internal calls
    bool Initialize();

    // Register a hook for a target method (string-based lookup), signature is optional for overload resolution
    bool RegisterHook(const std::string& targetClass, const std::string& targetMethod, void* hookMethod, HookType type, const std::string& modName, int priority = 0, const std::string& signature = "");

    // Register a hook by raw MonoMethod* pointers (no string lookup)
    bool RegisterHookByPointer(void* targetMethod, void* hookMethod, HookType type, const std::string& modName, int priority = 0);

    // InternalCall: Register a hook by pointer from managed code
    static void __cdecl NativeRegisterHookByPointer(void* targetMethod, void* hookMethod, int32_t hookType, int32_t priority);

    // Remove all hooks from a specific mod (for hot-reload), marks affected chains for reinstall
    int RemoveHooksForMod(const std::string& modName);

    // Apply all pending hooks - patches target methods to dispatch stubs
    int ApplyHooks();

    // Reinstall dispatch stubs for chains that need updating (unlike ApplyHooks, updates already-patched chains too)
    int ReinstallDispatchStubs();

    // Get a hook chain by target method
    HookChain* GetChain(void* originalMethod);
    HookChain* GetChainByName(const std::string& targetClass, const std::string& targetMethod);

    // Get all hook chains
    const std::unordered_map<void*, HookChain>& GetAllChains() const { return m_chains; }

    // Invoke the original method (trampoline) from managed code, returns the result
    void* InvokeOriginal(void* originalMethod, void* thisPtr, void** params);

    // Invoke the base method (skip all hooks) - called from managed code
    void* InvokeBase(void* originalMethod, void* thisPtr, void** params);

    // Native callbacks for managed code
    static void* __cdecl NativeCallNext(uint32_t methodToken, void* thisPtr, void* argsArray);
    static void* __cdecl NativeCallOriginal(uint32_t methodToken, void* thisPtr, void* argsArray);

    // Get target token by walking interpreter frames to find the calling hook (uses RuntimeMethod->method pointer)
    static uint32_t __cdecl NativeGetCallerHookToken();

    // Auto-prefixed log ICALLs - resolve caller assembly name from interpreter frames
    static void __cdecl NativeLogInfo(void* messageStr);
    static void __cdecl NativeLogWarn(void* messageStr);
    static void __cdecl NativeLogError(void* messageStr);
    static void __cdecl NativeLogDebug(void* messageStr);

    // Chain dispatch - entry point for instance method hooks via InstanceDispatch stubs
    static void* __cdecl NativeDispatch(uint32_t methodToken, void* thisPtr, void* argsArray);

    // Set current token for InstanceDispatch stubs (called before dispatch stub runs)
    static void __cdecl NativeSetCurrentToken(uint32_t token);

  private:
    HookManager() = default;
    ~HookManager() = default;
    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;

    // Find the target method in game assemblies, signature is optional for overload resolution
    void* FindTargetMethod(const std::string& targetClass, const std::string& targetMethod, const std::string& signature = "");

    // Parse signature string into vector of type names
    static std::vector<std::string> ParseSignature(const std::string& signature);

    // Check if a method matches the given signature
    bool MethodMatchesSignature(void* method, const std::vector<std::string>& paramTypes);

    // Convert type name to MonoTypeCode (returns Void if unknown)
    static MonoTypeCode TypeNameToCode(const std::string& typeName);

    // Create backup of original method for trampoline
    void BackupOriginalMethod(HookChain& chain);

    // Install the dispatch stub on a method
    bool InstallDispatchStub(HookChain& chain);

    // Cache signature info for a chain (speedup: avoid re-reading every call)
    void CacheSignatureInfo(HookChain& chain);

    // Check if a type code represents a primitive (for boxing optimization)
    static bool IsPrimitiveType(MonoTypeCode code) { return code >= MonoTypeCode::Boolean && code <= MonoTypeCode::R8; }

    bool m_initialized = false;

    // Map from original method pointer to hook chain
    std::unordered_map<void*, HookChain> m_chains;

    // Map from method token to original method pointer (for fast dispatch lookup)
    std::unordered_map<uint32_t, void*> m_tokenToMethod;

    // Map from hook method pointer to target method token (for auto-token resolution in hook[0])
    std::unordered_map<void*, uint32_t> m_hookMethodToToken;

    // Map from (image, hookToken) to target method token; key is (imagePtr << 32) | token to avoid cross-assembly collisions
    std::unordered_map<uint64_t, uint32_t> m_hookTokenToTargetToken;

    // Helper to create composite key for hookTokenToTargetToken
    static uint64_t MakeHookTokenKey(void* image, uint32_t token) { return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(image)) << 32) | token; }
};

} // namespace S3MM
