#include "heap_browser_panel.h"
#include "main_window.h"
#include "themes.h"
#include "../core/mono_bridge.h"
#include "../util/logger.h"
#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <sstream>

namespace S3MM {

void HeapBrowserPanel::Render() {
    // Scan button and stats
    if (m_isScanning) {
        Colors::Text(Colors::Get().neutral, "Scanning...");
    } else {
        if (ImGui::Button("Scan Heap")) { ScanHeap(); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Enumerate all live managed objects in the heap.\nThis may take a moment."); }
    }

    ImGui::SameLine();
    ImGui::Text("Objects: %zu  Types: %zu", GetObjectCount(), GetTypeCount());

    if (m_lastScanTime > 0.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%.1fms)", m_lastScanTime);
    }

    ImGui::Separator();

    // Two-pane layout
    float leftWidth = ImGui::GetContentRegionAvail().x * 0.45f;

    // Left pane: Type tree
    ImGui::BeginChild("TypeTree", ImVec2(leftWidth, -1), true);
    RenderTypeTree();
    ImGui::EndChild();

    ImGui::SameLine();

    // Right pane: Instance list
    ImGui::BeginChild("InstanceList", ImVec2(0, -1), true);
    RenderInstanceList();
    ImGui::EndChild();
}

void HeapBrowserPanel::RenderTypeTree() {
    // Filter input
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##typefilter", "Filter types...", m_typeFilter, sizeof(m_typeFilter))) {
        m_cachedFilterLower = m_typeFilter;
        std::transform(m_cachedFilterLower.begin(), m_cachedFilterLower.end(), m_cachedFilterLower.begin(), ::tolower);
    }

    ImGui::Separator();

    if (m_allObjects.empty()) {
        ImGui::TextDisabled("No objects loaded.");
        ImGui::TextDisabled("Click 'Scan Heap' to begin.");
        return;
    }

    // Render the namespace tree
    for (auto& [name, child] : m_rootNode.children) { RenderNamespaceNode(name, child); }

    // Render root-level types (no namespace)
    for (const auto& typeName : m_rootNode.types) {
        auto it = m_typeToIndices.find(typeName);
        if (it == m_typeToIndices.end()) continue;

        size_t count = it->second.size();

        // Apply filter
        if (!m_cachedFilterLower.empty()) {
            std::string lowerType = typeName;
            std::transform(lowerType.begin(), lowerType.end(), lowerType.begin(), ::tolower);
            if (lowerType.find(m_cachedFilterLower) == std::string::npos) continue;
        }

        ImGuiTreeNodeFlags leafFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;

        if (m_selectedType == typeName) { leafFlags |= ImGuiTreeNodeFlags_Selected; }

        ImGui::PushStyleColor(ImGuiCol_Text, Colors::Get().accent);

        // Extract just the class name for display
        std::string displayName = typeName;
        size_t lastDot = typeName.rfind('.');
        if (lastDot != std::string::npos) { displayName = typeName.substr(lastDot + 1); }

        ImGui::TreeNodeEx(typeName.c_str(), leafFlags, "%s (%zu)", displayName.c_str(), count);

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            m_selectedType = typeName;
            m_selectedInstance = SIZE_MAX;
        }

        // Right-click context menu on type
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("View Allocation Stats")) { MainWindow::Get().OpenProfilerForClass(typeName); }
            ImGui::EndPopup();
        }

        ImGui::PopStyleColor();
    }
}

