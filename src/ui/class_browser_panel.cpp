#include "class_browser_panel.h"
#include "main_window.h"
#include "themes.h"
#include "../core/mono_bridge.h"
#include "../util/logger.h"
#include "imgui.h"
#include <algorithm>
#include <sstream>

namespace S3MM {

void ClassBrowserPanel::Render() {
    // Assembly dropdown
    if (m_assemblyList.empty()) { RefreshAssemblyList(); }

    ImGui::Text("Assembly:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##assembly", m_assemblyList.empty() ? "Loading..." : m_assemblyList[m_selectedAssembly].c_str())) {
        for (int i = 0; i < (int)m_assemblyList.size(); i++) {
            bool isSelected = (i == m_selectedAssembly);
            if (ImGui::Selectable(m_assemblyList[i].c_str(), isSelected)) {
                if (m_selectedAssembly != i) {
                    m_selectedAssembly = i;
                    // Update selected image pointer
                    if (i == 0) {
                        m_selectedAssemblyImage = nullptr; // All assemblies
                    } else {
                        const auto& images = MonoBridge::Get().GetAllImages();
                        if (i - 1 < (int)images.size()) { m_selectedAssemblyImage = images[i - 1]; }
                    }
                }
            }
            if (isSelected) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Classes") && !m_isLoading) { LoadClasses(); }
    ImGui::SameLine();
    if (ImGui::Button("Reload") && !m_isLoading) {
        RefreshAssemblyList();
        LoadClasses();
    }
    if (m_isLoading) {
        ImGui::SameLine();
        Colors::Text(Colors::Get().neutral, "Loading...");
    }

    ImGui::Separator();

    // Search filter - recompute visibility only when filter text changes
    ImGui::SetNextItemWidth(-60);
    if (ImGui::InputTextWithHint("##search", "Filter...", m_searchFilter, sizeof(m_searchFilter))) {
        m_cachedSearchFilterLower = m_searchFilter;
        std::transform(m_cachedSearchFilterLower.begin(), m_cachedSearchFilterLower.end(), m_cachedSearchFilterLower.begin(), ::tolower);
    }
    // Recompute visibility cache if filter changed
    if (m_cachedSearchFilterLower != m_lastAppliedFilter && !m_classes.empty()) { RecomputeFilterVisibility(m_cachedSearchFilterLower); }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu", m_classes.size());

    // Class tree
    ImGui::BeginChild("ClassTree", ImVec2(-1, -1), true);

    const std::string& filter = m_cachedSearchFilterLower;

    if (m_rootNode) {
        // Render tree starting from root
        for (auto& child : m_rootNode->children) { RenderNamespaceNode(child.get(), filter); }

        // Also render classes at root level (no namespace)
        for (size_t idx : m_rootNode->classIndices) {
            if (idx >= m_classes.size()) continue;

            // Use cached visibility
            if (!m_classFilterVisible[idx]) continue;

            const std::string& displayName = m_classDisplayNames[idx];

            ImVec4 color = m_classIsEnum[idx] ? Colors::Get().neutral : Colors::Get().accent;

            ImGui::PushStyleColor(ImGuiCol_Text, color);

            ImGuiTreeNodeFlags leafFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;

            if (m_selectedIndex == idx) { leafFlags |= ImGuiTreeNodeFlags_Selected; }

            ImGui::TreeNodeEx((void*)(intptr_t)idx, leafFlags, "%s", displayName.c_str());

            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                if (m_selectedIndex != idx) {
                    m_selectedIndex = idx;
                    m_selectionChanged = true;
                    LOG_INFO("[ClassBrowser] Selected: " + m_classFullNames[idx]);
                }
            }

            ImGui::PopStyleColor();
        }
    } else if (!m_classes.empty()) {
        // Fallback: flat list if tree not built
        for (size_t i = 0; i < m_classes.size(); i++) {
            if (!m_classFilterVisible[i]) continue;

            const std::string& fullName = m_classFullNames[i];

            ImVec4 color = m_classIsEnum[i] ? Colors::Get().neutral : Colors::Get().accent;

            ImGui::PushStyleColor(ImGuiCol_Text, color);

            bool isSelected = (m_selectedIndex == i);
            if (ImGui::Selectable(fullName.c_str(), isSelected)) {
                if (m_selectedIndex != i) {
                    m_selectedIndex = i;
                    m_selectionChanged = true;
                }
            }

            ImGui::PopStyleColor();
        }
    } else {
        ImGui::TextDisabled("No classes loaded.");
        ImGui::TextDisabled("Click 'Load Classes' to begin.");
    }

    ImGui::EndChild();
}

void ClassBrowserPanel::RenderNamespaceNode(NamespaceNode* node, const std::string& filter) {
    if (!node) return;

    // Use cached visibility (recomputed only when filter changes)
    if (!node->filterVisible) return;

    // Count total items for display
    size_t totalClasses = node->classIndices.size();
    for (const auto& child : node->children) {
        // Could count recursively, but let's keep it simple
        totalClasses += child->classIndices.size();
    }

    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanAvailWidth;

    // Auto-expand if filtering
    if (!filter.empty()) { ImGui::SetNextItemOpen(true, ImGuiCond_Always); }

    // Render namespace node
    bool nodeOpen = ImGui::TreeNodeEx(node, nodeFlags, "%s", node->name.c_str());

    // Show count in gray
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", node->classIndices.size());

    if (nodeOpen) {
        // Render child namespaces first
        for (auto& child : node->children) { RenderNamespaceNode(child.get(), filter); }

        // Render classes in this namespace (including nested children)
        for (size_t idx : node->classIndices) { RenderClassWithNested(idx, filter, 0); }

        ImGui::TreePop();
    }
}

void ClassBrowserPanel::RecomputeFilterVisibility(const std::string& filter) {
    m_lastAppliedFilter = filter;
    m_classFilterVisible.assign(m_classes.size(), false);

    if (filter.empty()) {
        // Everything visible
        std::fill(m_classFilterVisible.begin(), m_classFilterVisible.end(), true);
        if (m_rootNode) RecomputeNodeVisibility(m_rootNode.get(), filter);
        return;
    }

    // Bottom-up: compute class visibility first, then propagate to nodes.
    // Process classes in reverse so nested children are resolved before parents.
    for (size_t i = m_classes.size(); i-- > 0;) { RecomputeClassVisibility(i, filter); }

    // Then compute node visibility
    if (m_rootNode) RecomputeNodeVisibility(m_rootNode.get(), filter);
}

bool ClassBrowserPanel::RecomputeClassVisibility(size_t classIdx, const std::string& filter) {
    if (filter.empty()) {
        m_classFilterVisible[classIdx] = true;
        return true;
    }
    if (classIdx >= m_classes.size()) return false;

    // Already computed? (nested children may have been visited first)
    // We can't early-return because we process in reverse, but parent classes need to check children.
    // Re-check is cheap since children are already resolved.

    bool visible = false;

    // Check if this class name matches
    if (classIdx < m_classNamesLower.size() && m_classNamesLower[classIdx].find(filter) != std::string::npos) visible = true;
    if (!visible && classIdx < m_classFullNamesLower.size() && m_classFullNamesLower[classIdx].find(filter) != std::string::npos) visible = true;

    // Check if any nested children are visible (parent should show if child matches)
    if (!visible) {
        auto it = m_nestedChildren.find(classIdx);
        if (it != m_nestedChildren.end()) {
            for (size_t childIdx : it->second) {
                if (m_classFilterVisible[childIdx]) {
                    visible = true;
                    break;
                }
            }
        }
    }

    m_classFilterVisible[classIdx] = visible;
    return visible;
}

bool ClassBrowserPanel::RecomputeNodeVisibility(NamespaceNode* node, const std::string& filter) {
    if (!node) return false;
    if (filter.empty()) {
        node->filterVisible = true;
        return true;
    }

    bool visible = false;

    // Check if namespace name matches
    if (node->fullPathLower.find(filter) != std::string::npos) visible = true;

    // Check classes in this node
    if (!visible) {
        for (size_t idx : node->classIndices) {
            if (idx < m_classFilterVisible.size() && m_classFilterVisible[idx]) {
                visible = true;
                break;
            }
        }
    }

    // Recurse into children
    for (auto& child : node->children) {
        if (RecomputeNodeVisibility(child.get(), filter)) visible = true;
    }

    node->filterVisible = visible;
    return visible;
}

void ClassBrowserPanel::RenderClassWithNested(size_t classIdx, const std::string& filter, int depth) {
    if (classIdx >= m_classes.size()) return;

    // Use cached visibility (recomputed only when filter changes)
    if (!m_classFilterVisible[classIdx]) return;

    const std::string& displayName = m_classDisplayNames[classIdx];
    const std::string& fullName = m_classFullNames[classIdx];
    bool isEnum = m_classIsEnum[classIdx];

    // Check if this class has nested children
    auto nestedIt = m_nestedChildren.find(classIdx);
    bool hasNestedChildren = (nestedIt != m_nestedChildren.end() && !nestedIt->second.empty());

    // Color based on type (same for nested and non-nested)
    ImVec4 color = isEnum ? Colors::Get().neutral : Colors::Get().accent;

    ImGui::PushStyleColor(ImGuiCol_Text, color);

    if (hasNestedChildren) {
        // Render as expandable node
        ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
        if (m_selectedIndex == classIdx) { nodeFlags |= ImGuiTreeNodeFlags_Selected; }

        // Auto-expand if filtering and children match
        if (!filter.empty()) { ImGui::SetNextItemOpen(true, ImGuiCond_Always); }

        bool nodeOpen = ImGui::TreeNodeEx((void*)(intptr_t)classIdx, nodeFlags, "%s", displayName.c_str());

        // Handle selection
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            if (m_selectedIndex != classIdx) {
                m_selectedIndex = classIdx;
                m_selectionChanged = true;
                LOG_INFO("[ClassBrowser] Selected: " + fullName);
            }
        }

        // Tooltip (only computed when hovered, rare path)
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", fullName.c_str());
            if (isEnum) { ImGui::TextColored(Colors::Get().neutral, "(Enum)"); }
            ImGui::TextDisabled("(%zu nested classes)", nestedIt->second.size());
            ImGui::EndTooltip();
        }

