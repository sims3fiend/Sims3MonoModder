#pragma once
#include "../core/mono_types.h"
#include "chain_compiler.h"
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
    bool usesChain = false;  // Handler forwards via S3MM.Chain.* (bake candidate) - inferred at registration by ChainCompiler::HookUsesChain
};

// HOOK CHAIN - All hooks targeting a single method
struct HookChain {
    void* originalMethod = nullptr;   // The original game MonoMethod*
    void* trampolineMethod = nullptr; // Copy for calling original (backed up values)

    // Backed-up original values for trampoline invocation
    void* originalKlass = nullptr;
    uint32_t originalToken = 0;
    void* originalRuntimeMethod = nullptr;

    // DISPATCH TOKEN - opaque u32 round-tripped by managed code to find this chain (the m_tokenToMethod key)
    // == originalToken unless another mod's target already claimed it (MethodDef tokens collide across assemblies); then a synthetic key outside the 0x06xxxxxx MethodDef space
    // originalToken stays the real metadata token
    uint32_t dispatchToken = 0;

    std::string targetClass;  // e.g., "Sims3.Gameplay.Sim"
    std::string targetMethod; // e.g., "DoThing"
    std::string signature;    // e.g., "(int,int)" for overload matching

    std::vector<HookEntry> hooks;    // Sorted by priority (high to low)
    std::vector<HookEntry> watchers; // Read-only observers (very high priority)

    bool isPatched = false;       // Has the dispatch stub been installed?
    bool needsReinstall = false;  // Does the dispatch stub need updating? (after hot-reload)
    bool bakedActive = false;     // Installed via the baked ChainCompiler path (not the boxing dispatch stub)
    std::string bakeRejectReason; // Why this chain didn't bake ("" if baked or never attempted); a demoted hook still runs boxed

    // The RuntimeMethod* the dispatch stub lives in: the original's own RTM when it existed at install (overwritten in place), else the transplanted one
    void* installedRTM = nullptr;
    // Pre-overwrite bytes of the original's RTM, copied back on unhook so callers that bound that pointer run the original again
    RuntimeMethod savedOriginalRTM = {};
    bool originalRTMOverwritten = false;
    bool originalIsInstance = false;   // True if original method is instance (not static)
    bool needsIdentityRestore = false; // True if identity was clobbered (deferred proxy compilation failed)

    // CACHED SIGNATURE INFO - Avoid re-reading signature every call
    bool signatureCached = false; // Has signature been cached?
    MonoTypeCode cachedReturnType = MonoTypeCode::Void;
    void* cachedReturnKlass = nullptr; // For value types that need boxing
    std::vector<MonoTypeCode> cachedParamTypes;
    std::vector<bool> cachedParamIsValueType; // True if param needs unboxing
    std::vector<bool> cachedParamIsByRef;     // True if param is ref/out - gates the Hook.Ref single-element-array deref in UnboxParametersFast
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
    // Baking is inferred per handler at registration (ChainCompiler::HookUsesChain): Chain.* forwarders that ExactMatch bake, the rest run boxed
    bool RegisterHook(const std::string& targetClass, const std::string& targetMethod, void* hookMethod, HookType type, const std::string& modName, int priority = 0, const std::string& signature = "");

    // Register a hook by raw MonoMethod* pointers (no string lookup); baking is inferred (see RegisterHook)
    bool RegisterHookByPointer(void* targetMethod, void* hookMethod, HookType type, const std::string& modName, int priority = 0);

    // InternalCall: Register a hook by pointer from managed code (baking inferred from the handler's IL)
    static void __cdecl NativeRegisterHookByPointer(void* targetMethod, void* hookMethod, int32_t hookType, int32_t priority);

    // InternalCall: Apply pending hook changes without a mod reload
    // Reinstalls chains marked needsReinstall (recompiling baked chains in place) and installs any not-yet-patched chains
    static void __cdecl NativeApplyPendingHooks();

    // Remove all hooks from a specific mod (for hot-reload), marks affected chains for reinstall
    int RemoveHooksForMod(const std::string& modName);

