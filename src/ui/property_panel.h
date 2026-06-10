#pragma once
#include "../core/mono_types.h"
#include <string>
#include <vector>

namespace S3MM {

// PROPERTY PANEL - Display and invoke properties on selected class
class PropertyPanel {
  public:
    void Render();

    // Set the class to display properties for
    void SetClass(void* klass, const std::string& className);

  private:
    void RenderPropertyList();
    void RenderPropertyDetails();
    void RefreshPropertyValues();
    std::string GetPropertyValue(const PropertyInfo& prop);
    void ApplyPropertyEdit();

    // Invoke getter/setter
    bool InvokeGetter(const PropertyInfo& prop, void* instance, MonoInvokeState** outState);
    bool InvokeSetter(const PropertyInfo& prop, void* instance, void* value);

    // Current class
    void* m_klass = nullptr;
    void* m_vtable = nullptr;
    std::string m_className;
    std::vector<PropertyInfo> m_properties;

    // Cached string values
    std::vector<std::string> m_cachedValues;

    // Selection
    size_t m_selectedIndex = SIZE_MAX;
    char m_filterText[256] = "";
    std::string m_cachedFilterLower;
    bool m_showStaticOnly = true;

    // Edit values
    int32_t m_editInt = 0;
    int64_t m_editInt64 = 0;
    double m_editDouble = 0.0;
    bool m_editBool = false;
    char m_editString[256] = "";
    int m_editEnumIndex = 0;

    // Reusable invoke state (allocated once, reused for all property reads)
    MonoInvokeState* m_invokeState = nullptr;
};

} // namespace S3MM
