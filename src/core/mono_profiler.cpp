#include "mono_profiler.h"
#include "mono_bridge.h"
#include "mono_types.h"
#include "addresses.h"
#include "../util/logger.h"

#include <detours/detours.h>
#include <intrin.h>
#include <algorithm>

namespace S3MM {

// STATIC MEMBERS
uint64_t MonoProfiler::s_tickFrequency = 0;
thread_local std::vector<MonoProfiler::CallFrame> MonoProfiler::t_callStack;
thread_local uint64_t MonoProfiler::t_dispatchCount = 0;

// Profiler event bits
constexpr uint32_t PROFILER_EVENT_ALLOCATIONS = 0x80;

// SINGLETON
MonoProfiler& MonoProfiler::Get() {
    static MonoProfiler instance;
    return instance;
}

// INITIALIZATION
bool MonoProfiler::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("[MonoProfiler] Initializing...");

    // Get timing frequency
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    s_tickFrequency = freq.QuadPart;
    LOG_INFO("[MonoProfiler] Timer frequency: " + std::to_string(s_tickFrequency) + " Hz");

    // Store original function pointers
    m_originalInvoke = (InvokeWithState_t)Addresses::Methods.interp_mono_invoke_with_state;
    m_originalLookupICall = (LookupInternalCall_t)Addresses::InternalCalls.mono_lookup_internal_call;
    m_originalValueBox = (ValueBox_t)Addresses::Core.mono_value_box;
    m_originalExecDispatch = (ExecDispatch_t)Addresses::Methods.SPEC_interp_exec_dispatch;
    m_originalExecDispatchVirtual = (ExecDispatchVirtual_t)Addresses::Methods.SPEC_interp_exec_dispatch_virtual;

    // Install Detours hooks
    LONG error = DetourTransactionBegin();
    if (error != NO_ERROR) {
        LOG_ERROR("[MonoProfiler] DetourTransactionBegin failed: " + std::to_string(error));
        return false;
    }

    error = DetourUpdateThread(GetCurrentThread());
    if (error != NO_ERROR) {
        LOG_ERROR("[MonoProfiler] DetourUpdateThread failed: " + std::to_string(error));
        DetourTransactionAbort();
        return false;
    }

    error = DetourAttach(&(PVOID&)m_originalInvoke, HookedInvokeWithState);
    if (error != NO_ERROR) {
        LOG_ERROR("[MonoProfiler] DetourAttach (invoke) failed: " + std::to_string(error));
        DetourTransactionAbort();
        return false;
    }

    error = DetourAttach(&(PVOID&)m_originalLookupICall, HookedLookupInternalCall);
    if (error != NO_ERROR) {
        LOG_ERROR("[MonoProfiler] DetourAttach (icall lookup) failed: " + std::to_string(error));
        DetourTransactionAbort();
        return false;
    }

    error = DetourAttach(&(PVOID&)m_originalValueBox, HookedValueBox);
    if (error != NO_ERROR) {
        LOG_ERROR("[MonoProfiler] DetourAttach (value_box) failed: " + std::to_string(error));
        DetourTransactionAbort();
        return false;
    }

    if (m_originalExecDispatch) {
        error = DetourAttach(&(PVOID&)m_originalExecDispatch, HookedExecDispatch);
        if (error != NO_ERROR) {
            LOG_ERROR("[MonoProfiler] DetourAttach (exec_dispatch) failed: " + std::to_string(error));
            DetourTransactionAbort();
            return false;
        }
    }

    if (m_originalExecDispatchVirtual) {
        error = DetourAttach(&(PVOID&)m_originalExecDispatchVirtual, HookedExecDispatchVirtual);
        if (error != NO_ERROR) {
            LOG_ERROR("[MonoProfiler] DetourAttach (exec_dispatch_virtual) failed: " + std::to_string(error));
            DetourTransactionAbort();
            return false;
        }
    }

    error = DetourTransactionCommit();
    if (error != NO_ERROR) {
        LOG_ERROR("[MonoProfiler] DetourTransactionCommit failed: " + std::to_string(error));
        return false;
    }

    LOG_INFO("[MonoProfiler] Hooked interp_mono_invoke_with_state @ " + ToHex(Addresses::Methods.interp_mono_invoke_with_state));
    LOG_INFO("[MonoProfiler] Hooked mono_lookup_internal_call @ " + ToHex(Addresses::InternalCalls.mono_lookup_internal_call));
    LOG_INFO("[MonoProfiler] Hooked mono_value_box @ " + ToHex(Addresses::Core.mono_value_box));
    if (m_originalExecDispatch) {
        LOG_INFO("[MonoProfiler] Hooked SPEC_interp_exec_dispatch @ " + ToHex(Addresses::Methods.SPEC_interp_exec_dispatch));
    } else {
        LOG_WARN("[MonoProfiler] SPEC_interp_exec_dispatch not resolved - sampling profiler unavailable");
    }
    if (m_originalExecDispatchVirtual) { LOG_INFO("[MonoProfiler] Hooked SPEC_interp_exec_dispatch_virtual @ " + ToHex(Addresses::Methods.SPEC_interp_exec_dispatch_virtual)); }

    // Set up allocation profiler via Mono globals: needs non-null profiler ptr, our allocation callback, and the allocation event bit

