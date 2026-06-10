#include "singleton_panel.h"
#include "themes.h"
#include "list_helpers.h"
#include "main_window.h"
#include "object_explorer_panel.h"
#include "../core/mono_bridge.h"
#include "../util/logger.h"
#include "imgui.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace S3MM {

void SingletonPanel::SetClass(void* klassHandle, const std::string& className) {
    Clear();

    if (!klassHandle) return;

    m_klassHandle = klassHandle;
    m_className = className;

    // Check if this class has a singleton instance
    MonoClass klass(klassHandle);
    if (!klass.IsValid()) return;

    std::string fieldName;
    if (klass.HasSingletonField(&fieldName)) {
        m_singletonFieldName = fieldName;
        m_instance = klass.GetSingletonInstance();

        if (m_instance && IsValidPointer(m_instance)) {
            LoadInstanceFields();
            LOG_INFO("[SingletonPanel] Found singleton for " + className + " via field " + fieldName);
        } else {
            LOG_DEBUG("[SingletonPanel] Singleton field exists but instance is null for " + className);
        }
    } else {
        LOG_DEBUG("[SingletonPanel] No singleton field found for " + className);
    }
}

void SingletonPanel::Clear() {
    m_klassHandle = nullptr;
    m_className.clear();
    m_singletonFieldName.clear();
    m_instance = nullptr;
    m_instanceFields.clear();
    m_cachedValues.clear();
    m_selectedFieldIndex = SIZE_MAX;
    m_fieldFilter[0] = '\0';
    m_showArrayContents = false;
    m_structStack.clear();
    m_selectedStructFieldIndex = SIZE_MAX;
}

void SingletonPanel::LoadInstanceFields() {
    m_instanceFields.clear();
    m_cachedValues.clear();
    m_selectedFieldIndex = SIZE_MAX;

    if (!m_instance || !IsValidPointer(m_instance)) return;
    if (!m_klassHandle) return;

    MonoClass klass(m_klassHandle);
    m_instanceFields = klass.GetInstanceFields();
    m_cachedValues.resize(m_instanceFields.size());

    RefreshInstanceFieldValues();

    LOG_INFO("[SingletonPanel] Loaded " + std::to_string(m_instanceFields.size()) + " instance fields");
}

void SingletonPanel::RefreshInstanceFieldValues() {
    if (!m_instance || !IsValidPointer(m_instance)) return;

    for (size_t i = 0; i < m_instanceFields.size(); i++) { m_cachedValues[i] = FormatInstanceFieldValue(m_instance, m_instanceFields[i]); }
}

void SingletonPanel::Render() {
    if (!m_klassHandle) {
        ImGui::TextDisabled("Select a class to view its instance.");
        return;
    }

    // Header
    Colors::Text(Colors::Get().neutral, "%s", m_className.c_str());

    if (m_singletonFieldName.empty()) {
        ImGui::TextDisabled("This class has no singleton field.");
        ImGui::TextDisabled("(No static field named 'Instance', 'sInstance', 'gInstance', etc.)");
        return;
    }

    ImGui::Text("Singleton field: %s", m_singletonFieldName.c_str());

    if (m_instance && IsValidPointer(m_instance)) {
        Colors::Text(Colors::Get().positive, "Instance: 0x%08X", (uintptr_t)m_instance);
    } else {
        Colors::Text(Colors::Get().negative, "Instance: null");
        ImGui::TextDisabled("(Not loaded in game yet?)");

        if (ImGui::Button("Retry")) {
            MonoClass klass(m_klassHandle);
            m_instance = klass.GetSingletonInstance();
            if (m_instance && IsValidPointer(m_instance)) { LoadInstanceFields(); }
        }
        return;
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        MonoClass klass(m_klassHandle);
        m_instance = klass.GetSingletonInstance();
        LoadInstanceFields();
    }

    ImGui::Text("%zu instance fields", m_instanceFields.size());
    ImGui::Separator();

    RenderInstanceFields();
}

