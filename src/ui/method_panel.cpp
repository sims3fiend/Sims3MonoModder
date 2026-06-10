#include "method_panel.h"
#include "main_window.h"
#include "class_browser_panel.h"
#include "themes.h"
#include "xref_panel.h"
#include "../core/mono_bridge.h"
#include "../core/mono_class.h"
#include "../util/logger.h"
#include "imgui.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace S3MM {

void MethodPanel::Render() {
    // Class selection is now handled by MainWindow before tabs render

    if (!m_klass) {
        ImGui::TextDisabled("Select a class from the Class Browser tab");
        return;
    }

    ImGui::Text("Class: %s", m_className.c_str());
    ImGui::Text("%zu methods", m_methods.size());

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        MonoClass mc(m_klass);
        m_methods = mc.GetMethods(false, m_showInherited);
        m_selectedIndex = SIZE_MAX;
        m_hasResult = false;
        LOG_INFO("[MethodPanel] Refreshed " + std::to_string(m_methods.size()) + " methods");
    }

    ImGui::SameLine();
    ImGui::Checkbox("Static only", &m_showStaticOnly);

    ImGui::SameLine();
    if (ImGui::Checkbox("Inherited", &m_showInherited)) {
        MonoClass mc(m_klass);
        m_methods = mc.GetMethods(false, m_showInherited);
        m_selectedIndex = SIZE_MAX;
        m_hasResult = false;
        LOG_INFO("[MethodPanel] Reloaded " + std::to_string(m_methods.size()) + " methods (inherited=" + (m_showInherited ? "true" : "false") + ")");
    }

    ImGui::Separator();

    // Split view: list on left, details on right
    float listWidth = ImGui::GetContentRegionAvail().x * 0.45f;

    ImGui::BeginChild("MethodList", ImVec2(listWidth, -1), true);
    RenderMethodList();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("MethodDetails", ImVec2(0, -1), true);
    RenderMethodDetails();
    ImGui::EndChild();
}

void MethodPanel::SetClass(void* klass, const std::string& className) {
    if (m_klass == klass) return;

    m_klass = klass;
    m_className = className;
    m_selectedIndex = SIZE_MAX;
    m_hasResult = false;

    // Clear all cached state to avoid stale pointers
    m_returnObject = nullptr;
    m_returnTypeClass = nullptr;
    m_returnEnumValues.clear();
    m_returnArrayLength = 0;
    m_returnArrayElementType = MonoTypeCode::Void;
    m_lastCachedMethodIndex = SIZE_MAX;

    // Clear param caches
    for (int i = 0; i < MAX_PARAMS; i++) {
        m_enumCache[i].isEnum = false;
        m_enumCache[i].values.clear();
        m_enumCache[i].selectedIndex = 0;
        m_arrayCache[i].isArray = false;
        m_arrayCache[i].elementType = MonoTypeCode::Void;
        m_arrayCache[i].elementClass = nullptr;
    }

    // Load methods
    MonoClass mc(klass);
    m_methods = mc.GetMethods(false, m_showInherited);

    LOG_INFO("[MethodPanel] Loaded " + std::to_string(m_methods.size()) + " methods from " + className);
}

void MethodPanel::NavigateToMethod(const std::string& methodName) {
    // Set filter to the method name to make it visible
    strncpy_s(m_filterText, methodName.c_str(), sizeof(m_filterText) - 1);

    // Try to select the method if found
    for (size_t i = 0; i < m_methods.size(); i++) {
        if (m_methods[i].name == methodName) {
            m_selectedIndex = i;
            break;
        }
    }

    LOG_INFO("[MethodPanel] Navigated to method: " + methodName);
}

