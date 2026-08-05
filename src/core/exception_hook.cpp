#include "exception_hook.h"
#include "addresses.h"
#include "mono_bridge.h"
#include "mono_types.h"
#include "../modding/hook_manager.h"
#include "../util/memory.h"
#include "../util/logger.h"
#include "../util/mod_log.h"
#include "exception_helper.h"
#include <detours/detours.h>
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace S3MM {

// STATIC MEMBERS - Hook function pointers and VEH state
ExceptionHook::raise_exception_t ExceptionHook::s_originalRaiseException = nullptr;
ExceptionHook::interp_ex_handler_t ExceptionHook::s_originalInterpExHandler = nullptr;
ExceptionHook::g_error_t ExceptionHook::s_originalGError = nullptr;
PVOID ExceptionHook::s_vehHandle = nullptr;
volatile LONG ExceptionHook::s_inVehHandler = 0;
bool ExceptionHook::s_interpRecoveryEnabled = true;

// SIGNATURE GUARD - verify the recovery site still holds the instructions the VEH rewind assumes: MOV ECX,[EAX] (8B 08) at +CRASH_OFFSET and TEST EAX,EAX (85 C0) at +REWIND_OFFSET
// Read-only and IsReadablePointer-guarded, so a bad base can't fault the probe
bool ExceptionHook::InterpRecoverySignatureIntact(uintptr_t execStateAddr) {
    if (!execStateAddr) return false;

    const uint8_t* crash = reinterpret_cast<const uint8_t*>(execStateAddr + INTERP_CRASH_OFFSET);
    const uint8_t* rewind = reinterpret_cast<const uint8_t*>(execStateAddr + INTERP_REWIND_OFFSET);
    if (!IsReadablePointer((void*)crash, 2) || !IsReadablePointer((void*)rewind, 2)) return false;

    return crash[0] == 0x8B && crash[1] == 0x08 && rewind[0] == 0x85 && rewind[1] == 0xC0;
}

// SINGLETON - Meyer's singleton accessor
ExceptionHook& ExceptionHook::Get() {
    static ExceptionHook instance;
    return instance;
}

// INITIALIZATION - Attach Detours hooks and install VEH
bool ExceptionHook::Initialize() {
    if (m_initialized) return true;

    int hooksInstalled = 0;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    // Hook mono_raise_exception
    uintptr_t raiseAddr = Addresses::Methods.mono_raise_exception;
    if (raiseAddr) {
        s_originalRaiseException = (raise_exception_t)raiseAddr;
        LONG err = DetourAttach(&(PVOID&)s_originalRaiseException, HookedRaiseException);
        if (err == NO_ERROR) {
            hooksInstalled++;
            LOG_INFO("[ExceptionHook] Hooking mono_raise_exception @ " + ToHex(raiseAddr));
        } else {
            LOG_ERROR("[ExceptionHook] DetourAttach mono_raise_exception failed: " + std::to_string(err));
            s_originalRaiseException = nullptr;
        }
    }

    // Hook interp_ex_handler
    uintptr_t interpAddr = Addresses::Methods.interp_ex_handler;
    if (interpAddr) {
        s_originalInterpExHandler = (interp_ex_handler_t)interpAddr;
        LONG err = DetourAttach(&(PVOID&)s_originalInterpExHandler, HookedInterpExHandler);
        if (err == NO_ERROR) {
            hooksInstalled++;
            LOG_INFO("[ExceptionHook] Hooking interp_ex_handler @ " + ToHex(interpAddr));
        } else {
            LOG_ERROR("[ExceptionHook] DetourAttach interp_ex_handler failed: " + std::to_string(err));
            s_originalInterpExHandler = nullptr;
        }
    }

    // Hook g_error
    uintptr_t gerrorAddr = Addresses::Core.g_error;
    if (gerrorAddr) {
        s_originalGError = (g_error_t)gerrorAddr;
        LONG err = DetourAttach(&(PVOID&)s_originalGError, HookedGError);
        if (err == NO_ERROR) {
            hooksInstalled++;
            LOG_INFO("[ExceptionHook] Hooking g_error @ " + ToHex(gerrorAddr));
        } else {
            LOG_ERROR("[ExceptionHook] DetourAttach g_error failed: " + std::to_string(err));
            s_originalGError = nullptr;
        }
    }

    if (hooksInstalled == 0) {
        LOG_ERROR("[ExceptionHook] No hooks could be installed");
        DetourTransactionAbort();
        return false;
    }

    LONG error = DetourTransactionCommit();
    if (error != NO_ERROR) {
        LOG_ERROR("[ExceptionHook] DetourTransactionCommit failed: " + std::to_string(error));
        return false;
    }

    // RECOVERY SIGNATURE GUARD - validate the interp_exec_state rewind site before trusting the VEH's EIP-rewind recovery
    // A co-resident interpreter patcher or a mismatched build alters those bytes, and the rewind would then corrupt the state
    // On mismatch recovery is off and faults fall through to the non-recoverable diagnostic path
    uintptr_t execStateAddr = Addresses::Methods.interp_exec_state;
    s_interpRecoveryEnabled = InterpRecoverySignatureIntact(execStateAddr);
    if (!s_interpRecoveryEnabled) {
        LOG_WARN("[ExceptionHook] interp_exec_state recovery signature MISMATCH @ " + ToHex(execStateAddr) + " - VEH EIP-rewind recovery DISABLED (co-resident interpreter patch or build mismatch)");
        ModLog::Get().Log("[Runtime]", "Interpreter recovery disabled: interp_exec_state signature altered (possible co-resident patcher)", LogLevel::Warn);
    }

    // VEH for native crashes in interpreter
    s_vehHandle = AddVectoredExceptionHandler(1, VectoredExceptionHandler);
    if (s_vehHandle) {
        hooksInstalled++;
        LOG_INFO("[ExceptionHook] Installed VEH for interpreter crash detection");
    } else {
        LOG_WARN("[ExceptionHook] AddVectoredExceptionHandler failed");
    }

    m_initialized = true;
    LOG_INFO("[ExceptionHook] Installed " + std::to_string(hooksInstalled) + "/4 hooks");
    return true;
}

// SHUTDOWN - Remove VEH and detach Detours hooks
void ExceptionHook::Shutdown() {
    if (!m_initialized) return;

    // Remove VEH first (before detaching Detours hooks)
    if (s_vehHandle) {
        RemoveVectoredExceptionHandler(s_vehHandle);
        s_vehHandle = nullptr;
        LOG_INFO("[ExceptionHook] Removed VEH");
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (s_originalRaiseException) { DetourDetach(&(PVOID&)s_originalRaiseException, HookedRaiseException); }
    if (s_originalInterpExHandler) { DetourDetach(&(PVOID&)s_originalInterpExHandler, HookedInterpExHandler); }
    if (s_originalGError) { DetourDetach(&(PVOID&)s_originalGError, HookedGError); }
    DetourTransactionCommit();

    m_initialized = false;
    LOG_INFO("[ExceptionHook] Shutdown complete");
}

// HOOK RAISE - C-level managed exception throws (184 xrefs)
void __cdecl ExceptionHook::HookedRaiseException(void* exception) {
    std::string excInfo = ExtractExceptionInfo(exception);
    std::string assemblyName = GetThrowingAssemblyName();

    if (!excInfo.empty()) {
        std::string prefix = assemblyName.empty() ? "" : (assemblyName + ": ");
        LOG_WARN("[ExceptionHook:raise] " + prefix + excInfo);
        ModLog::Get().Log(assemblyName.empty() ? "[Runtime]" : assemblyName, "EXCEPTION: " + excInfo, LogLevel::Warn);
    }

    s_originalRaiseException(exception);
}

// HOOK INTERP - Interpreter exception handler, called via PTR_01195b64, catches exceptions propagating through the frame chain
void __cdecl ExceptionHook::HookedInterpExHandler(void* exception) {
    std::string excInfo = ExtractExceptionInfo(exception);
    std::string assemblyName = GetThrowingAssemblyName();

    if (!excInfo.empty()) {
        std::string prefix = assemblyName.empty() ? "" : (assemblyName + ": ");
        LOG_WARN("[ExceptionHook:interp] " + prefix + excInfo);
        ModLog::Get().Log(assemblyName.empty() ? "[Runtime]" : assemblyName, "INTERP EXCEPTION: " + excInfo, LogLevel::Warn);
    }

    s_originalInterpExHandler(exception);
}

// HOOK G_ERROR - GLib fatal errors (assertions, Mono internals). Variadic, we format then forward with "%s".
void __cdecl ExceptionHook::HookedGError(const char* format, ...) {
    // Format the message from varargs
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    std::string msg(buffer);
    std::string assemblyName = GetThrowingAssemblyName();

    std::string prefix = assemblyName.empty() ? "" : (assemblyName + ": ");
    LOG_ERROR("[ExceptionHook:g_error] " + prefix + msg);
    ModLog::Get().Log(assemblyName.empty() ? "[Runtime]" : assemblyName, "FATAL: " + msg, LogLevel::Error);

    // Forward to original with pre-formatted string
    s_originalGError("%s", buffer);
}

// QUARANTINE ATTRIBUTION - runs in a post-AV context where the process is likely dying and the heap may be fragile, so the whole body sits inside __try
// A secondary fault is swallowed and the original crash still proceeds to its dump, since the VEH returns CONTINUE_SEARCH either way
// POD locals only, because __try needs an object-free frame... QuarantineBakedChain's formatting and file work lives in its own callee frame (allocation-free: stack buffers + WriteFile on pre-opened handles)
static void TryQuarantineFaultingBakedChain(void* threadCtxV, const char* reason) {
    __try {
        ThreadContext* tc = reinterpret_cast<ThreadContext*>(threadCtxV);
        // Attribute only to the innermost frame, the method actually executing at the fault
        // Walking to an ancestor would blame a baked caller for a crash inside a non-baked callee it invoked, so one unrelated interpreter AV would disable a good bake
        // mis-attribution bad, missing attribution okay
        InterpFrame* f = tc ? tc->current_frame : nullptr;
        if (!f || !IsValidPointer(f)) return;
        RuntimeMethod* rtm = f->runtime_method;
        if (!rtm || !IsValidPointer(rtm) || !rtm->method || !IsValidPointer(rtm->method)) return;
        if (HookManager::Get().QuarantineBakedChain(rtm->method, reason)) { // no-ops unless it's an active baked target
            // Baked-target fault, so append the typed forensic record (faulting link, ip offset, this, per-arg values) to baked_crashes.log
            // Same fragile-context rules - the callee guards every deref, and a secondary fault lands in this frame's __except
            HookManager::Get().DumpBakedCrashRecord(rtm->method, f, reason);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // a secondary fault while attributing - swallow; the original crash proceeds via CONTINUE_SEARCH
    }
}

// VEH HANDLER - First-chance native crash handler for interpreter. Checks if faulting EIP is within interp_exec_state, logs frame state, then lets normal crash handling proceed.
LONG CALLBACK ExceptionHook::VectoredExceptionHandler(PEXCEPTION_POINTERS exInfo) {
    // Reentrancy guard - if our logging itself faults, bail immediately
    if (InterlockedCompareExchange(&s_inVehHandler, 1, 0) != 0) { return EXCEPTION_CONTINUE_SEARCH; }

    DWORD code = exInfo->ExceptionRecord->ExceptionCode;

    // Only handle access violations and illegal instructions
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION) {
        InterlockedExchange(&s_inVehHandler, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD eip = exInfo->ContextRecord->Eip;
    uintptr_t execStateAddr = Addresses::Methods.interp_exec_state;

    // Check if EIP is within interp_exec_state
    if (!execStateAddr || eip < execStateAddr || eip >= execStateAddr + INTERP_EXEC_STATE_ESTIMATED_SIZE) {
        InterlockedExchange(&s_inVehHandler, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // We're crashing inside the interpreter dispatch loop
    DWORD faultAddr = 0;
    if (code == EXCEPTION_ACCESS_VIOLATION && exInfo->ExceptionRecord->NumberParameters >= 2) { faultAddr = (DWORD)exInfo->ExceptionRecord->ExceptionInformation[1]; }

    // RECOVERY - Post-call exception check crash at interp_exec_state+0x0C3A.
    // The interpreter reads the exception slot from the eval stack and tries to dereference it.
    // If the slot contains a small integer (not a valid pointer), we recover by clearing the bad pointer from EAX and the eval stack, then rewinding EIP to the TEST instruction so JZ takes the "no exception" path.
    //
    // Crash site: MOV ECX, [EAX] at +0x0C3A, rewind to: TEST EAX, EAX at +0x0C2C
    // Layout: [ESI] = exception slot, EAX = bad ptr, [EBP-0x28]/[EBP-0x50] = copies
    //
    // Fires every tick for hooked methods with stale eval stack exception slots.
    // Rate-limited logging, full diagnostics on first 3 hits, then silent recovery.
    // Gated on the signature guard - if a co-resident patch or a build mismatch altered the rewind site, s_interpRecoveryEnabled is false and we fall through to diagnostics rather than rewinding EIP onto shifted instructions
    if (s_interpRecoveryEnabled && code == EXCEPTION_ACCESS_VIOLATION && (eip - execStateAddr) == INTERP_CRASH_OFFSET && exInfo->ContextRecord->Eax < 0x10000) // small integer, not a valid pointer
    {
        static volatile LONG s_recoveryCount = 0;
        LONG count = InterlockedIncrement(&s_recoveryCount);

        // Clear the corrupt exception pointer
        DWORD esi = exInfo->ContextRecord->Esi;
        *(DWORD*)(esi) = 0;             // eval stack exception slot
        exInfo->ContextRecord->Eax = 0; // EAX register

        // Rewind EIP to TEST EAX,EAX, the JZ will now take the "no exception" path
        exInfo->ContextRecord->Eip = execStateAddr + INTERP_REWIND_OFFSET;

        // Rate-limited logging: full details for first 3, periodic summary after
        if (count <= 3) {
            std::string frameChain = GetInterpreterFrameChain();
            std::string assemblyName = GetThrowingAssemblyName();

            LOG_WARN("[ExceptionHook:VEH] RECOVERED (" + std::to_string(count) + "): corrupt exception ptr " + ToHex(faultAddr) + " in interp_exec_state+" + ToHex(eip - execStateAddr));

            // EBX = RuntimeMethod* of the callee whose return triggered the bad exception slot
            DWORD ebx = exInfo->ContextRecord->Ebx;
            if (ebx && IsValidPointer((void*)ebx)) {
                void* monoMethod = *(void**)((uintptr_t)ebx + 0x08); // RuntimeMethod->method
                if (monoMethod && IsValidPointer(monoMethod)) {
                    auto* layout = reinterpret_cast<MonoMethodLayout*>(monoMethod);
                    std::string calleeName;
                    if (layout->klass && IsValidPointer(layout->klass)) {
                        void* nsPtr = *(void**)((uintptr_t)layout->klass + Offsets::CLASS_NAMESPACE);
                        void* clsPtr = *(void**)((uintptr_t)layout->klass + Offsets::CLASS_NAME);
                        if (nsPtr && IsValidPointer(nsPtr) && std::strlen((const char*)nsPtr) > 0) {
                            calleeName += (const char*)nsPtr;
                            calleeName += ".";
                        }
                        if (clsPtr && IsValidPointer(clsPtr)) calleeName += (const char*)clsPtr;
                    }
                    if (layout->name && IsValidPointer((void*)layout->name)) calleeName += "::" + std::string(layout->name);

                    uint8_t stackSize = *(uint8_t*)((uintptr_t)ebx + 0x40);
                    LOG_WARN("[ExceptionHook:VEH]   Callee (CALL that returned corrupt): " + calleeName + " (RTM=" + ToHex(ebx) + ", stack_size=" + std::to_string(stackSize) + ")");
                }
            }

            if (!frameChain.empty()) { LOG_WARN("[ExceptionHook:VEH]   Frame chain: " + frameChain); }
            ModLog::Get().Log(assemblyName.empty() ? "[Runtime]" : assemblyName, "RECOVERED from interpreter crash @ " + ToHex(eip) + (frameChain.empty() ? "" : " | " + frameChain), LogLevel::Warn);

            if (count == 3) { LOG_WARN("[ExceptionHook:VEH] Suppressing further recovery logs (recurring per-tick pattern)"); }
        } else if ((count % 10000) == 0) {
            // Periodic heartbeat so user knows it's still happening
            LOG_INFO("[ExceptionHook:VEH] Recovery count: " + std::to_string(count));
        }

        InterlockedExchange(&s_inVehHandler, 0);
        return EXCEPTION_CONTINUE_EXECUTION; // resume execution!
    }

    // NON-RECOVERABLE - Full diagnostic dump for unknown crash sites
    std::string frameChain = GetInterpreterFrameChain();
    std::string assemblyName = GetThrowingAssemblyName();

    std::string codeStr = (code == EXCEPTION_ACCESS_VIOLATION) ? "ACCESS_VIOLATION" : "ILLEGAL_INSTRUCTION";
    LOG_ERROR("[ExceptionHook:VEH] " + codeStr + " in interp_exec_state");
    LOG_ERROR("[ExceptionHook:VEH]   EIP=" + ToHex(eip) + " (interp_exec_state+" + ToHex(eip - execStateAddr) + ")");
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        std::string rwStr = (exInfo->ExceptionRecord->ExceptionInformation[0] == 0) ? "reading" : "writing";
        LOG_ERROR("[ExceptionHook:VEH]   " + rwStr + " address " + ToHex(faultAddr));
    }
    LOG_ERROR("[ExceptionHook:VEH]   EAX=" + ToHex(exInfo->ContextRecord->Eax) + " ECX=" + ToHex(exInfo->ContextRecord->Ecx) + " EDX=" + ToHex(exInfo->ContextRecord->Edx) + " EBX=" + ToHex(exInfo->ContextRecord->Ebx));
    LOG_ERROR("[ExceptionHook:VEH]   ESI=" + ToHex(exInfo->ContextRecord->Esi) + " EDI=" + ToHex(exInfo->ContextRecord->Edi) + " ESP=" + ToHex(exInfo->ContextRecord->Esp) + " EBP=" + ToHex(exInfo->ContextRecord->Ebp));

    if (!frameChain.empty()) {
        LOG_ERROR("[ExceptionHook:VEH]   Frame chain: " + frameChain);
    } else {
        LOG_ERROR("[ExceptionHook:VEH]   Frame chain: <unavailable>");
    }

    // CO-RESIDENT ATTRIBUTION - when the fault address lands inside another native mod's module, name it in the crash record so the log reads "conflict with <OtherMod.dll>" instead of blaming MM
    // Snapshot lookup only (Memory::SnapshotModules ran at mono-ready), so it is safe in this post-crash context ;))
    std::string culprit = Memory::ResolveModuleForAddress(faultAddr);
    if (!culprit.empty()) {
        LOG_ERROR("[ExceptionHook:VEH]   fault address in co-resident module " + culprit);
        ModLog::Get().Log("[Runtime]", "Interpreter crash faulted inside co-resident module " + culprit, LogLevel::Error);
    }

    // Non-recoverable crash, log to ModLog and let normal crash handling proceed
    ModLog::Get().Log(assemblyName.empty() ? "[Runtime]" : assemblyName, "NATIVE CRASH in interpreter @ " + ToHex(eip) + " accessing " + ToHex(faultAddr) + (frameChain.empty() ? "" : " | " + frameChain), LogLevel::Error);

    // CRASH QUARANTINE - record a hit against the baked chain executing at the fault, so a repeatedly-crashing baked target boxes or drops on a later launch instead of crashing again
    // Innermost frame only, see TryQuarantineFaultingBakedChain 4 moree
    // The reentrancy guard is held throughout and attribution is SEH-isolated, so the original crash proceeds regardless and the quarantine takes effect on a future run
    // Plain stack buffer for reason, so no std::string is constructed in the fragile context
    if (Addresses::Profiler.g_thread_context_tls_index) {
        DWORD tlsIndex = *(DWORD*)Addresses::Profiler.g_thread_context_tls_index;
        if (void* ctxv = TlsGetValue(tlsIndex)) {
            char reason[192];
            _snprintf_s(reason, sizeof(reason), _TRUNCATE, "%s interp_exec_state+0x%X accessing 0x%08X", codeStr.c_str(), (unsigned)(eip - execStateAddr), (unsigned)faultAddr);
            TryQuarantineFaultingBakedChain(ctxv, reason);
        }
    }

    InterlockedExchange(&s_inVehHandler, 0);
    return EXCEPTION_CONTINUE_SEARCH; // let normal crash handling proceed
}

// FRAME ATTRIBUTION - Walk interpreter frames to find the assembly that threw. Same TLS + frame walking pattern as HookManager::NativeGetCallerHookToken.
std::string ExceptionHook::GetThrowingAssemblyName() {
    // Get thread context from TLS
    if (!Addresses::Profiler.g_thread_context_tls_index) return "";

    DWORD tlsIndex = *(DWORD*)Addresses::Profiler.g_thread_context_tls_index;
    void* ctx = TlsGetValue(tlsIndex);
    if (!ctx) return "";

    // ThreadContext->current_frame at offset 0x08
    ThreadContext* threadCtx = reinterpret_cast<ThreadContext*>(ctx);
    InterpFrame* frame = threadCtx->current_frame;
    if (!frame) return "";

    // Walk frames (limit to 10 for safety)
    InterpFrame* current = frame;
    for (int i = 0; i < 10 && current; i++, current = current->parent) {
        if (!IsValidPointer(current)) break;

        RuntimeMethod* rtm = current->runtime_method;
        if (!rtm || !IsValidPointer(rtm) || !rtm->method || !IsValidPointer(rtm->method)) continue;

        auto* layout = reinterpret_cast<MonoMethodLayout*>(rtm->method);
        if (!layout->klass || !IsValidPointer(layout->klass)) continue;

        // klass->image at offset 0x00, image->assembly_name at offset 0x18
        void* image = *(void**)((uintptr_t)layout->klass + Offsets::CLASS_IMAGE);
        if (!image || !IsValidPointer(image)) continue;

        const char* name = *(const char**)((uintptr_t)image + Offsets::IMAGE_ASSEMBLY_NAME);
        if (name && IsValidPointer((void*)name)) { return std::string(name); }
    }

    return "";
}

// FRAME CHAIN - Walk interpreter frames and build "Namespace.Class::Method <- ..." call chain for VEH crash diagnostics
std::string ExceptionHook::GetInterpreterFrameChain() {
    // Get thread context from TLS
    if (!Addresses::Profiler.g_thread_context_tls_index) return "";

    DWORD tlsIndex = *(DWORD*)Addresses::Profiler.g_thread_context_tls_index;
    void* ctx = TlsGetValue(tlsIndex);
    if (!ctx) return "";

    ThreadContext* threadCtx = reinterpret_cast<ThreadContext*>(ctx);
    InterpFrame* frame = threadCtx->current_frame;
    if (!frame) return "";

    std::string chain;
    InterpFrame* current = frame;
    for (int i = 0; i < 20 && current; i++, current = current->parent) {
        if (!IsValidPointer(current)) break;

        RuntimeMethod* rtm = current->runtime_method;
        if (!rtm || !IsValidPointer(rtm)) continue;
        if (!rtm->method || !IsValidPointer(rtm->method)) continue;

        auto* layout = reinterpret_cast<MonoMethodLayout*>(rtm->method);
        if (!layout->klass || !IsValidPointer(layout->klass)) continue;

        // Get class namespace + name
        const char* ns = nullptr;
        const char* className = nullptr;
        const char* methodName = nullptr;

        // klass->namespace at CLASS_NAMESPACE, klass->name at CLASS_NAME
        void* nsPtr = *(void**)((uintptr_t)layout->klass + Offsets::CLASS_NAMESPACE);
        if (nsPtr && IsValidPointer(nsPtr)) ns = (const char*)nsPtr;

        void* classNamePtr = *(void**)((uintptr_t)layout->klass + Offsets::CLASS_NAME);
        if (classNamePtr && IsValidPointer(classNamePtr)) className = (const char*)classNamePtr;

        if (layout->name && IsValidPointer((void*)layout->name)) { methodName = layout->name; }

        if (!chain.empty()) chain += " <- ";

        if (ns && std::strlen(ns) > 0) {
            chain += ns;
            chain += ".";
        }
        chain += (className ? className : "?");
        chain += "::";
        chain += (methodName ? methodName : "?");
    }

    return chain;
}

} // namespace S3MM
