#pragma once
#include <cstdint>

namespace S3MM {

// CACHE API - Native internal call implementations exposed to managed code as S3MM.Cache.*
class CacheAPI {
  public:
    // Lookup functions (go through cached native Mono calls)
    static void* __cdecl NativeFindClass(void* assemblyNameStr, void* nsStr, void* classNameStr);
    static void* __cdecl NativeFindField(void* klass, void* fieldNameStr);
    static void* __cdecl NativeFindMethod(void* klass, void* methodNameStr, int paramCount);

    // Instance field access (auto-boxes/unboxes value types)
    static void* __cdecl NativeGetField(void* instance, void* field);
    static void __cdecl NativeSetField(void* instance, void* field, void* value);

    // Static field access (auto-boxes/unboxes value types)
    static void* __cdecl NativeGetStaticField(void* klass, void* field);
    static void __cdecl NativeSetStaticField(void* klass, void* field, void* value);

    // Typed instance field access (no boxing overhead)
    static int32_t __cdecl NativeGetFieldInt32(void* instance, void* field);
    static void __cdecl NativeSetFieldInt32(void* instance, void* field, int32_t value);
    static uint32_t __cdecl NativeGetFieldUInt32(void* instance, void* field);
    static void __cdecl NativeSetFieldUInt32(void* instance, void* field, uint32_t value);
    static int64_t __cdecl NativeGetFieldInt64(void* instance, void* field);
    static void __cdecl NativeSetFieldInt64(void* instance, void* field, int64_t value);
    static uint64_t __cdecl NativeGetFieldUInt64(void* instance, void* field);
    static void __cdecl NativeSetFieldUInt64(void* instance, void* field, uint64_t value);
    static float __cdecl NativeGetFieldFloat(void* instance, void* field);
    static void __cdecl NativeSetFieldFloat(void* instance, void* field, float value);
    static double __cdecl NativeGetFieldDouble(void* instance, void* field);
    static void __cdecl NativeSetFieldDouble(void* instance, void* field, double value);
    static int32_t __cdecl NativeGetFieldBool(void* instance, void* field);
    static void __cdecl NativeSetFieldBool(void* instance, void* field, int32_t value);
    static uint8_t __cdecl NativeGetFieldByte(void* instance, void* field);
    static void __cdecl NativeSetFieldByte(void* instance, void* field, uint8_t value);
    static int8_t __cdecl NativeGetFieldSByte(void* instance, void* field);
    static void __cdecl NativeSetFieldSByte(void* instance, void* field, int8_t value);
    static int16_t __cdecl NativeGetFieldInt16(void* instance, void* field);
    static void __cdecl NativeSetFieldInt16(void* instance, void* field, int16_t value);
    static uint16_t __cdecl NativeGetFieldUInt16(void* instance, void* field);
    static void __cdecl NativeSetFieldUInt16(void* instance, void* field, uint16_t value);
    static uint16_t __cdecl NativeGetFieldChar(void* instance, void* field);
    static void __cdecl NativeSetFieldChar(void* instance, void* field, uint16_t value);

    // Typed static field access (no boxing overhead)
    static int32_t __cdecl NativeGetStaticFieldInt32(void* klass, void* field);
    static void __cdecl NativeSetStaticFieldInt32(void* klass, void* field, int32_t value);
    static uint32_t __cdecl NativeGetStaticFieldUInt32(void* klass, void* field);
    static void __cdecl NativeSetStaticFieldUInt32(void* klass, void* field, uint32_t value);
    static int64_t __cdecl NativeGetStaticFieldInt64(void* klass, void* field);
    static void __cdecl NativeSetStaticFieldInt64(void* klass, void* field, int64_t value);
    static uint64_t __cdecl NativeGetStaticFieldUInt64(void* klass, void* field);
    static void __cdecl NativeSetStaticFieldUInt64(void* klass, void* field, uint64_t value);
    static float __cdecl NativeGetStaticFieldFloat(void* klass, void* field);
    static void __cdecl NativeSetStaticFieldFloat(void* klass, void* field, float value);
    static double __cdecl NativeGetStaticFieldDouble(void* klass, void* field);
    static void __cdecl NativeSetStaticFieldDouble(void* klass, void* field, double value);
    static int32_t __cdecl NativeGetStaticFieldBool(void* klass, void* field);
    static void __cdecl NativeSetStaticFieldBool(void* klass, void* field, int32_t value);
    static uint8_t __cdecl NativeGetStaticFieldByte(void* klass, void* field);
    static void __cdecl NativeSetStaticFieldByte(void* klass, void* field, uint8_t value);
    static int8_t __cdecl NativeGetStaticFieldSByte(void* klass, void* field);
    static void __cdecl NativeSetStaticFieldSByte(void* klass, void* field, int8_t value);
    static int16_t __cdecl NativeGetStaticFieldInt16(void* klass, void* field);
    static void __cdecl NativeSetStaticFieldInt16(void* klass, void* field, int16_t value);
    static uint16_t __cdecl NativeGetStaticFieldUInt16(void* klass, void* field);
    static void __cdecl NativeSetStaticFieldUInt16(void* klass, void* field, uint16_t value);
    static uint16_t __cdecl NativeGetStaticFieldChar(void* klass, void* field);
    static void __cdecl NativeSetStaticFieldChar(void* klass, void* field, uint16_t value);

    // Invocation
    static void* __cdecl NativeInvoke(void* method, void* instance, void* argsArray);

    // Nested class lookup
    static void* __cdecl NativeFindNestedClass(void* parentKlass, void* nestedClassNameStr);

    // Object/Class metadata
    static void* __cdecl NativeGetObjectClass(void* obj);
    static void* __cdecl NativeGetClassName(void* klass);
    static void* __cdecl NativeGetClassNamespace(void* klass);
    static void* __cdecl NativeGetParentClass(void* klass);
    static int32_t __cdecl NativeIsEnum(void* klass);
    static void* __cdecl NativeGetMethodName(void* method);
};

} // namespace S3MM
