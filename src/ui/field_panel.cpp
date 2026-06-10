#include "field_panel.h"
#include "list_helpers.h"
#include "main_window.h"
#include "class_browser_panel.h"
#include "object_explorer_panel.h"
#include "../core/mono_bridge.h"
#include "../core/mono_class.h"
#include "../util/logger.h"
#include "themes.h"
#include "imgui.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace S3MM {

void FieldPanel::Render() {
    // Class selection is now handled by MainWindow before tabs render

    if (!m_klass) {
        ImGui::TextDisabled("Select a class from the Class Browser tab");
        return;
    }

    ImGui::Text("Class: %s", m_className.c_str());
    ImGui::Text("%zu fields", m_fields.size());

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) { RefreshFieldValues(); }

    ImGui::SameLine();
    ImGui::Checkbox("Static only", &m_showStaticOnly);

    ImGui::SameLine();
    if (ImGui::Checkbox("Inherited", &m_showInherited)) {
        // Reload fields when checkbox changes
        MonoClass mc(m_klass);
        m_fields = mc.GetAllFields(m_showInherited);
        m_cachedValues.resize(m_fields.size());
        RefreshFieldValues();
        m_selectedIndex = SIZE_MAX;
        LOG_INFO("[FieldPanel] Reloaded " + std::to_string(m_fields.size()) + " fields (inherited=" + (m_showInherited ? "true" : "false") + ")");
    }

    ImGui::Separator();

    // Split view
    float listWidth = ImGui::GetContentRegionAvail().x * 0.5f;

    ImGui::BeginChild("FieldList", ImVec2(listWidth, -1), true);
    RenderFieldList();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("FieldDetails", ImVec2(0, -1), true);
    RenderFieldDetails();
    ImGui::EndChild();
}

void FieldPanel::SetClass(void* klass, const std::string& className) {
    if (m_klass == klass) return;

    m_klass = klass;
    m_className = className;
    m_selectedIndex = SIZE_MAX;

    // Reset struct navigation state
    m_structStack.clear();
    m_selectedStructFieldIndex = SIZE_MAX;

    // Reset array state
    m_showArrayContents = false;
    m_arrayDisplayOffset = 0;
    m_selectedArrayIndex = -1;

    // Get vtable for static field access
    m_vtable = MonoBridge::Get().GetClassVTable(klass);

    // Load fields
    MonoClass mc(klass);
    m_fields = mc.GetAllFields();

    // Initialize cached values
    m_cachedValues.resize(m_fields.size());
    RefreshFieldValues();

    LOG_INFO("[FieldPanel] Loaded " + std::to_string(m_fields.size()) + " fields from " + className);
}

void FieldPanel::RefreshFieldValues() {
    for (size_t i = 0; i < m_fields.size(); i++) { m_cachedValues[i] = GetFieldValue(m_fields[i]); }
    LOG_DEBUG("[FieldPanel] Refreshed " + std::to_string(m_fields.size()) + " field values");
}

std::string FieldPanel::GetFieldValue(const FieldInfo& field) {
    if (!m_vtable || !field.isStatic) return "<n/a>";

    auto& bridge = MonoBridge::Get();
    std::ostringstream ss;

    switch (field.type) {
    case MonoTypeCode::Boolean: {
        int64_t val = 0; // Use 8-byte buffer for safety
        bridge.GetStaticFieldValue(m_vtable, field.handle, &val);
        return val ? "true" : "false";
    }

    case MonoTypeCode::I1:
    case MonoTypeCode::I2:
    case MonoTypeCode::I4: {
        int64_t val = 0; // Use 8-byte buffer for safety
        bridge.GetStaticFieldValue(m_vtable, field.handle, &val);
        int32_t val32 = (int32_t)val;
        if (field.isEnum) {
            for (const auto& ev : field.enumValues) {
                if (ev.value == val32) {
                    ss << ev.name << " (" << val32 << ")";
                    return ss.str();
                }
            }
        }
        ss << val32;
        return ss.str();
    }

    case MonoTypeCode::U1:
    case MonoTypeCode::U2:
    case MonoTypeCode::U4: {
        uint64_t val = 0; // Use 8-byte buffer for safety
        bridge.GetStaticFieldValue(m_vtable, field.handle, &val);
        ss << (uint32_t)val;
        return ss.str();
    }

    case MonoTypeCode::I8: {
        int64_t val = 0;
        bridge.GetStaticFieldValue(m_vtable, field.handle, &val);
        ss << val;
        return ss.str();
    }

    case MonoTypeCode::U8: {
        uint64_t val = 0;
        bridge.GetStaticFieldValue(m_vtable, field.handle, &val);
        ss << val;
        return ss.str();
    }

    case MonoTypeCode::R4: {
        float val = 0.0f;
        bridge.GetStaticFieldValue(m_vtable, field.handle, &val);
        ss << std::fixed << std::setprecision(4) << val;
        return ss.str();
    }

    case MonoTypeCode::R8: {
        double val = 0.0;
        bridge.GetStaticFieldValue(m_vtable, field.handle, &val);
        ss << std::fixed << std::setprecision(6) << val;
        return ss.str();
    }

    case MonoTypeCode::String: {
        void* strPtr = nullptr;
        bridge.GetStaticFieldValue(m_vtable, field.handle, &strPtr);
        if (strPtr && IsValidPointer(strPtr)) { return "\"" + bridge.ExtractString(strPtr) + "\""; }
        return "<null>";
    }

    case MonoTypeCode::ValueType: {
        if (field.isEnum) {
            int64_t val = 0; // Use 8-byte buffer for safety
            bridge.GetStaticFieldValue(m_vtable, field.handle, &val);
            int32_t val32 = (int32_t)val;
            for (const auto& ev : field.enumValues) {
                if (ev.value == val32) {
                    ss << ev.name << " (" << val32 << ")";
                    return ss.str();
                }
            }
            ss << val32;
            return ss.str();
        }
        return "<struct>";
    }

    case MonoTypeCode::Class:
    case MonoTypeCode::Object:
    case MonoTypeCode::GenericInst: {
        void* objPtr = nullptr;
        bridge.GetStaticFieldValue(m_vtable, field.handle, &objPtr);
        if (objPtr && IsValidPointer(objPtr)) {
            // Check if it's a List<T>
            void* objClass = MonoBridge::GetObjectClass(objPtr);
            if (objClass && IsListType(objClass)) {
                void* itemsArray = nullptr;
                int32_t count = 0;
                if (GetListContents(objPtr, &itemsArray, &count)) {
                    void* arrClass = MonoBridge::GetObjectClass(itemsArray);
                    MonoTypeCode elemType = MonoBridge::GetArrayElementType(arrClass);
                    ss << "List<" << GetTypeName(elemType) << ">[" << count << "]";
                    return ss.str();
                }
                ss << "List<?>[?]";
                return ss.str();
            }

            ss << "obj@0x" << std::hex << (uintptr_t)objPtr;
            return ss.str();
        }
        return "<null>";
    }

    case MonoTypeCode::SzArray: {
        void* arrPtr = nullptr;
        bridge.GetStaticFieldValue(m_vtable, field.handle, &arrPtr);
        if (arrPtr && IsValidPointer(arrPtr)) {
            int32_t len = bridge.GetArrayLength(arrPtr);
            ss << GetTypeName(field.arrayElementType) << "[" << len << "]";
            return ss.str();
        }
        return "<null>";
    }

    default:
        return "<unsupported>";
    }
}