    // Set our profiler instance pointer (just needs to be non-null)
    *(uintptr_t*)Addresses::Profiler.g_current_profiler = (uintptr_t)this;

    // Set allocation callback
    *(uintptr_t*)Addresses::Profiler.g_allocation_callback = (uintptr_t)&HookedAllocationCallback;

    // Enable allocation events
    *(uint32_t*)Addresses::Profiler.g_profiler_events |= PROFILER_EVENT_ALLOCATIONS;

    LOG_INFO("[MonoProfiler] Enabled allocation profiling via Mono globals");

    m_initialized = true;
    LOG_INFO("[MonoProfiler] Initialized successfully");
    return true;
}

void MonoProfiler::Shutdown() {
    if (!m_initialized) return;

    LOG_INFO("[MonoProfiler] Shutting down...");

    StopSampling();
    Stop();

    // Disable allocation profiling
    *(uint32_t*)Addresses::Profiler.g_profiler_events &= ~PROFILER_EVENT_ALLOCATIONS;
    *(uintptr_t*)Addresses::Profiler.g_allocation_callback = 0;
    *(uintptr_t*)Addresses::Profiler.g_current_profiler = 0;

    // Remove Detours hooks
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)m_originalInvoke, HookedInvokeWithState);
    DetourDetach(&(PVOID&)m_originalLookupICall, HookedLookupInternalCall);
    DetourDetach(&(PVOID&)m_originalValueBox, HookedValueBox);
    if (m_originalExecDispatch) { DetourDetach(&(PVOID&)m_originalExecDispatch, HookedExecDispatch); }
    if (m_originalExecDispatchVirtual) { DetourDetach(&(PVOID&)m_originalExecDispatchVirtual, HookedExecDispatchVirtual); }
    DetourTransactionCommit();

    m_initialized = false;
    LOG_INFO("[MonoProfiler] Shutdown complete");
}

// PROFILING CONTROL
void MonoProfiler::Start() {
    if (!m_initialized) return;

    // Record start time if not already profiling
    if (!m_running.load()) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        m_profileStartTick.store(now.QuadPart);
    }

    m_running.store(true);
    LOG_INFO("[MonoProfiler] Started profiling");
}

void MonoProfiler::Stop() {
    m_running.store(false);
    LOG_INFO("[MonoProfiler] Stopped profiling");
}

void MonoProfiler::Reset() {
    bool wasRunning = m_running.load();
    m_running.store(false);

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_methodStats.clear();
        m_allocStats.clear();
        m_boxingStats.clear();
        m_icallStats.clear();
    }

    m_totalCalls.store(0);
    m_totalAllocations.store(0);
    m_totalBoxing.store(0);
    m_totalICalls.store(0);

    // Also reset trace
    m_traceRecording.store(false);
    m_traceWriteIndex.store(0);

    // Reset sampling data
    {
        std::lock_guard<std::mutex> lock(m_samplingMutex);
        m_sampleCounts.clear();
        m_sampleStacks.clear();
    }
    m_totalSamples.store(0);
    m_sampleFaults.store(0);

    // Reset timer
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    m_profileStartTick.store(now.QuadPart);

    if (wasRunning) { m_running.store(true); }

    LOG_INFO("[MonoProfiler] Reset stats");
}

// TRACE RECORDING
void MonoProfiler::StartTraceRecording(size_t maxEvents) {
    if (!m_initialized) return;

    // Pre-allocate buffer
    m_traceEvents.resize(maxEvents);
    m_traceMaxEvents = maxEvents;
    m_traceWriteIndex.store(0);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    m_traceStartTick = now.QuadPart;

    m_traceRecording.store(true);
    LOG_INFO("[MonoProfiler] Started trace recording (max " + std::to_string(maxEvents) + " events)");
}

void MonoProfiler::StopTraceRecording() {
    m_traceRecording.store(false);
    LOG_INFO("[MonoProfiler] Stopped trace recording (" + std::to_string(m_traceWriteIndex.load()) + " events captured)");
}

bool MonoProfiler::ExportChromeTrace(const char* filename) {
    FILE* file = fopen(filename, "w");
    if (!file) {
        LOG_ERROR("[MonoProfiler] Failed to open trace file: " + std::string(filename));
        return false;
    }

    size_t eventCount = m_traceWriteIndex.load();
    if (eventCount > m_traceMaxEvents) { eventCount = m_traceMaxEvents; }

    fprintf(file, "{\"traceEvents\":[\n");

    bool first = true;
    for (size_t i = 0; i < eventCount; i++) {
        const TraceEvent& evt = m_traceEvents[i];

        // Get method name (need to look it up)
        std::string methodName = "<unknown>";
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            auto it = m_methodStats.find(evt.method);
            if (it != m_methodStats.end()) { methodName = it->second.fullName; }
        }

        // Escape quotes in method name for JSON
        std::string escaped;
        for (char c : methodName) {
            if (c == '"')
                escaped += "\\\"";
            else if (c == '\\')
                escaped += "\\\\";
            else
                escaped += c;
        }

        // Convert ticks to microseconds
        double startUs = (double)(evt.startTick - m_traceStartTick) / (double)s_tickFrequency * 1000000.0;
        double durUs = (double)evt.durationTicks / (double)s_tickFrequency * 1000000.0;

        if (!first) fprintf(file, ",\n");
        first = false;

        // Chrome Trace format: Complete event (ph: "X")
        fprintf(file, "{\"name\":\"%s\",\"cat\":\"mono\",\"ph\":\"X\",\"ts\":%.3f,\"dur\":%.3f,\"pid\":1,\"tid\":%u}", escaped.c_str(), startUs, durUs, evt.threadId);
    }

    fprintf(file, "\n]}\n");
    fclose(file);

    LOG_INFO("[MonoProfiler] Exported " + std::to_string(eventCount) + " events to " + std::string(filename));
    return true;
}

