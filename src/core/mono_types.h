#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
//wowee
namespace S3MM {

// MONO TYPE CODES - ECMA-335 element type values
enum class MonoTypeCode : uint8_t {
    Void = 0x01,
    Boolean = 0x02,
    Char = 0x03,
    I1 = 0x04, // sbyte
    U1 = 0x05, // byte
    I2 = 0x06, // short
    U2 = 0x07, // ushort
    I4 = 0x08, // int
    U4 = 0x09, // uint
    I8 = 0x0A, // long
    U8 = 0x0B, // ulong
    R4 = 0x0C, // float
    R8 = 0x0D, // double
    String = 0x0E,
    ValueType = 0x11,
    Class = 0x12,
    GenericInst = 0x15, // Generic instantiation (e.g. List<T>, Dictionary<K,V>)
    Object = 0x1C,
    SzArray = 0x1D,
};

const char* GetTypeName(MonoTypeCode type);

// MONO STRUCT OFFSETS - Sims 3 Mono 1.2.3.1 layout
namespace Offsets {
// ScriptHost
constexpr uint32_t SCRIPTHOST_DOMAIN = 0xACC;
constexpr uint32_t SCRIPTHOST_IMAGE_LIST_SENTINEL = 0xAD4;
constexpr uint32_t SCRIPTHOST_IMAGE_LIST_START = 0xAD8;
constexpr uint32_t IMAGE_NODE_IMAGE_PTR = 0x20;

// MonoImage
constexpr uint32_t IMAGE_NAME = 0x14;          // char* full filename (C:\path\foo.dll)
constexpr uint32_t IMAGE_ASSEMBLY_NAME = 0x18; // char* short name (foo)
constexpr uint32_t IMAGE_TYPEDEF_COUNT = 0x80; // uint32 (mask with 0x00FFFFFF)
constexpr uint32_t IMAGE_ASSEMBLY = 0x290;     // MonoAssembly* back-pointer

// MonoClass
constexpr uint32_t CLASS_IMAGE = 0x00; // MonoImage* at offset 0
constexpr uint32_t CLASS_NAME = 0x34;
constexpr uint32_t CLASS_NAMESPACE = 0x38;
constexpr uint32_t CLASS_NESTED_IN = 0x28;
constexpr uint32_t CLASS_PARENT = 0x24;
constexpr uint32_t CLASS_FLAGS_BYTE = 0x14;    // bitfield byte: inited|init_pending|size_inited|valuetype|enumtype|...
constexpr uint8_t CLASS_FLAG_VALUETYPE = 0x08; // interp_mono_invoke_with_state reads enumtype as (klass+0x14 & 0x10) valuetype is the bit below
constexpr uint8_t CLASS_FLAG_ENUMTYPE = 0x10;
constexpr uint32_t CLASS_FIELD_COUNT = 0x68;
constexpr uint32_t CLASS_FIELDS = 0x88;

// MonoClassField
constexpr uint32_t FIELD_SIZE = 0x1C;
constexpr uint32_t FIELD_TYPE = 0x00;   // MonoType* (used by CacheAPI for boxing/unboxing)
constexpr uint32_t FIELD_OFFSET = 0x08; // int32 offset within instance/static data
constexpr uint32_t FIELD_NAME = 0x0C;

// MonoMethod
constexpr uint32_t METHOD_FLAGS = 0x00;
constexpr uint32_t METHOD_KLASS = 0x08;
constexpr uint32_t METHOD_SIGNATURE = 0x0C;
constexpr uint32_t METHOD_GENERIC_CONTAINER = 0x10;
constexpr uint32_t METHOD_NAME = 0x18;
constexpr uint32_t METHOD_SLOT_AND_FLAGS = 0x1C;

// is_inflated bit in the packed dword at METHOD_SLOT_AND_FLAGS
// mono_method_signature branches on it, and only on that branch does it read METHOD_INFLATED_DECLARING as the declaring generic method and recurse into itself
constexpr uint32_t METHOD_IS_INFLATED_FLAG = 0x400;

// MonoMethodInflated extension - lives past the 0x28 bytes of MonoMethodLayout
// The inflation path allocates 0x30 bytes: base MonoMethod (0x28) + context (+0x28) + declaring (+0x2C)
// mono_method_get_context is literally `is_inflated ? *(m+0x28) : 0`
constexpr uint32_t METHOD_INFLATED_CONTEXT = 0x28;
constexpr uint32_t METHOD_INFLATED_DECLARING = 0x2C;

// MonoMethodSignature
constexpr uint32_t SIG_PARAM_COUNT = 0x04;
constexpr uint32_t SIG_RET_TYPE = 0x0C;
constexpr uint32_t SIG_PARAMS = 0x10;

// MonoString (old Mono 1.2.3.1 layout)
constexpr uint32_t STRING_LENGTH = 0x04;
constexpr uint32_t STRING_CHARS = 0x08;

// MonoArray (old Mono 1.2.3.1 layout) - data starts at +0x10 (confirmed via raw dump)
constexpr uint32_t ARRAY_BOUNDS = 0x04; // MonoArrayBounds* (null for 1D arrays)
constexpr uint32_t ARRAY_LENGTH = 0x08; // int32 element count (max_length)
constexpr uint32_t ARRAY_DATA = 0x10;   // Start of array data (offset 16, after 4-byte padding)

// MonoClass generic instantiation marker - MonoGenericClass*, null for ordinary classes
// mono_method_signature's non-inflated path asserts this is null, so a method whose declaring class is a generic instantiation has to take the inflated path
constexpr uint32_t CLASS_GENERIC_CLASS = 0xA8;

// MonoClass size/array-related
constexpr uint32_t CLASS_INSTANCE_SIZE = 0x54;      // uint32 instance size in bytes
constexpr uint32_t CLASS_ELEMENT_CLASS = 0x30;      // MonoClass* of element type (for arrays)
constexpr uint32_t CLASS_ELEMENT_TYPE = 0xA6;       // MonoTypeEnum of element type (byte at 0xA6)
constexpr uint32_t CLASS_ARRAY_ELEMENT_SIZE = 0x5C; // Cached element size for arrays

// MonoType
// data union - MonoClass* for CLASS and VALUETYPE, the element MonoClass* for SZARRAY, MonoGenericClass* for GENERICINST
constexpr uint32_t TYPE_DATA = 0x00;
constexpr uint32_t TYPE_ATTRS = 0x04;
constexpr uint32_t TYPE_CODE = 0x06;

// RuntimeMethod
constexpr uint32_t RUNTIME_METHOD_CODE = 0x00;
constexpr uint32_t RUNTIME_METHOD_MONO_METHOD = 0x08;
constexpr uint32_t RUNTIME_METHOD_NATIVE_FUNC = 0x10;
constexpr uint32_t RUNTIME_METHOD_ARG_OFFSETS = 0x1C;
constexpr uint32_t RUNTIME_METHOD_EXEC_FUNC = 0x24;
constexpr uint32_t RUNTIME_METHOD_PARAM_COUNT = 0x40;
constexpr uint32_t RUNTIME_METHOD_FLAGS = 0x44;

// InterpFrame
constexpr uint32_t INTERP_FRAME_PREV = 0x00;
constexpr uint32_t INTERP_FRAME_RUNTIME_METHOD = 0x04;

// ThreadContext
constexpr uint32_t THREAD_CTX_DOMAIN = 0x00;
constexpr uint32_t THREAD_CTX_BASE_FRAME = 0x04;
constexpr uint32_t THREAD_CTX_CURRENT_FRAME = 0x08;
constexpr uint32_t THREAD_CTX_ENV_FRAME = 0x0C; // setjmp boundary
constexpr uint32_t THREAD_CTX_MANAGED_CODE = 0x15;
} // namespace Offsets

// RUNTIME METHOD (0x48 bytes) - Interpreter's compiled form of a MonoMethod
// Allocated by SPEC_mono_interp_get_runtime_methodALT (0x00E51F20), zeroed with memset.
// Populated by SPEC_mono_interp_transform_method (0x00E643B0) which converts IL -> MINT bytecode.
// Cached at MonoMethod+0x14 (MonoMethodLayout::runtimeMethod).
//
// XeX version is _RuntimeMethod (0x5C / 92 bytes) with 5 extra fields not present in x86:
//   XeX 0x00: prev, 0x04: next, 0x08: image, 0x0C: container (linked list + image tracking)
//   XeX 0x18: new_body_start (hot-reload support?)
// Fields after 0x0C shift by -0x10, fields after 0x18 shift by -0x14 relative to XeX offsets.
//
// Lifecycle: get_runtime_methodALT sets [0x08, 0x40, 0x44]. transform_method sets the rest.
// SPEC_MonoGenerateCodeLookup (0x00E5CA90) generates the MINT bytecode at offset 0x00.
// For ICALLs, FUN_00e51400 generates a native thunk at 0x10 and sets code to a dispatch stub.

struct RuntimeMethod {
    uint16_t* code;          // 0x00: MINT bytecode (or ICALL dispatch stub DAT_0119592c). Matches XeX.
    uint32_t code_size;      // 0x04: XeX splits this as refcount(1B) + refcount(1B) + code_size(2B). x86 may differ.
    void* method;            // 0x08: Back-pointer to originating MonoMethod*. Matches XeX.
    void* template_method;   // 0x0C: Resolved MonoMethod* for delegation/generic instantiation. Matches XeX.
    void* func;              // 0x10: ICALL native thunk (set during transform). Matches XeX.
    void** data_items;       // 0x14: Array of resolved tokens (classes, methods, fields) for MINT opcodes. Matches XeX.
    void* clauses;           // 0x18: MonoExceptionClause* array for try/catch/finally blocks. Matches XeX.
    uint32_t* arg_offsets;   // 0x1C: Per-argument stack offsets (uint32_t[], mempool-allocated). Matches XeX.
    uint32_t* local_offsets; // 0x20: Per-local-variable stack offsets (uint32_t[], mempool-allocated). Matches XeX.
    void* nativeThunk;       // 0x24: Exec entry point - called by SPEC_interp_exec_dispatch for ALL methods. XeX name: nativeThunk (misleading).
    void* nativeMethod;      // 0x28: Raw native method pointer (before thunk wrapping). Matches XeX.
    void* nativeSetup;       // 0x2C: Setup callback for native methods (called during transform, then cleared). Matches XeX.
    uint8_t* evstack_traces; // 0x30: Eval stack type trace array for debugging/verification. Matches XeX.
    uint16_t stack_size;     // 0x34: Operand stack size in bytes. Matches XeX.
    uint16_t vt_stack_size;  // 0x36: Value type stack overflow area size. Matches XeX.
    uint16_t alloca_size;    // 0x38: Total stack allocation size (compared between caller/callee for realloc). Matches XeX.
    uint16_t num_ops;        // 0x3A: Number of MINT opcodes in compiled bytecode. Matches XeX.
    uint16_t locals_size;    // 0x3C: Total local variable area size (aligned). Matches XeX.
    uint16_t args_size;      // 0x3E: Total argument area size. Matches XeX.
    uint8_t param_count;     // 0x40: Parameter count from MonoMethodSignature. Matches XeX.
    uint8_t num_clauses;     // 0x41: Number of exception clauses. Matches XeX.
    uint8_t pad_42;          // 0x42: Padding
    uint8_t pad_43;          // 0x43: Padding
    uint32_t flags;          // 0x44: Packed bitfield. XeX has two unnamed uint16 fields here.
    //   x86 observed: low byte = rettype, 0x0100 = transformed, 0x0200 = hasthis, 0x0400 = constrained/valuetype
};

static_assert(sizeof(RuntimeMethod) == 0x48, "RuntimeMethod size mismatch");
static_assert(offsetof(RuntimeMethod, method) == 0x08, "RuntimeMethod::method offset mismatch");
static_assert(offsetof(RuntimeMethod, func) == 0x10, "RuntimeMethod::func offset mismatch");
static_assert(offsetof(RuntimeMethod, arg_offsets) == 0x1C, "RuntimeMethod::arg_offsets offset mismatch");
static_assert(offsetof(RuntimeMethod, nativeThunk) == 0x24, "RuntimeMethod::nativeThunk offset mismatch");
static_assert(offsetof(RuntimeMethod, param_count) == 0x40, "RuntimeMethod::param_count offset mismatch");
static_assert(offsetof(RuntimeMethod, flags) == 0x44, "RuntimeMethod::flags offset mismatch");

namespace RuntimeMethodFlags {
constexpr uint32_t TRANSFORMED = 0x0100;
constexpr uint32_t HAS_THIS = 0x0200;
constexpr uint32_t CONSTRAINED = 0x0400;      // XeX name: valuetype
constexpr uint32_t RETURN_TYPE_MASK = 0x00FF; // Low byte = MonoTypeCode of return type
} // namespace RuntimeMethodFlags

// INTERP FRAME (0x28 bytes) - Interpreter call frame, stack-allocated linked list
// Created by SPEC_interp_exec_state for each MINT_CALL opcode.
// Matches XeX _MonoInvocation exactly (same size, same offsets, same field names).

struct InterpFrame {
    InterpFrame* parent;           // 0x00: Previous frame (linked list)
    RuntimeMethod* runtime_method; // 0x04: The RuntimeMethod being executed
    void* retval;                  // 0x08: Return value destination (stackval*)
    void* obj;                     // 0x0C: 'this' pointer for instance methods (void*)
    void* args;                    // 0x10: Method arguments buffer (char*)
    void* stack_args;              // 0x14: Eval stack args area (stackval*)
    void* stack;                   // 0x18: Eval stack pointer (stackval*)
    const uint16_t* ip;            // 0x1C: Current MINT instruction pointer
    void* ex;                      // 0x20: MonoException* thrown in this frame
    void* ex_handler;              // 0x24: Active MonoExceptionClause* handler
};

static_assert(sizeof(InterpFrame) == 0x28, "InterpFrame size mismatch");
static_assert(offsetof(InterpFrame, runtime_method) == 0x04, "InterpFrame::runtime_method offset mismatch");
static_assert(offsetof(InterpFrame, ex) == 0x20, "InterpFrame::ex offset mismatch");
static_assert(offsetof(InterpFrame, ip) == 0x1C, "InterpFrame::ip offset mismatch");

// THREAD CONTEXT (0x18 bytes) - Per-thread interpreter state, accessed via TLS
// Allocated by SPEC_interp_mono_invoke_with_state on first use (GC_malloc or stack-alloc).
// TLS index stored at Addresses::Profiler::g_thread_context_tls_index.
// Matches XeX ThreadContext exactly (same size, same offsets, same field names).

struct ThreadContext {
    void* domain;               // 0x00: MonoDomain*
    InterpFrame* base_frame;    // 0x04: InterpFrame* (stale, set once at creation)
    InterpFrame* current_frame; // 0x08: InterpFrame* (dynamic, updated on every MINT_CALL)
    InterpFrame* env_frame;     // 0x0C: InterpFrame* (setjmp boundary frame for exception handling)
    void* current_env;          // 0x10: Pointer to setjmp buffer (jmp_buf*)
    uint8_t search_for_handler; // 0x14: Set during exception unwinding
    uint8_t managed_code;       // 0x15: 1 during managed opcode execution, 0 during native callouts
    uint8_t pad_16;             // 0x16: Padding
    uint8_t pad_17;             // 0x17: Padding
};

static_assert(sizeof(ThreadContext) == 0x18, "ThreadContext size mismatch");
static_assert(offsetof(ThreadContext, managed_code) == 0x15, "ThreadContext::managed_code offset mismatch");

// MONO METHOD STRUCTURE - Sims 3 Mono 1.2.3.1 layout (0x28 bytes, no header field)
// IL bytecode is looked up via token from MonoImage metadata tables.
// Patching: swap runtimeMethod pointer, or hook SPEC_MonoGenerateCodeLookup.

struct MonoMethodLayout {
    uint32_t flagsPacked;   // Offset 0x00 - flags + iflags packed
    uint32_t token;         // Offset 0x04 - Metadata token (0x06xxxxxx for MethodDef)
    void* klass;            // Offset 0x08 - MonoClass*
    void* signature;        // Offset 0x0C - MonoMethodSignature*
    void* genericContainer; // Offset 0x10
    void* runtimeMethod;    // Offset 0x14 - RuntimeMethod* (compiled form)
    const char* name;       // Offset 0x18 - Method name
    uint32_t slotAndFlags;  // Offset 0x1C
    void* nativeFunc;       // Offset 0x20 - UNION: ICALL fn ptr for icalls (0xF2 handler reads it) / cached MonoMethodHeader* for IL methods (mono_method_get_header caches here; the top gate keeps the two disjoint)
    uint32_t unknown2;      // Offset 0x24
    // Total size: 0x28 bytes - NO dedicated header field (header rides the +0x20 union above)
};

static_assert(sizeof(MonoMethodLayout) == 0x28, "MonoMethodLayout size mismatch");
static_assert(offsetof(MonoMethodLayout, token) == 0x04, "token offset mismatch");
static_assert(offsetof(MonoMethodLayout, klass) == 0x08, "klass offset mismatch");
static_assert(offsetof(MonoMethodLayout, runtimeMethod) == 0x14, "runtimeMethod offset mismatch");
static_assert(offsetof(MonoMethodLayout, name) == 0x18, "name offset mismatch");

// INFLATED METHOD (0x30 bytes) - a method on a constructed generic type (or a constructed generic method)
// Mirrors the runtime's inflation allocation (see Offsets::METHOD_INFLATED_CONTEXT notes)
// Any copy of a method whose METHOD_IS_INFLATED_FLAG is set MUST be this full struct - a 0x28-byte copy loses context/declaring and aborts on the forced re-transform
struct MonoMethodInflatedLayout {
    MonoMethodLayout method; // 0x00..0x27
    void* context;           // 0x28 - MonoGenericContext* (never null on a runtime-built inflated method)
    void* declaring;         // 0x2C - the generic method definition
};

static_assert(sizeof(MonoMethodInflatedLayout) == 0x30, "MonoMethodInflatedLayout size mismatch");
static_assert(offsetof(MonoMethodInflatedLayout, context) == Offsets::METHOD_INFLATED_CONTEXT, "context offset mismatch");
static_assert(offsetof(MonoMethodInflatedLayout, declaring) == Offsets::METHOD_INFLATED_DECLARING, "declaring offset mismatch");

// METHOD FLAGS - CLI method attribute bits
namespace MethodFlags {
constexpr uint32_t STATIC = 0x0010;
constexpr uint32_t VIRTUAL = 0x0040;
constexpr uint32_t ABSTRACT = 0x0400;
constexpr uint32_t SPECIAL_NAME = 0x0800;
} // namespace MethodFlags

// FIELD FLAGS - CLI field attribute bits
namespace FieldFlags {
constexpr uint16_t STATIC = 0x10;
constexpr uint16_t LITERAL = 0x40; // const
} // namespace FieldFlags

// TOKEN TYPES - Metadata token table identifiers
constexpr uint32_t TOKEN_TYPE_DEF = 0x02000000;

// INVOKE STATE - Layout for interp_mono_invoke_with_state
#pragma pack(push, 1)
struct MonoInvokeState {
    void* pMethod;
    void* pThis;
    void** ppParams;
    void* pThrowExc;
    void** ppExc;
    void* pLocalContext;
    int32_t isObject;
    void* pRet;
    void* pRetVal;
    void* pSig;
    void* pClass;
    void* pArgs;
    void* pFrame;
    void* pOldFrame;
    void* pResult;
    uint8_t padding[0x104];
    int32_t cur_exec_num;
    int32_t max_depth;
    uint8_t allowYield;
    uint8_t yieldReturn;
    uint8_t setupFrame;
    uint8_t privateContext;
    void* pRTMYield;
};
#pragma pack(pop)

// HIGH-LEVEL TYPES - Structured representations for UI and API use
struct EnumValue {
    std::string name;
    int32_t value;
};

struct FieldInfo {
    void* handle; // MonoClassField*
    std::string name;
    std::string nameLower; // Pre-computed lowercase name for filtering
    MonoTypeCode type;
    bool isStatic;
    bool isLiteral; // const/readonly
    bool isEnum;
    std::vector<EnumValue> enumValues;
    std::vector<std::string> enumLabels; // Pre-computed "Name (value)" strings for combo boxes

