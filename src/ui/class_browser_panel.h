#pragma once
#include "../core/mono_class.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <algorithm>

namespace S3MM {

// NAMESPACE TREE NODE - Represents a node in the namespace hierarchy
struct NamespaceNode {
    std::string name;          // Just this segment, e.g. "Gameplay"
    std::string fullPath;      // Full path, e.g. "Sims3.Gameplay"
    std::string fullPathLower; // Pre-computed lowercase for filter matching
    bool filterVisible = true; // Cached: does this node pass the current filter?

    std::vector<std::unique_ptr<NamespaceNode>> children;
    std::vector<size_t> classIndices; // Indices into m_classes for classes in THIS namespace

    NamespaceNode() = default;
    NamespaceNode(const std::string& n, const std::string& fp) : name(n), fullPath(fp) {
        fullPathLower = fp;
        std::transform(fullPathLower.begin(), fullPathLower.end(), fullPathLower.begin(), ::tolower);
    }
};

// CLASS BROWSER PANEL - Browse and select Mono classes with tree view
class ClassBrowserPanel {
  public:
    void Render();

    // Get currently selected class (for other panels)
    const MonoClass* GetSelectedClass() const { return m_selectedIndex < m_classes.size() ? &m_classes[m_selectedIndex] : nullptr; }

    // Signals for other panels
    bool HasSelectionChanged() const { return m_selectionChanged; }
    void ClearSelectionChanged() { m_selectionChanged = false; }

    // Get all loaded classes (for singleton detection)
    const std::vector<MonoClass>& GetLoadedClasses() const { return m_classes; }
    bool HasClassesLoaded() const { return !m_classes.empty(); }
    bool ClassesJustLoaded() const { return m_classesJustLoaded; }
    void ClearClassesJustLoaded() { m_classesJustLoaded = false; }

    // Navigation - select a class by full name (e.g., "Sims3.Gameplay.Sim")
    bool NavigateToClass(const std::string& fullClassName);

    // Select and load a specific assembly by image pointer
    void SelectAssembly(void* image);

  private:
    void LoadClasses();

    // Tree building and rendering
    void BuildNamespaceTree();
    void RenderNamespaceNode(NamespaceNode* node, const std::string& filter);
    void RenderClassWithNested(size_t classIdx, const std::string& filter, int depth = 0);
    void SortTreeRecursive(NamespaceNode* node);
    void RecomputeFilterVisibility(const std::string& filter);
    bool RecomputeNodeVisibility(NamespaceNode* node, const std::string& filter);
    bool RecomputeClassVisibility(size_t classIdx, const std::string& filter);

    // Assembly filter
    void RefreshAssemblyList();
    std::vector<std::string> m_assemblyList; // Cached assembly names
    int m_selectedAssembly = 0;              // 0 = All, 1+ = specific assembly
    void* m_selectedAssemblyImage = nullptr; // MonoImage* for selected assembly

    // Search filter
    char m_searchFilter[256] = "";
    std::string m_cachedSearchFilterLower;  // Cached lowercase filter
    std::string m_lastAppliedFilter;        // Filter used for cached visibility
    std::vector<bool> m_classFilterVisible; // Per-class cached filter visibility

    // Loaded classes
    std::vector<MonoClass> m_classes;
    std::vector<std::string> m_classDisplayNames;   // Pre-computed display names ("+Name" for nested)
    std::vector<std::string> m_classFullNames;      // Pre-computed full names
    std::vector<bool> m_classIsEnum;                // Pre-computed enum flags
    std::vector<std::string> m_classNamesLower;     // Pre-computed lowercase names for filtering
    std::vector<std::string> m_classFullNamesLower; // Pre-computed lowercase full names
    size_t m_selectedIndex = SIZE_MAX;
    bool m_selectionChanged = false;

    // Namespace tree
    std::unique_ptr<NamespaceNode> m_rootNode;
    std::unordered_map<std::string, NamespaceNode*> m_pathToNode;

    // Nested class hierarchy: maps parent class index -> nested class indices
    std::unordered_map<size_t, std::vector<size_t>> m_nestedChildren;

    // State
    bool m_isLoading = false;
    bool m_classesJustLoaded = false;
};

} // namespace S3MM
