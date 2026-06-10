#pragma once
#include "../core/mono_types.h"
#include "field_rendering.h"
#include <string>
#include <vector>

namespace S3MM {

// FIELD PANEL - Display and edit fields on selected class
class FieldPanel {
  public:
    void Render();

    // Set the class to display fields for
    void SetClass(void* klass, const std::string& className);

  private:
    void RenderFieldList();
    void RenderFieldDetails();
    void RenderArrayContents(const FieldInfo& field);
    void RenderListContents(const FieldInfo& field);   // For List<T> types
    void RenderStructContents(const FieldInfo& field); // For struct types
    void ApplyArrayElementEdit(const FieldInfo& field, void* arrPtr, int index);
    void RefreshFieldValues();
    std::string GetFieldValue(const FieldInfo& field);
    void ApplyFieldEdit();
    void ApplyStructFieldEdit(void* addr, const FieldInfo& subField);

    // Struct navigation helpers
    void PushStructContext(uintptr_t baseAddr, void* klass, const std::string& name);
    void PopStructContext();
    StructContext* CurrentStructContext() { return m_structStack.empty() ? nullptr : &m_structStack.back(); }

    // Current class
    void* m_klass = nullptr;
    void* m_vtable = nullptr;
    std::string m_className;
    std::vector<FieldInfo> m_fields;

    // Cached string values (avoid reading every frame)
    std::vector<std::string> m_cachedValues;

    // Selection
    size_t m_selectedIndex = SIZE_MAX;
    char m_filterText[256] = "";
    std::string m_cachedFilterLower; // Cached lowercase filter
    bool m_showStaticOnly = true;
    bool m_showInherited = false; // Show fields from parent classes

    // Edit values
    EditState m_editField;

    // Array expansion state
    bool m_showArrayContents = false;
    int m_arrayDisplayOffset = 0;
    static constexpr int MAX_ARRAY_DISPLAY = 50;

    // Array element editing
    int m_selectedArrayIndex = -1; // Currently selected element index (-1 = none)
    EditState m_editArray;

    // Struct navigation state
    std::vector<StructContext> m_structStack;
    size_t m_selectedStructFieldIndex = SIZE_MAX;

    // Struct field editing
    EditState m_editStruct;
};

} // namespace S3MM
