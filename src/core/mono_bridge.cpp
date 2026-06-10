#include "mono_bridge.h"
#include "addresses.h"
#include "assembly_hooks.h"
#include "../util/logger.h"
#include <algorithm>
#include <vector>
#include <set>
#include <map>

namespace S3MM {

// Hooks, conflict detection, vanilla extraction, and assembly diffing live in separate modules (assembly_hooks, package_reader, il_parser, conflict_detector, vanilla_extractor, assembly_diff).

// HARDCODED ADDRESSES - Sims 3 Steam version
#define g_ScriptVM (*(void**)Addresses::Globals.g_ScriptVM)
#define MONO_DOMAIN_TLS_INDEX_ADDR Addresses::Globals.g_mono_domain_tls_index

// SINGLETON - Global access point
MonoBridge& MonoBridge::Get() {
    static MonoBridge instance;
    return instance;
}

// INITIALIZATION - Resolve all Mono function pointers
bool MonoBridge::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("[MonoBridge] Initializing...");

    // Set up function pointers
    m_gcMalloc = (GC_malloc_t)Addresses::Core.mono_gc_malloc;
    m_invokeWithState = (interp_mono_invoke_with_state_t)Addresses::Methods.interp_mono_invoke_with_state;
    m_classGetMethods = (mono_class_get_methods_t)Addresses::Classes.mono_class_get_methods;
    m_methodSignature = (mono_method_signature_t)Addresses::Methods.mono_method_signature;
    m_classGetFull = (mono_class_get_full_t)Addresses::Classes.mono_class_get_full;
    m_classSetupFields = (mono_class_setup_fields_t)Addresses::Classes.mono_class_setup_fields;
    m_gameGetClass = (game_get_class_t)Addresses::ScriptHost.ScriptHost_FindClass;
    m_classFromName = (mono_class_from_name_t)Addresses::Classes.mono_class_from_name;
    m_classVtable = (mono_class_vtable_t)Addresses::Classes.mono_class_vtable;
    m_fieldStaticGetValue = (mono_field_static_get_value_t)Addresses::Fields.mono_field_static_get_value;
    m_fieldStaticSetValue = (mono_field_static_set_value_t)Addresses::Fields.mono_field_static_set_value;
    m_fieldGetValue = (mono_field_get_value_t)Addresses::Fields.mono_field_get_value;
    m_fieldSetValue = (mono_field_set_value_t)Addresses::Fields.mono_field_set_value;
    m_domainGet = (mono_domain_get_t)Addresses::Core.mono_domain_get;
    m_stringNew = (mono_string_new_t)Addresses::Objects.mono_string_new;
    m_arrayNew = (mono_array_new_t)Addresses::Objects.mono_array_new;
    m_arrayClassGet = (mono_array_class_get_t)Addresses::Classes.mono_array_class_get;
    m_arrayElementSize = (mono_array_element_size_t)Addresses::Classes.mono_array_element_size;
    m_addInternalCall = (mono_add_internal_call_t)Addresses::InternalCalls.mono_add_internal_call;
    m_gcApplyToEachObject = (GC_apply_to_each_object_t)Addresses::Core.GC_apply_to_each_object;
    m_valueBox = (mono_value_box_t)Addresses::Core.mono_value_box;

    // Custom attribute functions
    m_customAttrsFromMethod = (mono_custom_attrs_from_method_t)Addresses::CustomAttrs.mono_custom_attrs_from_method;
    m_customAttrsHasAttr = (mono_custom_attrs_has_attr_t)Addresses::CustomAttrs.mono_custom_attrs_has_attr;
    m_customAttrsGetAttr = (mono_custom_attrs_get_attr_t)Addresses::CustomAttrs.mono_custom_attrs_get_attr;
    m_classGetFieldFromName = (mono_class_get_field_from_name_t)Addresses::Classes.mono_class_get_field_from_name;
    m_monoFree = (mono_free_t)Addresses::Core.mono_free;

    // Signature description
    m_signatureGetDesc = (mono_signature_get_desc_t)Addresses::Methods.mono_signature_get_desc;

    // Method header / IL access
    m_methodGetHeader = (mono_method_get_header_t)Addresses::Methods.mono_method_get_header;
    m_methodFullName = (mono_method_full_name_t)Addresses::Methods.mono_method_full_name;

    // MINT bytecode preparation (used by MethodPatcher to pre-compile hook proxies without executing them)
    m_createRuntimeMethod = (mono_interp_create_runtime_method_t)Addresses::Methods.SPEC_mono_interp_create_runtime_method;
    m_transformMethod = (interp_transform_method_t)Addresses::Methods.SPEC_interp_transform_method;

    m_initialized = true;
    LOG_INFO("[MonoBridge] Initialized");
    return true;
}

bool MonoBridge::IsRuntimeReady() const {
    return g_ScriptVM != nullptr;
}

// DOMAIN ACCESS - ScriptHost and TLS setup
void* MonoBridge::GetScriptHost() {
    return g_ScriptVM;
}

void* MonoBridge::GetDomain() {
    void* scriptHost = GetScriptHost();
    if (!scriptHost) return nullptr;
    return *(void**)((uintptr_t)scriptHost + Offsets::SCRIPTHOST_DOMAIN);
}

bool MonoBridge::SetupTLS() {
    void* scriptHost = GetScriptHost();
    if (!scriptHost) return false;

    void* domain = *(void**)((uintptr_t)scriptHost + Offsets::SCRIPTHOST_DOMAIN);
    DWORD tlsIndex = *(DWORD*)MONO_DOMAIN_TLS_INDEX_ADDR;
    TlsSetValue(tlsIndex, domain);
    return true;
}

// MEMORY ALLOCATION - GC-tracked alloc
void* MonoBridge::GCAlloc(uint32_t size) {
    if (!m_gcMalloc) return nullptr;
    return m_gcMalloc(size);
}

// STRING OPERATIONS - Create and extract managed strings
void* MonoBridge::CreateString(const char* utf8) {
    if (!utf8 || utf8[0] == '\0') return nullptr;
    if (!m_stringNew) return nullptr;

    void* domain = GetDomain();
    if (!domain) {
        LOG_ERROR("[MonoBridge] CreateString: Domain is null");
        return nullptr;
    }

    void* str = m_stringNew(domain, utf8);
    if (!str) { LOG_ERROR("[MonoBridge] CreateString failed for: \"" + std::string(utf8) + "\""); }
    return str;
}