void FieldPanel::RenderFieldList() {
    // Filter - only recompute lowercase when filter text changes
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##filter", "Filter fields...", m_filterText, sizeof(m_filterText))) {
        m_cachedFilterLower = m_filterText;
        std::transform(m_cachedFilterLower.begin(), m_cachedFilterLower.end(), m_cachedFilterLower.begin(), ::tolower);
    }

    ImGui::BeginChild("FieldScroll", ImVec2(-1, -1));

    for (size_t i = 0; i < m_fields.size(); i++) {
        const FieldInfo& f = m_fields[i];

        // Filter by static
        if (m_showStaticOnly && !f.isStatic) continue;

        // Apply text filter using pre-computed lowercase names
        if (!m_cachedFilterLower.empty()) {
            if (f.nameLower.find(m_cachedFilterLower) == std::string::npos) continue;
        }

        // Color: green for editable static, gray for const/instance, dimmed for inherited
        ImVec4 color;
        if (f.isLiteral) {
            color = Colors::Desaturate(Colors::Get().accent);
        } else if (f.isStatic) {
            color = Colors::Get().positive;
        } else {
            color = Colors::Get().accent;
        }

        // Dim inherited fields slightly
        if (f.isInherited) {
            color.x *= 0.7f;
            color.y *= 0.7f;
            color.z *= 0.7f;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);

        bool isSelected = (m_selectedIndex == i);
        std::string display = f.name;
        if (f.isInherited && !f.declaringClassName.empty()) { display += " (from " + f.declaringClassName + ")"; }
        display += " = " + (i < m_cachedValues.size() ? m_cachedValues[i] : "?");

        if (ImGui::Selectable(display.c_str(), isSelected)) {
            m_selectedIndex = i;

            // Load value into edit controls
            if (f.isStatic && m_vtable) {
                auto& bridge = MonoBridge::Get();
                // Reset array display state
                m_showArrayContents = false;
                m_arrayDisplayOffset = 0;
                m_selectedArrayIndex = -1;

                // Reset struct display state
                m_structStack.clear();
                m_selectedStructFieldIndex = SIZE_MAX;

                // Use 8-byte buffer for all reads to prevent stack corruption
                int64_t valBuf = 0;
                switch (f.type) {
                case MonoTypeCode::Boolean: {
                    bridge.GetStaticFieldValue(m_vtable, f.handle, &valBuf);
                    m_editField.editBool = valBuf != 0;
                    break;
                }
                case MonoTypeCode::I1:
                case MonoTypeCode::I2:
                case MonoTypeCode::I4:
                case MonoTypeCode::U1:
                case MonoTypeCode::U2:
                case MonoTypeCode::U4: {
                    bridge.GetStaticFieldValue(m_vtable, f.handle, &valBuf);
                    m_editField.editInt = (int32_t)valBuf;
                    if (f.isEnum) {
                        m_editField.editEnumIndex = 0;
                        for (int j = 0; j < (int)f.enumValues.size(); j++) {
                            if (f.enumValues[j].value == m_editField.editInt) {
                                m_editField.editEnumIndex = j;
                                break;
                            }
                        }
                    }
                    break;
                }
                case MonoTypeCode::I8:
                case MonoTypeCode::U8:
                    bridge.GetStaticFieldValue(m_vtable, f.handle, &m_editField.editInt64);
                    break;
                case MonoTypeCode::R4: {
                    bridge.GetStaticFieldValue(m_vtable, f.handle, &valBuf);
                    float val = *(float*)&valBuf;
                    m_editField.editDouble = (double)val;
                    break;
                }
                case MonoTypeCode::R8:
                    bridge.GetStaticFieldValue(m_vtable, f.handle, &m_editField.editDouble);
                    break;
                case MonoTypeCode::String: {
                    void* strPtr = nullptr;
                    bridge.GetStaticFieldValue(m_vtable, f.handle, &strPtr);
                    if (strPtr && IsValidPointer(strPtr)) {
                        std::string s = bridge.ExtractString(strPtr);
                        strncpy_s(m_editField.editString, s.c_str(), sizeof(m_editField.editString) - 1);
                    } else {
                        m_editField.editString[0] = '\0';
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }

        // Context menu for this field
        {
            std::string ctxId = "field_ctx_" + std::to_string(i);
            if (ImGui::BeginPopupContextItem(ctxId.c_str())) {
                // Explore option for object-type fields
                if (f.isStatic && m_vtable && (f.type == MonoTypeCode::Class || f.type == MonoTypeCode::Object || f.type == MonoTypeCode::GenericInst || f.type == MonoTypeCode::SzArray)) {
                    void* objPtr = nullptr;
                    MonoBridge::Get().GetStaticFieldValue(m_vtable, f.handle, &objPtr);
                    if (objPtr && IsValidPointer(objPtr)) {
                        if (ImGui::MenuItem("Explore")) { MainWindow::Get().ExploreObject(objPtr, f.name); }
                    }
                }
                // Copy type
                if (ImGui::MenuItem("Copy Type")) {
                    std::string typeName = f.isEnum ? "enum" : GetTypeName(f.type);
                    if (f.type == MonoTypeCode::SzArray) { typeName = std::string(GetTypeName(f.arrayElementType)) + "[]"; }
                    ImGui::SetClipboardText(typeName.c_str());
                }
                // Copy name
                if (ImGui::MenuItem("Copy Name")) {
                    std::string fullName = m_className + "." + f.name;
                    ImGui::SetClipboardText(fullName.c_str());
                }
                // Copy address
                if (ImGui::MenuItem("Copy Address")) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "0x%08X", (unsigned int)(uintptr_t)f.handle);
                    ImGui::SetClipboardText(buf);
                }
                ImGui::EndPopup();
            }
        }

        // Tooltip
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s %s", f.isStatic ? "static" : "instance", f.isEnum ? "enum" : GetTypeName(f.type));
            ImGui::Text("%s", f.name.c_str());
            if (f.isLiteral) Colors::Text(Colors::Get().neutral, "const");
            if (f.isInherited && !f.declaringClassName.empty()) { ImGui::TextColored(Colors::Dim(Colors::Get().accent), "inherited from %s", f.declaringClassName.c_str()); }
            ImGui::TextDisabled("Right-click for options");
            ImGui::EndTooltip();
        }

        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
}

void FieldPanel::RenderFieldDetails() {
    if (m_selectedIndex >= m_fields.size()) {
        ImGui::TextDisabled("Select a field from the list");
        return;
    }

    const FieldInfo& f = m_fields[m_selectedIndex];

    // Header
    ImGui::TextColored(Colors::Get().neutral, "%s.%s", m_className.c_str(), f.name.c_str());
    ImGui::Text("Type: %s", f.isEnum ? "enum" : GetTypeName(f.type));
    ImGui::Text("Address: 0x%08X", (uintptr_t)f.handle);

    if (f.isStatic) Colors::Text(Colors::Get().positive, "static");
    if (f.isLiteral) Colors::Text(Colors::Get().neutral, "const (read-only)");
    if (f.isInherited) ImGui::TextColored(Colors::Dim(Colors::Get().accent), "inherited from %s", f.declaringClassName.c_str());

    ImGui::Separator();

    // Current value
    ImGui::Text("Current: %s", m_selectedIndex < m_cachedValues.size() ? m_cachedValues[m_selectedIndex].c_str() : "?");

    // Edit controls (only for editable static fields)
    if (f.isStatic && !f.isLiteral) {
        ImGui::Separator();

        // Special handling for arrays and lists - show contents
        if (f.type == MonoTypeCode::SzArray) {
            RenderArrayContents(f);
        } else if (f.type == MonoTypeCode::Class || f.type == MonoTypeCode::Object || f.type == MonoTypeCode::GenericInst) {
            // Check if it's a List<T>
            void* objPtr = nullptr;
            MonoBridge::Get().GetStaticFieldValue(m_vtable, f.handle, &objPtr);
            if (objPtr && IsValidPointer(objPtr)) {
                void* objClass = MonoBridge::GetObjectClass(objPtr);
                if (objClass && IsListType(objClass)) {
                    RenderListContents(f);
                } else {
                    // Regular object - show Explore button
                    ImGui::Text("Field: %s [%s]", f.name.c_str(), GetTypeName(f.type));
                    ImGui::Text("Value: obj@0x%08X", (uintptr_t)objPtr);
                    if (ImGui::Button("Explore", ImVec2(-1, 25))) { MainWindow::Get().ExploreObject(objPtr, f.name); }
                }
            } else {
                ImGui::Text("Field: %s [%s]", f.name.c_str(), GetTypeName(f.type));
                ImGui::TextDisabled("Value: <null>");
            }
        } else {
            ImGui::Text("New Value:");

            bool canEdit = true;

            switch (f.type) {
            case MonoTypeCode::Boolean:
                ImGui::Checkbox("##edit", &m_editField.editBool);
                break;

            case MonoTypeCode::I1:
            case MonoTypeCode::I2:
            case MonoTypeCode::I4:
            case MonoTypeCode::U1:
            case MonoTypeCode::U2:
            case MonoTypeCode::U4:
                if (f.isEnum && !f.enumValues.empty()) {
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::BeginCombo("##enum", f.enumValues[m_editField.editEnumIndex].name.c_str())) {
                        for (int j = 0; j < (int)f.enumValues.size(); j++) {
                            bool selected = (m_editField.editEnumIndex == j);
                            // Use pre-cached label
                            const char* label = (j < (int)f.enumLabels.size()) ? f.enumLabels[j].c_str() : f.enumValues[j].name.c_str();
                            if (ImGui::Selectable(label, selected)) {
                                m_editField.editEnumIndex = j;
                                m_editField.editInt = f.enumValues[j].value;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                } else {
                    ImGui::SetNextItemWidth(120);
                    ImGui::InputInt("##int", &m_editField.editInt);
                }
                break;

            case MonoTypeCode::I8:
            case MonoTypeCode::U8: {
                ImGui::SetNextItemWidth(180);
                // Use a string buffer for 64-bit input since ImGui doesn't have InputInt64
                char buf[32];
                snprintf(buf, sizeof(buf), "%lld", (long long)m_editField.editInt64);
                if (ImGui::InputText("##int64", buf, sizeof(buf), ImGuiInputTextFlags_CharsDecimal)) { m_editField.editInt64 = _atoi64(buf); }
                break;
            }

            case MonoTypeCode::R4:
            case MonoTypeCode::R8:
                ImGui::SetNextItemWidth(150);
                ImGui::InputDouble("##double", &m_editField.editDouble, 0.0, 0.0, "%.6f");
                break;

            case MonoTypeCode::String:
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("##str", m_editField.editString, sizeof(m_editField.editString));
                break;

            case MonoTypeCode::ValueType:
                if (f.isEnum && !f.enumValues.empty()) {
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::BeginCombo("##enum", f.enumValues[m_editField.editEnumIndex].name.c_str())) {
                        for (int j = 0; j < (int)f.enumValues.size(); j++) {
                            bool selected = (m_editField.editEnumIndex == j);
                            // Use pre-cached label
                            const char* label = (j < (int)f.enumLabels.size()) ? f.enumLabels[j].c_str() : f.enumValues[j].name.c_str();
                            if (ImGui::Selectable(label, selected)) {
                                m_editField.editEnumIndex = j;
                                m_editField.editInt = f.enumValues[j].value;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                } else if (f.valueTypeClass) {
                    // Struct type - show struct contents instead
                    RenderStructContents(f);
                    canEdit = false; // Editing happens in RenderStructContents
                } else {
                    ImGui::TextDisabled("(unknown struct type)");
                    canEdit = false;
                }
                break;

            default:
                ImGui::TextDisabled("(type not editable)");
                canEdit = false;
                break;
            }

            if (canEdit) {
                ImGui::Separator();
                if (ImGui::Button("Apply", ImVec2(-1, 25))) { ApplyFieldEdit(); }
            }
        }
    }
}

void FieldPanel::RenderArrayContents(const FieldInfo& f) {
    auto& bridge = MonoBridge::Get();

    void* arrPtr = nullptr;
    bridge.GetStaticFieldValue(m_vtable, f.handle, &arrPtr);

    if (!arrPtr || !IsValidPointer(arrPtr)) {
        ImGui::TextDisabled("Array is null");
        return;
    }

    int32_t length = bridge.GetArrayLength(arrPtr);
    ImGui::Text("Length: %d", length);

    if (length == 0) {
        ImGui::TextDisabled("(empty array)");
        return;
    }

    // Toggle to show/hide contents
    if (ImGui::Button(m_showArrayContents ? "Hide Contents" : "View Contents")) {
        m_showArrayContents = !m_showArrayContents;
        if (!m_showArrayContents) {
            m_selectedArrayIndex = -1; // Reset selection when hiding
        }
    }

    if (!m_showArrayContents) return;

    // Navigation for large arrays
    RenderPaginationControls(length, m_arrayDisplayOffset, MAX_ARRAY_DISPLAY);

    // Get element size
    int32_t elemSize = GetElementSize(f.arrayElementType);

    // Check if this element type is editable
    bool isEditable =
        (f.arrayElementType == MonoTypeCode::Boolean || f.arrayElementType == MonoTypeCode::I1 || f.arrayElementType == MonoTypeCode::U1 || f.arrayElementType == MonoTypeCode::Char ||
            f.arrayElementType == MonoTypeCode::I2 || f.arrayElementType == MonoTypeCode::U2 || f.arrayElementType == MonoTypeCode::I4 || f.arrayElementType == MonoTypeCode::U4 || f.arrayElementType == MonoTypeCode::I8 ||
            f.arrayElementType == MonoTypeCode::U8 || f.arrayElementType == MonoTypeCode::R4 || f.arrayElementType == MonoTypeCode::R8 || f.arrayElementType == MonoTypeCode::String);

    if (isEditable) { Colors::Text(Colors::Get().positive, "(click element to edit)"); }

    ImGui::BeginChild("ArrayContents", ImVec2(-1, 200), true);

    int endIdx = m_arrayDisplayOffset + MAX_ARRAY_DISPLAY;
    if (endIdx > length) endIdx = length;
    for (int i = m_arrayDisplayOffset; i < endIdx; i++) {
        void* elemPtr = bridge.GetArrayElementPtr(arrPtr, i, elemSize);
        if (!elemPtr || !IsValidPointer(elemPtr)) continue;

        std::string elemStr = FormatElementValue(elemPtr, f.arrayElementType);
        std::ostringstream ss;
        ss << "[" << i << "] " << elemStr;

        // Check if this is an object array element that can be explored
        bool isObjectElement = (f.arrayElementType == MonoTypeCode::Class || f.arrayElementType == MonoTypeCode::Object);
        if (isObjectElement) {
            void* objPtr = *(void**)elemPtr;
            if (objPtr && IsValidPointer(objPtr)) {
                ImGui::PushID(i);
                if (ImGui::SmallButton("Explore")) {
                    std::string label = f.name + "[" + std::to_string(i) + "]";
                    MainWindow::Get().ExploreObject(objPtr, label);
                }
                ImGui::SameLine();
                ImGui::PopID();
            }
        }

        // Make element selectable for editable types
        if (isEditable) {
            bool selected = (m_selectedArrayIndex == i);
            if (ImGui::Selectable(ss.str().c_str(), selected)) {
                m_selectedArrayIndex = i;
                // Load current value into edit buffer
                switch (f.arrayElementType) {
                case MonoTypeCode::Boolean:
                    m_editArray.editBool = *(int8_t*)elemPtr != 0;
                    break;
                case MonoTypeCode::Char:
                case MonoTypeCode::I2:
                case MonoTypeCode::U2:
                    m_editArray.editInt = *(int16_t*)elemPtr;
                    break;
                case MonoTypeCode::I1:
                case MonoTypeCode::U1:
                    m_editArray.editInt = *(int8_t*)elemPtr;
                    break;
                case MonoTypeCode::I4:
                case MonoTypeCode::U4:
                    m_editArray.editInt = *(int32_t*)elemPtr;
                    break;
                case MonoTypeCode::I8:
                case MonoTypeCode::U8:
                    m_editArray.editInt64 = *(int64_t*)elemPtr;
                    break;
                case MonoTypeCode::R4:
                    m_editArray.editDouble = *(float*)elemPtr;
                    break;
                case MonoTypeCode::R8:
                    m_editArray.editDouble = *(double*)elemPtr;
                    break;
                case MonoTypeCode::String: {
                    void* strObj = *(void**)elemPtr;
                    if (strObj && IsValidPointer(strObj)) {
                        std::string s = bridge.ExtractString(strObj);
                        strncpy_s(m_editArray.editString, s.c_str(), sizeof(m_editArray.editString) - 1);
                    } else {
                        m_editArray.editString[0] = '\0';
                    }
                    break;
                }
                default:
                    break;
                }
            }
        } else {
            ImGui::TextUnformatted(ss.str().c_str());
        }
    }

    ImGui::EndChild();

    // Show edit controls for selected element
    if (isEditable && m_selectedArrayIndex >= 0 && m_selectedArrayIndex < length) {
        ImGui::Separator();
        ImGui::Text("Edit [%d]:", m_selectedArrayIndex);

        switch (f.arrayElementType) {
        case MonoTypeCode::Boolean:
            ImGui::Checkbox("##arrayedit", &m_editArray.editBool);
            break;
        case MonoTypeCode::Char: {
            ImGui::SetNextItemWidth(60);
            char charBuf[2] = {(char)m_editArray.editInt, 0};
            if (ImGui::InputText("##arraychar", charBuf, sizeof(charBuf))) { m_editArray.editInt = (int32_t)(unsigned char)charBuf[0]; }
            break;
        }
        case MonoTypeCode::I1:
        case MonoTypeCode::U1:
        case MonoTypeCode::I2:
        case MonoTypeCode::U2:
        case MonoTypeCode::I4:
        case MonoTypeCode::U4:
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("##arrayint", &m_editArray.editInt);
            break;
        case MonoTypeCode::I8:
        case MonoTypeCode::U8: {
            ImGui::SetNextItemWidth(180);
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)m_editArray.editInt64);
            if (ImGui::InputText("##arrayint64", buf, sizeof(buf), ImGuiInputTextFlags_CharsDecimal)) { m_editArray.editInt64 = _atoi64(buf); }
            break;
        }
        case MonoTypeCode::R4:
        case MonoTypeCode::R8:
            ImGui::SetNextItemWidth(150);
            ImGui::InputDouble("##arraydouble", &m_editArray.editDouble, 0.0, 0.0, "%.6f");
            break;
        case MonoTypeCode::String:
            ImGui::SetNextItemWidth(200);
            ImGui::InputText("##arraystr", m_editArray.editString, sizeof(m_editArray.editString));
            break;
        default:
            break;
        }

        if (ImGui::Button("Apply", ImVec2(80, 0))) { ApplyArrayElementEdit(f, arrPtr, m_selectedArrayIndex); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) { m_selectedArrayIndex = -1; }
    }
}

void FieldPanel::RenderListContents(const FieldInfo& f) {
    auto& bridge = MonoBridge::Get();

    // Get the List object
    void* listObj = nullptr;
    bridge.GetStaticFieldValue(m_vtable, f.handle, &listObj);

    if (!listObj || !IsValidPointer(listObj)) {
        ImGui::TextDisabled("List is null");
        return;
    }

    // Get the backing array and count
    void* itemsArray = nullptr;
    int32_t count = 0;
    if (!GetListContents(listObj, &itemsArray, &count)) {
        ImGui::TextDisabled("Could not read List contents");
        return;
    }

    // Get element type from the array class
    void* arrClass = MonoBridge::GetObjectClass(itemsArray);
    MonoTypeCode elemType = MonoBridge::GetArrayElementType(arrClass);

    ImGui::Text("List<%s>[%d]", GetTypeName(elemType), count);

    if (count == 0) {
        ImGui::TextDisabled("(empty list)");
        return;
    }

    // Toggle to show/hide contents
    if (ImGui::Button(m_showArrayContents ? "Hide Contents##list" : "View Contents##list")) { m_showArrayContents = !m_showArrayContents; }

    if (!m_showArrayContents) return;

    // Navigation for large lists
    RenderPaginationControls(count, m_arrayDisplayOffset, MAX_ARRAY_DISPLAY, "##list");

    // Determine element size
    int32_t elemSize = GetElementSize(elemType);

    // Check if elements are objects (explorable)
    bool elementsAreObjects = (elemType == MonoTypeCode::Class || elemType == MonoTypeCode::Object);

    ImGui::BeginChild("ListContents", ImVec2(-1, 200), true);

    int endIdx = m_arrayDisplayOffset + MAX_ARRAY_DISPLAY;
    if (endIdx > count) endIdx = count;
    for (int i = m_arrayDisplayOffset; i < endIdx; i++) {
        void* elemPtr = bridge.GetArrayElementPtr(itemsArray, i, elemSize);
        if (!elemPtr || !IsValidPointer(elemPtr)) continue;

        std::string elemStr = FormatElementValue(elemPtr, elemType);
        std::ostringstream ss;
        ss << "[" << i << "] " << elemStr;

        // For object lists, make elements clickable
        if (elementsAreObjects) {
            void* objPtr = *(void**)elemPtr;
            if (objPtr && IsValidPointer(objPtr)) {
                ImGui::PushID(i);
                if (ImGui::SmallButton("Explore##listelem")) {
                    std::string label = f.name + "[" + std::to_string(i) + "]";
                    MainWindow::Get().ExploreObject(objPtr, label);
                }
                ImGui::SameLine();
                ImGui::PopID();
            }
        }

        ImGui::TextUnformatted(ss.str().c_str());
    }

    ImGui::EndChild();
}

void FieldPanel::ApplyArrayElementEdit(const FieldInfo& f, void* arrPtr, int index) {
    auto& bridge = MonoBridge::Get();

    // Get element size
    int32_t elemSize = GetElementSize(f.arrayElementType);

    void* elemPtr = bridge.GetArrayElementPtr(arrPtr, index, elemSize);
    if (!elemPtr || !IsValidPointer(elemPtr)) {
        LOG_ERROR("[FieldPanel] Invalid array element pointer for index " + std::to_string(index));
        return;
    }

    switch (f.arrayElementType) {
    case MonoTypeCode::Boolean:
        *(int8_t*)elemPtr = m_editArray.editBool ? 1 : 0;
        break;
    case MonoTypeCode::I1:
        *(int8_t*)elemPtr = (int8_t)m_editArray.editInt;
        break;
    case MonoTypeCode::U1:
        *(uint8_t*)elemPtr = (uint8_t)m_editArray.editInt;
        break;
    case MonoTypeCode::Char:
    case MonoTypeCode::I2:
        *(int16_t*)elemPtr = (int16_t)m_editArray.editInt;
        break;
    case MonoTypeCode::U2:
        *(uint16_t*)elemPtr = (uint16_t)m_editArray.editInt;
        break;
    case MonoTypeCode::I4:
        *(int32_t*)elemPtr = m_editArray.editInt;
        break;
    case MonoTypeCode::U4:
        *(uint32_t*)elemPtr = (uint32_t)m_editArray.editInt;
        break;
    case MonoTypeCode::I8:
        *(int64_t*)elemPtr = m_editArray.editInt64;
        break;
    case MonoTypeCode::U8:
        *(uint64_t*)elemPtr = (uint64_t)m_editArray.editInt64;
        break;
    case MonoTypeCode::R4:
        *(float*)elemPtr = (float)m_editArray.editDouble;
        break;
    case MonoTypeCode::R8:
        *(double*)elemPtr = m_editArray.editDouble;
        break;
    case MonoTypeCode::String: {
        void* newStr = bridge.CreateString(m_editArray.editString);
        if (newStr) { *(void**)elemPtr = newStr; }
        break;
    }
    default:
        break;
    }

    LOG_INFO("[FieldPanel] Set " + f.name + "[" + std::to_string(index) + "]");

    // Refresh the cached value for the field
    if (m_selectedIndex < m_fields.size()) { m_cachedValues[m_selectedIndex] = GetFieldValue(m_fields[m_selectedIndex]); }
}

void FieldPanel::ApplyFieldEdit() {
    if (m_selectedIndex >= m_fields.size()) return;
    const FieldInfo& f = m_fields[m_selectedIndex];

    if (!f.isStatic || f.isLiteral || !m_vtable) return;

    auto& bridge = MonoBridge::Get();

    switch (f.type) {
    case MonoTypeCode::Boolean: {
        int32_t val = m_editField.editBool ? 1 : 0;
        bridge.SetStaticFieldValue(m_vtable, f.handle, &val);
        break;
    }

    case MonoTypeCode::I1:
    case MonoTypeCode::I2:
    case MonoTypeCode::I4:
    case MonoTypeCode::U1:
    case MonoTypeCode::U2:
    case MonoTypeCode::U4:
        bridge.SetStaticFieldValue(m_vtable, f.handle, &m_editField.editInt);
        break;

    case MonoTypeCode::I8:
    case MonoTypeCode::U8:
        bridge.SetStaticFieldValue(m_vtable, f.handle, &m_editField.editInt64);
        break;

    case MonoTypeCode::R4: {
        float val = (float)m_editField.editDouble;
        bridge.SetStaticFieldValue(m_vtable, f.handle, &val);
        break;
    }

    case MonoTypeCode::R8:
        bridge.SetStaticFieldValue(m_vtable, f.handle, &m_editField.editDouble);
        break;

    case MonoTypeCode::String: {
        void* str = bridge.CreateString(m_editField.editString);
        if (str) { bridge.SetStaticFieldValue(m_vtable, f.handle, &str); }
        break;
    }

    case MonoTypeCode::ValueType:
        if (f.isEnum) { bridge.SetStaticFieldValue(m_vtable, f.handle, &m_editField.editInt); }
        break;

    default:
        break;
    }

    // Refresh cached value
    m_cachedValues[m_selectedIndex] = GetFieldValue(f);
    LOG_INFO("[FieldPanel] Set " + f.name + " = " + m_cachedValues[m_selectedIndex]);
}

// STRUCT NAVIGATION - Helpers for drilling into nested struct fields
void FieldPanel::PushStructContext(uintptr_t baseAddr, void* klass, const std::string& name) {
    m_structStack.push_back(MakeStructContext(baseAddr, klass, name));
    m_selectedStructFieldIndex = SIZE_MAX;
    LOG_DEBUG("[FieldPanel] Struct " + name + " base offset adjustment: " + std::to_string(m_structStack.back().baseOffset));
}

void FieldPanel::PopStructContext() {
    if (!m_structStack.empty()) {
        m_structStack.pop_back();
        m_selectedStructFieldIndex = SIZE_MAX;
    }
}

void FieldPanel::RenderStructContents(const FieldInfo& f) {
    if (!f.valueTypeClass || !m_vtable) {
        ImGui::TextDisabled("Cannot read struct");
        return;
    }

    // For static struct fields, we need to get the address from the vtable's static data area (at vtable + offset), where the field offset points within that.
    int32_t fieldOffset = *(int32_t*)((uintptr_t)f.handle + Offsets::FIELD_OFFSET);

    // Get the vtable's static data pointer - it's at offset 0x28 in the vtable
    void* staticData = *(void**)((uintptr_t)m_vtable + 0x28);
    if (!staticData || !IsValidPointer(staticData)) {
        ImGui::TextDisabled("Cannot access static data");
        return;
    }

    uintptr_t structBase = (uintptr_t)staticData + fieldOffset;

    const char* structName = MonoBridge::GetClassName(f.valueTypeClass);
    ImGui::Text("Struct: %s", structName ? structName : "?");

    // View/Hide button to push initial struct context
    if (m_structStack.empty()) {
        if (ImGui::Button("View##struct")) { PushStructContext(structBase, f.valueTypeClass, structName ? structName : "struct"); }
        return;
    }

    // Navigation breadcrumb and back button
    if (m_structStack.size() > 1) {
        if (ImGui::Button("< Back##struct")) { PopStructContext(); }
        ImGui::SameLine();
    } else {
        if (ImGui::Button("Hide##struct")) {
            m_structStack.clear();
            return;
        }
        ImGui::SameLine();
    }

    // Show breadcrumb path
    std::ostringstream breadcrumb;
    for (size_t i = 0; i < m_structStack.size(); i++) {
        if (i > 0) breadcrumb << " > ";
        breadcrumb << m_structStack[i].typeName;
    }
    ImGui::TextDisabled("%s", breadcrumb.str().c_str());

    StructContext* ctx = CurrentStructContext();
    if (!ctx || ctx->fields.empty()) {
        ImGui::TextDisabled("(no fields)");
        return;
    }

    ImGui::BeginChild("StructContents", ImVec2(-1, 200), true);

    for (size_t i = 0; i < ctx->fields.size(); i++) {
        const FieldInfo& sf = ctx->fields[i];

        // Calculate field address within the struct
        int32_t subFieldOffset = *(int32_t*)((uintptr_t)sf.handle + Offsets::FIELD_OFFSET);
        int32_t adjustedOffset = subFieldOffset - ctx->baseOffset;
        uintptr_t subFieldAddr = ctx->baseAddress + adjustedOffset;

        std::string valueStr = FormatValueAtAddress(subFieldAddr, sf);
        std::string display = sf.name + ": " + valueStr;

        bool isSelected = (m_selectedStructFieldIndex == i);

        // Make nested structs clickable to drill down
        bool isNestedStruct = (sf.type == MonoTypeCode::ValueType && !sf.isEnum && sf.valueTypeClass);
        // Make object references explorable
        bool isObjectRef = (sf.type == MonoTypeCode::Class || sf.type == MonoTypeCode::Object);

        if (isNestedStruct) {
            ImGui::PushStyleColor(ImGuiCol_Text, Colors::Get().accent);
        } else if (isObjectRef) {
            ImGui::PushStyleColor(ImGuiCol_Text, Colors::Brighten(Colors::Get().accent));
        }

        if (ImGui::Selectable(display.c_str(), isSelected)) {
            m_selectedStructFieldIndex = i;

            // If it's a nested struct, push it onto the stack
            if (isNestedStruct) {
                int32_t nestedOffset = *(int32_t*)((uintptr_t)sf.handle + Offsets::FIELD_OFFSET);
                uintptr_t nestedAddr = ctx->baseAddress + (nestedOffset - ctx->baseOffset);
                const char* nestedName = MonoBridge::GetClassName(sf.valueTypeClass);
                PushStructContext(nestedAddr, sf.valueTypeClass, nestedName ? nestedName : sf.name);
            }
        }

        // Add explore button for object references within structs
        if (isObjectRef) {
            void* objPtr = *(void**)subFieldAddr;
            if (objPtr && IsValidPointer(objPtr)) {
                ImGui::SameLine();
                ImGui::PushID((int)i);
                if (ImGui::SmallButton("Explore")) {
                    MainWindow::Get().ExploreObject(objPtr, sf.name);
                    ImGui::PopID();
                    if (isNestedStruct || isObjectRef) ImGui::PopStyleColor();
                    ImGui::EndChild();
                    return;
                }
                ImGui::PopID();
            }
        }

        if (isNestedStruct || isObjectRef) { ImGui::PopStyleColor(); }
    }

    ImGui::EndChild();

    // Edit controls for selected struct field
    if (m_selectedStructFieldIndex < ctx->fields.size()) {
        const FieldInfo& sf = ctx->fields[m_selectedStructFieldIndex];

        // Skip nested structs and object refs - they're navigated into, not edited directly
        if ((sf.type == MonoTypeCode::ValueType && !sf.isEnum && sf.valueTypeClass) || sf.type == MonoTypeCode::Class || sf.type == MonoTypeCode::Object) {
            if (sf.type == MonoTypeCode::ValueType) { ImGui::TextDisabled("Click to drill into nested struct"); }
            return;
        }

        int32_t fieldOffset = *(int32_t*)((uintptr_t)sf.handle + Offsets::FIELD_OFFSET);
        uintptr_t fieldAddr = ctx->baseAddress + (fieldOffset - ctx->baseOffset);

        ImGui::Separator();
        ImGui::Text("Edit: %s [%s]", sf.name.c_str(), GetTypeName(sf.type));

        bool canEdit = true;

        switch (sf.type) {
        case MonoTypeCode::Boolean:
            if (IsValidPointer((void*)fieldAddr)) { m_editStruct.editBool = *(int8_t*)fieldAddr != 0; }
            ImGui::Checkbox("##structEdit", &m_editStruct.editBool);
            break;

        case MonoTypeCode::I1:
        case MonoTypeCode::I2:
        case MonoTypeCode::I4:
        case MonoTypeCode::U1:
        case MonoTypeCode::U2:
        case MonoTypeCode::U4:
            if (sf.isEnum && !sf.enumValues.empty()) {
                if (IsValidPointer((void*)fieldAddr)) {
                    int32_t currentVal = *(int32_t*)fieldAddr;
                    for (int j = 0; j < (int)sf.enumValues.size(); j++) {
                        if (sf.enumValues[j].value == currentVal) {
                            m_editStruct.editInt = j;
                            break;
                        }
                    }
                }
                ImGui::SetNextItemWidth(200);
                if (ImGui::BeginCombo("##structEnum", sf.enumValues[m_editStruct.editInt].name.c_str())) {
                    for (int j = 0; j < (int)sf.enumValues.size(); j++) {
                        bool selected = (m_editStruct.editInt == j);
                        std::string label = sf.enumValues[j].name + " (" + std::to_string(sf.enumValues[j].value) + ")";
                        if (ImGui::Selectable(label.c_str(), selected)) { m_editStruct.editInt = j; }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                if (IsValidPointer((void*)fieldAddr)) { m_editStruct.editInt = *(int32_t*)fieldAddr; }
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("##structInt", &m_editStruct.editInt);
            }
            break;

        case MonoTypeCode::I8:
        case MonoTypeCode::U8: {
            if (IsValidPointer((void*)fieldAddr)) { m_editStruct.editInt64 = *(int64_t*)fieldAddr; }
            ImGui::SetNextItemWidth(180);
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)m_editStruct.editInt64);
            if (ImGui::InputText("##structInt64", buf, sizeof(buf), ImGuiInputTextFlags_CharsDecimal)) { m_editStruct.editInt64 = _atoi64(buf); }
            break;
        }

        case MonoTypeCode::R4: {
            if (IsValidPointer((void*)fieldAddr)) { m_editStruct.editDouble = *(float*)fieldAddr; }
            ImGui::SetNextItemWidth(150);
            ImGui::InputDouble("##structFloat", &m_editStruct.editDouble, 0.0, 0.0, "%.6f");
            break;
        }

        case MonoTypeCode::R8:
            if (IsValidPointer((void*)fieldAddr)) { m_editStruct.editDouble = *(double*)fieldAddr; }
            ImGui::SetNextItemWidth(150);
            ImGui::InputDouble("##structDouble", &m_editStruct.editDouble, 0.0, 0.0, "%.6f");
            break;

        case MonoTypeCode::ValueType:
            if (sf.isEnum && !sf.enumValues.empty()) {
                if (IsValidPointer((void*)fieldAddr)) {
                    int32_t currentVal = *(int32_t*)fieldAddr;
                    for (int j = 0; j < (int)sf.enumValues.size(); j++) {
                        if (sf.enumValues[j].value == currentVal) {
                            m_editStruct.editInt = j;
                            break;
                        }
                    }
                }
                ImGui::SetNextItemWidth(200);
                if (ImGui::BeginCombo("##structEnum", sf.enumValues[m_editStruct.editInt].name.c_str())) {
                    for (int j = 0; j < (int)sf.enumValues.size(); j++) {
                        bool selected = (m_editStruct.editInt == j);
                        std::string label = sf.enumValues[j].name + " (" + std::to_string(sf.enumValues[j].value) + ")";
                        if (ImGui::Selectable(label.c_str(), selected)) { m_editStruct.editInt = j; }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::TextDisabled("(struct - click to view)");
                canEdit = false;
            }
            break;

        default:
            ImGui::TextDisabled("(type not editable)");
            canEdit = false;
            break;
        }

        if (canEdit) {
            ImGui::SameLine();
            if (ImGui::Button("Apply##struct")) { ApplyStructFieldEdit((void*)fieldAddr, sf); }
        }
    }
}

void FieldPanel::ApplyStructFieldEdit(void* addr, const FieldInfo& subField) {
    if (!addr || !IsValidPointer(addr)) {
        LOG_ERROR("[FieldPanel] Invalid address for struct field edit");
        return;
    }

    ApplyEditToAddress(addr, subField, m_editStruct);

    LOG_INFO("[FieldPanel] Set struct field " + subField.name);

    // Refresh the cached value for the parent field
    if (m_selectedIndex < m_fields.size()) { m_cachedValues[m_selectedIndex] = GetFieldValue(m_fields[m_selectedIndex]); }
}

} // namespace S3MM
