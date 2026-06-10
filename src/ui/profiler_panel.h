#pragma once
#include "../core/mono_profiler.h"
#include <vector>
#include <string>

namespace S3MM {

class ProfilerPanel {
  public:
    ProfilerPanel() = default;
    ~ProfilerPanel() = default;

    void Render();

    // Set filter text externally (for context menu integration)
    void SetFilter(const std::string& filter);

  private:
    // Sorting
    enum class SortColumn { Name, Calls, CallsPerSec, TotalTime, PctTotal, SelfTime, SelfAvgTime, Samples };

    enum class AllocSortColumn { TypeName, Count, AllocsPerSec, Size, TotalBytes };
    enum class BoxingSortColumn { TypeName, BoxCount, BoxPerSec, Size, CallSites };

    void RenderControls();
    void RenderMethodsTable();
    void RenderAllocationsTable();
    void RenderBoxingTable();
    void RenderICallsTable();
    void ExportToCSV();
    void ExportChromeTrace();

    // Shared rendering helpers
    static void RenderPctColored(double pct);
    void RenderCallStackTree(const std::unordered_map<uint64_t, std::pair<std::vector<void*>, uint64_t>>& stacks);

    // Cached data (refreshed periodically)
    std::vector<MethodStats> m_cachedMethodStats;
    std::vector<AllocationStats> m_cachedAllocStats;
    std::vector<BoxingStats> m_cachedBoxingStats;
    std::vector<ICallStats> m_cachedICallStats;
    float m_refreshTimer = 0.0f;
    float m_refreshInterval = 0.5f; // Refresh every 500ms

    // Sorting state
    SortColumn m_sortColumn = SortColumn::TotalTime;
    bool m_sortDescending = true;

    AllocSortColumn m_allocSortColumn = AllocSortColumn::Count;
    bool m_allocSortDescending = true;

    BoxingSortColumn m_boxingSortColumn = BoxingSortColumn::BoxCount;
    bool m_boxingSortDescending = true;

    enum class ICallSortColumn { NativeAddr, Name, Calls, CalledByCount };
    ICallSortColumn m_icallSortColumn = ICallSortColumn::Calls;
    bool m_icallSortDescending = true;

    // Filtering
    char m_filterText[256] = "";

    // Display options
    int m_maxRows = 100;

    // Boxing call stacks window state
    std::string m_boxingWindowTypeName;
    void* m_boxingWindowKlass = nullptr; // MonoClass* for enum resolution
    std::unordered_map<uint64_t, std::pair<std::vector<void*>, uint64_t>> m_boxingWindowCallStacks;
    std::unordered_map<int64_t, uint64_t> m_boxingWindowValues; // value -> count
    bool m_showBoxingWindow = false;

    // Allocation call stacks window state
    std::string m_allocWindowTypeName;
    std::unordered_map<uint64_t, std::pair<std::vector<void*>, uint64_t>> m_allocWindowCallStacks;
    bool m_showAllocWindow = false;

    // Cached sample data
    std::unordered_map<void*, uint64_t> m_cachedSampleCounts;
    uint64_t m_cachedTotalSamples = 0;

    // Probe state
    std::vector<MethodProbe> m_cachedProbes;

    // Method detail window state
    std::string m_methodWindowName;
    void* m_methodWindowMethod = nullptr; // MonoMethod* for the selected method
    bool m_showMethodWindow = false;

    void RenderMethodDetailWindow();
    void RenderProbesTable();
    void NavigateToMethod(void* monoMethod);           // Switch detail window to a different method
    bool MatchesFilter(const std::string& text) const; // Case-insensitive filter check
};

} // namespace S3MM