void MethodPanel::RenderMethodList() {
    // Filter
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Filter methods...", m_filterText, sizeof(m_filterText));

    std::string filter(m_filterText);
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

    ImGui::BeginChild("MethodScroll", ImVec2(-1, -1));

    for (size_t i = 0; i < m_methods.size(); i++) {
        const MethodInfo& m = m_methods[i];

        // Filter by static
        if (m_showStaticOnly && !m.isStatic) continue;

        // Apply text filter
        if (!filter.empty()) {
            std::string lowerName = m.name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
            if (lowerName.find(filter) == std::string::npos) continue;
        }

        // Build display string
        std::string display = m.name + "(";
        for (size_t j = 0; j < m.paramTypes.size(); j++) {
            if (j > 0) display += ", ";
            display += GetTypeName(m.paramTypes[j]);
        }
        display += ")";
        if (m.isInherited && !m.declaringClassName.empty()) { display += " (from " + m.declaringClassName + ")"; }

        // Color: blue for static, gray for instance
        ImVec4 color = m.isStatic ? Colors::Get().accent : Colors::Dim(Colors::Get().accent);

        // Dim inherited methods slightly
        if (m.isInherited) {
            color.x *= 0.7f;
            color.y *= 0.7f;
            color.z *= 0.7f;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);

        bool isSelected = (m_selectedIndex == i);
        if (ImGui::Selectable(display.c_str(), isSelected)) {
            m_selectedIndex = i;
            m_hasResult = false;

            // Reset param inputs
            memset(m_paramInts, 0, sizeof(m_paramInts));
            memset(m_paramInt64s, 0, sizeof(m_paramInt64s));
            memset(m_paramFloats, 0, sizeof(m_paramFloats));
            memset(m_paramDoubles, 0, sizeof(m_paramDoubles));
            memset(m_paramBools, 0, sizeof(m_paramBools));
            memset(m_paramStrings, 0, sizeof(m_paramStrings));
            memset(m_paramArrays, 0, sizeof(m_paramArrays));
            for (int j = 0; j < MAX_PARAMS; j++) {
                m_enumCache[j].isEnum = false;
                m_enumCache[j].values.clear();
                m_enumCache[j].selectedIndex = 0;
                m_arrayCache[j].isArray = false;
                m_arrayCache[j].elementType = MonoTypeCode::Void;
                m_arrayCache[j].elementClass = nullptr;
            }

            // Cache enum and array params for the new selection
            m_lastCachedMethodIndex = SIZE_MAX; // Force re-cache
            CacheEnumParams();
            CacheArrayParams();
        }

        // Context menu for this method (right-click)
        std::string contextId = "method_ctx_" + std::to_string(i);
        if (ImGui::BeginPopupContextItem(contextId.c_str())) {
            // Build full method name for display
            std::string fullName = m_className + "::" + m.name;

            // Find Callers
            if (ImGui::MenuItem("Find Callers")) {
                void* image = GetCurrentImage();
                if (image) {
                    MainWindow::Get().GetXRefPanel()->FindCallers(image, fullName);
                } else {
                    LOG_WARN("[MethodPanel] No image available for callers search");
                }
            }

            // Find Callees
            if (ImGui::MenuItem("Find Callees")) {
                void* image = GetCurrentImage();
                if (image && m.handle) {
                    MainWindow::Get().GetXRefPanel()->FindCallees(image, m.handle, fullName);
                } else {
                    LOG_WARN("[MethodPanel] No image/handle available for callees search");
                }
            }

            ImGui::Separator();

            // Profile Method
            if (ImGui::MenuItem("Profile Method")) { MainWindow::Get().OpenProfilerForMethod(fullName); }

            // Copy signature
            if (ImGui::MenuItem("Copy Signature")) {
                // Build full signature
                std::string sig = m.isStatic ? "static " : "";
                sig += std::string(GetTypeName(m.returnType)) + " " + m_className + "." + m.name + "(";
                for (size_t pi = 0; pi < m.paramTypes.size(); pi++) {
                    if (pi > 0) sig += ", ";
                    sig += GetTypeName(m.paramTypes[pi]);
                }
                sig += ")";
                ImGui::SetClipboardText(sig.c_str());
            }

            // Copy Address
            if (ImGui::MenuItem("Copy Address") && m.handle) {
                char addrBuf[32];
                snprintf(addrBuf, sizeof(addrBuf), "0x%08X", (unsigned int)(uintptr_t)m.handle);
                ImGui::SetClipboardText(addrBuf);
            }

            ImGui::EndPopup();
        }

        ImGui::PopStyleColor();

        // Tooltip
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s %s", m.isStatic ? "static" : "instance", GetTypeName(m.returnType));
            ImGui::Text("%s", m.name.c_str());
            if (m.isInherited && !m.declaringClassName.empty()) { ImGui::TextColored(Colors::Dim(Colors::Get().accent), "inherited from %s", m.declaringClassName.c_str()); }
            ImGui::TextDisabled("Right-click for XRef options");
            ImGui::EndTooltip();
        }
    }

    ImGui::EndChild();
}