void HeapBrowserPanel::RenderNamespaceNode(const std::string& path, NamespaceNode& node) {
    // Check if this node or children match filter
    bool hasVisibleContent = m_cachedFilterLower.empty();
    if (!hasVisibleContent) {
        // Check if path matches
        std::string lowerPath = path;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
        if (lowerPath.find(m_cachedFilterLower) != std::string::npos) { hasVisibleContent = true; }

        // Check types in this node
        if (!hasVisibleContent) {
            for (const auto& typeName : node.types) {
                std::string lowerType = typeName;
                std::transform(lowerType.begin(), lowerType.end(), lowerType.begin(), ::tolower);
                if (lowerType.find(m_cachedFilterLower) != std::string::npos) {
                    hasVisibleContent = true;
                    break;
                }
            }
        }

        // Check children recursively (simple check - just see if any exist)
        if (!hasVisibleContent && !node.children.empty()) {
            hasVisibleContent = true; // Let children filter themselves
        }
    }

    if (!hasVisibleContent) return;

    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanAvailWidth;

    // Auto-expand when filtering
    if (!m_cachedFilterLower.empty()) { ImGui::SetNextItemOpen(true, ImGuiCond_Always); }

    bool nodeOpen = ImGui::TreeNodeEx(path.c_str(), nodeFlags, "%s", node.name.c_str());

    // Show count
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", node.totalCount);

    if (nodeOpen) {
        // Render child namespaces first
        for (auto& [childName, child] : node.children) {
            std::string childPath = path.empty() ? childName : path + "." + childName;
            RenderNamespaceNode(childPath, child);
        }

        // Render types in this namespace
        for (const auto& typeName : node.types) {
            auto it = m_typeToIndices.find(typeName);
            if (it == m_typeToIndices.end()) continue;

            size_t count = it->second.size();

            // Apply filter
            if (!m_cachedFilterLower.empty()) {
                std::string lowerType = typeName;
                std::transform(lowerType.begin(), lowerType.end(), lowerType.begin(), ::tolower);
                if (lowerType.find(m_cachedFilterLower) == std::string::npos) continue;
            }

            ImGuiTreeNodeFlags leafFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;

            if (m_selectedType == typeName) { leafFlags |= ImGuiTreeNodeFlags_Selected; }

            ImGui::PushStyleColor(ImGuiCol_Text, Colors::Get().accent);

            // Extract just the class name for display
            std::string displayName = typeName;
            size_t lastDot = typeName.rfind('.');
            if (lastDot != std::string::npos) { displayName = typeName.substr(lastDot + 1); }

            ImGui::TreeNodeEx(typeName.c_str(), leafFlags, "%s (%zu)", displayName.c_str(), count);

            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                m_selectedType = typeName;
                m_selectedInstance = SIZE_MAX;
            }

            // Right-click context menu on type
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("View Allocation Stats")) { MainWindow::Get().OpenProfilerForClass(typeName); }
                ImGui::EndPopup();
            }

            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("%s", typeName.c_str());
                ImGui::Text("%zu instances", count);
                ImGui::TextDisabled("Right-click for options");
                ImGui::EndTooltip();
            }

            ImGui::PopStyleColor();
        }

        ImGui::TreePop();
    }
}

void HeapBrowserPanel::RenderInstanceList() {
    if (m_selectedType.empty()) {
        ImGui::TextDisabled("Select a type to see instances");
        return;
    }

    ImGui::Text("%s", m_selectedType.c_str());
    ImGui::Separator();

    auto it = m_typeToIndices.find(m_selectedType);
    if (it == m_typeToIndices.end() || it->second.empty()) {
        ImGui::TextDisabled("No instances");
        return;
    }

    const auto& indices = it->second;
    bool isStringType = (m_selectedType == "System.String" || m_selectedType == "String");

    // Render instance list
    for (size_t i = 0; i < indices.size(); i++) {
        size_t objIdx = indices[i];
        if (objIdx >= m_allObjects.size()) continue;

        const HeapObject& obj = m_allObjects[objIdx];
        bool isValid = IsHeapPointer((void*)obj.address);

        bool isSelected = (m_selectedInstance == i);

        // Build display label - show preview for strings
        char label[256];
        if (isStringType && isValid) {
            std::string strValue = MonoBridge::Get().ExtractString((void*)obj.address);
            // Truncate long strings for display
            if (strValue.length() > 40) { strValue = strValue.substr(0, 40) + "..."; }
            snprintf(label, sizeof(label), "0x%08X  \"%s\"", (unsigned int)obj.address, strValue.c_str());
        } else {
            snprintf(label, sizeof(label), "0x%08X", (unsigned int)obj.address);
        }

        // Grey out invalid objects
        if (!isValid) { ImGui::PushStyleColor(ImGuiCol_Text, Colors::Desaturate(Colors::Get().accent)); }

        if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
            m_selectedInstance = i;

            // Double-click to explore
            if (ImGui::IsMouseDoubleClicked(0) && isValid) {
                MainWindow::Get().ExploreObject((void*)obj.address, m_selectedType);
                LOG_INFO("[HeapBrowser] Exploring object at 0x" + ToHex(obj.address));
            }
        }

        if (!isValid) { ImGui::PopStyleColor(); }

        // Right-click context menu
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Explore in Object Explorer", nullptr, false, isValid)) { MainWindow::Get().ExploreObject((void*)obj.address, m_selectedType); }
            if (ImGui::MenuItem("Copy Address")) {
                char addrStr[32];
                snprintf(addrStr, sizeof(addrStr), "0x%08X", (unsigned int)obj.address);
                ImGui::SetClipboardText(addrStr);
            }
            if (isStringType && isValid) {
                if (ImGui::MenuItem("Copy String Value")) {
                    std::string strValue = MonoBridge::Get().ExtractString((void*)obj.address);
                    ImGui::SetClipboardText(strValue.c_str());
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("View Allocation Stats")) { MainWindow::Get().OpenProfilerForClass(m_selectedType); }
            ImGui::Separator();
            ImGui::TextDisabled("Class: 0x%08X", (unsigned int)(uintptr_t)obj.klass);
            ImGui::EndPopup();
        }

        // Tooltip
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Address: 0x%08X", (unsigned int)obj.address);
            if (!isValid) {
                Colors::Text(Colors::Get().negative, "Object no longer valid!");
            } else {
                ImGui::TextDisabled("Double-click to explore, right-click for options");
            }
            ImGui::EndTooltip();
        }
    }
}

