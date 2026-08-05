#include "cache_api.h"
#include "../core/mono_bridge.h"
#include "../core/mono_types.h"
#include "../core/addresses.h"
#include "../util/logger.h"
#include "../core/exception_helper.h"
#include <cstring>
//https://www.youtube.com/watch?v=5roGS5WSLWQ S
namespace S3MM {

// HELPERS

// Get the raw value size for a value type based on MonoTypeCode
static uint32_t GetValueTypeRawSize(uint8_t typeCode, void* typeKlass) {
    switch (typeCode) {
    case 0x02:
        return 1; // Boolean
    case 0x03:
        return 2; // Char
    case 0x04:
        return 1; // I1 (sbyte)
    case 0x05:
        return 1; // U1 (byte)
    case 0x06:
        return 2; // I2 (short)
    case 0x07:
        return 2; // U2 (ushort)
    case 0x08:
        return 4; // I4 (int)
    case 0x09:
        return 4; // U4 (uint)
    case 0x0A:
        return 8; // I8 (long)
    case 0x0B:
        return 8; // U8 (ulong)
    case 0x0C:
        return 4; // R4 (float)
    case 0x0D:
        return 8; // R8 (double)
    case 0x11: {  // ValueType (struct)
        if (!typeKlass || !IsValidPointer(typeKlass)) return 0;
        uint32_t instanceSize = *(uint32_t*)((uintptr_t)typeKlass + Offsets::CLASS_INSTANCE_SIZE);
        // Subtract MonoObject header (4 bytes on 32-bit Mono 1.2)
        return (instanceSize > 4) ? (instanceSize - 4) : 0;
    }
    default:
        return 0;
    }
}

// Check if a MonoTypeCode represents a reference type
static bool IsReferenceType(uint8_t typeCode) {
    return typeCode == 0x0E     // String
           || typeCode == 0x12  // Class
           || typeCode == 0x1C  // Object
           || typeCode == 0x1D  // SzArray
           || typeCode == 0x15; // GenericInst
}

// Get MonoClass* for a value type field.
// For primitives (Bool..R8), data.klass in MonoType is unreliable in Sims3 Mono 1.2; use Addresses::Defaults (mono_defaults) instead.
// For ValueType (structs/enums), data.klass at MonoType+0x00 is valid.
static void* GetValueTypeKlass(uint8_t typeCode, void* monoType) {
    using namespace Addresses;
    uintptr_t addr = 0;
    switch (typeCode) {
    case 0x02:
        addr = Defaults.boolean_class;
        break; // MONO_TYPE_BOOLEAN
    case 0x03:
        addr = Defaults.char_class;
        break; // MONO_TYPE_CHAR
    case 0x04:
        addr = Defaults.sbyte_class;
        break; // MONO_TYPE_I1
    case 0x05:
        addr = Defaults.byte_class;
        break; // MONO_TYPE_U1
    case 0x06:
        addr = Defaults.int16_class;
        break; // MONO_TYPE_I2
    case 0x07:
        addr = Defaults.uint16_class;
        break; // MONO_TYPE_U2
    case 0x08:
        addr = Defaults.int32_class;
        break; // MONO_TYPE_I4
    case 0x09:
        addr = Defaults.uint32_class;
        break; // MONO_TYPE_U4
    case 0x0A:
        addr = Defaults.int64_class;
        break; // MONO_TYPE_I8
    case 0x0B:
        addr = Defaults.uint64_class;
        break; // MONO_TYPE_U8
    case 0x0C:
        addr = Defaults.single_class;
        break; // MONO_TYPE_R4
    case 0x0D:
        addr = Defaults.double_class;
        break;   // MONO_TYPE_R8
    case 0x11: { // ValueType (struct/enum) - data.klass is valid
        void* klass = *(void**)monoType;
        return (klass && IsValidPointer(klass)) ? klass : nullptr;
    }
    default:
        return nullptr;
    }
    return addr ? *(void**)addr : nullptr;
}

// FINDCLASS
void* __cdecl CacheAPI::NativeFindClass(void* assemblyNameStr, void* nsStr, void* classNameStr) {
    if (!assemblyNameStr || !classNameStr) return nullptr;

    MonoBridge& bridge = MonoBridge::Get();

    std::string assemblyName = bridge.ExtractString(assemblyNameStr);
    std::string ns = nsStr ? bridge.ExtractString(nsStr) : "";
    std::string className = bridge.ExtractString(classNameStr);

    if (assemblyName.empty() || className.empty()) return nullptr;

    // Initialize images if needed
    if (!bridge.AreImagesInitialized()) { bridge.InitializeAllImages(); }

    // Find the matching MonoImage by assembly name
    const auto& images = bridge.GetAllImages();
    for (void* image : images) {
        const char* name = MonoBridge::GetImageName(image);
        if (!name) continue;
        if (assemblyName == name) {
            // Found the image, call mono_class_from_name (goes through our detour cache)
            return bridge.GetClassFromNameInImage(image, ns.c_str(), className.c_str());
        }
    }

    // Fallback: try the game's ScriptHost::FindClass which searches all images
    return bridge.GetClassByName(ns.c_str(), className.c_str());
}

// FINDFIELD
void* __cdecl CacheAPI::NativeFindField(void* klass, void* fieldNameStr) {
    if (!klass || !fieldNameStr) return nullptr;

    MonoBridge& bridge = MonoBridge::Get();
    std::string fieldName = bridge.ExtractString(fieldNameStr);
    if (fieldName.empty()) return nullptr;

    // Calls mono_class_get_field_from_name (goes through our detour cache)
    return bridge.GetFieldFromName(klass, fieldName.c_str());
}

// FINDMETHOD
void* __cdecl CacheAPI::NativeFindMethod(void* klass, void* methodNameStr, int paramCount) {
    if (!klass || !methodNameStr) return nullptr;

    MonoBridge& bridge = MonoBridge::Get();
    std::string methodName = bridge.ExtractString(methodNameStr);
    if (methodName.empty()) return nullptr;

    // Call mono_class_get_method_from_name_flags directly via Addresses (goes through our detour cache).
    uintptr_t funcAddr = Addresses::Classes.mono_class_get_method_from_name_flags;
    if (!funcAddr) return nullptr;

    using method_from_name_t = void*(__cdecl*)(void* klass, const char* name, int param_count, int flags);
    auto fn = reinterpret_cast<method_from_name_t>(funcAddr);
    return fn(klass, methodName.c_str(), paramCount, 0);
}

// GETFIELD - Instance field access
void* __cdecl CacheAPI::NativeGetField(void* instance, void* field) {
    if (!instance || !field) return nullptr;

    MonoBridge& bridge = MonoBridge::Get();

    // Read MonoType* from MonoClassField+0x00
    void* monoType = *(void**)field;
    if (!monoType || !IsValidPointer(monoType)) return nullptr;

    uint8_t typeCode = *(uint8_t*)((uintptr_t)monoType + Offsets::TYPE_CODE);

    if (IsReferenceType(typeCode)) {
        // Reference type: read MonoObject* directly
        void* result = nullptr;
        bridge.GetInstanceFieldValue(instance, field, &result);
        return result;
    }

    // Value type: get the correct MonoClass* for boxing
    void* typeKlass = GetValueTypeKlass(typeCode, monoType);
    if (!typeKlass) return nullptr;

    uint32_t rawSize = GetValueTypeRawSize(typeCode, typeKlass);
    if (rawSize == 0 || rawSize > 128) return nullptr;

    uint8_t buffer[128];
    bridge.GetInstanceFieldValue(instance, field, buffer);

    return bridge.BoxValue(typeKlass, buffer);
}

// SETFIELD - Instance field access
void __cdecl CacheAPI::NativeSetField(void* instance, void* field, void* value) {
    if (!instance || !field) return;

    MonoBridge& bridge = MonoBridge::Get();

    void* monoType = *(void**)field;
    if (!monoType || !IsValidPointer(monoType)) return;

    uint8_t typeCode = *(uint8_t*)((uintptr_t)monoType + Offsets::TYPE_CODE);

    if (IsReferenceType(typeCode)) {
        // Reference type: pass pointer to the MonoObject*
        bridge.SetInstanceFieldValue(instance, field, &value);
        return;
    }

    // Value type: unbox the managed object to get raw data pointer (boxed value data starts at MonoObject + 4 after vtable on 32-bit).
    if (!value) return;
    void* rawData = (void*)((uintptr_t)value + 4);
    bridge.SetInstanceFieldValue(instance, field, rawData);
}

// GETSTATICFIELD
void* __cdecl CacheAPI::NativeGetStaticField(void* klass, void* field) {
    if (!klass || !field) return nullptr;

    MonoBridge& bridge = MonoBridge::Get();

    // Get vtable for the class
    void* vtable = bridge.GetClassVTable(klass);
    if (!vtable) return nullptr;

    void* monoType = *(void**)field;
    if (!monoType || !IsValidPointer(monoType)) return nullptr;

    uint8_t typeCode = *(uint8_t*)((uintptr_t)monoType + Offsets::TYPE_CODE);

    if (IsReferenceType(typeCode)) {
        void* result = nullptr;
        bridge.GetStaticFieldValue(vtable, field, &result);
        return result;
    }

    // Value type: get the correct MonoClass* for boxing
    void* typeKlass = GetValueTypeKlass(typeCode, monoType);
    if (!typeKlass) return nullptr;

    uint32_t rawSize = GetValueTypeRawSize(typeCode, typeKlass);
    if (rawSize == 0 || rawSize > 128) return nullptr;

    uint8_t buffer[128];
    bridge.GetStaticFieldValue(vtable, field, buffer);

    return bridge.BoxValue(typeKlass, buffer);
}

// SETSTATICFIELD
void __cdecl CacheAPI::NativeSetStaticField(void* klass, void* field, void* value) {
    if (!klass || !field) return;

    MonoBridge& bridge = MonoBridge::Get();

    void* vtable = bridge.GetClassVTable(klass);
    if (!vtable) return;

    void* monoType = *(void**)field;
    if (!monoType || !IsValidPointer(monoType)) return;

    uint8_t typeCode = *(uint8_t*)((uintptr_t)monoType + Offsets::TYPE_CODE);

    if (IsReferenceType(typeCode)) {
        bridge.SetStaticFieldValue(vtable, field, &value);
        return;
    }

    // Value type: unbox
    if (!value) return;
    void* rawData = (void*)((uintptr_t)value + 4);
    bridge.SetStaticFieldValue(vtable, field, rawData);
}

// INVOKE
void* __cdecl CacheAPI::NativeInvoke(void* method, void* instance, void* argsArray) {
    if (!method) return nullptr;

    MonoBridge& bridge = MonoBridge::Get();

    // Read method signature
    void* sig = *(void**)((uintptr_t)method + Offsets::METHOD_SIGNATURE);
    if (!sig || !IsValidPointer(sig)) return nullptr;

    uint16_t paramCount = *(uint16_t*)((uintptr_t)sig + Offsets::SIG_PARAM_COUNT);

    // Build unboxed params array on the stack
    void* unboxedParams[16] = {};
    void** params = nullptr;

    if (paramCount > 0) {
        if (paramCount > 16) {
            LOG_ERROR("[CacheAPI] Invoke: paramCount " + std::to_string(paramCount) + " exceeds max 16");
            return nullptr;
        }

        if (!argsArray) {
            LOG_ERROR("[CacheAPI] Invoke: method requires " + std::to_string(paramCount) + " params but argsArray is null");
            return nullptr;
        }

        // Validate array length
        int32_t arrayLen = *(int32_t*)((uintptr_t)argsArray + Offsets::ARRAY_LENGTH);
        if (arrayLen < (int32_t)paramCount) {
            LOG_ERROR("[CacheAPI] Invoke: array length " + std::to_string(arrayLen) + " < paramCount " + std::to_string(paramCount));
            return nullptr;
        }

        // Get array element pointers (MonoObject*[])
        void** elements = (void**)((uintptr_t)argsArray + Offsets::ARRAY_DATA);

        for (uint16_t i = 0; i < paramCount; i++) {
            // Read param MonoType* from signature
            void* paramType = *(void**)((uintptr_t)sig + Offsets::SIG_PARAMS + i * 4);
            if (!paramType || !IsValidPointer(paramType)) {
                unboxedParams[i] = elements[i];
                continue;
            }

            uint8_t typeCode = *(uint8_t*)((uintptr_t)paramType + Offsets::TYPE_CODE);

            if (IsReferenceType(typeCode)) {
                // Reference type: pass the MonoObject* as-is
                unboxedParams[i] = elements[i];
            } else {
                // Value type: unbox, raw data at MonoObject + 4
                if (elements[i]) {
                    unboxedParams[i] = (void*)((uintptr_t)elements[i] + 4);
                } else {
                    unboxedParams[i] = nullptr;
                }
            }
        }

        params = unboxedParams;
    }

    // Invoke via MonoBridge (SEH-protected)
    MonoInvokeState state = {};
    bool ok = bridge.InvokeMethod(method, instance, params, &state);
    if (!ok) {
        std::string excMsg = ExtractExceptionInfo(state.pThrowExc);
        LOG_WARN("[CacheAPI] Invoke exception: " + excMsg);
        return nullptr;
    }

    // Handle return value
    void* retType = *(void**)((uintptr_t)sig + Offsets::SIG_RET_TYPE);
    if (!retType || !IsValidPointer(retType)) return nullptr;

    uint8_t retTypeCode = *(uint8_t*)((uintptr_t)retType + Offsets::TYPE_CODE);

    // Void return
    if (retTypeCode == 0x01) return nullptr;

    // isObject is authoritative for reference types
    if (state.isObject) { return state.pRetVal; }

    // Fallback: typeCode says reference but isObject wasn't set
    if (IsReferenceType(retTypeCode)) {
        if (state.pRetVal) return state.pRetVal;
        if (state.pRet) return state.pRet;
        return nullptr;
    }

    // Value type: the invoke path has ALREADY boxed the return - interp_mono_invoke_with_state's setup helper (0xe52c70) 
    // pre-allocates pRetVal = mono_object_new(domain, retClass) with pRet = box+0x4, and RET_VT / the invoke tail's stackval_to_data fill the box payload (any width, wide structs included).
    // See HookManager::BoxInvokeResult
    if (state.pRetVal) return state.pRetVal;

    // Fallback: pre-box missing (setup helper never ran / unresolvable return class) - manual box
    void* valueKlass = GetValueTypeKlass(retTypeCode, retType);
    if (!valueKlass) {
        // Never hand back the raw pRet payload pointer as if it were an object - that is exactly the
        // pointer-shaped-garbage failure mode the terminals exist to prevent
        LOG_WARN("[CacheAPI] Invoke: unknown return type code " + std::to_string((int)retTypeCode) + " and no pre-boxed pRetVal - returning null");
        return nullptr;
    }

    if (!state.pRet) return nullptr;

    return bridge.BoxValue(valueKlass, state.pRet);
}

// FINDNESTEDCLASS
void* __cdecl CacheAPI::NativeFindNestedClass(void* parentKlass, void* nestedClassNameStr) {
    if (!parentKlass || !nestedClassNameStr) return nullptr;

    MonoBridge& bridge = MonoBridge::Get();
    std::string name = bridge.ExtractString(nestedClassNameStr);
    if (name.empty()) return nullptr;

    return bridge.GetNestedClass(parentKlass, name.c_str());
}

// GETOBJECTCLASS
void* __cdecl CacheAPI::NativeGetObjectClass(void* obj) {
    if (!obj) return nullptr;
    return MonoBridge::GetObjectClass(obj);
}

// GETCLASSNAME
void* __cdecl CacheAPI::NativeGetClassName(void* klass) {
    if (!klass) return nullptr;

    const char* name = MonoBridge::GetClassName(klass);
    if (!name || !name[0]) return nullptr;

    return MonoBridge::Get().CreateString(name);
}

// GETCLASSNAMESPACE
void* __cdecl CacheAPI::NativeGetClassNamespace(void* klass) {
    if (!klass) return nullptr;

    const char* ns = MonoBridge::GetClassNamespace(klass);
    if (!ns || !ns[0]) return nullptr;

    return MonoBridge::Get().CreateString(ns);
}

// GETPARENTCLASS
void* __cdecl CacheAPI::NativeGetParentClass(void* klass) {
    if (!klass) return nullptr;
    return MonoBridge::GetParentClass(klass);
}

// ISENUM
int32_t __cdecl CacheAPI::NativeIsEnum(void* klass) {
    if (!klass) return 0;
    return MonoBridge::IsEnumClass(klass) ? 1 : 0;
}

// GETMETHODNAME
void* __cdecl CacheAPI::NativeGetMethodName(void* method) {
    if (!method) return nullptr;

    std::string fullName = MonoBridge::GetMethodFullName(method);
    if (fullName.empty()) return nullptr;

    return MonoBridge::Get().CreateString(fullName.c_str());
}

// TYPED FIELD GETTERS/SETTERS - No boxing overhead
// Macros to generate boilerplate for each primitive type.
// The modder is responsible for calling the correct variant for the field type.
// IMPORTANT: Sims 3's mono_field_(static_)get/set_value may write up to 8 bytes (stackval-sized) regardless of actual field type.
// All buffers MUST be at least 8 bytes to prevent stack corruption.
// We use a uint64_t pad and memcpy the result out to avoid aliasing issues.

#define IMPL_GET_FIELD(TypeName, NativeType)                                                                                                                                                                                 \
    NativeType __cdecl CacheAPI::NativeGetField##TypeName(void* instance, void* field) {                                                                                                                                     \
        if (!instance || !field) return 0;                                                                                                                                                                                   \
        uint64_t buf = 0;                                                                                                                                                                                                    \
        MonoBridge::Get().GetInstanceFieldValue(instance, field, &buf);                                                                                                                                                      \
        NativeType result;                                                                                                                                                                                                   \
        memcpy(&result, &buf, sizeof(NativeType));                                                                                                                                                                           \
        return result;                                                                                                                                                                                                       \
    }

