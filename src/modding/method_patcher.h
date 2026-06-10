#pragma once
#include "../core/mono_types.h"

namespace S3MM {

// METHOD PATCHER - Identity-preserving MonoMethod patching utility.
// Redirects a MonoMethod to hook code while leaving its identity fields (token, klass, name, signature, flagsPacked) untouched, so external code still sees the original EA method.
// Stateless; callers manage their own tracking.
class MethodPatcher {
  public:
    static MethodPatcher& Get();

    // Redirects originalMethod to replacementMethod's code. Only originalMethod->runtimeMethod is modified; all identity fields are preserved.
    // Returns false on failure. See the .cpp for the install strategies (proxy compile / in-place reload / clobber).

    // deferredRestore (out, optional): set true when the patch took the clobber fallback.
    // The caller MUST restore the original's identity after the first real call compiles the method.

    // ioInstalledRTM (in/out, optional): hot-reload handle. First install writes the transplanted RuntimeMethod* here.
    // Pass it back on a later call to overwrite that RTM in place, so game callers that cached the pointer dispatch to the new hook without re-resolution.
    bool ForcePatchMethod(void* originalMethod, void* replacementMethod, bool* deferredRestore = nullptr, void** ioInstalledRTM = nullptr);

  private:
    MethodPatcher() = default;
    ~MethodPatcher() = default;
    MethodPatcher(const MethodPatcher&) = delete;
    MethodPatcher& operator=(const MethodPatcher&) = delete;
};

} // namespace S3MM