std::string MonoBridge::ExtractString(void* monoString) {
    if (!IsValidPointer(monoString)) return "<null>";

    int32_t length = *(int32_t*)((uintptr_t)monoString + Offsets::STRING_LENGTH);
    if (length <= 0 || length > 4096) return "<invalid>";

    wchar_t* chars = (wchar_t*)((uintptr_t)monoString + Offsets::STRING_CHARS);
    if (!IsValidPointer(chars)) return "<invalid>";

    // Convert UTF-16 to UTF-8
    std::string result;
    result.reserve(length * 2);
    for (int32_t i = 0; i < length; i++) {
        uint16_t c = (uint16_t)chars[i];
        if (c < 0x80) {
            result += (char)c;
        } else if (c < 0x800) {
            result += (char)(0xC0 | (c >> 6));
            result += (char)(0x80 | (c & 0x3F));
        } else {
            result += (char)(0xE0 | (c >> 12));
            result += (char)(0x80 | ((c >> 6) & 0x3F));
            result += (char)(0x80 | (c & 0x3F));
        }
    }
    return result;
}

// CLASS LOOKUP - Resolve classes by name or token
void* MonoBridge::GetClassByName(const char* nameSpace, const char* className) {
    void* scriptHost = GetScriptHost();
    if (!scriptHost || !m_gameGetClass) return nullptr;
    return m_gameGetClass(scriptHost, nameSpace, className);
}

void* MonoBridge::GetClassFromToken(void* image, uint32_t token) {
    if (!m_classGetFull) return nullptr;
    return m_classGetFull(image, token, nullptr);
}

void* MonoBridge::GetClassFromNameInImage(void* image, const char* nameSpace, const char* className) {
    if (!m_classFromName || !image) return nullptr;
    return m_classFromName(image, nameSpace, className);
}

void* MonoBridge::GetNestedClass(void* parentKlass, const char* nestedClassName) {
    if (!parentKlass || !nestedClassName || !m_classGetFull) return nullptr;

    // Get the image from the parent class
    void* image = GetClassImage(parentKlass);
    if (!image || !IsValidPointer(image)) return nullptr;

    // Get TypeDef count from image
    int typeDefCount = GetTypeDefCount(image);
    if (typeDefCount <= 0 || typeDefCount > 100000) return nullptr;

    // Iterate all TypeDefs looking for a class whose DeclaringClass matches parentKlass
    for (int i = 1; i <= typeDefCount; i++) {
        uint32_t token = TOKEN_TYPE_DEF | i;
        void* klass = GetClassFromToken(image, token);

        if (!klass || !IsValidPointer(klass)) continue;

        // Check if this class is nested in our parent
        void* declaring = GetDeclaringClass(klass);
        if (declaring != parentKlass) continue;

        // Check if the name matches
        const char* name = GetClassName(klass);
        if (name && strcmp(name, nestedClassName) == 0) { return klass; }
    }

    return nullptr;
}

// CLASS METADATA - Static helpers for class introspection
const char* MonoBridge::GetClassName(void* klass) {
    if (!klass) return nullptr;
    return *(const char**)((uintptr_t)klass + Offsets::CLASS_NAME);
}

const char* MonoBridge::GetClassNamespace(void* klass) {
    if (!klass) return nullptr;
    return *(const char**)((uintptr_t)klass + Offsets::CLASS_NAMESPACE);
}

void* MonoBridge::GetDeclaringClass(void* klass) {
    if (!klass) return nullptr;
    return *(void**)((uintptr_t)klass + Offsets::CLASS_NESTED_IN);
}

void* MonoBridge::GetParentClass(void* klass) {
    if (!klass) return nullptr;
    return *(void**)((uintptr_t)klass + Offsets::CLASS_PARENT);
}

bool MonoBridge::IsEnumClass(void* klass) {
    if (!klass || !IsValidPointer(klass)) return false;

    void* parent = GetParentClass(klass);
    if (!parent || !IsValidPointer(parent)) return false;

    const char* parentName = GetClassName(parent);
    const char* parentNs = GetClassNamespace(parent);

    if (!parentName || !parentNs) return false;
    return (strcmp(parentName, "Enum") == 0 && strcmp(parentNs, "System") == 0);
}

std::string MonoBridge::GetMethodFullName(void* method) {
    if (!method || !IsValidPointer(method)) return "<unknown>";

    // Read klass and name directly from MonoMethod structure
    void* klass = *(void**)((uintptr_t)method + Offsets::METHOD_KLASS);
    const char* methodName = *(const char**)((uintptr_t)method + Offsets::METHOD_NAME);

    if (!klass || !IsValidPointer(klass) || !methodName || !IsValidPointer((void*)methodName)) { return "<unknown>"; }

    const char* className = GetClassName(klass);
    const char* nameSpace = GetClassNamespace(klass);

    std::string fullName;
    if (nameSpace && nameSpace[0]) { fullName = std::string(nameSpace) + "."; }
    if (className) { fullName += std::string(className) + "::"; }
    if (methodName) { fullName += methodName; }

    return fullName.empty() ? "<unknown>" : fullName;
}

void* MonoBridge::GetObjectClass(void* object) {
    if (!object || !IsValidPointer(object)) return nullptr;

    // In Mono, every object starts with a vtable pointer at offset 0
    // The vtable contains a pointer to the class at offset 0
    void* vtable = *(void**)object;
    if (!vtable || !IsValidPointer(vtable)) return nullptr;

    // VTable layout: first pointer is the MonoClass*
    void* klass = *(void**)vtable;
    if (!klass || !IsValidPointer(klass)) return nullptr;

    return klass;
}

