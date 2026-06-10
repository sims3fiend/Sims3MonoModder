#pragma once
#include "../core/mono_types.h"
#include "../core/mono_class.h"
#include "field_rendering.h"
#include <string>
#include <vector>

namespace S3MM {

// SINGLETON PANEL - Shows instance fields for the selected class's singleton
class SingletonPanel {
  public:
    void Render();

    // Set the class to inspect (called when class selection changes)
    void SetClass(void* klassHandle, const std::string& className);
    void Clear();

    // Check if current class has a valid singleton
    bool HasInstance() const { return m_instance != nullptr; }

  private:
    void LoadInstanceFields();
    void RefreshInstanceFieldValues();
    void RenderInstanceFields();
    void RenderArrayContents(void* instance, const FieldInfo& field);
    void RenderListContents(void* instance, const FieldInfo& field); // For List<T> types
    void RenderStructContents(void* instance, const FieldInfo& field);
    void ApplyFieldEdit();
    void ApplyStructFieldEdit(void* addr, const FieldInfo& subField);

    // Current class info
    void* m_klassHandle = nullptr;
    std::string m_className;
    std::string m_singletonFieldName;
    void* m_instance = nullptr;

    // Instance fields
    std::vector<FieldInfo> m_instanceFields;
    std::vector<std::string> m_cachedValues;
    size_t m_selectedFieldIndex = SIZE_MAX;
    char m_fieldFilter[128] = "";

    // Field editing
    EditState m_editField;

    // Array expansion state
    bool m_showArrayContents = false;
    int m_arrayDisplayOffset = 0;
    static constexpr int MAX_ARRAY_DISPLAY = 50;

    // Struct expansion state - supports nested navigation
    std::vector<StructContext> m_structStack;
    size_t m_selectedStructFieldIndex = SIZE_MAX;

    // Struct field editing
    EditState m_editStruct;

    // Helper to get current struct context
    StructContext* CurrentStructContext() { return m_structStack.empty() ? nullptr : &m_structStack.back(); }
    void PushStructContext(uintptr_t baseAddr, void* klass, const std::string& name);
    void PopStructContext();
};

} // namespace S3MM