// DATA ACCESS
std::vector<MethodStats> MonoProfiler::GetMethodStats() {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    std::vector<MethodStats> result;
    result.reserve(m_methodStats.size());
    for (const auto& [key, stats] : m_methodStats) { result.push_back(stats); }
    return result;
}

std::vector<AllocationStats> MonoProfiler::GetAllocationStats() {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    std::vector<AllocationStats> result;
    result.reserve(m_allocStats.size());
    for (const auto& [key, stats] : m_allocStats) { result.push_back(stats); }
    return result;
}

std::vector<BoxingStats> MonoProfiler::GetBoxingStats() {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    std::vector<BoxingStats> result;
    result.reserve(m_boxingStats.size());
    for (const auto& [key, stats] : m_boxingStats) { result.push_back(stats); }
    return result;
}

std::vector<ICallStats> MonoProfiler::GetICallStats() {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    std::vector<ICallStats> result;
    result.reserve(m_icallStats.size());
    for (const auto& [key, stats] : m_icallStats) { result.push_back(stats); }
    return result;
}

// TIMING HELPERS
double MonoProfiler::TicksToMs(uint64_t ticks) {
    if (s_tickFrequency == 0) return 0.0;
    return (double)ticks / (double)s_tickFrequency * 1000.0;
}

double MonoProfiler::TicksToUs(uint64_t ticks) {
    if (s_tickFrequency == 0) return 0.0;
    return (double)ticks / (double)s_tickFrequency * 1000000.0;
}

double MonoProfiler::GetElapsedSeconds() const {
    if (s_tickFrequency == 0) return 0.0;
    uint64_t startTick = m_profileStartTick.load();
    if (startTick == 0) return 0.0;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t elapsed = now.QuadPart - startTick;
    return (double)elapsed / (double)s_tickFrequency;
}

// HOOKED INVOKE - Hot path, every managed method call goes through here
void __cdecl MonoProfiler::HookedInvokeWithState(void* state) {
    auto& profiler = Get();

    // Get method from state (MonoInvokeState.pMethod at offset 0)
    void* method = *(void**)state;

    // Probe check - works independently of profiler running state
    size_t probeCount = profiler.m_probeCount.load(std::memory_order_acquire);
    if (probeCount > 0 && method) {
        MethodProbe* probe = profiler.FindProbe(method, probeCount);
        if (probe) {
            uint64_t start = __rdtsc();
            profiler.m_originalInvoke(state);
            uint64_t elapsed = __rdtsc() - start;

            probe->callCount.fetch_add(1, std::memory_order_relaxed);
            probe->totalCycles.fetch_add(elapsed, std::memory_order_relaxed);

            uint64_t cur = probe->minCycles.load(std::memory_order_relaxed);
            while (elapsed < cur && !probe->minCycles.compare_exchange_weak(cur, elapsed));
            cur = probe->maxCycles.load(std::memory_order_relaxed);
            while (elapsed > cur && !probe->maxCycles.compare_exchange_weak(cur, elapsed));

            // If profiler is also running, still record method stats below
            // But if not running, we're done
            if (!profiler.m_running.load(std::memory_order_relaxed)) { return; }
            // Fall through to normal profiling path (method stats etc.)
            // Note: original already called above, so we need a different path
            // Actually we can't fall through since we already called original.
            // Just return, probe timing is what matters here.
            return;
        }
    }

    // Fast path: if not running, just call original
    // (sampling is handled by HookedExecDispatch, not here)
    if (!profiler.m_running.load(std::memory_order_relaxed)) {
        profiler.m_originalInvoke(state);
        return;
    }

    if (!method) {
        profiler.m_originalInvoke(state);
        return;
    }

    // Record start time
    LARGE_INTEGER startTime;
    QueryPerformanceCounter(&startTime);
    uint64_t startTick = startTime.QuadPart;

    // Push to call stack for self-time calculation
    t_callStack.push_back({method, startTick, 0});

    // Increment total calls
    profiler.m_totalCalls.fetch_add(1, std::memory_order_relaxed);

    // Call original
    profiler.m_originalInvoke(state);

    // Record end time
    LARGE_INTEGER endTime;
    QueryPerformanceCounter(&endTime);
    uint64_t endTick = endTime.QuadPart;
    uint64_t elapsed = endTick - startTick;

    // Pop from call stack and calculate self time
    uint64_t childTicks = 0;
    if (!t_callStack.empty()) {
        childTicks = t_callStack.back().childTicks;
        t_callStack.pop_back();

        // Add our elapsed time to parent's childTicks
        if (!t_callStack.empty()) { t_callStack.back().childTicks += elapsed; }
    }

    // Self time = total time minus time spent in children
    uint64_t selfTime = (elapsed > childTicks) ? (elapsed - childTicks) : 0;

    // Capture caller before we do anything else (parent on the call stack)
    void* caller = nullptr;
    if (!t_callStack.empty()) { caller = t_callStack.back().method; }

    // Record trace event if tracing is enabled
    if (profiler.m_traceRecording.load(std::memory_order_relaxed)) {
        size_t idx = profiler.m_traceWriteIndex.fetch_add(1, std::memory_order_relaxed);
        if (idx < profiler.m_traceMaxEvents) {
            TraceEvent& evt = profiler.m_traceEvents[idx];
            evt.method = method;
            evt.threadId = GetCurrentThreadId();
            evt.startTick = startTick;
            evt.durationTicks = elapsed;
        }
    }

    // Update stats (minimize time holding lock)
    {
        std::lock_guard<std::mutex> lock(profiler.m_statsMutex);
        auto& stats = profiler.m_methodStats[method];

        if (stats.method == nullptr) {
            // First time seeing this method - cache the name
            stats.method = method;

            // Get class and method names
            void* klass = *(void**)((uintptr_t)method + Offsets::METHOD_KLASS);
            const char* methodName = *(const char**)((uintptr_t)method + Offsets::METHOD_NAME);

            if (klass && methodName) {
                const char* className = MonoBridge::GetClassName(klass);
                const char* nameSpace = MonoBridge::GetClassNamespace(klass);

                std::string fullName;
                if (nameSpace && nameSpace[0]) { fullName = std::string(nameSpace) + "."; }
                if (className) { fullName += std::string(className) + "::"; }
                if (methodName) { fullName += methodName; }
                stats.fullName = fullName;
            } else {
                stats.fullName = "<unknown>";
            }
        }

        stats.callCount++;
        stats.totalTicks += elapsed;
        stats.selfTicks += selfTime;
        if (elapsed < stats.minTicks) stats.minTicks = elapsed;
        if (elapsed > stats.maxTicks) stats.maxTicks = elapsed;

        // Record caller -> callee relationship
        if (caller) {
            auto& callerEntry = stats.callers[caller];
            callerEntry.first++;           // call count from this caller
            callerEntry.second += elapsed; // ticks attributed from this caller

            auto& callerStats = profiler.m_methodStats[caller];
            auto& calleeEntry = callerStats.callees[method];
            calleeEntry.first++;           // call count to this callee
            calleeEntry.second += elapsed; // ticks attributed to this callee
        }
    }
}

