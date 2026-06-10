#pragma once
#include <cstdint>
#include <string>

namespace S3MM {

// DBPF PACKAGE STRUCTURES - Header and index entry layouts for Sims 3 .package files
#pragma pack(push, 1)
struct DBPFHeader {
    char magic[4];         // "DBPF"
    uint32_t majorVersion; // Usually 2
    uint32_t minorVersion; // Usually 0
    uint32_t reserved1[3];
    uint32_t dateCreated;
    uint32_t dateModified;
    uint32_t indexMajorVersion; // Usually 3
    uint32_t indexEntryCount;
    uint32_t indexFirstOffset; // Unused in v2
    uint32_t indexSize;
    uint32_t reserved2[3];
    uint32_t indexVersion; // Usually 3
    uint32_t indexOffset;  // Where index table starts
    uint32_t reserved3[7]; // Padding to 96 bytes
};
static_assert(sizeof(DBPFHeader) == 96, "DBPFHeader must be 96 bytes");

struct DBPFIndexEntry {
    uint32_t type;
    uint32_t group;
    uint32_t instanceHi;
    uint32_t instanceLo;
    uint32_t offset;
    uint32_t fileSize;   // Compressed size in file (with flags in high bits)
    uint32_t memSize;    // Decompressed size
    uint16_t compressed; // Compression type (0xFFFF = compressed, 0 = not)
    uint16_t unknown;
};
static_assert(sizeof(DBPFIndexEntry) == 32, "DBPFIndexEntry must be 32 bytes");
#pragma pack(pop)

// Resource type constants
constexpr uint32_t RESOURCE_TYPE_S3SA = 0x073FAA07; // Script assembly

// PACKAGE EXTRACTION API - Extract and decrypt resources from .package files

// Extract a resource from a .package file by type and instance ID.
// Returns allocated buffer (caller must free with delete[]) and sets outSize, or nullptr on failure.
uint8_t* ExtractResourceFromPackage(const char* packagePath, uint32_t type, uint64_t instance, uint32_t* outSize);

// Decrypt S3SA (Sims 3 Script Assembly) data; algorithm ported from s3pi by Peter L Jones (GPL v3).
// Returns output size or 0 on failure. Caller must delete[] the output buffer.
uint32_t DecryptS3SA(const uint8_t* input, uint32_t inputSize, uint8_t** output);

// Read assembly name from PE/CLI metadata without loading via Mono; returns empty string on failure.
std::string ReadAssemblyNameFromPE(const uint8_t* data, uint32_t size);

} // namespace S3MM
