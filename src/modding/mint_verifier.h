#pragma once
#include "../core/mono_types.h"
#include "chain_compiler.h" // SignatureInfo
#include <cstdint>
#include <string>
#include <vector>

namespace S3MM {

// MINT STACK-EFFECT VERIFIER - a compile-time model of the interpreter eval stack, proving a synthesized forward/adaptor link is stack-coherent BEFORE install
//   - StackInfo is 8 bytes; sp advances by 8 per push
//   - Coarse tag at StackInfo+0x04 comes from a per-mt lookup table: [0,0,0,0,0,1,2,2,3,5,4] for mt 0..10
//   - A managed pointer / by-ref is one 8-byte slot regardless of pointee width
//   - An instance call reads 'this' at arg_base-1, where arg_base = sp - param_count*8
//   - The baked forward op this-splits iff the CALLEE RTM's hasThis is set, so a static callee takes the receiver as an explicit leading slot

// COARSE STACK TYPE TAG - the byte stored at StackInfo+0x04, value domain 0..5
enum class CoarseTag : uint8_t {
    I4 = 0,        // sub-word + 32-bit integers all widen here (I1/U1/I2/U2/I4)
    I8 = 1,        // 64-bit integer
    R = 2,         // float (R4/R8)
    O = 3,         // object reference
    VT = 4,        // value type (payload tracked off-band via vt_sp)
    Ptr = 5,       // managed pointer / by-ref (one slot regardless of pointee)
    Invalid = 0xFF // not a stack value (e.g. void) or unclassifiable
};

const char* CoarseTagName(CoarseTag t);

CoarseTag MtToCoarseTag(uint8_t mt);

// MONOTYPECODE -> MT - read out of mint_type's element-type switch (a two-level jump table in the binary):
//   byref -> 9 (P)        Boolean -> 1 (U1)     Char -> 3 (U2)
//   I1 -> 0   U1 -> 1   I2 -> 2   U2 -> 3   I4/U4/I/U(native) -> 4   I8/U8 -> 5   R4 -> 6   R8 -> 7
//   String/Class/Object/Array/SzArray -> 8 (O)   Ptr -> 9 (P)   ValueType -> 10 (VT)
// Returns 0xFF (unclassifiable) for GenericInst (the runtime unwraps it to O or VT; not modeled) and Void
// CAVEAT (enum) - an enum ValueType really collapses to its underlying primitive on the eval stack, but SignatureInfo only shows ValueType so an enum reports mt 10 (VT)
// Self-consistent for forward verification (hook and target enums both classify VT); only a future RTM-walk verifier reading the real emitted tag would care
uint8_t MonoTypeCodeToMt(MonoTypeCode code, bool byref);

// Invalid (GenericInst/void) means the caller should box rather than bake
CoarseTag CoarseTagForType(MonoTypeCode code, bool byref);

struct MintSlot {
    CoarseTag tag = CoarseTag::Invalid;
    bool isByRef = false; // tag==Ptr that came from a ref/out param (1 slot, size-safe re: pointee width)
    uint32_t vtSize = 0;  // for tag==VT: payload size (rounded up to 8); 0 otherwise
};

// MINT STACK MODEL - simulate pushes/pops to verify a forward layout
// Models only the slot stack (depth + tags); MintSlot::vtSize stands in for the VT payload side-stack
// NOT YET WIRED - substrate (with VerifyForwardLayout) for the planned RTM-walk verifier; installs today are guarded by VerifyCallNextForward + ValidateBakedRtm
class MintStack {
  public:
    void Push(const MintSlot& s) { m_slots.push_back(s); }
    bool Pop(MintSlot* out = nullptr); // false on underflow (never reads past bottom)
    size_t Depth() const { return m_slots.size(); }
    const std::vector<MintSlot>& Slots() const { return m_slots; }
    void Clear() { m_slots.clear(); }

  private:
    std::vector<MintSlot> m_slots;
};

struct MintVerifyResult {
    bool ok = false;
    std::string reason;
};

// EXPECTED pre-call eval-stack layout for forwarding into `sig`, returned bottom-first:
//   instance: [this(O)] [arg0] [arg1] ... [argN-1]   (this lowest; read at arg_base-1)
//   static:           [arg0] [arg1] ... [argN-1]
// Each explicit arg is exactly one 8-byte slot, by-ref included
std::vector<MintSlot> ExpectedForwardLayout(const SignatureInfo& sig, bool instance);

// VERIFY a modeled stack against the expected forward layout (depth, per-slot tag, this-presence), returning the first divergence
// A by-ref (Ptr) slot matches any Ptr regardless of pointee; an Invalid expected tag isn't provable -> ok=false
MintVerifyResult VerifyForwardLayout(const MintStack& stack, const SignatureInfo& sig, bool instance);

// byref (0x10) -> Ptr, one managed-pointer slot, unclassifiable bytes (Var/MVar/Ptr/IntPtr/GenericInst) -> Invalid, which the caller treats as "skip, can't prove"
CoarseTag CoarseTagFromElementByte(uint8_t elementType);

// BAKE GUARDS - install-time proofs wired into the chain compiler so a malformed bake defers to boxing, the universal safe ABI, instead of crashing the interpreter
// Layer 1 reads the RTM fields the dispatch path consumes; Layer 2 reads what a CallNext site actually pushes

// LAYER 1 (RTM STRUCTURAL VALIDATION) - check a freshly-transformed link RTM against the trampoline, the golden reference for the SAME target signature
// Signature-derived fields (param_count, hasThis, return tag, args_size) must agree with the trampoline, and func must be sane
// instanceBridge=true also requires func == trampoline func (the bridge copies it) - guards the crash-#1, dispatch_virtual calling a garbage RTM->func
// ok=false means do NOT install - fall back to boxing dispatch
MintVerifyResult ValidateBakedRtm(void* linkRTM, void* trampolineRTM, const SignatureInfo& sig, bool instanceBridge);

// LAYER 2 (FORWARD-LAYOUT / CALLNEXT-ARITY) - verify one Chain.CallNext / CallNextVoid site forwards a stack-coherent arg list for the baked `target`
// genArgCount + genArgElementTypes come from DecodeMethodSpecGenericArgs; CallNext's first generic arg is the return type R, CallNextVoid has none (isVoidVariant=true)
// `instance` selects the leading-self layout ([self][args] vs [args])
//   - 'arity gate (pushed slots == target.paramCount, +1 for instance self): authoritative, catches the crash-#2 (forwarding without `self`, or the wrong count)
//   - Void/non-void variant must match target.ret (CallNextVoid <-> void, CallNext<R> <-> non-void)
//   - Per-arg coarse tags, return included, checked where decodable and not byref
//   - A ref/out param's CallNext arg is the pointee T (the ByRef smuggle), so its tag legitimately differs from the Ptr slot - skipped, the arity gate covers it
// Returns the first divergence; ok=false means the caller boxes (a hook with no CallNext site never reaches here)
MintVerifyResult VerifyCallNextForward(uint32_t genArgCount, const std::vector<uint8_t>& genArgElementTypes, bool isVoidVariant, const SignatureInfo& target, bool instance);

} // namespace S3MM