    // Apply all pending hooks - patches target methods to dispatch stubs
    int ApplyHooks();

    // Reinstall dispatch stubs for chains that need updating (unlike ApplyHooks, updates already-patched chains too)
    int ReinstallDispatchStubs();

    // Per-mod install diagnostics: requested count + refusal records, surfaced by LogHookSummary and Audit.GetRefusedHooks
    struct HookRefusal {
        std::string target; // "Class:Method"
        std::string reason; // one-line refusal reason
    };
    struct ModHookStats {
        int requested = 0;
        std::vector<HookRefusal> refusals;
    };

    // Per-mod hook install summary block ("N requested, M installed, K REFUSED (reason)") - call after startup load, hot-reload, and runtime applies
    // onlyIfNewRefusals = print only mods with refusals, and only when refusal records grew since the last summary
    void LogHookSummary(bool onlyIfNewRefusals = false);

    // Per-mod request/refusal records for Audit.GetRefusedHooks
    const std::unordered_map<std::string, ModHookStats>& GetModHookStats() const { return m_modStats; }

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

    // InternalCall: 1 if the token's target is an instance method, else 0 (unknown token -> 0)
    // The boxing Chain.CallNext fallback needs hasThis to split a leading-self arg from the forwarded params
    static int32_t __cdecl NativeTargetIsInstance(uint32_t methodToken);

    // CALLER ICALLs (S3MM.Hook.Caller*) - the game frame that called the innermost hooked method; every call re-walks so nothing is captured unless asked
    // Method/Is are allocation-free, Name allocates the string, Instance is a pointer read (boxed copy for struct receivers), Args boxes a copy of every param
    static void* __cdecl NativeCallerMethod(); // canonical MonoMethod* (never a chain clone); null = native/engine caller or no hook context
    static void* __cdecl NativeCallerName();   // "Namespace.Class::Method" or null
    static int32_t __cdecl NativeCallerIs(void* nameStr);
    static void* __cdecl NativeCallerInstance();      // null for statics
    static void* __cdecl NativeCallerArgs();          // object[] in signature order, by-ref params deref'd; null if unreadable
    static uint32_t __cdecl NativeCallerMintOffset(); // frame->ip - rtm->code: tells call sites apart, never maps to IL

    // Auto-prefixed log ICALLs - resolve caller assembly name from interpreter frames
    static void __cdecl NativeLogInfo(void* messageStr);
    static void __cdecl NativeLogWarn(void* messageStr);
    static void __cdecl NativeLogError(void* messageStr);
    static void __cdecl NativeLogDebug(void* messageStr);
    static void __cdecl NativeLogRaw(void* messageStr);

    // Chain dispatch - entry point for instance method hooks via InstanceDispatch stubs
    static void* __cdecl NativeDispatch(uint32_t methodToken, void* thisPtr, void* argsArray);

    // Set current token for InstanceDispatch stubs (called before dispatch stub runs)
    static void __cdecl NativeSetCurrentToken(uint32_t token);

    // SHIM CONTINUATION - a ShimHook link's boxing Hook.CallNext routes here via NativeCallNext (which finds the calling link via FindCallerBakedLinkIndex)
    // Runs the rest of the chain from fromLinkIndex+1 by runtime-invoking that link's clone, or the trampoline past the last link
    // Boxed args in / boxed result out, matching the managed Hook.CallNext contract
    void* ContinueBakedChain(void* originalMethod, size_t fromLinkIndex, void* thisPtr, void* argsArray);

    // CRASH QUARANTINE - the VEH crash handler calls this to APPEND a crash record for the faulting baked target; returns true iff `method` is one
    // A target is refused only after kBakeQuarantineThreshold crashes across runs, so a transient fault never poisons a correct bake
    // Post-crash context: best-effort, no std::string, no throws
    bool QuarantineBakedChain(void* method, const char* reason);

