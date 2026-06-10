#pragma once
#include "../core/resource_system.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace S3MM {

// RESOURCE BROWSER PANEL - Browse registered databases (packages) and their resources
class ResourceBrowserPanel {
  public:
    void Render();

    // Signals for main window
    bool HasSelectionChanged() const { return m_selectionChanged; }
    void ClearSelectionChanged() { m_selectionChanged = false; }
    const DatabaseInfo* GetSelectedDatabase() const { return m_selectedDbIdx < m_databases.size() ? &m_databases[m_selectedDbIdx] : nullptr; }

  private:
    void LoadDatabases();
    void RenderDatabaseList();
    void RenderDatabaseNode(size_t dbIdx);
    void RenderDetailPane();
    void LoadTGIsForDatabase(size_t dbIdx);
    void BuildTGIDisplayStrings();
    void ApplyTGIFilter();

    // Database list
    std::vector<DatabaseInfo> m_databases;
    bool m_loaded = false;
    bool m_loading = false;

    // Selection
    size_t m_selectedDbIdx = SIZE_MAX;
    bool m_selectionChanged = false;

    // Filter
    char m_searchFilter[256] = "";
    std::string m_cachedFilterLower;

    // Pre-computed display strings
    std::vector<std::string> m_dbDisplayNames;
    std::vector<std::string> m_dbDisplayNamesLower;

    // Per-database TGI cache
    std::vector<std::vector<PackageKeyInfo>> m_dbPackageKeys; // For PackedFile/PackedStream
    std::vector<std::vector<ResourceKey>> m_dbDDFKeys;        // For DDF
    std::vector<bool> m_dbKeysLoaded;

    // Pre-computed TGI display strings (per selected db)
    std::vector<std::string> m_tgiDisplayStrings;
    std::vector<std::string> m_tgiDisplayStringsLower;

    // TGI filter
    char m_tgiFilter[256] = "";
    std::string m_cachedTgiFilterLower;
    std::vector<size_t> m_filteredTgiIndices; // Indices into display strings passing filter

    // TGI state
    size_t m_loadedDbIdx = SIZE_MAX; // Which db's keys are currently loaded
    int m_selectedTgiIdx = -1;

    // Name Map (instance ID -> name)
    std::unordered_map<uint64_t, std::string> m_nameMap;

    // Extract status
    std::string m_extractStatus;
    float m_extractStatusTimer = 0.0f;

    // UI state
    bool m_showDetails = true;
    float m_splitRatio = 0.55f;
};

} // namespace S3MM