void MethodPanel::RenderMethodDetails() {
    if (m_selectedIndex >= m_methods.size()) {
        ImGui::TextDisabled("Select a method from the list");
        return;
    }

    const MethodInfo& m = m_methods[m_selectedIndex];

    // Header
    Colors::Text(Colors::Get().neutral, "%s.%s", m_className.c_str(), m.name.c_str());

    // Signature
    std::string sig = m.isStatic ? "static " : "";
    sig += std::string(GetTypeName(m.returnType)) + " " + m.name + "(";
    for (size_t i = 0; i < m.paramTypes.size(); i++) {
        if (i > 0) sig += ", ";
        sig += GetTypeName(m.paramTypes[i]);
    }
    sig += ")";
    ImGui::Text("%s", sig.c_str());

    ImGui::Text("Address: 0x%08X", (uintptr_t)m.handle);
    ImGui::Separator();

    // Parameters
    ImGui::Text("Parameters:");
    if (m.paramTypes.empty()) {
        ImGui::TextDisabled("(none)");
    } else {
        RenderParamInputs();
    }

    ImGui::Separator();

    // Invoke button
    bool canInvoke = m.isStatic; // Only static for now
    if (!canInvoke) { ImGui::TextDisabled("(Instance methods not yet supported)"); }

    ImGui::BeginDisabled(!canInvoke);
    if (ImGui::Button("Invoke", ImVec2(-1, 30))) { InvokeSelectedMethod(); }
    ImGui::EndDisabled();

    // Result
    if (m_hasResult) {
        ImGui::Separator();
        if (m_resultSuccess) {
            Colors::Text(Colors::Get().positive, "Result: %s", m_resultText.c_str());

            // Show "Explore" button for objects and arrays
            if (m_returnObject && IsValidPointer(m_returnObject)) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Explore")) { MainWindow::Get().ExploreObject(m_returnObject, m_className + "." + m.name + " result"); }
            }
        } else {
            Colors::Text(Colors::Get().negative, "Error: %s", m_resultError.c_str());
        }
    }
}

void MethodPanel::CacheEnumParams() {
    // Skip if already cached for this method
    if (m_lastCachedMethodIndex == m_selectedIndex) return;
    m_lastCachedMethodIndex = m_selectedIndex;

    // Clear cache
    for (int i = 0; i < MAX_PARAMS; i++) {
        m_enumCache[i].isEnum = false;
        m_enumCache[i].values.clear();
        m_enumCache[i].selectedIndex = 0;
    }

    if (m_selectedIndex >= m_methods.size()) return;
    const MethodInfo& m = m_methods[m_selectedIndex];

    if (!m.signature || !IsValidPointer(m.signature)) return;

    for (size_t i = 0; i < m.paramTypes.size() && i < MAX_PARAMS; i++) {
        if (m.paramTypes[i] != MonoTypeCode::ValueType) continue;

        // Get the MonoType* for this parameter from signature
        void* paramType = *(void**)((uintptr_t)m.signature + Offsets::SIG_PARAMS + i * sizeof(void*));
        if (!paramType || !IsValidPointer(paramType)) continue;

        // Get the klass from MonoType (data.klass at offset 0)
        void* klass = *(void**)paramType;
        if (!klass || !IsValidPointer(klass)) continue;

        // Check if it's an enum
        if (MonoBridge::IsEnumClass(klass)) {
            m_enumCache[i].isEnum = true;
            MonoClass enumClass(klass);
            m_enumCache[i].values = enumClass.GetEnumValues();

            // Initialize m_paramInts with the first enum value so default selection is valid
            if (!m_enumCache[i].values.empty()) { m_paramInts[i] = m_enumCache[i].values[0].value; }

            LOG_DEBUG("[MethodPanel] Param " + std::to_string(i) + " is enum: " + enumClass.FullName() + " with " + std::to_string(m_enumCache[i].values.size()) + " values");
        }
    }
}

