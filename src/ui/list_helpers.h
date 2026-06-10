#pragma once
#include "../core/mono_types.h"
#include <cstdint>

namespace S3MM {

// LIST<T> HELPERS - Shared across panels

// Check if a MonoClass* is System.Collections.Generic.List`1
bool IsListType(void* klass);

// Extract the backing array and count from a List<T> instance.
// outItemsArray receives the T[] _items, outCount receives _size.
bool GetListContents(void* listObj, void** outItemsArray, int32_t* outCount);

} // namespace S3MM
