#include "profiler_panel.h"
#include "themes.h"
#include "../core/mono_profiler.h"
#include "../core/mono_bridge.h"
#include "../core/mono_class.h"
#include "../util/logger.h"
#include "imgui.h"

// Prevent Windows.h min/max macros from conflicting with std::min/std::max
#undef min
#undef max

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace S3MM {

// Shared helper: color-coded percentage text (>=10% red, >=1% yellow, else default)
void ProfilerPanel::RenderPctColored(double pct) {
    if (pct >= 10.0) {
        ImGui::TextColored(Colors::Get().negative, "%.1f%%", pct);
    } else if (pct >= 1.0) {
        ImGui::TextColored(Colors::Get().neutral, "%.1f%%", pct);
    } else {
        ImGui::Text("%.1f%%", pct);
    }
}

// Shared helper: render sorted call stack tree nodes
void ProfilerPanel::RenderCallStackTree(const std::unordered_map<uint64_t, std::pair<std::vector<void*>, uint64_t>>& stacks) {
    if (stacks.empty()) {
        ImGui::TextDisabled("No call stacks captured");
        return;
    }

    std::vector<std::pair<uint64_t, const std::pair<std::vector<void*>, uint64_t>*>> sorted;
    for (const auto& [hash, stackData] : stacks) { sorted.push_back({hash, &stackData}); }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second->second > b.second->second; });

    ImGui::Text("%zu unique call stacks", sorted.size());
    ImGui::Separator();

    for (const auto& [hash, stackDataPtr] : sorted) {
        const auto& [stack, count] = *stackDataPtr;

        std::string stackStr;
        for (size_t i = 0; i < stack.size(); i++) {
            if (i > 0) stackStr += "\n    -> ";
            stackStr += MonoBridge::GetMethodFullName(stack[i]);
        }
        if (stackStr.empty()) { stackStr = "<no managed stack>"; }

        char header[64];
        snprintf(header, sizeof(header), "%llu entries##%llx", count, hash);
        if (ImGui::TreeNode(header)) {
            ImGui::TextWrapped("%s", stackStr.c_str());
            ImGui::TreePop();
        }
    }
}

