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

bool MethodPatcher::ForcePatchMethod(void* originalMethod, void* replacementMethod, bool* deferredRestore, void** ioInstalledRTM) {
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

    // DOMAIN HASH CACHE BYPASS - two install strategies depending on prior compilation.
    // If original has a compiled RTM, a stale MonoDomain->jit_code_hash entry would reinstate the old RTM.
    // So pre-compile a fresh RTM via a heap-allocated proxy (never entered into the hash) and transplant it onto original.
    // If original was never compiled (runtimeMethod == nullptr), no hash entry exists.
    // Then clobber token/klass directly so lazy first-call compilation reads the hook's IL.
    // Caller is told which path ran via deferredRestore.

    // Detect instance->static (static hook replacing an instance method)
    bool instanceToStatic = ((original->flagsPacked & MethodFlags::STATIC) == 0) && ((replacement->flagsPacked & MethodFlags::STATIC) != 0);

    // Proxy-compile only when original was already compiled (non-null runtimeMethod => stale hash entry; see top block).
    // Never-compiled methods have no hash entry, so the clobber path suffices and avoids VEH crashes from an unresolvable proxy.
    bool needsProxyCompile = (original->runtimeMethod != nullptr);

    DWORD oldProtect;

    // HOT-RELOAD IN-PLACE UPDATE - the original already points at a previously installed dispatch RTM
    // Game callers have cached that RTM pointer in their compiled MINT (data_items), exec_dispatch (0x00E525C0) reads ->nativeThunk live off it.
    // We overwrite the installed RTM's contents rather than swapping original->runtimeMethod to a new RTM the callers would never see.
    // Source the new bytecode by transforming the REAL replacement method.
    // Transforming a detached proxy SEH-crashes against freshly-loaded hot-reload images, whereas the real hook method is exactly what the game compiles lazily.
    // Never touch original->runtimeMethod or the domain hash here, so the proxy's hash-bypass rationale doesn't apply.
    if (ioInstalledRTM && *ioInstalledRTM && original->runtimeMethod == *ioInstalledRTM) {
        void* srcRtm = nullptr;
        if (instanceToStatic) {
            // instance->static needs the original's instance calling convention baked into the MINT.
            // So a signature-borrowing proxy is unavoidable here.
            auto* p = new MonoMethodLayout(*replacement);
            p->runtimeMethod = nullptr;
            p->signature = original->signature;
            p->flagsPacked = original->flagsPacked;
            if (MonoBridge::Get().TransformMethod(p)) { srcRtm = p->runtimeMethod; } // p leaked (mempool-backed)
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
        memcpy(*ioInstalledRTM, srcRtm, sizeof(RuntimeMethod));
        reinterpret_cast<RuntimeMethod*>(*ioInstalledRTM)->method = originalMethod;
        if (deferredRestore) *deferredRestore = false;
        LOG_INFO("[MethodPatcher] Force patched (in-place RTM): " + origName + " -> " + replName);
        return true;
    }

    if (needsProxyCompile) {
        // PROXY PATH - Bypass stale domain hash entry
        auto* compileProxy = new MonoMethodLayout(*replacement);
        compileProxy->runtimeMethod = nullptr;

        if (instanceToStatic) {
            // Proxy must use original's instance calling convention so MINT maps ldarg.0 -> this (hasthis=1).
            // A static convention here would compile an RTM that mismatches call sites.
            compileProxy->signature = original->signature;
            compileProxy->flagsPacked = original->flagsPacked;
            LOG_DEBUG("[MethodPatcher] ForcePatch instance->static: proxy uses original's calling convention");
        }

        LOG_DEBUG("[MethodPatcher] Pre-compiling via proxy to bypass domain hash cache");

        // Use the MINT transform path (interp_get_imethod + interp_transform_method) instead of InvokeMethod.
        // InvokeMethod actually *executes* the proxy body. For hooks with parameters (e.g. float(float,float)) the interpreter dereferences NULL when copying args from `params == nullptr`.
        // TransformMethod compiles MINT bytecode without entering execution.
        // On success it populates compileProxy->runtimeMethod (via cache at MonoMethod+0x14).
        MonoBridge::Get().TransformMethod(compileProxy);

        if (compileProxy->runtimeMethod) {
            LOG_DEBUG("[MethodPatcher] Fresh RTM from proxy: " + ToHex((uintptr_t)compileProxy->runtimeMethod));

            // Fix RTM back-pointer: RTM->method (offset 0x08) points to proxy, redirect to original for frame walking
            *(void**)((uintptr_t)compileProxy->runtimeMethod + 0x08) = originalMethod;

            // FIRST INSTALL - transplant the fresh RTM onto the original.
            // Remember it so future reloads overwrite its contents in place (handled by the in-place branch above).
            if (!VirtualProtect(originalMethod, sizeof(MonoMethodLayout), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                LOG_ERROR("[MethodPatcher] ForcePatchMethod: VirtualProtect failed");
                delete compileProxy;
                return false;
            }
            original->runtimeMethod = compileProxy->runtimeMethod;
            VirtualProtect(originalMethod, sizeof(MonoMethodLayout), oldProtect, &oldProtect);

            if (ioInstalledRTM) *ioInstalledRTM = compileProxy->runtimeMethod;

            // compileProxy intentionally leaked - RTM->method may reference it.
            if (deferredRestore) *deferredRestore = false;

            LOG_INFO("[MethodPatcher] Force patched: " + origName + " -> " + replName);
            return true;
        }

        // Proxy compilation failed, fall through to clobber path
        LOG_WARN("[MethodPatcher] Proxy compilation failed for: " + origName + " - falling back to clobber");
        delete compileProxy;
    }

    // CLOBBER PATH - No stale hash entry (or proxy failed), overwrite token/klass so lazy compilation reads hook IL
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