// HOOKED ALLOCATION CALLBACK - Called by Mono's allocation profiler infrastructure
void __cdecl MonoProfiler::HookedAllocationCallback(void* profilerPtr, void* object, void* klass) {
    (void)profilerPtr;
    (void)object;
    auto& profiler = Get();

    if (!profiler.m_running.load(std::memory_order_relaxed)) { return; }

    if (!klass) return;

    profiler.m_totalAllocations.fetch_add(1, std::memory_order_relaxed);

    // Walk the interpreter's frame stack to get the managed call stack
    std::vector<void*> stackFrames;
    stackFrames.reserve(32);

    // Get TLS index for thread context
    DWORD tlsIndex = *(DWORD*)Addresses::Profiler.g_thread_context_tls_index;
    void* threadCtx = TlsGetValue(tlsIndex);

    if (threadCtx) {
        // Walk the frame chain from threadCtx[2] (offset 0x08)
        void* frame = *(void**)((uintptr_t)threadCtx + Offsets::THREAD_CTX_CURRENT_FRAME);

        // Also try current frame at threadCtx[1] (offset 0x04) if frame chain is empty
        if (!frame) { frame = *(void**)((uintptr_t)threadCtx + Offsets::THREAD_CTX_BASE_FRAME); }

        // Walk the linked list of InterpFrames
        constexpr size_t MAX_FRAMES = 64;
        size_t frameCount = 0;

        while (frame && frameCount < MAX_FRAMES) {
            // frame[1] (offset 0x04) = RuntimeMethod*
            void* runtimeMethod = *(void**)((uintptr_t)frame + Offsets::INTERP_FRAME_RUNTIME_METHOD);

            if (runtimeMethod) {
                // RuntimeMethod[2] (offset 0x08) = MonoMethod*
                void* monoMethod = *(void**)((uintptr_t)runtimeMethod + Offsets::RUNTIME_METHOD_MONO_METHOD);

                if (monoMethod) { stackFrames.push_back(monoMethod); }
            }

            // frame[0] (offset 0x00) = previous frame
            frame = *(void**)((uintptr_t)frame + Offsets::INTERP_FRAME_PREV);
            frameCount++;
        }
    }

    // Fallback: if interpreter frame walk failed, use t_callStack (invoke boundaries only)
    if (stackFrames.empty()) {
        stackFrames.reserve(t_callStack.size());
        for (const auto& frame : t_callStack) { stackFrames.push_back(frame.method); }
    }

    // Hash the stack for deduplication
    uint64_t stackHash = 0;
    for (void* m : stackFrames) { stackHash ^= (uintptr_t)m + 0x9e3779b9 + (stackHash << 6) + (stackHash >> 2); }

    // Read instance size from MonoClass + 0x54
    uint32_t instanceSize = *(uint32_t*)((uintptr_t)klass + Offsets::CLASS_INSTANCE_SIZE);

    // Update allocation stats
    {
        std::lock_guard<std::mutex> lock(profiler.m_statsMutex);
        auto& stats = profiler.m_allocStats[klass];

        if (stats.klass == nullptr) {
            stats.klass = klass;
            stats.instanceSize = instanceSize;

            const char* className = MonoBridge::GetClassName(klass);
            const char* nameSpace = MonoBridge::GetClassNamespace(klass);

            std::string typeName;
            if (nameSpace && nameSpace[0]) { typeName = std::string(nameSpace) + "."; }
            if (className) { typeName += className; }
            stats.typeName = typeName.empty() ? "<unknown>" : typeName;
        }

        stats.allocCount++;
        stats.totalBytes += instanceSize;

        // Store unique call stacks with their counts
        auto& stackEntry = stats.callStacks[stackHash];
        if (stackEntry.first.empty()) { stackEntry.first = std::move(stackFrames); }
        stackEntry.second++;
    }
}

