#include "object_explorer_panel.h"
#include "list_helpers.h"
#include "themes.h"
#include "../core/mono_bridge.h"
#include "../util/logger.h"
#include "main_window.h"
#include "imgui.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace S3MM {

std::string ObjectExplorerPanel::GetWindowTitle() const {
    std::string title = "Object Explorer";
    if (!m_historyStack.empty()) { title += " - " + m_historyStack.back().label; }
    title += "###ObjExplorer" + std::to_string(m_windowId);
    return title;
}

void ObjectExplorerPanel::ExploreObject(void* object, const std::string& label) {
    if (!object || !IsValidPointer(object)) {
        LOG_WARN("[ObjectExplorer] Attempted to explore null/invalid object");
        return;
    }

    // Get the class from the object
    void* klass = MonoBridge::GetObjectClass(object);
    if (!klass || !IsValidPointer(klass)) {
        LOG_ERROR("[ObjectExplorer] Could not get class from object at 0x" + std::to_string((uintptr_t)object));
        return;
    }

    // Build class name
    MonoClass mc(klass);
    std::string className = mc.FullName();

    // Create new context
    ObjectContext ctx;
    ctx.object = object;
    ctx.klass = klass;
    ctx.className = className;
    ctx.label = label.empty() ? className : label;

    // Load fields
    LoadObjectFields(ctx);

    // Push onto stack
    m_historyStack.push_back(std::move(ctx));

    // Reset UI state
    m_selectedFieldIndex = SIZE_MAX;
    m_fieldFilter[0] = '\0';
    m_showArrayContents = false;
    m_structStack.clear();
    m_selectedStructFieldIndex = SIZE_MAX;

    // Refresh values
    RefreshFieldValues();

    LOG_INFO("[ObjectExplorer] Now exploring " + className + " at 0x" + std::to_string((uintptr_t)object));
}

void ObjectExplorerPanel::Clear() {
    m_historyStack.clear();
    m_cachedValues.clear();
    m_selectedFieldIndex = SIZE_MAX;
    m_fieldFilter[0] = '\0';
    m_showArrayContents = false;
    m_structStack.clear();
    m_selectedStructFieldIndex = SIZE_MAX;
}

void* ObjectExplorerPanel::GetCurrentObject() const {
    if (m_historyStack.empty()) return nullptr;
    return m_historyStack.back().object;
}

ObjectExplorerPanel::ObjectContext* ObjectExplorerPanel::CurrentContext() {
    return m_historyStack.empty() ? nullptr : &m_historyStack.back();
}

void ObjectExplorerPanel::NavigateBack() {
    if (m_historyStack.size() > 1) {
        m_historyStack.pop_back();
        m_selectedFieldIndex = SIZE_MAX;
        m_showArrayContents = false;
        m_structStack.clear();
        m_selectedStructFieldIndex = SIZE_MAX;
        RefreshFieldValues();
    }
}

void ObjectExplorerPanel::LoadObjectFields(ObjectContext& ctx) {
    ctx.fields.clear();

    if (!ctx.klass) return;

    MonoBridge::Get().SetupFields(ctx.klass);
    MonoClass mc(ctx.klass);
    ctx.fields = mc.GetInstanceFields();

    LOG_DEBUG("[ObjectExplorer] Loaded " + std::to_string(ctx.fields.size()) + " instance fields for " + ctx.className);
}

void ObjectExplorerPanel::RefreshFieldValues() {
    ObjectContext* ctx = CurrentContext();
    if (!ctx || !ctx->object || !IsValidPointer(ctx->object)) {
        m_cachedValues.clear();
        m_cachedDisplayStrings.clear();
        return;
    }

    m_cachedValues.resize(ctx->fields.size());
    m_cachedDisplayStrings.resize(ctx->fields.size());
    for (size_t i = 0; i < ctx->fields.size(); i++) {
        m_cachedValues[i] = FormatInstanceFieldValue(ctx->object, ctx->fields[i]);
        // Pre-build display string
        m_cachedDisplayStrings[i] = ctx->fields[i].name + " = " + m_cachedValues[i];
    }
}

