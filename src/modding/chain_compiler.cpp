#include "chain_compiler.h"
#include "method_patcher.h"
#include "../core/addresses.h"
#include "../core/il_parser.h"
#include "../core/mono_bridge.h"
#include "mint_verifier.h"
#include "../util/logger.h"
#include <Windows.h>
#include <cstring>
#include <detours/detours.h>
#include <string>
#include <vector>

// given enough time, all software evolves into a ~~crab~~ compiler - Not Harry
namespace S3MM {

ChainCompiler& ChainCompiler::Get() {
    static ChainCompiler instance;
    return instance;
}

// Primitives (bool..r8) and ValueType structs need by-value stack layout
static bool IsValueTypeCode(MonoTypeCode code) {
    return (code >= MonoTypeCode::Boolean && code <= MonoTypeCode::R8) || code == MonoTypeCode::ValueType;
}

// AGGREGATE TYPE IDENTITY - codes the MonoTypeCode alone doesn't pin
// For these the MonoType+0x00 data word (Offsets::TYPE_DATA) is the identity: MonoClass* for Class/ValueType, element MonoClass* for SzArray, MonoGenericClass* for GenericInst
// Both are canonical per type (Mono hash-conses generic instantiations process-wide), so pointer equality is a safe identity test that never false-rejects, even across assemblies
static bool TypeCodeNeedsDataIdentity(MonoTypeCode code) {
    return code == MonoTypeCode::Class || code == MonoTypeCode::ValueType || code == MonoTypeCode::SzArray || code == MonoTypeCode::GenericInst;
}

static void* TypeDataOf(void* monoType, MonoTypeCode code) {
    if (!TypeCodeNeedsDataIdentity(code)) return nullptr; // code pins the type by itself
    return *(void**)((uintptr_t)monoType + Offsets::TYPE_DATA);
}

// Shared method -> (image, IL) resolution for the call-site scans below
// Returns nullptr on success or a static reason string on failure (threaded into VerifyForwardSites' reject diagnostics)
static const char* ReadHookIL(void* method, void*& image, const uint8_t*& il, uint32_t& ilSize) {
    image = nullptr;
    il = nullptr;
    ilSize = 0;
    if (!IsValidPointer(method)) return "hook method pointer invalid";
    void* klass = reinterpret_cast<MonoMethodLayout*>(method)->klass;
    image = MonoBridge::GetClassImage(klass);
    if (!IsValidPointer(image)) return "hook image unresolved";
    void* header = MonoBridge::Get().GetMethodHeader(method);
    if (!header) return "hook method header unresolved";
    il = MonoBridge::Get().GetMethodIL(header, &ilSize);
    if (!il || ilSize == 0) return "hook IL unreadable";
    return nullptr;
}

// MONOTYPE BY-REF BIT - byref = bit 6 (mask 0x40) of the +0x07 byte, and pinned is 0x80 of that same byte so don't conflate the two
// MonoType is 8 bytes total: +0x00 data | +0x04 attrs:16 | +0x06 type:8 | +0x07 { num_mods:6, byref:1, pinned:1 }
bool ChainCompiler::TypeIsByRef(void* monoType) {
    if (!IsReadablePointer(monoType, 8)) return false;
    uint8_t packed = *(uint8_t*)((uintptr_t)monoType + 0x07);
    return (packed & 0x40u) != 0;
}

// CALLNEXT SLOT CAPTURE - instrument on SPEC_interp_add_data_item (0xe5bcd0)
// The slot index isn't recoverable post-transform (array length isn't stored on the RTM), so we capture (value -> index) live during the link's transform and keep the indices whose value is a CallNext stub RTM
// Patching those slots to <next> is what makes the chain forward
// add_data_item has a custom register ABI (ESI = transform-data, EBX = value, EAX = index), so the detour is a naked shim forwarding (value, index)

static void* s_origAddDataItem = nullptr; // Detours trampoline to the real add_data_item

struct SlotCapture {
    std::vector<uint16_t> slots;
    bool active = false;
};
// Per-thread, scoped to a single TransformMethod call (set in CompileChain)
static thread_local SlotCapture* t_capture = nullptr;

// Recognize any instantiation of S3MM.Chain.CallNext / CallNextVoid by a freshly-built callee RTM
// The "CallNext" name prefix excludes ByRef, which is nopped at the IL level before transform anyway
static bool IsCallNextStubRTM(void* rtm) {
    if (!IsValidPointer(rtm)) return false;
    void* method = reinterpret_cast<RuntimeMethod*>(rtm)->method; // RTM+0x08
    if (!IsValidPointer(method)) return false;

    const char* name = reinterpret_cast<MonoMethodLayout*>(method)->name; // +0x18
    if (!IsReadablePointer((void*)name, 8) || strncmp(name, "CallNext", 8) != 0) return false;

    void* klass = reinterpret_cast<MonoMethodLayout*>(method)->klass; // +0x08
    if (!IsValidPointer(klass)) return false;
    const char* cn = MonoBridge::GetClassName(klass);
    const char* ns = MonoBridge::GetClassNamespace(klass);
    return cn && ns && strcmp(cn, "Chain") == 0 && strcmp(ns, "S3MM") == 0;
}

// Called by the naked shim after the original runs, where value = the added RTM and index = its data_items slot
// TODO(nesting): a .cctor transformed mid-transform could re-enter on this thread, but it only adds CallNext slots if it too is a hook calling CallNext (rare); scope by transform-data ptr (ESI) if seen
static void __cdecl CaptureDataItem(void* value, uint32_t index) {
    SlotCapture* cap = t_capture;
    if (!cap || !cap->active) return;
    if (IsCallNextStubRTM(value)) cap->slots.push_back(static_cast<uint16_t>(index));
}

__declspec(naked) static void NakedAddDataItemHook() {
    __asm {
        mov   ecx, s_origAddDataItem // call original through the Detours trampoline
        call  ecx // ESI/EBX preserved (nonvolatile); EAX = returned index
        pushfd
        pushad
        push  eax // index (arg 2)
        push  ebx // value (arg 1, = EBX per add_data_item ABI)
        call  CaptureDataItem // __cdecl(value, index)
        add   esp, 8
        popad
        popfd
        ret
    }
}

bool SignatureInfo::ExactMatch(const SignatureInfo& target) const {
    if (!valid || !target.valid) return false;
    if (ret != target.ret || retIsValueType != target.retIsValueType) return false;
    // Aggregate return identity: `SimDescription Hook(..)` must not match `Sim Target(..)` (both code Class)
    if (retTypeData != target.retTypeData) return false;

    // INSTANCE / LEADING-SELF CONVENTION - a typed instance hook is a STATIC method whose first param is the receiver: `static R Hook(object self, T0..)` <-> `instance R Target(T0..)`
    // Match that shape: target.hasThis && !hook.hasThis, hook.paramCount == target.paramCount + 1, a plain (non-byref, non-VT) leading ref slot, and the rest matching 1:1
    // The self slot itself is NOT identity-compared: any reference type verifies, since the receiver arrives via the frame either way
    // CompileChain re-signs each link clone under the target's instance signature so `ldarg.0` lowers to LDARG_this; value-type receivers are refused there (a struct's `this` is a managed pointer `object self` can't carry)
    if (target.hasThis && !hasThis) {
        if (paramCount != (uint16_t)(target.paramCount + 1)) return false;
        if (paramCount == 0) return false;                        // defensive: need the leading self slot
        if (paramIsByRef[0] || paramIsValueType[0]) return false; // receiver = plain ref slot
        for (uint16_t i = 0; i < target.paramCount; ++i) {
            if (paramTypes[i + 1] != target.paramTypes[i]) return false;
            if (paramIsByRef[i + 1] != target.paramIsByRef[i]) return false;
            if (paramIsValueType[i + 1] != target.paramIsValueType[i]) return false;
            if (paramTypeData[i + 1] != target.paramTypeData[i]) return false; // aggregate identity (see header)
        }
        return true;
    }

    // SAME-SHAPE (static hook <-> static target): strict structural identity
    if (hasThis != target.hasThis) return false;
    if (paramCount != target.paramCount) return false;
    for (uint16_t i = 0; i < paramCount; ++i) {
        if (paramTypes[i] != target.paramTypes[i]) return false;
        if (paramIsByRef[i] != target.paramIsByRef[i]) return false;
        if (paramIsValueType[i] != target.paramIsValueType[i]) return false;
        if (paramTypeData[i] != target.paramTypeData[i]) return false; // aggregate identity (see header)
    }
    return true;
}

SignatureInfo ChainCompiler::ReadSignature(void* method) {
    SignatureInfo info;
    if (!IsValidPointer(method)) return info;

    auto* layout = reinterpret_cast<MonoMethodLayout*>(method);
    info.hasThis = (layout->flagsPacked & MethodFlags::STATIC) == 0;

    // MonoMethod.signature (+0x0C) is lazily decoded - NULL on a freshly-compiled method that has never been invoked (e.g. a just-registered hook)
    // Force the decode via mono_method_signature rather than reading the raw field, or an unhooked handler reads back as paramCount=0 / invalid
    void* sig = MonoBridge::Get().GetMethodSignature(method);
    if (!IsReadablePointer(sig, Offsets::SIG_PARAMS)) return info;

    // RETURN TYPE - an unreadable MonoType is NOT "probably object", so a prove-or-defer signature stays invalid (ExactMatch fails on either side and the chain boxes) rather than matching on guesswork
    void* retType = *(void**)((uintptr_t)sig + Offsets::SIG_RET_TYPE);
    if (!IsReadablePointer(retType, 8)) return info;
    info.ret = (MonoTypeCode)(*(uint8_t*)((uintptr_t)retType + Offsets::TYPE_CODE));
    info.retIsValueType = (info.ret != MonoTypeCode::Void) && IsValueTypeCode(info.ret);
    info.retTypeData = TypeDataOf(retType, info.ret);

    // Parameters
    info.paramCount = *(uint16_t*)((uintptr_t)sig + Offsets::SIG_PARAM_COUNT);
    info.paramTypes.resize(info.paramCount);
    info.paramIsByRef.resize(info.paramCount);
    info.paramIsValueType.resize(info.paramCount);
    info.paramTypeData.resize(info.paramCount);

    for (uint16_t i = 0; i < info.paramCount; ++i) {
        void* paramType = *(void**)((uintptr_t)sig + Offsets::SIG_PARAMS + i * sizeof(void*));
        if (!IsReadablePointer(paramType, 8)) return info; // unreadable param type -> not provable, stay invalid
        MonoTypeCode code = (MonoTypeCode)(*(uint8_t*)((uintptr_t)paramType + Offsets::TYPE_CODE));
        info.paramTypes[i] = code;
        info.paramIsByRef[i] = TypeIsByRef(paramType);
        info.paramIsValueType[i] = IsValueTypeCode(code);
        info.paramTypeData[i] = TypeDataOf(paramType, code);
    }

    info.valid = true;
    return info;
}

// Clone a user hook so the link gets its own MonoMethod+0x14 RTM slot
// Owned by the ChainLink and never freed: an in-flight interpreter frame can still be executing an old link when a recompile replaces it, so clones (like their RTMs) are deliberately leaked
// CompiledChain `version` counts recompiles for leak accounting
void* ChainCompiler::CloneMethod(void* method) {
    if (!IsValidPointer(method)) return nullptr;
    auto* src = reinterpret_cast<MonoMethodLayout*>(method);

    // SIZE-AWARE: an inflated method (constructed generic type/method) is 0x30 bytes - context @+0x28 and declaring @+0x2C
    // are consumed by the forced re-transform's token resolution and the runtime lazy callee resolver
    // A 0x28-byte copy keeps the is_inflated bit but loses both pointers -> abort on the re-transform (see Offsets::METHOD_INFLATED_CONTEXT)
    if (src->slotAndFlags & Offsets::METHOD_IS_INFLATED_FLAG) {
        if (!IsReadablePointer(method, sizeof(MonoMethodInflatedLayout))) return nullptr;
        auto* copy = new MonoMethodInflatedLayout(*reinterpret_cast<MonoMethodInflatedLayout*>(method));
        copy->method.runtimeMethod = nullptr; // force fresh transform off a clean +0x14 slot
        return &copy->method;
    }

    auto* copy = new MonoMethodLayout(*src);
    copy->runtimeMethod = nullptr; // force fresh transform off a clean +0x14 slot
    return copy;
}

// BY-REF SMUGGLE NOP - rewrite `call S3MM.Chain::ByRef<T>` sites to a 5-byte nop run, TRANSIENTLY
// For a `ref/out T` arg the compiler emits `<ldarg/ldloca>; call ByRef<T>`; the load pushes T&, and ByRef (body `return default(T)`) would pop it and push garbage
// Nopping the call leaves T& on the eval stack to flow straight into the rewritten `call CallNext<...>` (retargeted to <next>), which expects T& there
// Net stack effect is unchanged (pop-1/push-1 == 5x nop == 0) and T&/T are both pointer-width on the MINT stack, so arg_offsets/args_size are unaffected - same-size patch
//
// TRANSIENT - the nop only needs to exist while TransformMethod reads the IL, since execution runs the generated RTM and never the IL again
// The original 5 bytes are saved per site and restored IMMEDIATELY after transform, on success or failure
// Restore-always keeps the IL truthful for every later reader (HookUsesByRef, recompile, a second link cloned from the same user method)
// A post-nop failure makes the demote path re-scan this IL - it must see the real ByRef sites or it DEMOTES the hook to boxing with an orphaned pointer push

// Locate every `call S3MM.Chain::ByRef<T>` site (IL byte offsets)
// OWNER-QUALIFIED like the other two scans (ResolveCallTargetFull, not the bare member name) so a user generic method that happens to be named ByRef is never nopped - a name-only match would punch a 5-byte hole in someone else's IL
// EXACT member-name match: ByRef<T> is generic, so its call token is a MethodSpec and ResolveCallTargetFull yields the bare MemberRef name ("S3MM.Chain::ByRef") - a generic instantiation NEVER appears in a MemberRef name (it lives in the MethodSpec blob), so matching "ByRef<" can never fire. Exact ==, not prefix, so a future "ByRefHelper" can't false-positive
// Shared by the nop pass and the box-fallback probe (HookUsesByRef) - a hook with a ByRef site can't fall back to boxing
static std::vector<uint32_t> FindByRefCallSites(void* method) {
    std::vector<uint32_t> sites;
    void* image;
    const uint8_t* il;
    uint32_t ilSize;
    if (ReadHookIL(method, image, il, ilSize)) return sites;

    ForEachCallSite(il, ilSize, [&](uint32_t token, uint32_t insnOff) {
        std::string owner = ResolveCallTargetFull(image, token); // "Namespace.Type::Member"
        if (owner == "S3MM.Chain::ByRef") sites.push_back(insnOff);
        return true;
    });
    return sites;
}

struct ByRefNopPatch {
    uint32_t off;     // IL byte offset of the `call ByRef<T>` site
    uint8_t saved[5]; // original bytes (call + token), written back by SPEC_RestoreByRefCallSites
};

static std::vector<ByRefNopPatch> SPEC_NopByRefCallSites(void* method) {
    std::vector<ByRefNopPatch> patches;
    std::vector<uint32_t> sites = FindByRefCallSites(method);
    if (sites.empty()) return patches;

    void* header = MonoBridge::Get().GetMethodHeader(method);
    if (!header) return patches;
    uint32_t ilSize = 0;
    const uint8_t* il = MonoBridge::Get().GetMethodIL(header, &ilSize);
    if (!il || ilSize == 0) return patches;

    // IL lives in the image data buffer (mono_method_get_header->code maps straight into it)
    // Make the span writable for the same-size patch, then restore protection
    DWORD oldProtect = 0;
    auto* writable = const_cast<uint8_t*>(il);
    if (!VirtualProtect(writable, ilSize, PAGE_READWRITE, &oldProtect)) {
        LOG_ERROR("[ChainCompiler] ByRef nop: VirtualProtect failed");
        return patches;
    }
    for (uint32_t off : sites) {
        ByRefNopPatch p;
        p.off = off;
        memcpy(p.saved, writable + off, 5);
        memset(writable + off, 0x00, 5); // call <tok> (5 bytes) -> 5x nop
        patches.push_back(p);
    }
    VirtualProtect(writable, ilSize, oldProtect, &oldProtect);
    return patches;
}

// Write the saved bytes back, called unconditionally right after TransformMethod (see CompileChain) so the shared user-method IL is only ever mutated for the duration of the transform
static void SPEC_RestoreByRefCallSites(void* method, const std::vector<ByRefNopPatch>& patches) {
    if (patches.empty()) return;

    void* header = MonoBridge::Get().GetMethodHeader(method);
    if (!header) return;
    uint32_t ilSize = 0;
    const uint8_t* il = MonoBridge::Get().GetMethodIL(header, &ilSize);
    if (!il || ilSize == 0) return;

    DWORD oldProtect = 0;
    auto* writable = const_cast<uint8_t*>(il);
    if (!VirtualProtect(writable, ilSize, PAGE_READWRITE, &oldProtect)) {
        LOG_ERROR("[ChainCompiler] ByRef restore: VirtualProtect failed - IL left nopped, HookUsesByRef may under-report");
        return;
    }
    for (const ByRefNopPatch& p : patches) { memcpy(writable + p.off, p.saved, 5); }
    VirtualProtect(writable, ilSize, oldProtect, &oldProtect);
}

// True if the hook's IL has a Chain.ByRef<T> site; such a hook can't fall back to boxing (see header)
bool ChainCompiler::HookUsesByRef(void* hookMethod) {
    return !FindByRefCallSites(hookMethod).empty();
}

// INFERRED BAKE SIGNAL - does the hook forward via S3MM.Chain.* (vs the boxing S3MM.Hook.CallNext, or no forward at all)?
// Owner-class verified via ResolveCallTargetFull so S3MM.Hook.CallNext (same basename, generic too) can't false-positive into a silent replace-only bake
bool ChainCompiler::HookUsesChain(void* hookMethod) {
    void* image;
    const uint8_t* il;
    uint32_t ilSize;
    if (ReadHookIL(hookMethod, image, il, ilSize)) return false;

    bool found = false;
    ForEachCallSite(il, ilSize, [&](uint32_t token, uint32_t) {
        std::string owner = ResolveCallTargetFull(image, token); // "Namespace.Type::Member"
        if (owner.rfind("S3MM.Chain::", 0) == 0) {
            found = true;
            return false; // stop the walk
        }
        return true;
    });
    return found;
}

// BAKE GUARD (Layer 2) - prove every Chain.CallNext / CallNextVoid site in `hookMethod` forwards a stack-coherent argument list for the baked `target` (mint_verifier::VerifyCallNextForward)
// A hook with NO CallNext site is a pure replacement -> trivially coherent (returns true)
// On the first unprovable site this sets reasonOut and returns false so CompileChain fails and the caller falls back to boxing (the safe ABI)
// Reads the SAME IL the transform will (clone shares the user method's token/image); ByRef nops don't touch CallNext sites, so order vs SPEC_NopByRefCallSites is irrelevant
static bool VerifyForwardSites(void* hookMethod, const SignatureInfo& target, bool instance, std::string& reasonOut) {
    void* image;
    const uint8_t* il;
    uint32_t ilSize;
    if (const char* err = ReadHookIL(hookMethod, image, il, ilSize)) {
        reasonOut = err;
        return false;
    }

    bool ok = true;
    uint32_t forwardSites = 0;
    ForEachCallSite(il, ilSize, [&](uint32_t token, uint32_t) {
        // OWNER GATE - a typed forward site is a call to S3MM.Chain::CallNext* ONLY
        // This MUST be gated on the owner class, not just the basename: S3MM.Hook.CallNext<T> (the boxing forward used by shim links) is ALSO a generic method named "CallNext", so a basename-only test would decode its <T>-only instantiation as a 0-param short forward and false-reject the whole chain
        // Use the same owner-qualified resolver as HookUsesChain ("Namespace.Type::Member")
        std::string owner = ResolveCallTargetFull(image, token);
        if (owner.rfind("S3MM.Chain::", 0) != 0) return true; // not a Chain forward - skip

        std::string base = owner.substr(sizeof("S3MM.Chain::") - 1); // member name after the owner
        size_t lt = base.find('<');
        if (lt != std::string::npos) base = base.substr(0, lt);

        // Chain.CallNext<...>/CallNextVoid<...> are generic (MethodSpec, table 0x2B); the bare CallNextVoid() overload is a non-generic MemberRef
        bool isMethodSpec = ((token >> 24) & 0xFF) == 0x2B;
        bool isGenericForward = isMethodSpec && base.compare(0, 8, "CallNext") == 0;
        bool isVoidForward = !isMethodSpec && base == "CallNextVoid";
        if (isGenericForward || isVoidForward) {
            ++forwardSites; // counted for the by-ref pairing check below the walk
            bool isVoid = base.compare(0, 12, "CallNextVoid") == 0;
            uint32_t count = 0;
            std::vector<uint8_t> types;
            if (isMethodSpec) {
                MethodSpecGenericArgs ga = DecodeMethodSpecGenericArgs(image, token);
                if (!ga.valid) {
                    // A generic CallNext<...> whose instantiation blob won't decode is NOT the same as a zero-arg CallNextVoid() - refuse explicitly instead of silently treating it as count 0
                    reasonOut = "could not decode CallNext<...> generic instantiation (MethodSpec blob unreadable)";
                    ok = false;
                    return false; // stop the walk
                }
                count = ga.count;
                types = std::move(ga.leadingElementTypes);
            }
            // non-generic CallNextVoid(): count stays 0 - its exact pushed-slot count
            MintVerifyResult vr = VerifyCallNextForward(count, types, isVoid, target, instance);
            if (!vr.ok) {
                reasonOut = vr.reason;
                ok = false;
                return false; // stop the walk
            }
        }
        return true;
    });

    // BY-REF FORWARD PAIRING - a ref/out param forwarded WITHOUT Chain.ByRef is broken on BOTH paths, and the tag
    // check above can't see it: the generic args are the pointee T either way, so the byref positions are skipped.
    // But IL-wise only a `call ByRef<T>` leaves the managed pointer the slot expects (C# can't express it otherwise),
    // so a plain-value forward feeds an int into a managed-pointer slot: wild-pointer deref on the baked path,
    // silently discarded ref/out writes on the boxing path. Every forward must smuggle every byref param
    if (ok && forwardSites > 0) {
        uint32_t byrefParams = 0;
        for (size_t i = 0; i < target.paramIsByRef.size(); ++i) {
            if (target.paramIsByRef[i]) ++byrefParams;
        }
        size_t byrefSites = FindByRefCallSites(hookMethod).size();
        if (byrefParams > 0 && byrefSites < (size_t)byrefParams * forwardSites) {
            reasonOut = "target takes " + std::to_string(byrefParams) + " ref/out param(s) but the hook has " + std::to_string(byrefSites) + " Chain.ByRef site(s) across " + std::to_string(forwardSites) +
                        " forward(s) - wrap each ref/out arg with Chain.ByRef(ref x); a by-value forward crashes the baked path and silently discards writes when boxed";
            return false;
        }
    }
    return ok;
}

// Public wrapper over VerifyForwardSites (Layer 2), instance = target.hasThis - see header
bool ChainCompiler::VerifyHookForward(void* hookMethod, const SignatureInfo& target, bool instance, std::string& reason) {
    return VerifyForwardSites(hookMethod, target, instance, reason);
}

bool ChainCompiler::Install() {
    if (m_installed) return true;

    uintptr_t addr = Addresses::Methods.SPEC_interp_add_data_item;
    if (!addr) {
        LOG_ERROR("[ChainCompiler] Install: SPEC_interp_add_data_item address not resolved");
        return false;
    }
    s_origAddDataItem = reinterpret_cast<void*>(addr);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG err = DetourAttach(&s_origAddDataItem, reinterpret_cast<void*>(NakedAddDataItemHook));
    if (err != NO_ERROR) {
        DetourTransactionAbort();
        LOG_ERROR("[ChainCompiler] Install: DetourAttach failed (error " + std::to_string(err) + ")");
        return false;
    }
    LONG commit = DetourTransactionCommit();
    if (commit != NO_ERROR) {
        LOG_ERROR("[ChainCompiler] Install: DetourTransactionCommit failed (error " + std::to_string(commit) + ")");
        return false;
    }

    m_installed = true;
    LOG_INFO("[ChainCompiler] add_data_item instrument installed");
    return true;
}

void ChainCompiler::PatchDataItem(void* linkRTM, uint16_t idx, void* nextRTM) {
    auto* rtm = reinterpret_cast<RuntimeMethod*>(linkRTM);
    rtm->data_items[idx] = nextRTM; // aligned pointer store, atomic on x86
}

bool ChainCompiler::CompileChain(CompiledChain& c) {
    c.failReason.clear(); // every refusing return below sets it (surfaced via bakeRejectReason -> Audit)
    if (!IsValidPointer(c.targetMethod)) {
        c.failReason = "invalid target method pointer";
        LOG_ERROR("[ChainCompiler] CompileChain: invalid target method");
        return false;
    }
    c.sig = ReadSignature(c.targetMethod);
    if (!c.sig.valid) {
        c.failReason = "could not read the target method signature";
        LOG_ERROR("[ChainCompiler] CompileChain: could not read target signature");
        return false;
    }
    if (!IsValidPointer(c.trampolineRTM)) {
        // Tail target is the original's preserved RTM; caller must wire it before compiling
        c.failReason = "trampoline RTM not set (original method backup missing)";
        LOG_ERROR("[ChainCompiler] CompileChain: trampolineRTM not set");
        return false;
    }

    // INSTANCE BRIDGE (EXPERIMENTAL) - for an instance target, transform each hook-link clone under the TARGET's instance signature so `ldarg.0` lowers to LDARG_this and `ldarg.1..` map to the target params (see ExactMatch)
    // The original trampoline is already instance and needs no swap
    void* targetSig = nullptr;
    if (c.sig.hasThis) {
        // Leading-self assumes a REFERENCE-type receiver, since self is `object`
        // A value-type receiver's `this` is a managed pointer, which the `object self` shape can't carry - refuse
        void* targetKlass = reinterpret_cast<MonoMethodLayout*>(c.targetMethod)->klass;
        if (IsReadablePointer(targetKlass, 0x15) && (*(uint8_t*)((uintptr_t)targetKlass + 0x14) & 0x8) != 0) {
            c.failReason = "the target is an instance method on a struct (value-type receiver); the leading-self bridge only supports reference-type receivers";
            LOG_WARN("[ChainCompiler] CompileChain: value-type-receiver instance target not supported by the leading-self bridge; falling back to boxing");
            return false;
        }
        targetSig = MonoBridge::Get().GetMethodSignature(c.targetMethod);
        if (!IsReadablePointer(targetSig, 8)) {
            c.failReason = "instance target signature unreadable; cannot bridge the receiver";
            LOG_WARN("[ChainCompiler] CompileChain: instance target signature unreadable; cannot bridge");
            return false;
        }
    }

    // 1. Lower each link to an owned, transformed RTM clone
    // TypedHook and ShimHook links are transformed IDENTICALLY - the boxing handler is target-shaped too (leading-self for instance), so it enters via the baked call op with aliased args exactly like a typed clone
    // A shim handler forwards via the boxing Hook.CallNext ICALL, so it simply has no Chain.CallNext sites: VerifyForwardSites passes trivially, the ByRef nop finds nothing, and no CallNext slots are captured
    // The kind only matters at wiring (2) and at run time, where a ShimHook's forward lands in NativeCallNext -> ContinueBakedChain rather than a baked slot
    for (size_t i = 0; i < c.links.size(); ++i) {
        ChainLink& L = c.links[i];

        L.linkMethod = CloneMethod(L.userMethod);
        if (!L.linkMethod) {
            c.failReason = "could not clone the hook method for link " + std::to_string(i) + " (mod '" + L.modName + "')";
            LOG_ERROR("[ChainCompiler] CompileChain: clone failed for link " + std::to_string(i));
            return false;
        }

        // INSTANCE BRIDGE - re-sign the clone as the target instance method (see note above)
        // The clone keeps the hook's token/klass/IL (so GetMethodHeader reads the hook body) but borrows the target's signature for arg lowering: ldarg.0 -> receiver, ldarg.1 -> target param[0], etc
        // Clearing STATIC keeps any flagsPacked-based hasThis reads consistent with the borrowed signature
        if (targetSig) {
            auto* cloneLayout = reinterpret_cast<MonoMethodLayout*>(L.linkMethod);
            cloneLayout->signature = targetSig;
            cloneLayout->flagsPacked &= ~MethodFlags::STATIC;
        }

        // BAKE GUARD (Layer 2) - prove this link's CallNext sites forward a stack-coherent arg list BEFORE we transform/install it
        // Catches forwarding without `self`, a wrong arg count, or a void/non-void variant mismatch
        // A failure here is a clean fall-back to boxing, not a crash
        std::string fwdReason;
        if (!VerifyForwardSites(L.linkMethod, c.sig, c.sig.hasThis, fwdReason)) {
            c.failReason = "link " + std::to_string(i) + " (mod '" + L.modName + "') forward-layout not provable: " + fwdReason;
            LOG_WARN("[ChainCompiler] CompileChain: link " + std::to_string(i) + " forward-layout not provable (" + fwdReason + "); falling back to boxing");
            return false;
        }

        // Nop ByRef<T> call-sites BEFORE transform so the managed pointer survives into the baked call
        // The clone shares the user method's token/image, so this patches the same IL the transform reads
        std::vector<ByRefNopPatch> byrefNops = SPEC_NopByRefCallSites(L.linkMethod);
        // FAIL CLOSED - a hook with ByRef sites that could NOT be nopped (VirtualProtect or header/IL re-read failure inside
        // SPEC_NopByRefCallSites) would otherwise transform the live ByRef<T> body, which throws at runtime instead of
        // refusing here like every other guard. IL is untouched on this path, so the demote re-scan still sees the real sites
        if (byrefNops.empty() && HookUsesByRef(L.linkMethod)) {
            c.failReason = "link " + std::to_string(i) + " (mod '" + L.modName + "') has Chain.ByRef<T> call-sites that could not be nopped for transform";
            LOG_ERROR("[ChainCompiler] CompileChain: ByRef nop failed for link " + std::to_string(i) + "; hook stays rejected (a ByRef hook cannot box)");
            return false;
        }
        if (!byrefNops.empty()) { LOG_DEBUG("[ChainCompiler] link " + std::to_string(i) + " ByRef call-sites nopped (transient): " + std::to_string(byrefNops.size())); }

        // Capture the data_items slots resolving to CallNext during THIS link's transform
        SlotCapture cap;
        cap.active = true;
        t_capture = &cap;
        bool transformed = MonoBridge::Get().TransformMethod(L.linkMethod);
        t_capture = nullptr;

        // Restore the ByRef sites NOW, on every path - the transform has consumed the IL, and a later failure makes the demote path re-scan it (see the nop block above)
        SPEC_RestoreByRefCallSites(L.linkMethod, byrefNops);

        if (!transformed) {
            c.failReason = "MINT transform failed for link " + std::to_string(i) + " (mod '" + L.modName + "')";
            LOG_ERROR("[ChainCompiler] CompileChain: transform failed for link " + std::to_string(i));
            return false;
        }

        L.linkRTM = reinterpret_cast<MonoMethodLayout*>(L.linkMethod)->runtimeMethod;
        if (!IsValidPointer(L.linkRTM)) {
            c.failReason = "transform produced no RTM for link " + std::to_string(i) + " (mod '" + L.modName + "')";
            LOG_ERROR("[ChainCompiler] CompileChain: null linkRTM after transform for link " + std::to_string(i));
            return false;
        }

        // Frame-walk / audit back-pointer: RTM->method (RTM+0x08) -> the original target, not the clone
        reinterpret_cast<RuntimeMethod*>(L.linkRTM)->method = c.targetMethod;

        // INSTANCE FIX - the callvirt dispatch path calls RTM->func (+0x10) when non-zero; the re-signed clone's func and nativeThunk (+0x24) come out garbage
        // Copy both from the trampoline, a correctly-transformed instance method of the same shape - func is frame-generic, so the head still runs its OWN code
        // Only needed for instance links
        if (targetSig && IsValidPointer(c.trampolineRTM)) {
            auto* lr = reinterpret_cast<RuntimeMethod*>(L.linkRTM);
            auto* tr = reinterpret_cast<RuntimeMethod*>(c.trampolineRTM);
            lr->func = tr->func;
            lr->nativeThunk = tr->nativeThunk;
        }

        // BAKE GUARD (Layer 1) - validate the freshly-transformed link RTM against the trampoline (golden same-signature reference) plus the target signature
        // Catches a garbage RTM->func that dispatch would call, plus any signature-derived field divergence, BEFORE install
        MintVerifyResult rtmCheck = ValidateBakedRtm(L.linkRTM, c.trampolineRTM, c.sig, /*instanceBridge=*/targetSig != nullptr);
        if (!rtmCheck.ok) {
            c.failReason = "link " + std::to_string(i) + " (mod '" + L.modName + "') RTM failed structural validation: " + rtmCheck.reason;
            LOG_WARN("[ChainCompiler] CompileChain: link " + std::to_string(i) + " RTM failed validation (" + rtmCheck.reason + "); falling back to boxing");
            return false;
        }

        L.callNextSlots = std::move(cap.slots);
        LOG_DEBUG("[ChainCompiler] link " + std::to_string(i) + " transformed (" + (L.kind == LinkKind::ShimHook ? "shim" : "typed") + "); captured " + std::to_string(L.callNextSlots.size()) + " CallNext slot(s)");
        // A ShimHook legitimately captures zero, since it forwards via the boxing ICALL rather than a baked slot
        // For a TypedHook, empty means either a pure replacement (legal) or - if the instrument is down - that every baked slot was silently missed, and only the latter is worth warning about
        if (L.kind == LinkKind::TypedHook && L.callNextSlots.empty() && !m_installed) { LOG_WARN("[ChainCompiler] CompileChain: instrument not installed; CallNext slots not captured for link " + std::to_string(i)); }
    }

    // 2. Wire the baked forwards: a TypedHook link's captured CallNext data_items -> link[i+1].linkRTM (which may be a typed clone OR a shim clone, since the baked call op enters either identically), and the last typed link -> trampolineRTM
    // A ShimHook link has no captured slots (it forwards via the boxing ICALL, continued natively by HookManager::ContinueBakedChain), so there is nothing to patch for it
    for (size_t i = 0; i < c.links.size(); ++i) {
        ChainLink& L = c.links[i];
        void* next = (i + 1 < c.links.size()) ? c.links[i + 1].linkRTM : c.trampolineRTM;
        for (uint16_t slot : L.callNextSlots) { PatchDataItem(L.linkRTM, slot, next); }
        L.baked = true;
    }

    return true;
}

bool ChainCompiler::InstallChain(CompiledChain& c) {
    void* newHead = c.links.empty() ? c.trampolineRTM : c.links[0].linkRTM;
    if (!IsValidPointer(newHead)) {
        LOG_ERROR("[ChainCompiler] InstallChain: no head RTM to install");
        return false;
    }

    auto* target = reinterpret_cast<MonoMethodLayout*>(c.targetMethod);
    DWORD oldProtect = 0;

    if (!c.headRTM && target->runtimeMethod) {
        // FIRST INSTALL, target already transformed - overwrite its own RTM in place: direct calls (0xEB) and ldftn bound that pointer at their transform, a swapped +0x14 would munt/strand them
        // Keep the pre-overwrite bytes so teardown can put the original back under the same pointer
        auto* liveRTM = reinterpret_cast<RuntimeMethod*>(target->runtimeMethod);
        c.savedOriginalRTM = *liveRTM;
        c.overwroteOriginal = true;
        MethodPatcher::OverwriteRTM(liveRTM, reinterpret_cast<RuntimeMethod*>(newHead), c.targetMethod);
        c.headRTM = liveRTM;
    } else if (!c.headRTM) {
        // FIRST INSTALL, never transformed, nothing can have bound an RTM yet (r-right?), so transplant the head onto the runtimeMethod slot
        if (!VirtualProtect(c.targetMethod, sizeof(MonoMethodLayout), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG_ERROR("[ChainCompiler] InstallChain: VirtualProtect failed");
            return false;
        }
        target->runtimeMethod = newHead;
        VirtualProtect(c.targetMethod, sizeof(MonoMethodLayout), oldProtect, &oldProtect);
        c.headRTM = newHead;
    } else {
        // RECOMPILE / HOT-RELOAD - overwrite the installed head's CONTENTS in place, because callers cached the headRTM pointer in their data_items and swapping it would strand them
        // The interp latches data_items once per frame, so publish it LAST via an aligned atomic store: an in-flight frame sees the old array (old chain) or the new (new chain), never a torn mix
        // Entry fields are copied non-atomically, which is benign while the head method is unchanged since it re-transforms to identical bytes
        // KNOWN GAP - if link[0]'s USER METHOD changed across the recompile the staged bytes differ, and a game thread entering during the memcpy can latch a torn RTM (deferred with the hot-reload review; the header-stub design would reduce this to one atomic PatchDataItem)
        // Old link RTMs and their clone MonoMethodLayouts leak per recompile - deliberate, since an in-flight frame may still be executing an old link, and `version` counts recompiles for leak accounting
        MethodPatcher::OverwriteRTM(reinterpret_cast<RuntimeMethod*>(c.headRTM), reinterpret_cast<RuntimeMethod*>(newHead), c.targetMethod);
    }

    c.version++;
    LOG_INFO("[ChainCompiler] Installed chain (v" + std::to_string(c.version) + "), links=" + std::to_string(c.links.size()));
    return true;
}

} // namespace S3MM
