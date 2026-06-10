#include "property_panel.h"
#include "themes.h"
#include "main_window.h"
#include "../core/mono_bridge.h"
#include "../core/mono_class.h"
#include "../util/logger.h"
#include "imgui.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace S3MM {

void PropertyPanel::Render() {
    if (!m_klass) {
        ImGui::TextDisabled("Select a class from the Class Browser tab");
        return;
    }

    ImGui::Text("Class: %s", m_className.c_str());
    ImGui::Text("%zu properties", m_properties.size());

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) { RefreshPropertyValues(); }

    ImGui::SameLine();
    ImGui::Checkbox("Static only", &m_showStaticOnly);

    ImGui::Separator();

    if (m_properties.empty()) {
        ImGui::TextDisabled("No properties found");
        return;
    }

    // Split view
    float listWidth = ImGui::GetContentRegionAvail().x * 0.5f;

    ImGui::BeginChild("PropertyList", ImVec2(listWidth, -1), true);
    RenderPropertyList();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("PropertyDetails", ImVec2(0, -1), true);
    RenderPropertyDetails();
    ImGui::EndChild();
}

void PropertyPanel::SetClass(void* klass, const std::string& className) {
    if (m_klass == klass) return;

    m_klass = klass;
    m_className = className;
    m_selectedIndex = SIZE_MAX;

    // Get vtable for static property access
    m_vtable = MonoBridge::Get().GetClassVTable(klass);

    // Load properties
    MonoClass mc(klass);
    m_properties = mc.GetProperties();

    // Initialize cached values
    m_cachedValues.resize(m_properties.size());
    RefreshPropertyValues();

    LOG_INFO("[PropertyPanel] Loaded " + std::to_string(m_properties.size()) + " properties from " + className);
}

void PropertyPanel::RefreshPropertyValues() {
    // Allocate a fresh invoke state for this batch of property reads to avoid potential GC/runtime issues from rapidly reusing the same state.
    auto& bridge = MonoBridge::Get();
    bridge.SetupTLS();
    m_invokeState = (MonoInvokeState*)bridge.GCAlloc(sizeof(MonoInvokeState));

    if (!m_invokeState) {
        LOG_ERROR("[PropertyPanel] Failed to allocate invoke state for refresh");
        return;
    }

    for (size_t i = 0; i < m_properties.size(); i++) { m_cachedValues[i] = GetPropertyValue(m_properties[i]); }
    LOG_DEBUG("[PropertyPanel] Refreshed " + std::to_string(m_properties.size()) + " property values");
}