void ObjectExplorerPanel::Render() {
    ObjectContext* ctx = CurrentContext();

    if (!ctx) {
        ImGui::TextDisabled("No object selected.");
        ImGui::TextDisabled("Click 'Explore' on an object field to inspect it.");
        return;
    }

    // Validate object is still valid
    if (!ctx->object || !IsValidPointer(ctx->object)) {
        Colors::Text(Colors::Get().negative, "Object no longer valid!");
        ImGui::TextDisabled("The object may have been garbage collected.");
        if (ImGui::Button("Close")) { Clear(); }
        return;
    }

    // Navigation breadcrumb
    if (m_historyStack.size() > 1) {
        if (ImGui::Button("< Back")) {
            NavigateBack();
            return;
        }
        ImGui::SameLine();

        // Show breadcrumb path
        std::ostringstream breadcrumb;
        for (size_t i = 0; i < m_historyStack.size(); i++) {
            if (i > 0) breadcrumb << " > ";
            breadcrumb << m_historyStack[i].label;
        }
        ImGui::TextDisabled("%s", breadcrumb.str().c_str());
    }

    // Header
    ImGui::TextColored(Colors::Get().neutral, "%s", ctx->className.c_str());
    ImGui::TextColored(Colors::Get().positive, "Address: 0x%08X", (uintptr_t)ctx->object);

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) { RefreshFieldValues(); }

    ImGui::Text("%zu instance fields", ctx->fields.size());
    ImGui::Separator();

    RenderObjectFields();
}