// HOOKED LOOKUP INTERNAL CALL - Called when managed code invokes an icall/pinvoke
void* __cdecl MonoProfiler::HookedLookupInternalCall(void* method) {
    auto& profiler = Get();

    // Always call original, we need the native address
    void* nativeAddr = profiler.m_originalLookupICall(method);

    // Fast path: skip if not running or lookup failed
    if (!profiler.m_running.load(std::memory_order_relaxed) || !nativeAddr) { return nativeAddr; }

    profiler.m_totalICalls.fetch_add(1, std::memory_order_relaxed);

    // Get the current managed caller from the call stack
    void* caller = nullptr;
    if (!t_callStack.empty()) { caller = t_callStack.back().method; }

    // Update icall stats
    {
        std::lock_guard<std::mutex> lock(profiler.m_statsMutex);

        // Update ICallStats for this native address
        auto& icallStats = profiler.m_icallStats[nativeAddr];
        if (icallStats.nativeAddr == nullptr) {
            // First time seeing this icall - resolve name from MonoMethod
            icallStats.nativeAddr = nativeAddr;

            if (method) {
                void* klass = *(void**)((uintptr_t)method + Offsets::METHOD_KLASS);
                const char* methodName = *(const char**)((uintptr_t)method + Offsets::METHOD_NAME);

                if (klass && methodName) {
                    const char* className = MonoBridge::GetClassName(klass);
                    const char* nameSpace = MonoBridge::GetClassNamespace(klass);

                    std::string fullName;
                    if (nameSpace && nameSpace[0]) { fullName = std::string(nameSpace) + "."; }
                    if (className) { fullName += std::string(className) + "::"; }
                    if (methodName) { fullName += methodName; }
                    icallStats.name = fullName;
                } else {
                    icallStats.name = "<unknown>";
                }
            } else {
                icallStats.name = "<unknown>";
            }
        }
        icallStats.callCount++;

        // Track which managed methods call this icall
        if (caller) {
            auto& calledBy = icallStats.calledBy;
            if (std::find(calledBy.begin(), calledBy.end(), caller) == calledBy.end()) { calledBy.push_back(caller); }

            // Also update the caller's MethodStats with this icall target
            auto it = profiler.m_methodStats.find(caller);
            if (it != profiler.m_methodStats.end()) {
                auto& targets = it->second.icallTargets;
                if (std::find(targets.begin(), targets.end(), nativeAddr) == targets.end()) { targets.push_back(nativeAddr); }
            }
        }
    }

    return nativeAddr;
}