MonoTypeCode MonoBridge::GetTypeCodeFromClass(void* klass) {
    if (!klass || !IsValidPointer(klass)) return MonoTypeCode::Object;

    const char* name = GetClassName(klass);
    const char* ns = GetClassNamespace(klass);

    // Safety check - if we can't read names, the pointer might be bad
    if (!name || !IsValidPointer((void*)name)) {
        LOG_WARN("[MonoBridge] GetTypeCodeFromClass: invalid name pointer for klass=0x" + std::to_string((uintptr_t)klass));
        return MonoTypeCode::Object;
    }

    LOG_DEBUG("[MonoBridge] GetTypeCodeFromClass: ns='" + std::string(ns ? ns : "null") + "' name='" + std::string(name ? name : "null") + "'");

    // Check for System primitive types by class name
    if (ns && strcmp(ns, "System") == 0 && name) {
        if (strcmp(name, "Single") == 0) return MonoTypeCode::R4;
        if (strcmp(name, "Double") == 0) return MonoTypeCode::R8;
        if (strcmp(name, "Int32") == 0) return MonoTypeCode::I4;
        if (strcmp(name, "UInt32") == 0) return MonoTypeCode::U4;
        if (strcmp(name, "Int64") == 0) return MonoTypeCode::I8;
        if (strcmp(name, "UInt64") == 0) return MonoTypeCode::U8;
        if (strcmp(name, "Int16") == 0) return MonoTypeCode::I2;
        if (strcmp(name, "UInt16") == 0) return MonoTypeCode::U2;
        if (strcmp(name, "Byte") == 0) return MonoTypeCode::U1;
        if (strcmp(name, "SByte") == 0) return MonoTypeCode::I1;
        if (strcmp(name, "Boolean") == 0) return MonoTypeCode::Boolean;
        if (strcmp(name, "Char") == 0) return MonoTypeCode::Char;
        if (strcmp(name, "String") == 0) return MonoTypeCode::String;
        if (strcmp(name, "Object") == 0) return MonoTypeCode::Object;
    }

    // Check if it's an enum
    if (IsEnumClass(klass)) { return MonoTypeCode::ValueType; }

    // Check if it's a value type by checking parent
    void* parent = GetParentClass(klass);
    if (parent && IsValidPointer(parent)) {
        const char* parentName = GetClassName(parent);
        const char* parentNs = GetClassNamespace(parent);
        if (parentNs && parentName && strcmp(parentNs, "System") == 0 && strcmp(parentName, "ValueType") == 0) { return MonoTypeCode::ValueType; }
    }

    // Default to class/object type
    return MonoTypeCode::Class;
}

MonoTypeCode MonoBridge::GetArrayElementType(void* arrayClass) {
    if (!arrayClass || !IsValidPointer(arrayClass)) return MonoTypeCode::Object;

    // Get the array class name - for primitive arrays like "Single[]", "Int32[]", etc.
    const char* arrClassName = GetClassName(arrayClass);
    const char* arrClassNs = GetClassNamespace(arrayClass);

    // For primitive arrays in System namespace, parse the type from the class name
    // Names are like "Single[]", "Int32[]", "Boolean[]", etc.
    if (arrClassName && arrClassNs && strcmp(arrClassNs, "System") == 0) {
        // Check for primitive array types by name prefix
        if (strncmp(arrClassName, "Single[", 7) == 0) return MonoTypeCode::R4;
        if (strncmp(arrClassName, "Double[", 7) == 0) return MonoTypeCode::R8;
        if (strncmp(arrClassName, "Int32[", 6) == 0) return MonoTypeCode::I4;
        if (strncmp(arrClassName, "UInt32[", 7) == 0) return MonoTypeCode::U4;
        if (strncmp(arrClassName, "Int64[", 6) == 0) return MonoTypeCode::I8;
        if (strncmp(arrClassName, "UInt64[", 7) == 0) return MonoTypeCode::U8;
        if (strncmp(arrClassName, "Int16[", 6) == 0) return MonoTypeCode::I2;
        if (strncmp(arrClassName, "UInt16[", 7) == 0) return MonoTypeCode::U2;
        if (strncmp(arrClassName, "Byte[", 5) == 0) return MonoTypeCode::U1;
        if (strncmp(arrClassName, "SByte[", 6) == 0) return MonoTypeCode::I1;
        if (strncmp(arrClassName, "Boolean[", 8) == 0) return MonoTypeCode::Boolean;
        if (strncmp(arrClassName, "Char[", 5) == 0) return MonoTypeCode::Char;
        if (strncmp(arrClassName, "String[", 7) == 0) return MonoTypeCode::String;
        if (strncmp(arrClassName, "Object[", 7) == 0) return MonoTypeCode::Object;
    }

    // Try getting element class from CLASS_ELEMENT_CLASS at offset 0x30
    void* elemClass = *(void**)((uintptr_t)arrayClass + Offsets::CLASS_ELEMENT_CLASS);
    if (elemClass && IsValidPointer(elemClass)) { return GetTypeCodeFromClass(elemClass); }

    // For non-System arrays, try to determine type from class name pattern "TypeName[]"
    if (arrClassName) {
        size_t len = strlen(arrClassName);
        if (len > 2 && arrClassName[len - 1] == ']' && arrClassName[len - 2] == '[') {
            // It's an array type, but we couldn't get the element class
            // Check if it might be a struct/value type or class
            return MonoTypeCode::ValueType; // Assume struct for non-primitive arrays
        }
    }

    return MonoTypeCode::Object; // Default fallback
}

void* MonoBridge::GetArrayElementClass(void* arrayClass) {
    if (!arrayClass || !IsValidPointer(arrayClass)) return nullptr;

    // Get element class from CLASS_ELEMENT_CLASS at offset 0x30
    void* elemClass = *(void**)((uintptr_t)arrayClass + Offsets::CLASS_ELEMENT_CLASS);
    if (elemClass && IsValidPointer(elemClass)) { return elemClass; }

    return nullptr;
}

// METHOD ENUMERATION - Iterate and inspect methods
void* MonoBridge::GetNextMethod(void* klass, int* iter) {
    if (!m_classGetMethods) return nullptr;
    return m_classGetMethods(klass, iter);
}

void* MonoBridge::GetMethodSignature(void* method) {
    if (!method) return nullptr;

    // Try cached signature first
    void* sig = *(void**)((uintptr_t)method + Offsets::METHOD_SIGNATURE);
    if (sig && IsValidPointer(sig)) return sig;

    // Fall back to function call
    if (!m_methodSignature) return nullptr;
    return m_methodSignature(method);
}

std::string MonoBridge::GetSignatureDescription(void* signature, bool includeNamespace) {
    if (!signature || !m_signatureGetDesc || !m_monoFree) { return ""; }

    char* desc = m_signatureGetDesc(signature, includeNamespace ? 1 : 0);
    if (!desc) { return ""; }

    std::string result(desc);
    m_monoFree(desc);
    return result;
}

// FIELD OPERATIONS - Static and instance field access
void MonoBridge::SetupFields(void* klass) {
    if (m_classSetupFields && klass) { m_classSetupFields(klass); }
}

void* MonoBridge::GetClassVTable(void* klass) {
    if (!m_classVtable || !klass) return nullptr;
    void* domain = GetDomain();
    if (!domain) return nullptr;
    return m_classVtable(domain, klass);
}

void MonoBridge::GetStaticFieldValue(void* vtable, void* field, void* outValue) {
    if (m_fieldStaticGetValue && vtable && field) { m_fieldStaticGetValue(vtable, field, outValue); }
}