void ObjectExplorerPanel::RenderObjectFields() {
    ObjectContext* ctx = CurrentContext();
    if (!ctx || !ctx->object || !IsValidPointer(ctx->object)) return;

    // Filter - only recompute lowercase when filter text changes
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputTextWithHint("##fieldFilter", "Filter fields...", m_fieldFilter, sizeof(m_fieldFilter))) {
        m_cachedFilterLower = m_fieldFilter;
        std::transform(m_cachedFilterLower.begin(), m_cachedFilterLower.end(), m_cachedFilterLower.begin(), ::tolower);
    }

    // Field list with values
    ImGui::BeginChild("FieldScroll", ImVec2(-1, -120), true);

    for (size_t i = 0; i < ctx->fields.size(); i++) {
        const FieldInfo& f = ctx->fields[i];

        // Apply filter using pre-computed lowercase names
        if (!m_cachedFilterLower.empty()) {
            if (f.nameLower.find(m_cachedFilterLower) == std::string::npos) continue;
        }

        // Color by type (theme-adaptive)
        const auto& fc = Colors::Get();
        ImVec4 color;
        if (f.type == MonoTypeCode::Class || f.type == MonoTypeCode::Object) {
            color = fc.accent; // Blue for object references
        } else if (f.type == MonoTypeCode::SzArray) {
            color = fc.neutral; // Amber for arrays
        } else if (f.type == MonoTypeCode::ValueType && !f.isEnum) {
            color = Colors::Tint(fc.accent, 0.75f); // Purple for structs
        } else {
            color = fc.accent;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);

        bool isSelected = (m_selectedFieldIndex == i);
        // Use pre-cached display string
        const char* display = (i < m_cachedDisplayStrings.size()) ? m_cachedDisplayStrings[i].c_str() : f.name.c_str();

        if (ImGui::Selectable(display, isSelected)) {
            m_selectedFieldIndex = i;

            // Reset array/struct display state
            m_showArrayContents = false;
            m_arrayDisplayOffset = 0;
            m_structStack.clear();
            m_selectedStructFieldIndex = SIZE_MAX;

            // Load value into edit controls
            auto& bridge = MonoBridge::Get();
            int64_t valBuf = 0;
            switch (f.type) {
            case MonoTypeCode::Boolean:
                bridge.GetInstanceFieldValue(ctx->object, f.handle, &valBuf);
                m_editField.editBool = valBuf != 0;
                break;
            case MonoTypeCode::I1:
            case MonoTypeCode::I2:
            case MonoTypeCode::I4:
            case MonoTypeCode::U1:
            case MonoTypeCode::U2:
            case MonoTypeCode::U4:
                bridge.GetInstanceFieldValue(ctx->object, f.handle, &valBuf);
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
                bridge.GetInstanceFieldValue(ctx->object, f.handle, &m_editField.editInt64);
                break;
            case MonoTypeCode::R4:
                bridge.GetInstanceFieldValue(ctx->object, f.handle, &valBuf);
                m_editField.editDouble = (double)*(float*)&valBuf;
                break;
            case MonoTypeCode::R8:
                bridge.GetInstanceFieldValue(ctx->object, f.handle, &m_editField.editDouble);
                break;
            case MonoTypeCode::String: {
                void* strPtr = nullptr;
                bridge.GetInstanceFieldValue(ctx->object, f.handle, &strPtr);
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

        ImGui::PopStyleColor();
    }

    ImGui::EndChild();

    // Edit panel
    if (m_selectedFieldIndex < ctx->fields.size()) {
        const FieldInfo& f = ctx->fields[m_selectedFieldIndex];

        ImGui::Separator();

        // Special handling for object references - show Explore button or List contents
        if (f.type == MonoTypeCode::Class || f.type == MonoTypeCode::Object || f.type == MonoTypeCode::GenericInst) {
            void* objPtr = nullptr;
            MonoBridge::Get().GetInstanceFieldValue(ctx->object, f.handle, &objPtr);

            if (objPtr && IsValidPointer(objPtr)) {
                // Check if it's a List<T>
                void* objClass = MonoBridge::GetObjectClass(objPtr);
                if (objClass && IsListType(objClass)) {
                    RenderListContents(f);
                } else {
                    // Regular object - show Explore button
                    ImGui::Text("Field: %s [%s]", f.name.c_str(), GetTypeName(f.type));
                    ImGui::Text("Value: obj@0x%08X", (uintptr_t)objPtr);
                    if (ImGui::Button("Explore")) { ExploreObject(objPtr, f.name); }
                }
            } else {
                ImGui::Text("Field: %s [%s]", f.name.c_str(), GetTypeName(f.type));
                ImGui::TextDisabled("Value: <null>");
            }
        }
        // Special handling for arrays
        else if (f.type == MonoTypeCode::SzArray) {
            RenderArrayContents(f);
        }
        // Special handling for structs
        else if (f.type == MonoTypeCode::ValueType && !f.isEnum && f.valueTypeClass) {
            RenderStructContents(f);
        }
        // Primitive types - editable
        else {
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

void ObjectExplorerPanel::RenderArrayContents(const FieldInfo& f) {
    ObjectContext* ctx = CurrentContext();
    if (!ctx) return;

    auto& bridge = MonoBridge::Get();

    void* arrPtr = nullptr;
    bridge.GetInstanceFieldValue(ctx->object, f.handle, &arrPtr);

    if (!arrPtr || !IsValidPointer(arrPtr)) {
        ImGui::TextDisabled("Array is null");
        return;
    }

    int32_t length = bridge.GetArrayLength(arrPtr);

    // Get the element class for struct arrays
    void* arrClass = MonoBridge::GetObjectClass(arrPtr);
    void* elemClass = arrClass ? MonoBridge::GetArrayElementClass(arrClass) : nullptr;
    bool elementsAreStructs = (f.arrayElementType == MonoTypeCode::ValueType && elemClass != nullptr);

    // Display array type with proper name
    if (elementsAreStructs && elemClass) {
        const char* elemClassName = MonoBridge::GetClassName(elemClass);
        ImGui::Text("Array: %s[%d]", elemClassName ? elemClassName : "struct", length);
    } else {
        ImGui::Text("Array: %s[%d]", GetTypeName(f.arrayElementType), length);
    }

    if (length == 0) {
        ImGui::TextDisabled("(empty array)");
        return;
    }

    ImGui::Checkbox("Show Contents", &m_showArrayContents);

    if (!m_showArrayContents) return;

    // Navigation for large arrays
    RenderPaginationControls(length, m_arrayDisplayOffset, MAX_ARRAY_DISPLAY);

    int32_t elemSize = 4; // Default pointer size for 32-bit

    if (elementsAreStructs && arrClass) {
        // For struct arrays, get the actual element size
        elemSize = bridge.GetArrayElementSize(arrClass);
        if (elemSize <= 0) {
            elemSize = 4; // Fallback
            LOG_WARN("[ObjectExplorer] Could not determine struct array element size, using fallback");
        }
    } else {
        elemSize = GetElementSize(f.arrayElementType);
    }

    // Check if elements are objects (explorable)
    bool elementsAreObjects = (f.arrayElementType == MonoTypeCode::Class || f.arrayElementType == MonoTypeCode::Object || f.arrayElementType == MonoTypeCode::GenericInst);

    ImGui::BeginChild("ArrayContents", ImVec2(-1, 150), true);

    int endIdx = m_arrayDisplayOffset + MAX_ARRAY_DISPLAY;
    if (endIdx > length) endIdx = length;
    for (int i = m_arrayDisplayOffset; i < endIdx; i++) {
        void* elemPtr = bridge.GetArrayElementPtr(arrPtr, i, elemSize);
        if (!elemPtr || !IsValidPointer(elemPtr)) continue;

        std::ostringstream ss;
        ss << "[" << i << "] ";

        if (f.arrayElementType == MonoTypeCode::ValueType) {
            // For struct elements, show a summary of key fields
            if (elemClass) {
                ss << "{";
                // Get the struct's fields
                bridge.SetupFields(elemClass);
                MonoClass structClass(elemClass);
                auto fields = structClass.GetInstanceFields();

                // Calculate base offset for struct fields
                int32_t baseOffset = INT32_MAX;
                for (const auto& sf : fields) {
                    int32_t offset = *(int32_t*)((uintptr_t)sf.handle + Offsets::FIELD_OFFSET);
                    if (offset < baseOffset) baseOffset = offset;
                }
                if (baseOffset == INT32_MAX) baseOffset = 0;

                // Show first few fields as a summary
                int fieldsShown = 0;
                const int maxSummaryFields = 3;
                for (const auto& sf : fields) {
                    if (fieldsShown >= maxSummaryFields) {
                        ss << ", ...";
                        break;
                    }

                    int32_t fieldOffset = *(int32_t*)((uintptr_t)sf.handle + Offsets::FIELD_OFFSET);
                    uintptr_t fieldAddr = (uintptr_t)elemPtr + (fieldOffset - baseOffset);

                    if (fieldsShown > 0) ss << ", ";
                    ss << sf.name << "=";

                    // Format field value based on type
                    switch (sf.type) {
                    case MonoTypeCode::Boolean:
                        ss << (*(int8_t*)fieldAddr ? "true" : "false");
                        break;
                    case MonoTypeCode::I4:
                        ss << *(int32_t*)fieldAddr;
                        break;
                    case MonoTypeCode::U4:
                        ss << *(uint32_t*)fieldAddr;
                        break;
                    case MonoTypeCode::R4:
                        ss << std::fixed << std::setprecision(2) << *(float*)fieldAddr;
                        break;
                    case MonoTypeCode::R8:
                        ss << std::fixed << std::setprecision(2) << *(double*)fieldAddr;
                        break;
                    case MonoTypeCode::ValueType:
                        if (sf.isEnum) {
                            int32_t enumVal = *(int32_t*)fieldAddr;
                            bool found = false;
                            for (const auto& ev : sf.enumValues) {
                                if (ev.value == enumVal) {
                                    ss << ev.name;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) ss << enumVal;
                        } else {
                            ss << "{...}";
                        }
                        break;
                    default:
                        ss << "?";
                        break;
                    }
                    fieldsShown++;
                }
                ss << "}";
            } else {
                ss << "<struct>";
            }
        } else {
            ss << FormatElementValue(elemPtr, f.arrayElementType);
        }

        // For struct elements in arrays, make them expandable via tree node
        if (elementsAreStructs && elemClass) {
            std::string nodeLabel = ss.str();
            ImGui::PushID(i);
            if (ImGui::TreeNode("##structelem", "[%d] %s", i, nodeLabel.c_str())) {
                // Show full struct contents when expanded
                bridge.SetupFields(elemClass);
                MonoClass structClass(elemClass);
                auto fields = structClass.GetInstanceFields();

                // Calculate base offset
                int32_t baseOffset = INT32_MAX;
                for (const auto& sf : fields) {
                    int32_t offset = *(int32_t*)((uintptr_t)sf.handle + Offsets::FIELD_OFFSET);
                    if (offset < baseOffset) baseOffset = offset;
                }
                if (baseOffset == INT32_MAX) baseOffset = 0;

                for (const auto& sf : fields) {
                    int32_t fieldOffset = *(int32_t*)((uintptr_t)sf.handle + Offsets::FIELD_OFFSET);
                    uintptr_t fieldAddr = (uintptr_t)elemPtr + (fieldOffset - baseOffset);

                    std::string valueStr = FormatValueAtAddress(fieldAddr, sf);
                    ImGui::Text("  %s: %s", sf.name.c_str(), valueStr.c_str());
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        // For object arrays, make elements clickable
        else if (elementsAreObjects) {
            void* objPtr = *(void**)elemPtr;
            if (objPtr && IsValidPointer(objPtr)) {
                ImGui::PushID(i);
                if (ImGui::Button("Explore")) {
                    std::string label = f.name + "[" + std::to_string(i) + "]";
                    ExploreObject(objPtr, label);
                    ImGui::PopID();
                    ImGui::EndChild();
                    return; // Exit early since we changed context
                }
                ImGui::SameLine();
                ImGui::PopID();
            }
            ImGui::TextUnformatted(ss.str().c_str());
        } else {
            ImGui::TextUnformatted(ss.str().c_str());
        }
    }

    ImGui::EndChild();
}

void ObjectExplorerPanel::RenderListContents(const FieldInfo& f) {
    ObjectContext* ctx = CurrentContext();
    if (!ctx) return;

    auto& bridge = MonoBridge::Get();

    // Get the List object
    void* listObj = nullptr;
    bridge.GetInstanceFieldValue(ctx->object, f.handle, &listObj);

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
    void* elemClass = MonoBridge::GetArrayElementClass(arrClass);

    // Get the element class name for better display
    const char* elemClassName = nullptr;
    if (elemClass) { elemClassName = MonoBridge::GetClassName(elemClass); }

    if (elemClassName && elemType == MonoTypeCode::ValueType) {
        ImGui::Text("List<%s>[%d]", elemClassName, count);
    } else {
        ImGui::Text("List<%s>[%d]", GetTypeName(elemType), count);
    }

    if (count == 0) {
        ImGui::TextDisabled("(empty list)");
        return;
    }

    ImGui::Checkbox("Show Contents##list", &m_showArrayContents);

    if (!m_showArrayContents) return;

    // Navigation for large lists
    RenderPaginationControls(count, m_arrayDisplayOffset, MAX_ARRAY_DISPLAY, "##list");

    // Determine element size
    int32_t elemSize = 4; // Default pointer size for 32-bit
    bool elementsAreStructs = (elemType == MonoTypeCode::ValueType && elemClass != nullptr);

    if (elementsAreStructs) {
        // For struct arrays, get the actual element size from the array class
        elemSize = bridge.GetArrayElementSize(arrClass);
        if (elemSize <= 0) {
            // Fallback: try to calculate from struct fields if GetArrayElementSize fails
            elemSize = 4; // Minimum fallback
            LOG_WARN("[ObjectExplorer] Could not determine struct element size, using fallback");
        }
    } else {
        elemSize = GetElementSize(elemType);
    }

    // Check if elements are objects (explorable)
    bool elementsAreObjects = (elemType == MonoTypeCode::Class || elemType == MonoTypeCode::Object || elemType == MonoTypeCode::GenericInst);

    ImGui::BeginChild("ListContents", ImVec2(-1, 150), true);

    int endIdx = m_arrayDisplayOffset + MAX_ARRAY_DISPLAY;
    if (endIdx > count) endIdx = count;
    for (int i = m_arrayDisplayOffset; i < endIdx; i++) {
        void* elemPtr = bridge.GetArrayElementPtr(itemsArray, i, elemSize);
        if (!elemPtr || !IsValidPointer(elemPtr)) continue;

        std::ostringstream ss;
        ss << "[" << i << "] ";

        if (elemType == MonoTypeCode::ValueType) {
            // For struct elements, show a summary of key fields
            if (elemClass) {
                ss << "{";
                // Get the struct's fields
                bridge.SetupFields(elemClass);
                MonoClass structClass(elemClass);
                auto fields = structClass.GetInstanceFields();

                // Calculate base offset for struct fields
                int32_t baseOffset = INT32_MAX;
                for (const auto& sf : fields) {
                    int32_t offset = *(int32_t*)((uintptr_t)sf.handle + Offsets::FIELD_OFFSET);
                    if (offset < baseOffset) baseOffset = offset;
                }
                if (baseOffset == INT32_MAX) baseOffset = 0;

                // Show first few fields as a summary
                int fieldsShown = 0;
                const int maxSummaryFields = 3;
                for (const auto& sf : fields) {
                    if (fieldsShown >= maxSummaryFields) {
                        ss << ", ...";
                        break;
                    }

                    int32_t fieldOffset = *(int32_t*)((uintptr_t)sf.handle + Offsets::FIELD_OFFSET);
                    uintptr_t fieldAddr = (uintptr_t)elemPtr + (fieldOffset - baseOffset);

                    if (fieldsShown > 0) ss << ", ";
                    ss << sf.name << "=";

                    // Format field value based on type
                    switch (sf.type) {
                    case MonoTypeCode::Boolean:
                        ss << (*(int8_t*)fieldAddr ? "true" : "false");
                        break;
                    case MonoTypeCode::I4:
                        ss << *(int32_t*)fieldAddr;
                        break;
                    case MonoTypeCode::U4:
                        ss << *(uint32_t*)fieldAddr;
                        break;
                    case MonoTypeCode::R4:
                        ss << std::fixed << std::setprecision(2) << *(float*)fieldAddr;
                        break;
                    case MonoTypeCode::R8:
                        ss << std::fixed << std::setprecision(2) << *(double*)fieldAddr;
                        break;
                    case MonoTypeCode::ValueType:
                        if (sf.isEnum) {
                            int32_t enumVal = *(int32_t*)fieldAddr;
                            bool found = false;
                            for (const auto& ev : sf.enumValues) {
                                if (ev.value == enumVal) {
                                    ss << ev.name;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) ss << enumVal;
                        } else {
                            ss << "{...}";
                        }
                        break;
                    default:
                        ss << "?";
                        break;
                    }
                    fieldsShown++;
                }
                ss << "}";
            } else {
                ss << "<struct>";
            }
        } else {
            ss << FormatElementValue(elemPtr, elemType);
        }

        // For struct elements in lists, make them expandable via tree node
        if (elementsAreStructs && elemClass) {
            std::string nodeLabel = ss.str();
            ImGui::PushID(i);
            if (ImGui::TreeNode("##structelem", "[%d] %s", i, nodeLabel.c_str())) {
                // Show full struct contents when expanded
                bridge.SetupFields(elemClass);
                MonoClass structClass(elemClass);
                auto fields = structClass.GetInstanceFields();

                // Calculate base offset
                int32_t baseOffset = INT32_MAX;
                for (const auto& sf : fields) {
                    int32_t offset = *(int32_t*)((uintptr_t)sf.handle + Offsets::FIELD_OFFSET);
                    if (offset < baseOffset) baseOffset = offset;
                }
                if (baseOffset == INT32_MAX) baseOffset = 0;

                for (const auto& sf : fields) {
                    int32_t fieldOffset = *(int32_t*)((uintptr_t)sf.handle + Offsets::FIELD_OFFSET);
                    uintptr_t fieldAddr = (uintptr_t)elemPtr + (fieldOffset - baseOffset);

                    std::string valueStr = FormatValueAtAddress(fieldAddr, sf);
                    ImGui::Text("  %s: %s", sf.name.c_str(), valueStr.c_str());
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        // For object lists, make elements clickable
        else if (elementsAreObjects) {
            void* objPtr = *(void**)elemPtr;
            if (objPtr && IsValidPointer(objPtr)) {
                ImGui::PushID(i);
                if (ImGui::Button("Explore##listelem")) {
                    std::string label = f.name + "[" + std::to_string(i) + "]";
                    ExploreObject(objPtr, label);
                    ImGui::PopID();
                    ImGui::EndChild();
                    return; // Exit early since we changed context
                }
                ImGui::SameLine();
                ImGui::PopID();
            }
            ImGui::TextUnformatted(ss.str().c_str());
        } else {
            ImGui::TextUnformatted(ss.str().c_str());
        }
    }

    ImGui::EndChild();
}

void ObjectExplorerPanel::PushStructContext(uintptr_t baseAddr, void* klass, const std::string& name) {
    m_structStack.push_back(MakeStructContext(baseAddr, klass, name));
    m_selectedStructFieldIndex = SIZE_MAX;
    LOG_DEBUG("[ObjectExplorer] Struct " + name + " base offset adjustment: " + std::to_string(m_structStack.back().baseOffset));
}

void ObjectExplorerPanel::PopStructContext() {
    if (!m_structStack.empty()) {
        m_structStack.pop_back();
        m_selectedStructFieldIndex = SIZE_MAX;
    }
}

void ObjectExplorerPanel::RenderStructContents(const FieldInfo& f) {
    ObjectContext* ctx = CurrentContext();
    if (!ctx) return;

    // Calculate the struct's base address from the parent instance
    int32_t fieldOffset = *(int32_t*)((uintptr_t)f.handle + Offsets::FIELD_OFFSET);
    uintptr_t structBase = (uintptr_t)ctx->object + fieldOffset;

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

    StructContext* structCtx = CurrentStructContext();
    if (!structCtx || structCtx->fields.empty()) {
        ImGui::TextDisabled("(no fields)");
        return;
    }

    ImGui::BeginChild("StructContents", ImVec2(-1, 200), true);

    for (size_t i = 0; i < structCtx->fields.size(); i++) {
        const FieldInfo& sf = structCtx->fields[i];

        uintptr_t subFieldAddr = GetStructFieldAddress(*structCtx, sf);

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
                uintptr_t nestedAddr = GetStructFieldAddress(*structCtx, sf);
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
                    ExploreObject(objPtr, sf.name);
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
    if (m_selectedStructFieldIndex < structCtx->fields.size()) {
        const FieldInfo& sf = structCtx->fields[m_selectedStructFieldIndex];

        // Skip nested structs and object refs - they're navigated into, not edited directly
        if ((sf.type == MonoTypeCode::ValueType && !sf.isEnum && sf.valueTypeClass) || sf.type == MonoTypeCode::Class || sf.type == MonoTypeCode::Object) {
            if (sf.type == MonoTypeCode::ValueType) { ImGui::TextDisabled("Click to drill into nested struct"); }
            return;
        }

        uintptr_t fieldAddr = GetStructFieldAddress(*structCtx, sf);

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
                        // Use pre-cached label
                        const char* label = (j < (int)sf.enumLabels.size()) ? sf.enumLabels[j].c_str() : sf.enumValues[j].name.c_str();
                        if (ImGui::Selectable(label, selected)) { m_editStruct.editInt = j; }
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
                        // Use pre-cached label
                        const char* label = (j < (int)sf.enumLabels.size()) ? sf.enumLabels[j].c_str() : sf.enumValues[j].name.c_str();
                        if (ImGui::Selectable(label, selected)) { m_editStruct.editInt = j; }
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

void ObjectExplorerPanel::ApplyStructFieldEdit(void* addr, const FieldInfo& subField) {
    ApplyEditToAddress(addr, subField, m_editStruct);
    LOG_INFO("[ObjectExplorer] Set struct field " + subField.name);
}

void ObjectExplorerPanel::ApplyFieldEdit() {
    ObjectContext* ctx = CurrentContext();
    if (!ctx || m_selectedFieldIndex >= ctx->fields.size()) return;
    if (!ctx->object || !IsValidPointer(ctx->object)) return;

    const FieldInfo& f = ctx->fields[m_selectedFieldIndex];
    auto& bridge = MonoBridge::Get();

    switch (f.type) {
    case MonoTypeCode::Boolean: {
        int32_t val = m_editField.editBool ? 1 : 0;
        bridge.SetInstanceFieldValue(ctx->object, f.handle, &val);
        break;
    }
    case MonoTypeCode::I1:
    case MonoTypeCode::I2:
    case MonoTypeCode::I4:
    case MonoTypeCode::U1:
    case MonoTypeCode::U2:
    case MonoTypeCode::U4:
        bridge.SetInstanceFieldValue(ctx->object, f.handle, &m_editField.editInt);
        break;
    case MonoTypeCode::I8:
    case MonoTypeCode::U8:
        bridge.SetInstanceFieldValue(ctx->object, f.handle, &m_editField.editInt64);
        break;
    case MonoTypeCode::R4: {
        float val = (float)m_editField.editDouble;
        bridge.SetInstanceFieldValue(ctx->object, f.handle, &val);
        break;
    }
    case MonoTypeCode::R8:
        bridge.SetInstanceFieldValue(ctx->object, f.handle, &m_editField.editDouble);
        break;
    case MonoTypeCode::String: {
        void* str = bridge.CreateString(m_editField.editString);
        if (str) bridge.SetInstanceFieldValue(ctx->object, f.handle, &str);
        break;
    }
    case MonoTypeCode::ValueType:
        if (f.isEnum) bridge.SetInstanceFieldValue(ctx->object, f.handle, &m_editField.editInt);
        break;
    default:
        break;
    }

    m_cachedValues[m_selectedFieldIndex] = FormatInstanceFieldValue(ctx->object, f);
    m_cachedDisplayStrings[m_selectedFieldIndex] = f.name + " = " + m_cachedValues[m_selectedFieldIndex];
    LOG_INFO("[ObjectExplorer] Set " + f.name + " = " + m_cachedValues[m_selectedFieldIndex]);
}

} // namespace S3MM