// HOOKED VALUE BOX - Called when a value type is boxed, captures full call stack
void* __cdecl MonoProfiler::HookedValueBox(void* domain, void* klass, void* valuePtr) {
    auto& profiler = Get();

    // Always call original first
    void* result = profiler.m_originalValueBox(domain, klass, valuePtr);

    if (!profiler.m_running.load(std::memory_order_relaxed) || !klass) { return result; }

    profiler.m_totalBoxing.fetch_add(1, std::memory_order_relaxed);

    // Walk the interpreter's frame stack for the real managed call stack (t_callStack only contains invoke boundaries, not internal interpreter calls)
    std::vector<void*> stackFrames;
    stackFrames.reserve(32);

    // Get TLS index for thread context
    DWORD tlsIndex = *(DWORD*)Addresses::Profiler.g_thread_context_tls_index;
    void* threadCtx = TlsGetValue(tlsIndex);

    if (threadCtx) {
        // Walk the frame chain from threadCtx[2] (offset 0x08)
        // Outer frame chain linking invoke boundaries
        void* frame = *(void**)((uintptr_t)threadCtx + Offsets::THREAD_CTX_CURRENT_FRAME);

        // Also try current frame at threadCtx[1] (offset 0x04) if frame chain is empty
        if (!frame) { frame = *(void**)((uintptr_t)threadCtx + Offsets::THREAD_CTX_BASE_FRAME); }

        // Walk the linked list of InterpFrames
        constexpr size_t MAX_FRAMES = 64;
        size_t frameCount = 0;

        while (frame && frameCount < MAX_FRAMES) {
            // frame[1] (offset 0x04) = RuntimeMethod*
            void* runtimeMethod = *(void**)((uintptr_t)frame + Offsets::INTERP_FRAME_RUNTIME_METHOD);

            if (runtimeMethod) {
                // RuntimeMethod[2] (offset 0x08) = MonoMethod*
                void* monoMethod = *(void**)((uintptr_t)runtimeMethod + Offsets::RUNTIME_METHOD_MONO_METHOD);

                if (monoMethod) { stackFrames.push_back(monoMethod); }
            }

            // frame[0] (offset 0x00) = previous frame
            frame = *(void**)((uintptr_t)frame + Offsets::INTERP_FRAME_PREV);
            frameCount++;
        }
    }

    // Fallback: if interpreter frame walk failed, use t_callStack (invoke boundaries only)
    if (stackFrames.empty()) {
        stackFrames.reserve(t_callStack.size());
        for (const auto& frame : t_callStack) { stackFrames.push_back(frame.method); }
    }

    // Hash the stack for deduplication
    uint64_t stackHash = 0;
    for (void* m : stackFrames) { stackHash ^= (uintptr_t)m + 0x9e3779b9 + (stackHash << 6) + (stackHash >> 2); }

    // Read instance size from MonoClass + 0x54
    uint32_t instanceSize = *(uint32_t*)((uintptr_t)klass + Offsets::CLASS_INSTANCE_SIZE);

    // Capture the actual value being boxed (for enums/small value types)
    // Value size = instanceSize - 8 (8-byte object header)
    int64_t boxedValue = 0;
    if (valuePtr) {
        uint32_t valueSize = (instanceSize > 8) ? (instanceSize - 8) : 0;
        if (valueSize == 1) {
            boxedValue = *(int8_t*)valuePtr;
        } else if (valueSize == 2) {
            boxedValue = *(int16_t*)valuePtr;
        } else if (valueSize <= 4) {
            boxedValue = *(int32_t*)valuePtr;
        } else if (valueSize <= 8) {
            boxedValue = *(int64_t*)valuePtr;
        } else {
            // Larger struct - just capture first 8 bytes as fingerprint
            boxedValue = *(int64_t*)valuePtr;
        }
    }

    // Update boxing stats
    {
        std::lock_guard<std::mutex> lock(profiler.m_statsMutex);
        auto& stats = profiler.m_boxingStats[klass];

        if (stats.klass == nullptr) {
            stats.klass = klass;
            stats.instanceSize = instanceSize;

            const char* className = MonoBridge::GetClassName(klass);
            const char* nameSpace = MonoBridge::GetClassNamespace(klass);

            std::string typeName;
            if (nameSpace && nameSpace[0]) { typeName = std::string(nameSpace) + "."; }
            if (className) { typeName += className; }
            stats.typeName = typeName.empty() ? "<unknown>" : typeName;
        }

        stats.boxCount++;
        stats.totalBytes += instanceSize;

        // Store unique call stacks with their counts
        auto& stackEntry = stats.callStacks[stackHash];
        if (stackEntry.first.empty()) { stackEntry.first = std::move(stackFrames); }
        stackEntry.second++;

        // Track boxed values
        stats.boxedValues[boxedValue]++;
    }

    return result;
}

// SAMPLING PROFILER - Hooks dispatch to count MINT_CALL, walks full frame chain every Nth dispatch
void MonoProfiler::StartSampling(uint32_t intervalMs) {
    (void)intervalMs; // not used in hook-based approach
    if (!m_originalExecDispatch && !m_originalExecDispatchVirtual) {
        LOG_ERROR("[MonoProfiler] Cannot start sampling: no dispatch hooks available");
        return;
    }
    if (m_samplingActive.load()) return;

    m_samplingActive.store(true);
    LOG_INFO("[MonoProfiler] Started dispatch sampling (stack walk every " + std::to_string(m_stackWalkInterval) + " dispatches)");
}

void MonoProfiler::StopSampling() {
    if (!m_samplingActive.load()) return;
    m_samplingActive.store(false);

    LOG_INFO("[MonoProfiler] Stopped dispatch sampling (" + std::to_string(m_totalSamples.load()) + " dispatches, " + std::to_string(m_sampleFaults.load()) + " faults)");
}

void MonoProfiler::ResetSamples() {
    std::lock_guard<std::mutex> lock(m_samplingMutex);
    m_sampleCounts.clear();
    m_sampleStacks.clear();
    m_totalSamples.store(0);
    m_sampleFaults.store(0);
}

std::unordered_map<void*, uint64_t> MonoProfiler::GetSampleCounts() {
    std::lock_guard<std::mutex> lock(m_samplingMutex);
    return m_sampleCounts;
}

std::vector<SampleStack> MonoProfiler::GetSampleStacks() {
    std::lock_guard<std::mutex> lock(m_samplingMutex);
    std::vector<SampleStack> result;
    result.reserve(m_sampleStacks.size());
    for (const auto& [hash, stack] : m_sampleStacks) { result.push_back(stack); }
    return result;
}