void SingletonPanel::RenderInstanceFields() {
    if (!m_instance || !IsValidPointer(m_instance)) return;

    // Filter
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##fieldFilter", "Filter fields...", m_fieldFilter, sizeof(m_fieldFilter));

    std::string filter(m_fieldFilter);
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

    // Field list with values
    ImGui::BeginChild("FieldScroll", ImVec2(-1, -100), true);

    for (size_t i = 0; i < m_instanceFields.size(); i++) {
        const FieldInfo& f = m_instanceFields[i];

        // Apply filter
        if (!filter.empty()) {
            std::string lowerName = f.name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
            if (lowerName.find(filter) == std::string::npos) continue;
        }

        // Color by type
        ImVec4 color = Colors::Get().accent;

        ImGui::PushStyleColor(ImGuiCol_Text, color);

        bool isSelected = (m_selectedFieldIndex == i);
        std::string display = f.name + " = " + (i < m_cachedValues.size() ? m_cachedValues[i] : "?");

        if (ImGui::Selectable(display.c_str(), isSelected)) {
            m_selectedFieldIndex = i;

            // Reset array display state
            m_showArrayContents = false;
            m_arrayDisplayOffset = 0;

            // Reset struct display state
            m_structStack.clear();
            m_selectedStructFieldIndex = SIZE_MAX;

            // Load value into edit controls
            auto& bridge = MonoBridge::Get();
            int64_t valBuf = 0;
            switch (f.type) {
            case MonoTypeCode::Boolean:
                bridge.GetInstanceFieldValue(m_instance, f.handle, &valBuf);
                m_editField.editBool = valBuf != 0;
                break;
            case MonoTypeCode::I1:
            case MonoTypeCode::I2:
            case MonoTypeCode::I4:
            case MonoTypeCode::U1:
            case MonoTypeCode::U2:
            case MonoTypeCode::U4:
                bridge.GetInstanceFieldValue(m_instance, f.handle, &valBuf);
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
            case MonoTypeCode::I8:
            case MonoTypeCode::U8:
                bridge.GetInstanceFieldValue(m_instance, f.handle, &m_editField.editInt64);
                break;
            case MonoTypeCode::R4:
                bridge.GetInstanceFieldValue(m_instance, f.handle, &valBuf);
                m_editField.editDouble = (double)*(float*)&valBuf;
                break;
            case MonoTypeCode::R8:
                bridge.GetInstanceFieldValue(m_instance, f.handle, &m_editField.editDouble);
                break;
            case MonoTypeCode::String: {
                void* strPtr = nullptr;
                bridge.GetInstanceFieldValue(m_instance, f.handle, &strPtr);
                if (strPtr && IsValidPointer(strPtr)) {
                    std::string str = bridge.ExtractString(strPtr);
                    strncpy_s(m_editField.editString, str.c_str(), sizeof(m_editField.editString) - 1);
                } else {
                    m_editField.editString[0] = '\0';
                }
                break;
            }
            default:
                break;
            }
        }

        // Context menu for this field
        {
            std::string ctxId = "inst_field_ctx_" + std::to_string(i);
            if (ImGui::BeginPopupContextItem(ctxId.c_str())) {
                // Explore option for object-type fields
                if (m_instance && (f.type == MonoTypeCode::Class || f.type == MonoTypeCode::Object || f.type == MonoTypeCode::GenericInst || f.type == MonoTypeCode::SzArray)) {
                    void* objPtr = nullptr;
                    MonoBridge::Get().GetInstanceFieldValue(m_instance, f.handle, &objPtr);
                    if (objPtr && IsValidPointer(objPtr)) {
                        if (ImGui::MenuItem("Explore")) { MainWindow::Get().ExploreObject(objPtr, f.name); }
                    }
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

        ImGui::PopStyleColor();
    }

    ImGui::EndChild();

    // Edit panel
    if (m_selectedFieldIndex < m_instanceFields.size()) {
        const FieldInfo& f = m_instanceFields[m_selectedFieldIndex];

        ImGui::Separator();

        // Special handling for arrays, lists, and structs
        if (f.type == MonoTypeCode::SzArray) {
            RenderArrayContents(m_instance, f);
        } else if (f.type == MonoTypeCode::Class || f.type == MonoTypeCode::Object || f.type == MonoTypeCode::GenericInst) {
            // Check if it's a List<T>
            void* objPtr = nullptr;
            MonoBridge::Get().GetInstanceFieldValue(m_instance, f.handle, &objPtr);
            if (objPtr && IsValidPointer(objPtr)) {
                void* objClass = MonoBridge::GetObjectClass(objPtr);
                if (objClass && IsListType(objClass)) {
                    RenderListContents(m_instance, f);
                } else {
                    // Regular object - show Explore button
                    ImGui::Text("Field: %s [%s]", f.name.c_str(), GetTypeName(f.type));
                    ImGui::Text("Value: obj@0x%08X", (uintptr_t)objPtr);
                    if (ImGui::Button("Explore")) { MainWindow::Get().ExploreObject(objPtr, f.name); }
                }
            } else {
                ImGui::Text("Field: %s [%s]", f.name.c_str(), GetTypeName(f.type));
                ImGui::TextDisabled("Value: <null>");
            }
        } else if (f.type == MonoTypeCode::ValueType && !f.isEnum && f.valueTypeClass) {
            RenderStructContents(m_instance, f);
        } else {
            ImGui::Text("Edit: %s [%s]", f.name.c_str(), GetTypeName(f.type));

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
                            std::string label = f.enumValues[j].name + " (" + std::to_string(f.enumValues[j].value) + ")";
                            if (ImGui::Selectable(label.c_str(), selected)) {
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
                            std::string label = f.enumValues[j].name + " (" + std::to_string(f.enumValues[j].value) + ")";
                            if (ImGui::Selectable(label.c_str(), selected)) {
                                m_editField.editEnumIndex = j;
                                m_editField.editInt = f.enumValues[j].value;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                } else {
                    // Struct without type info - use View button in struct section above
                    ImGui::TextDisabled("(unknown value type)");
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
                if (ImGui::Button("Apply")) { ApplyFieldEdit(); }
            }
        }
    }
}

void SingletonPanel::RenderArrayContents(void* instance, const FieldInfo& f) {
    auto& bridge = MonoBridge::Get();

    void* arrPtr = nullptr;
    bridge.GetInstanceFieldValue(instance, f.handle, &arrPtr);

    if (!arrPtr || !IsValidPointer(arrPtr)) {
        ImGui::TextDisabled("Array is null");
        return;
    }

    int32_t length = bridge.GetArrayLength(arrPtr);
    ImGui::Text("Array: %s[%d]", GetTypeName(f.arrayElementType), length);

    if (length == 0) {
        ImGui::TextDisabled("(empty array)");
        return;
    }

    ImGui::Checkbox("Show Contents", &m_showArrayContents);

    if (!m_showArrayContents) return;

    // Navigation for large arrays
    RenderPaginationControls(length, m_arrayDisplayOffset, MAX_ARRAY_DISPLAY);

    int32_t elemSize = GetElementSize(f.arrayElementType);

    ImGui::BeginChild("ArrayContents", ImVec2(-1, 150), true);

    int endIdx = m_arrayDisplayOffset + MAX_ARRAY_DISPLAY;
    if (endIdx > length) endIdx = length;
    for (int i = m_arrayDisplayOffset; i < endIdx; i++) {
        void* elemPtr = bridge.GetArrayElementPtr(arrPtr, i, elemSize);
        if (!elemPtr || !IsValidPointer(elemPtr)) continue;

        std::string elemStr = "[" + std::to_string(i) + "] " + FormatElementValue(elemPtr, f.arrayElementType);

        // Add explore button for object array elements
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

        ImGui::TextUnformatted(elemStr.c_str());
    }

    ImGui::EndChild();
}

void SingletonPanel::RenderListContents(void* instance, const FieldInfo& f) {
    auto& bridge = MonoBridge::Get();

    // Get the List object
    void* listObj = nullptr;
    bridge.GetInstanceFieldValue(instance, f.handle, &listObj);

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

    ImGui::Checkbox("Show Contents##list", &m_showArrayContents);

    if (!m_showArrayContents) return;

    // Navigation for large lists
    RenderPaginationControls(count, m_arrayDisplayOffset, MAX_ARRAY_DISPLAY, "##list");

    // Determine element size
    int32_t elemSize = GetElementSize(elemType);

    // Check if elements are objects (explorable)
    bool elementsAreObjects = (elemType == MonoTypeCode::Class || elemType == MonoTypeCode::Object);

    ImGui::BeginChild("ListContents", ImVec2(-1, 150), true);

    int endIdx = m_arrayDisplayOffset + MAX_ARRAY_DISPLAY;
    if (endIdx > count) endIdx = count;
    for (int i = m_arrayDisplayOffset; i < endIdx; i++) {
        void* elemPtr = bridge.GetArrayElementPtr(itemsArray, i, elemSize);
        if (!elemPtr || !IsValidPointer(elemPtr)) continue;

        std::string elemStr = "[" + std::to_string(i) + "] " + FormatElementValue(elemPtr, elemType);

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

        ImGui::TextUnformatted(elemStr.c_str());
    }

    ImGui::EndChild();
}

void SingletonPanel::PushStructContext(uintptr_t baseAddr, void* klass, const std::string& name) {
    m_structStack.push_back(MakeStructContext(baseAddr, klass, name));
    m_selectedStructFieldIndex = SIZE_MAX;
    LOG_DEBUG("[SingletonPanel] Struct " + name + " base offset adjustment: " + std::to_string(m_structStack.back().baseOffset));
}

void SingletonPanel::PopStructContext() {
    if (!m_structStack.empty()) {
        m_structStack.pop_back();
        m_selectedStructFieldIndex = SIZE_MAX;
    }
}

void SingletonPanel::RenderStructContents(void* instance, const FieldInfo& f) {
    // Calculate the struct's base address from the parent instance
    int32_t fieldOffset = *(int32_t*)((uintptr_t)f.handle + Offsets::FIELD_OFFSET);
    uintptr_t structBase = (uintptr_t)instance + fieldOffset;

    const char* structName = MonoBridge::GetClassName(f.valueTypeClass);
    ImGui::Text("Struct: %s", structName ? structName : "?");

    // View/Hide button to push initial struct context
    if (m_structStack.empty()) {
        if (ImGui::Button("View")) { PushStructContext(structBase, f.valueTypeClass, structName ? structName : "struct"); }
        return;
    }

    // Navigation breadcrumb and back button
    if (m_structStack.size() > 1) {
        if (ImGui::Button("< Back")) { PopStructContext(); }
        ImGui::SameLine();
    } else {
        if (ImGui::Button("Hide")) {
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
        uintptr_t subFieldAddr = GetStructFieldAddress(*ctx, sf);

        std::string valueStr = FormatValueAtAddress(subFieldAddr, sf);
        std::string display = sf.name + ": " + valueStr;

        bool isSelected = (m_selectedStructFieldIndex == i);

        // Make nested structs clickable to drill down
        bool isNestedStruct = (sf.type == MonoTypeCode::ValueType && !sf.isEnum && sf.valueTypeClass);
        // Make object references explorable
        bool isObjectRef = (sf.type == MonoTypeCode::Class || sf.type == MonoTypeCode::Object);

        if (isNestedStruct) {
            ImGui::PushStyleColor(ImGuiCol_Text, Colors::Get().accent); // Blue for nested structs
        } else if (isObjectRef) {
            ImGui::PushStyleColor(ImGuiCol_Text, Colors::Tint(Colors::Get().positive, 0.45f)); // Teal for object refs
        }

        if (ImGui::Selectable(display.c_str(), isSelected)) {
            m_selectedStructFieldIndex = i;

            // If it's a nested struct, push it onto the stack
            if (isNestedStruct) {
                uintptr_t nestedAddr = GetStructFieldAddress(*ctx, sf);
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

        uintptr_t fieldAddr = GetStructFieldAddress(*ctx, sf);

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
                // Find current enum index
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

void SingletonPanel::ApplyStructFieldEdit(void* addr, const FieldInfo& subField) {
    if (!addr || !IsValidPointer(addr)) {
        LOG_ERROR("[SingletonPanel] Invalid address for struct field edit");
        return;
    }

    ApplyEditToAddress(addr, subField, m_editStruct);
    LOG_INFO("[SingletonPanel] Set struct field " + subField.name);
}

void SingletonPanel::ApplyFieldEdit() {
    if (m_selectedFieldIndex >= m_instanceFields.size()) return;
    if (!m_instance || !IsValidPointer(m_instance)) return;

    const FieldInfo& f = m_instanceFields[m_selectedFieldIndex];
    auto& bridge = MonoBridge::Get();

    switch (f.type) {
    case MonoTypeCode::Boolean: {
        int32_t val = m_editField.editBool ? 1 : 0;
        bridge.SetInstanceFieldValue(m_instance, f.handle, &val);
        break;
    }
    case MonoTypeCode::I1:
    case MonoTypeCode::I2:
    case MonoTypeCode::I4:
    case MonoTypeCode::U1:
    case MonoTypeCode::U2:
    case MonoTypeCode::U4:
        bridge.SetInstanceFieldValue(m_instance, f.handle, &m_editField.editInt);
        break;
    case MonoTypeCode::I8:
    case MonoTypeCode::U8:
        bridge.SetInstanceFieldValue(m_instance, f.handle, &m_editField.editInt64);
        break;
    case MonoTypeCode::R4: {
        float val = (float)m_editField.editDouble;
        bridge.SetInstanceFieldValue(m_instance, f.handle, &val);
        break;
    }
    case MonoTypeCode::R8:
        bridge.SetInstanceFieldValue(m_instance, f.handle, &m_editField.editDouble);
        break;
    case MonoTypeCode::String: {
        void* str = bridge.CreateString(m_editField.editString);
        if (str) bridge.SetInstanceFieldValue(m_instance, f.handle, &str);
        break;
    }
    case MonoTypeCode::ValueType:
        if (f.isEnum) bridge.SetInstanceFieldValue(m_instance, f.handle, &m_editField.editInt);
        break;
    default:
        break;
    }

    m_cachedValues[m_selectedFieldIndex] = FormatInstanceFieldValue(m_instance, f);
    LOG_INFO("[SingletonPanel] Set " + f.name + " = " + m_cachedValues[m_selectedFieldIndex]);
}

} // namespace S3MM