void MonoBridge::SetStaticFieldValue(void* vtable, void* field, void* value) {
    if (m_fieldStaticSetValue && vtable && field) { m_fieldStaticSetValue(vtable, field, value); }
}

void MonoBridge::GetInstanceFieldValue(void* object, void* field, void* outValue) {
    if (m_fieldGetValue && object && field) { m_fieldGetValue(object, field, outValue); }
}

void MonoBridge::SetInstanceFieldValue(void* object, void* field, void* value) {
    if (m_fieldSetValue && object && field) { m_fieldSetValue(object, field, value); }
}

// ARRAY OPERATIONS - Create and access managed arrays
void* MonoBridge::CreateArray(void* elementClass, int32_t length) {
    if (!m_arrayNew || !elementClass || length < 0) return nullptr;

    void* domain = GetDomain();
    if (!domain) {
        LOG_ERROR("[MonoBridge] CreateArray: Domain is null");
        return nullptr;
    }

    return m_arrayNew(domain, elementClass, length);
}

int32_t MonoBridge::GetArrayLength(void* monoArray) {
    if (!IsValidPointer(monoArray)) return 0;
    return *(int32_t*)((uintptr_t)monoArray + Offsets::ARRAY_LENGTH);
}

void* MonoBridge::GetArrayElementPtr(void* monoArray, int32_t index, int32_t elementSize) {
    if (!IsValidPointer(monoArray) || index < 0 || elementSize <= 0) return nullptr;

    int32_t length = GetArrayLength(monoArray);
    if (index >= length) return nullptr;

    return (void*)((uintptr_t)monoArray + Offsets::ARRAY_DATA + (index * elementSize));
}

int32_t MonoBridge::GetArrayElementSize(void* arrayClass) {
    if (!m_arrayElementSize || !IsValidPointer(arrayClass)) return 0;

    // The mono_array_element_size function asserts that byte at offset 0x10 is non-zero
    // If it's zero, the class isn't properly initialized and calling the function will crash
    uint8_t rankByte = *(uint8_t*)((uintptr_t)arrayClass + 0x10);
    if (rankByte == 0) {
        LOG_ERROR("[MonoBridge] GetArrayElementSize: array class not initialized (rank=0)");
        return 0;
    }

    return m_arrayElementSize(arrayClass);
}

// INTERNAL CALLS - Register native methods
void MonoBridge::AddInternalCall(const char* name, void* method) {
    if (!m_addInternalCall) {
        LOG_ERROR("[MonoBridge] AddInternalCall: function not initialized");
        return;
    }
    if (!name || !method) {
        LOG_ERROR("[MonoBridge] AddInternalCall: null parameter");
        return;
    }
    m_addInternalCall(name, method);
    LOG_INFO("[MonoBridge] Registered internal call: " + std::string(name));
}

// METHOD INVOCATION - SEH safety net for Mono 1.2 interpreter
//
// The interpreter faults on the first trampoline invocation before its lazy state is initialized.
// That faulting call performs the initialization as a side-effect, so a retry succeeds;
// InvokeWithSEH catches the fault and retries once.
//
// When SEH catches an interpreter crash, the Mono thread context (TLS) is left with dangling
// pointers to stack frames that were unwound.
// interp_mono_invoke_with_state saves threadCtx[2..4] (previous frame, top frame, longjmp buffer)
// into locals and restores them on normal return,
// but SEH unwind destroys those locals without restoring.
// Subsequent game-initiated interpreter calls then crash on the corrupt frame chain,
// so we save/restore the thread context frame chain ourselves around each invoke attempt.

// DIAGNOSTIC - capture the faulting address/code of the most recent transform crash so we can
// pinpoint what mono_interp_transform_method dereferences on a hot-reload image.
static thread_local void* s_lastTransformFaultAddr = nullptr;
static thread_local uint32_t s_lastTransformFaultCode = 0;
static int TransformFaultFilter(EXCEPTION_POINTERS* ep) {
    s_lastTransformFaultAddr = ep->ExceptionRecord->ExceptionAddress;
    s_lastTransformFaultCode = ep->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}

// Save the 3 frame-chain slots from the Mono thread context that interp_mono_invoke_with_state
// stores in locals and restores on normal return. SEH unwind skips that restore.
struct ThreadCtxSnapshot {
    void** ctx;     // TLS thread context pointer
    void* frame;    // ctx[2]: previous/current frame
    void* topFrame; // ctx[3]: top frame
    void* jmpBuf;   // ctx[4]: longjmp exception buffer

    static ThreadCtxSnapshot Capture() {
        ThreadCtxSnapshot snap = {};
        DWORD tlsIndex = *(DWORD*)Addresses::Profiler.g_thread_context_tls_index;
        snap.ctx = (void**)TlsGetValue(tlsIndex);
        if (snap.ctx) {
            snap.frame = snap.ctx[2];
            snap.topFrame = snap.ctx[3];
            snap.jmpBuf = snap.ctx[4];
        }
        return snap;
    }

    void Restore() const {
        if (ctx) {
            ctx[2] = frame;
            ctx[3] = topFrame;
            ctx[4] = jmpBuf;
        }
    }
};

static int InvokeWithSEH(void(__cdecl* invokeFunc)(void*), MonoInvokeState* state, void* method, void* thisPtr, void** params) {
    ThreadCtxSnapshot snap = ThreadCtxSnapshot::Capture();

    __try {
        invokeFunc(state);
        return 1; // Success on first try
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Restore thread context frame chain before retry - SEH unwind left dangling pointers
        snap.Restore();

        // Clear state and try again (the crash triggered initialization)
        memset(state, 0, sizeof(MonoInvokeState));
        state->pMethod = method;
        state->pThis = thisPtr;
        state->ppParams = params;
        state->cur_exec_num = -1;
        state->max_depth = -1;
        state->setupFrame = 1;

        __try {
            invokeFunc(state);
            return 2; // Success after retry
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Second failure - restore context again and give up.
            // Do NOT set pThrowExc to a sentinel value here. The (void*)1 sentinel leaks into the eval stack exception slot via corrupt
            // InterpFrame frames (SEH unwind skips interpreter frame cleanup), causing an ACCESS_VIOLATION on deref as MonoObject*.
            // Callers must check InvokeMethod's return value instead.
            snap.Restore();
            return 0;
        }
    }
}