        // Context menu (only computed when open, rare path)
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Copy Full Name")) { ImGui::SetClipboardText(fullName.c_str()); }
            if (ImGui::MenuItem("Profile All Methods")) { MainWindow::Get().OpenProfilerForClass(fullName); }
            if (ImGui::MenuItem("Copy Class Handle")) {
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%08X", (unsigned int)(uintptr_t)m_classes[classIdx].Handle());
                ImGui::SetClipboardText(buf);
            }
            ImGui::EndPopup();
        }

        ImGui::PopStyleColor();

        if (nodeOpen) {
            // Render nested children
            for (size_t childIdx : nestedIt->second) { RenderClassWithNested(childIdx, filter, depth + 1); }
            ImGui::TreePop();
        }
    } else {
        // Render as leaf node
        ImGuiTreeNodeFlags leafFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;

        if (m_selectedIndex == classIdx) { leafFlags |= ImGuiTreeNodeFlags_Selected; }

        ImGui::TreeNodeEx((void*)(intptr_t)classIdx, leafFlags, "%s", displayName.c_str());

        // Handle selection
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            if (m_selectedIndex != classIdx) {
                m_selectedIndex = classIdx;
                m_selectionChanged = true;
                LOG_INFO("[ClassBrowser] Selected: " + fullName);
            }
        }

        // Tooltip (only computed when hovered, rare path)
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", fullName.c_str());
            if (isEnum) { ImGui::TextColored(Colors::Get().neutral, "(Enum)"); }
            ImGui::EndTooltip();
        }

        // Context menu (only computed when open, rare path)
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Copy Full Name")) { ImGui::SetClipboardText(fullName.c_str()); }
            if (ImGui::MenuItem("Profile All Methods")) { MainWindow::Get().OpenProfilerForClass(fullName); }
            if (ImGui::MenuItem("Copy Class Handle")) {
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%08X", (unsigned int)(uintptr_t)m_classes[classIdx].Handle());
                ImGui::SetClipboardText(buf);
            }
            ImGui::EndPopup();
        }

        ImGui::PopStyleColor();
    }
}