std::string PropertyPanel::GetPropertyValue(const PropertyInfo& prop) {
    if (!prop.canRead) return "<write-only>";
    if (!prop.isStatic) return "<instance>";
    if (!m_vtable) return "<no vtable>";
    if (!m_invokeState) return "<no state>";

    // Invoke getter
    MonoInvokeState* state = nullptr;
    if (!InvokeGetter(prop, nullptr, &state) || !state) { return "<error>"; }

    std::ostringstream ss;

    // Return value locations: for value types (isObject=0) pRet points to raw value bytes, for reference types (isObject=1) pRetVal holds the object pointer.
    void* valuePtr = state->pRet; // For value types

    switch (prop.type) {
    case MonoTypeCode::Boolean: {
        // Boolean stored as int32 at pRet
        if (valuePtr && IsValidPointer(valuePtr)) {
            int32_t val = *(int32_t*)valuePtr;
            return val ? "true" : "false";
        }
        return state->isObject ? "true" : "false";
    }

    case MonoTypeCode::I1:
    case MonoTypeCode::I2:
    case MonoTypeCode::I4: {
        int32_t val = 0;
        if (valuePtr && IsValidPointer(valuePtr)) { val = *(int32_t*)valuePtr; }
        if (prop.isEnum) {
            for (const auto& ev : prop.enumValues) {
                if (ev.value == val) { return ev.name + " (" + std::to_string(val) + ")"; }
            }
        }
        ss << val;
        return ss.str();
    }

    case MonoTypeCode::U1:
    case MonoTypeCode::U2:
    case MonoTypeCode::U4: {
        uint32_t val = 0;
        if (valuePtr && IsValidPointer(valuePtr)) { val = *(uint32_t*)valuePtr; }
        ss << val;
        return ss.str();
    }

    case MonoTypeCode::I8: {
        int64_t val = 0;
        if (valuePtr && IsValidPointer(valuePtr)) { val = *(int64_t*)valuePtr; }
        ss << val;
        return ss.str();
    }

    case MonoTypeCode::U8: {
        uint64_t val = 0;
        if (valuePtr && IsValidPointer(valuePtr)) { val = *(uint64_t*)valuePtr; }
        ss << val;
        return ss.str();
    }

    case MonoTypeCode::R4: {
        float val = 0.0f;
        if (valuePtr && IsValidPointer(valuePtr)) { val = *(float*)valuePtr; }
        ss << std::fixed << std::setprecision(4) << val;
        return ss.str();
    }

    case MonoTypeCode::R8: {
        double val = 0.0;
        if (valuePtr && IsValidPointer(valuePtr)) { val = *(double*)valuePtr; }
        ss << std::fixed << std::setprecision(6) << val;
        return ss.str();
    }

    case MonoTypeCode::String:
        if (state->pRetVal && IsValidPointer(state->pRetVal)) { return "\"" + MonoBridge::Get().ExtractString(state->pRetVal) + "\""; }
        return "<null>";

    case MonoTypeCode::ValueType:
        if (prop.isEnum) {
            int32_t val = 0;
            if (valuePtr && IsValidPointer(valuePtr)) { val = *(int32_t*)valuePtr; }
            for (const auto& ev : prop.enumValues) {
                if (ev.value == val) { return ev.name + " (" + std::to_string(val) + ")"; }
            }
            ss << val;
            return ss.str();
        }
        return "<struct>";

    case MonoTypeCode::Class:
    case MonoTypeCode::Object:
    case MonoTypeCode::GenericInst:
        if (state->pRetVal && IsValidPointer(state->pRetVal)) {
            ss << "obj@0x" << std::hex << (uintptr_t)state->pRetVal;
            return ss.str();
        }
        return "<null>";

    case MonoTypeCode::SzArray:
        if (state->pRetVal && IsValidPointer(state->pRetVal)) {
            int32_t len = MonoBridge::Get().GetArrayLength(state->pRetVal);
            ss << "array[" << len << "]";
            return ss.str();
        }
        return "<null>";

    default:
        return "<unsupported>";
    }
}

