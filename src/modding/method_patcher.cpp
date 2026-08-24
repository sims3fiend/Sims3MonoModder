#include "method_patcher.h"
#include "../core/mono_bridge.h"
#include "../util/logger.h"
#include <Windows.h>
#include <cstring>

namespace S3MM {

MethodPatcher& MethodPatcher::Get() {
    static MethodPatcher instance;
    return instance;
}

// COMPILE PROXY ALLOCATION - always the full 0x30-byte MonoMethodInflatedLayout, extension zeroed
// A proxy borrowing an INFLATED original's signature trips transform_method's sig-inflated-bit gate (sig+0x08 & 0x40000000)
// The gate unconditionally reads context at proxy+0x28, four bytes past a plain 0x28-byte MonoMethodLayout allocation
// Mirroring the original's context/declaring makes that read in-bounds and correct
// (context is only consulted for VAR/MVAR positions, absent from hook IL, but a MethodSpec forward like Chain.CallNext<R,..> routes through the same context-taking resolver)
// The proxy's own is_inflated bit stays CLEAR (copied from the hook) so signature/header lazy resolution keeps using the hook's token/klass
// Returns the full allocation; callers use &proxy->method as the MonoMethod* and delete the returned pointer (not the inner method) on failure
static MonoMethodInflatedLayout* MakeCompileProxy(const MonoMethodLayout* replacement, const MonoMethodLayout* original, bool borrowOriginalConvention) {
    auto* p = new MonoMethodInflatedLayout{};
    p->method = *replacement;
    p->method.runtimeMethod = nullptr;
    if (borrowOriginalConvention) {
        p->method.signature = original->signature;
        p->method.flagsPacked = original->flagsPacked;
        if (original->slotAndFlags & Offsets::METHOD_IS_INFLATED_FLAG) {
            auto* inf = reinterpret_cast<const MonoMethodInflatedLayout*>(original);
            p->context = inf->context;
            p->declaring = inf->declaring;
        }
    }
    return p;
}

void MethodPatcher::OverwriteRTM(RuntimeMethod* dst, const RuntimeMethod* src, void* methodBackPointer) {
    RuntimeMethod staged = *src;
    staged.method = methodBackPointer;
    staged.data_items = dst->data_items; // keep the live array during the bulk copy
    memcpy(dst, &staged, sizeof(RuntimeMethod));
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&dst->data_items), src->data_items);
}

