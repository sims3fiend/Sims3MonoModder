#pragma once
#include "mono_types.h"
#include <Windows.h>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

// Split-out modules: assembly_hooks.h, modding/conflict_detector.h, vanilla_extractor.h, modding/assembly_diff.h

namespace S3MM {

// MONO BRIDGE - Low-level access to Mono runtime functions
class MonoBridge {
  public:
    static MonoBridge& Get();

    // Initialization
    bool Initialize();
    bool IsInitialized() const { return m_initialized; }
    bool IsRuntimeReady() const; // ScriptVM available?

    // Domain access
    void* GetScriptHost();
    void* GetDomain();
    bool SetupTLS(); // Required before some Mono calls

    // Memory allocation (GC-tracked)
    void* GCAlloc(uint32_t size);

    // String operations
    void* CreateString(const char* utf8);
    // maxLength guards against a garbage length field on a bad pointer; raw data channel (S3MM.Log.Raw) passes a higher bound if you need it
    std::string ExtractString(void* monoString, int32_t maxLength = 4096);

    // Class lookup
    void* GetClassByName(const char* nameSpace, const char* className);
    void* GetClassFromToken(void* image, uint32_t token);
    void* GetClassFromNameInImage(void* image, const char* nameSpace, const char* className);

    // Nested class lookup, iterates TypeDefs and checks DeclaringClass
    void* GetNestedClass(void* parentKlass, const char* nestedClassName);

    // Class metadata
    static const char* GetClassName(void* klass);
    static const char* GetClassNamespace(void* klass);
    static void* GetDeclaringClass(void* klass);
    static void* GetParentClass(void* klass);
    static bool IsEnumClass(void* klass);

    // Method metadata - resolve full name from MonoMethod* directly.
    // Returns "Namespace.Class::MethodName" or "<unknown>" on failure
    static std::string GetMethodFullName(void* method);

    // Object metadata, get class from an object instance
    static void* GetObjectClass(void* object);

    // Determine MonoTypeCode from an element class (for array/list elements)
    static MonoTypeCode GetTypeCodeFromClass(void* klass);

    // Get element type from an array class (primitives and complex types)
    static MonoTypeCode GetArrayElementType(void* arrayClass);

    // Get the element class from an array class (for struct/class arrays).
    // Returns MonoClass* of the element type, or nullptr for primitives
    static void* GetArrayElementClass(void* arrayClass);

    // Method enumeration
    void* GetNextMethod(void* klass, int* iter);
    void* GetMethodSignature(void* method);

    // Get canonical signature description (e.g. "int,string,List`1").
    // Returns empty string on failure. Caller does not need to free.
    std::string GetSignatureDescription(void* signature, bool includeNamespace = false);

    // Field operations
    void SetupFields(void* klass);
    void* GetClassVTable(void* klass);
    void GetStaticFieldValue(void* vtable, void* field, void* outValue);
    void SetStaticFieldValue(void* vtable, void* field, void* value);
    void GetInstanceFieldValue(void* object, void* field, void* outValue);
    void SetInstanceFieldValue(void* object, void* field, void* value);

    // Array operations
    void* CreateArray(void* elementClass, int32_t length);
    int32_t GetArrayLength(void* monoArray);
    void* GetArrayElementPtr(void* monoArray, int32_t index, int32_t elementSize);
    int32_t GetArrayElementSize(void* arrayClass);

    // Method invocation
    bool InvokeMethod(void* method, void* thisPtr, void** params, MonoInvokeState* state);

    // Invoke a managed method on the sim thread; the interpreter is not reentrant across threads.
    // Use for any invoke originating off the sim thread; runs inline if already on it.
    // Returns false on timeout/failure.
    bool InvokeMethodOnSimThread(void* method, void* thisPtr, void** params, MonoInvokeState* state, DWORD timeoutMs = 5000);

    // Run a task on the sim thread, blocking until it completes. Use for any UI-thread operation that executes managed code (hot-reload, mod scanning);
    // Runs inline if already on the sim thread.
    // Returns false if the sim thread isn't pumping within timeoutMs.
    bool RunOnSimThread(std::function<void()> task, DWORD timeoutMs = 15000);

