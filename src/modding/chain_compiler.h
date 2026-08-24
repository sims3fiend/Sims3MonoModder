#pragma once
#include "../core/mono_types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace S3MM {

// CHAIN COMPILER - lowers an ordered hook list into a baked RuntimeMethod chain
// The dispatch half is a linked list of RTMs, with no per-call frame-walk, ChainExecutionState, or object[] boxing://
//   game method RTM (identity-preserved, in-place overwritten)
//   -> link[0].RTM --(data_items CallNext slot)--> link[1].RTM --> ... --> trampoline RTM (original)//

// The interpreter walks RTM to RTM through MINT_CALL so native does zero per-call work and all cost moves to install time
// Which methods are hooked stays in HookManager::m_chains, the Audit source of truth; this module only owns the compiled dispatch structure

// SIGNATURE INFO - structural identity of a method signature, driving ExactMatch (the typed-vs-shim gate) and the MINT arg layout a baked call must satisfy
// Unlike HookChain's cached* fields it tracks by-ref and hasThis
struct SignatureInfo {
    bool valid = false;
    bool hasThis = false; // instance (true) vs static (false); maps to RTM+0x44 & 0x200
    MonoTypeCode ret = MonoTypeCode::Void;
    bool retIsValueType = false;
    void* retTypeData = nullptr; // MonoType+0x00 identity word for aggregate ret codes (null otherwise) - see paramTypeData
    uint16_t paramCount = 0;
    std::vector<MonoTypeCode> paramTypes;
    std::vector<bool> paramIsByRef;     // ref/out positions (SPEC_ read - see chain_compiler.cpp)
    std::vector<bool> paramIsValueType; // primitive or ValueType struct
    // AGGREGATE TYPE IDENTITY - for Class/ValueType/SzArray/GenericInst the MonoTypeCode alone doesn't pin the type, since two distinct classes both read `Class`
    // For those codes this holds the MonoType+0x00 data word (klass / element klass / MonoGenericClass*), and null for codes the MonoTypeCode pins by itself
    // ExactMatch compares it - the baked path has no cast, so a wrong-class hook would silently read field offsets off the wrong layout (boxing would at least throw InvalidCastException)
    std::vector<void*> paramTypeData;

    // TYPED-VS-SHIM GATE - a baked `call <next>` needs the link's eval stack to match the target exactly (return, params, by-ref, value-type, aggregate identity, hasThis) or MINT's arg copy corrupts, and for identity the hook reads the wrong layout
    // Mismatch means shim
    bool ExactMatch(const SignatureInfo& target) const;
};

// LINK KIND - how a chain node forwards to the next link
// Both kinds are transformed clones entered via the interpreter's call op with args aliased on the eval stack; the only difference is the forward mechanism the handler's own IL uses
enum class LinkKind {
    TypedHook,  // forwards via S3MM.Chain.* (baked): IL ByRef-nopped, data_items CallNext slot patched to the next link
    ShimHook,   // forwards via S3MM.Hook.CallNext (boxing ICALL): no slot to patch, and its CallNext lands in NativeCallNext, which continues the chain natively via HookManager::ContinueBakedChain
                // Lets a boxing hook (or read-only watcher) sit inside an otherwise-baked chain without demoting the whole chain to the dispatch stub
                // Requires an ExactMatch signature like a typed link, since it's entered with raw arg slots rather than boxed
    Trampoline, // implicit tail: the original method's preserved RTM
};

// CHAIN LINK - one node in the compiled dispatch list
struct ChainLink {
    LinkKind kind = LinkKind::TypedHook;
    std::string modName;
    void* userMethod = nullptr; // MonoMethod* the mod registered
    void* linkMethod = nullptr; // owned MonoMethodLayout clone the link RTM was transformed from; runtimeMethod == linkRTM
                                // ContinueBakedChain runtime-invokes this to enter the link, and via its baked slots the rest of the chain
    void* linkRTM = nullptr;    // RuntimeMethod* after transform; what the previous link's data_items targets (and how NativeCallNext identifies the calling link)

    std::vector<uint16_t> callNextSlots; // data_items indices in linkRTM patched to point at the next link (TypedHook only; empty for ShimHook)
    bool baked = false;
};

