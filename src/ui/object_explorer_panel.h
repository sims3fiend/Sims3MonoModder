#pragma once
#include "../core/mono_types.h"
#include "../core/mono_class.h"
#include "field_rendering.h"
#include <string>
#include <vector>

namespace S3MM {

// OBJECT EXPLORER PANEL - Displays instance fields for any object, with navigation history
class ObjectExplorerPanel {
  public:
    void Render();

    // Navigate to an object - pushes onto history stack
    void ExploreObject(void* object, const std::string& label = "");

    // Clear the explorer
    void Clear();

    // Check if we have an object to display
    bool HasObject() const { return !m_historyStack.empty(); }

    // Get the current object being explored (for external reference)
    void* GetCurrentObject() const;

    // Window management (for multi-window explorer)
    void SetWindowId(int id) { m_windowId = id; }
    int GetWindowId() const { return m_windowId; }
    std::string GetWindowTitle() const;
    bool IsOpen() const { return m_open; }
    void SetOpen(bool open) { m_open = open; }

  private:
    // Represents one object in the navigation history
    struct ObjectContext {
        void* object;                  // The object pointer
        void* klass;                   // MonoClass* of this object
        std::string className;         // Full class name
        std::string label;             // User-friendly label (e.g., field name that led here)
        std::vector<FieldInfo> fields; // Instance fields
    };

    void LoadObjectFields(ObjectContext& ctx);
    void RefreshFieldValues();
    void RenderObjectFields();
    void RenderArrayContents(const FieldInfo& field);
    void RenderListContents(const FieldInfo& field); // For List<T> types
    void RenderStructContents(const FieldInfo& field);
    void ApplyFieldEdit();
    void ApplyStructFieldEdit(void* addr, const FieldInfo& subField);

    // Navigation
    void NavigateBack();
    ObjectContext* CurrentContext();

    // Navigation history stack
    std::vector<ObjectContext> m_historyStack;

    // Cached field values for display
    std::vector<std::string> m_cachedValues;
    std::vector<std::string> m_cachedDisplayStrings; // Pre-built "name = value" strings
    size_t m_selectedFieldIndex = SIZE_MAX;
    char m_fieldFilter[128] = "";
    std::string m_cachedFilterLower; // Cached lowercase filter to avoid repeated transforms

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

    // Helper methods for struct navigation
    StructContext* CurrentStructContext() { return m_structStack.empty() ? nullptr : &m_structStack.back(); }
    void PushStructContext(uintptr_t baseAddr, void* klass, const std::string& name);
    void PopStructContext();

    // Window state
    int m_windowId = 0;
    bool m_open = true;
};

} // namespace S3MM
