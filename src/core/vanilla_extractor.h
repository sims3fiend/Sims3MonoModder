#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace S3MM {

// VANILLA ASSEMBLY EXTRACTION - Extracts vanilla DLLs from game packages to Libs/ for compilation refs and diff comparison

// Extract all vanilla DLLs to Libs/ at startup, returns count extracted (skips existing)
int ExtractVanillaLibraries();

// Proactive extraction from known game packages (gameplay.package, scripts.package, simcore.package)
// Returns total count of newly extracted assemblies.
int ExtractVanillaLibrariesFromGameDir();

// Check if a stored vanilla version of an assembly exists
bool HasVanillaAssembly(const char* assemblyName);

// Get the vanilla MonoImage for comparison, loaded refonly (nullptr if unavailable)
void* GetVanillaImage(const char* assemblyName);

// Get raw bytes of vanilla assembly for direct PE parsing (nullptr if unavailable)
const uint8_t* GetVanillaBytes(const char* assemblyName, uint32_t* outSize);

// Direct RVA-to-pointer mapping on raw PE bytes, bypasses Mono's lazy loading
const uint8_t* VanillaRvaToPointer(const uint8_t* peBytes, uint32_t peSize, uint32_t rva);

// PE metadata context for direct parsing, bypasses Mono entirely
struct PEMetadata {
    const uint8_t* peBytes;
    uint32_t peSize;

    // Heap pointers
    const uint8_t* stringsHeap;
    uint32_t stringsHeapSize;
    const uint8_t* blobHeap;
    uint32_t blobHeapSize;
    const uint8_t* usHeap; // #US (user strings) heap
    uint32_t usHeapSize;

    // Table metadata
    const uint8_t* tablesBase; // Start of #~ stream
    const uint8_t* tableData;  // Start of actual table rows (after header)
    uint32_t tableRows[64];    // Row counts for each table
    uint32_t tableOffsets[64]; // Byte offset to each table's data
    uint8_t heapSizes;         // Bit flags for heap index sizes

    bool valid;
};

// Initialize PE metadata context from raw bytes with full table parsing
bool InitPEMetadata(PEMetadata* ctx, const uint8_t* peBytes, uint32_t peSize);

// Read string from #Strings heap
const char* PEGetString(const PEMetadata* ctx, uint32_t index);

// Read blob from #Blob heap, returns pointer to length-prefixed blob data
const uint8_t* PEGetBlob(const PEMetadata* ctx, uint32_t index);

// Resolve a metadata token to a deterministic string for IL hash normalization
std::string PEResolveToken(const PEMetadata* ctx, uint32_t token);

// Store a vanilla assembly from assembly hooks, takes ownership of data pointer
void StoreVanillaAssembly(const char* assemblyName, const char* sourcePath, uint8_t* data, uint32_t size);

// Record a vanilla package path discovered during assembly load
void RecordVanillaPackage(const std::string& packagePath);

// Get all discovered vanilla package paths
std::vector<std::string> GetDiscoveredPackages();

// Look up assembly name by FNV-1a 64-bit instance ID (nullptr if unknown)
const char* LookupAssemblyName(uint64_t instanceId);

} // namespace S3MM