void PropertyPanel::RenderPropertyList() {
    // Filter
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##filter", "Filter properties...", m_filterText, sizeof(m_filterText))) {
        m_cachedFilterLower = m_filterText;
        std::transform(m_cachedFilterLower.begin(), m_cachedFilterLower.end(), m_cachedFilterLower.begin(), ::tolower);
    }

    ImGui::BeginChild("PropertyScroll", ImVec2(-1, -1));

    for (size_t i = 0; i < m_properties.size(); i++) {
        const PropertyInfo& p = m_properties[i];

        // Filter by static
        if (m_showStaticOnly && !p.isStatic) continue;

        // Apply text filter
        if (!m_cachedFilterLower.empty()) {
            if (p.nameLower.find(m_cachedFilterLower) == std::string::npos) continue;
        }

        // Color based on read/write capability (theme-adaptive)
        const auto& pc = Colors::Get();
        ImVec4 color;
        if (p.canRead && p.canWrite) {
            color = pc.positive; // Green for read/write
        } else if (p.canRead) {
            color = pc.accent;
        } else {
            color = pc.neutral; // Amber for write-only
        }

        if (!p.isStatic) {
            color = Colors::Desaturate(pc.accent); // Gray for instance
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);

        bool isSelected = (m_selectedIndex == i);
        std::string display = p.name + " = " + (i < m_cachedValues.size() ? m_cachedValues[i] : "?");

        if (ImGui::Selectable(display.c_str(), isSelected)) {
            m_selectedIndex = i;

            // Load value into edit controls
            if (p.isStatic && p.canRead) {
                MonoInvokeState* state = nullptr;
                if (InvokeGetter(p, nullptr, &state) && state) {
                    // Value types use pRet, reference types use pRetVal
                    void* valuePtr = state->pRet;

                    switch (p.type) {
                    case MonoTypeCode::Boolean:
                        if (valuePtr && IsValidPointer(valuePtr)) { m_editBool = *(int32_t*)valuePtr != 0; }
                        break;
                    case MonoTypeCode::I1:
                    case MonoTypeCode::I2:
                    case MonoTypeCode::I4:
                    case MonoTypeCode::U1:
                    case MonoTypeCode::U2:
                    case MonoTypeCode::U4:
                        if (valuePtr && IsValidPointer(valuePtr)) { m_editInt = *(int32_t*)valuePtr; }
                        if (p.isEnum) {
                            m_editEnumIndex = 0;
                            for (int j = 0; j < (int)p.enumValues.size(); j++) {
                                if (p.enumValues[j].value == m_editInt) {
                                    m_editEnumIndex = j;
                                    break;
                                }
                            }
                        }
                        break;
                    case MonoTypeCode::I8:
                    case MonoTypeCode::U8:
                        if (valuePtr && IsValidPointer(valuePtr)) { m_editInt64 = *(int64_t*)valuePtr; }
                        break;
                    case MonoTypeCode::R4:
                        if (valuePtr && IsValidPointer(valuePtr)) { m_editDouble = (double)*(float*)valuePtr; }
                        break;
                    case MonoTypeCode::R8:
                        if (valuePtr && IsValidPointer(valuePtr)) { m_editDouble = *(double*)valuePtr; }
                        break;
                    case MonoTypeCode::String:
                        if (state->pRetVal && IsValidPointer(state->pRetVal)) {
                            std::string s = MonoBridge::Get().ExtractString(state->pRetVal);
                            strncpy_s(m_editString, s.c_str(), sizeof(m_editString) - 1);
                        } else {
                            m_editString[0] = '\0';
                        }
                        break;
                    case MonoTypeCode::ValueType:
                        if (p.isEnum) {
                            if (valuePtr && IsValidPointer(valuePtr)) { m_editInt = *(int32_t*)valuePtr; }
                            m_editEnumIndex = 0;
                            for (int j = 0; j < (int)p.enumValues.size(); j++) {
                                if (p.enumValues[j].value == m_editInt) {
                                    m_editEnumIndex = j;
                                    break;
                                }
                            }
                        }
                        break;
                    default:
                        break;
                    }
                }
            }
        }

        // Context menu for this property
        {
            std::string ctxId = "prop_ctx_" + std::to_string(i);
            if (ImGui::BeginPopupContextItem(ctxId.c_str())) {
                // Explore option for object-type static properties with getters
                if (p.isStatic && p.canRead && (p.type == MonoTypeCode::Class || p.type == MonoTypeCode::Object || p.type == MonoTypeCode::GenericInst || p.type == MonoTypeCode::SzArray)) {
                    MonoInvokeState* state = nullptr;
                    if (InvokeGetter(p, nullptr, &state) && state && state->pRetVal && IsValidPointer(state->pRetVal)) {
                        if (ImGui::MenuItem("Explore")) { MainWindow::Get().ExploreObject(state->pRetVal, p.name); }
                    }
                }
                // Copy type
                if (ImGui::MenuItem("Copy Type")) {
                    std::string typeName = p.isEnum ? "enum" : GetTypeName(p.type);
                    ImGui::SetClipboardText(typeName.c_str());
                }
                // Copy name
                if (ImGui::MenuItem("Copy Name")) {
                    std::string fullName = m_className + "." + p.name;
                    ImGui::SetClipboardText(fullName.c_str());
                }
                ImGui::EndPopup();
            }
        }

        // Tooltip
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s %s", p.isStatic ? "static" : "instance", p.isEnum ? "enum" : GetTypeName(p.type));
            ImGui::Text("%s", p.name.c_str());
            std::string access;
            if (p.canRead) access += "get";
            if (p.canRead && p.canWrite) access += "/";
            if (p.canWrite) access += "set";
            ImGui::TextDisabled("[%s]", access.c_str());
            ImGui::TextDisabled("Right-click for options");
            ImGui::EndTooltip();
        }

        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
}