void MethodPanel::RenderParamInputs() {
    if (m_selectedIndex >= m_methods.size()) return;
    const MethodInfo& m = m_methods[m_selectedIndex];

    // Ensure caches are populated
    CacheEnumParams();
    CacheArrayParams();

    for (size_t i = 0; i < m.paramTypes.size() && i < MAX_PARAMS; i++) {
        MonoTypeCode type = m.paramTypes[i];
        ImGui::PushID((int)i);

        // Check if this is an array
        if (type == MonoTypeCode::SzArray && m_arrayCache[i].isArray) {
            ImGui::Text("Param %zu [%s[]]:", i, GetTypeName(m_arrayCache[i].elementType));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(300);
            ImGui::InputTextWithHint("##val", "comma-separated values", m_paramArrays[i], sizeof(m_paramArrays[i]));
        }
        // Check if this is an enum
        else if (type == MonoTypeCode::ValueType && m_enumCache[i].isEnum && !m_enumCache[i].values.empty()) {
            ImGui::Text("Param %zu [enum]:", i);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);

            // Create combo for enum values
            const std::string& currentName = m_enumCache[i].values[m_enumCache[i].selectedIndex].name;
            if (ImGui::BeginCombo("##val", currentName.c_str())) {
                for (int j = 0; j < (int)m_enumCache[i].values.size(); j++) {
                    bool isSelected = (m_enumCache[i].selectedIndex == j);
                    std::string label = m_enumCache[i].values[j].name + " (" + std::to_string(m_enumCache[i].values[j].value) + ")";
                    if (ImGui::Selectable(label.c_str(), isSelected)) {
                        m_enumCache[i].selectedIndex = j;
                        m_paramInts[i] = m_enumCache[i].values[j].value;
                    }
                    if (isSelected) { ImGui::SetItemDefaultFocus(); }
                }
                ImGui::EndCombo();
            }
        } else {
            // Standard type handling
            ImGui::Text("Param %zu [%s]:", i, GetTypeName(type));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);

            switch (type) {
            case MonoTypeCode::Boolean:
                ImGui::Checkbox("##val", &m_paramBools[i]);
                break;

            case MonoTypeCode::I8:
            case MonoTypeCode::U8: {
                ImGui::SetNextItemWidth(180);
                char buf[32];
                snprintf(buf, sizeof(buf), "%lld", (long long)m_paramInt64s[i]);
                if (ImGui::InputText("##val", buf, sizeof(buf), ImGuiInputTextFlags_CharsDecimal)) { m_paramInt64s[i] = _atoi64(buf); }
                break;
            }

            case MonoTypeCode::R4:
                ImGui::InputFloat("##val", &m_paramFloats[i], 0.0f, 0.0f, "%.4f");
                break;

            case MonoTypeCode::R8:
                ImGui::InputDouble("##val", &m_paramDoubles[i], 0.0, 0.0, "%.6f");
                break;

            case MonoTypeCode::String:
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("##val", m_paramStrings[i], sizeof(m_paramStrings[i]));
                break;

            case MonoTypeCode::Class:
            case MonoTypeCode::Object:
            case MonoTypeCode::GenericInst:
                ImGui::TextDisabled("<unsupported - needs object registry>");
                break;

            default:
                ImGui::InputInt("##val", &m_paramInts[i]);
                break;
            }
        }

        ImGui::PopID();
    }
}