    // CRASH FORENSICS - companion to QuarantineBakedChain; appends a typed record of a baked-chain fault to <S3MM>/baked_crashes.log (call after it returns true)
    // Attributes the faulting LINK and its MINT ip offset, and renders `this` (frame->obj) and each typed argument (frame->args + RTM arg_offsets)
    // A raw hex dump of the args buffer is appended as ground truth
    // Post-AV context: every deref readability-guarded, unreadable values render as '?'; the VEH caller's __try swallows a secondary fault
    void DumpBakedCrashRecord(void* method, void* frameV, const char* reason);

  private:
    HookManager() = default;
    ~HookManager() = default;
    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;

    // Find the target method in game assemblies, signature is optional for overload resolution
    // preferImage (optional): a mod image to fall back to when the game's ScriptHost::FindClass can't resolve the class, lets a hook target a method in a mod assembly (self-targeting / mod-to-mod)
    void* FindTargetMethod(const std::string& targetClass, const std::string& targetMethod, const std::string& signature = "", void* preferImage = nullptr);

    // Parse signature string into vector of type names
    static std::vector<std::string> ParseSignature(const std::string& signature);

    // Check if a method matches the given signature
    bool MethodMatchesSignature(void* method, const std::vector<std::string>& paramTypes);

    // Convert type name to MonoTypeCode (returns Void if unknown)
    static MonoTypeCode TypeNameToCode(const std::string& typeName);

    // GENERIC-TARGET SAFETY - human-readable reason when `method` cannot be trampolined, "" when safe
    // Inflated methods (constructed generic types) ARE trampolinable: CloneMethod copies the full 0x30-byte MonoMethodInflated (context @+0x28, declaring @+0x2C), so the forced re-transform resolves like the original's
    // Refused: open generic method definitions, inflated methods with unreadable context/declaring, and the corrupt generic_class-without-is_inflated shape
    // Past registration the failure mode is abort(), so anything unprovable must be refused here
    static std::string DescribeTrampolineHazard(void* method);

    // Create backup of original method for trampoline
    void BackupOriginalMethod(HookChain& chain);

    // Allocate a chain's unique dispatch token (see HookChain::dispatchToken): the target's own MethodDef token when free, else a synthetic key probed outside the 0x06xxxxxx MethodDef space
    uint32_t AllocateDispatchToken(uint32_t preferredToken, void* targetMethod);

    // Install the dispatch stub on a method
    bool InstallDispatchStub(HookChain& chain);

    // BAKED CHAIN PATH - whether the chain qualifies for zero-overhead baked dispatch: some entry forwards via Chain.* (usesChain), every entry ExactMatches the target signature, no generated wrappers, and the interpreter/quarantine/instrument preconditions hold
    // Instance targets bake via the leading-self convention; boxing entries ride as ShimHook links (see BuildCompiledChain)
    // Anything that fails falls back to the boxing dispatch stub
    bool ChainBakeEligible(HookChain& chain);

    // CRASH QUARANTINE store - append-only crash log at <S3MM>/baked_quarantine.txt (one "Class::Method" line per attributed crash), lazily counted per key
    // Append-only keeps the crash-time write trivial; delete the file to clear
    static constexpr int kBakeQuarantineThreshold = 2;            // strikes across runs before a bake is refused
    static std::string BakeQuarantineKey(const HookChain& chain); // "Class::Method(sig)" - signature-qualified so overloads don't share strikes
    void EnsureBakeQuarantineLoaded();
    bool IsBakeQuarantined(const HookChain& chain);
    std::unordered_map<std::string, int> m_bakeQuarantine; // key -> accumulated crash count
    bool m_quarantineLoaded = false;

    // DEMOTE-OR-REJECT - when a chain can't bake, a boxing-safe typed entry is KEPT and runs boxed (usesChain is left as-is; the chain may still bake on hot-reload)
    // A defective entry - signature mismatch, incoherent Chain.CallNext forward, or Chain.ByRef - breaks both paths and is rejected and stripped
    // Records the first reason on chain.bakeRejectReason for Audit.GetBakeRejectReason; call only when ineligible
    void DemoteIneligibleTypedHooks(HookChain& chain);

