#pragma once
#include <string>
#include <vector>
#include <memory>

namespace S3MM {

// Forward declarations
class ClassBrowserPanel;
class MethodPanel;
class FieldPanel;
class PropertyPanel;
class SingletonPanel;
class SettingsPanel;
class ObjectExplorerPanel;
class ProfilerPanel;
class ModsPanel;
class XRefPanel;
class InspectorPanel;
class ResourceBrowserPanel;

// MAIN WINDOW - Top-level UI container
class MainWindow {
  public:
    static MainWindow& Get();

    void Initialize();
    void Shutdown();

    // Called every frame by ImGuiManager
    void Render();

    // Access to panels (for inter-panel communication)
    ClassBrowserPanel* GetClassBrowser() { return m_classBrowser.get(); }
    MethodPanel* GetMethodPanel() { return m_methodPanel.get(); }
    FieldPanel* GetFieldPanel() { return m_fieldPanel.get(); }
    PropertyPanel* GetPropertyPanel() { return m_propertyPanel.get(); }
    SingletonPanel* GetSingletonPanel() { return m_singletonPanel.get(); }
    const std::vector<std::unique_ptr<ObjectExplorerPanel>>& GetObjectExplorers() const { return m_objectExplorers; }
    XRefPanel* GetXRefPanel() { return m_xrefPanel.get(); }

    // Show the object explorer with a specific object
    void ExploreObject(void* object, const std::string& label = "");

    // Navigate to a hooked method (from mods panel)
    void NavigateToHook(const std::string& className, const std::string& methodName);

    // Browse an assembly in the class browser
    void BrowseAssembly(void* image);

    // Profiler filter integration (Phase 3)
    void OpenProfilerForMethod(const std::string& methodFullName);
    void OpenProfilerForClass(const std::string& classFullName);

  private:
    MainWindow() = default;

    // Panels
    std::unique_ptr<ClassBrowserPanel> m_classBrowser;
    std::unique_ptr<MethodPanel> m_methodPanel;
    std::unique_ptr<FieldPanel> m_fieldPanel;
    std::unique_ptr<PropertyPanel> m_propertyPanel;
    std::unique_ptr<SingletonPanel> m_singletonPanel;
    std::unique_ptr<SettingsPanel> m_settingsPanel;
    std::vector<std::unique_ptr<ObjectExplorerPanel>> m_objectExplorers;
    int m_nextExplorerId = 0;
    std::unique_ptr<ProfilerPanel> m_profilerPanel;
    std::unique_ptr<ModsPanel> m_modsPanel;
    std::unique_ptr<XRefPanel> m_xrefPanel;
    std::unique_ptr<InspectorPanel> m_inspectorPanel;
    std::unique_ptr<ResourceBrowserPanel> m_resourceBrowser;

    // Window visibility state
    bool m_showMods = true;       // Mods Hub - visible by default
    bool m_showInspector = false; // Developer Inspector
    bool m_showProfiler = false;
    bool m_showXRef = false; // IL References panel
    bool m_showResourceBrowser = false;
};

} // namespace S3MM