void MethodPanel::InvokeSelectedMethod() {
    if (m_selectedIndex >= m_methods.size()) return;

    MethodInfo& m = m_methods[m_selectedIndex];
    auto& bridge = MonoBridge::Get();

    LOG_INFO("[MethodPanel] Invoking: " + m_className + "." + m.name);

    // Set up TLS
    if (!bridge.SetupTLS()) {
        m_hasResult = true;
        m_resultSuccess = false;
        m_resultError = "Failed to setup TLS";
        return;
    }

    // Allocate invoke state
    MonoInvokeState* state = (MonoInvokeState*)bridge.GCAlloc(sizeof(MonoInvokeState));
    if (!state) {
        m_hasResult = true;
        m_resultSuccess = false;
        m_resultError = "Failed to allocate invoke state";
        return;
    }

    // Build params
    std::vector<uint64_t> storage(m.paramTypes.size());
    std::vector<void*> ptrs(m.paramTypes.size());
    std::vector<void*> gcObjects; // Keep strings/arrays alive

    for (size_t i = 0; i < m.paramTypes.size() && i < MAX_PARAMS; i++) {
        switch (m.paramTypes[i]) {
        case MonoTypeCode::Boolean:
            storage[i] = m_paramBools[i] ? 1 : 0;
            ptrs[i] = &storage[i];
            break;

        case MonoTypeCode::I8:
        case MonoTypeCode::U8:
            *(int64_t*)&storage[i] = m_paramInt64s[i];
            ptrs[i] = &storage[i];
            break;

        case MonoTypeCode::R4:
            *(float*)&storage[i] = m_paramFloats[i];
            ptrs[i] = &storage[i];
            break;

        case MonoTypeCode::R8:
            *(double*)&storage[i] = m_paramDoubles[i];
            ptrs[i] = &storage[i];
            break;

        case MonoTypeCode::String: {
            void* str = bridge.CreateString(m_paramStrings[i]);
            if (str) gcObjects.push_back(str);
            ptrs[i] = str;
            break;
        }

        case MonoTypeCode::SzArray: {
            if (m_arrayCache[i].isArray) {
                void* arr = CreateArrayFromInput((int)i, m_arrayCache[i]);
                if (arr) gcObjects.push_back(arr);
                ptrs[i] = arr;
            } else {
                ptrs[i] = nullptr;
            }
            break;
        }

        default:
            storage[i] = (uint64_t)m_paramInts[i];
            ptrs[i] = &storage[i];
            break;
        }
    }

    // Invoke on the sim thread - this runs on the render/UI thread, and the Mono interpreter is not reentrant across threads (see MonoBridge::InvokeMethodOnSimThread).
    bool success = bridge.InvokeMethodOnSimThread(m.handle, nullptr, ptrs.empty() ? nullptr : ptrs.data(), state);

    m_hasResult = true;
    m_resultSuccess = success;

    // Reset return value metadata
    m_returnObject = nullptr;
    m_returnTypeClass = nullptr;
    m_returnEnumValues.clear();
    m_returnArrayLength = 0;
    m_returnArrayElementType = MonoTypeCode::Void;

    if (success) {
        // Determine return type from signature
        m_returnType = MonoTypeCode::Void;
        if (state->pSig) {
            void* retTypePtr = *(void**)((uintptr_t)state->pSig + Offsets::SIG_RET_TYPE);
            if (retTypePtr && IsValidPointer(retTypePtr)) {
                m_returnType = (MonoTypeCode)(*(uint8_t*)((uintptr_t)retTypePtr + Offsets::TYPE_CODE));

                // For ValueType (potential enum), get the class
                if (m_returnType == MonoTypeCode::ValueType) {
                    void* retClass = *(void**)retTypePtr; // data.klass at offset 0
                    if (retClass && IsValidPointer(retClass) && MonoBridge::IsEnumClass(retClass)) {
                        m_returnTypeClass = retClass;
                        MonoClass enumClass(retClass);
                        m_returnEnumValues = enumClass.GetEnumValues();
                    }
                }
            }
        }

        m_resultText = FormatReturnValue(state, m_returnType);
        LOG_INFO("[MethodPanel] Result: " + m_resultText);
    } else {
        m_resultError = ExtractExceptionMessage(state->pThrowExc);
        LOG_ERROR("[MethodPanel] " + m_resultError);
    }
}

std::string MethodPanel::ExtractExceptionMessage(void* exception) {
    if (!exception || !IsValidPointer(exception)) { return "Managed exception thrown (no details available)"; }

    std::ostringstream ss;

    // Get exception type name
    void* excClass = MonoBridge::GetObjectClass(exception);
    if (excClass) {
        const char* typeName = MonoBridge::GetClassName(excClass);
        const char* ns = MonoBridge::GetClassNamespace(excClass);
        if (ns && strlen(ns) > 0) { ss << ns << "."; }
        if (typeName) {
            ss << typeName;
        } else {
            ss << "Exception";
        }
    } else {
        ss << "Exception";
    }

    // Try to read the _message field from the exception object at a known offset.
    // In Mono, System.Exception._message is typically at object header + 8 bytes (after _className), but the layout can vary.
    auto& bridge = MonoBridge::Get();

    // Try offset 0x10 (after vtable ptr + sync block + _className pointer)
    void* messagePtr = *(void**)((uintptr_t)exception + 0x10);
    if (messagePtr && IsValidPointer(messagePtr)) {
        std::string msg = bridge.ExtractString(messagePtr);
        if (!msg.empty()) { ss << ": " << msg; }
    }

    return ss.str();
}