void ClassBrowserPanel::BuildNamespaceTree() {
    LOG_INFO("[ClassBrowser] Building namespace tree for " + std::to_string(m_classes.size()) + " classes");

    // Create root node
    m_rootNode = std::make_unique<NamespaceNode>("", "");
    m_pathToNode.clear();
    m_pathToNode[""] = m_rootNode.get();
    m_nestedChildren.clear();

    // First pass: build map of full names to indices for nested class parent lookup
    std::unordered_map<std::string, size_t> fullNameToIndex;
    for (size_t i = 0; i < m_classes.size(); i++) { fullNameToIndex[m_classFullNames[i]] = i; }

    // Second pass: identify nested class relationships and build tree
    int nestedCount = 0;
    for (size_t i = 0; i < m_classes.size(); i++) {
        const MonoClass& klass = m_classes[i];

        // Handle nested classes - link to parent
        if (klass.IsNested()) {
            nestedCount++;
            // Get parent class's full name
            MonoClass parent = klass.DeclaringClass();
            if (parent.IsValid()) {
                std::string parentFullName = parent.FullName();
                auto it = fullNameToIndex.find(parentFullName);
                if (it != fullNameToIndex.end()) {
                    // Found parent in our loaded classes - add as child
                    m_nestedChildren[it->second].push_back(i);
                    continue; // Don't add to namespace tree directly
                }
            }
            // Parent not found - fall through and add to namespace like a top-level class.
            // This can happen if parent class wasn't loaded due to namespace filtering.
        }

        // Top-level class (or nested class without loaded parent)
        std::string ns = klass.Namespace();

        if (ns.empty()) {
            // No namespace - add directly to root
            m_rootNode->classIndices.push_back(i);
            continue;
        }

        // Split namespace by '.' and build tree path
        std::vector<std::string> segments;
        std::stringstream ss(ns);
        std::string segment;
        while (std::getline(ss, segment, '.')) {
            if (!segment.empty()) { segments.push_back(segment); }
        }

        // Navigate/create path in tree
        NamespaceNode* current = m_rootNode.get();
        std::string currentPath;

        for (const auto& seg : segments) {
            currentPath += (currentPath.empty() ? "" : ".") + seg;

            // Check if node already exists
            auto it = m_pathToNode.find(currentPath);
            if (it != m_pathToNode.end()) {
                current = it->second;
            } else {
                // Create new node
                auto newNode = std::make_unique<NamespaceNode>(seg, currentPath);
                NamespaceNode* newNodePtr = newNode.get();
                current->children.push_back(std::move(newNode));
                m_pathToNode[currentPath] = newNodePtr;
                current = newNodePtr;
            }
        }

        // Add class index to the leaf namespace node
        current->classIndices.push_back(i);
    }

    // Sort the tree
    SortTreeRecursive(m_rootNode.get());

    LOG_INFO("[ClassBrowser] Built tree with " + std::to_string(m_pathToNode.size()) + " namespace nodes, " + std::to_string(nestedCount) + " nested classes");
}

