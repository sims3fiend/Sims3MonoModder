#include "list_helpers.h"
#include "../core/mono_bridge.h"
#include <cstring>

namespace S3MM {

bool IsListType(void* klass) {
    if (!klass || !IsValidPointer(klass)) return false;

    const char* ns = MonoBridge::GetClassNamespace(klass);
    const char* name = MonoBridge::GetClassName(klass);

    if (!ns || !name) return false;

    // Check for System.Collections.Generic.List`1
    if (strcmp(ns, "System.Collections.Generic") == 0 && strncmp(name, "List`", 5) == 0) { return true; }

    return false;
}

bool GetListContents(void* listObj, void** outItemsArray, int32_t* outCount) {
    if (!listObj || !IsValidPointer(listObj)) return false;

    // Get the List's class
    void* klass = MonoBridge::GetObjectClass(listObj);
    if (!klass || !IsListType(klass)) return false;

    auto& bridge = MonoBridge::Get();
    bridge.SetupFields(klass);

    // List<T> internal fields:
    // - T[] _items  (backing array)
    // - int _size   (actual count)

    uint32_t fieldCount = *(uint32_t*)((uintptr_t)klass + Offsets::CLASS_FIELD_COUNT);
    void* fieldsPtr = *(void**)((uintptr_t)klass + Offsets::CLASS_FIELDS);

    if (!fieldsPtr || !IsValidPointer(fieldsPtr) || fieldCount == 0 || fieldCount > 100) { return false; }

    void* itemsArray = nullptr;
    int32_t count = 0;
    bool foundItems = false;
    bool foundSize = false;

    for (uint32_t i = 0; i < fieldCount && (!foundItems || !foundSize); i++) {
        uintptr_t fieldAddr = (uintptr_t)fieldsPtr + i * Offsets::FIELD_SIZE;

        const char* fieldName = *(const char**)(fieldAddr + Offsets::FIELD_NAME);
        if (!fieldName || !IsValidPointer((void*)fieldName)) continue;

        void* fieldType = *(void**)fieldAddr;
        if (!fieldType || !IsValidPointer(fieldType)) continue;

        MonoTypeCode typeCode = (MonoTypeCode)(*(uint8_t*)((uintptr_t)fieldType + Offsets::TYPE_CODE));

        if (strcmp(fieldName, "_items") == 0 && typeCode == MonoTypeCode::SzArray) {
            bridge.GetInstanceFieldValue(listObj, (void*)fieldAddr, &itemsArray);
            foundItems = true;
        } else if (strcmp(fieldName, "_size") == 0 && (typeCode == MonoTypeCode::I4 || typeCode == MonoTypeCode::U4)) {
            bridge.GetInstanceFieldValue(listObj, (void*)fieldAddr, &count);
            foundSize = true;
        }
    }

    if (foundItems && foundSize && itemsArray && IsValidPointer(itemsArray)) {
        *outItemsArray = itemsArray;
        *outCount = count;
        return true;
    }

    return false;
}

} // namespace S3MM
