#include "il_parser.h"
#include "mono_bridge.h"
#include "addresses.h"
#include "vanilla_extractor.h"
#include "../util/logger.h"
#include <string>
#include <vector>
#include <Windows.h>
//https://www.youtube.com/watch?v=aESGzjXgYsQ
namespace S3MM {

// MONO METADATA FUNCTION POINTERS - Raw access for orphaned images without typeref resolution
using mono_metadata_string_heap_t = const char*(__cdecl*)(void* image, uint32_t index);
using mono_metadata_decode_row_col_t = uint32_t(__cdecl*)(void* tablePtr, int rowIndex, int colIndex);
using mono_metadata_blob_heap_t = const uint8_t*(__cdecl*)(void* image, uint32_t index);
using mono_image_rva_map_t = void*(__cdecl*)(void* image, uint32_t rva);

static mono_metadata_string_heap_t s_metadataStringHeap = nullptr;
static mono_metadata_decode_row_col_t s_metadataDecodeRowCol = nullptr;
static mono_metadata_blob_heap_t s_metadataBlobHeap = nullptr;
static mono_image_rva_map_t s_imageRvaMap = nullptr;
static bool s_metadataInitialized = false;
static CRITICAL_SECTION s_metadataCS;
static bool s_metadataCSInit = false;

static void EnsureMetadataFunctions() {
    if (s_metadataInitialized) return;

    if (!s_metadataCSInit) {
        InitializeCriticalSection(&s_metadataCS);
        s_metadataCSInit = true;
    }
    EnterCriticalSection(&s_metadataCS);
    if (!s_metadataInitialized) {
        s_metadataStringHeap = (mono_metadata_string_heap_t)Addresses::Metadata.mono_metadata_string_heap;
        s_metadataDecodeRowCol = (mono_metadata_decode_row_col_t)Addresses::Metadata.mono_metadata_decode_row_col;
        s_metadataBlobHeap = (mono_metadata_blob_heap_t)Addresses::Metadata.mono_metadata_blob_heap;
        s_imageRvaMap = (mono_image_rva_map_t)Addresses::Metadata.mono_image_rva_map;
        s_metadataInitialized = true;
    }
    LeaveCriticalSection(&s_metadataCS);
}

// METADATA TABLE ACCESS - MonoTableInfo is 12 bytes, tables array at image+0x64

void* GetTableInfo(void* image, int tableId) {
    return (void*)((uintptr_t)image + 0x64 + tableId * 12);
}

int GetTableRowCount(void* tableInfo) {
    uint32_t value = *(uint32_t*)((uintptr_t)tableInfo + 4);
    return (int)(value & 0x00FFFFFF);
}

uint32_t MetadataDecodeRowCol(void* tableInfo, int row, int col) {
    EnsureMetadataFunctions();
    return s_metadataDecodeRowCol(tableInfo, row, col);
}

const char* MetadataStringHeap(void* image, uint32_t index) {
    EnsureMetadataFunctions();
    return s_metadataStringHeap(image, index);
}

void* ImageRvaMap(void* image, uint32_t rva) {
    EnsureMetadataFunctions();
    return s_imageRvaMap(image, rva);
}

// IL OPCODE OPERAND TABLE - Maps opcodes to operand types for instruction walking

// Single-byte opcodes (0x00-0xFF)
static const uint8_t s_opcodeOperand[256] = {
    // 0x00-0x0F: nop, break, ldarg.0-3, ldloc.0-3, stloc.0-3, ldarg.s, ldarga.s, starg.s, ldloc.s
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // 00-07
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_I1,
    OP_I1, // 08-0F (ldarg.s, ldarga.s)
    // 0x10-0x1F: starg.s, ldloc.s, ldloca.s, stloc.s, ldnull, ldc.i4.m1, ldc.i4.0-8
    OP_I1,
    OP_I1,
    OP_I1,
    OP_I1,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // 10-17 (starg.s, ldloc.s, ldloca.s, stloc.s)
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_I1, // 18-1F (ldc.i4.0-7, ldc.i4.s)
    // 0x20-0x2F: ldc.i4, ldc.i8, ldc.r4, ldc.r8, unused, dup, pop, jmp, call, calli, ret, br.s...
    OP_I4,
    OP_I8,
    OP_R4,
    OP_R8,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_TOKEN, // 20-27 (ldc.i4/i8/r4/r8, dup, pop, jmp)
    OP_TOKEN,
    OP_TOKEN,
    OP_NONE,
    OP_I1,
    OP_I1,
    OP_I1,
    OP_I1,
    OP_I1, // 28-2F (call, calli, ret, br.s, brfalse.s, brtrue.s, beq.s, bge.s)
    // 0x30-0x3F: bgt.s, ble.s, blt.s, bne.un.s, bge.un.s, bgt.un.s, ble.un.s, blt.un.s, br, brfalse...
    OP_I1,
    OP_I1,
    OP_I1,
    OP_I1,
    OP_I1,
    OP_I1,
    OP_I1,
    OP_I1, // 30-37 (bgt.s thru blt.un.s)
    OP_I4,
    OP_I4,
    OP_I4,
    OP_I4,
    OP_I4,
    OP_I4,
    OP_I4,
    OP_I4, // 38-3F (br, brfalse, brtrue, beq, bge, bgt, ble, blt)
    // 0x40-0x4F: bne.un, bge.un, bgt.un, ble.un, blt.un, switch, ldind.i1-u4
    OP_I4,
    OP_I4,
    OP_I4,
    OP_I4,
    OP_I4,
    OP_SWITCH,
    OP_NONE,
    OP_NONE, // 40-47 (bne.un thru switch, ldind.i1, ldind.u1)
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // 48-4F (ldind.i2 thru stind.r4)
    // 0x50-0x5F: stind.r8, add-rem.un
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // 50-57
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // 58-5F
    // 0x60-0x6F: and-conv.u2, conv.u1, conv.i, conv.ovf.*, conv.ovf.un.*, callvirt
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // 60-67
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_TOKEN, // 68-6F (callvirt)
    // 0x70-0x7F: cpobj, ldobj, ldstr, newobj, castclass, isinst, conv.r.un, unused, unused, unbox
    OP_TOKEN,
    OP_TOKEN,
    OP_TOKEN,
    OP_TOKEN,
    OP_TOKEN,
    OP_TOKEN,
    OP_NONE,
    OP_NONE, // 70-77 (cpobj, ldobj, ldstr, newobj, castclass, isinst, conv.r.un)
    OP_NONE,
    OP_TOKEN,
    OP_NONE,
    OP_TOKEN,
    OP_TOKEN,
    OP_TOKEN,
    OP_TOKEN,
    OP_TOKEN, // 78-7F (unbox=79, ldfld=7B, ldflda=7C, stfld=7D, ldsfld=7E, ldsflda=7F)
    // 0x80-0x8F: stsfld, stobj, conv.ovf.*, box, newarr, ldlen, ldelema
    OP_TOKEN,
    OP_TOKEN,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // 80-87 (stsfld, stobj)
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_TOKEN,
    OP_TOKEN,
    OP_NONE,
    OP_TOKEN, // 88-8F (box=8C, newarr=8D, ldelema=8F)
    // 0x90-0x9F: ldelem.i1 thru ldelem.ref, stelem.i thru stelem.ref
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // 90-97
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // 98-9F
    // 0xA0-0xAF: stelem.ref, ldelem.any, stelem.any, unbox.any, ...
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_TOKEN,
    OP_TOKEN,
    OP_TOKEN,
    OP_NONE,
    OP_NONE, // A0-A7 (ldelem.any=A3, stelem.any=A4, unbox.any=A5)
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // A8-AF
    // 0xB0-0xBF: conv.ovf.* (all OP_NONE)
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // B0-B7
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // B8-BF
    // 0xC0-0xCF: refanyval, ckfinite, mkrefany, ..., ldtoken, conv.u2, conv.u1, conv.i, conv.ovf
    OP_NONE,
    OP_NONE,
    OP_TOKEN,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_TOKEN,
    OP_NONE, // C0-C7 (refanyval=C2, mkrefany=C6)
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // C8-CF
    // 0xD0-0xDF: ldtoken, conv.u2, conv.u1, conv.i, conv.ovf.i, conv.ovf.u, add.ovf...
    OP_TOKEN,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // D0-D7 (ldtoken=D0)
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_I4,
    OP_I1,
    OP_NONE, // D8-DF (leave=DD, leave.s=DE)
    // 0xE0-0xEF: conv.u, unused...
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // E0-E7
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // E8-EF
    // 0xF0-0xFF: reserved/prefix bytes (0xFE handled separately as two-byte prefix)
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // F0-F7
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE, // F8-FF
};

// 0xFE-prefixed opcodes (0xFE 0x00 - 0xFE 0x1F)
// FE 00=arglist, 01=ceq, 02=cgt, 03=cgt.un, 04=clt, 05=clt.un
// FE 06=ldftn(TOKEN), 07=ldvirtftn(TOKEN)
// FE 09=ldarg(I2), 0A=ldarga(I2), 0B=starg(I2)
// FE 0C=ldloc(I2), 0D=ldloca(I2), 0E=stloc(I2)
// FE 12=unaligned.(I1), 15=initobj(TOKEN), 16=constrained.(TOKEN)
// FE 19=no.(I1), 1C=sizeof(TOKEN)
static const uint8_t s_opcodeOperandFE[32] = {
    //  00       01       02       03       04       05       06       07
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_NONE,
    OP_TOKEN,
    OP_TOKEN,
    //  08       09       0A       0B       0C       0D       0E       0F
    OP_NONE,
    OP_I2,
    OP_I2,
    OP_I2,
    OP_I2,
    OP_I2,
    OP_I2,
    OP_NONE,
    //  10       11       12       13       14       15       16       17
    OP_NONE,
    OP_NONE,
    OP_I1,
    OP_NONE,
    OP_NONE,
    OP_TOKEN,
    OP_TOKEN,
    OP_NONE,
    //  18       19       1A       1B       1C       1D       1E       1F
    OP_NONE,
    OP_I1,
    OP_NONE,
    OP_NONE,
    OP_TOKEN,
    OP_NONE,
    OP_NONE,
    OP_NONE,
};

ILOperandType GetOpcodeOperandType(uint8_t opcode) {
    return (ILOperandType)s_opcodeOperand[opcode];
}

ILOperandType GetOpcodeOperandTypeFE(uint8_t opcode2) {
    return (opcode2 < 32) ? (ILOperandType)s_opcodeOperandFE[opcode2] : OP_NONE;
}

// BLOB SIGNATURE DECODING - Compressed uint and type signature parsing

uint32_t DecodeCompressedUInt(const uint8_t** pp, const uint8_t* end) {
    if (*pp >= end) return 0;
    uint8_t b0 = **pp;
    if ((b0 & 0x80) == 0) {
        (*pp) += 1;
        return b0;
    } else if ((b0 & 0xC0) == 0x80) {
        if (*pp + 1 >= end) return 0;
        uint32_t val = ((b0 & 0x3F) << 8) | (*pp)[1];
        (*pp) += 2;
        return val;
    } else if ((b0 & 0xE0) == 0xC0) {
        if (*pp + 3 >= end) return 0;
        uint32_t val = ((b0 & 0x1F) << 24) | ((*pp)[1] << 16) | ((*pp)[2] << 8) | (*pp)[3];
        (*pp) += 4;
        return val;
    }
    return 0;
}

std::string ResolveTypeDefOrRef(void* image, uint32_t codedIndex) {
    uint32_t tag = codedIndex & 0x03;
    uint32_t row = (codedIndex >> 2); // 1-based
    if (row == 0) return "?";
    row--; // Convert to 0-based

    int tableId = (tag == 0) ? MONO_TABLE_TYPEDEF : (tag == 1) ? MONO_TABLE_TYPEREF : (tag == 2) ? MONO_TABLE_TYPESPEC : -1;
    if (tableId < 0) return "?";

    void* tableInfo = GetTableInfo(image, tableId);
    int rowCount = GetTableRowCount(tableInfo);
    if ((int)row >= rowCount) return "?";

    if (tableId == MONO_TABLE_TYPEDEF || tableId == MONO_TABLE_TYPEREF) {
        // Both have: ..., TypeName(1), TypeNamespace(2)
        uint32_t nameIdx = s_metadataDecodeRowCol(tableInfo, row, 1);
        uint32_t nsIdx = s_metadataDecodeRowCol(tableInfo, row, 2);
        const char* name = s_metadataStringHeap(image, nameIdx);
        const char* ns = s_metadataStringHeap(image, nsIdx);
        std::string result;
        if (ns && ns[0]) {
            result = ns;
            result += '.';
        }
        if (name) result += name;
        return result.empty() ? "?" : result;
    }

    // TypeSpec referencing another TypeSpec - avoid infinite recursion, use generic fallback
    return "TypeSpec";
}

std::string DecodeTypeSig(void* image, const uint8_t** pp, const uint8_t* end, int depth) {
    if (depth > 8 || *pp >= end) return "?";

    uint8_t etype = **pp;
    (*pp)++;

    switch (etype) {
    case 0x01:
        return "void";
    case 0x02:
        return "bool";
    case 0x03:
        return "char";
    case 0x04:
        return "i1";
    case 0x05:
        return "u1";
    case 0x06:
        return "i2";
    case 0x07:
        return "u2";
    case 0x08:
        return "i4";
    case 0x09:
        return "u4";
    case 0x0A:
        return "i8";
    case 0x0B:
        return "u8";
    case 0x0C:
        return "r4";
    case 0x0D:
        return "r8";
    case 0x0E:
        return "string";
    case 0x16:
        return "typedbyref";
    case 0x18:
        return "IntPtr";
    case 0x19:
        return "UIntPtr";
    case 0x1C:
        return "object";

    case 0x0F: // PTR <type>
        return "ptr<" + DecodeTypeSig(image, pp, end, depth + 1) + ">";
    case 0x10: // BYREF <type>
        return "ref<" + DecodeTypeSig(image, pp, end, depth + 1) + ">";

    case 0x11:   // VALUETYPE <TypeDefOrRef>
    case 0x12: { // CLASS <TypeDefOrRef>
        uint32_t codedIdx = DecodeCompressedUInt(pp, end);
        return ResolveTypeDefOrRef(image, codedIdx);
    }

    case 0x13: { // VAR <number> (generic type parameter)
        uint32_t num = DecodeCompressedUInt(pp, end);
        return "!" + std::to_string(num);
    }
    case 0x1E: { // MVAR <number> (generic method parameter)
        uint32_t num = DecodeCompressedUInt(pp, end);
        return "!!" + std::to_string(num);
    }

    case 0x14: { // ARRAY <type> <rank> <boundsCount> ... <loCount> ...
        std::string elemType = DecodeTypeSig(image, pp, end, depth + 1);
        uint32_t rank = DecodeCompressedUInt(pp, end);
        uint32_t boundsCount = DecodeCompressedUInt(pp, end);
        for (uint32_t i = 0; i < boundsCount; i++) DecodeCompressedUInt(pp, end);
        uint32_t loCount = DecodeCompressedUInt(pp, end);
        for (uint32_t i = 0; i < loCount; i++) DecodeCompressedUInt(pp, end);
        return elemType + "[" + std::to_string(rank) + "]";
    }

    case 0x15: { // GENERICINST <class|valuetype> <TypeDefOrRef> <argCount> <args...>
        uint8_t classOrValue = **pp;
        (*pp)++; // 0x11 or 0x12
        (void)classOrValue;
        uint32_t codedIdx = DecodeCompressedUInt(pp, end);
        std::string baseName = ResolveTypeDefOrRef(image, codedIdx);
        uint32_t argCount = DecodeCompressedUInt(pp, end);
        baseName += "<";
        for (uint32_t i = 0; i < argCount && i < 16; i++) {
            if (i > 0) baseName += ",";
            baseName += DecodeTypeSig(image, pp, end, depth + 1);
        }
        baseName += ">";
        return baseName;
    }

    case 0x1D: { // SZARRAY <type>
        return DecodeTypeSig(image, pp, end, depth + 1) + "[]";
    }

    default:
        return "etype_" + std::to_string(etype);
    }
}

// METADATA TOKEN RESOLUTION - Resolve tokens to deterministic semantic strings

// Find owning TypeDef name for a Method or Field row (0-based)
static std::string FindOwningTypeDefMono(void* image, int memberRow, bool isMethod) {
    void* typeDefTable = GetTableInfo(image, 0x02); // TypeDef
    int typeDefCount = GetTableRowCount(typeDefTable);
    if (typeDefCount <= 0) return "";

    // Get total member count for end boundary
    void* memberTable = GetTableInfo(image, isMethod ? 0x06 : 0x04);
    int memberCount = GetTableRowCount(memberTable);

    for (int t = 0; t < typeDefCount; t++) {
        // Column 5 = MethodList, Column 4 = FieldList (1-based row indices)
        uint32_t listStart = s_metadataDecodeRowCol(typeDefTable, t, isMethod ? 5 : 4);

        // Get next type's list start (or end of table)
        uint32_t listEnd = (uint32_t)(memberCount + 1); // 1-based end
        if (t + 1 < typeDefCount) { listEnd = s_metadataDecodeRowCol(typeDefTable, t + 1, isMethod ? 5 : 4); }

        // memberRow is 0-based, listStart/listEnd are 1-based
        if ((uint32_t)(memberRow + 1) >= listStart && (uint32_t)(memberRow + 1) < listEnd) {
            // Found owner - get type name
            uint32_t nameIdx = s_metadataDecodeRowCol(typeDefTable, t, 1);
            uint32_t nsIdx = s_metadataDecodeRowCol(typeDefTable, t, 2);
            const char* name = s_metadataStringHeap(image, nameIdx);
            const char* ns = s_metadataStringHeap(image, nsIdx);
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

// Resolve a metadata token to a semantic string via Mono's API, must match PEResolveToken output
static thread_local int s_monoResolveDepth = 0;

std::string ResolveMetadataToken(void* image, uint32_t token) {
    EnsureMetadataFunctions();

    // Prevent infinite recursion
    if (s_monoResolveDepth > 10) return "...";
    s_monoResolveDepth++;
    struct DepthGuard {
        ~DepthGuard() { s_monoResolveDepth--; }
    } guard;

    uint8_t tableId = (token >> 24) & 0xFF;
    int row = (int)(token & 0x00FFFFFF) - 1; // Convert to 0-based

    // Handle User String tokens (0x70) before table bounds check
    // String tokens use the low 24 bits as heap offset, not row index
    if (tableId == 0x70) {
        uint32_t usOffset = token & 0x00FFFFFF;
        // US heap: image+0x40 = data ptr, image+0x44 = size
        const uint8_t* usData = *(const uint8_t**)((uintptr_t)image + 0x40);
        uint32_t usSize = *(uint32_t*)((uintptr_t)image + 0x44);
        if (!usData || usOffset >= usSize) return "str_oob";

        // Decode compressed length prefix (ECMA-335 II.24.2.4)
        const uint8_t* p = usData + usOffset;
        const uint8_t* end = usData + usSize;
        uint32_t strByteLen = 0;
        if (p >= end) return "str_empty";
        if ((*p & 0x80) == 0) {
            strByteLen = *p;
            p += 1;
        } else if ((*p & 0xC0) == 0x80) {
            if (p + 1 >= end) return "str_trunc";
            strByteLen = ((*p & 0x3F) << 8) | p[1];
            p += 2;
        } else {
            if (p + 3 >= end) return "str_trunc";
            strByteLen = ((*p & 0x1F) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
            p += 4;
        }
        // strByteLen includes terminal flag byte; actual UTF-16 data = strByteLen - 1
        if (strByteLen > 0) strByteLen--;
        if (strByteLen == 0 || p + strByteLen > end) return "str_empty";

        // Convert UTF-16LE to ASCII for readable hashing
        std::string result = "str:";
        uint32_t charCount = strByteLen / 2;
        for (uint32_t i = 0; i < charCount && i < 256; i++) {
            uint16_t wc = ((const uint16_t*)p)[i];
            if (wc >= 0x20 && wc < 0x7F)
                result += (char)wc;
            else
                result += '?';
        }
        return result;
    }

    if (row < 0) return "tok_null";

    void* tableInfo = GetTableInfo(image, tableId);
    int rowCount = GetTableRowCount(tableInfo);
    if (row >= rowCount) return "tok_oob";

    switch (tableId) {
    case 0x01: { // TypeRef: ResolutionScope(0), TypeName(1), TypeNamespace(2)
        uint32_t nameIdx = s_metadataDecodeRowCol(tableInfo, row, 1);
        uint32_t nsIdx = s_metadataDecodeRowCol(tableInfo, row, 2);
        const char* name = s_metadataStringHeap(image, nameIdx);
        const char* ns = s_metadataStringHeap(image, nsIdx);
        std::string result;
        if (ns && ns[0]) {
            result = ns;
            result += '.';
        }
        if (name) result += name;
        return result.empty() ? "TypeRef_" + std::to_string(row) : result;
    }
    case 0x02: { // TypeDef: Flags(0), TypeName(1), TypeNamespace(2), Extends(3), FieldList(4), MethodList(5)
        uint32_t nameIdx = s_metadataDecodeRowCol(tableInfo, row, 1);
        uint32_t nsIdx = s_metadataDecodeRowCol(tableInfo, row, 2);
        const char* name = s_metadataStringHeap(image, nameIdx);
        const char* ns = s_metadataStringHeap(image, nsIdx);
        std::string result;
        if (ns && ns[0]) {
            result = ns;
            result += '.';
        }
        if (name) result += name;
        return result.empty() ? "TypeDef_" + std::to_string(row) : result;
    }
    case 0x04: { // Field: Flags(0), Name(1), Signature(2)
        uint32_t nameIdx = s_metadataDecodeRowCol(tableInfo, row, 1);
        const char* name = s_metadataStringHeap(image, nameIdx);
        std::string owner = FindOwningTypeDefMono(image, row, false);
        std::string result = owner.empty() ? "" : (owner + "::");
        result += (name && name[0]) ? name : ("Field_" + std::to_string(row));
        return result;
    }
    case 0x06: { // MethodDef: RVA(0), ImplFlags(1), Flags(2), Name(3), Signature(4), ParamList(5)
        uint32_t nameIdx = s_metadataDecodeRowCol(tableInfo, row, 3);
        const char* name = s_metadataStringHeap(image, nameIdx);
        std::string owner = FindOwningTypeDefMono(image, row, true);
        std::string result = owner.empty() ? "" : (owner + "::");
        result += (name && name[0]) ? name : ("Method_" + std::to_string(row));
        return result;
    }
    case 0x0A: { // MemberRef: Class(0), Name(1), Signature(2)
        uint32_t nameIdx = s_metadataDecodeRowCol(tableInfo, row, 1);
        const char* name = s_metadataStringHeap(image, nameIdx);
        return (name && name[0]) ? std::string(name) : "MemberRef_" + std::to_string(row);
    }
    case 0x1B: { // TypeSpec - decode type signature
        void* tsTableInfo = GetTableInfo(image, MONO_TABLE_TYPESPEC);
        uint32_t blobIdx = s_metadataDecodeRowCol(tsTableInfo, row, 0);
        const uint8_t* blob = s_metadataBlobHeap(image, blobIdx);
        if (!blob) return "TypeSpec?";

        // Decode length prefix
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

        if (len > 0) {
            const uint8_t* end = p + len;
            uint8_t etype = *p++;

            if (etype == 0x1D && p < end) { // SZARRAY
                uint8_t elemType = *p++;
                if (elemType == 0x11 || elemType == 0x12) { // VALUETYPE or CLASS
                    uint32_t codedIdx = DecodeCompressedUInt(&p, end);
                    return ResolveTypeDefOrRef(image, codedIdx) + "[]";
                }
                static const char* primitives[] = {"?", "void", "bool", "char", "i1", "u1", "i2", "u2", "i4", "u4", "i8", "u8", "r4", "r8", "string"};
                if (elemType < 15) return std::string(primitives[elemType]) + "[]";
                return "arr_e" + std::to_string(elemType);
            }
            if (etype == 0x14 && p < end) { // ARRAY - multi-dim
                uint8_t elemType = *p++;
                std::string elemStr;
                if (elemType == 0x11 || elemType == 0x12) {
                    uint32_t codedIdx = DecodeCompressedUInt(&p, end);
                    elemStr = ResolveTypeDefOrRef(image, codedIdx);
                } else if (elemType < 15) {
                    static const char* primitives[] = {"?", "void", "bool", "char", "i1", "u1", "i2", "u2", "i4", "u4", "i8", "u8", "r4", "r8", "string"};
                    elemStr = primitives[elemType];
                } else {
                    elemStr = "e" + std::to_string(elemType);
                }
                uint32_t rank = DecodeCompressedUInt(&p, end);
                return elemStr + "[" + std::to_string(rank) + "d]";
            }
            if (etype == 0x13) { // VAR - generic type parameter
                uint32_t idx = DecodeCompressedUInt(&p, end);
                return "!" + std::to_string(idx);
            }
            if (etype == 0x1E) { // MVAR - generic method parameter
                uint32_t idx = DecodeCompressedUInt(&p, end);
                return "!!" + std::to_string(idx);
            }
            if (etype == 0x15) { // GENERICINST
                p++;             // Skip class/valuetype marker
                uint32_t codedIdx = DecodeCompressedUInt(&p, end);
                return ResolveTypeDefOrRef(image, codedIdx) + "<...>";
            }
            return "TypeSpec_e" + std::to_string(etype);
        }
        return "TypeSpec?";
    }
    case 0x2B: { // MethodSpec - simplified to match PE version
        void* tableInfo = GetTableInfo(image, MONO_TABLE_METHODSPEC);
        uint32_t methodCoded = s_metadataDecodeRowCol(tableInfo, row, 0);

        uint32_t tag = methodCoded & 1;
        uint32_t methodRow = (methodCoded >> 1) - 1;

        std::string methodName;
        if (tag == 0) { // MethodDef
            void* methodTable = GetTableInfo(image, MONO_TABLE_METHOD);
            if ((int)methodRow < GetTableRowCount(methodTable)) {
                uint32_t nameIdx = s_metadataDecodeRowCol(methodTable, methodRow, 3);
                const char* name = s_metadataStringHeap(image, nameIdx);
                methodName = (name && name[0]) ? name : "?";
            }
        } else { // MemberRef
            void* memberTable = GetTableInfo(image, MONO_TABLE_MEMBERREF);
            if ((int)methodRow < GetTableRowCount(memberTable)) {
                uint32_t nameIdx = s_metadataDecodeRowCol(memberTable, methodRow, 1);
                const char* name = s_metadataStringHeap(image, nameIdx);
                methodName = (name && name[0]) ? name : "?";
            }
        }
        return methodName + "<...>";
    }
    // case 0x70 (User String) handled before bounds check above
    default:
        return "tok_" + std::to_string(token);
    }
}

// NORMALIZED IL HASH - FNV-1a hash with token resolution for semantic comparison
uint32_t HashILNormalized(void* image, const uint8_t* code, uint32_t codeSize) {
    if (!code || codeSize == 0 || !image) return 0;
    EnsureMetadataFunctions();

    uint32_t hash = 0x811c9dc5; // FNV-1a offset basis

    // Helper: hash a single byte into FNV-1a
    auto hashByte = [&hash](uint8_t b) {
        hash ^= b;
        hash *= 0x01000193;
    };

    // Helper: hash a string into FNV-1a
    auto hashString = [&hash](const std::string& s) {
        for (char c : s) {
            hash ^= (uint8_t)c;
            hash *= 0x01000193;
        }
        // Hash a separator to avoid collisions between adjacent resolved names
        hash ^= 0xFF;
        hash *= 0x01000193;
    };

    uint32_t pos = 0;
    while (pos < codeSize) {
        uint8_t op = code[pos++];

        // Two-byte prefix
        if (op == 0xFE) {
            if (pos >= codeSize) break; // Truncated
            uint8_t op2 = code[pos++];
            hashByte(0xFE);
            hashByte(op2);

            ILOperandType opType = (op2 < 32) ? (ILOperandType)s_opcodeOperandFE[op2] : OP_NONE;

            if (opType == OP_TOKEN && pos + 4 <= codeSize) {
                uint32_t token = *(uint32_t*)(code + pos);
                std::string resolved = ResolveMetadataToken(image, token);
                hashString(resolved);
                pos += 4;
            } else {
                uint32_t operandSize = GetOperandSize(opType);
                for (uint32_t i = 0; i < operandSize && pos < codeSize; i++) hashByte(code[pos++]);
            }
            continue;
        }

        // Single-byte opcode
        hashByte(op);
        ILOperandType opType = (ILOperandType)s_opcodeOperand[op];

        if (opType == OP_TOKEN && pos + 4 <= codeSize) {
            uint32_t token = *(uint32_t*)(code + pos);
            std::string resolved = ResolveMetadataToken(image, token);
            hashString(resolved);
            pos += 4;
        } else if (opType == OP_SWITCH && pos + 4 <= codeSize) {
            // switch: 4-byte count + 4*count branch targets
            uint32_t count = *(uint32_t*)(code + pos);
            for (uint32_t i = 0; i < 4 && pos < codeSize; i++) hashByte(code[pos++]);
            uint32_t targetBytes = count * 4;
            for (uint32_t i = 0; i < targetBytes && pos < codeSize; i++) hashByte(code[pos++]);
        } else {
            uint32_t operandSize = GetOperandSize(opType);
            for (uint32_t i = 0; i < operandSize && pos < codeSize; i++) hashByte(code[pos++]);
        }
    }

    return hash;
}

// PE-BASED NORMALIZED IL HASH - Same as above but uses raw PE metadata, no Mono dependency
uint32_t HashILNormalizedPE(const PEMetadata* ctx, const uint8_t* code, uint32_t codeSize) {
    if (!code || codeSize == 0 || !ctx || !ctx->valid) return 0;

    uint32_t hash = 0x811c9dc5; // FNV-1a offset basis

    auto hashByte = [&hash](uint8_t b) {
        hash ^= b;
        hash *= 0x01000193;
    };

    auto hashString = [&hash](const std::string& s) {
        for (char c : s) {
            hash ^= (uint8_t)c;
            hash *= 0x01000193;
        }
        hash ^= 0xFF;
        hash *= 0x01000193;
    };

    uint32_t pos = 0;
    while (pos < codeSize) {
        uint8_t op = code[pos++];

        // Two-byte prefix
        if (op == 0xFE) {
            if (pos >= codeSize) break;
            uint8_t op2 = code[pos++];
            hashByte(0xFE);
            hashByte(op2);

            ILOperandType opType = GetOpcodeOperandTypeFE(op2);

            if (opType == OP_TOKEN && pos + 4 <= codeSize) {
                uint32_t token = *(uint32_t*)(code + pos);
                std::string resolved = PEResolveToken(ctx, token);
                hashString(resolved);
                pos += 4;
            } else {
                uint32_t operandSize = GetOperandSize(opType);
                for (uint32_t i = 0; i < operandSize && pos < codeSize; i++) hashByte(code[pos++]);
            }
            continue;
        }

        // Single-byte opcode
        hashByte(op);
        ILOperandType opType = GetOpcodeOperandType(op);

        if (opType == OP_TOKEN && pos + 4 <= codeSize) {
            uint32_t token = *(uint32_t*)(code + pos);
            std::string resolved = PEResolveToken(ctx, token);
            hashString(resolved);
            pos += 4;
        } else if (opType == OP_SWITCH && pos + 4 <= codeSize) {
            uint32_t count = *(uint32_t*)(code + pos);
            for (uint32_t i = 0; i < 4 && pos < codeSize; i++) hashByte(code[pos++]);
            uint32_t targetBytes = count * 4;
            for (uint32_t i = 0; i < targetBytes && pos < codeSize; i++) hashByte(code[pos++]);
        } else {
            uint32_t operandSize = GetOperandSize(opType);
            for (uint32_t i = 0; i < operandSize && pos < codeSize; i++) hashByte(code[pos++]);
        }
    }

    return hash;
}

} // namespace S3MM