// SEH-protected frame walker - standalone function with no C++ objects that need unwinding.
// Writes into a raw buffer. Returns frame count, or 0 on fault.
static size_t WalkFramesSEH(void* startFrame, void** outBuf, size_t maxFrames) {
    size_t count = 0;
    __try {
        void* frame = startFrame;
        while (frame && count < maxFrames) {
            void* runtimeMethod = *(void**)((uintptr_t)frame + Offsets::INTERP_FRAME_RUNTIME_METHOD);
            if (runtimeMethod) {
                void* monoMethod = *(void**)((uintptr_t)runtimeMethod + Offsets::RUNTIME_METHOD_MONO_METHOD);
                if (monoMethod) { outBuf[count++] = monoMethod; }
            }
            void* prev = *(void**)((uintptr_t)frame + Offsets::INTERP_FRAME_PREV);
            if (prev == frame) break; // self-loop guard
            frame = prev;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0; // fault, discard
    }
    return count;
}

// DISPATCH SAMPLING - Shared logic for both hook paths
void MonoProfiler::RecordDispatchSample(void* frame) {
    m_totalSamples.fetch_add(1, std::memory_order_relaxed);
    t_dispatchCount++;

    // Every Nth dispatch: walk the full frame chain and record
    if ((t_dispatchCount & (m_stackWalkInterval - 1)) == 0) {
        constexpr size_t MAX_FRAMES = 64;
        void* frameBuf[MAX_FRAMES];

        size_t frameCount = WalkFramesSEH(frame, frameBuf, MAX_FRAMES);

        if (frameCount == 0) {
            m_sampleFaults.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        uint64_t stackHash = 0;
        for (size_t i = 0; i < frameCount; i++) {
            uintptr_t m = (uintptr_t)frameBuf[i];
            stackHash ^= m + 0x9e3779b9 + (stackHash << 6) + (stackHash >> 2);
        }

        {
            std::lock_guard<std::mutex> lock(m_samplingMutex);

            for (size_t i = 0; i < frameCount; i++) { m_sampleCounts[frameBuf[i]]++; }

            auto& entry = m_sampleStacks[stackHash];
            if (entry.frames.empty()) { entry.frames.assign(frameBuf, frameBuf + frameCount); }
            entry.count++;
        }
    }
}

// METHOD PROBES - Targeted per-method timing via dispatch hook filtering.
// Hot path (no probes): single atomic load, branch-not-taken.

// Resolve method name from MonoMethod* (same pattern as MethodStats caching)
static std::string ResolveMethodName(void* method) {
    if (!method) return "<null>";

    void* klass = *(void**)((uintptr_t)method + Offsets::METHOD_KLASS);
    const char* methodName = *(const char**)((uintptr_t)method + Offsets::METHOD_NAME);

    if (klass && methodName) {
        const char* className = MonoBridge::GetClassName(klass);
        const char* nameSpace = MonoBridge::GetClassNamespace(klass);

        std::string fullName;
        if (nameSpace && nameSpace[0]) { fullName = std::string(nameSpace) + "."; }
        if (className) { fullName += std::string(className) + "::"; }
        if (methodName) { fullName += methodName; }
        return fullName;
    }
    return "<unknown>";
}

bool MonoProfiler::AttachProbe(void* monoMethod) {
    if (!monoMethod) return false;
    if (!m_originalExecDispatch && !m_originalExecDispatchVirtual) {
        LOG_ERROR("[MonoProfiler] Cannot attach probe: dispatch hooks not installed");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_probeMutex);

    // Check for duplicate
    size_t count = m_probeCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; i++) {
        if (m_probes[i].active && m_probes[i].targetMethod == monoMethod) {
            LOG_WARN("[MonoProfiler] Probe already attached to method");
            return false;
        }
    }

    // Find first inactive slot or append
    size_t slot = MAX_PROBES;
    for (size_t i = 0; i < count; i++) {
        if (!m_probes[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == MAX_PROBES) {
        if (count >= MAX_PROBES) {
            LOG_ERROR("[MonoProfiler] Max probes reached (" + std::to_string(MAX_PROBES) + ")");
            return false;
        }
        slot = count;
    }

    // Populate probe
    auto& probe = m_probes[slot];
    probe.targetMethod = monoMethod;
    probe.methodName = ResolveMethodName(monoMethod);
    probe.callCount.store(0, std::memory_order_relaxed);
    probe.totalCycles.store(0, std::memory_order_relaxed);
    probe.minCycles.store(UINT64_MAX, std::memory_order_relaxed);
    probe.maxCycles.store(0, std::memory_order_relaxed);
    probe.active = true;

    // Publish: if we used a new slot past current count, bump count
    if (slot >= count) { m_probeCount.store(slot + 1, std::memory_order_release); }

    LOG_INFO("[MonoProfiler] Probe attached: " + probe.methodName + " (method=" + ToHex((uintptr_t)monoMethod) + ", slot=" + std::to_string(slot) + ", count=" + std::to_string(m_probeCount.load()) +
             ", dispatchHook=" + (m_originalExecDispatch ? "YES" : "NO") + ", virtualHook=" + (m_originalExecDispatchVirtual ? "YES" : "NO") + ")");
    return true;
}

bool MonoProfiler::DetachProbe(void* monoMethod) {
    if (!monoMethod) return false;

    std::lock_guard<std::mutex> lock(m_probeMutex);

    size_t count = m_probeCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; i++) {
        if (m_probes[i].active && m_probes[i].targetMethod == monoMethod) {
            m_probes[i].active = false;
            LOG_INFO("[MonoProfiler] Probe detached: " + m_probes[i].methodName);

            // Shrink count if this was the last slot
            while (count > 0 && !m_probes[count - 1].active) { count--; }
            m_probeCount.store(count, std::memory_order_release);
            return true;
        }
    }
    return false;
}

void MonoProfiler::DetachAllProbes() {
    std::lock_guard<std::mutex> lock(m_probeMutex);

    size_t count = m_probeCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; i++) { m_probes[i].active = false; }
    m_probeCount.store(0, std::memory_order_release);
    LOG_INFO("[MonoProfiler] All probes detached");
}

void MonoProfiler::ResetProbeStats() {
    std::lock_guard<std::mutex> lock(m_probeMutex);

    size_t count = m_probeCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; i++) {
        if (m_probes[i].active) {
            m_probes[i].callCount.store(0, std::memory_order_relaxed);
            m_probes[i].totalCycles.store(0, std::memory_order_relaxed);
            m_probes[i].minCycles.store(UINT64_MAX, std::memory_order_relaxed);
            m_probes[i].maxCycles.store(0, std::memory_order_relaxed);
        }
    }
}

