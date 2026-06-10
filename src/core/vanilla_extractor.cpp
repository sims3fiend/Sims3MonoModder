#include "vanilla_extractor.h"
#include "../modding/package_reader.h"
#include "mono_bridge.h"
#include "addresses.h"
#include "../modding/mod_compiler.h"
#include "../util/logger.h"
#include <Windows.h>
#include <map>
#include <set>
#include <mutex>
#include <fstream>
#include <filesystem>

namespace S3MM {

// VANILLA ASSEMBLY STORAGE - In-memory cache of extracted vanilla DLLs
struct VanillaAssemblyData {
    std::string assemblyName;      // "ScriptCore", "SimIFace", etc.
    std::string sourcePackagePath; // Path to vanilla .package (e.g., scripts.package)
    std::vector<uint8_t> bytes;    // Raw DLL bytes (decrypted from S3SA)
    void* monoImage = nullptr;     // Loaded MonoImage for comparison (not registered as assembly)
};
static std::map<std::string, VanillaAssemblyData> s_vanillaAssemblies;
static std::mutex s_vanillaAssembliesMutex;

// Discovered vanilla packages
static std::set<std::string> s_discoveredVanillaPackages;
static std::mutex s_discoveredPackagesMutex;

// mono_image_open_from_data_full, for loading vanilla as MonoImage
using mono_image_open_from_data_vanilla_t = void*(__cdecl*)(char* data, uint32_t data_len, int need_copy, int* status, int refonly);
static mono_image_open_from_data_vanilla_t s_imageOpenFromDataVanilla = nullptr;

// KNOWN ASSEMBLY INSTANCE IDS - FNV-1a 64-bit hashes to assembly name mapping
const char* LookupAssemblyName(uint64_t instanceId) {
    switch (instanceId) {
    case 0x78CF6CF5304D0C4F:
        return "ScriptCore";
    case 0xC356DF69B70ADD42:
        return "SimIFace";
    case 0x28EE9D383A73463E:
        return "mscorlib";
    case 0x03D6C8D903CE868C:
        return "Sims3GameplaySystems";
    case 0x600F9EA1DDC99FB1:
        return "Sims3MetaData";
    case 0x0CAE1C361E05B2B3:
        return "Sims3StoreObjects";
    case 0x342EE04373CF1E1C:
        return "System";
    case 0x6AC101133051BEF1:
        return "System.Xml";
    case 0xF7C3ADE896D4E765:
        return "UI";
    case 0x158B3F1099AD94BC:
        return "ScriptCore_Pure";
    case 0xB9C90FDC6793BC0A:
        return "Sims3GameplayObjects";
    default:
        return nullptr;
    }
}

// PACKAGE DISCOVERY - Track vanilla .package files found during assembly loading
void RecordVanillaPackage(const std::string& packagePath) {
    std::lock_guard<std::mutex> lock(s_discoveredPackagesMutex);
    if (s_discoveredVanillaPackages.insert(packagePath).second) { LOG_INFO("[Vanilla] Discovered vanilla package: " + packagePath); }
}

std::vector<std::string> GetDiscoveredPackages() {
    std::lock_guard<std::mutex> lock(s_discoveredPackagesMutex);
    return std::vector<std::string>(s_discoveredVanillaPackages.begin(), s_discoveredVanillaPackages.end());
}

static void* LoadVanillaAsMonoImage(const std::string& assemblyName) {
    std::lock_guard<std::mutex> lock(s_vanillaAssembliesMutex);

    auto it = s_vanillaAssemblies.find(assemblyName);
    if (it == s_vanillaAssemblies.end()) return nullptr;

    VanillaAssemblyData& data = it->second;
    if (data.monoImage) return data.monoImage;
    if (data.bytes.empty()) return nullptr;

    LOG_INFO("[Vanilla] Loading " + assemblyName + " as MonoImage for comparison...");
    if (!s_imageOpenFromDataVanilla) s_imageOpenFromDataVanilla = (mono_image_open_from_data_vanilla_t)Addresses::Assemblies.mono_image_open_from_data_full;

    int status = 0;
    void* image = s_imageOpenFromDataVanilla(reinterpret_cast<char*>(data.bytes.data()), static_cast<uint32_t>(data.bytes.size()),
        1, // need_copy = true
        &status,
        1 // refonly = true (use separate cache to avoid collision with loaded mods)
    );

    if (image && status == 0) {
        data.monoImage = image;
        char buf[64];
        snprintf(buf, sizeof(buf), "[Vanilla] Loaded MonoImage @ 0x%08X", (uintptr_t)image);
        LOG_INFO(buf);

    } else {
        LOG_ERROR("[Vanilla] Failed to load MonoImage (status=" + std::to_string(status) + ")");
    }

    return data.monoImage;
}

// PUBLIC API - Store, query, and load vanilla assemblies
void StoreVanillaAssembly(const char* assemblyName, const char* sourcePath, uint8_t* data, uint32_t size) {
    if (!assemblyName || !data || size == 0) {
        if (data) delete[] data;
        return;
    }

    std::string name(assemblyName);

    {
        std::lock_guard<std::mutex> lock(s_vanillaAssembliesMutex);
        if (s_vanillaAssemblies.find(name) != s_vanillaAssemblies.end()) {
            LOG_DEBUG("[Vanilla] Already have " + name + ", skipping duplicate");
            delete[] data;
            return;
        }
    }

    LOG_INFO("[Vanilla] Storing assembly: " + name + " (" + std::to_string(size) + " bytes)");

    VanillaAssemblyData vanillaData;
    vanillaData.assemblyName = name;
    vanillaData.sourcePackagePath = sourcePath ? sourcePath : "";
    vanillaData.bytes.resize(size);
    memcpy(vanillaData.bytes.data(), data, size);
    delete[] data;

    {
        std::lock_guard<std::mutex> lock(s_vanillaAssembliesMutex);
        s_vanillaAssemblies[name] = std::move(vanillaData);
    }

    LoadVanillaAsMonoImage(name);
}

bool HasVanillaAssembly(const char* assemblyName) {
    if (!assemblyName) return false;
    std::lock_guard<std::mutex> lock(s_vanillaAssembliesMutex);
    return s_vanillaAssemblies.find(assemblyName) != s_vanillaAssemblies.end();
}

void* GetVanillaImage(const char* assemblyName) {
    if (!assemblyName) return nullptr;
    std::lock_guard<std::mutex> lock(s_vanillaAssembliesMutex);
    auto it = s_vanillaAssemblies.find(assemblyName);
    if (it == s_vanillaAssemblies.end()) return nullptr;
    return it->second.monoImage;
}

const uint8_t* GetVanillaBytes(const char* assemblyName, uint32_t* outSize) {
    if (!assemblyName) return nullptr;
    std::lock_guard<std::mutex> lock(s_vanillaAssembliesMutex);
    auto it = s_vanillaAssemblies.find(assemblyName);
    if (it == s_vanillaAssemblies.end()) return nullptr;
    if (it->second.bytes.empty()) return nullptr;
    if (outSize) *outSize = (uint32_t)it->second.bytes.size();
    return it->second.bytes.data();
}

// Direct PE RVA resolution, bypasses Mono's potentially broken lazy loading
const uint8_t* VanillaRvaToPointer(const uint8_t* peBytes, uint32_t peSize, uint32_t rva) {
    if (!peBytes || peSize < 64 || rva == 0) return nullptr;

    // DOS header check
    if (peBytes[0] != 'M' || peBytes[1] != 'Z') return nullptr;

    // PE header offset at 0x3C
    uint32_t peOffset = *(uint32_t*)(peBytes + 0x3C);
    if (peOffset + 24 > peSize) return nullptr;

    // Verify PE signature
    if (memcmp(peBytes + peOffset, "PE\0\0", 4) != 0) return nullptr;

    // COFF header starts at peOffset + 4
    uint16_t numSections = *(uint16_t*)(peBytes + peOffset + 6);
    uint16_t optHeaderSize = *(uint16_t*)(peBytes + peOffset + 20);

    // Section table starts after optional header
    uint32_t sectionTableOffset = peOffset + 24 + optHeaderSize;
    if (sectionTableOffset + numSections * 40 > peSize) return nullptr;

    // Search sections for the RVA
    for (uint16_t i = 0; i < numSections; i++) {
        const uint8_t* section = peBytes + sectionTableOffset + i * 40;
        uint32_t virtualSize = *(uint32_t*)(section + 8);
        uint32_t virtualAddr = *(uint32_t*)(section + 12);
        uint32_t rawOffset = *(uint32_t*)(section + 20);

        // Check if RVA falls within this section
        if (rva >= virtualAddr && rva < virtualAddr + virtualSize) {
            uint32_t offset = rawOffset + (rva - virtualAddr);
            if (offset < peSize && offset + 1 <= peSize) { return peBytes + offset; }
            return nullptr; // Out of bounds
        }
    }

    return nullptr; // RVA not found in any section
}

// PE METADATA TABLE PARSING - Column size calculation helpers for coded indices

// Coded index tag bits and referenced tables
struct CodedIndexInfo {
    int tagBits;
    const int* tables;
    int tableCount;
};

// Table definitions for coded indices (ECMA-335 II.24.2.6)
static const int kTypeDefOrRef[] = {0x02, 0x01, 0x1B}; // TypeDef, TypeRef, TypeSpec
static const int kHasConstant[] = {0x04, 0x08, 0x17};  // Field, Param, Property
static const int kHasCustomAttribute[] = {0x06, 0x04, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x00, 0x0E, 0x17, 0x14, 0x11, 0x1A, 0x1B, 0x20, 0x23, 0x26, 0x27, 0x28, 0x2A, 0x2B, 0x2C};
static const int kMemberRefParent[] = {0x02, 0x01, 0x1A, 0x06, 0x1B}; // TypeDef, TypeRef, ModuleRef, MethodDef, TypeSpec
static const int kHasSemantics[] = {0x14, 0x17};                      // Event, Property
static const int kMethodDefOrRef[] = {0x06, 0x0A};                    // MethodDef, MemberRef
static const int kHasFieldMarshal[] = {0x04, 0x08};                   // Field, Param
static const int kHasDeclSecurity[] = {0x02, 0x06, 0x20};             // TypeDef, MethodDef, Assembly
static const int kMemberForwarded[] = {0x04, 0x06};                   // Field, MethodDef
static const int kImplementation[] = {0x26, 0x23, 0x27};              // File, AssemblyRef, ExportedType
static const int kCustomAttributeType[] = {-1, -1, 0x06, 0x0A, -1};   // MethodDef, MemberRef (others unused)
static const int kResolutionScope[] = {0x00, 0x1A, 0x23, 0x01};       // Module, ModuleRef, AssemblyRef, TypeRef

// Calculate number of bits needed for coded index tag
static int GetTagBits(int tableCount) {
    if (tableCount <= 2) return 1;
    if (tableCount <= 4) return 2;
    if (tableCount <= 8) return 3;
    if (tableCount <= 16) return 4;
    return 5;
}

// Calculate coded index size (2 or 4 bytes)
static int GetCodedIndexSize(const uint32_t* tableRows, const int* tables, int tableCount) {
    int tagBits = GetTagBits(tableCount);
    uint32_t maxRows = 0;
    for (int i = 0; i < tableCount; i++) {
        if (tables[i] >= 0 && tables[i] < 64) {
            if (tableRows[tables[i]] > maxRows) maxRows = tableRows[tables[i]];
        }
    }
    // If maxRows << tagBits fits in 16 bits, use 2 bytes
    return (maxRows < (1u << (16 - tagBits))) ? 2 : 4;
}

// Get simple table index size (2 or 4 bytes)
static int GetTableIndexSize(uint32_t rowCount) {
    return (rowCount < 65536) ? 2 : 4;
}

// Row size calculator per ECMA-335 II.22. Must match spec exactly or table offsets drift.
// Gotchas: Assembly(0x20) is 26 bytes, MethodSemantics(0x18) uses simple MethodDef index, heap index sizes depend on heapSizes byte (bit 0=strings, 1=GUID, 2=blob)
static int CalcRowSize(int tableId, const uint32_t* tableRows, uint8_t heapSizes) {
    int strSize = (heapSizes & 1) ? 4 : 2;
    int guidSize = (heapSizes & 2) ? 4 : 2;
    int blobSize = (heapSizes & 4) ? 4 : 2;

    switch (tableId) {
    case 0x00:
        return 2 + strSize + guidSize + guidSize + guidSize; // Module
    case 0x01:
        return GetCodedIndexSize(tableRows, kResolutionScope, 4) + strSize + strSize; // TypeRef
    case 0x02:
        return 4 + strSize + strSize + GetCodedIndexSize(tableRows, kTypeDefOrRef, 3) + GetTableIndexSize(tableRows[0x04]) + GetTableIndexSize(tableRows[0x06]); // TypeDef
    case 0x03:
        return GetTableIndexSize(tableRows[0x04]); // FieldPtr (rare, edit-and-continue)
    case 0x04:
        return 2 + strSize + blobSize; // Field
    case 0x05:
        return GetTableIndexSize(tableRows[0x06]); // MethodPtr (rare, edit-and-continue)
    case 0x06:
        return 4 + 2 + 2 + strSize + blobSize + GetTableIndexSize(tableRows[0x08]); // MethodDef
    case 0x07:
        return GetTableIndexSize(tableRows[0x08]); // ParamPtr (rare, edit-and-continue)
    case 0x08:
        return 2 + 2 + strSize; // Param
    case 0x09:
        return GetTableIndexSize(tableRows[0x02]) + GetCodedIndexSize(tableRows, kTypeDefOrRef, 3); // InterfaceImpl
    case 0x0A:
        return GetCodedIndexSize(tableRows, kMemberRefParent, 5) + strSize + blobSize; // MemberRef
    case 0x0B:
        return GetCodedIndexSize(tableRows, kHasConstant, 3) + 1 + 1 + blobSize; // Constant
    case 0x0C:
        return GetCodedIndexSize(tableRows, kHasCustomAttribute, 22) + GetCodedIndexSize(tableRows, kCustomAttributeType, 5) + blobSize; // CustomAttribute
    case 0x0D:
        return GetCodedIndexSize(tableRows, kHasFieldMarshal, 2) + blobSize; // FieldMarshal
    case 0x0E:
        return 2 + GetCodedIndexSize(tableRows, kHasDeclSecurity, 3) + blobSize; // DeclSecurity
    case 0x0F:
        return 2 + 4 + GetTableIndexSize(tableRows[0x02]); // ClassLayout
    case 0x10:
        return 4 + GetTableIndexSize(tableRows[0x04]); // FieldLayout
    case 0x11:
        return blobSize; // StandAloneSig (just blob index)
    case 0x12:
        return GetTableIndexSize(tableRows[0x02]) + GetTableIndexSize(tableRows[0x14]); // EventMap
    case 0x13:
        return GetTableIndexSize(tableRows[0x14]); // EventPtr (rare)
    case 0x14:
        return 2 + strSize + GetCodedIndexSize(tableRows, kTypeDefOrRef, 3); // Event
    case 0x15:
        return GetTableIndexSize(tableRows[0x02]) + GetTableIndexSize(tableRows[0x17]); // PropertyMap
    case 0x16:
        return GetTableIndexSize(tableRows[0x17]); // PropertyPtr (rare)
    case 0x17:
        return 2 + strSize + blobSize; // Property
    case 0x18:
        return 2 + GetTableIndexSize(tableRows[0x06]) + GetCodedIndexSize(tableRows, kHasSemantics, 2); // MethodSemantics
    case 0x19:
        return GetTableIndexSize(tableRows[0x02]) + GetCodedIndexSize(tableRows, kMethodDefOrRef, 2) + GetCodedIndexSize(tableRows, kMethodDefOrRef, 2); // MethodImpl
    case 0x1A:
        return strSize; // ModuleRef
    case 0x1B:
        return blobSize; // TypeSpec
    case 0x1C:
        return 2 + GetCodedIndexSize(tableRows, kMemberForwarded, 2) + strSize + GetTableIndexSize(tableRows[0x1A]); // ImplMap
    case 0x1D:
        return 4 + GetTableIndexSize(tableRows[0x04]); // FieldRVA
    case 0x20:
        return 4 + 2 + 2 + 2 + 2 + 4 + blobSize + strSize + strSize; // Assembly (HashAlgId, Major, Minor, Build, Rev, Flags, PubKey, Name, Culture)
    case 0x21:
        return 4; // AssemblyProcessor
    case 0x22:
        return 4 + 4 + 4; // AssemblyOS
    case 0x23:
        return 2 + 2 + 2 + 2 + 4 + blobSize + strSize + strSize + blobSize; // AssemblyRef
    case 0x24:
        return 4 + GetTableIndexSize(tableRows[0x23]); // AssemblyRefProcessor
    case 0x25:
        return 4 + 4 + 4 + GetTableIndexSize(tableRows[0x23]); // AssemblyRefOS
    case 0x26:
        return 4 + strSize + blobSize; // File
    case 0x27:
        return 4 + 4 + strSize + strSize + GetCodedIndexSize(tableRows, kImplementation, 3); // ExportedType
    case 0x28:
        return 4 + 4 + strSize + GetCodedIndexSize(tableRows, kImplementation, 3); // ManifestResource
    case 0x29:
        return GetTableIndexSize(tableRows[0x02]) + GetTableIndexSize(tableRows[0x02]); // NestedClass
    case 0x2A:
        return 2 + 2 + GetCodedIndexSize(tableRows, kTypeDefOrRef, 3) + strSize; // GenericParam
    case 0x2B:
        return GetCodedIndexSize(tableRows, kMethodDefOrRef, 2) + blobSize; // MethodSpec
    case 0x2C:
        return GetTableIndexSize(tableRows[0x2A]) + GetCodedIndexSize(tableRows, kTypeDefOrRef, 3); // GenericParamConstraint
    default:
        return 0; // Unknown table - will break offset calc if this table has rows!
    }
}

// Initialize PE metadata context for direct parsing
bool InitPEMetadata(PEMetadata* ctx, const uint8_t* peBytes, uint32_t peSize) {
    if (!ctx || !peBytes || peSize < 512) return false;

    memset(ctx, 0, sizeof(PEMetadata));
    ctx->peBytes = peBytes;
    ctx->peSize = peSize;

    // DOS header check
    if (peBytes[0] != 'M' || peBytes[1] != 'Z') return false;

    // PE header offset
    uint32_t peOffset = *(uint32_t*)(peBytes + 0x3C);
    if (peOffset + 24 > peSize) return false;
    if (memcmp(peBytes + peOffset, "PE\0\0", 4) != 0) return false;

    // Optional header, need to find CLI header RVA
    uint16_t magic = *(uint16_t*)(peBytes + peOffset + 24);
    uint32_t cliHeaderOffset = (magic == 0x20B) ? 224 : 208;

    uint32_t cliHeaderRva = *(uint32_t*)(peBytes + peOffset + 24 + cliHeaderOffset);
    if (cliHeaderRva == 0) return false;

    const uint8_t* cliHeader = VanillaRvaToPointer(peBytes, peSize, cliHeaderRva);
    if (!cliHeader) return false;

    uint32_t metadataRva = *(uint32_t*)(cliHeader + 8);
    const uint8_t* metadata = VanillaRvaToPointer(peBytes, peSize, metadataRva);
    if (!metadata) return false;

    if (memcmp(metadata, "BSJB", 4) != 0) return false;

    uint32_t versionLen = *(uint32_t*)(metadata + 12);
    uint32_t streamsOffset = 16 + ((versionLen + 3) & ~3);
    uint16_t numStreams = *(uint16_t*)(metadata + streamsOffset + 2);

    // Parse stream headers
    const uint8_t* streamPtr = metadata + streamsOffset + 4;
    for (uint16_t i = 0; i < numStreams; i++) {
        uint32_t streamOffset = *(uint32_t*)streamPtr;
        uint32_t streamSize = *(uint32_t*)(streamPtr + 4);
        const char* streamName = (const char*)(streamPtr + 8);

        if (strcmp(streamName, "#Strings") == 0) {
            ctx->stringsHeap = metadata + streamOffset;
            ctx->stringsHeapSize = streamSize;
        } else if (strcmp(streamName, "#Blob") == 0) {
            ctx->blobHeap = metadata + streamOffset;
            ctx->blobHeapSize = streamSize;
        } else if (strcmp(streamName, "#US") == 0) {
            ctx->usHeap = metadata + streamOffset;
            ctx->usHeapSize = streamSize;
        } else if (strcmp(streamName, "#~") == 0 || strcmp(streamName, "#-") == 0) {
            ctx->tablesBase = metadata + streamOffset;
        }

        size_t nameLen = strlen(streamName) + 1;
        streamPtr += 8 + ((nameLen + 3) & ~3);
    }

    if (!ctx->tablesBase || !ctx->stringsHeap) return false;

    // Parse #~ stream header: reserved(4), major(1), minor(1), heapSizes(1), reserved(1), valid(8), sorted(8), rows...
    const uint8_t* tbl = ctx->tablesBase;
    ctx->heapSizes = tbl[6];
    uint64_t validMask = *(uint64_t*)(tbl + 8);

    // Count bits set in validMask
    int validTableCount = 0;
    for (int i = 0; i < 64; i++) {
        if (validMask & (1ULL << i)) validTableCount++;
    }

    // Read row counts for each valid table
    const uint32_t* rowPtr = (const uint32_t*)(tbl + 24);
    for (int i = 0; i < 64; i++) {
        if (validMask & (1ULL << i)) { ctx->tableRows[i] = *rowPtr++; }
    }

    // Calculate table offsets using ECMA-335 row size formulas
    uint32_t offset = 0;
    ctx->tableData = (const uint8_t*)rowPtr; // Table data starts after row counts

    for (int i = 0; i < 64; i++) {
        ctx->tableOffsets[i] = offset;
        if (ctx->tableRows[i] > 0) {
            int rowSize = CalcRowSize(i, ctx->tableRows, ctx->heapSizes);
            offset += rowSize * ctx->tableRows[i];
        }
    }

    ctx->valid = true;
    return true;
}

const char* PEGetString(const PEMetadata* ctx, uint32_t index) {
    if (!ctx || !ctx->valid || !ctx->stringsHeap) return nullptr;
    if (index >= ctx->stringsHeapSize) return nullptr;
    return (const char*)(ctx->stringsHeap + index);
}

const uint8_t* PEGetBlob(const PEMetadata* ctx, uint32_t index) {
    if (!ctx || !ctx->valid || !ctx->blobHeap) return nullptr;
    if (index >= ctx->blobHeapSize) return nullptr;
    return ctx->blobHeap + index;
}

// Helper: read a value of given size from table data
static uint32_t ReadTableValue(const uint8_t* data, int size) {
    if (size == 1) return *data;
    if (size == 2) return *(uint16_t*)data;
    return *(uint32_t*)data;
}

// Forward decl for recursive use
static const uint8_t* GetTableRow(const PEMetadata* ctx, int tableId, int row);

// Find owning TypeDef name for a Method or Field row (0-based)
static std::string FindOwningTypeDef(const PEMetadata* ctx, int memberRow, bool isMethod) {
    if (!ctx || !ctx->valid) return "";

    int strSize = (ctx->heapSizes & 1) ? 4 : 2;
    int extendsSize = GetCodedIndexSize(ctx->tableRows, kTypeDefOrRef, 3);
    int fieldIdxSize = GetTableIndexSize(ctx->tableRows[0x04]);  // Field table
    int methodIdxSize = GetTableIndexSize(ctx->tableRows[0x06]); // Method table

    // Offset to FieldList column in TypeDef
    int fieldListOff = 4 + strSize + strSize + extendsSize;
    // Offset to MethodList column in TypeDef
    int methodListOff = fieldListOff + fieldIdxSize;

    uint32_t typeDefCount = ctx->tableRows[0x02];
    uint32_t memberCount = isMethod ? ctx->tableRows[0x06] : ctx->tableRows[0x04];

    for (uint32_t t = 0; t < typeDefCount; t++) {
        const uint8_t* trow = GetTableRow(ctx, 0x02, t);
        if (!trow) continue;

        // Get this type's member list start (1-based in metadata)
        uint32_t listStart = isMethod ? ReadTableValue(trow + methodListOff, methodIdxSize) : ReadTableValue(trow + fieldListOff, fieldIdxSize);

        // Get next type's member list start (or end of table)
        uint32_t listEnd = memberCount + 1; // 1-based end
        if (t + 1 < typeDefCount) {
            const uint8_t* nextRow = GetTableRow(ctx, 0x02, t + 1);
            if (nextRow) { listEnd = isMethod ? ReadTableValue(nextRow + methodListOff, methodIdxSize) : ReadTableValue(nextRow + fieldListOff, fieldIdxSize); }
        }

        // memberRow is 0-based, listStart/listEnd are 1-based
        if ((uint32_t)(memberRow + 1) >= listStart && (uint32_t)(memberRow + 1) < listEnd) {
            // Found owner - get type name
            uint32_t nameIdx = ReadTableValue(trow + 4, strSize);
            uint32_t nsIdx = ReadTableValue(trow + 4 + strSize, strSize);
            const char* name = PEGetString(ctx, nameIdx);
            const char* ns = PEGetString(ctx, nsIdx);
            std::string result;
            if (ns && ns[0]) {
                result = ns;
                result += '.';
            }
            if (name) result += name;
            return result;
        }
    }
    return "";
}

// Helper: get pointer to a row in a table
static const uint8_t* GetTableRow(const PEMetadata* ctx, int tableId, int row) {
    if (!ctx || !ctx->valid || tableId < 0 || tableId >= 64) return nullptr;
    if (row < 0 || (uint32_t)row >= ctx->tableRows[tableId]) return nullptr;

    int rowSize = CalcRowSize(tableId, ctx->tableRows, ctx->heapSizes);
    if (rowSize == 0) return nullptr;

    return ctx->tableData + ctx->tableOffsets[tableId] + row * rowSize;
}

// Resolve a metadata token to a semantic string for IL comparison across recompilations.
// Handles "token drift" where row indices shift when types/methods are added.
static thread_local int s_peResolveDepth = 0;

std::string PEResolveToken(const PEMetadata* ctx, uint32_t token) {
    if (!ctx || !ctx->valid) return "?";

    // Prevent infinite recursion
    if (s_peResolveDepth > 10) return "...";
    s_peResolveDepth++;
    struct DepthGuard {
        ~DepthGuard() { s_peResolveDepth--; }
    } guard;

    uint8_t tableId = (token >> 24) & 0xFF;
    int row = (int)(token & 0x00FFFFFF) - 1; // Convert to 0-based

    // Handle User String tokens (0x70), uses heap offset not row index
    if (tableId == 0x70) {
        if (!ctx->usHeap) return "str_noheap";
        uint32_t usOffset = token & 0x00FFFFFF;
        if (usOffset >= ctx->usHeapSize) return "str_oob";

        const uint8_t* p = ctx->usHeap + usOffset;
        const uint8_t* end = ctx->usHeap + ctx->usHeapSize;
        uint32_t len = 0;
        if (p >= end) return "str_empty";
        if ((*p & 0x80) == 0) {
            len = *p;
            p += 1;
        } else if ((*p & 0xC0) == 0x80) {
            if (p + 1 >= end) return "str_trunc";
            len = ((*p & 0x3F) << 8) | p[1];
            p += 2;
        } else {
            if (p + 3 >= end) return "str_trunc";
            len = ((*p & 0x1F) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
            p += 4;
        }
        if (len > 0) len--; // Remove terminal flag byte
        if (len == 0 || p + len > end) return "str_empty";

        std::string result = "str:";
        uint32_t charCount = len / 2;
        for (uint32_t i = 0; i < charCount && i < 256; i++) {
            uint16_t wc = ((const uint16_t*)p)[i];
            result += (wc >= 0x20 && wc < 0x7F) ? (char)wc : '?';
        }
        return result;
    }

    if (row < 0) return "tok_null";
    if (tableId >= 64 || (uint32_t)row >= ctx->tableRows[tableId]) return "tok_oob";

    int strSize = (ctx->heapSizes & 1) ? 4 : 2;
    int blobSize = (ctx->heapSizes & 4) ? 4 : 2;

    const uint8_t* rowData = GetTableRow(ctx, tableId, row);
    if (!rowData) return "tok_err";

    switch (tableId) {
    case 0x01: { // TypeRef: ResolutionScope(coded), TypeName(str), TypeNamespace(str)
        int scopeSize = GetCodedIndexSize(ctx->tableRows, kResolutionScope, 4);
        uint32_t nameIdx = ReadTableValue(rowData + scopeSize, strSize);
        uint32_t nsIdx = ReadTableValue(rowData + scopeSize + strSize, strSize);
        const char* name = PEGetString(ctx, nameIdx);
        const char* ns = PEGetString(ctx, nsIdx);
        std::string result;
        if (ns && ns[0]) {
            result = ns;
            result += '.';
        }
        if (name) result += name;
        return result.empty() ? "TypeRef?" : result;
    }
    case 0x02: { // TypeDef: Flags(4), TypeName(str), TypeNamespace(str), ...
        uint32_t nameIdx = ReadTableValue(rowData + 4, strSize);
        uint32_t nsIdx = ReadTableValue(rowData + 4 + strSize, strSize);
        const char* name = PEGetString(ctx, nameIdx);
        const char* ns = PEGetString(ctx, nsIdx);
        std::string result;
        if (ns && ns[0]) {
            result = ns;
            result += '.';
        }
        if (name) result += name;
        return result.empty() ? "TypeDef?" : result;
    }
    case 0x04: { // Field: Flags(2), Name(str), Signature(blob)
        uint32_t nameIdx = ReadTableValue(rowData + 2, strSize);
        const char* name = PEGetString(ctx, nameIdx);
        std::string owner = FindOwningTypeDef(ctx, row, false);
        std::string result = owner.empty() ? "" : (owner + "::");
        result += (name && name[0]) ? name : "Field?";
        return result;
    }
    case 0x06: { // MethodDef: RVA(4), ImplFlags(2), Flags(2), Name(str), ...
        uint32_t nameIdx = ReadTableValue(rowData + 8, strSize);
        const char* name = PEGetString(ctx, nameIdx);
        std::string owner = FindOwningTypeDef(ctx, row, true);
        std::string result = owner.empty() ? "" : (owner + "::");
        result += (name && name[0]) ? name : "Method?";
        return result;
    }
    case 0x0A: { // MemberRef: Class(coded), Name(str), Signature(blob)
        int classSize = GetCodedIndexSize(ctx->tableRows, kMemberRefParent, 5);
        uint32_t nameIdx = ReadTableValue(rowData + classSize, strSize);
        const char* name = PEGetString(ctx, nameIdx);
        return (name && name[0]) ? std::string(name) : "MemberRef?";
    }
    case 0x1B: { // TypeSpec: Signature(blob) - decode it
        if (!ctx->blobHeap) return "TypeSpec?";
        uint32_t blobIdx = ReadTableValue(rowData, blobSize);
        const uint8_t* blob = ctx->blobHeap + blobIdx;
        if (blobIdx >= ctx->blobHeapSize) return "TypeSpec_oob";

        // Decode compressed length
        const uint8_t* p = blob;
        uint32_t len = 0;
        if ((*p & 0x80) == 0) {
            len = *p++;
        } else if ((*p & 0xC0) == 0x80) {
            len = ((*p & 0x3F) << 8) | p[1];
            p += 2;
        } else {
            len = ((*p & 0x1F) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
            p += 4;
        }

        // Decode type signature for common cases
        if (len > 0) {
            const uint8_t* end = p + len;
            uint8_t etype = *p++;

            // DEBUG: if we hit fallback, include more info
            auto debugFallback = [len, etype, p, end]() -> std::string {
                char buf[64];
                snprintf(buf, sizeof(buf), "TypeSpec_e%d_len%d_rem%d", etype, (int)len, (int)(end - p));
                return buf;
            };

            // Helper lambda to decode compressed uint
            auto decodeCompressed = [&p, end]() -> uint32_t {
                if (p >= end) return 0;
                if ((*p & 0x80) == 0) return *p++;
                if ((*p & 0xC0) == 0x80) {
                    uint32_t v = ((*p & 0x3F) << 8) | p[1];
                    p += 2;
                    return v;
                }
                uint32_t v = ((*p & 0x1F) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
                p += 4;
                return v;
            };

            // Helper to resolve TypeDefOrRef coded index
            auto resolveTypeDefOrRef = [ctx](uint32_t codedIdx) -> std::string {
                uint32_t tag = codedIdx & 0x03;
                uint32_t idx = (codedIdx >> 2);
                if (idx == 0) return "?";
                uint32_t token = 0;
                if (tag == 0)
                    token = 0x02000000 | idx; // TypeDef
                else if (tag == 1)
                    token = 0x01000000 | idx; // TypeRef
                else if (tag == 2)
                    token = 0x1B000000 | idx; // TypeSpec
                return PEResolveToken(ctx, token);
            };

            if (etype == 0x1D && p < end) { // SZARRAY - zero-based single-dim array
                uint8_t elemType = *p++;
                if (elemType == 0x11 || elemType == 0x12) { // VALUETYPE or CLASS
                    uint32_t codedIdx = decodeCompressed();
                    return resolveTypeDefOrRef(codedIdx) + "[]";
                }
                // Primitive array
                static const char* primitives[] = {"?", "void", "bool", "char", "i1", "u1", "i2", "u2", "i4", "u4", "i8", "u8", "r4", "r8", "string"};
                if (elemType < 15) return std::string(primitives[elemType]) + "[]";
                return "arr_e" + std::to_string(elemType);
            }
            if (etype == 0x14 && p < end) { // ARRAY - general multi-dim array
                uint8_t elemType = *p++;
                std::string elemStr;
                if (elemType == 0x11 || elemType == 0x12) {
                    uint32_t codedIdx = decodeCompressed();
                    elemStr = resolveTypeDefOrRef(codedIdx);
                } else if (elemType < 15) {
                    static const char* primitives[] = {"?", "void", "bool", "char", "i1", "u1", "i2", "u2", "i4", "u4", "i8", "u8", "r4", "r8", "string"};
                    elemStr = primitives[elemType];
                } else {
                    elemStr = "e" + std::to_string(elemType);
                }
                uint32_t rank = decodeCompressed();
                return elemStr + "[" + std::to_string(rank) + "d]";
            }
            if (etype == 0x15) {             // GENERICINST
                uint8_t classOrValue = *p++; // 0x11 or 0x12
                (void)classOrValue;
                uint32_t codedIdx = decodeCompressed();
                return resolveTypeDefOrRef(codedIdx) + "<...>";
            }
            // Handle other common element types
            if (etype == 0x11 || etype == 0x12) { // VALUETYPE or CLASS directly
                uint32_t codedIdx = decodeCompressed();
                return resolveTypeDefOrRef(codedIdx);
            }
            if (etype == 0x13) { // VAR - generic type parameter
                uint32_t idx = decodeCompressed();
                return "!" + std::to_string(idx);
            }
            if (etype == 0x1E) { // MVAR - generic method parameter
                uint32_t idx = decodeCompressed();
                return "!!" + std::to_string(idx);
            }
            if (etype == 0x0F && p < end) { // PTR
                uint8_t inner = *p;
                if (inner < 15) {
                    static const char* prims[] = {"?", "void", "bool", "char", "i1", "u1", "i2", "u2", "i4", "u4", "i8", "u8", "r4", "r8", "string"};
                    return std::string(prims[inner]) + "*";
                }
                return "ptr";
            }
            if (etype == 0x10 && p < end) { // BYREF
                uint8_t inner = *p;
                if (inner < 15) {
                    static const char* prims[] = {"?", "void", "bool", "char", "i1", "u1", "i2", "u2", "i4", "u4", "i8", "u8", "r4", "r8", "string"};
                    return std::string(prims[inner]) + "&";
                }
                return "ref";
            }
            // Unhandled complex type, dump raw bytes for debugging
            if (end - p >= 1) {
                // Dump hex for pattern analysis
                char hexdump[64];
                int hpos = 0;
                hpos += snprintf(hexdump + hpos, sizeof(hexdump) - hpos, "e%02X:", etype);
                const uint8_t* dbg = p;
                for (int i = 0; i < 8 && dbg + i < end; i++) { hpos += snprintf(hexdump + hpos, sizeof(hexdump) - hpos, "%02X", dbg[i]); }
                return hexdump;
            }

            // Primitive types (shouldn't be in TypeSpec normally, but handle anyway)
            static const char* primNames[] = {"?", "void", "bool", "char", "i1", "u1", "i2", "u2", "i4", "u4", "i8", "u8", "r4", "r8", "string"};
            if (etype < 15) return primNames[etype];
            if (etype == 0x18) return "nint";
            if (etype == 0x19) return "nuint";
            if (etype == 0x1C) return "object";
            return debugFallback();
        }
        return "TypeSpec?";
    }
    case 0x2B: { // MethodSpec: Method(MethodDefOrRef coded), Instantiation(blob)
        int methodSize = GetCodedIndexSize(ctx->tableRows, kMethodDefOrRef, 2);
        uint32_t methodCoded = ReadTableValue(rowData, methodSize);

        // Decode MethodDefOrRef coded index: tag(1 bit) + row index
        uint32_t tag = methodCoded & 1;
        uint32_t idx = (methodCoded >> 1);
        if (idx > 0) {
            uint32_t methodToken = (tag == 0) ? (0x06000000 | idx) : (0x0A000000 | idx);
            return PEResolveToken(ctx, methodToken) + "<...>";
        }
        return "MethodSpec?";
    }
    // case 0x70 (User String) is handled before bounds check above
    default:
        return "tok_" + std::to_string(token);
    }
}

// S3SA resource entry from a DBPF package
struct S3SAEntry {
    uint64_t instanceId;
    uint32_t offset;
    uint32_t fileSize;
    uint32_t memSize;
};

// Enumerate all S3SA entries in a package, forward decl uses DBPFHeader from package_reader
static std::vector<S3SAEntry> EnumerateS3SAInPackage(const char* packagePath);
static int ExtractAllS3SAFromPackage(const char* packagePath, const std::filesystem::path& libsPath, int* outSkipped = nullptr);

int ExtractVanillaLibrariesFromGameDir() {
    LOG_INFO("[VanillaExtract] Proactive extraction from game directory...");

    // Derive game bin dir from exe path
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path binDir = std::filesystem::path(exePath).parent_path();

    LOG_INFO("[VanillaExtract] Game bin dir: " + binDir.string());

    std::filesystem::path libsPath = ModCompiler::Get().GetLibsPath();
    if (libsPath.empty()) {
        ModCompiler::Get().Initialize();
        libsPath = ModCompiler::Get().GetLibsPath();
    }
    if (libsPath.empty()) {
        LOG_ERROR("[VanillaExtract] No Libs path available");
        return 0;
    }

    std::error_code ec;
    std::filesystem::create_directories(libsPath, ec);

    // Known vanilla packages sitting next to the exe in Game/Bin/
    const char* packageNames[] = {"gameplay.package", "scripts.package", "simcore.package"};

    int totalExtracted = 0;
    int totalSkipped = 0;

    for (const char* pkgName : packageNames) {
        std::filesystem::path pkgPath = binDir / pkgName;
        if (!std::filesystem::exists(pkgPath)) {
            LOG_WARN("[VanillaExtract] Package not found: " + pkgPath.string());
            continue;
        }

        LOG_INFO("[VanillaExtract] Scanning: " + std::string(pkgName));
        int skipped = 0;
        int count = ExtractAllS3SAFromPackage(pkgPath.string().c_str(), libsPath, &skipped);
        totalExtracted += count;
        totalSkipped += skipped;
    }

    if (totalExtracted > 0) {
        LOG_INFO("[VanillaExtract]" + std::to_string(totalExtracted) + " new assemblies extracted");
    } else if (totalSkipped > 0) {
        LOG_INFO("[VanillaExtract] all" + std::to_string(totalSkipped) + " assemblies already in Libs/");
    } else {
        LOG_WARN("[VanillaExtract] no S3SA resources found in game packages");
    }

    return totalExtracted;
}

int ExtractVanillaLibraries() {
    LOG_INFO("[VanillaExtract] Starting proactive vanilla library extraction...");

    std::filesystem::path libsPath = ModCompiler::Get().GetLibsPath();
    if (libsPath.empty()) {
        ModCompiler::Get().Initialize();
        libsPath = ModCompiler::Get().GetLibsPath();
    }

    if (libsPath.empty()) {
        LOG_ERROR("[VanillaExtract] No Libs path available");
        return 0;
    }

    std::error_code ec;
    std::filesystem::create_directories(libsPath, ec);

    LOG_INFO("[VanillaExtract] Libs path: " + libsPath.string());

    std::vector<std::string> packages = GetDiscoveredPackages();

    if (packages.empty()) {
        LOG_WARN("[VanillaExtract] No vanilla packages discovered from assembly hooks");
        return 0;
    }

    LOG_INFO("[VanillaExtract] Found " + std::to_string(packages.size()) + " packages to scan");

    int totalExtracted = 0;
    int totalSkipped = 0;
    for (const auto& packagePath : packages) {
        int skipped = 0;
        int count = ExtractAllS3SAFromPackage(packagePath.c_str(), libsPath, &skipped);
        totalExtracted += count;
        totalSkipped += skipped;
    }

    if (totalExtracted > 0) {
        LOG_INFO("[VanillaExtract] Complete: " + std::to_string(totalExtracted) + " new assemblies extracted");
    } else if (totalSkipped > 0) {
        LOG_INFO("[VanillaExtract] Complete: " + std::to_string(totalSkipped) + " assemblies already exist in Libs/");
    } else {
        LOG_WARN("[VanillaExtract] No S3SA resources found in packages");
    }
    return totalExtracted;
}

// Enumerate S3SA entries
static std::vector<S3SAEntry> EnumerateS3SAInPackage(const char* packagePath) {
    std::vector<S3SAEntry> entries;

    HANDLE hFile = CreateFileA(packagePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return entries;

    DBPFHeader header;
    DWORD bytesRead;
    if (!ReadFile(hFile, &header, sizeof(header), &bytesRead, NULL) || bytesRead != sizeof(header)) {
        CloseHandle(hFile);
        return entries;
    }

    if (memcmp(header.magic, "DBPF", 4) != 0) {
        CloseHandle(hFile);
        return entries;
    }

    if (SetFilePointer(hFile, header.indexOffset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(hFile);
        return entries;
    }

    uint32_t indexFlags = 0;
    ReadFile(hFile, &indexFlags, 4, &bytesRead, NULL);

    uint32_t constType = 0, constGroup = 0, constInstHi = 0;
    if (indexFlags & 0x01) ReadFile(hFile, &constType, 4, &bytesRead, NULL);
    if (indexFlags & 0x02) ReadFile(hFile, &constGroup, 4, &bytesRead, NULL);
    if (indexFlags & 0x04) ReadFile(hFile, &constInstHi, 4, &bytesRead, NULL);

    for (uint32_t i = 0; i < header.indexEntryCount; i++) {
        uint32_t entryType = constType, entryGroup = constGroup, entryInstHi = constInstHi, entryInstLo;
        uint32_t entryOffset, entryFileSize, entryMemSize, entryFlags;

        if (!(indexFlags & 0x01)) ReadFile(hFile, &entryType, 4, &bytesRead, NULL);
        if (!(indexFlags & 0x02)) ReadFile(hFile, &entryGroup, 4, &bytesRead, NULL);
        if (!(indexFlags & 0x04)) ReadFile(hFile, &entryInstHi, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryInstLo, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryOffset, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryFileSize, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryMemSize, 4, &bytesRead, NULL);
        ReadFile(hFile, &entryFlags, 4, &bytesRead, NULL);

        if (entryType == RESOURCE_TYPE_S3SA) {
            S3SAEntry entry;
            entry.instanceId = ((uint64_t)entryInstHi << 32) | entryInstLo;
            entry.offset = entryOffset;
            entry.fileSize = entryFileSize;
            entry.memSize = entryMemSize;
            entries.push_back(entry);
        }
    }

    CloseHandle(hFile);
    return entries;
}

// Extract all S3SA from a package to Libs/, returns count of new assemblies (skips existing)
static int ExtractAllS3SAFromPackage(const char* packagePath, const std::filesystem::path& libsPath, int* outSkipped) {
    auto entries = EnumerateS3SAInPackage(packagePath);
    int extracted = 0;
    int skipped = 0;

    for (const auto& entry : entries) {
        uint32_t extractedSize = 0;
        uint8_t* extractedData = ExtractResourceFromPackage(packagePath, RESOURCE_TYPE_S3SA, entry.instanceId, &extractedSize);

        if (!extractedData || extractedSize == 0) continue;

        std::string assemblyName;
        const char* knownName = LookupAssemblyName(entry.instanceId);
        if (knownName) {
            assemblyName = knownName;
        } else {
            assemblyName = ReadAssemblyNameFromPE(extractedData, extractedSize);
            if (assemblyName.empty()) {
                char buf[32];
                snprintf(buf, sizeof(buf), "Unknown_%016llX", entry.instanceId);
                assemblyName = buf;
            }
        }

        std::filesystem::path outputPath = libsPath / (assemblyName + ".dll");
        if (std::filesystem::exists(outputPath)) {
            skipped++;
            delete[] extractedData;
            continue;
        }

        std::ofstream outFile(outputPath, std::ios::binary);
        if (outFile) {
            outFile.write(reinterpret_cast<char*>(extractedData), extractedSize);
            outFile.close();
            if (outFile.good()) {
                LOG_INFO("[VanillaExtract] Saved: " + assemblyName);
                extracted++;
            }
        }
        delete[] extractedData;
    }

    if (outSkipped) *outSkipped = skipped;
    return extracted;
}

} // namespace S3MM