    // Drain queued sim-thread tasks. Call only on the sim thread (the ProcessTasks detour calls it
    // once per frame); records the sim thread id on first call.
    void DrainSimTasks();

    // Prepare a method's MINT bytecode (CIL -> MINT) without executing it. Idempotent, SEH-wrapped.
    // Side effect being it triggers the declaring class's .cctor on first call per type.
    bool TransformMethod(void* method);

    // Value boxing, wraps a value type (int, bool, etc.) in a managed object
    // Unguarded - mono_value_box reads instance_size-4 bytes from valuePtr regardless of how much is actually there
    // Prefer BoxValueChecked when the source is a fixed-size slot
    void* BoxValue(void* klass, void* valuePtr);

    // Bytes mono_value_box will read from the source for this klass (instance_size - 4), or 0 if the size cannot be established
    // 0 means unknown, not zero-sized
    static uint32_t GetBoxPayloadSize(void* klass);

    // Box a value whose payload lives in availableBytes of readable storage
    // Returns null rather than over-reading when the class is provably larger, and logs the refusal tagged with `site`
    // An unknown payload size falls through to the unguarded box
    void* BoxValueChecked(void* klass, void* valuePtr, uint32_t availableBytes, const char* site);

    // Offset of the payload inside a boxed value type
    // The MonoObject header is 4 bytes, so mono_value_box writes to boxed+4 and CacheAPI unboxes at +4
    static constexpr uint32_t BOXED_PAYLOAD_OFFSET = 4;

    // One interpreter stackval slot
    // Values returned through MonoInvokeState::pRet live in a single slot, and anything wider stays on the vt side-stack, which is not mapped
    static constexpr uint32_t STACKVAL_SLOT_BYTES = 8;

    // Get the raw invoke function pointer (for hot-reload pre-compilation)
    using InvokeFunc = void(__cdecl*)(void* state);
    InvokeFunc GetInvokeFunction() const { return (InvokeFunc)m_invokeWithState; }

    // Image enumeration
    int GetTypeDefCount(void* image);

    // Internal calls, expose native functions to managed code.
    // Name format: "Namespace.Class::Method" e.g. "S3MM.Native::Dispatch"
    void AddInternalCall(const char* name, void* method);

    // Diagnostics, dump icall/pinvoke registration table
    void DumpIcallTable();

    // Get assembly name from image (short name like "MyMod")
    static const char* GetImageName(void* image);

    // Get assembly filename from image (full path)
    static const char* GetImageFilename(void* image);

    // Get image pointer from a class
    static void* GetClassImage(void* klass);

    // Initialize all known assembly images via seed class lookup.
    // Call once after runtime is ready
    void InitializeAllImages();

    // Check if images have been initialized
    bool AreImagesInitialized() const { return m_imagesInitialized; }

    // Get all cached images (call InitializeAllImages first)
    const std::vector<void*>& GetAllImages() const { return m_cachedImages; }

    // Fresh walk of Mono's loaded_assemblies GList, including game-loaded .package mods
    // GetAllImages() is a one-time startup snapshot, this re-enumerates on every call so it picks up assemblies loaded after startup, e.g. world-load script mods like NRaas
    // Returns image pointers and leaves the cache untouched
    std::vector<void*> EnumerateLoadedImagesFresh() const;

    // Moved: assembly_hooks.h, modding/conflict_detector.h, modding/vanilla_extractor.h

    // Heap scanning, enumerates all live managed objects.
    // Callback receives (objectAddress, klass). Keep callbacks fast.
    using HeapScanCallback = std::function<void(uintptr_t objectAddress, void* klass)>;
    void ScanHeap(HeapScanCallback callback);

    // CUSTOM ATTRIBUTES - Query and extract custom attrs from methods

    // Get custom attributes from a method, returns MonoCustomAttrInfo* or nullptr
    void* GetCustomAttrsFromMethod(void* method);

    // Check if custom attr info contains a specific attribute type
    bool CustomAttrsHasAttr(void* attrInfo, void* attrClass);

    // Get attribute instance, returns MonoObject* or nullptr
    void* CustomAttrsGetAttr(void* attrInfo, void* attrClass);

    // Free custom attribute info (if not cached)
    void CustomAttrsFree(void* attrInfo);