bool MethodPatcher::ForcePatchMethod(void* originalMethod, void* replacementMethod, bool* deferredRestore, void** ioInstalledRTM, RuntimeMethod* outSavedOriginalRTM) {
    if (!originalMethod || !replacementMethod) {
        LOG_ERROR("[MethodPatcher] ForcePatchMethod: null method pointer");
        return false;
    }

    if (!IsValidPointer(originalMethod) || !IsValidPointer(replacementMethod)) {
        LOG_ERROR("[MethodPatcher] ForcePatchMethod: invalid method pointer");
        return false;
    }

    auto* original = reinterpret_cast<MonoMethodLayout*>(originalMethod);
    auto* replacement = reinterpret_cast<MonoMethodLayout*>(replacementMethod);

    std::string origName = original->name ? original->name : "<unknown>";
    std::string replName = replacement->name ? replacement->name : "<unknown>";

    LOG_DEBUG("[MethodPatcher] ForcePatch: " + origName + " -> " + replName);

    // FRESH-RTM TRANSPLANT - two install strategies depending on prior compilation
    // Stock mono caches RTMs in MonoDomain->jit_code_hash; the EA build removed that, create_runtime_method (0xe51f20) uses MonoMethod+0x14
    // An already-transformed method can't be re-transformed (transform early-outs on RTM flags & 0x100), so the hook RTM is built off-heap
    // Direct calls (0xEB) and ldftn intern the callee RTM pointer in the caller's data_items at transform time, so an already-compiled original is overwritten IN PLACE (first install and reload alike):
    // pointer-swapping +0x14 would strand every caller transformed before the install, and every delegate created before it
    // Already-compiled original: transform a heap proxy, copy its RTM over the original's own RTM (pre-overwrite bytes go to outSavedOriginalRTM for unhook)
    // Never-compiled original: clobber token/klass so lazy first-call compilation reads the hook's IL
    // Caller is told which path ran via deferredRestore.

    // Detect instance->static (static hook replacing an instance method)
    bool instanceToStatic = ((original->flagsPacked & MethodFlags::STATIC) == 0) && ((replacement->flagsPacked & MethodFlags::STATIC) != 0);

    // Proxy-compile only when original was already compiled (non-null runtimeMethod => transformed RTM we can't re-transform + cached callers; see top block).
    // Never-compiled methods have no RTM at all, so the clobber path suffices and avoids VEH crashes from an unresolvable proxy.
    bool needsProxyCompile = (original->runtimeMethod != nullptr);

    // INFLATED never-compiled target (the common case for a generic interaction method hooked at mod load, before gameplay ever ran it)
    // The clobber fallback is unsound for this shape, lazy header fetch on an inflated method ignores the swapped token/klass and re-derives IL from declaring (+0x2C), so the "hook" would recompile the ORIGINAL body
    // Pre-transform the REAL original instead (exactly what the game's own first call would do so it's safe for inflated methods)
    // so the install proceeds through the RTM-transplant proxy path
    if (!needsProxyCompile && (original->slotAndFlags & Offsets::METHOD_IS_INFLATED_FLAG)) {
        LOG_DEBUG("[MethodPatcher] Inflated never-compiled target - pre-transforming original to enable the proxy path");
        MonoBridge::Get().TransformMethod(originalMethod);
        needsProxyCompile = (original->runtimeMethod != nullptr);
        if (!needsProxyCompile) { LOG_WARN("[MethodPatcher] Pre-transform of inflated target " + origName + " did not produce an RTM - install will likely fail cleanly below"); }
    }

    DWORD oldProtect;

    // HOT-RELOAD IN-PLACE UPDATE - the original already points at a previously installed dispatch RTM
    // Game callers have cached that RTM pointer in their compiled MINT (data_items), exec_dispatch (0x00E525C0) reads ->nativeThunk live off it.
    // We overwrite the installed RTM's contents rather than swapping original->runtimeMethod to a new RTM the callers would never see.
    // Source the new bytecode by transforming the REAL replacement method.
    // Transforming a detached proxy SEH-crashes against freshly-loaded hot-reload images, whereas the real hook method is exactly what the game compiles lazily.
    // Never touch original->runtimeMethod here, so the proxy's fresh-RTM rationale doesn't apply.
    if (ioInstalledRTM && *ioInstalledRTM && original->runtimeMethod == *ioInstalledRTM) {
        void* srcRtm = nullptr;
        if (instanceToStatic) {
            // instance->static needs the original's instance calling convention baked into the MINT,
            // so a signature-borrowing proxy is unavoidable here (inflated-aware: see MakeCompileProxy)
            auto* p = MakeCompileProxy(replacement, original, true);
            if (MonoBridge::Get().TransformMethod(&p->method)) { srcRtm = p->method.runtimeMethod; } // p leaked (mempool-backed)
        } else {
            // Transform the real hook method directly (idempotent if the game already compiled it).
            if (MonoBridge::Get().TransformMethod(replacementMethod)) { srcRtm = replacement->runtimeMethod; }
        }

        if (!srcRtm) {
            // Transform failed - keep the last-working dispatch RTM rather than copying a half-baked one over it.
            // Trust TransformMethod's return, not runtimeMethod: createRuntimeMethod leaves it non-null even when transform faults.
            LOG_ERROR("[MethodPatcher] In-place reload: transform of " + replName + " failed - keeping previous hook");
            return false;
        }

        // Copy the new bytecode RTM over the installed one.
        // Fix the back-pointer on the COPY only (the real method's own RTM keeps pointing at itself for frame walking).
        // Sub-arrays (code, data_items, arg/local offsets, clauses) are domain-mempool allocated, so the copied pointers stay valid.
        // original->runtimeMethod already == *ioInstalledRTM, unchanged.
        OverwriteRTM(reinterpret_cast<RuntimeMethod*>(*ioInstalledRTM), reinterpret_cast<RuntimeMethod*>(srcRtm), originalMethod);
        if (deferredRestore) *deferredRestore = false;
        LOG_INFO("[MethodPatcher] Force patched (in-place RTM): " + origName + " -> " + replName);
        return true;
    }

    if (needsProxyCompile) {
        // PROXY PATH - build the hook RTM off-heap (an already-transformed original can't be re-transformed)
        // Proxy for instance->static must use original's instance calling convention so MINT maps ldarg.0 -> this (hasthis=1)
        // a static convention here would compile an RTM that mismatches call sites
        // MakeCompileProxy is inflated-aware: borrowing an inflated original's signature requires the 0x30-byte
        // allocation w/ mirrored context/declaring, or transform's sig-gated context read runs off the end
        auto* proxyAlloc = MakeCompileProxy(replacement, original, instanceToStatic);
        MonoMethodLayout* compileProxy = &proxyAlloc->method;
        if (instanceToStatic) { LOG_DEBUG("[MethodPatcher] ForcePatch instance->static: proxy uses original's calling convention"); }

        LOG_DEBUG("[MethodPatcher] Pre-compiling via proxy (original already transformed)");

        // Use the MINT transform path (interp_get_imethod + interp_transform_method) instead of InvokeMethod.
        // InvokeMethod actually *executes* the proxy body. For hooks with parameters (e.g. float(float,float)) the interpreter dereferences NULL when copying args from `params == nullptr`.
        // TransformMethod compiles MINT bytecode without entering execution.
        // On success it populates compileProxy->runtimeMethod (via cache at MonoMethod+0x14).
        MonoBridge::Get().TransformMethod(compileProxy);

        if (compileProxy->runtimeMethod) {
            LOG_DEBUG("[MethodPatcher] Fresh RTM from proxy: " + ToHex((uintptr_t)compileProxy->runtimeMethod));

            // FIRST INSTALL - overwrite the original's own RTM in place; +0x14 never changes, so pre-bound call sites and ldftn captures dispatch through the hook too
            // The trampoline is a separate clone with its own RTM (BackupOriginalMethod), so nothing else depends on these bytes, the pre-overwrite copy is the unhook restore
            auto* liveRTM = reinterpret_cast<RuntimeMethod*>(original->runtimeMethod);
            if (outSavedOriginalRTM) *outSavedOriginalRTM = *liveRTM;
            OverwriteRTM(liveRTM, reinterpret_cast<RuntimeMethod*>(compileProxy->runtimeMethod), originalMethod);

            if (ioInstalledRTM) *ioInstalledRTM = liveRTM;

            // compileProxy intentionally leaked - RTM->method may reference it.
            if (deferredRestore) *deferredRestore = false;

            LOG_INFO("[MethodPatcher] Force patched: " + origName + " -> " + replName);
            return true;
        }

        // Proxy compilation failed, fall through to clobber path
        LOG_WARN("[MethodPatcher] Proxy compilation failed for: " + origName + " - falling back to clobber");
        delete proxyAlloc;
    }

    // CLOBBER PATH - No stale hash entry (or proxy failed), overwrite token/klass so lazy compilation reads hook IL
    // INFLATED original + instanceToStatic clobber is unsound: the token/klass swap assumes lazy header fetch reads them, but mono_method_get_header's
    // inflated branch ignores both and re-derives the header from declaring (+0x2C), the "hook" would silently recompile the ORIGINAL body, so refuse instead
    // The full-copy branch \/ is fine since it clears is_inflated along with the rest of slotAndFlags
    if (instanceToStatic && (original->slotAndFlags & Offsets::METHOD_IS_INFLATED_FLAG)) {
        LOG_ERROR("[MethodPatcher] ForcePatchMethod: clobber fallback cannot patch an inflated (generic) instance target with a static hook - " + origName + " left unpatched");
        return false;
    }

    if (!VirtualProtect(originalMethod, sizeof(MonoMethodLayout), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("[MethodPatcher] ForcePatchMethod: VirtualProtect failed");
        return false;
    }

    if (instanceToStatic) {
        original->token = replacement->token;
        original->klass = replacement->klass;
    } else {
        const char* savedName = original->name;
        *original = *replacement;
        original->name = savedName;
    }
    original->runtimeMethod = nullptr;

    VirtualProtect(originalMethod, sizeof(MonoMethodLayout), oldProtect, &oldProtect);

    if (deferredRestore) *deferredRestore = true;

    LOG_INFO("[MethodPatcher] Force patched: " + origName + " -> " + replName);
    return true;
}

} // namespace S3MM
