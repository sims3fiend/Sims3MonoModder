#pragma once
#include <string>
#include <Windows.h>

namespace S3MM {

// EXCEPTION HOOK
//   1. mono_raise_exception (0x00EA18F0) - C-level managed throws
//   2. interp_ex_handler (0x00E53740) - interpreter exception handler
//   3. g_error (0x00E46B10) - GLib fatal assertions (754 xrefs)
//   4. VEH - native access violations inside interp_exec_state
class ExceptionHook {
  public:
    static ExceptionHook& Get();

    // Install all Detours hooks + VEH
    bool Initialize();

    // Detach hooks + remove VEH (call on shutdown)
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }

  private:
    ExceptionHook() = default;
    ~ExceptionHook() = default;
    ExceptionHook(const ExceptionHook&) = delete;
    ExceptionHook& operator=(const ExceptionHook&) = delete;

    bool m_initialized = false;

    // mono_raise_exception hook
    static void __cdecl HookedRaiseException(void* exception);
    using raise_exception_t = void(__cdecl*)(void*);
    static raise_exception_t s_originalRaiseException;

    // interp_ex_handler hook
    static void __cdecl HookedInterpExHandler(void* exception);
    using interp_ex_handler_t = void(__cdecl*)(void*);
    static interp_ex_handler_t s_originalInterpExHandler;

    // g_error hook
    static void __cdecl HookedGError(const char* format, ...);
    using g_error_t = void(__cdecl*)(const char* format, ...);
    static g_error_t s_originalGError;

    // VEH for native crashes in interpreter
    static LONG CALLBACK VectoredExceptionHandler(PEXCEPTION_POINTERS exInfo);
    static PVOID s_vehHandle;
    static volatile LONG s_inVehHandler; // reentrancy guard

    // Estimated size of interp_exec_state dispatch loop.
    // Function spans 0x00E53EB0 to at least 0x00E593DF (error handlers).
    // 0x7000 (28KB) is a conservative upper bound.
    static constexpr size_t INTERP_EXEC_STATE_ESTIMATED_SIZE = 0x7000;

    // Helper: walk interpreter frames to find the source assembly name
    static std::string GetThrowingAssemblyName();

    // Helper: walk interpreter frames to build full method call chain
    static std::string GetInterpreterFrameChain();
};

} // namespace S3MM