    // Get a field from a class by name
    void* GetFieldFromName(void* klass, const char* fieldName);

    // IL / METHOD HEADER ACCESS - Raw IL bytecode retrieval

    // Get method header (IL bytecode pointer and size).
    // Returns nullptr for abstract/extern/pinvoke methods
    void* GetMethodHeader(void* method);

    // Get IL bytecode from a method header. Sets outSize to bytecode length
    const uint8_t* GetMethodIL(void* header, uint32_t* outSize);

    // IL comparison and assembly diffing moved to modding/assembly_diff.h

  private:
    MonoBridge() = default;
    ~MonoBridge() = default;
    MonoBridge(const MonoBridge&) = delete;
    MonoBridge& operator=(const MonoBridge&) = delete;

    bool m_initialized = false;
    bool m_imagesInitialized = false;
    std::vector<void*> m_cachedImages; // All discovered MonoImage pointers

    // Cross thread task queue
    struct SimTask {
        std::function<void()> fn;
        HANDLE done = nullptr;
    };
    std::atomic<DWORD> m_simThreadId{0}; // 0 until first DrainSimTasks() on the sim thread
    std::mutex m_taskMutex;
    std::deque<SimTask*> m_pendingTasks;

    // Function pointer types
    using GC_malloc_t = void*(__cdecl*)(uint32_t size);
    using interp_mono_invoke_with_state_t = void(__cdecl*)(void* state);
    using mono_class_get_methods_t = void*(__cdecl*)(void* klass, int* iter);
    using mono_method_signature_t = void*(__cdecl*)(void* method);
    using mono_class_get_full_t = void*(__cdecl*)(void* image, uint32_t token, void* context);
    using mono_class_setup_fields_t = void(__cdecl*)(void* klass);
    using game_get_class_t = void*(__thiscall*)(void*, const char*, const char*);
    using mono_class_from_name_t = void*(__cdecl*)(void* image, const char* ns, const char* name);
    using mono_class_vtable_t = void*(__cdecl*)(void* domain, void* klass);
    using mono_field_static_get_value_t = void(__cdecl*)(void* vtable, void* field, void* value);
    using mono_field_static_set_value_t = void(__cdecl*)(void* vtable, void* field, void* value);
    using mono_field_get_value_t = void(__cdecl*)(void* object, void* field, void* value);
    using mono_field_set_value_t = void(__cdecl*)(void* object, void* field, void* value);
    using mono_domain_get_t = void*(__stdcall*)();
    using mono_string_new_t = void*(__cdecl*)(void* domain, const char* text);
    using mono_array_new_t = void*(__cdecl*)(void* domain, void* elementClass, int32_t length);
    using mono_array_class_get_t = void*(__cdecl*)(void* elementClass, uint32_t rank);
    using mono_array_element_size_t = int32_t(__cdecl*)(void* arrayClass);
    using mono_add_internal_call_t = void(__cdecl*)(const char* name, void* method);

    // GC heap walker, callback receives (objectAddress, sizeCode, stride, flags)
    using GC_apply_to_each_object_t = void(__cdecl*)(void* userCallback);
    // Value boxing, wraps a value type in a managed object
    using mono_value_box_t = void*(__cdecl*)(void* domain, void* klass, void* valuePtr);

    // Custom attribute functions
    using mono_custom_attrs_from_method_t = void*(__cdecl*)(void* method);
    using mono_custom_attrs_has_attr_t = int(__cdecl*)(void* attrInfo, void* attrClass);
    using mono_custom_attrs_get_attr_t = void*(__cdecl*)(void* attrInfo, void* attrClass);
    using mono_class_get_field_from_name_t = void*(__cdecl*)(void* klass, const char* name);
    using mono_free_t = void(__cdecl*)(void* ptr);

    // Signature description, returns malloc'd string (free with mono_free).
    // include_namespace: 0 = short names, 1 = full names
    using mono_signature_get_desc_t = char*(__cdecl*)(void* sig, int include_namespace);