// METHOD TRANSFORM - SEH-wrapped MINT preparation (no execution)
// Returns: 1=already transformed, 2=transformed this call, 0=failure (caller logs).
// Sets *outRtm to the RuntimeMethod* (nullable on failure) and *outRc to the transform return code.
static int TransformWithSEH(void*(__cdecl* createRuntimeMethod)(void*), int(__cdecl* transformMethod)(void*), void* method, void* threadCtxSeed, void** outRtm, int* outRc) {
    ThreadCtxSnapshot snap = ThreadCtxSnapshot::Capture();
    *outRtm = nullptr;
    *outRc = 0;

    // mono_interp_transform_method reads the thread context (ctx[3]=topFrame, ctx[4]=jmpBuf) from g_thread_context_tls_index.
    // When we transform from the ProcessTasks drain outside any active interp call, that TLS is null, so transform faults at +0x55 dereferencing ctx[3].
    // The engine's own out-of-interp transform path (00e52b90) handles this by installing a temporary zeroed thread context for the duration, then restoring.
    // Mirror that: only override when the TLS is genuinely null
    DWORD tlsIndex = *(DWORD*)Addresses::Profiler.g_thread_context_tls_index;
    void* savedCtx = TlsGetValue(tlsIndex);

    uint32_t frameRegion[10] = {0}; // zeroed frame the temp context's frame slots point at
    void* tempCtx[5] = {threadCtxSeed, frameRegion, frameRegion, frameRegion, nullptr};
    bool installedTemp = (savedCtx == nullptr);
    if (installedTemp) { TlsSetValue(tlsIndex, tempCtx); }

    __try {
        void* rtm = createRuntimeMethod(method);
        *outRtm = rtm;
        if (!rtm) {
            if (installedTemp) TlsSetValue(tlsIndex, savedCtx);
            return 0;
        }

        // Bit 0x100 at RTM+0x44 = "already transformed"; both wrappers in the engine short-circuit on it.
        uint32_t flags = *(uint32_t*)((uintptr_t)rtm + 0x44);
        if ((flags & 0x100) != 0) {
            if (installedTemp) TlsSetValue(tlsIndex, savedCtx);
            return 1;
        }

        *outRc = transformMethod(rtm);
        if (installedTemp) TlsSetValue(tlsIndex, savedCtx);
        return (*outRc == 0) ? 2 : 0;
    } __except (TransformFaultFilter(GetExceptionInformation())) {
        // Same dangling-frame-chain hazard as InvokeWithSEH, restore the slots so subsequent game-initiated interpreter calls don't walk corrupt chain
        if (installedTemp) TlsSetValue(tlsIndex, savedCtx);
        snap.Restore();
        return 0;
    }
}

bool MonoBridge::TransformMethod(void* method) {
    if (!m_createRuntimeMethod || !m_transformMethod || !method) {
        LOG_ERROR("[MonoBridge] TransformMethod: function pointers or method null");
        return false;
    }

    SetupTLS();

    // Seed for the temporary thread context's ctx[0] when transforming outside an interp call (hot-reload).
    // The engine's helper stores a context object here, the domain is the safe, always-valid analogue. transform's early path only reads ctx[3]/ctx[4], so this is belt-and-suspenders ;__;
    void* domain = GetDomain();

    void* rtm = nullptr;
    int rc = 0;
    int result = TransformWithSEH(m_createRuntimeMethod, m_transformMethod, method, domain, &rtm, &rc);

    if (result == 0) {
        if (!rtm) {
            LOG_ERROR("[MonoBridge] TransformMethod: get_imethod returned null (domain missing? or SEH caught)");
        } else {
            LOG_ERROR("[MonoBridge] TransformMethod: transform failed rc=" + std::to_string(rc) + " faultAddr=" + ToHex((uintptr_t)s_lastTransformFaultAddr) + " code=" + ToHex(s_lastTransformFaultCode));
        }
        return false;
    }

    return true;
}

// METHOD INVOCATION - Main entry point with SEH protection
bool MonoBridge::InvokeMethod(void* method, void* thisPtr, void** params, MonoInvokeState* state) {
    if (!m_invokeWithState || !method || !state) return false;

    memset(state, 0, sizeof(MonoInvokeState));
    state->pMethod = method;
    state->pThis = thisPtr;
    state->ppParams = params;
    state->cur_exec_num = -1;
    state->max_depth = -1;
    state->setupFrame = 1;

    // Use SEH wrapper to handle Mono interpreter initialization crashes (see InvokeWithSEH comment)
    int result = InvokeWithSEH(m_invokeWithState, state, method, thisPtr, params);

    if (result == 2) {
        LOG_DEBUG("[MonoBridge] InvokeMethod: SEH retry succeeded");
    } else if (result == 0) {
        LOG_ERROR("[MonoBridge] InvokeMethod: SEH retry also failed");
        return false; // Both attempts crashed, fail regardless of pThrowExc
    }

    return state->pThrowExc == nullptr;
}

// THREAD-MARSHALLED INVOCATION - thin wrapper over RunOnSimThread
bool MonoBridge::InvokeMethodOnSimThread(void* method, void* thisPtr, void** params, MonoInvokeState* state, DWORD timeoutMs) {
    if (!method || !state) return false;
    bool invokeOk = false;
    bool ran = RunOnSimThread([&]() { invokeOk = InvokeMethod(method, thisPtr, params, state); }, timeoutMs);
    return ran && invokeOk;
}

// RUN ON SIM THREAD - enqueue a task for the sim thread, or run inline if already on it
bool MonoBridge::RunOnSimThread(std::function<void()> task, DWORD timeoutMs) {
    if (!task) return false;

    // On the sim thread (or before the sim thread is known, the only pre-drain callers are cold-start callbacks that already run on the game thread) run inline
    // Enqueuing from the sim thread would deadlock. This also makes nested calls (e.g. a marshalled hot-reload that internally invokes managed code) execute inline
    DWORD simTid = m_simThreadId.load(std::memory_order_acquire);
    if (simTid == 0 || GetCurrentThreadId() == simTid) {
        task();
        return true;
    }

    SimTask entry;
    entry.fn = std::move(task);
    entry.done = CreateEventA(nullptr, TRUE, FALSE, nullptr); // manual-reset
    if (!entry.done) {
        LOG_ERROR("[MonoBridge] RunOnSimThread: CreateEvent failed");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_pendingTasks.push_back(&entry);
    }

    DWORD wait = WaitForSingleObject(entry.done, timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        // Timed out. Either the sim thread never picked it up (safe to drop), or it picked it up
        // and is mid-run (must NOT free - our stack-local `entry`/captures are still in use).
        bool stillQueued = false;
        {
            std::lock_guard<std::mutex> lock(m_taskMutex);
            auto it = std::find(m_pendingTasks.begin(), m_pendingTasks.end(), &entry);
            if (it != m_pendingTasks.end()) {
                m_pendingTasks.erase(it);
                stillQueued = true;
            }
        }
        if (stillQueued) {
            LOG_WARN("[MonoBridge] RunOnSimThread: timed out (" + std::to_string(timeoutMs) + "ms) - sim thread not pumping");
            CloseHandle(entry.done);
            return false;
        }
        // Already dequeued and running on the sim thread - block until it signals completion.
        WaitForSingleObject(entry.done, INFINITE);
    }

    CloseHandle(entry.done);
    return true;
}