void ClassBrowserPanel::SortTreeRecursive(NamespaceNode* node) {
    if (!node) return;

    // Sort children by name
    std::sort(node->children.begin(), node->children.end(), [](const std::unique_ptr<NamespaceNode>& a, const std::unique_ptr<NamespaceNode>& b) { return a->name < b->name; });

    // Sort class indices by class name (using cached display names)
    std::sort(node->classIndices.begin(), node->classIndices.end(), [this](size_t a, size_t b) {
        if (a >= m_classDisplayNames.size() || b >= m_classDisplayNames.size()) return a < b;
        return m_classDisplayNames[a] < m_classDisplayNames[b];
    });

    // Recurse into children
    for (auto& child : node->children) { SortTreeRecursive(child.get()); }
}

void ClassBrowserPanel::LoadClasses() {
    m_isLoading = true;
    m_classes.clear();
    m_classDisplayNames.clear();
    m_classFullNames.clear();
    m_classIsEnum.clear();
    m_classNamesLower.clear();
    m_classFullNamesLower.clear();
    m_classFilterVisible.clear();
    m_lastAppliedFilter.clear();
    m_rootNode.reset();
    m_pathToNode.clear();
    m_selectedIndex = SIZE_MAX;
    m_selectionChanged = false;
    m_classesJustLoaded = false;

    if (m_selectedAssemblyImage) {
        const char* asmName = MonoBridge::GetImageName(m_selectedAssemblyImage);
        LOG_INFO("[ClassBrowser] Loading from assembly: " + std::string(asmName ? asmName : "???"));
        m_classes = ClassBrowser::GetClassesFromImage(m_selectedAssemblyImage, "", 50000);
    } else {
        LOG_INFO("[ClassBrowser] Loading from all assemblies");
        m_classes = ClassBrowser::GetClassesInNamespace("", 50000);
    }

    LOG_INFO("[ClassBrowser] Loaded " + std::to_string(m_classes.size()) + " classes");

    // Pre-compute all display strings once, eliminates per-frame Mono calls
    size_t count = m_classes.size();
    m_classDisplayNames.reserve(count);
    m_classFullNames.reserve(count);
    m_classIsEnum.reserve(count);
    m_classNamesLower.reserve(count);
    m_classFullNamesLower.reserve(count);

    for (const auto& klass : m_classes) {
        std::string name = klass.Name();
        std::string fullName = klass.FullName();

        // Display name: "+Name" for nested, "Name" otherwise
        m_classDisplayNames.push_back(klass.IsNested() ? "+" + name : name);
        m_classFullNames.push_back(fullName);
        m_classIsEnum.push_back(klass.IsEnum());

        // Lowercase copies for filter matching
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        std::transform(fullName.begin(), fullName.end(), fullName.begin(), ::tolower);
        m_classNamesLower.push_back(std::move(name));
        m_classFullNamesLower.push_back(std::move(fullName));
    }

    // Build namespace tree
    if (!m_classes.empty()) { BuildNamespaceTree(); }

    // Initialize filter visibility (all visible, no filter applied yet)
    RecomputeFilterVisibility(m_cachedSearchFilterLower);

    m_isLoading = false;
    m_classesJustLoaded = true;
}

