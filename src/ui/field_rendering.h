#pragma once
#include "../core/mono_types.h"
#include "../core/mono_class.h"
#include <string>
#include <vector>
#include <cstdint>

namespace S3MM {

// FIELD RENDERING UTILITIES - Shared across panels

// Element size from MonoTypeCode (1/2/4/8)
int32_t GetElementSize(MonoTypeCode type);

// Format value at a raw memory address based on FieldInfo type -> display string.
// Used for struct fields, where you have a direct pointer to the data.
std::string FormatValueAtAddress(uintptr_t addr, const FieldInfo& field);

// Format a single array/list element at elemPtr by MonoTypeCode
std::string FormatElementValue(void* elemPtr, MonoTypeCode elemType);

// Format an instance field value: reads from instance via MonoBridge, then formats.
std::string FormatInstanceFieldValue(void* instance, const FieldInfo& field);

// Struct navigation context - shared definition
struct StructContext {
    uintptr_t baseAddress; // Memory address of the struct data
    void* klassHandle;     // MonoClass* of the struct type
    std::string typeName;  // Display name
    std::vector<FieldInfo> fields;
    int32_t baseOffset = 0; // Minimum field offset (for adjustment)
};

// Create a StructContext: loads fields and computes baseOffset
StructContext MakeStructContext(uintptr_t baseAddr, void* klass, const std::string& name);

// Compute the address of a field within a struct context
uintptr_t GetStructFieldAddress(const StructContext& ctx, const FieldInfo& field);

// Pagination UI (<<, <, >, >>), returns true if offset changed
bool RenderPaginationControls(int32_t totalCount, int& displayOffset, int pageSize, const char* idSuffix = "");

// Consolidated edit buffers
struct EditState {
    int32_t editInt = 0;
    int64_t editInt64 = 0;
    double editDouble = 0.0;
    bool editBool = false;
    char editString[256] = "";
    int editEnumIndex = 0;
};

// Write edit state values to a raw memory address based on field type
void ApplyEditToAddress(void* addr, const FieldInfo& field, const EditState& editState);

} // namespace S3MM