#define IMPL_SET_FIELD(TypeName, NativeType)                                                                                                                                                                                 \
    void __cdecl CacheAPI::NativeSetField##TypeName(void* instance, void* field, NativeType value) {                                                                                                                         \
        if (!instance || !field) return;                                                                                                                                                                                     \
        uint64_t buf = 0;                                                                                                                                                                                                    \
        memcpy(&buf, &value, sizeof(NativeType));                                                                                                                                                                            \
        MonoBridge::Get().SetInstanceFieldValue(instance, field, &buf);                                                                                                                                                      \
    }

#define IMPL_GET_STATIC_FIELD(TypeName, NativeType)                                                                                                                                                                          \
    NativeType __cdecl CacheAPI::NativeGetStaticField##TypeName(void* klass, void* field) {                                                                                                                                  \
        if (!klass || !field) return 0;                                                                                                                                                                                      \
        void* vtable = MonoBridge::Get().GetClassVTable(klass);                                                                                                                                                              \
        if (!vtable) return 0;                                                                                                                                                                                               \
        uint64_t buf = 0;                                                                                                                                                                                                    \
        MonoBridge::Get().GetStaticFieldValue(vtable, field, &buf);                                                                                                                                                          \
        NativeType result;                                                                                                                                                                                                   \
        memcpy(&result, &buf, sizeof(NativeType));                                                                                                                                                                           \
        return result;                                                                                                                                                                                                       \
    }