void ProfilerPanel::Render() {
    auto& profiler = MonoProfiler::Get();

    // Refresh cached data periodically
    m_refreshTimer += ImGui::GetIO().DeltaTime;
    if (m_refreshTimer >= m_refreshInterval) {
        m_refreshTimer = 0.0f;
        m_cachedMethodStats = profiler.GetMethodStats();
        m_cachedAllocStats = profiler.GetAllocationStats();
        m_cachedBoxingStats = profiler.GetBoxingStats();
        m_cachedICallStats = profiler.GetICallStats();
        if (profiler.IsSampling() || m_cachedTotalSamples > 0) {
            m_cachedSampleCounts = profiler.GetSampleCounts();
            m_cachedTotalSamples = profiler.GetTotalSamples();
        }
        if (profiler.GetProbeCount() > 0 || !m_cachedProbes.empty()) { m_cachedProbes = profiler.GetProbeSnapshot(); }
    }

    RenderControls();

    ImGui::Separator();

    // Tab bar for Methods vs Allocations
    if (ImGui::BeginTabBar("ProfilerTabs")) {
        if (ImGui::BeginTabItem("Methods")) {
            RenderMethodsTable();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Allocations")) {
            RenderAllocationsTable();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Boxing")) {
            RenderBoxingTable();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("ICalls")) {
            RenderICallsTable();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Probes")) {
            RenderProbesTable();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void ProfilerPanel::RenderControls() {
    auto& profiler = MonoProfiler::Get();

    bool isRunning = profiler.IsRunning();

    // Start/Stop button
    if (isRunning) {
        if (ImGui::Button("Stop")) { profiler.Stop(); }
    } else {
        if (ImGui::Button("Start")) { profiler.Start(); }
    }

    ImGui::SameLine();

    if (ImGui::Button("Export CSV")) { ExportToCSV(); }

    ImGui::SameLine();

    // Trace recording controls
    if (profiler.IsTraceRecording()) {
        if (ImGui::Button("Stop Trace")) { profiler.StopTraceRecording(); }
    } else {
        if (ImGui::Button("Record Trace")) { profiler.StartTraceRecording(1000000); }
    }

    ImGui::SameLine();

    if (ImGui::Button("Export Trace")) { ExportChromeTrace(); }

    ImGui::SameLine();

    // Dispatch sampling controls (hooks SPEC_interp_exec_dispatch)
    if (profiler.IsSampling()) {
        if (ImGui::Button("Stop Sample")) { profiler.StopSampling(); }
    } else {
        if (ImGui::Button("Sample")) { profiler.StartSampling(); }
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Hook-based dispatch sampling: counts internal MINT_CALL dispatches"); }

    ImGui::SameLine();

    if (ImGui::Button("Reset")) {
        profiler.Reset();
        m_cachedMethodStats.clear();
        m_cachedAllocStats.clear();
        m_cachedBoxingStats.clear();
        m_cachedICallStats.clear();
        m_cachedSampleCounts.clear();
        m_cachedTotalSamples = 0;
    }

    ImGui::SameLine();

    // Status indicator
    if (isRunning) {
        Colors::Text(Colors::Get().positive, "PROFILING");
    } else {
        Colors::Text(Colors::Get().neutral, "STOPPED");
    }

    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();

    // Stats summary
    double elapsed = profiler.GetElapsedSeconds();
    uint64_t totalCalls = profiler.GetTotalCalls();
    uint64_t totalAllocs = profiler.GetTotalAllocations();
    double callsPerSec = (elapsed > 0.0) ? (double)totalCalls / elapsed : 0.0;
    double allocsPerSec = (elapsed > 0.0) ? (double)totalAllocs / elapsed : 0.0;

    ImGui::Text("Calls: %llu (%.0f/s)", totalCalls, callsPerSec);
    ImGui::SameLine();
    ImGui::Text("| Allocs: %llu (%.0f/s)", totalAllocs, allocsPerSec);
    ImGui::SameLine();
    ImGui::Text("| ICalls: %llu", profiler.GetTotalICalls());
    ImGui::SameLine();
    ImGui::Text("| Methods: %zu", m_cachedMethodStats.size());
    ImGui::SameLine();
    ImGui::Text("| Types: %zu", m_cachedAllocStats.size());
    if (m_cachedTotalSamples > 0) {
        ImGui::SameLine();
        ImGui::Text("| Dispatches: %llu", m_cachedTotalSamples);
    }

    // Filter
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##filter", "Filter...", m_filterText, sizeof(m_filterText));

    ImGui::SameLine();

    // Refresh rate slider
    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat("Refresh", &m_refreshInterval, 0.1f, 2.0f, "%.1fs");

    ImGui::SameLine();

    // Max rows
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("Max", &m_maxRows);
    if (m_maxRows < 10) m_maxRows = 10;
    if (m_maxRows > 10000) m_maxRows = 10000;
}

void ProfilerPanel::RenderMethodsTable() {
    // Compute total ticks for % Total column
    uint64_t grandTotalTicks = 0;
    for (const auto& stats : m_cachedMethodStats) { grandTotalTicks += stats.totalTicks; }

    // Filter and sort the data
    std::vector<const MethodStats*> filtered;
    filtered.reserve(m_cachedMethodStats.size());

    for (const auto& stats : m_cachedMethodStats) {
        if (!MatchesFilter(stats.fullName)) continue;
        filtered.push_back(&stats);
    }

    // Sort
    auto& sampleCountsRef = m_cachedSampleCounts;
    double elapsed = MonoProfiler::Get().GetElapsedSeconds();
    bool isRunning = MonoProfiler::Get().IsRunning();
    std::sort(filtered.begin(), filtered.end(), [this, grandTotalTicks, &sampleCountsRef, elapsed](const MethodStats* a, const MethodStats* b) {
        int cmp = 0;
        switch (m_sortColumn) {
        case SortColumn::Name:
            cmp = a->fullName.compare(b->fullName);
            break;
        case SortColumn::Calls:
            cmp = (a->callCount > b->callCount) ? 1 : (a->callCount < b->callCount) ? -1 : 0;
            break;
        case SortColumn::CallsPerSec: {
            double cpsA = elapsed > 0.0 ? (double)a->callCount / elapsed : 0.0;
            double cpsB = elapsed > 0.0 ? (double)b->callCount / elapsed : 0.0;
            cmp = (cpsA > cpsB) ? 1 : (cpsA < cpsB) ? -1 : 0;
        } break;
        case SortColumn::TotalTime:
        case SortColumn::PctTotal:
            cmp = (a->totalTicks > b->totalTicks) ? 1 : (a->totalTicks < b->totalTicks) ? -1 : 0;
            break;
        case SortColumn::SelfTime:
            cmp = (a->selfTicks > b->selfTicks) ? 1 : (a->selfTicks < b->selfTicks) ? -1 : 0;
            break;
        case SortColumn::SelfAvgTime: {
            double avgA = a->callCount > 0 ? (double)a->selfTicks / a->callCount : 0;
            double avgB = b->callCount > 0 ? (double)b->selfTicks / b->callCount : 0;
            cmp = (avgA > avgB) ? 1 : (avgA < avgB) ? -1 : 0;
        } break;
        case SortColumn::Samples: {
            auto itA = sampleCountsRef.find(a->method);
            auto itB = sampleCountsRef.find(b->method);
            uint64_t sA = (itA != sampleCountsRef.end()) ? itA->second : 0;
            uint64_t sB = (itB != sampleCountsRef.end()) ? itB->second : 0;
            cmp = (sA > sB) ? 1 : (sA < sB) ? -1 : 0;
        } break;
        }
        return m_sortDescending ? (cmp > 0) : (cmp < 0);
    });

    // Limit rows
    size_t displayCount = std::min(filtered.size(), (size_t)m_maxRows);

    // Table has 8 columns: Method, Calls, Calls/s, Total, % Total, Self, Self Avg, Samples
    ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV;

    if (ImGui::BeginTable("MethodsTable", 8, tableFlags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Method", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Calls/s", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Total (ms)", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("% Total", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Self (ms)", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Self Avg (us)", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Samples", ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableHeadersRow();

        // Handle sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
            if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                m_sortColumn = static_cast<SortColumn>(spec.ColumnIndex);
                m_sortDescending = (spec.SortDirection == ImGuiSortDirection_Descending);
                sortSpecs->SpecsDirty = false;
            }
        }

        // Rows
        ImGuiListClipper clipper;
        clipper.Begin((int)displayCount);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const MethodStats* stats = filtered[row];

                ImGui::TableNextRow();

                // Method name
                ImGui::TableNextColumn();
                ImGui::PushID(stats->method);
                ImGui::Selectable(stats->fullName.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("View Details")) {
                        m_methodWindowName = stats->fullName;
                        m_methodWindowMethod = stats->method;
                        m_showMethodWindow = true;
                    }
                    if (ImGui::MenuItem("Filter to This")) { SetFilter(stats->fullName); }
                    ImGui::Separator();
                    bool isProbed = MonoProfiler::Get().IsMethodProbed(stats->method);
                    if (!isProbed) {
                        if (ImGui::MenuItem("Attach Probe")) { MonoProfiler::Get().AttachProbe(stats->method); }
                    } else {
                        if (ImGui::MenuItem("Detach Probe")) { MonoProfiler::Get().DetachProbe(stats->method); }
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();

                // Calls
                ImGui::TableNextColumn();
                ImGui::Text("%llu", stats->callCount);

                // Calls/s
                ImGui::TableNextColumn();
                if (isRunning) {
                    double cps = elapsed > 0.0 ? (double)stats->callCount / elapsed : 0.0;
                    ImGui::Text("%.0f", cps);
                } else {
                    ImGui::TextDisabled("-");
                }

                // Total (ms)
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", MonoProfiler::TicksToMs(stats->totalTicks));

                // % Total
                ImGui::TableNextColumn();
                if (grandTotalTicks > 0) {
                    RenderPctColored((double)stats->totalTicks / (double)grandTotalTicks * 100.0);
                } else {
                    ImGui::TextDisabled("-");
                }

                // Self (ms)
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", MonoProfiler::TicksToMs(stats->selfTicks));

                // Self Avg (us)
                ImGui::TableNextColumn();
                {
                    double selfAvg = stats->callCount > 0 ? MonoProfiler::TicksToUs(stats->selfTicks) / stats->callCount : 0;
                    ImGui::Text("%.1f", selfAvg);
                }

                // Samples
                ImGui::TableNextColumn();
                {
                    auto it = m_cachedSampleCounts.find(stats->method);
                    uint64_t smpCount = (it != m_cachedSampleCounts.end()) ? it->second : 0;
                    if (smpCount > 0) {
                        ImGui::Text("%llu", smpCount);
                    } else {
                        ImGui::TextDisabled("-");
                    }
                }
            }
        }

        ImGui::EndTable();
    }

    if (filtered.size() > (size_t)m_maxRows) { ImGui::TextDisabled("Showing %d of %zu methods", m_maxRows, filtered.size()); }

    // Render method detail window if open
    RenderMethodDetailWindow();
}

void ProfilerPanel::RenderMethodDetailWindow() {
    if (!m_showMethodWindow) return;

    // Live lookup from cached stats (auto-refreshed on timer)
    const MethodStats* statsPtr = nullptr;
    for (const auto& s : m_cachedMethodStats) {
        if (s.method == m_methodWindowMethod) {
            statsPtr = &s;
            break;
        }
    }
    MethodStats emptyStats{};
    emptyStats.method = m_methodWindowMethod;
    emptyStats.fullName = m_methodWindowName;
    const auto& stats = statsPtr ? *statsPtr : emptyStats;

    std::string windowTitle = "Method Details: " + m_methodWindowName;
    ImGui::SetNextWindowSize(ImVec2(750, 550), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(windowTitle.c_str(), &m_showMethodWindow)) {
        // Dump to file button
        if (ImGui::Button("Dump to File")) {
            std::string safeName = m_methodWindowName;
            for (char& c : safeName) {
                if (c == '.' || c == ':' || c == '<' || c == '>' || c == ' ' || c == '/') c = '_';
            }
            std::string filename = "method_" + safeName + ".txt";

            FILE* f = fopen(filename.c_str(), "w");
            if (f) {
                fprintf(f, "Method Details: %s\n", m_methodWindowName.c_str());
                fprintf(f, "================================================\n\n");
                fprintf(f, "Calls: %llu\n", stats.callCount);
                fprintf(f, "Total Time: %.3f ms\n", MonoProfiler::TicksToMs(stats.totalTicks));
                fprintf(f, "Self Time:  %.3f ms\n", MonoProfiler::TicksToMs(stats.selfTicks));
                fprintf(f, "Avg Time:   %.1f us\n", stats.callCount > 0 ? MonoProfiler::TicksToUs(stats.totalTicks) / stats.callCount : 0.0);
                fprintf(f, "Self Avg:   %.1f us\n", stats.callCount > 0 ? MonoProfiler::TicksToUs(stats.selfTicks) / stats.callCount : 0.0);
                fprintf(f, "Min Time:   %.1f us\n", stats.minTicks != UINT64_MAX ? MonoProfiler::TicksToUs(stats.minTicks) : 0.0);
                fprintf(f, "Max Time:   %.1f us\n", MonoProfiler::TicksToUs(stats.maxTicks));

                fprintf(f, "\nCALLERS (%zu):\n", stats.callers.size());
                fprintf(f, "------------------------\n");

                // Sort callers by call count
                std::vector<std::pair<void*, std::pair<uint64_t, uint64_t>>> sortedCallers(stats.callers.begin(), stats.callers.end());
                std::sort(sortedCallers.begin(), sortedCallers.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });

                for (const auto& [method, data] : sortedCallers) {
                    std::string callerName = MonoBridge::GetMethodFullName(method);
                    fprintf(f, "  %s  (%llu calls, %.2f ms)\n", callerName.c_str(), data.first, MonoProfiler::TicksToMs(data.second));
                }

                fprintf(f, "\nCALLEES (%zu):\n", stats.callees.size());
                fprintf(f, "------------------------\n");

                std::vector<std::pair<void*, std::pair<uint64_t, uint64_t>>> sortedCallees(stats.callees.begin(), stats.callees.end());
                std::sort(sortedCallees.begin(), sortedCallees.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });

                for (const auto& [method, data] : sortedCallees) {
                    std::string calleeName = MonoBridge::GetMethodFullName(method);
                    fprintf(f, "  %s  (%llu calls, %.2f ms)\n", calleeName.c_str(), data.first, MonoProfiler::TicksToMs(data.second));
                }

                // Dump sample stacks that include this method
                auto allSampleStacks = MonoProfiler::Get().GetSampleStacks();
                std::vector<const SampleStack*> relevantStacks;
                for (const auto& ss : allSampleStacks) {
                    for (void* m : ss.frames) {
                        if (m == m_methodWindowMethod) {
                            relevantStacks.push_back(&ss);
                            break;
                        }
                    }
                }
                if (!relevantStacks.empty()) {
                    std::sort(relevantStacks.begin(), relevantStacks.end(), [](const SampleStack* a, const SampleStack* b) { return a->count > b->count; });
                    fprintf(f, "\nSAMPLE STACKS (%zu containing this method):\n", relevantStacks.size());
                    fprintf(f, "------------------------\n");
                    for (const auto* ss : relevantStacks) {
                        fprintf(f, "\n  [%llu samples]\n", ss->count);
                        for (size_t i = 0; i < ss->frames.size(); i++) {
                            std::string mName = MonoBridge::GetMethodFullName(ss->frames[i]);
                            const char* marker = (ss->frames[i] == m_methodWindowMethod) ? " <---" : "";
                            fprintf(f, "    %s%s%s\n", (i > 0 ? "-> " : ""), mName.c_str(), marker);
                        }
                    }
                }

                fclose(f);
                LOG_INFO("[Profiler] Dumped method details to: " + filename);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(saves to working directory)");

        ImGui::Separator();

        // Summary stats
        ImGui::Text("Calls: %llu", stats.callCount);
        ImGui::SameLine();
        ImGui::Text("| Total: %.3f ms", MonoProfiler::TicksToMs(stats.totalTicks));
        ImGui::SameLine();
        ImGui::Text("| Self: %.3f ms", MonoProfiler::TicksToMs(stats.selfTicks));

        double avg = stats.callCount > 0 ? MonoProfiler::TicksToUs(stats.totalTicks) / stats.callCount : 0;
        double selfAvg = stats.callCount > 0 ? MonoProfiler::TicksToUs(stats.selfTicks) / stats.callCount : 0;
        ImGui::Text("Avg: %.1f us", avg);
        ImGui::SameLine();
        ImGui::Text("| Self Avg: %.1f us", selfAvg);
        ImGui::SameLine();
        ImGui::Text("| Min: %.1f us", stats.minTicks != UINT64_MAX ? MonoProfiler::TicksToUs(stats.minTicks) : 0.0);
        ImGui::SameLine();
        ImGui::Text("| Max: %.1f us", MonoProfiler::TicksToUs(stats.maxTicks));

        ImGui::Separator();

        if (ImGui::BeginTabBar("MethodDetailTabs")) {
            // Callers tab
            if (ImGui::BeginTabItem("Callers")) {
                if (stats.callers.empty()) {
                    ImGui::TextDisabled("No callers recorded (this may be a top-level entry point)");
                } else {
                    ImGui::Text("%zu unique callers", stats.callers.size());
                    ImGui::Separator();

                    // Sort callers by call count
                    std::vector<std::pair<void*, std::pair<uint64_t, uint64_t>>> sortedCallers(stats.callers.begin(), stats.callers.end());
                    std::sort(sortedCallers.begin(), sortedCallers.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });

                    ImGuiTableFlags tblFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;

                    if (ImGui::BeginTable("CallersTable", 4, tblFlags, ImVec2(0, 0))) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Caller", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 80);
                        ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, 90);
                        ImGui::TableSetupColumn("% of Total", ImGuiTableColumnFlags_WidthFixed, 75);
                        ImGui::TableHeadersRow();

                        for (const auto& [method, data] : sortedCallers) {
                            ImGui::TableNextRow();

                            ImGui::TableNextColumn();
                            std::string callerName = MonoBridge::GetMethodFullName(method);
                            ImGui::PushID(method);
                            if (ImGui::Selectable(callerName.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) { NavigateToMethod(method); }
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Click to view details"); }
                            ImGui::PopID();

                            ImGui::TableNextColumn();
                            ImGui::Text("%llu", data.first);

                            ImGui::TableNextColumn();
                            ImGui::Text("%.2f", MonoProfiler::TicksToMs(data.second));

                            ImGui::TableNextColumn();
                            if (stats.totalTicks > 0) {
                                double pct = (double)data.second / (double)stats.totalTicks * 100.0;
                                ImGui::Text("%.1f%%", pct);
                            } else {
                                ImGui::TextDisabled("-");
                            }
                        }

                        ImGui::EndTable();
                    }
                }
                ImGui::EndTabItem();
            }

            // Callees tab
            if (ImGui::BeginTabItem("Callees")) {
                if (stats.callees.empty()) {
                    ImGui::TextDisabled("No callees recorded (this method doesn't call other managed methods)");
                } else {
                    ImGui::Text("%zu unique callees", stats.callees.size());
                    ImGui::Separator();

                    // Sort callees by time (descending)
                    std::vector<std::pair<void*, std::pair<uint64_t, uint64_t>>> sortedCallees(stats.callees.begin(), stats.callees.end());
                    std::sort(sortedCallees.begin(), sortedCallees.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });

                    ImGuiTableFlags tblFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;

                    if (ImGui::BeginTable("CalleesTable", 4, tblFlags, ImVec2(0, 0))) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Callee", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 80);
                        ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, 90);
                        ImGui::TableSetupColumn("% of Total", ImGuiTableColumnFlags_WidthFixed, 75);
                        ImGui::TableHeadersRow();

                        for (const auto& [method, data] : sortedCallees) {
                            ImGui::TableNextRow();

                            ImGui::TableNextColumn();
                            std::string calleeName = MonoBridge::GetMethodFullName(method);
                            ImGui::PushID(method);
                            if (ImGui::Selectable(calleeName.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) { NavigateToMethod(method); }
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Click to view details"); }
                            ImGui::PopID();

                            ImGui::TableNextColumn();
                            ImGui::Text("%llu", data.first);

                            ImGui::TableNextColumn();
                            ImGui::Text("%.2f", MonoProfiler::TicksToMs(data.second));

                            ImGui::TableNextColumn();
                            if (stats.totalTicks > 0) {
                                double pct = (double)data.second / (double)stats.totalTicks * 100.0;
                                ImGui::Text("%.1f%%", pct);
                            } else {
                                ImGui::TextDisabled("-");
                            }
                        }

                        ImGui::EndTable();
                    }
                }
                ImGui::EndTabItem();
            }

            // ICalls tab (native calls made by this method)
            if (ImGui::BeginTabItem("ICalls")) {
                if (stats.icallTargets.empty()) {
                    ImGui::TextDisabled("This method does not make any native internal calls");
                } else {
                    ImGui::Text("%zu native calls", stats.icallTargets.size());
                    ImGui::Separator();

                    for (void* addr : stats.icallTargets) {
                        std::string icallName = "<unknown>";
                        uint64_t icallCallCount = 0;
                        for (const auto& icall : m_cachedICallStats) {
                            if (icall.nativeAddr == addr) {
                                icallName = icall.name;
                                icallCallCount = icall.callCount;
                                break;
                            }
                        }
                        ImGui::BulletText("0x%08X  %s  (%llu lookups)", (uintptr_t)addr, icallName.c_str(), icallCallCount);
                    }
                }
                ImGui::EndTabItem();
            }

            // Samples tab (sampling profiler stacks containing this method)
            if (ImGui::BeginTabItem("Samples")) {
                auto allSampleStacks = MonoProfiler::Get().GetSampleStacks();

                // Filter to stacks that include this method
                std::vector<const SampleStack*> relevantStacks;
                for (const auto& ss : allSampleStacks) {
                    for (void* m : ss.frames) {
                        if (m == m_methodWindowMethod) {
                            relevantStacks.push_back(&ss);
                            break;
                        }
                    }
                }

                if (relevantStacks.empty()) {
                    ImGui::TextDisabled("No samples contain this method (start sampling to collect data)");
                } else {
                    // Sort by count descending
                    std::sort(relevantStacks.begin(), relevantStacks.end(), [](const SampleStack* a, const SampleStack* b) { return a->count > b->count; });

                    // Show sample count for this method
                    auto it = m_cachedSampleCounts.find(m_methodWindowMethod);
                    uint64_t methodSamples = (it != m_cachedSampleCounts.end()) ? it->second : 0;
                    ImGui::Text("Method hit in %llu samples, %zu unique stacks", methodSamples, relevantStacks.size());
                    if (m_cachedTotalSamples > 0) {
                        ImGui::SameLine();
                        double pct = (double)methodSamples / (double)m_cachedTotalSamples * 100.0;
                        ImGui::Text("(%.1f%% of total)", pct);
                    }
                    ImGui::Separator();

                    for (const auto* ss : relevantStacks) {
                        // Build call stack string
                        std::string stackStr;
                        for (size_t i = 0; i < ss->frames.size(); i++) {
                            if (i > 0) stackStr += "\n    -> ";
                            std::string mName = MonoBridge::GetMethodFullName(ss->frames[i]);
                            if (ss->frames[i] == m_methodWindowMethod) { mName += "  <---"; }
                            stackStr += mName;
                        }

                        if (stackStr.empty()) { stackStr = "<no stack>"; }

                        // Use pointer-based ID to avoid collisions
                        char header[64];
                        snprintf(header, sizeof(header), "%llu samples##smp%p", ss->count, (const void*)ss);
                        if (ImGui::TreeNode(header)) {
                            ImGui::TextWrapped("%s", stackStr.c_str());
                            ImGui::TreePop();
                        }
                    }
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void ProfilerPanel::RenderAllocationsTable() {
    // Filter and sort
    std::vector<const AllocationStats*> filtered;
    filtered.reserve(m_cachedAllocStats.size());

    for (const auto& stats : m_cachedAllocStats) {
        if (!MatchesFilter(stats.typeName)) continue;
        filtered.push_back(&stats);
    }

    // Sort
    double elapsed = MonoProfiler::Get().GetElapsedSeconds();
    bool isRunning = MonoProfiler::Get().IsRunning();
    std::sort(filtered.begin(), filtered.end(), [this, elapsed](const AllocationStats* a, const AllocationStats* b) {
        int cmp = 0;
        switch (m_allocSortColumn) {
        case AllocSortColumn::TypeName:
            cmp = a->typeName.compare(b->typeName);
            break;
        case AllocSortColumn::Count:
            cmp = (a->allocCount > b->allocCount) ? 1 : (a->allocCount < b->allocCount) ? -1 : 0;
            break;
        case AllocSortColumn::AllocsPerSec: {
            double apsA = elapsed > 0.0 ? (double)a->allocCount / elapsed : 0.0;
            double apsB = elapsed > 0.0 ? (double)b->allocCount / elapsed : 0.0;
            cmp = (apsA > apsB) ? 1 : (apsA < apsB) ? -1 : 0;
        } break;
        case AllocSortColumn::Size:
            cmp = (a->instanceSize > b->instanceSize) ? 1 : (a->instanceSize < b->instanceSize) ? -1 : 0;
            break;
        case AllocSortColumn::TotalBytes:
            cmp = (a->totalBytes > b->totalBytes) ? 1 : (a->totalBytes < b->totalBytes) ? -1 : 0;
            break;
        }
        return m_allocSortDescending ? (cmp > 0) : (cmp < 0);
    });

    size_t displayCount = std::min(filtered.size(), (size_t)m_maxRows);

    ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV;

    if (ImGui::BeginTable("AllocationsTable", 6, tableFlags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Allocations", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Allocs/s", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Sites", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();

        // Handle sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
            if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                m_allocSortColumn = static_cast<AllocSortColumn>(spec.ColumnIndex);
                m_allocSortDescending = (spec.SortDirection == ImGuiSortDirection_Descending);
                sortSpecs->SpecsDirty = false;
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)displayCount);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const AllocationStats* stats = filtered[row];

                ImGui::TableNextRow();

                // Type name (selectable for right-click)
                ImGui::TableNextColumn();
                ImGui::PushID(stats->klass);
                ImGui::Selectable(stats->typeName.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("View Call Stacks")) {
                        m_allocWindowTypeName = stats->typeName;
                        m_allocWindowCallStacks = stats->callStacks;
                        m_showAllocWindow = true;
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();

                // Allocation count
                ImGui::TableNextColumn();
                ImGui::Text("%llu", stats->allocCount);

                // Allocs/s
                ImGui::TableNextColumn();
                if (isRunning) {
                    double aps = elapsed > 0.0 ? (double)stats->allocCount / elapsed : 0.0;
                    ImGui::Text("%.0f", aps);
                } else {
                    ImGui::TextDisabled("-");
                }

                // Instance size
                ImGui::TableNextColumn();
                ImGui::Text("%u", stats->instanceSize);

                // Total bytes (formatted as KB/MB if large)
                ImGui::TableNextColumn();
                if (stats->totalBytes >= 1024 * 1024) {
                    ImGui::Text("%.1f MB", (double)stats->totalBytes / (1024.0 * 1024.0));
                } else if (stats->totalBytes >= 1024) {
                    ImGui::Text("%.1f KB", (double)stats->totalBytes / 1024.0);
                } else {
                    ImGui::Text("%llu B", stats->totalBytes);
                }

                // Number of unique allocation sites
                ImGui::TableNextColumn();
                ImGui::Text("%zu", stats->callStacks.size());
            }
        }

        ImGui::EndTable();
    }

    if (filtered.size() > (size_t)m_maxRows) { ImGui::TextDisabled("Showing %d of %zu types", m_maxRows, filtered.size()); }

    // Allocation details window
    if (m_showAllocWindow) {
        std::string windowTitle = "Allocation Call Stacks: " + m_allocWindowTypeName;
        ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);

        if (ImGui::Begin(windowTitle.c_str(), &m_showAllocWindow)) {
            // Dump to file button
            if (ImGui::Button("Dump to File")) {
                std::string safeTypeName = m_allocWindowTypeName;
                for (char& c : safeTypeName) {
                    if (c == '.' || c == ':' || c == '<' || c == '>' || c == ' ') c = '_';
                }
                std::string filename = "alloc_" + safeTypeName + ".txt";

                FILE* f = fopen(filename.c_str(), "w");
                if (f) {
                    fprintf(f, "Allocation Call Stacks: %s\n", m_allocWindowTypeName.c_str());
                    fprintf(f, "================================================\n\n");

                    fprintf(f, "CALL STACKS (%zu unique):\n", m_allocWindowCallStacks.size());
                    fprintf(f, "------------------------\n");

                    std::vector<std::pair<uint64_t, const std::pair<std::vector<void*>, uint64_t>*>> sortedStacks;
                    for (const auto& [hash, stackData] : m_allocWindowCallStacks) { sortedStacks.push_back({hash, &stackData}); }
                    std::sort(sortedStacks.begin(), sortedStacks.end(), [](const auto& a, const auto& b) { return a.second->second > b.second->second; });

                    for (const auto& [hash, stackDataPtr] : sortedStacks) {
                        const auto& [stack, count] = *stackDataPtr;
                        fprintf(f, "\n  [%llu allocations]\n", count);
                        for (size_t i = 0; i < stack.size(); i++) {
                            std::string methodName = MonoBridge::GetMethodFullName(stack[i]);
                            fprintf(f, "    %s%s\n", (i > 0 ? "-> " : ""), methodName.c_str());
                        }
                    }

                    fclose(f);
                    LOG_INFO("[Profiler] Dumped allocation stacks to: " + filename);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(saves to working directory)");

            ImGui::Separator();

            RenderCallStackTree(m_allocWindowCallStacks);
        }
        ImGui::End();
    }
}

void ProfilerPanel::RenderBoxingTable() {
    double elapsed = MonoProfiler::Get().GetElapsedSeconds();
    bool isRunning = MonoProfiler::Get().IsRunning();

    // Summary stats
    uint64_t totalBoxing = MonoProfiler::Get().GetTotalBoxing();
    ImGui::Text("Total Boxing: %llu", totalBoxing);
    ImGui::SameLine();
    if (isRunning) {
        double bps = elapsed > 0.0 ? (double)totalBoxing / elapsed : 0.0;
        ImGui::Text("(%.0f/s)", bps);
    }
    ImGui::SameLine();
    ImGui::Text("| Unique Types: %zu", m_cachedBoxingStats.size());

    // Filter and sort
    std::vector<const BoxingStats*> filtered;
    filtered.reserve(m_cachedBoxingStats.size());

    for (const auto& stats : m_cachedBoxingStats) {
        if (!MatchesFilter(stats.typeName)) continue;
        filtered.push_back(&stats);
    }
    std::sort(filtered.begin(), filtered.end(), [this, elapsed](const BoxingStats* a, const BoxingStats* b) {
        int cmp = 0;
        switch (m_boxingSortColumn) {
        case BoxingSortColumn::TypeName:
            cmp = a->typeName.compare(b->typeName);
            break;
        case BoxingSortColumn::BoxCount:
            cmp = (a->boxCount > b->boxCount) ? 1 : (a->boxCount < b->boxCount) ? -1 : 0;
            break;
        case BoxingSortColumn::BoxPerSec: {
            double bpsA = elapsed > 0.0 ? (double)a->boxCount / elapsed : 0.0;
            double bpsB = elapsed > 0.0 ? (double)b->boxCount / elapsed : 0.0;
            cmp = (bpsA > bpsB) ? 1 : (bpsA < bpsB) ? -1 : 0;
        } break;
        case BoxingSortColumn::Size:
            cmp = (a->instanceSize > b->instanceSize) ? 1 : (a->instanceSize < b->instanceSize) ? -1 : 0;
            break;
        case BoxingSortColumn::CallSites:
            cmp = (a->callStacks.size() > b->callStacks.size()) ? 1 : (a->callStacks.size() < b->callStacks.size()) ? -1 : 0;
            break;
        }
        return m_boxingSortDescending ? (cmp > 0) : (cmp < 0);
    });

    ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable;

    float tableHeight = ImGui::GetContentRegionAvail().y;

    if (ImGui::BeginTable("BoxingTable", 5, tableFlags, ImVec2(0, tableHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Box Count", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 90);
        ImGui::TableSetupColumn("Box/s", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Call Sites", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();

        // Handle sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
            if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                m_boxingSortColumn = static_cast<BoxingSortColumn>(spec.ColumnIndex);
                m_boxingSortDescending = (spec.SortDirection == ImGuiSortDirection_Descending);
                sortSpecs->SpecsDirty = false;
            }
        }

        size_t displayCount = std::min(filtered.size(), (size_t)m_maxRows);
        ImGuiListClipper clipper;
        clipper.Begin((int)displayCount);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const BoxingStats* stats = filtered[row];

                ImGui::TableNextRow();

                // Type name
                ImGui::TableNextColumn();
                ImGui::PushID(stats->klass);
                ImGui::Selectable(stats->typeName.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("View Details")) {
                        m_boxingWindowTypeName = stats->typeName;
                        m_boxingWindowKlass = stats->klass;
                        m_boxingWindowCallStacks = stats->callStacks;
                        m_boxingWindowValues = stats->boxedValues;
                        m_showBoxingWindow = true;
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();

                // Box count
                ImGui::TableNextColumn();
                ImGui::Text("%llu", stats->boxCount);

                // Box/s
                ImGui::TableNextColumn();
                if (isRunning) {
                    double bps = elapsed > 0.0 ? (double)stats->boxCount / elapsed : 0.0;
                    ImGui::Text("%.0f", bps);
                } else {
                    ImGui::TextDisabled("-");
                }

                // Instance size
                ImGui::TableNextColumn();
                ImGui::Text("%u", stats->instanceSize);

                // Call sites
                ImGui::TableNextColumn();
                ImGui::Text("%zu", stats->callStacks.size());
            }
        }

        ImGui::EndTable();
    }

    if (filtered.size() > (size_t)m_maxRows) { ImGui::TextDisabled("Showing %d of %zu types", m_maxRows, filtered.size()); }

    // Boxing details window
    if (m_showBoxingWindow) {
        std::string windowTitle = "Boxing Details: " + m_boxingWindowTypeName;
        ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);

        if (ImGui::Begin(windowTitle.c_str(), &m_showBoxingWindow)) {
            // Resolve enum names once for both dump and display
            std::unordered_map<int64_t, std::string> enumNames;
            if (m_boxingWindowKlass) {
                MonoClass mc(m_boxingWindowKlass);
                if (mc.IsEnum()) {
                    auto enumValues = mc.GetEnumValues();
                    for (const auto& ev : enumValues) { enumNames[(int64_t)ev.value] = ev.name; }
                }
            }

            // Sort values once for both dump and display
            std::vector<std::pair<int64_t, uint64_t>> sortedValues(m_boxingWindowValues.begin(), m_boxingWindowValues.end());
            std::sort(sortedValues.begin(), sortedValues.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

            // Dump to file button
            if (ImGui::Button("Dump to File")) {
                std::string safeTypeName = m_boxingWindowTypeName;
                for (char& c : safeTypeName) {
                    if (c == '.' || c == ':' || c == '<' || c == '>' || c == ' ') c = '_';
                }
                std::string filename = "boxing_" + safeTypeName + ".txt";

                FILE* f = fopen(filename.c_str(), "w");
                if (f) {
                    fprintf(f, "Boxing Details: %s\n", m_boxingWindowTypeName.c_str());
                    fprintf(f, "================================================\n\n");

                    fprintf(f, "BOXED VALUES (%zu unique):\n", m_boxingWindowValues.size());
                    fprintf(f, "------------------------\n");
                    for (const auto& [value, count] : sortedValues) {
                        auto it = enumNames.find(value);
                        if (it != enumNames.end()) {
                            fprintf(f, "  %s (%lld / 0x%llX): %llu times\n", it->second.c_str(), value, value, count);
                        } else {
                            fprintf(f, "  %lld (0x%llX): %llu times\n", value, value, count);
                        }
                    }

                    fprintf(f, "\nCALL STACKS (%zu unique):\n", m_boxingWindowCallStacks.size());
                    fprintf(f, "------------------------\n");
                    std::vector<std::pair<uint64_t, const std::pair<std::vector<void*>, uint64_t>*>> dumpStacks;
                    for (const auto& [hash, stackData] : m_boxingWindowCallStacks) { dumpStacks.push_back({hash, &stackData}); }
                    std::sort(dumpStacks.begin(), dumpStacks.end(), [](const auto& a, const auto& b) { return a.second->second > b.second->second; });

                    for (const auto& [hash, stackDataPtr] : dumpStacks) {
                        const auto& [stack, count] = *stackDataPtr;
                        fprintf(f, "\n  [%llu boxes]\n", count);
                        for (size_t i = 0; i < stack.size(); i++) {
                            std::string methodName = MonoBridge::GetMethodFullName(stack[i]);
                            fprintf(f, "    %s%s\n", (i > 0 ? "-> " : ""), methodName.c_str());
                        }
                    }

                    fclose(f);
                    LOG_INFO("[Profiler] Dumped boxing details to: " + filename);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(saves to working directory)");

            ImGui::Separator();

            if (ImGui::BeginTabBar("BoxingDetailsTabs")) {
                // Boxed Values tab
                if (ImGui::BeginTabItem("Boxed Values")) {
                    if (sortedValues.empty()) {
                        ImGui::TextDisabled("No boxed values captured");
                    } else {
                        ImGui::Text("%zu unique values boxed", sortedValues.size());
                        if (!enumNames.empty()) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(enum with %zu known values)", enumNames.size());
                        }
                        ImGui::Separator();

                        ImGuiTableFlags tblFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;

                        int numCols = enumNames.empty() ? 3 : 4;
                        if (ImGui::BeginTable("BoxedValuesTable", numCols, tblFlags, ImVec2(0, 0))) {
                            ImGui::TableSetupScrollFreeze(0, 1);
                            if (!enumNames.empty()) { ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch); }
                            ImGui::TableSetupColumn("Value (Dec)", ImGuiTableColumnFlags_WidthFixed, 100);
                            ImGui::TableSetupColumn("Value (Hex)", ImGuiTableColumnFlags_WidthFixed, 120);
                            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 80);
                            ImGui::TableHeadersRow();

                            for (const auto& [value, count] : sortedValues) {
                                ImGui::TableNextRow();

                                if (!enumNames.empty()) {
                                    ImGui::TableNextColumn();
                                    auto it = enumNames.find(value);
                                    if (it != enumNames.end()) {
                                        ImGui::TextUnformatted(it->second.c_str());
                                    } else {
                                        ImGui::TextDisabled("<unknown>");
                                    }
                                }

                                ImGui::TableNextColumn();
                                ImGui::Text("%lld", value);

                                ImGui::TableNextColumn();
                                ImGui::Text("0x%llX", value);

                                ImGui::TableNextColumn();
                                ImGui::Text("%llu", count);
                            }

                            ImGui::EndTable();
                        }
                    }
                    ImGui::EndTabItem();
                }

                // Call Stacks tab
                if (ImGui::BeginTabItem("Call Stacks")) {
                    RenderCallStackTree(m_boxingWindowCallStacks);
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    }
}

void ProfilerPanel::RenderICallsTable() {
    // Filter and sort
    std::vector<const ICallStats*> filtered;
    filtered.reserve(m_cachedICallStats.size());

    for (const auto& stats : m_cachedICallStats) {
        if (!MatchesFilter(stats.name)) continue;
        filtered.push_back(&stats);
    }

    // Sort
    std::sort(filtered.begin(), filtered.end(), [this](const ICallStats* a, const ICallStats* b) {
        int cmp = 0;
        switch (m_icallSortColumn) {
        case ICallSortColumn::NativeAddr:
            cmp = ((uintptr_t)a->nativeAddr > (uintptr_t)b->nativeAddr) ? 1 : ((uintptr_t)a->nativeAddr < (uintptr_t)b->nativeAddr) ? -1 : 0;
            break;
        case ICallSortColumn::Name:
            cmp = a->name.compare(b->name);
            break;
        case ICallSortColumn::Calls:
            cmp = (a->callCount > b->callCount) ? 1 : (a->callCount < b->callCount) ? -1 : 0;
            break;
        case ICallSortColumn::CalledByCount:
            cmp = (a->calledBy.size() > b->calledBy.size()) ? 1 : (a->calledBy.size() < b->calledBy.size()) ? -1 : 0;
            break;
        }
        return m_icallSortDescending ? (cmp > 0) : (cmp < 0);
    });

    size_t displayCount = std::min(filtered.size(), (size_t)m_maxRows);

    ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV;

    if (ImGui::BeginTable("ICallsTable", 4, tableFlags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Native Addr", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Lookups", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Callers", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();

        // Handle sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
            if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                m_icallSortColumn = static_cast<ICallSortColumn>(spec.ColumnIndex);
                m_icallSortDescending = (spec.SortDirection == ImGuiSortDirection_Descending);
                sortSpecs->SpecsDirty = false;
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)displayCount);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const ICallStats* stats = filtered[row];

                ImGui::TableNextRow();

                // Native address
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", (uintptr_t)stats->nativeAddr);

                // Name
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(stats->name.c_str());

                // Call count
                ImGui::TableNextColumn();
                ImGui::Text("%llu", stats->callCount);

                // Called by count (with tooltip showing callers)
                ImGui::TableNextColumn();
                size_t callerCount = stats->calledBy.size();
                ImGui::Text("%zu", callerCount);
                if (callerCount > 0 && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Called by:");
                    for (void* caller : stats->calledBy) {
                        // Find the caller name from our cached method stats
                        const char* callerName = "<unknown>";
                        for (const auto& method : m_cachedMethodStats) {
                            if (method.method == caller) {
                                callerName = method.fullName.c_str();
                                break;
                            }
                        }
                        ImGui::BulletText("%s", callerName);
                    }
                    ImGui::EndTooltip();
                }
            }
        }

        ImGui::EndTable();
    }

    if (filtered.size() > (size_t)m_maxRows) { ImGui::TextDisabled("Showing %d of %zu icalls", m_maxRows, filtered.size()); }
}

void ProfilerPanel::ExportToCSV() {
    // Generate timestamped filenames
    SYSTEMTIME st;
    GetLocalTime(&st);
    char methodsFilename[256];
    char allocsFilename[256];
    snprintf(methodsFilename, sizeof(methodsFilename), "profiler_methods_%04d%02d%02d_%02d%02d%02d.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    snprintf(allocsFilename, sizeof(allocsFilename), "profiler_allocs_%04d%02d%02d_%02d%02d%02d.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // Compute totals for percentage columns
    uint64_t grandTotalTicks = 0;
    uint64_t grandSelfTicks = 0;
    for (const auto& stats : m_cachedMethodStats) {
        grandTotalTicks += stats.totalTicks;
        grandSelfTicks += stats.selfTicks;
    }

    // Export methods
    FILE* methodsFile = fopen(methodsFilename, "w");
    if (methodsFile) {
        fprintf(methodsFile, "Method,Calls,Calls/s,Total (ms),%% Total,Self (ms),%% Self,Avg (us),Self Avg (us),Min (us),Max (us),ICalls,Callers,Callees,Samples,%% Smp\n");

        for (const auto& stats : m_cachedMethodStats) {
            // Escape commas in method names
            std::string escapedName = stats.fullName;
            bool needsQuotes = escapedName.find(',') != std::string::npos || escapedName.find('"') != std::string::npos;
            if (needsQuotes) {
                // Escape quotes by doubling them
                size_t pos = 0;
                while ((pos = escapedName.find('"', pos)) != std::string::npos) {
                    escapedName.insert(pos, "\"");
                    pos += 2;
                }
                escapedName = "\"" + escapedName + "\"";
            }

            double elapsed = MonoProfiler::Get().GetElapsedSeconds();
            double callsPerSec = elapsed > 0.0 ? (double)stats.callCount / elapsed : 0.0;
            double totalMs = MonoProfiler::TicksToMs(stats.totalTicks);
            double pctTotal = grandTotalTicks > 0 ? (double)stats.totalTicks / (double)grandTotalTicks * 100.0 : 0.0;
            double selfMs = MonoProfiler::TicksToMs(stats.selfTicks);
            double pctSelf = grandSelfTicks > 0 ? (double)stats.selfTicks / (double)grandSelfTicks * 100.0 : 0.0;
            double avgUs = stats.callCount > 0 ? MonoProfiler::TicksToUs(stats.totalTicks) / stats.callCount : 0;
            double selfAvgUs = stats.callCount > 0 ? MonoProfiler::TicksToUs(stats.selfTicks) / stats.callCount : 0;
            double minUs = stats.minTicks != UINT64_MAX ? MonoProfiler::TicksToUs(stats.minTicks) : 0;
            double maxUs = MonoProfiler::TicksToUs(stats.maxTicks);

            // Sample counts
            auto smpIt = m_cachedSampleCounts.find(stats.method);
            uint64_t smpCount = (smpIt != m_cachedSampleCounts.end()) ? smpIt->second : 0;
            double pctSmp = m_cachedTotalSamples > 0 ? (double)smpCount / (double)m_cachedTotalSamples * 100.0 : 0.0;

            fprintf(methodsFile, "%s,%llu,%.0f,%.2f,%.1f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%zu,%zu,%zu,%llu,%.1f\n", escapedName.c_str(), stats.callCount, callsPerSec, totalMs, pctTotal, selfMs, pctSelf, avgUs, selfAvgUs,
                minUs, maxUs, stats.icallTargets.size(), stats.callers.size(), stats.callees.size(), smpCount, pctSmp);
        }

        fclose(methodsFile);
    }

    // Export allocations
    FILE* allocsFile = fopen(allocsFilename, "w");
    if (allocsFile) {
        fprintf(allocsFile, "Type,Allocations,Allocs/s,Size,Total Bytes,Sites\n");

        double elapsed = MonoProfiler::Get().GetElapsedSeconds();
        for (const auto& stats : m_cachedAllocStats) {
            std::string escapedName = stats.typeName;
            bool needsQuotes = escapedName.find(',') != std::string::npos;
            if (needsQuotes) { escapedName = "\"" + escapedName + "\""; }

            double allocsPerSec = elapsed > 0.0 ? (double)stats.allocCount / elapsed : 0.0;
            fprintf(allocsFile, "%s,%llu,%.0f,%u,%llu,%zu\n", escapedName.c_str(), stats.allocCount, allocsPerSec, stats.instanceSize, stats.totalBytes, stats.callStacks.size());
        }

        fclose(allocsFile);
    }
}

void ProfilerPanel::ExportChromeTrace() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char filename[256];
    snprintf(filename, sizeof(filename), "profiler_trace_%04d%02d%02d_%02d%02d%02d.json", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    MonoProfiler::Get().ExportChromeTrace(filename);
}

void ProfilerPanel::NavigateToMethod(void* monoMethod) {
    // Try cached stats for the name first
    for (const auto& s : m_cachedMethodStats) {
        if (s.method == monoMethod) {
            m_methodWindowName = s.fullName;
            m_methodWindowMethod = monoMethod;
            m_showMethodWindow = true;
            return;
        }
    }

    // Not in cache, resolve name directly
    m_methodWindowName = MonoBridge::GetMethodFullName(monoMethod);
    m_methodWindowMethod = monoMethod;
    m_showMethodWindow = true;
}

bool ProfilerPanel::MatchesFilter(const std::string& text) const {
    if (m_filterText[0] == '\0') return true;
    // Case-insensitive substring search
    const char* filter = m_filterText;
    size_t filterLen = strlen(filter);
    auto it = std::search(text.begin(), text.end(), filter, filter + filterLen, [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); });
    return it != text.end();
}

void ProfilerPanel::SetFilter(const std::string& filter) {
    strncpy(m_filterText, filter.c_str(), sizeof(m_filterText) - 1);
    m_filterText[sizeof(m_filterText) - 1] = '\0';
}

// PROBES TABLE - targeted per-method timing
void ProfilerPanel::RenderProbesTable() {
    auto& profiler = MonoProfiler::Get();
    // Controls
    if (ImGui::Button("Detach All")) {
        profiler.DetachAllProbes();
        m_cachedProbes.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Stats")) { profiler.ResetProbeStats(); }
    ImGui::SameLine();
    ImGui::Text("Active probes: %zu / %zu", m_cachedProbes.size(), MonoProfiler::MAX_PROBES);

    if (!profiler.IsInitialized()) {
        Colors::Text(Colors::Get().negative, "Profiler not initialized - dispatch hooks required for probes");
        return;
    }

    if (m_cachedProbes.empty()) {
        ImGui::TextDisabled("No probes attached. Right-click a method in the Methods tab to attach a probe.");
        return;
    }

    ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV;

    if (ImGui::BeginTable("ProbesTable", 7, tableFlags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Method", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Total (Kcyc)", ImGuiTableColumnFlags_WidthFixed, 95);
        ImGui::TableSetupColumn("Avg (cyc)", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Min (cyc)", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Max (cyc)", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < m_cachedProbes.size(); i++) {
            const auto& probe = m_cachedProbes[i];
            uint64_t calls = probe.callCount.load(std::memory_order_relaxed);
            uint64_t total = probe.totalCycles.load(std::memory_order_relaxed);
            uint64_t minC = probe.minCycles.load(std::memory_order_relaxed);
            uint64_t maxC = probe.maxCycles.load(std::memory_order_relaxed);
            double avg = calls > 0 ? (double)total / (double)calls : 0.0;

            ImGui::TableNextRow();

            // Method name - green if receiving hits
            ImGui::TableNextColumn();
            ImGui::PushID(probe.targetMethod);
            if (calls > 0) {
                ImGui::TextColored(Colors::Get().positive, "%s", probe.methodName.c_str());
            } else {
                ImGui::Text("%s", probe.methodName.c_str());
            }

            // Calls
            ImGui::TableNextColumn();
            ImGui::Text("%llu", calls);

            // Total (Kcycles)
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", (double)total / 1000.0);

            // Avg (cycles)
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", avg);

            // Min (cycles)
            ImGui::TableNextColumn();
            if (minC == UINT64_MAX) {
                ImGui::TextDisabled("-");
            } else {
                ImGui::Text("%llu", minC);
            }

            // Max (cycles)
            ImGui::TableNextColumn();
            ImGui::Text("%llu", maxC);

            // Actions
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Detach")) { profiler.DetachProbe(probe.targetMethod); }
            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

} // namespace S3MM
