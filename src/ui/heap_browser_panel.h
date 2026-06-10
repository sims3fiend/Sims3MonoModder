#pragma once
#include "../core/mono_types.h"
#include <string>
#include <vector>
#include <map>

namespace S3MM {

// HEAP BROWSER PANEL - Enumerates live managed objects, grouped by type with namespace tree
class HeapBrowserPanel {
  public:
    void Render();
    void Clear();
    void ScanHeap(); // Manual trigger to scan heap

    // Get scan statistics
    size_t GetObjectCount() const { return m_allObjects.size(); }
    size_t GetTypeCount() const { return m_typeToIndices.size(); }

  private:
    // Represents one object found in the heap
    struct HeapObject {
        uintptr_t address;
        void* klass;
        std::string typeName;      // "Namespace.ClassName"
        std::string typeNameLower; // For filtering
    };

    // Tree node for namespace hierarchy
    struct NamespaceNode {
        std::string name;                              // This segment (e.g., "Gameplay")
        std::map<std::string, NamespaceNode> children; // Child namespaces
        std::vector<std::string> types;                // Leaf type names at this level
        size_t totalCount = 0;                         // Total objects in this subtree
    };

    // Rendering
    void RenderTypeTree();
    void RenderInstanceList();
    void RenderNamespaceNode(const std::string& path, NamespaceNode& node);

    // Data building
    void BuildTypeIndex();
    void BuildNamespaceTree();
    void AddToNamespaceTree(const std::string& fullTypeName, size_t count);

    // All collected objects
    std::vector<HeapObject> m_allObjects;

    // Type name -> indices into m_allObjects
    std::map<std::string, std::vector<size_t>> m_typeToIndices;

    // Namespace tree root
    NamespaceNode m_rootNode;

    // UI state
    std::string m_selectedType;           // Currently selected type (full name)
    size_t m_selectedInstance = SIZE_MAX; // Index into the type's instance list
    char m_typeFilter[256] = "";
    std::string m_cachedFilterLower;
    bool m_isScanning = false;

    // Scan timing
    float m_lastScanTime = 0.0f;
};

} // namespace S3MM
