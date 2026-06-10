#include "addresses.h"
#include "../util/memory.h"
#include "../util/logger.h"
#include <unordered_map>
#include <cctype>
// Using these to de-ugly it but it's still hard to parse, oh well! Just don't look at it and it's fine!
#define COUNTOF(arr) (sizeof(arr) / sizeof(arr[0]))
#define ADDR(field, pat, off, fb, crit) {&field, #field, pat, off, fb, crit}
#define SITE(field, name, off) {&field, name, off}
#define SITE_ABS(field, name, off) {&field, name, off, true}
////https://www.youtube.com/watch?v=XJgh_D379jU
namespace S3MM {
namespace Addresses {

// STRUCT INSTANCES
MonoCore Core;
MonoClasses Classes;
MonoMethods Methods;
MonoFields Fields;
MonoObjects Objects;
MonoCustomAttrs CustomAttrs;
MonoInternalCalls InternalCalls;
MonoAssemblies Assemblies;
MonoMetadata Metadata;
MonoProfiler Profiler;
MonoGlobals Globals;
MonoDefaults Defaults;
GameScriptHost ScriptHost;
GameMisc Misc;

// ADDRESS RESOLUTION TABLE

struct AddressEntry {
    uintptr_t* target;
    const char* name;
    const char* pattern; // nullptr = no pattern, use fallback only
    int offset;          // offset from pattern match
    uintptr_t fallback;  // hardcoded address
    bool critical;       // if true, Initialize() fails if not resolved
};

// CALL SITE EXTRACTOR
// Find a function via pattern, then extract call targets from known offsets, we do this because some aren't able to be found w/ pattern searching and I hate myself
struct CallSite {
    uintptr_t* target;
    const char* name;
    int offsetToCall; // offset from anchor to the E8 instruction (or to address bytes if isAbsolute)
    bool isAbsolute;  // false = E8 relative call, true = read dword at offset as absolute address
};

struct CallSiteExtractor {
    const char* name;    // anchor function name (for logging)
    const char* pattern; // pattern to find the anchor
    int patternOffset;   // offset from pattern match to anchor start
    const CallSite* sites;
    size_t siteCount;
};

// Resolve an E8 relative call: target = call_addr + 5 + rel32
static uintptr_t ResolveE8Call(uintptr_t callAddr) {
    if (*reinterpret_cast<BYTE*>(callAddr) != 0xE8) {
        return 0; // Not an E8 call
    }
    int32_t rel = *reinterpret_cast<int32_t*>(callAddr + 1);
    return callAddr + 5 + rel;
}

// Read an absolute address operand embedded in an instruction
// addr points to the start of the 4-byte address data (not the opcode)
static uintptr_t ResolveAbsoluteOperand(uintptr_t addr) {
    return *reinterpret_cast<uintptr_t*>(addr);
}

// EXTRACTOR CALL SITE ARRAYS

// MonoHeapCollector::Run @ 0x00d702a0
static const CallSite s_heapCollectorRunSites[] = {
    SITE(Core.mono_domain_get, "Core.mono_domain_get", 0x29),                             // 00d702c9
    SITE(Assemblies.mono_assembly_get_image, "Assemblies.mono_assembly_get_image", 0x43), // 00d702e3
    // +0x7D is mono_class_get (0x00E6F910)
    SITE(Assemblies.mono_loader_get_last_error, "Assemblies.mono_loader_get_last_error", 0x87), // 00d70327
    SITE(Assemblies.mono_loader_clear_error, "Assemblies.mono_loader_clear_error", 0x90),       // 00d70330
    SITE(Core.GC_apply_to_each_object, "Core.GC_apply_to_each_object", 0x1DA),                  // 00d7047a
};

// mono_class_setup_fields @ 0x00e6e600
static const CallSite s_setupFieldsSites[] = {
    SITE(Metadata.mono_metadata_string_heap, "Metadata.mono_metadata_string_heap", 0x31C), // 00e6e91c
    SITE(Metadata.mono_metadata_blob_heap, "Metadata.mono_metadata_blob_heap", 0x32A),     // 00e6e92a
    SITE(Metadata.mono_image_rva_map, "Metadata.mono_image_rva_map", 0x447),               // 00e6ea47
};

// mono_assembly_open_full @ 0x00e68590
static const CallSite s_assemblyOpenFullSites[] = {
    SITE(Core.mono_free, "Core.mono_free", 0x98),                                                    // 00e68628 -> 0x00D70A60
    SITE(Assemblies.mono_image_open_full, "Assemblies.mono_image_open_full", 0x129),                 // 00e686b9 -> 0x00E84140
    SITE(Assemblies.mono_assembly_load_from_full, "Assemblies.mono_assembly_load_from_full", 0x198), // 00e68728 -> 0x00E68290
};

// SPEC_ves_icall_AppDomain_LoadAssemblyRaw @ 0x00e65be0
static const CallSite s_loadAssemblyRawSites[] = {
    SITE(Assemblies.mono_image_open_from_data_full, "Assemblies.mono_image_open_from_data_full", 0x20), // 00e65c00 -> 0x00E84780
    SITE(Assemblies.mono_assembly_load_from_full, "Assemblies.mono_assembly_load_from_full", 0x6C),     // 00e65c4c -> 0x00E68290
};

// mono_array_new @ 0x00EA30F0
static const CallSite s_arrayNewSites[] = {
    SITE(Classes.mono_array_class_get, "Classes.mono_array_class_get", 0x08), // 00ea30f8 -> 0x00E70B50
    SITE(Classes.mono_class_vtable, "Classes.mono_class_vtable", 0x3D),       // 00ea312d -> 0x00EA2AD0
};

// mono_class_get_method_from_name_flags @ 0x00E70330
static const CallSite s_getMethodFromNameFlagsSites[] = {
    SITE(Methods.mono_method_signature, "Methods.mono_method_signature", 0x86), // 00e703b6 -> 0x00E85040
    SITE(Methods.mono_get_method, "Methods.mono_get_method", 0x13B),            // 00e7046b -> 0x00E86290
};

// Sims3::MonoEngine::MonoScriptHost::LoadImage @ 0x00D78760
static const CallSite s_loadImageSites[] = {
    SITE(ScriptHost.GetResourceManager, "ScriptHost.GetResourceManager", 0x05),               // 00d78765 -> 0x004AFD20
    SITE(Assemblies.mono_image_open_from_data, "Assemblies.mono_image_open_from_data", 0x68), // 00d787c8 -> 0x00E84860
};

// interp_mono_invoke_with_state @ 0x00E59FD0
// +0x06: A1 <g_thread_context_tls_index>  (MOV EAX, [imm32])
static const CallSite s_invokeWithStateSites[] = {
    SITE_ABS(Profiler.g_thread_context_tls_index, "Profiler.g_thread_context_tls_index", 0x07), // 00e59fd7 -> 0x011F4160
};

// mono_profiler_allocation @ 0x00EA4C30
// +0x00: F6 05 <g_profiler_events> 80 (TEST byte ptr [imm32], 0x80)
// +0x09: A1 <g_allocation_callback> (MOV EAX, [imm32])
// +0x1B: 8B 0D <g_current_profiler> (MOV ECX, [imm32])
static const CallSite s_profilerAllocationSites[] = {
    SITE_ABS(Profiler.g_profiler_events, "Profiler.g_profiler_events", 0x02),         // 00ea4c32 -> 0x01220E2C
    SITE_ABS(Profiler.g_allocation_callback, "Profiler.g_allocation_callback", 0x0A), // 00ea4c3a -> 0x011F45F0
    SITE_ABS(Profiler.g_current_profiler, "Profiler.g_current_profiler", 0x1D),       // 00ea4c4d -> 0x011F4678
};

// ScriptHost_InitHeap @ 0x00D7F2B0
// +0x00: 83 3D <g_ScriptVM> 00  (CMP dword ptr [g_ScriptVM], 0x0)
static const CallSite s_initHeapSites[] = {
    SITE_ABS(Globals.g_ScriptVM, "Globals.g_ScriptVM", 0x02), // 00d7f2b2 -> 0x011EE514
};

// mono_domain_get @ 0x00E75260
// +0x00: A1 <g_mono_domain_tls_index>  (MOV EAX, [imm32])
static const CallSite s_domainGetSites[] = {
    SITE_ABS(Globals.g_mono_domain_tls_index, "Globals.g_mono_domain_tls_index", 0x01), // 00e75261 -> 0x011959DC
};

// mono_init_internal @ 0x00E75BF0
// Calls mono_class_from_name for each built-in type, stores result via A3 (MOV [imm32], EAX).
// Each A3 store is preceded by an assertion string "mono_defaults.XXX_class != 0".
// Only the 12 primitive type classes used by S3MM are extracted here.
// To add more: find the next A3 store in mono_init_internal, read its operand offset.
// Init order differs from struct layout (e.g., UIntPtr before IntPtr).
static const CallSite s_monoInitInternalSites[] = {
    SITE_ABS(Defaults.boolean_class, "Defaults.boolean_class", 0x1E7), // e75dd7
    SITE_ABS(Defaults.byte_class, "Defaults.byte_class", 0x225),       // e75e15
    SITE_ABS(Defaults.sbyte_class, "Defaults.sbyte_class", 0x262),     // e75e52
    SITE_ABS(Defaults.int16_class, "Defaults.int16_class", 0x2A0),     // e75e90
    SITE_ABS(Defaults.uint16_class, "Defaults.uint16_class", 0x2DE),   // e75ece
    SITE_ABS(Defaults.int32_class, "Defaults.int32_class", 0x31B),     // e75f0b
    SITE_ABS(Defaults.uint32_class, "Defaults.uint32_class", 0x359),   // e75f49
    SITE_ABS(Defaults.int64_class, "Defaults.int64_class", 0x412),     // e76002
    SITE_ABS(Defaults.uint64_class, "Defaults.uint64_class", 0x450),   // e76040
    SITE_ABS(Defaults.single_class, "Defaults.single_class", 0x48D),   // e7607d
    SITE_ABS(Defaults.double_class, "Defaults.double_class", 0x4CB),   // e760bb
    SITE_ABS(Defaults.char_class, "Defaults.char_class", 0x509),       // e760f9
    SITE_ABS(Defaults.string_class, "Defaults.string_class", 0x546),   // e76136
};

// EXTRACTOR TABLE

static const CallSiteExtractor s_extractors[] = {
    {"MonoHeapCollector::Run", "83 EC 1C 53 55 56 57 8B F1 68", 0, s_heapCollectorRunSites, COUNTOF(s_heapCollectorRunSites)},
    {"mono_class_setup_fields", "83 EC 44 53 55 8B 6C 24 50 8B 45 68 56 8B 75 60", 0, s_setupFieldsSites, COUNTOF(s_setupFieldsSites)},
    {"mono_assembly_open_full", "83 EC 08 55 8B 6C 24 10 85 ED 75 ?? 68", 0, s_assemblyOpenFullSites, COUNTOF(s_assemblyOpenFullSites)},
    {"SPEC_ves_icall_AppDomain_LoadAssemblyRaw", "8B 44 24 04 53 8B 58 08 8B 44 24 0C 8B 48 08", 0, s_loadAssemblyRawSites, COUNTOF(s_loadAssemblyRawSites)},
    {"mono_array_new", "8B 44 24 08 56 6A 01 50 E8", 0, s_arrayNewSites, COUNTOF(s_arrayNewSites)},
    {"mono_class_get_method_from_name_flags", "83 EC 18 53 55 56 57 8B 7C 24 2C 57 33 F6 E8", 0, s_getMethodFromNameFlagsSites, COUNTOF(s_getMethodFromNameFlagsSites)},
    {"ScriptHost_LoadImage", "51 53 55 8B D9 E8 ?? ?? ?? ?? 8B 6C", 0, s_loadImageSites, COUNTOF(s_loadImageSites)},
    {"interp_mono_invoke_with_state", "55 8B EC 83 EC 60 A1 ?? ?? ?? ?? 53", 0, s_invokeWithStateSites, COUNTOF(s_invokeWithStateSites)},
    {"mono_profiler_allocation", "F6 05 ?? ?? ?? ?? 80 74 ?? A1 ?? ?? ?? ?? 85 C0 74", 0, s_profilerAllocationSites, COUNTOF(s_profilerAllocationSites)},
    {"ScriptHost_InitHeap", "83 3D ?? ?? ?? ?? 00 74 ?? B0 01 C3 57 8B 3D", 0, s_initHeapSites, COUNTOF(s_initHeapSites)},
    {"mono_domain_get", "A1 ?? ?? ?? ?? 50 FF 15 ?? ?? ?? ?? C3", 0, s_domainGetSites, COUNTOF(s_domainGetSites)},
    {"mono_init_internal", "83 EC 18 53 55 56 33 ED 33 DB 39 2D", 0, s_monoInitInternalSites, COUNTOF(s_monoInitInternalSites)},
};

// ADDRESS ENTRY TABLE

static AddressEntry s_entries[] = {
    // Core
    ADDR(Core.mono_gc_malloc, "8B 0D ?? ?? ?? ?? 8B 44 24 04 53 BA 00 08", 0, 0x00E4BF90, true),
    ADDR(Core.GC_apply_to_each_object, nullptr, 0, 0x00E4A3F0, false),                         // extracted: HeapCollector::Run+0x1DA
    ADDR(Core.mono_free, nullptr, 0, 0x00D70A60, false),                                       // game-side wrapper: SynchronizedFree
    ADDR(Core.mono_domain_get, "A1 ?? ?? ?? ?? 50 FF 15 ?? ?? ?? ?? C3", 0, 0x00E75260, true), // also extractor anchor
    ADDR(Core.mono_value_box, "53 8B 5C 24 0C F6 43 14 08 56 57 75", 0, 0x00EA3140, false),
    ADDR(Core.g_error, "51 8B 4C 24 08 8D 44 24 0C 50 51 8D 54 24 08 52 E8", 0, 0x00E46B10, false), // GLib g_error - fatal assertion handler

    // Classes
    ADDR(Classes.mono_class_from_name, "81 EC 18 04 00 00 53 8B 9C", 0, 0x00E6FA90, true),
    ADDR(Classes.mono_class_get_full, "53 56 8B 74 24 0C 33 DB F6 46 10 02 74", 0, 0x00E6F840, false),
    ADDR(Classes.mono_class_get_methods, "57 8B 7C 24 0C 85 FF 75 ?? 33 C0 5F C3 56 8B 74 24 0C F6 46 14 01 75 ?? 56 E8 ?? ?? ?? ?? 83 C4 04 8B 07 85 C0 75 ?? 56 E8 ?? ?? ?? ?? 83 C4 04 83 7E 70", 0, 0x00E701F0, true),
    ADDR(Classes.mono_class_setup_fields, "83 EC 44 53 55 8B 6C 24 50 8B 45 68 56 8B 75 60 33 C9 57 8B 7D 00", 0, 0x00E6E600, false),
    ADDR(Classes.mono_class_vtable, "53 8B 5C 24 0C 85 DB 75 ?? 68 ?? ?? ?? ?? 68 C2 03 00 00 68", 0, 0x00EA2AD0, false),
    ADDR(Classes.mono_class_get_field_from_name, "53 55 56 57 E8 ?? ?? ?? ?? 8B 6C 24 14 55 E8", 0, 0x00E6F720, false),
    ADDR(Classes.mono_array_class_get, nullptr, 0, 0x00E70B50, false), // extracted: mono_array_new+0x08
    ADDR(Classes.mono_array_element_size, "56 8B 74 24 08 80 7E 10 00 75 ?? 68", 0, 0x00E6B690, false),
    ADDR(Classes.mono_class_get_method_from_name_flags, "83 EC 18 53 55 56 57 8B 7C 24 2C 57 33 F6 E8", 0, 0x00E70330, true),
    ADDR(Classes.SPEC_mono_class_from_mono_type, "8B 44 24 04 0F B6 50 06 8D 4A FF 83 F9 1D 0F 87", 0, 0x00E70D50, true),

    // Methods
    ADDR(Methods.mono_get_method, nullptr, 0, 0x00E86290, true),        // extracted: get_method_from_name_flags+0x13B
    ADDR(Methods.mono_method_signature, nullptr, 0, 0x00E85040, false), // extracted: get_method_from_name_flags+0x86
    ADDR(Methods.mono_method_get_header, "57 8B 7C 24 08 0F B7 07 A9 00 04 00 00 0F 85", 0, 0x00E85390, false),
    ADDR(Methods.mono_method_full_name, "83 EC 40 53 56 8B 74 24 4C 8B 46 08 57 8B 78 38 50 E8", 0, 0x00E72010, false),
    ADDR(Methods.mono_signature_get_desc, "53 55 56 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 6C 24", 0, 0x00E71870, false),
    ADDR(Methods.interp_mono_invoke_with_state, "55 8B EC 83 EC 60 A1 ?? ?? ?? ?? 53", 0, 0x00E59FD0, true),
    ADDR(Methods.SPEC_interp_exec_dispatch, "55 8B EC 83 EC 48 8B 45 0C 8B 48 10 8B 50 0C 56 8D 45 B8 6A 00 50", 0, 0x00E525C0, false),                    // MINT_CALL dispatch within interp_exec_state
    ADDR(Methods.SPEC_interp_exec_dispatch_virtual, "55 8B EC 83 EC 50 8B 45 18 8B 48 08 8B 50 0C 8B 40 10 53 56 57 33 DB", 0, 0x00E52630, false),         // virtual/delegate MINT_CALL dispatch
    ADDR(Methods.mono_raise_exception, "56 8B 74 24 08 8B 06 8B 08 3B 0D ?? ?? ?? ?? 75 ?? E8", 0, 0x00EA18F0, false),                                     // all managed throws go through here
    ADDR(Methods.interp_ex_handler, "51 A1 ?? ?? ?? ?? 55 50 FF 15 ?? ?? ?? ?? 8B E8 85 ED 0F 84", 0, 0x00E53740, false),                                  // interpreter exception handler (installed by mono_interp_init)
    ADDR(Methods.interp_exec_state, "55 8B EC B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 53 8B 5D 08", 0, 0x00E53EB0, false),          // main interpreter dispatch loop (~28KB)
    ADDR(Methods.SPEC_mono_interp_create_runtime_method, "57 8B 7C 24 08 8B 47 14 85 C0 75 ?? 56 E8 ?? ?? ?? ?? 8B 40 18 6A 48 50", 0, 0x00E51F20, false), // RuntimeMethod* create_runtime_method(MonoMethod*), get-or-create
    ADDR(Methods.SPEC_interp_transform_method, "A1 ?? ?? ?? ?? 56 50 33 F6 FF 15 ?? ?? ?? ?? 8B 4C 24 08 F7 41 44 ?? ?? ?? ??", 0, 0x00E52B60, false),     // int transform(RuntimeMethod*) 0 = success

    // Fields
    ADDR(Fields.mono_field_get_value, "8B 44 24 08 8B 08 F6 41 04 10 74 ?? 68 ?? ?? ?? ?? 68 4B 07 00 00 68", 0, 0x00EA1350, false),
    ADDR(Fields.mono_field_set_value, "8B 44 24 08 8B 08 F6 41 04 10 74 ?? 68 ?? ?? ?? ?? 68 11 07 00 00 68", 0, 0x00EA1290, false),
    ADDR(Fields.mono_field_static_get_value, "8B 54 24 08 8B 0A 8B 41 04 A8 10 75 ?? 68 ?? ?? ?? ?? 68 ED", 0, 0x00EA2510, false),
    ADDR(Fields.mono_field_static_set_value, "8B 54 24 08 8B 0A 8B 41 04 A8 10 75 ?? 68 ?? ?? ?? ?? 68 26", 0, 0x00EA12E0, false),

    // Objects
    ADDR(Objects.mono_string_new, "83 EC 08 8B 54 24 10 56 8B C2", 0, 0x00EA40B0, false),
    ADDR(Objects.mono_array_new, "8B 44 24 08 56 6A 01 50 E8", 0, 0x00EA30F0, false),

    // Custom Attributes
    ADDR(CustomAttrs.mono_custom_attrs_from_method, "A1 ?? ?? ?? ?? 85 C0 56 8B 74 24 08 74 ?? 56 50 E8 ?? ?? ?? ?? 83 C4 08 85 C0 75 ?? 56 E8", 0, 0x00EAA530, false),
    ADDR(CustomAttrs.mono_custom_attrs_has_attr, "8B 44 24 04 53 55 56 57 8B 38 33 D2 85 FF 7E", 0, 0x00EAA7E0, false),
    ADDR(CustomAttrs.mono_custom_attrs_get_attr, "8B 4C 24 04 53 55 56 57 8B 39 33 F6 85 FF 7E", 0, 0x00EB5AF0, false),

    // Internal Calls
    ADDR(InternalCalls.mono_add_internal_call, "E8 ?? ?? ?? ?? 8B 44 24 08 8B 4C 24 04 6A 00 50 51 E8", 0, 0x00E82550, false),
    ADDR(InternalCalls.mono_lookup_internal_call, "81 EC 08 08 00 00 53 55 8B AC 24 14 08", 0, 0x00E82680, false),

    // Assemblies
    ADDR(Assemblies.mono_assembly_open_full, "83 EC 08 55 8B 6C 24 10 85 ED 75 ?? 68", 0, 0x00E68590, true),
    ADDR(Assemblies.mono_assembly_get_image, nullptr, 0, 0x00E68140, true),      // extracted: HeapCollector::Run+0x43
    ADDR(Assemblies.mono_assembly_load_from_full, nullptr, 0, 0x00E68290, true), // extracted: open_full+0x198, LoadAssemblyRaw+0x6C
    ADDR(Assemblies.mono_assembly_names_equal, "56 57 8B 7C 24 0C 8B 07 85 C0 0F 84 ?? ?? ?? ?? 8B 74 24 10 8B", 0, 0x00E66990, false),
    ADDR(Assemblies.mono_image_open_from_data_full, nullptr, 0, 0x00E84780, true), // extracted: LoadAssemblyRaw+0x20
    ADDR(Assemblies.mono_image_open_full, nullptr, 0, 0x00E84140, false),          // extracted: open_full+0x129
    ADDR(Assemblies.mono_image_open_from_data, nullptr, 0, 0x00E84860, false),     // wrapper; extracted: ScriptHost_LoadImage+0x68
    ADDR(Assemblies.SPEC_ves_icall_AppDomain_LoadAssemblyRaw, "8B 44 24 04 53 8B 58 08 8B 44", 0, 0x00E65BE0, false),
    ADDR(Assemblies.mono_loader_get_last_error, nullptr, 0, 0x00E848B0, false), // extracted: HeapCollector::Run+0x87
    ADDR(Assemblies.mono_loader_clear_error, nullptr, 0, 0x00E848C0, false),    // extracted: HeapCollector::Run+0x90

    // Metadata
    ADDR(Metadata.mono_metadata_string_heap, nullptr, 0, 0x00E9B060, false), // extracted: setup_fields+0x31C
    ADDR(Metadata.mono_metadata_decode_row_col, "8B 44 24 04 53 8B 5C 24 10 56 0F B6 70 07 0F AF", 0, 0x00E9B1F0, false),
    ADDR(Metadata.mono_metadata_blob_heap, nullptr, 0, 0x00E9B0E0, false), // extracted: setup_fields+0x32A
    ADDR(Metadata.mono_image_rva_map, nullptr, 0, 0x00E84000, false),      // extracted: setup_fields+0x447

    // Profiler
    ADDR(Profiler.g_thread_context_tls_index, nullptr, 0, 0x011F4160, false), // extracted (abs): invoke+0x07
    ADDR(Profiler.g_profiler_events, nullptr, 0, 0x01220E2C, false),          // extracted (abs): profiler_allocation+0x02
    ADDR(Profiler.g_allocation_callback, nullptr, 0, 0x011F45F0, false),      // extracted (abs): profiler_allocation+0x0A
    ADDR(Profiler.g_current_profiler, nullptr, 0, 0x011F4678, false),         // extracted (abs): profiler_allocation+0x1D

    // Globals
    ADDR(Globals.g_ScriptVM, nullptr, 0, 0x011EE514, false),              // extracted (abs): ScriptHost_InitHeap+0x02
    ADDR(Globals.g_mono_domain_tls_index, nullptr, 0, 0x011959DC, false), // extracted (abs): mono_domain_get+0x01

    // MonoDefaults - primitive types only (12 of 62)
    // Extracted from mono_init_internal; fallback = global address of mono_defaults field
    ADDR(Defaults.boolean_class, nullptr, 0, 0x01220E50, false), // +0x10
    ADDR(Defaults.sbyte_class, nullptr, 0, 0x01220E54, false),   // +0x14
    ADDR(Defaults.byte_class, nullptr, 0, 0x01220E48, false),    // +0x08
    ADDR(Defaults.int16_class, nullptr, 0, 0x01220E58, false),   // +0x18
    ADDR(Defaults.uint16_class, nullptr, 0, 0x01220E5C, false),  // +0x1C
    ADDR(Defaults.int32_class, nullptr, 0, 0x01220E60, false),   // +0x20
    ADDR(Defaults.uint32_class, nullptr, 0, 0x01220E64, false),  // +0x24
    ADDR(Defaults.int64_class, nullptr, 0, 0x01220E70, false),   // +0x30
    ADDR(Defaults.uint64_class, nullptr, 0, 0x01220E74, false),  // +0x34
    ADDR(Defaults.single_class, nullptr, 0, 0x01220E78, false),  // +0x38
    ADDR(Defaults.double_class, nullptr, 0, 0x01220E7C, false),  // +0x3C
    ADDR(Defaults.char_class, nullptr, 0, 0x01220E80, false),    // +0x40
    ADDR(Defaults.string_class, nullptr, 0, 0x01220E84, false),  // +0x44
    // Remaining mono_defaults fields: trace A3 stores in mono_init_internal.
    // Each target operand is a mono_defaults global. Base ~0x01220E40, stride 4.

    // Game - Script Host
    ADDR(ScriptHost.ScriptHost_FindClass, "83 EC 1C 53 8B 5C 24 24 33 C0 55 56 89 44", 0, 0x00D79CA0, false),
    ADDR(ScriptHost.ScriptHost_LoadImage, "51 53 55 8B D9 E8 ?? ?? ?? ?? 8B 6C", 0, 0x00D78760, false),
    ADDR(ScriptHost.ScriptHost_InitHeap, "83 3D ?? ?? ?? ?? 00 74 ?? B0 01 C3 57 8B 3D", 0, 0x00D7F2B0, false),
    ADDR(ScriptHost.GetResourceManager, nullptr, 0, 0x004AFD20, false), // extracted: ScriptHost_LoadImage+0x05
    // MonoScriptHost::ProcessTasks @ 0x00D7FC60 - per-frame task pump, sole caller is Simulate
    ADDR(ScriptHost.MonoScriptHost_ProcessTasks, "55 8B EC 83 E4 F8 83 EC 50 53 55 56 8B D9 8B 83 ?? ?? ?? ?? 8D B3 ?? ?? ?? ?? 57 50 8B CE", 0, 0x00D7FC60, false),

    // Game - Misc
    ADDR(Misc.RefPack_Decompress, "56 8B 74 24 08 66 8B 06 8A E8 8A CC 0F B7 C1", 0, 0x004EC010, false),
    ADDR(Misc.GameUtils_StartupComplete, "E8 ?? ?? ?? ?? 85 C0 74 ?? 8B 10 6A 00 6A 00 6A 00 8B C8 8B 42 1C 68 AE E3 94 D5", 0, 0x00EC0EF0, false),

    // Game - INI System
    ADDR(Misc.IniScalarRead, "83 EC 10 8B 4C 24 14 56 8D 54 24 04 52 8B 54 24 24 B8", 0, 0x004EF2B0, false),
};

static constexpr size_t ENTRY_COUNT = COUNTOF(s_entries);

static constexpr size_t EXTRACTOR_COUNT = COUNTOF(s_extractors);

static bool s_initialized = false;
static int s_scannedCount = 0;
static int s_extractedCount = 0;
static int s_fallbackCount = 0;
static int s_failedCount = 0;

// BATCH PATTERN SCANNER
// Single-pass scan: collects all patterns, scans the module image once with first-byte dispatch.
// Turns O(P*N) into O(N*k) where k << P.

struct PatternJob {
    std::vector<BYTE> bytes;
    std::vector<BYTE> masks;
    uintptr_t result = 0;
};

// Parse a pattern string ("55 8B EC ?? ??") into byte/mask arrays
static bool ParsePatternString(const char* pattern, std::vector<BYTE>& outBytes, std::vector<BYTE>& outMasks) {
    outBytes.clear();
    outMasks.clear();

    auto parseNibble = [](char c) -> BYTE {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return 0;
    };

    const char* p = pattern;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        char hiChar = *p;
        char loChar = *(p + 1);
        if (!loChar || loChar == ' ') return false;

        bool hiWild = (hiChar == '?');
        bool loWild = (loChar == '?');
        bool hiHex = isxdigit(static_cast<unsigned char>(hiChar)) != 0;
        bool loHex = isxdigit(static_cast<unsigned char>(loChar)) != 0;

        if ((!hiWild && !hiHex) || (!loWild && !loHex)) return false;

        BYTE byteVal = 0;
        BYTE maskVal = 0;
        if (hiHex) {
            byteVal |= (parseNibble(hiChar) << 4);
            maskVal |= 0xF0;
        }
        if (loHex) {
            byteVal |= parseNibble(loChar);
            maskVal |= 0x0F;
        }

        outBytes.push_back(byteVal);
        outMasks.push_back(maskVal);
        p += 2;
    }

