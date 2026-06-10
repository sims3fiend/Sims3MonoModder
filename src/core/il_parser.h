#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace S3MM {

// IL OPERAND TYPES - Encoding categories for IL instruction operands
enum ILOperandType : uint8_t {
    OP_NONE = 0,   // No operand
    OP_I1 = 1,     // 1-byte operand (short branch, ldarg.s, etc.)
    OP_I2 = 2,     // 2-byte operand (ldarg, ldloc, stloc, starg)
    OP_I4 = 3,     // 4-byte operand (ldc.i4, branch target)
    OP_I8 = 4,     // 8-byte operand (ldc.i8)
    OP_R4 = 5,     // 4-byte float (ldc.r4)
    OP_R8 = 6,     // 8-byte float (ldc.r8)
    OP_TOKEN = 7,  // 4-byte metadata token (needs resolution)
    OP_SWITCH = 8, // Variable: 4 + 4*N bytes
};

// METADATA TABLE CONSTANTS - ECMA-335 table IDs
constexpr int MONO_TABLE_TYPEREF = 1;
constexpr int MONO_TABLE_TYPEDEF = 2;
constexpr int MONO_TABLE_FIELD = 4;
constexpr int MONO_TABLE_METHOD = 6;
constexpr int MONO_TABLE_MEMBERREF = 10;
constexpr int MONO_TABLE_TYPESPEC = 27;
constexpr int MONO_TABLE_METHODSPEC = 43;

// METADATA TABLE ACCESS - Read rows, columns, and strings from Mono metadata
// Get table info pointer from MonoImage: image + 0x64 + table_id * 12
void* GetTableInfo(void* image, int tableId);

// Get row count from table info (bits 0-23 of dword at offset 4)
int GetTableRowCount(void* tableInfo);

// Read a column value from a metadata table row
uint32_t MetadataDecodeRowCol(void* tableInfo, int row, int col);

// Get string from #Strings heap
const char* MetadataStringHeap(void* image, uint32_t index);

// Map RVA to pointer within image
void* ImageRvaMap(void* image, uint32_t rva);

// METADATA TOKEN RESOLUTION - Convert IL tokens to symbolic names
// Resolve IL metadata token to symbolic name for normalized hashing.
// Token format: high byte = table ID, low 3 bytes = 1-based row index.
std::string ResolveMetadataToken(void* image, uint32_t token);

// IL BYTECODE HASHING - Produce token-independent hashes of IL method bodies
// Walk IL opcodes, resolve metadata tokens to symbolic names, then hash the normalized representation. Should produce consistent hashes across assemblies where tokens differ but logic is identical.
uint32_t HashILNormalized(void* image, const uint8_t* code, uint32_t codeSize);

// Forward declare PEMetadata from vanilla_extractor.h
struct PEMetadata;

// PE-based version: resolve tokens from raw PE bytes (deterministic, no Mono dependency)
uint32_t HashILNormalizedPE(const PEMetadata* ctx, const uint8_t* code, uint32_t codeSize);

// Get fixed operand size in bytes for an ILOperandType (0 for NONE/SWITCH)
inline uint32_t GetOperandSize(ILOperandType opType) {
    switch (opType) {
    case OP_I1:
        return 1;
    case OP_I2:
        return 2;
    case OP_I4:
    case OP_R4:
    case OP_TOKEN:
        return 4;
    case OP_I8:
    case OP_R8:
        return 8;
    default:
        return 0;
    }
}

// Get operand type for a single-byte opcode
ILOperandType GetOpcodeOperandType(uint8_t opcode);

// Get operand type for a 0xFE-prefixed opcode
ILOperandType GetOpcodeOperandTypeFE(uint8_t opcode2);

// BLOB SIGNATURE DECODING - Parse ECMA-335 compressed signatures
// Decode compressed unsigned int from blob data. Advances *pp past the decoded value. Returns 0 on error.
uint32_t DecodeCompressedUInt(const uint8_t** pp, const uint8_t* end);

// Decode a type signature from blob data into a deterministic string.
std::string DecodeTypeSig(void* image, const uint8_t** pp, const uint8_t* end, int depth = 0);

// Resolve a TypeDefOrRef coded index to a name string.
std::string ResolveTypeDefOrRef(void* image, uint32_t codedIndex);

} // namespace S3MM
