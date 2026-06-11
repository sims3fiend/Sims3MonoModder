#include "xref_panel.h"
#include "main_window.h"
#include "class_browser_panel.h"
#include "themes.h"
#include "../core/mono_bridge.h"
#include "../util/logger.h"
#include "imgui.h"
#include <algorithm>

namespace S3MM {

void XRefPanel::FindCallers(void* image, const std::string& methodFullName) {
    // Cancel any existing scan
    if (m_isScanning && m_scanFuture.valid()) {
        // Can't cancel std::async, but we'll ignore its result
        m_isScanning = false;
    }

    m_mode = Mode::Callers;
    m_targetMethod = methodFullName;
    m_image = image;
    m_methodHandle = nullptr;
    m_resultsComputed = false;
    m_searchAllAssemblies = false; // Start with single assembly
    m_results.clear();
    m_groupedResults.clear();
    m_visible = true;

    LOG_INFO("[XRefPanel] FindCallers: " + methodFullName);

    // Start async scan
    StartAsyncScan();
}

void XRefPanel::FindCallees(void* image, void* methodHandle, const std::string& methodFullName) {
    // Cancel any existing scan
    if (m_isScanning && m_scanFuture.valid()) { m_isScanning = false; }

    m_mode = Mode::Callees;
    m_targetMethod = methodFullName;
    m_image = image;
    m_methodHandle = methodHandle;
    m_resultsComputed = false;
    m_results.clear();
    m_groupedResults.clear();
    m_visible = true;

    LOG_INFO("[XRefPanel] FindCallees: " + methodFullName);

    // Start async scan
    StartAsyncScan();
}

void XRefPanel::StartAsyncScan() {
    m_isScanning = true;
    m_scanStartTime = std::chrono::steady_clock::now();

    if (m_mode == Mode::Callers) {
        std::string target = m_targetMethod;

        if (m_searchAllAssemblies) {
            // Search all loaded assemblies
            m_scanFuture = std::async(std::launch::async, [target]() { return S3MM::FindCallersAllAssemblies(target); });
            LOG_INFO("[XRefPanel] Async scan started (all assemblies)");
        } else {
            // Search single assembly
            if (!m_image) {
                LOG_WARN("[XRefPanel] No image set");
                m_isScanning = false;
                m_resultsComputed = true;
                return;
            }
            void* image = m_image;
            m_scanFuture = std::async(std::launch::async, [image, target]() { return S3MM::FindCallers(image, target); });
            LOG_INFO("[XRefPanel] Async scan started (single assembly)");
        }
    } else if (m_mode == Mode::Callees) {
        // Callees is fast (single method), but still async for consistency
        if (!m_image) {
            LOG_WARN("[XRefPanel] No image set");
            m_isScanning = false;
            m_resultsComputed = true;
            return;
        }
        void* image = m_image;
        void* handle = m_methodHandle;
        std::string target = m_targetMethod;

        m_scanFuture = std::async(std::launch::async, [image, handle, target]() { return S3MM::FindCallees(image, handle, target); });
        LOG_INFO("[XRefPanel] Async scan started");
    }
}

void XRefPanel::CheckScanComplete() {
    if (!m_isScanning || !m_scanFuture.valid()) return;

    // Check if future is ready (non-blocking)
    if (m_scanFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        m_results = m_scanFuture.get();
        m_isScanning = false;
        m_resultsComputed = true;

        auto elapsed = std::chrono::steady_clock::now() - m_scanStartTime;
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        LOG_INFO("[XRefPanel] Scan complete: " + std::to_string(m_results.size()) + " results in " + std::to_string(elapsedMs) + "ms");

        GroupResults();
    }
}

void XRefPanel::GroupResults() {
    m_groupedResults.clear();

    for (const auto& entry : m_results) {
        std::string ns = entry.sourceNamespace.empty() ? "(global)" : entry.sourceNamespace;
        m_groupedResults[ns].classes[entry.sourceClass].entries.push_back(entry);
    }
}

const char* XRefPanel::GetOpcodeName(uint8_t opcode) {
    switch (opcode) {
    case 0x28:
        return "call";
    case 0x29:
        return "calli";
    case 0x6F:
        return "callvirt";
    case 0x73:
        return "newobj";
    case 0x06:
        return "ldftn"; // FE 06
    case 0x07:
        return "ldvirtftn"; // FE 07
    case 0x7B:
        return "ldfld";
    case 0x7C:
        return "ldflda";
    case 0x7D:
        return "stfld";
    case 0x7E:
        return "ldsfld";
    case 0x7F:
        return "ldsflda";
    case 0x80:
        return "stsfld";
    default:
        return "?";
    }
}

// Field opcodes (ldfld/stfld/etc.) target a field, calli has no method token, and MemberRef/MethodSpec targets resolve to a bare name (owner discarded) those are filtered separately by requiring "::" in the resolved targetName.
static bool IsNavigableCalleeOpcode(uint8_t opcode) {
    switch (opcode) {
    case 0x28: // call
    case 0x6F: // callvirt
    case 0x73: // newobj
    case 0x06: // ldftn (FE 06)
    case 0x07: // ldvirtftn (FE 07)
        return true;
    default:
        return false;
    }
}

void XRefPanel::RenderScanningIndicator() {
    auto elapsed = std::chrono::steady_clock::now() - m_scanStartTime;
    auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

    // Animated spinner
    const char* spinner[] = {"|", "/", "-", "\\"};
    int frame = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 100) % 4;