std::string MethodPanel::FormatReturnValue(MonoInvokeState* state, MonoTypeCode retType) {
    auto& bridge = MonoBridge::Get();
    std::ostringstream ss;

    // For value types pRet contains a pointer to the raw value bytes; for reference types pRetVal contains the object pointer.
    void* valuePtr = state->pRet;

    switch (retType) {
    case MonoTypeCode::Void:
        return "(void)";

    case MonoTypeCode::Boolean: {
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

    case MonoTypeCode::Char: {
        uint16_t val = 0;
        if (valuePtr && IsValidPointer(valuePtr)) { val = *(uint16_t*)valuePtr; }
        if (val >= 32 && val < 127) {
            ss << "'" << (char)val << "' (" << (int)val << ")";
        } else {
            ss << "\\u" << std::hex << std::setfill('0') << std::setw(4) << val;
        }
        return ss.str();
    }

    case MonoTypeCode::String:
        if (state->pRetVal && IsValidPointer(state->pRetVal)) { return "\"" + bridge.ExtractString(state->pRetVal) + "\""; }
        return "<null>";

    case MonoTypeCode::ValueType: {
        // Check if it's an enum with cached values
        if (m_returnTypeClass && !m_returnEnumValues.empty()) {
            int32_t val = 0;
            if (valuePtr && IsValidPointer(valuePtr)) { val = *(int32_t*)valuePtr; }

            for (const auto& ev : m_returnEnumValues) {
                if (ev.value == val) {
                    ss << ev.name << " (" << val << ")";
                    return ss.str();
                }
            }
            // Unknown enum value
            const char* enumName = MonoBridge::GetClassName(m_returnTypeClass);
            ss << (enumName ? enumName : "enum") << "(" << val << ")";
            return ss.str();
        }
        // Non-enum struct
        ss << "<struct>";
        return ss.str();
    }

    case MonoTypeCode::SzArray: {
        if (state->pRetVal && IsValidPointer(state->pRetVal)) {
            m_returnObject = state->pRetVal;
            m_returnArrayLength = bridge.GetArrayLength(state->pRetVal);

            // Get element type
            void* arrClass = MonoBridge::GetObjectClass(state->pRetVal);
            if (arrClass) {
                m_returnArrayElementType = MonoBridge::GetArrayElementType(arrClass);
                ss << GetTypeName(m_returnArrayElementType) << "[" << m_returnArrayLength << "]";
            } else {
                ss << "array[" << m_returnArrayLength << "]";
            }
            return ss.str();
        }
        return "<null>";
    }

    case MonoTypeCode::Class:
    case MonoTypeCode::Object:
    case MonoTypeCode::GenericInst:
    default: {
        if (state->pRetVal && IsValidPointer(state->pRetVal)) {
            m_returnObject = state->pRetVal;

            void* objClass = MonoBridge::GetObjectClass(state->pRetVal);
            if (objClass) {
                const char* typeName = MonoBridge::GetClassName(objClass);
                if (typeName) {
                    ss << typeName << "@0x" << std::hex << (uintptr_t)state->pRetVal;
                    return ss.str();
                }
            }
            ss << "obj@0x" << std::hex << (uintptr_t)state->pRetVal;
            return ss.str();
        }
        return "<null>";
    }
    }
}

void MethodPanel::CacheArrayParams() {
    if (m_selectedIndex >= m_methods.size()) return;
    const MethodInfo& m = m_methods[m_selectedIndex];

    if (!m.signature || !IsValidPointer(m.signature)) return;

    for (size_t i = 0; i < m.paramTypes.size() && i < MAX_PARAMS; i++) {
        m_arrayCache[i].isArray = false;
        m_arrayCache[i].elementType = MonoTypeCode::Void;
        m_arrayCache[i].elementClass = nullptr;

        if (m.paramTypes[i] != MonoTypeCode::SzArray) continue;

        // Get MonoType* for this parameter
        void* paramType = *(void**)((uintptr_t)m.signature + Offsets::SIG_PARAMS + i * sizeof(void*));
        if (!paramType || !IsValidPointer(paramType)) continue;

        // Get the array class from MonoType.data.klass
        void* arrayKlass = *(void**)paramType;
        if (!arrayKlass || !IsValidPointer(arrayKlass)) continue;

        m_arrayCache[i].isArray = true;
        // Element type code is at CLASS_ELEMENT_TYPE in the array class
        m_arrayCache[i].elementType = (MonoTypeCode)(*(uint8_t*)((uintptr_t)arrayKlass + Offsets::CLASS_ELEMENT_TYPE));
        // Element class is at CLASS_ELEMENT_CLASS in the array class - needed for mono_array_new
        void* elementKlass = *(void**)((uintptr_t)arrayKlass + Offsets::CLASS_ELEMENT_CLASS);
        if (elementKlass && IsValidPointer(elementKlass)) { m_arrayCache[i].elementClass = elementKlass; }

        LOG_DEBUG("[MethodPanel] Param " + std::to_string(i) + " is array of " + GetTypeName(m_arrayCache[i].elementType));
    }
}

void* MethodPanel::CreateArrayFromInput(int paramIndex, const ArrayCache& cache) {
    auto& bridge = MonoBridge::Get();

    if (!cache.elementClass || !cache.isArray) return nullptr;

    // Parse comma-separated input
    std::string input = m_paramArrays[paramIndex];
    std::vector<std::string> elements;

    // Split by comma
    size_t pos = 0;
    while (pos < input.length()) {
        size_t commaPos = input.find(',', pos);
        if (commaPos == std::string::npos) commaPos = input.length();
        std::string elem = input.substr(pos, commaPos - pos);
        // Trim whitespace
        size_t start = elem.find_first_not_of(" \t");
        size_t end = elem.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            elements.push_back(elem.substr(start, end - start + 1));
        } else if (!elem.empty()) {
            elements.push_back("");
        }
        pos = commaPos + 1;
    }

    if (elements.empty()) {
        // Create empty array
        return bridge.CreateArray(cache.elementClass, 0);
    }

    // Create array
    void* arr = bridge.CreateArray(cache.elementClass, (int32_t)elements.size());
    if (!arr) return nullptr;

    // Determine element size
    int32_t elemSize = 0;
    switch (cache.elementType) {
    case MonoTypeCode::Boolean:
    case MonoTypeCode::I1:
    case MonoTypeCode::U1:
        elemSize = 1;
        break;
    case MonoTypeCode::Char:
    case MonoTypeCode::I2:
    case MonoTypeCode::U2:
        elemSize = 2;
        break;
    case MonoTypeCode::I4:
    case MonoTypeCode::U4:
    case MonoTypeCode::R4:
        elemSize = 4;
        break;
    case MonoTypeCode::I8:
    case MonoTypeCode::U8:
    case MonoTypeCode::R8:
        elemSize = 8;
        break;
    default:
        elemSize = 4;
        break; // Pointers
    }

    // Fill array elements
    for (size_t i = 0; i < elements.size(); i++) {
        void* elemPtr = bridge.GetArrayElementPtr(arr, (int32_t)i, elemSize);
        if (!elemPtr) continue;

        const std::string& val = elements[i];

        switch (cache.elementType) {
        case MonoTypeCode::Boolean:
            *(int8_t*)elemPtr = (val == "true" || val == "1") ? 1 : 0;
            break;
        case MonoTypeCode::I1:
            *(int8_t*)elemPtr = (int8_t)atoi(val.c_str());
            break;
        case MonoTypeCode::U1:
            *(uint8_t*)elemPtr = (uint8_t)atoi(val.c_str());
            break;
        case MonoTypeCode::Char:
            *(uint16_t*)elemPtr = val.empty() ? 0 : (uint16_t)val[0];
            break;
        case MonoTypeCode::I2:
            *(int16_t*)elemPtr = (int16_t)atoi(val.c_str());
            break;
        case MonoTypeCode::U2:
            *(uint16_t*)elemPtr = (uint16_t)atoi(val.c_str());
            break;
        case MonoTypeCode::I4:
            *(int32_t*)elemPtr = atoi(val.c_str());
            break;
        case MonoTypeCode::U4:
            *(uint32_t*)elemPtr = (uint32_t)atoi(val.c_str());
            break;
        case MonoTypeCode::I8:
            *(int64_t*)elemPtr = _atoi64(val.c_str());
            break;
        case MonoTypeCode::U8:
            *(uint64_t*)elemPtr = (uint64_t)_atoi64(val.c_str());
            break;
        case MonoTypeCode::R4:
            *(float*)elemPtr = (float)atof(val.c_str());
            break;
        case MonoTypeCode::R8:
            *(double*)elemPtr = atof(val.c_str());
            break;
        case MonoTypeCode::String:
            *(void**)elemPtr = bridge.CreateString(val.c_str());
            break;
        default:
            break;
        }
    }

    LOG_INFO("[MethodPanel] Created array with " + std::to_string(elements.size()) + " elements");
    return arr;
}

void* MethodPanel::GetCurrentImage() const {
    if (!m_klass) return nullptr;
    return MonoBridge::GetClassImage(m_klass);
}

} // namespace S3MM