// DRAIN SIM TASKS - run queued cross-thread tasks on the sim thread (ProcessTasks detour)
void MonoBridge::DrainSimTasks() {
    // Record the sim thread identity on first run so producers can detect on-thread callers.
    if (m_simThreadId.load(std::memory_order_relaxed) == 0) { m_simThreadId.store(GetCurrentThreadId(), std::memory_order_release); }

    // Drain only what was queued at frame entry; a task may enqueue more (or its inline nested calls might), and we don't want to spin within a single frame
    size_t budget;
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        budget = m_pendingTasks.size();
    }

    for (size_t i = 0; i < budget; i++) {
        SimTask* t = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_taskMutex);
            if (m_pendingTasks.empty()) break;
            t = m_pendingTasks.front();
            m_pendingTasks.pop_front();
        }
        // Run outside the lock - tasks are slow and may re-enter the queue
        t->fn();
        SetEvent(t->done); // hands ownership of `t` back to the waiting producer
    }
}

// VALUE BOXING - Box value types into managed objects
void* MonoBridge::BoxValue(void* klass, void* valuePtr) {
    if (!m_valueBox || !klass || !valuePtr) return nullptr;

    void* domain = GetDomain();
    if (!domain) {
        LOG_ERROR("[MonoBridge] BoxValue: Domain is null");
        return nullptr;
    }

    return m_valueBox(domain, klass, valuePtr);
}

int MonoBridge::GetTypeDefCount(void* image) {
    if (!image || !IsValidPointer(image)) return 0;
    uint32_t value = *(uint32_t*)((uintptr_t)image + 0x80);
    return (int)(value & 0x00FFFFFF);
}

// ICALL TABLE DUMP - Walk the GHashTable at DAT_011f436c
// GHashTable: [0]=hash_func [1]=key_equal_func [2]=nodes [3]=size [4]=nnodes
// GHashNode: [0]=key (char*) [1]=value (void*) [2]=next
void MonoBridge::DumpIcallTable() {
    LOG_INFO("=== DUMPING ICALL HASH TABLE ===");

    // EA icall hash table global at 0x011f436c
    constexpr uintptr_t ICALL_HASH_TABLE_ADDR = 0x011f436c;

    void* hashTable = *(void**)ICALL_HASH_TABLE_ADDR;
    LOG_INFO("[Icall] Hash table ptr @ " + ToHex(ICALL_HASH_TABLE_ADDR) + " = " + ToHex((uintptr_t)hashTable));

    if (!hashTable || !IsValidPointer(hashTable)) {
        LOG_ERROR("[Icall] Invalid hash table pointer");
        return;
    }

    // Read GHashTable fields
    void* hashFunc = *(void**)((uintptr_t)hashTable + 0x00);
    void* equalFunc = *(void**)((uintptr_t)hashTable + 0x04);
    void** nodes = *(void***)((uintptr_t)hashTable + 0x08);
    int size = *(int*)((uintptr_t)hashTable + 0x0C);
    int nnodes = *(int*)((uintptr_t)hashTable + 0x10);

    LOG_INFO("[Icall] hash_func @ " + ToHex((uintptr_t)hashFunc));
    LOG_INFO("[Icall] equal_func @ " + ToHex((uintptr_t)equalFunc));
    LOG_INFO("[Icall] nodes @ " + ToHex((uintptr_t)nodes));
    LOG_INFO("[Icall] size (buckets): " + std::to_string(size));
    LOG_INFO("[Icall] nnodes (entries): " + std::to_string(nnodes));

    if (!nodes || !IsValidPointer(nodes)) {
        LOG_ERROR("[Icall] Invalid nodes array");
        return;
    }

    if (size <= 0 || size > 100000) {
        LOG_ERROR("[Icall] Invalid bucket count: " + std::to_string(size));
        return;
    }

    LOG_INFO("");
    LOG_INFO("=== ICALL ENTRIES ===");

    int totalFound = 0;
    int bucketsUsed = 0;

    // Walk each bucket
    for (int i = 0; i < size && totalFound < 5000; i++) {
        void* node = nodes[i];
        if (!node) continue;

        bucketsUsed++;

        // Walk the chain for this bucket
        while (node && IsValidPointer(node) && totalFound < 5000) {
            const char* key = *(const char**)((uintptr_t)node + 0x00);
            void* value = *(void**)((uintptr_t)node + 0x04);
            void* next = *(void**)((uintptr_t)node + 0x08);

            if (key && IsValidPointer((void*)key) && value) {
                // Validate key is a string
                bool valid = true;
                int len = 0;
                for (int j = 0; j < 256 && key[j]; j++) {
                    char c = key[j];
                    if (c < 0x20 || c > 0x7E) {
                        valid = false;
                        break;
                    }
                    len++;
                }

                if (valid && len > 0) {
                    std::string methodName(key, len);
                    LOG_INFO(ToHex((uintptr_t)value) + " = " + methodName);
                    totalFound++;
                }
            }

            node = next;
        }
    }

    LOG_INFO("");
    LOG_INFO("=== ICALL DUMP COMPLETE ===");
    LOG_INFO("Buckets used: " + std::to_string(bucketsUsed) + "/" + std::to_string(size));
    LOG_INFO("Total icalls found: " + std::to_string(totalFound));
}

// GET IMAGE FROM CLASS - Read image pointer from class struct
void* MonoBridge::GetClassImage(void* klass) {
    if (!klass || !IsValidPointer(klass)) return nullptr;
    return *(void**)((uintptr_t)klass + Offsets::CLASS_IMAGE);
}

// SEED CLASSES - One per assembly for image discovery fallback
struct SeedClass {
    const char* nameSpace;
    const char* className;
    const char* assemblyName; // For logging only
};