#define IMPL_SET_STATIC_FIELD(TypeName, NativeType)                                                                                                                                                                          \
    void __cdecl CacheAPI::NativeSetStaticField##TypeName(void* klass, void* field, NativeType value) {                                                                                                                      \
        if (!klass || !field) return;                                                                                                                                                                                        \
        void* vtable = MonoBridge::Get().GetClassVTable(klass);                                                                                                                                                              \
        if (!vtable) return;                                                                                                                                                                                                 \
        uint64_t buf = 0;                                                                                                                                                                                                    \
        memcpy(&buf, &value, sizeof(NativeType));                                                                                                                                                                            \
        MonoBridge::Get().SetStaticFieldValue(vtable, field, &buf);                                                                                                                                                          \
    }

#define IMPL_ALL_FIELD_OPS(TypeName, NativeType)                                                                                                                                                                             \
    IMPL_GET_FIELD(TypeName, NativeType)                                                                                                                                                                                     \
    IMPL_SET_FIELD(TypeName, NativeType)                                                                                                                                                                                     \
    IMPL_GET_STATIC_FIELD(TypeName, NativeType)                                                                                                                                                                              \
    IMPL_SET_STATIC_FIELD(TypeName, NativeType)

IMPL_ALL_FIELD_OPS(Int32, int32_t)
IMPL_ALL_FIELD_OPS(UInt32, uint32_t)
IMPL_ALL_FIELD_OPS(Int64, int64_t)
IMPL_ALL_FIELD_OPS(UInt64, uint64_t)
IMPL_ALL_FIELD_OPS(Float, float)
IMPL_ALL_FIELD_OPS(Double, double)
IMPL_ALL_FIELD_OPS(Bool, int32_t) // Mono InternalCall ABI: bool as int32_t
IMPL_ALL_FIELD_OPS(Byte, uint8_t)
IMPL_ALL_FIELD_OPS(SByte, int8_t)
IMPL_ALL_FIELD_OPS(Int16, int16_t)
IMPL_ALL_FIELD_OPS(UInt16, uint16_t)
IMPL_ALL_FIELD_OPS(Char, uint16_t) // C# char = UTF-16 code unit = uint16_t

#undef IMPL_GET_FIELD
#undef IMPL_SET_FIELD
#undef IMPL_GET_STATIC_FIELD
#undef IMPL_SET_STATIC_FIELD
#undef IMPL_ALL_FIELD_OPS

} // namespace S3MM
