#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace S3MM {

// RESOURCE KEY - EA::ResourceMan::Key layout confirmed from disassembly (push sequence at 0x004AF09C)
struct ResourceKey {
    uint32_t instance_lo; // +0x00
    uint32_t instance_hi; // +0x04
    uint32_t type;        // +0x08
    uint32_t group;       // +0x0C

    uint64_t GetInstance() const { return ((uint64_t)instance_hi << 32) | instance_lo; }
    void SetInstance(uint64_t inst) {
        instance_lo = (uint32_t)inst;
        instance_hi = (uint32_t)(inst >> 32);
    }

    // Format as "0xGROUP!0xINST_HI+LO.0xTYPE"
    std::string Format() const;

    bool operator==(const ResourceKey& o) const { return instance_lo == o.instance_lo && instance_hi == o.instance_hi && type == o.type && group == o.group; }
};

// DATABASE TYPE IDS - From DDF vtable[3]
constexpr uint32_t DB_TYPE_DIRECTORY_FILES = 0x34728492;
constexpr uint32_t DB_TYPE_PACKED_FILE = 0x2A43BD44;
constexpr uint32_t DB_TYPE_PACKED_STREAM = 0x03C4B9A4; // DatabasePackedStream, main game data stores (FullBuild, thumbnails)

// DATABASE INFO - One entry in the Manager's DBArray
struct DatabaseInfo {
    void* ptr;            // Database pointer (opaque)
    int priority;         // Priority in DBArray (higher = checked first)
    std::string path;     // From Database->vtable[0x0A] (GetPath)
    std::string filename; // Just the filename portion
    uint32_t typeId;      // vtable[3]() return - DB_TYPE_DIRECTORY_FILES or DB_TYPE_PACKED_FILE
    std::string typeName; // "DirectoryFiles" or "PackedFile" or "Unknown"
};

// MANAGER VTABLE OFFSETS - PC, confirmed
namespace ManagerVtable {
constexpr uint32_t RegisterDatabase = 0x34;     // [13] bool(bool add, Database*, int priority)
constexpr uint32_t IsDatabaseRegistered = 0x38; // [14] bool(Database*)
constexpr uint32_t GetPackageSource = 0x40;     // [16] Database*(Key*, int*)
constexpr uint32_t FindDatabase = 0x44;         // [17] Database*(Key*)
constexpr uint32_t FindCache = 0x58;            // [22] Cache*(Key*)
constexpr uint32_t CacheResource = 0x5C;        // [23] void(Resource*, bool)
constexpr uint32_t FlushCache = 0x60;           // [24] void(Cache*, KeyFilter*)
constexpr uint32_t GetTypenameFromType = 0x94;  // [37] wchar_t*(uint typeId)
} // namespace ManagerVtable

// DATABASE VTABLE OFFSETS - Shared base class
namespace DatabaseVtable {
constexpr uint32_t GetTypeId = 0x0C;   // [3]  uint32_t() - returns type magic
constexpr uint32_t GetPath = 0x28;     // [10] wchar_t*()
constexpr uint32_t HasResource = 0x34; // [13] bool(Key*, int, int, int, int, int)
} // namespace DatabaseVtable

// PACKAGE KEY INFO - DBPF index entry
struct PackageKeyInfo {
    ResourceKey key;
    uint32_t offset;   // File offset in package
    uint32_t fileSize; // Compressed size (lower 31 bits of raw field)
    uint32_t memSize;  // Decompressed size
    bool compressed;   // High bit of raw fileSize field
};

// RESOURCE SYSTEM API - Public interface for database and package operations
namespace ResourceSystem {

// Get the Manager singleton, wraps Addresses::ScriptHost.GetResourceManager
void* GetManager();

// Enumerate all registered databases from the Manager's DBArray, sorted by priority descending
std::vector<DatabaseInfo> GetRegisteredDatabases();

// Get a human-readable type name for a resource type ID (e.g. 0x0333406C -> "XML Tuning"), falls back to built-in table
std::string GetTypeName(uint32_t typeId);

// Check if a specific database has a given resource key
bool DatabaseHasResource(void* database, const ResourceKey& key);

// Find which database provides a resource (highest priority match), returns empty info if none
DatabaseInfo FindWinningDatabase(const ResourceKey& key);

// Enumerate all TGIs in a .package file (DBPF v2), works for PackedFile and PackedStream databases
std::vector<PackageKeyInfo> EnumeratePackageKeys(const std::string& path);

// Enumerate DDF loose files, parsing filenames back to TGIs (format: 0xGROUP!0xINSTHILO.0xTYPE)
std::vector<ResourceKey> EnumerateDDFKeys(const std::string& directoryPath);

// Get file extension for a resource type ID (e.g. 0x0333406C -> ".xml"), falls back to ".dat"
std::string GetTypeExtension(uint32_t typeId);

// Parse the Name Map (type 0x0166038C) from a .package file, returns instance ID -> name mapping
std::unordered_map<uint64_t, std::string> ParseNameMap(const std::string& packagePath);

// Extract raw resource data from a .package file, handles Refpack decompression. Returns empty on failure.
std::vector<uint8_t> ExtractResourceData(const std::string& packagePath, const PackageKeyInfo& entry);

// Extract a resource to a file on disk, creates directories as needed. Returns true on success.
bool ExtractResourceToFile(const std::string& packagePath, const PackageKeyInfo& entry, const std::string& outputPath);

} // namespace ResourceSystem

} // namespace S3MM