    Colors::Text(Colors::Get().neutral, "%s Scanning assembly... (%llds)", spinner[frame], (long long)elapsedSec);

    ImGui::TextDisabled("This may take a while for large assemblies");

    if (ImGui::Button("Cancel")) {
        m_isScanning = false;
        m_resultsComputed = true;
        LOG_INFO("[XRefPanel] Scan cancelled by user");
    }
}

void XRefPanel::Render() {
    if (!m_visible) return;

    // Check if async scan completed
    CheckScanComplete();

    // Build window title
    std::string title;
    if (m_mode == Mode::Callers) {
        title = "Callers of: " + m_targetMethod + "###XRefPanel";
    } else if (m_mode == Mode::Callees) {
        title = "Callees of: " + m_targetMethod + "###XRefPanel";
    } else {
        title = "IL References###XRefPanel";
    }

    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(title.c_str(), &m_visible)) {
        if (m_isScanning) {
            RenderScanningIndicator();
        } else {
            RenderResults();
        }
    }
    ImGui::End();
}

void XRefPanel::RenderResults() {
    // Header
    if (m_mode == Mode::Callers) {
        ImGui::TextColored(Colors::Get().accent, "Callers of:");
    } else if (m_mode == Mode::Callees) {
        ImGui::TextColored(Colors::Brighten(Colors::Get().accent), "Callees of:");
    } else {
        ImGui::TextDisabled("No query active");
        return;
    }

    ImGui::SameLine();
    ImGui::Text("%s", m_targetMethod.c_str());

    // Result count and search scope
    if (m_searchAllAssemblies) {
        ImGui::Text("%zu results (all assemblies)", m_results.size());
    } else {
        ImGui::Text("%zu results", m_results.size());

        // Offer to expand search (only for callers, and only if not already searching all)
        if (m_mode == Mode::Callers) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Search all assemblies")) {
                m_searchAllAssemblies = true;
                m_resultsComputed = false;
                m_results.clear();
                m_groupedResults.clear();
                StartAsyncScan();
            }
        }
    }
    ImGui::Separator();

    if (m_results.empty()) {
        ImGui::TextDisabled("No references found");
        if (!m_searchAllAssemblies && m_mode == Mode::Callers) { ImGui::TextDisabled("Try 'Search all assemblies' above"); }
        return;
    }

    // Scrollable results area
    ImGui::BeginChild("XRefResults", ImVec2(0, 0), false);

    if (m_mode == Mode::Callers) {
        // Group by namespace -> class -> method
        for (auto& [nsName, nsGroup] : m_groupedResults) {
            // Namespace collapsible
            bool nsOpen = ImGui::TreeNodeEx(nsName.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);

            if (nsOpen) {
                for (auto& [className, classGroup] : nsGroup.classes) {
                    // Class collapsible
                    bool classOpen = ImGui::TreeNodeEx(className.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);

                    if (classOpen) {
                        for (const auto& entry : classGroup.entries) {
                            // Format: MethodName (+0xNN) [opcode]
                            char label[256];
                            snprintf(label, sizeof(label), "%s (+0x%X) [%s]", entry.sourceMethod.c_str(), entry.ilOffset, GetOpcodeName(entry.opcode));

                            if (ImGui::Selectable(label)) {
                                // Navigate to the method
                                std::string fullClassName;
                                if (!entry.sourceNamespace.empty()) {
                                    fullClassName = entry.sourceNamespace + "." + entry.sourceClass;
                                } else {
                                    fullClassName = entry.sourceClass;
                                }
                                MainWindow::Get().NavigateToHook(fullClassName, entry.sourceMethod);
                            }

                            // Right-click context menu
                            if (ImGui::BeginPopupContextItem()) {
                                if (ImGui::MenuItem("Profile Method")) { MainWindow::Get().OpenProfilerForMethod(entry.SourceFullName()); }
                                ImGui::EndPopup();
                            }

                            // Tooltip with full details
                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::Text("Source: %s", entry.SourceFullName().c_str());
                                ImGui::Text("Target: %s", entry.targetName.c_str());
                                ImGui::Text("IL Offset: 0x%X", entry.ilOffset);
                                ImGui::Text("Opcode: %s (0x%02X)", GetOpcodeName(entry.opcode), entry.opcode);
                                ImGui::TextDisabled("Click to navigate, right-click for options");
                                ImGui::EndTooltip();
                            }
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
        }
    } else if (m_mode == Mode::Callees) {
        // For callees, show flat list with target names
        for (const auto& entry : m_results) {
            // Format: [opcode] TargetName (+0xNN)
            char label[512];
            snprintf(label, sizeof(label), "[%s] %s (+0x%X)", GetOpcodeName(entry.opcode), entry.targetName.c_str(), entry.ilOffset);

            // Color by opcode type (theme-adaptive)
            const auto& xc = Colors::Get();
            ImVec4 color;
            if (entry.opcode == 0x28 || entry.opcode == 0x29 || entry.opcode == 0x6F) {
                color = xc.accent; // Blue for calls
            } else if (entry.opcode == 0x73) {
                color = xc.positive; // Green for newobj
            } else {
                color = xc.neutral; // Amber for field access
            }

            // Navigable only for method-call opcodes whose target resolved to a MethodDef ("Namespace.Class::Method")
            // Bare names (cross-assembly MemberRef) and field accesses are left non-navigable.
            size_t colonPos = entry.targetName.find("::");
            bool navigable = IsNavigableCalleeOpcode(entry.opcode) && colonPos != std::string::npos;

            ImGui::PushStyleColor(ImGuiCol_Text, color);
            if (ImGui::Selectable(label)) {
                if (navigable) {
                    std::string fullClassName = entry.targetName.substr(0, colonPos);
                    std::string targetMethod = entry.targetName.substr(colonPos + 2);
                    MainWindow::Get().NavigateToHook(fullClassName, targetMethod);
                } else {
                    LOG_INFO("[XRefPanel] Target not navigable (external ref or field): " + entry.targetName);
                }
            }
            ImGui::PopStyleColor();

            // Right-click context menu
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Profile Method")) { MainWindow::Get().OpenProfilerForMethod(entry.targetName); }
                ImGui::EndPopup();
            }

            // Tooltip
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Target: %s", entry.targetName.c_str());
                ImGui::Text("IL Offset: 0x%X", entry.ilOffset);
                ImGui::Text("Opcode: %s (0x%02X)", GetOpcodeName(entry.opcode), entry.opcode);
                if (navigable) {
                    ImGui::TextDisabled("Click to navigate, right-click for options");
                } else {
                    ImGui::TextDisabled("External ref or field - right-click for options");
                }
                ImGui::EndTooltip();
            }
        }
    }

    ImGui::EndChild();
}

} // namespace S3MM
