#pragma once
#include "../modding/mod_loader.h"
#include "../modding/hook_manager.h"
#include "../modding/assembly_diff.h"
#include "../util/mod_log.h"
#include <string>
#include <vector>
#include <set>
#include <map>

namespace S3MM {

// Forward declarations
class SettingsPanel;

// MODS PANEL - Tabbed hub with Mods, Logs, Settings and init status bar
class ModsPanel {
  public:
    void Render();

    // Settings panel injection (non-owning, set by MainWindow)
    void SetSettingsPanel(SettingsPanel* panel) { m_settingsPanel = panel; }

  private:
    // Hub tabs
    void RenderModsTab();
    void RenderLogsTab();
    void RenderSettingsTab();
    void RenderStatusBar();

    // Mods sub-content
    void RenderModsSection();
    void RenderModEntry(const LoadedMod& mod);
    void RenderHooksForMod(const std::string& modName);

    // Detected external mods (NRaas, etc.)
    void RenderDetectedMods();
    void RefreshDetectedMods();

    // Bottom section: Logs
    void RenderLogsSection();

    // Actions
    void ScanForNewMods();

    // State - Mods section
    std::set<std::string> m_expandedMods; // Which mods are expanded in tree

    // State - Detected mods
    struct DetectedMod {
        std::string name;
        int typeCount;
        void* image;                                       // MonoImage*
        std::string packagePath;                           // Source .package file
        std::vector<std::string> overriddenCoreAssemblies; // Core DLLs this package overrides
    };
    std::vector<DetectedMod> m_detectedMods;
    bool m_detectedModsLoaded = false;
    std::string m_selectedModForContext; // For right-click context menu

    // State - Overrides viewer window
    void RenderOverridesWindow();
    bool m_showOverridesWindow = false;
    std::string m_overridesModName;
    std::string m_overridesPackagePath;
    std::vector<std::string> m_overridesCoreAssemblies;

    // Cached diff results (assembly name -> diff result)
    struct CachedDiff {
        std::vector<std::string> modifiedMethods;
        std::vector<std::string> addedMethods;
        std::vector<std::string> removedMethods;
        bool computed = false;
    };
    std::map<std::string, CachedDiff> m_cachedDiffs;

    // State - Logs section
    char m_logFilter[128] = "";
    std::string m_sourceFilter;              // Empty = all sources
    std::vector<std::string> m_knownSources; // For dropdown
    bool m_autoScroll = true;
    size_t m_lastLogVersion = 0;
    std::vector<ModLogEntry> m_cachedLogs;

    // Split ratio (0.0 - 1.0, portion for mods section)
    float m_splitRatio = 0.5f;

    // Settings panel (non-owning)
    SettingsPanel* m_settingsPanel = nullptr;
};

} // namespace S3MM