    // First-time compile+install of the baked chain; returns false (leaving chain unpatched) so the caller can fall back to InstallDispatchStub
    bool TryInstallBakedChain(HookChain& chain);

    // Rebuild links for an already-installed baked chain and re-install IN PLACE (hot-reload / priority / change); returns false if recompile fails (chain left as-is on its previously installed head)
    bool RecompileBakedChain(HookChain& chain);

    // Populate a CompiledChain's links from chain.hooks (already priority-sorted, watchers merged) and ensure trampolineRTM (the original IL) is transformed; shared by first-install and recompile
    bool BuildCompiledChain(HookChain& chain, CompiledChain& c);

    // Frame-walk the live interpreter frames to the INNERMOST one whose runtime_method is a compiled link of this chain; returns that link's index, or -1
    // Used by NativeCallNext to route a ShimHook link's boxing Hook.CallNext into ContinueBakedChain
    int FindCallerBakedLinkIndex(void* originalMethod);

    // CALLER FRAME - the frame pointer is only valid for the current hook activation (pool slots are reused after the join), so ICALLs hand out copies, never the frame
    struct CallerFrame {
        InterpFrame* frame = nullptr;
        void* method = nullptr; // canonical MonoMethod*
    };
    bool ResolveCallerFrame(CallerFrame& out);
    bool ResolveCallerFrameUnguarded(InterpFrame* frame, CallerFrame& out);
    void* CanonicalFrameMethod(InterpFrame* frame);

    // Cache signature info for a chain (speedup: avoid re-reading every call)
    void CacheSignatureInfo(HookChain& chain);

    // SHARED RETURN TERMINAL - convert an interpreter invoke result into the MonoObject* boxing callers expect
    // All boxing return paths funnel through here; returning the raw pRet stackval pointer would hand the caller a pointer-shaped integer
    // `chain` may be null (unknown return type), in which case only isObject is consulted
    static void* BoxInvokeResult(HookChain* chain, MonoInvokeState& state, const char* site);

    // Check if a type code represents a primitive (for boxing optimization)
    static bool IsPrimitiveType(MonoTypeCode code) { return code >= MonoTypeCode::Boolean && code <= MonoTypeCode::R8; }

    bool m_initialized = false;

    // Per-mod install diagnostics for LogHookSummary / Audit.GetRefusedHooks
    std::unordered_map<std::string, ModHookStats> m_modStats;
    size_t m_summarizedRefusals = 0; // total refusal records at last LogHookSummary (gates delta prints)

    void RecordHookRequest(const std::string& modName);
    void RecordHookRefusal(const std::string& modName, const std::string& target, const std::string& reason); // dedups on (mod, target, reason)

    // Map from original method pointer to hook chain
    std::unordered_map<void*, HookChain> m_chains;

    // Baked dispatch structures, keyed by original method pointer; owns CompiledChain lifetimes, present only for baked chains (chain.bakedActive)
    std::unordered_map<void*, CompiledChain> m_compiledChains;

    // Map from HookChain::dispatchToken (NOT the raw MethodDef token - those collide across mod assemblies) to original method pointer
    std::unordered_map<uint32_t, void*> m_tokenToMethod;

    // Map from hook method pointer to target method token (for auto-token resolution in hook[0])
    std::unordered_map<void*, uint32_t> m_hookMethodToToken;

    // Trampoline clone MonoMethod* -> original (frames running the original body identify by the clone)
    std::unordered_map<void*, void*> m_trampolineToOriginal;

    // Map from (image, hookToken) to target method token; key is (imagePtr << 32) | token to avoid cross-assembly collisions
    std::unordered_map<uint64_t, uint32_t> m_hookTokenToTargetToken;

    // Helper to create composite key for hookTokenToTargetToken
    static uint64_t MakeHookTokenKey(void* image, uint32_t token) { return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(image)) << 32) | token; }
};

} // namespace S3MM
