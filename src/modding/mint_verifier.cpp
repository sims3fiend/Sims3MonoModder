#include "mint_verifier.h"
#include "../core/mono_bridge.h" // IsValidPointer / IsReadablePointer
#include <cstdio>
#include <string>
// me when I'm mentally ill:
namespace S3MM {

// Reject-diagnostic hex formatting, since std::to_string is decimal and "0x" + decimal reads as the wrong number
static std::string Hex(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%X", v);
    return buf;
}

const char* CoarseTagName(CoarseTag t) {
    switch (t) {
    case CoarseTag::I4:
        return "I4";
    case CoarseTag::I8:
        return "I8";
    case CoarseTag::R:
        return "R";
    case CoarseTag::O:
        return "O";
    case CoarseTag::VT:
        return "VT";
    case CoarseTag::Ptr:
        return "Ptr";
    default:
        return "Invalid";
    }
}

// Interpreter's own mt -> coarse-tag table for mt 0..10: [0,0,0,0,0,1,2,2,3,5,4]
CoarseTag MtToCoarseTag(uint8_t mt) {
    static const CoarseTag kTable[11] = {
        CoarseTag::I4,  // mt 0  I1
        CoarseTag::I4,  // mt 1  U1
        CoarseTag::I4,  // mt 2  I2
        CoarseTag::I4,  // mt 3  U2
        CoarseTag::I4,  // mt 4  I4
        CoarseTag::I8,  // mt 5  I8
        CoarseTag::R,   // mt 6  R4
        CoarseTag::R,   // mt 7  R8
        CoarseTag::O,   // mt 8  O
        CoarseTag::Ptr, // mt 9  P (managed pointer)
        CoarseTag::VT,  // mt 10 VT
    };
    return (mt < 11) ? kTable[mt] : CoarseTag::Invalid;
}

// ELEMENT_TYPE -> MT - a by-ref type short-circuits to mt 9 (managed pointer) before anything else is read, and every other code goes through the interpreter's type table
// Aliases collapse the way you'd expect: bool->U1(1), char->U2(3), U4->I4(4), U8->I8(5)
uint8_t MonoTypeCodeToMt(MonoTypeCode code, bool byref) {
    if (byref) return 9; // mt P (managed pointer), set the instant the by-ref bit is seen

    switch (code) {
    case MonoTypeCode::I1:
        return 0;
    case MonoTypeCode::Boolean:
    case MonoTypeCode::U1:
        return 1;
    case MonoTypeCode::I2:
        return 2;
    case MonoTypeCode::Char:
    case MonoTypeCode::U2:
        return 3;
    case MonoTypeCode::I4:
    case MonoTypeCode::U4:
        return 4;
    case MonoTypeCode::I8:
    case MonoTypeCode::U8:
        return 5;
    case MonoTypeCode::R4:
        return 6;
    case MonoTypeCode::R8:
        return 7;
    case MonoTypeCode::String:
    case MonoTypeCode::Class:
    case MonoTypeCode::Object:
    case MonoTypeCode::SzArray:
        return 8; // mt O
    case MonoTypeCode::ValueType:
        return 10;                  // mt VT (enum unwrap not modeled - see header CAVEAT)
    case MonoTypeCode::GenericInst: // runtime unwraps to O or VT; can't tell from the type code alone
    case MonoTypeCode::Void:
    default:
        return 0xFF; // unclassifiable -> caller boxes
    }
}

CoarseTag CoarseTagForType(MonoTypeCode code, bool byref) {
    uint8_t mt = MonoTypeCodeToMt(code, byref);
    return (mt == 0xFF) ? CoarseTag::Invalid : MtToCoarseTag(mt);
}

bool MintStack::Pop(MintSlot* out) {
    if (m_slots.empty()) return false;
    if (out) *out = m_slots.back();
    m_slots.pop_back();
    return true;
}

std::vector<MintSlot> ExpectedForwardLayout(const SignatureInfo& sig, bool instance) {
    std::vector<MintSlot> layout;
    layout.reserve(sig.paramCount + (instance ? 1u : 0u));

    // Instance: the receiver is the lowest slot, read at arg_base-1, and is always an object reference
    if (instance) {
        MintSlot self;
        self.tag = CoarseTag::O;
        layout.push_back(self);
    }

    for (uint16_t i = 0; i < sig.paramCount; ++i) {
        MintSlot s;
        bool byref = (i < sig.paramIsByRef.size()) && sig.paramIsByRef[i];
        MonoTypeCode code = (i < sig.paramTypes.size()) ? sig.paramTypes[i] : MonoTypeCode::Object;
        s.tag = CoarseTagForType(code, byref);
        s.isByRef = byref;
        layout.push_back(s);
    }
    return layout;
}

MintVerifyResult VerifyForwardLayout(const MintStack& stack, const SignatureInfo& sig, bool instance) {
    MintVerifyResult r;

    if (!sig.valid) {
        r.reason = "signature is not valid (could not be read)";
        return r;
    }

    std::vector<MintSlot> expected = ExpectedForwardLayout(sig, instance);

    if (stack.Depth() != expected.size()) {
        std::string shape = instance ? ("this + " + std::to_string(sig.paramCount) + " args") : (std::to_string(sig.paramCount) + " args");
        r.reason = "stack depth " + std::to_string(stack.Depth()) + " != expected " + std::to_string(expected.size()) + " (" + shape + ")";
        return r;
    }

    const std::vector<MintSlot>& actual = stack.Slots();
    for (size_t i = 0; i < expected.size(); ++i) {
        // An expected slot we couldn't classify means we can't PROVE coherence -> refuse and let the caller box
        if (expected[i].tag == CoarseTag::Invalid) {
            r.reason = "slot[" + std::to_string(i) + "] expected type is unclassifiable (GenericInst/void) - not provable, box instead";
            return r;
        }
        if (actual[i].tag != expected[i].tag) {
            std::string where = (instance && i == 0) ? "this" : ("arg[" + std::to_string(instance ? i - 1 : i) + "]");
            r.reason = "slot[" + std::to_string(i) + "] (" + where + ") tag " + CoarseTagName(actual[i].tag) + " != expected " + CoarseTagName(expected[i].tag);
            return r;
        }
    }

    r.ok = true;
    return r;
}

CoarseTag CoarseTagFromElementByte(uint8_t elementType) {
    if (elementType == 0x10) return CoarseTag::Ptr; // ELEMENT_TYPE_BYREF -> one managed-pointer slot
    // ECMA element-type bytes coincide with MonoTypeCode, and CoarseTagForType returns Invalid for the ones it can't classify (IntPtr/UIntPtr/Ptr/Var/MVar/GenericInst), which the caller treats as "skip"
    return CoarseTagForType(static_cast<MonoTypeCode>(elementType), false);
}

// LAYER 1 - RTM structural validation against the trampoline golden reference (see header)
MintVerifyResult ValidateBakedRtm(void* linkRTM, void* trampolineRTM, const SignatureInfo& sig, bool instanceBridge) {
    MintVerifyResult r;
    // Probe the WHOLE RTM struct before touching any field, since IsValidPointer is only a range check while IsReadablePointer actually maps-checks
    // Reading L->flags/param_count/args_size off an in-range-but-unmapped (or page-straddling) RTM would fault INSIDE the guard, turning the crash-prevention net into a crash source
    if (!IsReadablePointer(linkRTM, sizeof(RuntimeMethod))) {
        r.reason = "link RTM is not a readable 0x48-byte RuntimeMethod";
        return r;
    }
    if (!IsReadablePointer(trampolineRTM, sizeof(RuntimeMethod))) {
        r.reason = "trampoline RTM is not a readable 0x48-byte RuntimeMethod";
        return r;
    }
    auto* L = reinterpret_cast<RuntimeMethod*>(linkRTM);
    auto* T = reinterpret_cast<RuntimeMethod*>(trampolineRTM);

    // The link must have been lowered to MINT, or dispatch reads an un-transformed (garbage) RTM
    if ((L->flags & RuntimeMethodFlags::TRANSFORMED) == 0) {
        r.reason = "link RTM flags " + Hex(L->flags) + " missing TRANSFORMED (0x100) - not lowered";
        return r;
    }

    // Explicit param_count (RTM+0x40 excludes `this`) drives the call op's arg-base
    // It must equal both the target signature's param count AND the trampoline's, which is a golden reference under the same target signature
    if (L->param_count != (uint8_t)sig.paramCount) {
        r.reason = "link RTM param_count " + std::to_string(L->param_count) + " != signature paramCount " + std::to_string(sig.paramCount);
        return r;
    }
    if (L->param_count != T->param_count) {
        r.reason = "link RTM param_count " + std::to_string(L->param_count) + " != trampoline param_count " + std::to_string(T->param_count);
        return r;
    }

    // hasThis (RTM+0x44 & 0x200) drives the call op's this-split, so it must agree with both the signature and the trampoline
    bool linkHasThis = (L->flags & RuntimeMethodFlags::HAS_THIS) != 0;
    bool trampHasThis = (T->flags & RuntimeMethodFlags::HAS_THIS) != 0;
    if (linkHasThis != sig.hasThis) {
        r.reason = std::string("link RTM hasThis ") + (linkHasThis ? "set" : "clear") + " disagrees with signature hasThis " + (sig.hasThis ? "set" : "clear");
        return r;
    }
    if (linkHasThis != trampHasThis) {
        r.reason = "link RTM hasThis disagrees with trampoline hasThis";
        return r;
    }

    // Return-type tag (RTM+0x44 low byte) and argument-area size are signature-derived, so they must match the golden trampoline
    // A divergence means the link came out under a different effective signature
    if ((L->flags & RuntimeMethodFlags::RETURN_TYPE_MASK) != (T->flags & RuntimeMethodFlags::RETURN_TYPE_MASK)) {
        r.reason = "link RTM return-type tag " + Hex(L->flags & RuntimeMethodFlags::RETURN_TYPE_MASK) + " != trampoline " + Hex(T->flags & RuntimeMethodFlags::RETURN_TYPE_MASK);
        return r;
    }
    if (L->args_size != T->args_size) {
        r.reason = "link RTM args_size " + std::to_string(L->args_size) + " != trampoline args_size " + std::to_string(T->args_size);
        return r;
    }

    // NB: nativeThunk (+0x24) and func (+0x10) are both NULL for a pure MANAGED method, which dispatches via code (+0x00) and the interp loop - they are only populated for native/ICALL thunks
    // So readability is the WRONG invariant here and would false-reject every managed bake
    // The grounded check is congruence with the trampoline plus a func sanity check, since dispatch CALLS func when non-zero and a garbage non-null func is the crash
    if (L->func != nullptr && !IsReadablePointer(L->func, 1)) {
        r.reason = "link RTM func (+0x10) non-null but unreadable - dispatch_virtual would call garbage";
        return r;
    }
    // The instance bridge copies func AND nativeThunk from the trampoline over in chain_compiler
    // These equality checks re-verify chain_compiler's own writes, so they are a reorder/regression ratchet that only fires if CompileChain's copy step moves or is dropped, NOT an independent proof
    // Equality holds even when both are NULL, so this never false-rejects a managed instance method the way a readability probe would
    if (instanceBridge && L->func != T->func) {
        r.reason = "instance link RTM func (+0x10) != trampoline func - bridge copy did not take";
        return r;
    }
    if (instanceBridge && L->nativeThunk != T->nativeThunk) {
        r.reason = "instance link RTM nativeThunk (+0x24) != trampoline - bridge copy did not take";
        return r;
    }

    r.ok = true;
    return r;
}

// LAYER 2 - CallNext forward-layout / arity verification (see header)
MintVerifyResult VerifyCallNextForward(uint32_t genArgCount, const std::vector<uint8_t>& gen, bool isVoidVariant, const SignatureInfo& target, bool instance) {
    MintVerifyResult r;
    if (!target.valid) {
        r.reason = "target signature is not valid";
        return r;
    }

    const bool targetVoid = (target.ret == MonoTypeCode::Void);
    const uint32_t expectedSlots = (uint32_t)target.paramCount + (instance ? 1u : 0u);
    const std::string shape = instance ? ("self + " + std::to_string(target.paramCount) + " arg(s)") : (std::to_string(target.paramCount) + " arg(s)");

    if (isVoidVariant) {
        if (!targetVoid) {
            r.reason = "CallNextVoid used but the target returns non-void - use CallNext<R, ..>";
            return r;
        }
        // CallNextVoid<...>(...) : all generic args are pushed value slots, no return slot
        if (genArgCount != expectedSlots) {
            r.reason = "CallNextVoid forwards " + std::to_string(genArgCount) + " slot(s) != expected " + std::to_string(expectedSlots) + " (" + shape + ")";
            return r;
        }
    } else {
        if (targetVoid) {
            r.reason = "CallNext<R, ..> used but the target returns void - use CallNextVoid";
            return r;
        }
        if (genArgCount < 1) {
            r.reason = "CallNext<R, ..> has no generic args (need at least the return type R)";
            return r;
        }
        // CallNext<R, T..>(...) : first generic arg is R (not pushed), the rest are pushed value slots
        if (genArgCount - 1 != expectedSlots) {
            r.reason = "CallNext forwards " + std::to_string(genArgCount - 1) + " value slot(s) != expected " + std::to_string(expectedSlots) + " (" + shape + ")";
            return r;
        }
        // Return-type coarse tag (the R generic arg) must match the target return, where both are decodable
        if (!gen.empty()) {
            CoarseTag actualRet = CoarseTagFromElementByte(gen[0]);
            CoarseTag expectRet = CoarseTagForType(target.ret, false);
            if (actualRet != CoarseTag::Invalid && expectRet != CoarseTag::Invalid && actualRet != expectRet) {
                r.reason = std::string("CallNext return-type tag ") + CoarseTagName(actualRet) + " != target " + CoarseTagName(expectRet);
                return r;
            }
        }
    }

    // Per-value-slot coarse tag check, skipping by-ref param positions (the ByRef smuggle changes the slot tag) and any slot whose tag we couldn't classify, which depth has already gated
    std::vector<MintSlot> expected = ExpectedForwardLayout(target, instance);
    const uint32_t valueBase = isVoidVariant ? 0u : 1u; // gen[] index where the pushed value slots begin
    for (uint32_t i = 0; i < expectedSlots && i < expected.size(); ++i) {
        uint32_t gi = valueBase + i;
        if (gi >= gen.size()) break; // the generic-arg walk bailed earlier; arity already proven, stop here

        // Does this slot correspond to a by-ref target param? For an instance target slot 0 is `self`, which is never byref
        bool slotIsByRef = false;
        if (!(instance && i == 0)) {
            uint32_t paramIdx = instance ? (i - 1) : i;
            if (paramIdx < target.paramIsByRef.size()) slotIsByRef = target.paramIsByRef[paramIdx];
        }
        if (slotIsByRef) continue; // arg is the pointee T; slot is Ptr - legitimately different, depth covers it

        CoarseTag actual = CoarseTagFromElementByte(gen[gi]);
        CoarseTag expect = expected[i].tag;
        if (actual == CoarseTag::Invalid || expect == CoarseTag::Invalid) continue;
        if (actual != expect) {
            std::string where = (instance && i == 0) ? "self" : ("arg[" + std::to_string(instance ? i - 1 : i) + "]");
            r.reason = "CallNext " + where + " tag " + CoarseTagName(actual) + " != expected " + CoarseTagName(expect);
            return r;
        }
    }

    r.ok = true;
    return r;
}

} // namespace S3MM