static const SeedClass kSeedClasses[] = {{"System", "String", "mscorlib"}, {"Sims3.SimIFace", "ObjectGuid", "SimIFace"}, {"ScriptCore", "Simulator", "ScriptCore"}, {"Sims3.Metadata", "CASLODAsset", "Sims3Metadata"},
    {"Sims3.UI", "WindowBase", "UI"}, {"Sims3.Store.Objects", "BabyDragon", "Sims3StoreObjects"}, {"Sims3.Gameplay.Objects", "TableBar", "Sims3GameplayObjects"}, {"Sims3.Gameplay", "BlogAppModel", "Sims3GameplaySystems"},
    {"System", "UriBuilder", "System"}, {"System.Xml.Schema", "XmlSchemaAll", "System.Xml"}, {nullptr, nullptr, nullptr}};

// SAFE PROBING HELPERS - SEH-protected, no C++ objects
static bool ProbeStringSafe_Impl(void* base, int offset, char* outBuf, int maxLen) {
    __try {
        void* ptr = *(void**)((uintptr_t)base + offset);
        if (!ptr) return false;

        // Quick valid pointer check
        if ((uintptr_t)ptr < 0x10000 || (uintptr_t)ptr > 0x7FFFFFFF) return false;

        const char* str = (const char*)ptr;

        // First char should be printable ASCII
        if (str[0] < 0x20 || str[0] > 0x7E) return false;

        strncpy(outBuf, str, maxLen - 1);
        outBuf[maxLen - 1] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// C++ wrappers
static std::string ProbeStringSafe(void* base, int offset, int maxLen = 20) {
    char buf[256];
    if (maxLen > 255) maxLen = 255;
    if (ProbeStringSafe_Impl(base, offset, buf, maxLen)) { return std::string(buf); }
    return "null";
}

// INITIALIZE ALL IMAGES - Walk loaded_assemblies GList (includes .package mods)
void MonoBridge::InitializeAllImages() {
    if (m_imagesInitialized) return;

    LOG_INFO("[MonoBridge] Initializing all assembly images via loaded_assemblies...");

    m_cachedImages.clear();
    std::set<void*> seenImages;

    // loaded_assemblies global at 0x011f422c (GList*)
    // GList: { void* data, GList* next, GList* prev }
    // MonoAssembly+0x44 = MonoImage*
    constexpr uintptr_t LOADED_ASSEMBLIES_ADDR = 0x011f422c;
    constexpr int ASSEMBLY_IMAGE_OFFSET = 0x44;

    void* listHead = *(void**)LOADED_ASSEMBLIES_ADDR;
    LOG_INFO("[MonoBridge] loaded_assemblies @ " + ToHex(LOADED_ASSEMBLIES_ADDR) + " = " + ToHex((uintptr_t)listHead));

    if (!listHead || !IsValidPointer(listHead)) {
        LOG_WARN("[MonoBridge] loaded_assemblies is empty or invalid, falling back to seed classes");
        // Fallback to seed class approach
        for (const SeedClass* seed = kSeedClasses; seed->nameSpace != nullptr; seed++) {
            void* klass = GetClassByName(seed->nameSpace, seed->className);
            if (!klass || !IsValidPointer(klass)) continue;
            void* image = GetClassImage(klass);
            if (!image || !IsValidPointer(image)) continue;
            if (seenImages.find(image) != seenImages.end()) continue;
            seenImages.insert(image);
            m_cachedImages.push_back(image);
        }
        m_imagesInitialized = true;
        LOG_INFO("[MonoBridge] Initialized " + std::to_string(m_cachedImages.size()) + " images via fallback");
        return;
    }

    // Walk the GList
    void* node = listHead;
    int count = 0;
    while (node && IsValidPointer(node) && count < 500) {
        // GList->data = MonoAssembly*
        void* assembly = *(void**)((uintptr_t)node + 0x00);

        if (assembly && IsValidPointer(assembly)) {
            // MonoAssembly+0x44 = MonoImage*
            void* image = *(void**)((uintptr_t)assembly + ASSEMBLY_IMAGE_OFFSET);

            if (image && IsValidPointer(image)) {
                if (seenImages.find(image) == seenImages.end()) {
                    seenImages.insert(image);
                    m_cachedImages.push_back(image);

                    const char* name = GetImageName(image);
                    int typeDefCount = GetTypeDefCount(image);

                    // Get captured load path (from our hook)
                    std::string loadPath = GetImageLoadPath(image);

                    // Get MVID GUID for identification
                    std::string guid = ProbeStringSafe(image, 0x28, 40);

                    LOG_INFO("[MonoBridge] Found: " + std::string(name ? name : "???") + " @ " + ToHex((uintptr_t)image) + " (" + std::to_string(typeDefCount) + " types)");
                    LOG_INFO("  GUID: " + guid);
                    LOG_INFO("  LoadPath: " + (loadPath.empty() ? "<not captured - loaded before hook>" : loadPath));
                }
            }
        }

        // GList->next
        void* next = *(void**)((uintptr_t)node + 0x04);
        if (next == node) break; // Circular reference check
        node = next;
        count++;
    }

    m_imagesInitialized = true;
    LOG_INFO("[MonoBridge] Initialized " + std::to_string(m_cachedImages.size()) + " unique images from loaded_assemblies");
}

// TYPE NAME HELPER - Convert MonoTypeCode to display string
const char* GetTypeName(MonoTypeCode type) {
    switch (type) {
    case MonoTypeCode::Void:
        return "void";
    case MonoTypeCode::Boolean:
        return "bool";
    case MonoTypeCode::Char:
        return "char";
    case MonoTypeCode::I1:
        return "sbyte";
    case MonoTypeCode::U1:
        return "byte";
    case MonoTypeCode::I2:
        return "short";
    case MonoTypeCode::U2:
        return "ushort";
    case MonoTypeCode::I4:
        return "int";
    case MonoTypeCode::U4:
        return "uint";
    case MonoTypeCode::I8:
        return "long";
    case MonoTypeCode::U8:
        return "ulong";
    case MonoTypeCode::R4:
        return "float";
    case MonoTypeCode::R8:
        return "double";
    case MonoTypeCode::String:
        return "string";
    case MonoTypeCode::ValueType:
        return "struct";
    case MonoTypeCode::Class:
        return "class";
    case MonoTypeCode::GenericInst:
        return "generic";
    case MonoTypeCode::Object:
        return "object";
    case MonoTypeCode::SzArray:
        return "array";
    default:
        return "?";
    }
}

// HEAP SCANNING - Enumerate live managed objects via GC walker

// Thread-local storage for the user callback since GC_apply_to_each_object, doesn't support a user context parameter
static thread_local MonoBridge::HeapScanCallback* g_heapScanCallback = nullptr;

// The callback that GC_apply_to_each_object will call
static void __cdecl HeapScanInternalCallback(uintptr_t objAddress, char sizeCode, int stride, int flags) {
    (void)sizeCode;
    (void)stride;
    (void)flags;
    if (!g_heapScanCallback) return;

    // Validate the object address
    if (!IsHeapPointer((void*)objAddress)) return;

    // Read VTable pointer from object (offset 0)
    void* vtable = *(void**)objAddress;
    if (!IsHeapPointer(vtable)) return;

    // Read MonoClass from VTable (offset 0)
    void* klass = *(void**)vtable;
    if (!IsHeapPointer(klass)) return;

    // Call user callback with object address and class
    (*g_heapScanCallback)(objAddress, klass);
}

void MonoBridge::ScanHeap(HeapScanCallback callback) {
    if (!m_gcApplyToEachObject) {
        LOG_ERROR("[MonoBridge] ScanHeap: GC walker not initialized");
        return;
    }

    if (!IsRuntimeReady()) {
        LOG_ERROR("[MonoBridge] ScanHeap: Runtime not ready");
        return;
    }

    LOG_INFO("[MonoBridge] Starting heap scan...");

    // GC_apply_to_each_object takes just ONE param - the callback function pointer
    g_heapScanCallback = &callback;
    m_gcApplyToEachObject((void*)HeapScanInternalCallback);
    g_heapScanCallback = nullptr;

    LOG_INFO("[MonoBridge] Heap scan complete");
}

// CUSTOM ATTRIBUTES - Read and query method attributes
void* MonoBridge::GetCustomAttrsFromMethod(void* method) {
    if (!m_customAttrsFromMethod || !method) return nullptr;
    return m_customAttrsFromMethod(method);
}

bool MonoBridge::CustomAttrsHasAttr(void* attrInfo, void* attrClass) {
    if (!m_customAttrsHasAttr || !attrInfo || !attrClass) return false;
    return m_customAttrsHasAttr(attrInfo, attrClass) != 0;
}

// SEH wrapper for mono_custom_attrs_get_attr, which internally invokes attribute constructors
// through the interpreter and can hit the same lazy-init crash as InvokeMethod (see InvokeWithSEH).
static uintptr_t s_lastAttrCrashAddr = 0;

static bool CustomAttrsGetAttrWithSEH(void*(__cdecl* func)(void*, void*), void* attrInfo, void* attrClass, void** outResult) {
    ThreadCtxSnapshot snap = ThreadCtxSnapshot::Capture();

    __try {
        *outResult = func(attrInfo, attrClass);
        return true;
    } __except (s_lastAttrCrashAddr = (uintptr_t)(GetExceptionInformation()->ExceptionRecord->ExceptionAddress), EXCEPTION_EXECUTE_HANDLER) {
        // Restore thread context frame chain - SEH unwind left dangling pointers
        snap.Restore();

        // First call crashed, same lazy-init issue as InvokeWithSEH.
        // The crash triggered initialization; retry should succeed.
        __try {
            *outResult = func(attrInfo, attrClass);
            return true;
        } __except (s_lastAttrCrashAddr = (uintptr_t)(GetExceptionInformation()->ExceptionRecord->ExceptionAddress), EXCEPTION_EXECUTE_HANDLER) {
            snap.Restore();
            *outResult = nullptr;
            return false;
        }
    }
}

void* MonoBridge::CustomAttrsGetAttr(void* attrInfo, void* attrClass) {
    if (!m_customAttrsGetAttr || !attrInfo || !attrClass) return nullptr;
    void* result = nullptr;
    if (!CustomAttrsGetAttrWithSEH(m_customAttrsGetAttr, attrInfo, attrClass, &result)) {
        LOG_WARN("[MonoBridge] CustomAttrsGetAttr crashed at " + ToHex(s_lastAttrCrashAddr) + " - retry also failed, skipping attribute");
    } else if (result) {
        if (s_lastAttrCrashAddr != 0) {
            LOG_DEBUG("[MonoBridge] CustomAttrsGetAttr: recovered via retry after crash at " + ToHex(s_lastAttrCrashAddr));
            s_lastAttrCrashAddr = 0;
        }
    }
    return result;
}

void MonoBridge::CustomAttrsFree(void* attrInfo) {
    if (!attrInfo) return;

    // MonoCustomAttrInfo: +0x00 num_attrs, +0x04 cached (if true, Mono owns it)
    int cached = *(int*)((uintptr_t)attrInfo + 0x04);
    if (!cached && m_monoFree) { m_monoFree(attrInfo); }
}

void* MonoBridge::GetFieldFromName(void* klass, const char* fieldName) {
    if (!m_classGetFieldFromName || !klass || !fieldName) return nullptr;
    return m_classGetFieldFromName(klass, fieldName);
}

// ASSEMBLY INSPECTOR - Image name accessors

const char* MonoBridge::GetImageName(void* image) {
    if (!image || !IsValidPointer(image)) return nullptr;
    const char* name = *(const char**)((uintptr_t)image + Offsets::IMAGE_ASSEMBLY_NAME);
    if (!name || !IsValidPointer((void*)name)) return nullptr;
    return name;
}

const char* MonoBridge::GetImageFilename(void* image) {
    if (!image || !IsValidPointer(image)) return nullptr;
    const char* name = *(const char**)((uintptr_t)image + Offsets::IMAGE_NAME);
    if (!name || !IsValidPointer((void*)name)) return nullptr;
    return name;
}

// IL COMPARISON / METHOD HEADER ACCESS
// MonoMethodHeader layout (RE of mono_metadata_parse_mh_full @ 0x00e9eab0): +0x00 code_size, +0x04 code (IL), +0x08 max_stack, +0x0C num_clauses, +0x10 num_locals, +0x14 clauses, +0x18+ locals[] (variable length)

void* MonoBridge::GetMethodHeader(void* method) {
    if (!method || !m_methodGetHeader) return nullptr;
    return m_methodGetHeader(method);
}

const uint8_t* MonoBridge::GetMethodIL(void* header, uint32_t* outSize) {
    if (!header) {
        if (outSize) *outSize = 0;
        return nullptr;
    }

    uint32_t codeSize = *(uint32_t*)((uintptr_t)header + 0x00);
    const uint8_t* code = *(const uint8_t**)((uintptr_t)header + 0x04);

    if (outSize) *outSize = codeSize;
    return code;
}

} // namespace S3MM