    return !outBytes.empty();
}

// Scan the entire module image once, matching all patterns via first-byte dispatch
static void BatchScanPatterns(BYTE* start, size_t size, std::vector<PatternJob>& jobs) {
    if (jobs.empty() || size == 0) return;

    // Build first-byte dispatch table: byte value -> list of job indices
    std::vector<std::vector<size_t>> dispatch(256);

    for (size_t i = 0; i < jobs.size(); i++) {
        if (jobs[i].bytes.empty()) continue;
        BYTE firstByte = jobs[i].bytes[0];
        BYTE firstMask = jobs[i].masks[0];

        if (firstMask == 0xFF) {
            // Exact first byte - add to one bucket
            dispatch[firstByte].push_back(i);
        } else {
            // Partial/wildcard first byte - add to all matching buckets
            for (int b = 0; b < 256; b++) {
                if ((b & firstMask) == (firstByte & firstMask)) { dispatch[b].push_back(i); }
            }
        }
    }

    size_t remaining = jobs.size();

    for (size_t i = 0; i < size && remaining > 0; i++) {
        const auto& candidates = dispatch[start[i]];
        if (candidates.empty()) continue;

        for (size_t jobIdx : candidates) {
            auto& job = jobs[jobIdx];
            if (job.result != 0) continue;             // already found
            if (i + job.bytes.size() > size) continue; // bounds check

            bool match = true;
            for (size_t j = 1; j < job.bytes.size(); j++) { // j=1: first byte already matched via dispatch
                if ((start[i + j] & job.masks[j]) != (job.bytes[j] & job.masks[j])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                job.result = reinterpret_cast<uintptr_t>(start + i);
                remaining--;
            }
        }
    }
}

// IMPLEMENTATION

bool Initialize() {
    if (s_initialized) { return true; }

    ULONGLONG t0 = GetTickCount64();

    auto mod = Memory::GetMainModule();
    BYTE* base = reinterpret_cast<BYTE*>(mod.base);
    size_t size = mod.size;

    s_scannedCount = 0;
    s_extractedCount = 0;
    s_fallbackCount = 0;
    s_failedCount = 0;

    bool allCriticalResolved = true;

    // Collect all unique patterns
    std::vector<PatternJob> jobs;
    std::unordered_map<std::string, size_t> patternIndex; // pattern string -> job index (dedup)
    constexpr size_t NO_JOB = static_cast<size_t>(-1);

    auto getOrCreateJob = [&](const char* pattern) -> size_t {
        std::string key(pattern);
        auto it = patternIndex.find(key);
        if (it != patternIndex.end()) return it->second;

        size_t idx = jobs.size();
        PatternJob job;
        ParsePatternString(pattern, job.bytes, job.masks);
        jobs.push_back(std::move(job));
        patternIndex[key] = idx;
        return idx;
    };

    // Register extractor anchor patterns
    std::vector<size_t> extractorJobIdx(EXTRACTOR_COUNT);
    for (size_t i = 0; i < EXTRACTOR_COUNT; i++) { extractorJobIdx[i] = getOrCreateJob(s_extractors[i].pattern); }

    // Register entry patterns (skip entries with no pattern)
    std::vector<size_t> entryJobIdx(ENTRY_COUNT, NO_JOB);
    for (size_t i = 0; i < ENTRY_COUNT; i++) {
        if (s_entries[i].pattern && s_entries[i].pattern[0] != '\0') { entryJobIdx[i] = getOrCreateJob(s_entries[i].pattern); }
    }

    LOG_INFO("[Addresses] Batch scanning " + std::to_string(jobs.size()) + " unique patterns across " + std::to_string(size / 1024) + " KB...");

    // Single-pass batch scan
    ULONGLONG tScan0 = GetTickCount64();
    BatchScanPatterns(base, size, jobs);
    ULONGLONG tScan1 = GetTickCount64();

    LOG_INFO("[Addresses] Batch scan completed in " + std::to_string(tScan1 - tScan0) + " ms");

    // Phase 1: Process extractors
    for (size_t i = 0; i < EXTRACTOR_COUNT; i++) {
        auto& job = jobs[extractorJobIdx[i]];
        if (job.result == 0) {
            LOG_WARN("[Addresses] Extractor '" + std::string(s_extractors[i].name) + "': anchor pattern not found");
            continue;
        }

        uintptr_t anchor = job.result + s_extractors[i].patternOffset;
        LOG_DEBUG("[Addresses] Extractor '" + std::string(s_extractors[i].name) + "': anchor at " + ToHex(anchor));

        for (size_t j = 0; j < s_extractors[i].siteCount; j++) {
            const CallSite& site = s_extractors[i].sites[j];

            // Skip if already resolved (by a previous extractor)
            if (*site.target != 0) { continue; }

            uintptr_t callAddr = anchor + site.offsetToCall;
            uintptr_t resolved = site.isAbsolute ? ResolveAbsoluteOperand(callAddr) : ResolveE8Call(callAddr);

            if (resolved != 0) {
                *site.target = resolved;
                s_extractedCount++;
                LOG_DEBUG("[Addresses] " + std::string(site.name) + ": extracted from " + s_extractors[i].name + "+" + ToHex(site.offsetToCall) + " -> " + ToHex(resolved));
            } else {
                LOG_WARN("[Addresses] " + std::string(site.name) + ": failed to extract from " + s_extractors[i].name + "+" + ToHex(site.offsetToCall));
            }
        }
    }

    // Phase 2: Process entries
    for (size_t i = 0; i < ENTRY_COUNT; i++) {
        // Skip if already resolved by extractor
        if (*s_entries[i].target != 0) { continue; }

        bool resolved = false;

        // Use batch scan result
        if (entryJobIdx[i] != NO_JOB) {
            auto& job = jobs[entryJobIdx[i]];
            if (job.result != 0) {
                *s_entries[i].target = job.result + s_entries[i].offset;
                s_scannedCount++;
                resolved = true;
                LOG_DEBUG("[Addresses] " + std::string(s_entries[i].name) + ": pattern matched at " + ToHex(*s_entries[i].target));
            }
        }

        // Fallback to hardcoded
        if (!resolved && s_entries[i].fallback != 0) {
            *s_entries[i].target = s_entries[i].fallback;
            s_fallbackCount++;
            resolved = true;
            LOG_DEBUG("[Addresses] " + std::string(s_entries[i].name) + ": using fallback " + ToHex(*s_entries[i].target));
        }

        // Track failures
        if (!resolved) {
            s_failedCount++;
            LOG_ERROR("[Addresses] " + std::string(s_entries[i].name) + ": FAILED to resolve");
            if (s_entries[i].critical) { allCriticalResolved = false; }
        }
    }

    ULONGLONG tTotal = GetTickCount64() - t0;
    LOG_INFO("[Addresses] Initialized in " + std::to_string(tTotal) + " ms: " + std::to_string(s_extractedCount) + " extracted, " + std::to_string(s_scannedCount) + " scanned, " + std::to_string(s_fallbackCount) +
             " fallback, " + std::to_string(s_failedCount) + " failed (total entries: " + std::to_string((int)ENTRY_COUNT) + ")");

    s_initialized = allCriticalResolved;
    return allCriticalResolved;
}

int Validate() {
    if (!s_initialized) {
        LOG_ERROR("[Addresses] Validate() called before Initialize()");
        return -1;
    }

    int matches = 0;
    int mismatches = 0;
    int noFallback = 0;

    LOG_INFO("[Addresses] ========== VALIDATION ==========");

    for (size_t i = 0; i < ENTRY_COUNT; i++) {
        const auto& entry = s_entries[i];
        uintptr_t resolved = *entry.target;

        if (entry.fallback == 0) {
            // No fallback to compare against
            LOG_INFO("[Addresses]   " + std::string(entry.name) + ": " + ToHex(resolved) + " (no fallback to compare)");
            noFallback++;
            continue;
        }

        if (resolved == entry.fallback) {
            LOG_INFO("[Addresses]   " + std::string(entry.name) + ": " + ToHex(resolved) + " MATCH");
            matches++;
        } else {
            LOG_ERROR("[Addresses]   " + std::string(entry.name) + ": " + ToHex(resolved) + " MISMATCH (expected " + ToHex(entry.fallback) + ")");
            mismatches++;
        }
    }

    LOG_INFO("[Addresses] Validation complete: " + std::to_string(matches) + " match, " + std::to_string(mismatches) + " mismatch, " + std::to_string(noFallback) + " no fallback");
    LOG_INFO("[Addresses] ================================");

    return mismatches;
}

} // namespace Addresses
} // namespace S3MM
