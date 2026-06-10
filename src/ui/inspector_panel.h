#pragma once
#include <string>

namespace S3MM {

// Forward declarations
class ClassBrowserPanel;
class MethodPanel;
class FieldPanel;
class PropertyPanel;
class SingletonPanel;
// INSPECTOR PANEL - Class browser + tabbed detail panels, with optional undock
class InspectorPanel {
  public:
    void Initialize(ClassBrowserPanel* classBrowser, MethodPanel* methods, FieldPanel* fields, PropertyPanel* properties, SingletonPanel* singletons);

    // Main render (top half + splitter + bottom half when docked)
    void Render();

    // Bottom half render (called by MainWindow when undocked)
    void RenderBottomHalf();

    // Undock state
    bool IsBottomUndocked() const { return m_bottomUndocked; }
    void SetBottomDocked() { m_bottomUndocked = false; }

    // Class selection notification
    void OnClassSelected(void* klass, const std::string& className);

  private:
    void RenderTopHalf();
    void RenderBottomTabs();
    void RenderSplitter(float availHeight);

    // Non-owning panel pointers (owned by MainWindow)
    ClassBrowserPanel* m_classBrowser = nullptr;
    MethodPanel* m_methods = nullptr;
    FieldPanel* m_fields = nullptr;
    PropertyPanel* m_properties = nullptr;
    SingletonPanel* m_singletons = nullptr;

    // Layout
    float m_splitRatio = 0.45f;
    bool m_bottomUndocked = false;

    // Class selection state
    bool m_hasClassSelected = false;
    std::string m_selectedClassName;
};

} // namespace S3MM
