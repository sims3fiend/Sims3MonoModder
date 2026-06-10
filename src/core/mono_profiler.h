#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace S3MM {

// SAMPLE STACK - Sampling profiler result
struct SampleStack {
    std::vector<void*> frames; // MonoMethod* list, [0] = innermost
    uint64_t count = 0;
};

// TRACE EVENT - Chrome Trace export record
struct TraceEvent {
    void* method;
    uint32_t threadId;
    uint64_t startTick;
    uint64_t durationTicks;
};

// METHOD STATS - Per-method profiling data
struct MethodStats {
    void* method = nullptr;          // MonoMethod*
    std::string fullName;            // Cached "Namespace.Class::Method" string
    uint64_t callCount = 0;          // Total times called
    uint64_t totalTicks = 0;         // Total CPU ticks spent (inclusive)
    uint64_t selfTicks = 0;          // Ticks spent excluding child calls
    uint64_t minTicks = UINT64_MAX;  // Fastest call
    uint64_t maxTicks = 0;           // Slowest call
    std::vector<void*> icallTargets; // Native addrs of icalls this method invokes

    // Caller/callee tracking: MonoMethod* -> (call count, total ticks)
    std::unordered_map<void*, std::pair<uint64_t, uint64_t>> callers; // who calls us: method -> (count, ticks)
    std::unordered_map<void*, std::pair<uint64_t, uint64_t>> callees; // who we call: method -> (count, ticks)
};

// ALLOCATION STATS - Per-type allocation data with call stacks
struct AllocationStats {
    void* klass = nullptr;     // MonoClass*
    std::string typeName;      // Cached type name
    uint64_t allocCount = 0;   // Total allocations
    uint64_t totalBytes = 0;   // Total bytes allocated
    uint32_t instanceSize = 0; // Size per instance

    // Stack hash -> (frames, count) per unique allocation site
    std::unordered_map<uint64_t, std::pair<std::vector<void*>, uint64_t>> callStacks;
};

// BOXING STATS - Per-type boxing data with call stacks
struct BoxingStats {
    void* klass = nullptr;     // MonoClass* of the value type
    std::string typeName;      // Cached type name
    uint64_t boxCount = 0;     // Total boxing operations
    uint64_t totalBytes = 0;   // Total bytes boxed
    uint32_t instanceSize = 0; // Size per boxed instance

    // Stack hash -> (frames, count) per unique boxing site
    std::unordered_map<uint64_t, std::pair<std::vector<void*>, uint64_t>> callStacks;

    // Raw boxed value (up to 8 bytes) -> count, useful for seeing which enum values get boxed
    std::unordered_map<int64_t, uint64_t> boxedValues;
};

// ICALL STATS - Per-native-function call data
struct ICallStats {
    void* nativeAddr = nullptr;  // The resolved native function pointer (unique key)
    std::string name;            // Lazy-resolved "Class::Method" from MonoMethod
    uint64_t callCount = 0;      // Total times this icall was invoked
    std::vector<void*> calledBy; // MonoMethod* list of managed callers
};

// METHOD PROBE - Per-method targeted timing
struct MethodProbe {
    void* targetMethod = nullptr; // MonoMethod*
    std::string methodName;       // Cached "Namespace.Class::Method"
    std::atomic<uint64_t> callCount{0};
    std::atomic<uint64_t> totalCycles{0}; // RDTSC cycles (not QPC ticks)
    std::atomic<uint64_t> minCycles{UINT64_MAX};
    std::atomic<uint64_t> maxCycles{0};
    bool active = false;

    // Non-copyable due to atomics, but we need snapshot support
    MethodProbe() = default;
    MethodProbe(const MethodProbe& o)
        : targetMethod(o.targetMethod), methodName(o.methodName), callCount(o.callCount.load()), totalCycles(o.totalCycles.load()), minCycles(o.minCycles.load()), maxCycles(o.maxCycles.load()), active(o.active) {}
};

// MONO PROFILER - Hooks Mono interpreter to collect profiling data
class MonoProfiler {
  public:
    static constexpr size_t MAX_PROBES = 64;

    static MonoProfiler& Get();

    // Lifecycle
    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    // Profiling control
    void Start();
    void Stop();
    void Reset();
    bool IsRunning() const { return m_running.load(); }

    // Trace recording (for Chrome Trace export)
    void StartTraceRecording(size_t maxEvents = 1000000);
    void StopTraceRecording();
    bool IsTraceRecording() const { return m_traceRecording.load(); }
    bool ExportChromeTrace(const char* filename);

    // Sampling profiler
    void StartSampling(uint32_t intervalMs = 1);
    void StopSampling();
    bool IsSampling() const { return m_samplingActive.load(); }
    std::unordered_map<void*, uint64_t> GetSampleCounts();
    std::vector<SampleStack> GetSampleStacks();
    uint64_t GetTotalSamples() const { return m_totalSamples.load(); }
    void ResetSamples();

    // Method probes - targeted per-method timing via dispatch hook filtering
    bool AttachProbe(void* monoMethod);
    bool DetachProbe(void* monoMethod);
    void DetachAllProbes();
    void ResetProbeStats();
    bool IsMethodProbed(void* monoMethod) const;
    size_t GetProbeCount() const { return m_probeCount.load(std::memory_order_acquire); }
    std::vector<MethodProbe> GetProbeSnapshot() const;