bool ClassBrowserPanel::NavigateToClass(const std::string& fullClassName) {
    // Search for class by full name (using cached names)
    for (size_t i = 0; i < m_classFullNames.size(); i++) {
        if (m_classFullNames[i] == fullClassName) {
            m_selectedIndex = i;
            m_selectionChanged = true;

            // Set search filter to help user see it in the tree
            strncpy_s(m_searchFilter, fullClassName.c_str(), sizeof(m_searchFilter) - 1);
            m_cachedSearchFilterLower = fullClassName;
            std::transform(m_cachedSearchFilterLower.begin(), m_cachedSearchFilterLower.end(), m_cachedSearchFilterLower.begin(), ::tolower);

            LOG_INFO("[ClassBrowser] Navigated to: " + fullClassName);
            return true;
        }
    }

    LOG_WARN("[ClassBrowser] Class not found for navigation: " + fullClassName);
    return false;
}

void ClassBrowserPanel::RefreshAssemblyList() {
    m_assemblyList.clear();
    m_assemblyList.push_back("All Assemblies");

    auto& bridge = MonoBridge::Get();
    if (!bridge.IsRuntimeReady()) return;

    if (!bridge.AreImagesInitialized()) { bridge.InitializeAllImages(); }

    for (void* image : bridge.GetAllImages()) {
        if (!image) continue;
        const char* name = MonoBridge::GetImageName(image);
        if (name) {
            int typeCount = bridge.GetTypeDefCount(image);
            std::string entry = std::string(name) + " (" + std::to_string(typeCount) + ")";
            m_assemblyList.push_back(entry);
        }
    }

    LOG_INFO("[ClassBrowser] Refreshed assembly list: " + std::to_string(m_assemblyList.size() - 1) + " assemblies");
}

void ClassBrowserPanel::SelectAssembly(void* image) {
    // Ensure assembly list is populated
    if (m_assemblyList.empty()) { RefreshAssemblyList(); }

    // Find the assembly in the list
    const auto& images = MonoBridge::Get().GetAllImages();
    for (size_t i = 0; i < images.size(); i++) {
        if (images[i] == image) {
            m_selectedAssembly = (int)(i + 1); // +1 because 0 is "All Assemblies"
            m_selectedAssemblyImage = image;

            LoadClasses();

            LOG_INFO("[ClassBrowser] Selected assembly: " + m_assemblyList[m_selectedAssembly]);
            return;
        }
    }

    LOG_WARN("[ClassBrowser] Assembly not found in list");
}

} // namespace S3MM