// COMPILED CHAIN - the baked dispatch structure for one target method
struct CompiledChain {
    void* targetMethod = nullptr;        // game MonoMethod* (identity preserved; head)
    void* headRTM = nullptr;             // the RTM at targetMethod->runtimeMethod: the target's own RTM when it existed at install, else the transplanted link-0 RTM; overwritten IN PLACE on recompile
    RuntimeMethod savedOriginalRTM = {}; // pre-overwrite bytes of the target's own RTM (restore image for teardown), valid when overwroteOriginal
    bool overwroteOriginal = false;
    void* trampolineRTM = nullptr; // original method's preserved RTM (tail target); caller supplies it
    SignatureInfo sig;             // target signature
    std::vector<ChainLink> links;  // execution order: watchers..., hooks by priority...; trampoline is implicit tail
    uint32_t version = 0;          // ++ per recompile (leak accounting / audit)
    std::string failReason;        // first bake-refusing failure from the last CompileChain, surfaced via HookChain::bakeRejectReason -> Audit.GetBakeRejectReason; "" on success
};

// CHAIN COMPILER - stateless w.r.t. chains; the caller (HookManager) owns CompiledChain lifetimes
class ChainCompiler {
  public:
    static ChainCompiler& Get();

    // Read a method's structural signature (params, by-ref, value-type, return, hasThis)
    static SignatureInfo ReadSignature(void* method);

    // MONOTYPE BY-REF BIT - bit 6 (mask 0x40) of the MonoType +0x07 packed byte
    // Shared with HookManager::CacheSignatureInfo so the boxing path's Hook.Ref gating and the bake path's SignatureInfo agree on what "by-ref param" means
    static bool TypeIsByRef(void* monoType);

    // True if the hook's IL contains a Chain.ByRef<T> call site
    // Such a hook can't fall back to the boxing chain, since the ByRef pointer smuggle only works nopped on the baked path, so it stays rejected rather than demoted
    static bool HookUsesByRef(void* hookMethod);

    // True if the hook forwards via the S3MM.Chain.* API (CallNext / CallNextVoid / ByRef) - the INFERRED bake signal
    // A chain bakes when every entry uses Chain and ExactMatches; a hook forwarding via the boxing S3MM.Hook.CallNext (or a pure replacement) reports false and keeps the chain on the boxing path
    // Owner-class verified (S3MM.Chain, not S3MM.Hook) so the shared CallNext basename can't false-positive
    static bool HookUsesChain(void* hookMethod);

    // LAYER-2 FORWARD COHERENCE (public wrapper) - true if every Chain.CallNext site in `hookMethod` forwards a stack-coherent arg list for `target`, with instance = target.hasThis
    // A false is a HOOK DEFECT (a short forward, or a self-less instance forward) that breaks baking and boxing alike, so the caller rejects rather than boxes
    static bool VerifyHookForward(void* hookMethod, const SignatureInfo& target, bool instance, std::string& reason);

    // Compile every link's RTM and wire data_items so link[i] -> link[i+1] and the tail -> trampolineRTM
    // Does NOT install; returns false on any transform/bake failure, and the caller falls back to boxing dispatch
    // Precondition: c.targetMethod valid and c.trampolineRTM set (via BackupOriginalMethod)
    bool CompileChain(CompiledChain& c);

    // Install (first time) or in-place re-install (recompile) the head RTM on the target
    // Recompile overwrites the installed head IN PLACE so game callers that cached headRTM keep dispatching to the new chain - the one coherence invariant carried over from MethodPatcher
    bool InstallChain(CompiledChain& c);

    // Attach the add_data_item instrument used to locate CallNext slots during link transforms
    // Call once after Addresses::Initialize() and before compiling any chain; idempotent
    bool Install();
    bool IsInstalled() const { return m_installed; }

    // Clone a method into an owned MonoMethod copy with a fresh (null) RTM slot, so TransformMethod builds off a clean MonoMethod+0x14 instead of colliding on the source method's single RTM cache
    // SIZE-AWARE: an inflated method (constructed generic type/method) is copied as the full 0x30-byte MonoMethodInflatedLayout so context (+0x28) / declaring (+0x2C) survive
    // Shared with HookManager::BackupOriginalMethod (the trampoline copy has identical requirements); returns nullptr if the span is unreadable
    static void* CloneMethod(void* method);

  private:
    ChainCompiler() = default;
    ~ChainCompiler() = default;
    ChainCompiler(const ChainCompiler&) = delete;
    ChainCompiler& operator=(const ChainCompiler&) = delete;

    // data_items[idx] = nextRTM - an aligned pointer store, atomic on x86, so a concurrent reader in the MINT_CALL handler sees old-or-new and both are valid RTMs
    static void PatchDataItem(void* linkRTM, uint16_t idx, void* nextRTM);

    bool m_installed = false; // add_data_item instrument attached
};

} // namespace S3MM