void PropertyPanel::RenderPropertyDetails() {
    if (m_selectedIndex >= m_properties.size()) {
        ImGui::TextDisabled("Select a property from the list");
        return;
    }

    const PropertyInfo& p = m_properties[m_selectedIndex];

    // Header
    Colors::Text(Colors::Get().neutral, "%s.%s", m_className.c_str(), p.name.c_str());
    ImGui::Text("Type: %s", p.isEnum ? "enum" : GetTypeName(p.type));

    // Access info
    std::string access;
    if (p.canRead) access += "get";
    if (p.canRead && p.canWrite) access += "/";
    if (p.canWrite) access += "set";
    ImGui::Text("Access: %s", access.c_str());

    if (p.isStatic) Colors::Text(Colors::Get().positive, "static");

    ImGui::Separator();

    // Current value
    ImGui::Text("Current: %s", m_selectedIndex < m_cachedValues.size() ? m_cachedValues[m_selectedIndex].c_str() : "?");

    // Edit controls (only for static properties with setters)
    if (p.isStatic && p.canWrite) {
        ImGui::Separator();
        ImGui::Text("New Value:");

        bool canEdit = true;

        switch (p.type) {
        case MonoTypeCode::Boolean:
            ImGui::Checkbox("##edit", &m_editBool);
            break;

        case MonoTypeCode::I1:
        case MonoTypeCode::I2:
        case MonoTypeCode::I4:
        case MonoTypeCode::U1:
        case MonoTypeCode::U2:
        case MonoTypeCode::U4:
            if (p.isEnum && !p.enumValues.empty()) {
                ImGui::SetNextItemWidth(200);
                if (ImGui::BeginCombo("##enum", p.enumValues[m_editEnumIndex].name.c_str())) {
                    for (int j = 0; j < (int)p.enumValues.size(); j++) {
                        bool selected = (m_editEnumIndex == j);
                        std::string label = p.enumValues[j].name + " (" + std::to_string(p.enumValues[j].value) + ")";
                        if (ImGui::Selectable(label.c_str(), selected)) {
                            m_editEnumIndex = j;
                            m_editInt = p.enumValues[j].value;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("##int", &m_editInt);
            }
            break;

        case MonoTypeCode::I8:
        case MonoTypeCode::U8: {
            ImGui::SetNextItemWidth(180);
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)m_editInt64);
            if (ImGui::InputText("##int64", buf, sizeof(buf), ImGuiInputTextFlags_CharsDecimal)) { m_editInt64 = _atoi64(buf); }
            break;
        }

        case MonoTypeCode::R4:
        case MonoTypeCode::R8:
            ImGui::SetNextItemWidth(150);
            ImGui::InputDouble("##double", &m_editDouble, 0.0, 0.0, "%.6f");
            break;

        case MonoTypeCode::String:
            ImGui::SetNextItemWidth(200);
            ImGui::InputText("##str", m_editString, sizeof(m_editString));
            break;

        case MonoTypeCode::ValueType:
            if (p.isEnum && !p.enumValues.empty()) {
                ImGui::SetNextItemWidth(200);
                if (ImGui::BeginCombo("##enum", p.enumValues[m_editEnumIndex].name.c_str())) {
                    for (int j = 0; j < (int)p.enumValues.size(); j++) {
                        bool selected = (m_editEnumIndex == j);
                        std::string label = p.enumValues[j].name + " (" + std::to_string(p.enumValues[j].value) + ")";
                        if (ImGui::Selectable(label.c_str(), selected)) {
                            m_editEnumIndex = j;
                            m_editInt = p.enumValues[j].value;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::TextDisabled("(struct properties not editable)");
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
            if (ImGui::Button("Apply", ImVec2(-1, 25))) { ApplyPropertyEdit(); }
        }
    } else if (!p.isStatic) {
        ImGui::Separator();
        ImGui::TextDisabled("(Instance properties not yet supported)");
    } else if (!p.canWrite) {
        ImGui::Separator();
        ImGui::TextDisabled("(Read-only property)");
    }

    // Explore button for object-type properties (read-only or read-write)
    if (p.isStatic && p.canRead && (p.type == MonoTypeCode::Class || p.type == MonoTypeCode::Object || p.type == MonoTypeCode::GenericInst || p.type == MonoTypeCode::SzArray)) {
        MonoInvokeState* state = nullptr;
        if (InvokeGetter(p, nullptr, &state) && state && state->pRetVal && IsValidPointer(state->pRetVal)) {
            ImGui::Separator();
            if (ImGui::Button("Explore Object", ImVec2(-1, 25))) { MainWindow::Get().ExploreObject(state->pRetVal, p.name); }
        }
    }
}

bool PropertyPanel::InvokeGetter(const PropertyInfo& prop, void* instance, MonoInvokeState** outState) {
    if (!prop.getterMethod || !prop.canRead) return false;
    if (!m_invokeState) return false;

    auto& bridge = MonoBridge::Get();

    if (!bridge.SetupTLS()) {
        LOG_ERROR("[PropertyPanel] Failed to setup TLS for getter");
        return false;
    }

    // Note: InvokeMethod already does memset on the state
    bool success = bridge.InvokeMethod(prop.getterMethod, instance, nullptr, m_invokeState);

    if (!success) { LOG_WARN("[PropertyPanel] Getter " + prop.name + " crashed in interpreter (method=0x" + std::to_string((uintptr_t)prop.getterMethod) + ")"); }

    if (success && outState) {
        *outState = m_invokeState;

        // Debug: log all potentially relevant return fields
        LOG_DEBUG("[PropertyPanel] Getter " + prop.name + ": pRet=0x" + std::to_string((uintptr_t)m_invokeState->pRet) + " pRetVal=0x" + std::to_string((uintptr_t)m_invokeState->pRetVal) + " pResult=0x" +
                  std::to_string((uintptr_t)m_invokeState->pResult) + " isObject=" + std::to_string(m_invokeState->isObject));
    }

    return success;
}

bool PropertyPanel::InvokeSetter(const PropertyInfo& prop, void* instance, void* value) {
    if (!prop.setterMethod || !prop.canWrite) return false;
    if (!m_invokeState) return false;

    auto& bridge = MonoBridge::Get();

    if (!bridge.SetupTLS()) {
        LOG_ERROR("[PropertyPanel] Failed to setup TLS for setter");
        return false;
    }

    // Note: InvokeMethod already does memset on the state
    void* params[1] = {value};
    bool success = bridge.InvokeMethod(prop.setterMethod, instance, params, m_invokeState);

    if (!success) { LOG_ERROR("[PropertyPanel] Setter invocation failed"); }

    return success;
}

void PropertyPanel::ApplyPropertyEdit() {
    if (m_selectedIndex >= m_properties.size()) return;
    const PropertyInfo& p = m_properties[m_selectedIndex];

    if (!p.isStatic || !p.canWrite) return;

    auto& bridge = MonoBridge::Get();
    void* valuePtr = nullptr;
    uint64_t storage = 0;

    switch (p.type) {
    case MonoTypeCode::Boolean:
        storage = m_editBool ? 1 : 0;
        valuePtr = &storage;
        break;

    case MonoTypeCode::I1:
    case MonoTypeCode::I2:
    case MonoTypeCode::I4:
    case MonoTypeCode::U1:
    case MonoTypeCode::U2:
    case MonoTypeCode::U4:
        storage = (uint64_t)m_editInt;
        valuePtr = &storage;
        break;

    case MonoTypeCode::I8:
    case MonoTypeCode::U8:
        *(int64_t*)&storage = m_editInt64;
        valuePtr = &storage;
        break;

    case MonoTypeCode::R4: {
        float f = (float)m_editDouble;
        *(float*)&storage = f;
        valuePtr = &storage;
        break;
    }

    case MonoTypeCode::R8:
        *(double*)&storage = m_editDouble;
        valuePtr = &storage;
        break;

    case MonoTypeCode::String:
        valuePtr = bridge.CreateString(m_editString);
        break;

    case MonoTypeCode::ValueType:
        if (p.isEnum) {
            storage = (uint64_t)m_editInt;
            valuePtr = &storage;
        }
        break;

    default:
        return;
    }

    if (InvokeSetter(p, nullptr, valuePtr)) {
        // Refresh cached value
        m_cachedValues[m_selectedIndex] = GetPropertyValue(p);
        LOG_INFO("[PropertyPanel] Set " + p.name + " = " + m_cachedValues[m_selectedIndex]);
    } else {
        LOG_ERROR("[PropertyPanel] Failed to set " + p.name);
    }
}

} // namespace S3MM
