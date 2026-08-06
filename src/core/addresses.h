#pragma once
#include <cstdint>

namespace S3MM {
namespace Addresses {

// MONO RUNTIME - Core
struct MonoCore {
    uintptr_t mono_gc_malloc = 0;
    uintptr_t GC_apply_to_each_object = 0;
    uintptr_t mono_free = 0; // game-side SynchronizedFree wrapper, not actual mono_free
    uintptr_t mono_domain_get = 0;
    uintptr_t mono_value_box = 0;
    uintptr_t g_error = 0; // 00e46b10: GLib fatal error (assertion failures)
};

// MONO RUNTIME - Class/Type System
struct MonoClasses {
    uintptr_t mono_class_from_name = 0;
    uintptr_t mono_class_get_full = 0;
    uintptr_t mono_class_get_methods = 0;
    uintptr_t mono_class_setup_fields = 0;
    uintptr_t mono_class_vtable = 0;
    uintptr_t mono_class_get_field_from_name = 0;
    uintptr_t mono_array_class_get = 0;
    uintptr_t mono_array_element_size = 0;
    uintptr_t mono_class_get_method_from_name_flags = 0; // 0x00E70330
    uintptr_t SPEC_mono_class_from_mono_type = 0;        // 0x00E70D50
};

// MONO RUNTIME - Methods
struct MonoMethods {
    uintptr_t mono_get_method = 0;
    uintptr_t mono_method_signature = 0;
    uintptr_t mono_method_get_header = 0;
    uintptr_t mono_method_full_name = 0;
    uintptr_t mono_signature_get_desc = 0;
    uintptr_t interp_mono_invoke_with_state = 0;
    uintptr_t SPEC_interp_exec_dispatch = 0;              // 00e525c0: internal MINT_CALL dispatch within interp_exec_state
    uintptr_t SPEC_interp_exec_dispatch_virtual = 0;      // 00e52630: virtual/delegate MINT_CALL dispatch
    uintptr_t mono_raise_exception = 0;                   // 00ea18f0: entry point for all managed exception throws
    uintptr_t interp_ex_handler = 0;                      // 00e53740: interpreter exception handler, installed by mono_interp_init
    uintptr_t interp_exec_state = 0;                      // 00e53eb0: main interpreter dispatch loop (~28KB)
    uintptr_t SPEC_mono_interp_create_runtime_method = 0; // 00e51f20: RuntimeMethod* create_runtime_method(MonoMethod*): get-or-create, caches at MonoMethod+0x14, allocates from domain->mp (0x48 bytes)
    uintptr_t SPEC_interp_transform_method = 0;           // 00e52b60: int transform(RuntimeMethod*): 0 = success; checks RTM+0x44 & 0x100 (transformed flag), pulls thread ctx from TLS, calls mono_interp_transform_method
    uintptr_t SPEC_interp_add_data_item = 0;              // 00e5bcd0: u16 add_data_item(td=ESI, value=EBX) -> dedup index in EAX; appends resolved callee RTM/class/field to td->data_items (consumed by MINT_CALL 0xF1)
};

// MONO RUNTIME - Fields
struct MonoFields {
    uintptr_t mono_field_get_value = 0;
    uintptr_t mono_field_set_value = 0;
    uintptr_t mono_field_static_get_value = 0;
    uintptr_t mono_field_static_set_value = 0;
};

// MONO RUNTIME - Objects/Arrays/Strings
struct MonoObjects {
    uintptr_t mono_string_new = 0;
    uintptr_t mono_array_new = 0;
};

// MONO RUNTIME - Custom Attributes
struct MonoCustomAttrs {
    uintptr_t mono_custom_attrs_from_method = 0;
    uintptr_t mono_custom_attrs_has_attr = 0;
    uintptr_t mono_custom_attrs_get_attr = 0;
};

// MONO RUNTIME - Internal Calls
struct MonoInternalCalls {
    uintptr_t mono_add_internal_call = 0;
    uintptr_t mono_lookup_internal_call = 0;
    uintptr_t g_icall_hash_table = 0; // *(GHashTable**)addr = icall registry
};

// MONO RUNTIME - Assembly Loading
struct MonoAssemblies {
    uintptr_t mono_assembly_open_full = 0;
    uintptr_t mono_assembly_get_image = 0;
    uintptr_t mono_assembly_load_from_full = 0;
    uintptr_t mono_assembly_names_equal = 0;
    uintptr_t mono_image_open_from_data_full = 0;
    uintptr_t mono_image_open_full = 0;
    uintptr_t mono_image_open_from_data = 0; // wrapper: calls mono_image_open_from_data_full with refonly=0
    uintptr_t SPEC_ves_icall_AppDomain_LoadAssemblyRaw = 0;
    uintptr_t mono_loader_get_last_error = 0;
    uintptr_t mono_loader_clear_error = 0;
};

// MONO RUNTIME - Metadata
struct MonoMetadata {
    uintptr_t mono_metadata_string_heap = 0;
    uintptr_t mono_metadata_decode_row_col = 0;
    uintptr_t mono_metadata_blob_heap = 0;
    uintptr_t mono_image_rva_map = 0;
};

// MONO RUNTIME - Profiler
struct MonoProfiler {
    uintptr_t g_thread_context_tls_index = 0;
    uintptr_t g_profiler_events = 0;
    uintptr_t g_allocation_callback = 0;
    uintptr_t g_current_profiler = 0;
};

// MONO RUNTIME - Globals
struct MonoGlobals {
    uintptr_t g_ScriptVM = 0;              // *(void**)addr = MonoScriptHost*
    uintptr_t g_mono_domain_tls_index = 0; // TLS index used by mono_domain_get
    uintptr_t g_loaded_assemblies = 0;     // *(GList**)addr = loaded assembly list
};

// MONO RUNTIME - mono_defaults (subset)
// Only primitive type classes actually used by S3MM (cache_api, hook_manager).
// Full struct: 62 fields, sizeof=0xF8, base 0x01220E40.
// Validate against mono_init_internal (0x00E75BF0) assertion strings
struct MonoDefaults {
    // Primitive type classes - used by GetTypeClassFromMonoType / GetMonoClassForTypeCode
    uintptr_t boolean_class = 0; // +0x10 System.Boolean
    uintptr_t sbyte_class = 0;   // +0x14 System.SByte
    uintptr_t byte_class = 0;    // +0x08 System.Byte
    uintptr_t int16_class = 0;   // +0x18 System.Int16
    uintptr_t uint16_class = 0;  // +0x1C System.UInt16
    uintptr_t int32_class = 0;   // +0x20 System.Int32
    uintptr_t uint32_class = 0;  // +0x24 System.UInt32
    uintptr_t int64_class = 0;   // +0x30 System.Int64
    uintptr_t uint64_class = 0;  // +0x34 System.UInt64
    uintptr_t single_class = 0;  // +0x38 System.Single
    uintptr_t double_class = 0;  // +0x3C System.Double
    uintptr_t char_class = 0;    // +0x40 System.Char
    uintptr_t string_class = 0;  // +0x44 System.String
};

// GAME - Script Host
struct GameScriptHost {
    uintptr_t ScriptHost_FindClass = 0; // Sims3::MonoEngine::MonoScriptHost::FindClass
    uintptr_t ScriptHost_LoadImage = 0; // Sims3::MonoEngine::MonoScriptHost::LoadImage
    uintptr_t ScriptHost_InitHeap = 0;  // TODO: unknown actual name, might be exe onli
    uintptr_t GetResourceManager = 0;
    uintptr_t MonoScriptHost_ProcessTasks = 0; // per-frame managed-task pump inside MonoScriptHost::Simulate
};

// GAME - Misc
struct GameMisc {
    uintptr_t RefPack_Decompress = 0;
    uintptr_t GameUtils_StartupComplete = 0; // icall: fires when game startup finishes
    uintptr_t IniScalarRead = 0;             // 004ef2b0: universal INI scalar reader (5 params: parser, section, key, format, dest)
};

// GLOBAL INSTANCES
extern MonoCore Core;
extern MonoClasses Classes;
extern MonoMethods Methods;
extern MonoFields Fields;
extern MonoObjects Objects;
extern MonoCustomAttrs CustomAttrs;
extern MonoInternalCalls InternalCalls;
extern MonoAssemblies Assemblies;
extern MonoMetadata Metadata;
extern MonoProfiler Profiler;
extern MonoGlobals Globals;
extern MonoDefaults Defaults;
extern GameScriptHost ScriptHost;
extern GameMisc Misc;

// Initialize all addresses via pattern scan (with hardcoded fallbacks)
// Returns true if all critical addresses resolved
bool Initialize();

// Validate resolved addresses against hardcoded fallbacks
// Returns number of mismatches (0 = all good, -1 = not initialized)
int Validate();

} // namespace Addresses
} // namespace S3MM
