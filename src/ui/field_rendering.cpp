#include "field_rendering.h"
#include "../core/mono_bridge.h"
#include "../util/logger.h"
#include "list_helpers.h"
#include "imgui.h"
#include <sstream>
#include <iomanip>

namespace S3MM {

int32_t GetElementSize(MonoTypeCode type) {
    switch (type) {
    case MonoTypeCode::Boolean:
    case MonoTypeCode::I1:
    case MonoTypeCode::U1:
        return 1;
    case MonoTypeCode::Char:
    case MonoTypeCode::I2:
    case MonoTypeCode::U2:
        return 2;
    case MonoTypeCode::I4:
    case MonoTypeCode::U4:
    case MonoTypeCode::R4:
        return 4;
    case MonoTypeCode::I8:
    case MonoTypeCode::U8:
    case MonoTypeCode::R8:
        return 8;
    case MonoTypeCode::String:
    case MonoTypeCode::Class:
    case MonoTypeCode::Object:
    case MonoTypeCode::SzArray:
    case MonoTypeCode::GenericInst:
    default:
        return 4; // Pointer size for 32-bit
    }
}

std::string FormatValueAtAddress(uintptr_t fieldAddr, const FieldInfo& field) {
    if (!IsValidPointer((void*)fieldAddr)) { return "<invalid>"; }

    auto& bridge = MonoBridge::Get();
    std::ostringstream ss;

    switch (field.type) {
    case MonoTypeCode::Boolean:
        ss << (*(int8_t*)fieldAddr ? "true" : "false");
        break;
    case MonoTypeCode::I1:
        ss << (int)(*(int8_t*)fieldAddr);
        break;
    case MonoTypeCode::U1:
        ss << (unsigned)(*(uint8_t*)fieldAddr);
        break;
    case MonoTypeCode::I2:
        ss << *(int16_t*)fieldAddr;
        break;
    case MonoTypeCode::U2:
        ss << *(uint16_t*)fieldAddr;
        break;
    case MonoTypeCode::I4:
        ss << *(int32_t*)fieldAddr;
        break;
    case MonoTypeCode::U4:
        ss << *(uint32_t*)fieldAddr;
        break;
    case MonoTypeCode::I8:
        ss << *(int64_t*)fieldAddr;
        break;
    case MonoTypeCode::U8:
        ss << *(uint64_t*)fieldAddr;
        break;
    case MonoTypeCode::R4:
        ss << std::fixed << std::setprecision(4) << *(float*)fieldAddr;
        break;
    case MonoTypeCode::R8:
        ss << std::fixed << std::setprecision(6) << *(double*)fieldAddr;
        break;
    case MonoTypeCode::String: {
        void* strObj = *(void**)fieldAddr;
        if (strObj && IsValidPointer(strObj)) {
            ss << "\"" << bridge.ExtractString(strObj) << "\"";
        } else {
            ss << "<null>";
        }
        break;
    }
    case MonoTypeCode::ValueType: {
        if (field.isEnum) {
            int32_t val = *(int32_t*)fieldAddr;
            for (const auto& ev : field.enumValues) {
                if (ev.value == val) {
                    ss << ev.name << " (" << val << ")";
                    return ss.str();
                }
            }
            ss << val;
        } else if (field.valueTypeClass) {
            const char* name = MonoBridge::GetClassName(field.valueTypeClass);
            ss << "{" << (name ? name : "struct") << "}";
        } else {
            ss << "<struct>";
        }
        break;
    }
    case MonoTypeCode::Class:
    case MonoTypeCode::Object: {
        void* objPtr = *(void**)fieldAddr;
        if (objPtr && IsValidPointer(objPtr)) {
            void* objClass = MonoBridge::GetObjectClass(objPtr);
            if (objClass) {
                const char* typeName = MonoBridge::GetClassName(objClass);
                if (typeName) {
                    ss << typeName << "@0x" << std::hex << (uintptr_t)objPtr;
                } else {
                    ss << "obj@0x" << std::hex << (uintptr_t)objPtr;
                }
            } else {
                ss << "obj@0x" << std::hex << (uintptr_t)objPtr;
            }
        } else {
            ss << "<null>";
        }
        break;
    }
    case MonoTypeCode::SzArray: {
        void* arrPtr = *(void**)fieldAddr;
        if (arrPtr && IsValidPointer(arrPtr)) {
            int32_t len = MonoBridge::Get().GetArrayLength(arrPtr);
            ss << GetTypeName(field.arrayElementType) << "[" << len << "]";
        } else {
            ss << "<null>";
        }
        break;
    }
    default:
        ss << "<unsupported>";
        break;
    }

    return ss.str();
}

std::string FormatElementValue(void* elemPtr, MonoTypeCode elemType) {
    if (!elemPtr || !IsValidPointer(elemPtr)) return "<invalid>";

    auto& bridge = MonoBridge::Get();
    std::ostringstream ss;

    switch (elemType) {
    case MonoTypeCode::Boolean:
        ss << (*(int8_t*)elemPtr ? "true" : "false");
        break;
    case MonoTypeCode::I1:
        ss << (int)(*(int8_t*)elemPtr);
        break;
    case MonoTypeCode::U1:
        ss << (unsigned)(*(uint8_t*)elemPtr);
        break;
    case MonoTypeCode::Char:
        ss << "'" << (char)(*(uint16_t*)elemPtr) << "'";
        break;
    case MonoTypeCode::I2:
        ss << *(int16_t*)elemPtr;
        break;
    case MonoTypeCode::U2:
        ss << *(uint16_t*)elemPtr;
        break;
    case MonoTypeCode::I4:
        ss << *(int32_t*)elemPtr;
        break;
    case MonoTypeCode::U4:
        ss << *(uint32_t*)elemPtr;
        break;
    case MonoTypeCode::I8:
        ss << *(int64_t*)elemPtr;
        break;
    case MonoTypeCode::U8:
        ss << *(uint64_t*)elemPtr;
        break;
    case MonoTypeCode::R4:
        ss << std::fixed << std::setprecision(4) << *(float*)elemPtr;
        break;
    case MonoTypeCode::R8:
        ss << std::fixed << std::setprecision(6) << *(double*)elemPtr;
        break;
    case MonoTypeCode::String: {
        void* strObj = *(void**)elemPtr;
        if (strObj && IsValidPointer(strObj)) {
            ss << "\"" << bridge.ExtractString(strObj) << "\"";
        } else {
            ss << "<null>";
        }
        break;
    }
    case MonoTypeCode::Class:
    case MonoTypeCode::Object:
    case MonoTypeCode::GenericInst: {
        void* objPtr = *(void**)elemPtr;
        if (objPtr && IsValidPointer(objPtr)) {
            void* objClass = MonoBridge::GetObjectClass(objPtr);
            if (objClass) {
                const char* typeName = MonoBridge::GetClassName(objClass);
                if (typeName) {
                    ss << typeName << "@0x" << std::hex << (uintptr_t)objPtr;
                } else {
                    ss << "obj@0x" << std::hex << (uintptr_t)objPtr;
                }
            } else {
                ss << "obj@0x" << std::hex << (uintptr_t)objPtr;
            }
        } else {
            ss << "<null>";
        }
        break;
    }
    default:
        ss << "0x" << std::hex << *(uint32_t*)elemPtr;
        break;
    }

    return ss.str();
}

std::string FormatInstanceFieldValue(void* instance, const FieldInfo& field) {
    if (!instance || !IsValidPointer(instance)) return "<null>";

    auto& bridge = MonoBridge::Get();
    std::ostringstream ss;

    switch (field.type) {
    case MonoTypeCode::Boolean: {
        int64_t val = 0;
        bridge.GetInstanceFieldValue(instance, field.handle, &val);
        return val ? "true" : "false";
    }

    case MonoTypeCode::I1:
    case MonoTypeCode::I2:
    case MonoTypeCode::I4: {
        int64_t val = 0;
        bridge.GetInstanceFieldValue(instance, field.handle, &val);
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
        uint64_t val = 0;
        bridge.GetInstanceFieldValue(instance, field.handle, &val);
        ss << (uint32_t)val;
        return ss.str();
    }

    case MonoTypeCode::I8: {
        int64_t val = 0;
        bridge.GetInstanceFieldValue(instance, field.handle, &val);
        ss << val;
        return ss.str();
    }

    case MonoTypeCode::U8: {
        uint64_t val = 0;
        bridge.GetInstanceFieldValue(instance, field.handle, &val);
        ss << val;
        return ss.str();
    }

    case MonoTypeCode::R4: {
        float val = 0.0f;
        bridge.GetInstanceFieldValue(instance, field.handle, &val);
        ss << std::fixed << std::setprecision(4) << val;
        return ss.str();
    }

    case MonoTypeCode::R8: {
        double val = 0.0;
        bridge.GetInstanceFieldValue(instance, field.handle, &val);
        ss << std::fixed << std::setprecision(6) << val;
        return ss.str();
    }

    case MonoTypeCode::String: {
        void* strPtr = nullptr;
        bridge.GetInstanceFieldValue(instance, field.handle, &strPtr);
        if (strPtr && IsValidPointer(strPtr)) { return "\"" + bridge.ExtractString(strPtr) + "\""; }
        return "<null>";
    }

    case MonoTypeCode::ValueType: {
        if (field.isEnum) {
            int64_t val = 0;
            bridge.GetInstanceFieldValue(instance, field.handle, &val);
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
        if (field.valueTypeClass) {
            const char* structName = MonoBridge::GetClassName(field.valueTypeClass);
            if (structName) {
                ss << "{" << structName << "}";
                return ss.str();
            }
        }
        return "<struct>";
    }

    case MonoTypeCode::Class:
    case MonoTypeCode::Object:
    case MonoTypeCode::GenericInst: {
        void* objPtr = nullptr;
        bridge.GetInstanceFieldValue(instance, field.handle, &objPtr);
        if (objPtr && IsValidPointer(objPtr)) {
            void* objClass = MonoBridge::GetObjectClass(objPtr);
            if (objClass) {
                // Check if it's a List<T>
                if (IsListType(objClass)) {
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

                const char* typeName = MonoBridge::GetClassName(objClass);
                if (typeName) {
                    ss << typeName << "@0x" << std::hex << (uintptr_t)objPtr;
                    return ss.str();
                }
            }
            ss << "obj@0x" << std::hex << (uintptr_t)objPtr;
            return ss.str();
        }
        return "<null>";
    }

    case MonoTypeCode::SzArray: {
        void* arrPtr = nullptr;
        bridge.GetInstanceFieldValue(instance, field.handle, &arrPtr);
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

StructContext MakeStructContext(uintptr_t baseAddr, void* klass, const std::string& name) {
    auto& bridge = MonoBridge::Get();
    bridge.SetupFields(klass);

    StructContext ctx;
    ctx.baseAddress = baseAddr;
    ctx.klassHandle = klass;
    ctx.typeName = name;

    MonoClass structClass(klass);
    ctx.fields = structClass.GetInstanceFields();

    // Calculate the minimum field offset to use as the base adjustment
    ctx.baseOffset = INT32_MAX;
    for (const auto& f : ctx.fields) {
        int32_t offset = *(int32_t*)((uintptr_t)f.handle + Offsets::FIELD_OFFSET);
        if (offset < ctx.baseOffset) { ctx.baseOffset = offset; }
    }
    if (ctx.baseOffset == INT32_MAX) ctx.baseOffset = 0;

    return ctx;
}

uintptr_t GetStructFieldAddress(const StructContext& ctx, const FieldInfo& field) {
    int32_t fieldOffset = *(int32_t*)((uintptr_t)field.handle + Offsets::FIELD_OFFSET);
    int32_t adjustedOffset = fieldOffset - ctx.baseOffset;
    return ctx.baseAddress + adjustedOffset;
}

bool RenderPaginationControls(int32_t totalCount, int& displayOffset, int pageSize, const char* idSuffix) {
    if (totalCount <= pageSize) return false;

    int oldOffset = displayOffset;

    int displayEnd = displayOffset + pageSize;
    if (displayEnd > totalCount) displayEnd = totalCount;
    ImGui::Text("Showing %d-%d of %d", displayOffset, displayEnd - 1, totalCount);

    char label[32];
    snprintf(label, sizeof(label), "<<%s", idSuffix);
    if (ImGui::Button(label)) displayOffset = 0;
    ImGui::SameLine();

    snprintf(label, sizeof(label), "<%s", idSuffix);
    if (ImGui::Button(label) && displayOffset > 0) {
        displayOffset -= pageSize;
        if (displayOffset < 0) displayOffset = 0;
    }
    ImGui::SameLine();

    snprintf(label, sizeof(label), ">%s", idSuffix);
    if (ImGui::Button(label) && displayOffset + pageSize < totalCount) { displayOffset += pageSize; }
    ImGui::SameLine();

    snprintf(label, sizeof(label), ">>%s", idSuffix);
    if (ImGui::Button(label)) {
        displayOffset = totalCount - pageSize;
        if (displayOffset < 0) displayOffset = 0;
    }

    return displayOffset != oldOffset;
}

void ApplyEditToAddress(void* addr, const FieldInfo& field, const EditState& editState) {
    if (!addr || !IsValidPointer(addr)) {
        LOG_ERROR("[FieldRendering] Invalid address for edit");
        return;
    }

    switch (field.type) {
    case MonoTypeCode::Boolean:
        *(int8_t*)addr = editState.editBool ? 1 : 0;
        break;
    case MonoTypeCode::I1:
        *(int8_t*)addr = (int8_t)editState.editInt;
        break;
    case MonoTypeCode::U1:
        *(uint8_t*)addr = (uint8_t)editState.editInt;
        break;
    case MonoTypeCode::I2:
        *(int16_t*)addr = (int16_t)editState.editInt;
        break;
    case MonoTypeCode::U2:
        *(uint16_t*)addr = (uint16_t)editState.editInt;
        break;
    case MonoTypeCode::I4:
    case MonoTypeCode::U4:
        if (field.isEnum && editState.editInt < (int)field.enumValues.size()) {
            *(int32_t*)addr = field.enumValues[editState.editInt].value;
        } else {
            *(int32_t*)addr = editState.editInt;
        }
        break;
    case MonoTypeCode::I8:
    case MonoTypeCode::U8:
        *(int64_t*)addr = editState.editInt64;
        break;
    case MonoTypeCode::R4:
        *(float*)addr = (float)editState.editDouble;
        break;
    case MonoTypeCode::R8:
        *(double*)addr = editState.editDouble;
        break;
    case MonoTypeCode::ValueType:
        if (field.isEnum && editState.editInt < (int)field.enumValues.size()) { *(int32_t*)addr = field.enumValues[editState.editInt].value; }
        break;
    default:
        LOG_WARN("[FieldRendering] Unsupported type for edit: " + field.name);
        return;
    }

    LOG_INFO("[FieldRendering] Set field " + field.name);
}

} // namespace S3MM