    // MINT bytecode preparation. create_runtime_method returns RuntimeMethod*, transform returns 0 on success.
    using mono_interp_create_runtime_method_t = void*(__cdecl*)(void* method);
    // 2-arg __cdecl - mono_interp_transform_method(rtm, threadCtx)
    // param_2 is the thread-context block transform writes its frame and jmpBuf slots through (ctx+0xc, ctx+0x10), it is not read from TLS
    // Omitting it makes transform write through an uninitialized [esp+8] stack slot, an RTC #2 stack smash
    // See TransformWithSEH and engine ref FUN_00e52b90
    using interp_transform_method_t = int(__cdecl*)(void* runtimeMethod, void* threadCtx);

    // Method header access, returns MonoMethodHeader* containing IL bytecode
    using mono_method_get_header_t = void*(__cdecl*)(void* method);

    // Method full name, returns malloc'd string (free with mono_free).
    // signature: 0 = name only, 1 = include signature
    using mono_method_full_name_t = char*(__cdecl*)(void* method, int signature);

    // Function pointers (initialized in Initialize())
    GC_malloc_t m_gcMalloc = nullptr;
    interp_mono_invoke_with_state_t m_invokeWithState = nullptr;
    mono_class_get_methods_t m_classGetMethods = nullptr;
    mono_method_signature_t m_methodSignature = nullptr;
    mono_class_get_full_t m_classGetFull = nullptr;
    mono_class_setup_fields_t m_classSetupFields = nullptr;
    game_get_class_t m_gameGetClass = nullptr;
    mono_class_from_name_t m_classFromName = nullptr;
    mono_class_vtable_t m_classVtable = nullptr;
    mono_field_static_get_value_t m_fieldStaticGetValue = nullptr;
    mono_field_static_set_value_t m_fieldStaticSetValue = nullptr;
    mono_field_get_value_t m_fieldGetValue = nullptr;
    mono_field_set_value_t m_fieldSetValue = nullptr;
    mono_domain_get_t m_domainGet = nullptr;
    mono_string_new_t m_stringNew = nullptr;
    mono_array_new_t m_arrayNew = nullptr;
    mono_array_class_get_t m_arrayClassGet = nullptr;
    mono_array_element_size_t m_arrayElementSize = nullptr;
    mono_add_internal_call_t m_addInternalCall = nullptr;
    GC_apply_to_each_object_t m_gcApplyToEachObject = nullptr;
    mono_value_box_t m_valueBox = nullptr;

    // Custom attribute function pointers
    mono_custom_attrs_from_method_t m_customAttrsFromMethod = nullptr;
    mono_custom_attrs_has_attr_t m_customAttrsHasAttr = nullptr;
    mono_custom_attrs_get_attr_t m_customAttrsGetAttr = nullptr;
    mono_class_get_field_from_name_t m_classGetFieldFromName = nullptr;
    mono_free_t m_monoFree = nullptr;

    // Signature description
    mono_signature_get_desc_t m_signatureGetDesc = nullptr;

    // Method header / IL access
    mono_method_get_header_t m_methodGetHeader = nullptr;
    mono_method_full_name_t m_methodFullName = nullptr;

    // MINT bytecode preparation
    mono_interp_create_runtime_method_t m_createRuntimeMethod = nullptr;
    interp_transform_method_t m_transformMethod = nullptr;
};

// POINTER VALIDATION - Range and readability checks for Mono pointers
inline bool IsValidPointer(void* ptr) {
    if (!ptr) return false;
    uintptr_t addr = (uintptr_t)ptr;
    // Sims 3 is LAA-enabled, so vtables can be above 0x80000000
    return addr > 0x10000 && addr < 0xFFFF0000;
}

// Stricter validation, checks memory is actually readable via Windows API
inline bool IsReadablePointer(void* ptr, size_t size = 4) {
    if (!IsValidPointer(ptr)) return false;
    // IsBadReadPtr returns TRUE if memory is NOT readable
    return !IsBadReadPtr(ptr, size);
}

// Even stricter, for heap pointers above typical module range
inline bool IsHeapPointer(void* ptr) {
    if (!ptr) return false;
    uintptr_t addr = (uintptr_t)ptr;
    // LAA-enabled, heap can reach ~0xFFFF0000. Skip low stack/special regions.
    return addr >= 0x01000000 && addr < 0xFFFF0000 && IsReadablePointer(ptr);
}

} // namespace S3MM