bool MonoProfiler::IsMethodProbed(void* monoMethod) const {
    size_t count = m_probeCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; i++) {
        if (m_probes[i].active && m_probes[i].targetMethod == monoMethod) return true;
    }
    return false;
}

std::vector<MethodProbe> MonoProfiler::GetProbeSnapshot() const {
    std::lock_guard<std::mutex> lock(m_probeMutex);

    std::vector<MethodProbe> result;
    size_t count = m_probeCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; i++) {
        if (m_probes[i].active) {
            result.push_back(m_probes[i]); // Uses copy ctor (reads atomics)
        }
    }
    return result;
}

MethodProbe* MonoProfiler::FindProbe(void* method, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (m_probes[i].active && m_probes[i].targetMethod == method) return &m_probes[i];
    }
    return nullptr;
}

// HOOKED EXEC DISPATCH - Direct MINT_CALL path
// 00e525c0: void __cdecl (InterpFrame*, ThreadContext*)
void __cdecl MonoProfiler::HookedExecDispatch(void* frame, void* threadCtx) {
    auto& profiler = Get();

    // Probe check - fast path: single atomic load, branch-not-taken when no probes
    size_t probeCount = profiler.m_probeCount.load(std::memory_order_acquire);
    if (probeCount > 0) {
        void* rtm = *(void**)((uintptr_t)frame + Offsets::INTERP_FRAME_RUNTIME_METHOD);
        if (rtm) {
            void* method = *(void**)((uintptr_t)rtm + Offsets::RUNTIME_METHOD_MONO_METHOD);

            MethodProbe* probe = profiler.FindProbe(method, probeCount);
            if (probe) {
                uint64_t start = __rdtsc();
                profiler.m_originalExecDispatch(frame, threadCtx);
                uint64_t elapsed = __rdtsc() - start;

                probe->callCount.fetch_add(1, std::memory_order_relaxed);
                probe->totalCycles.fetch_add(elapsed, std::memory_order_relaxed);

                // CAS loops for min/max
                uint64_t cur = probe->minCycles.load(std::memory_order_relaxed);
                while (elapsed < cur && !probe->minCycles.compare_exchange_weak(cur, elapsed));
                cur = probe->maxCycles.load(std::memory_order_relaxed);
                while (elapsed > cur && !probe->maxCycles.compare_exchange_weak(cur, elapsed));

                // Still do sampling if active
                if (profiler.m_samplingActive.load(std::memory_order_relaxed)) { profiler.RecordDispatchSample(frame); }
                return;
            }
        }
    }

    // Unprobed path
    profiler.m_originalExecDispatch(frame, threadCtx);

    if (profiler.m_samplingActive.load(std::memory_order_relaxed)) { profiler.RecordDispatchSample(frame); }
}

// HOOKED EXEC DISPATCH VIRTUAL - Virtual/delegate MINT_CALL path
// 00e52630: void __cdecl (InterpFrame*, ptr, dword, int, ThreadContext*)
void __cdecl MonoProfiler::HookedExecDispatchVirtual(void* frame, void* arg2, void* arg3, int arg4, void* threadCtx) {
    auto& profiler = Get();

    // Probe check - same fast path as direct dispatch
    size_t probeCount = profiler.m_probeCount.load(std::memory_order_acquire);
    if (probeCount > 0) {
        void* rtm = *(void**)((uintptr_t)frame + Offsets::INTERP_FRAME_RUNTIME_METHOD);
        if (rtm) {
            void* method = *(void**)((uintptr_t)rtm + Offsets::RUNTIME_METHOD_MONO_METHOD);
            MethodProbe* probe = profiler.FindProbe(method, probeCount);
            if (probe) {
                uint64_t start = __rdtsc();
                profiler.m_originalExecDispatchVirtual(frame, arg2, arg3, arg4, threadCtx);
                uint64_t elapsed = __rdtsc() - start;

                probe->callCount.fetch_add(1, std::memory_order_relaxed);
                probe->totalCycles.fetch_add(elapsed, std::memory_order_relaxed);

                uint64_t cur = probe->minCycles.load(std::memory_order_relaxed);
                while (elapsed < cur && !probe->minCycles.compare_exchange_weak(cur, elapsed));
                cur = probe->maxCycles.load(std::memory_order_relaxed);
                while (elapsed > cur && !probe->maxCycles.compare_exchange_weak(cur, elapsed));

                if (profiler.m_samplingActive.load(std::memory_order_relaxed)) { profiler.RecordDispatchSample(frame); }
                return;
            }
        }
    }

    // Unprobed path
    profiler.m_originalExecDispatchVirtual(frame, arg2, arg3, arg4, threadCtx);

    if (profiler.m_samplingActive.load(std::memory_order_relaxed)) { profiler.RecordDispatchSample(frame); }
}

} // namespace S3MM