void HeapBrowserPanel::ScanHeap() {
    if (m_isScanning) return;

    auto& bridge = MonoBridge::Get();
    if (!bridge.IsRuntimeReady()) {
        LOG_ERROR("[HeapBrowser] Runtime not ready");
        return;
    }

    m_isScanning = true;
    Clear();

    LOG_INFO("[HeapBrowser] Starting heap scan...");

    auto startTime = std::chrono::high_resolution_clock::now();

    // Collect all objects
    bridge.ScanHeap([this](uintptr_t objAddress, void* klass) {
        // Get type name
        const char* name = MonoBridge::GetClassName(klass);
        const char* ns = MonoBridge::GetClassNamespace(klass);

        // Name pointer must be a valid heap/module pointer, not garbage
        if (!name || !IsHeapPointer((void*)name)) return;

        // Validate that name looks like a valid C identifier (printable ASCII, reasonable length)
        bool validName = true;
        int nameLen = 0;
        for (int i = 0; i < 256 && name[i]; i++) {
            char c = name[i];
            // Valid class name chars: alphanumeric, underscore, backtick (generic), angle brackets, comma, space
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '`' || c == '<' || c == '>' || c == ',' || c == ' ' || c == '[' || c == ']')) {
                validName = false;
                break;
            }
            nameLen++;
        }
        if (!validName || nameLen == 0 || nameLen >= 256) return;

        std::string typeName;
        if (ns && IsHeapPointer((void*)ns) && ns[0] != '\0') {
            typeName = std::string(ns) + "." + name;
        } else {
            typeName = name;
        }

        HeapObject obj;
        obj.address = objAddress;
        obj.klass = klass;
        obj.typeName = typeName;
        obj.typeNameLower = typeName;
        std::transform(obj.typeNameLower.begin(), obj.typeNameLower.end(), obj.typeNameLower.begin(), ::tolower);

        m_allObjects.push_back(std::move(obj));
    });

    auto endTime = std::chrono::high_resolution_clock::now();
    m_lastScanTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    LOG_INFO("[HeapBrowser] Found " + std::to_string(m_allObjects.size()) + " objects in " + std::to_string(m_lastScanTime) + "ms");

    // Build indices
    BuildTypeIndex();
    BuildNamespaceTree();

    LOG_INFO("[HeapBrowser] Indexed " + std::to_string(m_typeToIndices.size()) + " types");

    m_isScanning = false;
}

void HeapBrowserPanel::Clear() {
    m_allObjects.clear();
    m_typeToIndices.clear();
    m_rootNode = NamespaceNode();
    m_selectedType.clear();
    m_selectedInstance = SIZE_MAX;
}

void HeapBrowserPanel::BuildTypeIndex() {
    m_typeToIndices.clear();

    for (size_t i = 0; i < m_allObjects.size(); i++) {
        const auto& obj = m_allObjects[i];
        m_typeToIndices[obj.typeName].push_back(i);
    }
}

void HeapBrowserPanel::BuildNamespaceTree() {
    m_rootNode = NamespaceNode();
    m_rootNode.name = "";

    for (const auto& [typeName, indices] : m_typeToIndices) { AddToNamespaceTree(typeName, indices.size()); }
}

void HeapBrowserPanel::AddToNamespaceTree(const std::string& fullTypeName, size_t count) {
    // Split by '.' to get namespace segments
    std::vector<std::string> segments;
    std::stringstream ss(fullTypeName);
    std::string segment;

    while (std::getline(ss, segment, '.')) {
        if (!segment.empty()) { segments.push_back(segment); }
    }

    if (segments.empty()) return;

    // Last segment is the type name, rest is namespace
    std::string typeName = segments.back();
    segments.pop_back();

    // Navigate/create namespace path
    NamespaceNode* current = &m_rootNode;
    current->totalCount += count;

    for (const auto& seg : segments) {
        auto it = current->children.find(seg);
        if (it == current->children.end()) {
            NamespaceNode newNode;
            newNode.name = seg;
            current->children[seg] = newNode;
            it = current->children.find(seg);
        }
        current = &it->second;
        current->totalCount += count;
    }

    // Add the type to the leaf namespace
    current->types.push_back(fullTypeName);

    // Sort types alphabetically
    std::sort(current->types.begin(), current->types.end());
}

} // namespace S3MM