    // Data access (lock-free snapshots for UI)
    std::vector<MethodStats> GetMethodStats();
    std::vector<AllocationStats> GetAllocationStats();
    std::vector<BoxingStats> GetBoxingStats();
    std::vector<ICallStats> GetICallStats();
    uint64_t GetTotalCalls() const { return m_totalCalls.load(); }
    uint64_t GetTotalAllocations() const { return m_totalAllocations.load(); }
    uint64_t GetTotalBoxing() const { return m_totalBoxing.load(); }
    uint64_t GetTotalICalls() const { return m_totalICalls.load(); }
    double GetElapsedSeconds() const;

    // Timing helpers
    static double TicksToMs(uint64_t ticks);
    static double TicksToUs(uint64_t ticks);

  private:
    MonoProfiler() = default;
    ~MonoProfiler() = default;
    MonoProfiler(const MonoProfiler&) = delete;
    MonoProfiler& operator=(const MonoProfiler&) = delete;

    // Hook trampolines
    static void __cdecl HookedInvokeWithState(void* state);
    static void __cdecl HookedAllocationCallback(void* profiler, void* object, void* klass);
    static void* __cdecl HookedLookupInternalCall(void* method);
    static void* __cdecl HookedValueBox(void* domain, void* klass, void* valuePtr);
    static void __cdecl HookedExecDispatch(void* frame, void* threadCtx);
    static void __cdecl HookedExecDispatchVirtual(void* frame, void* arg2, void* arg3, int arg4, void* threadCtx);

    // Original function pointers
    using InvokeWithState_t = void(__cdecl*)(void* state);
    using LookupInternalCall_t = void*(__cdecl*)(void* method);
    using ValueBox_t = void*(__cdecl*)(void* domain, void* klass, void* valuePtr);
    using ExecDispatch_t = void(__cdecl*)(void* frame, void* threadCtx);
    using ExecDispatchVirtual_t = void(__cdecl*)(void* frame, void* arg2, void* arg3, int arg4, void* threadCtx);
    InvokeWithState_t m_originalInvoke = nullptr;
    LookupInternalCall_t m_originalLookupICall = nullptr;
    ValueBox_t m_originalValueBox = nullptr;
    ExecDispatch_t m_originalExecDispatch = nullptr;
    ExecDispatchVirtual_t m_originalExecDispatchVirtual = nullptr;

    // Profiler state
    bool m_initialized = false;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_traceRecording{false};

    // Stats storage
    std::mutex m_statsMutex;
    std::unordered_map<void*, MethodStats> m_methodStats;    // keyed by MonoMethod*
    std::unordered_map<void*, AllocationStats> m_allocStats; // keyed by MonoClass*
    std::unordered_map<void*, BoxingStats> m_boxingStats;    // keyed by MonoClass*
    std::unordered_map<void*, ICallStats> m_icallStats;      // keyed by native addr

    // Counters
    std::atomic<uint64_t> m_totalCalls{0};
    std::atomic<uint64_t> m_totalAllocations{0};
    std::atomic<uint64_t> m_totalBoxing{0};
    std::atomic<uint64_t> m_totalICalls{0};
    std::atomic<uint64_t> m_profileStartTick{0};

    // Trace recording buffer
    std::vector<TraceEvent> m_traceEvents;
    std::atomic<size_t> m_traceWriteIndex{0};
    size_t m_traceMaxEvents = 0;
    uint64_t m_traceStartTick = 0;

    // Timing calibration
    static uint64_t s_tickFrequency;

    // Call stack tracking (per-thread)
    struct CallFrame {
        void* method;
        uint64_t startTick;
        uint64_t childTicks; // Time spent in child calls (to subtract for self-time)
    };
    static thread_local std::vector<CallFrame> t_callStack;

    // Sampling profiler state (hooks SPEC_interp_exec_dispatch)
    std::atomic<bool> m_samplingActive{false};
    uint32_t m_stackWalkInterval = 128; // walk full stack every N dispatches
    std::mutex m_samplingMutex;

    // Sample data (protected by m_samplingMutex)
    std::unordered_map<void*, uint64_t> m_sampleCounts;       // MonoMethod* -> hit count
    std::unordered_map<uint64_t, SampleStack> m_sampleStacks; // hash -> (frames, count)
    std::atomic<uint64_t> m_totalSamples{0};
    std::atomic<uint64_t> m_sampleFaults{0}; // SEH faults during walks

    // Shared dispatch sampling logic (called from both hook paths)
    void RecordDispatchSample(void* frame);

    // Per-thread dispatch counter (for stack walk interval)
    static thread_local uint64_t t_dispatchCount;

    // Method probes - fixed-size array, hot path does linear scan (<=64 entries, cache-friendly)
    MethodProbe m_probes[MAX_PROBES];
    std::atomic<size_t> m_probeCount{0}; // Published count (acquire/release fence)
    mutable std::mutex m_probeMutex;     // Protects attach/detach (not the hot path)

    // Returns probe for method or nullptr
    MethodProbe* FindProbe(void* method, size_t count);
};

} // namespace S3MM