    // Array info (only valid when type == SzArray)
    MonoTypeCode arrayElementType = MonoTypeCode::Void;
    void* arrayElementClass = nullptr; // MonoClass* of the array (e.g., int[]) - for GetArrayElementSize

    // Struct info (only valid when type == ValueType and !isEnum)
    void* valueTypeClass = nullptr; // MonoClass* of the struct type

    // Inheritance info
    bool isInherited = false;       // True if this field comes from a parent class
    void* declaringClass = nullptr; // MonoClass* where this field is declared
    std::string declaringClassName; // Name of declaring class (only set if isInherited)
};

struct MethodInfo {
    void* handle;    // MonoMethod*
    void* signature; // MonoMethodSignature*
    std::string name;
    MonoTypeCode returnType;
    std::vector<MonoTypeCode> paramTypes;
    uint32_t flags;
    bool isStatic;

    // Inheritance info
    bool isInherited = false;       // True if this method comes from a parent class
    void* declaringClass = nullptr; // MonoClass* where this method is declared
    std::string declaringClassName; // Name of declaring class (only set if isInherited)
};

struct PropertyInfo {
    std::string name;
    std::string nameLower; // Pre-computed lowercase for filtering
    MonoTypeCode type;
    bool canRead;
    bool canWrite;
    bool isStatic;
    void* getterMethod;    // MonoMethod* for get_PropertyName
    void* setterMethod;    // MonoMethod* for set_PropertyName
    void* getterSignature; // Cached signature for getter
    void* setterSignature; // Cached signature for setter

    // For enum properties
    bool isEnum;
    std::vector<EnumValue> enumValues;

    // For value type properties (non-enum structs)
    void* valueTypeClass; // MonoClass* of the struct type
};

} // namespace S3MM
