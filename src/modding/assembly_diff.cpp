#include "assembly_diff.h"
#include "../core/vanilla_extractor.h"
#include "mod_compiler.h"
#include "../core/mono_bridge.h"
#include "../core/il_parser.h"
#include "../util/logger.h"
#include <vector>
#include <map>
#include <fstream>
#include <filesystem>
#include <cstring>

namespace S3MM {

// METHOD MAP BUILDING - For vanilla images, pass raw PE bytes to bypass Mono's broken RVA mapping.
// When peCtx is provided, uses PE-based token resolution (deterministic).
static std::map<std::string, MethodKey> BuildMethodMapInternal(void* image, const uint8_t* rawPeBytes, uint32_t rawPeSize, const PEMetadata* peCtx = nullptr) {

    std::map<std::string, MethodKey> methods;

    if (!image || !IsValidPointer(image)) return methods;

    void* typeDefTable = GetTableInfo(image, MONO_TABLE_TYPEDEF);
    void* methodTable = GetTableInfo(image, MONO_TABLE_METHOD);

    int typeDefCount = GetTableRowCount(typeDefTable);
    int methodCount = GetTableRowCount(methodTable);

    if (typeDefCount <= 0 || typeDefCount > 50000) return methods;
    if (methodCount <= 0 || methodCount > 500000) return methods;

    // Build NestedClass lookup: nestedTypeIdx -> enclosingTypeIdx (0-based)
    void* nestedTable = GetTableInfo(image, 0x29); // NestedClass table
    int nestedCount = GetTableRowCount(nestedTable);
    std::map<int, int> nestedToEnclosing;
    for (int i = 0; i < nestedCount; i++) {
        uint32_t nestedIdx = MetadataDecodeRowCol(nestedTable, i, 0);    // NestedClass (1-based)
        uint32_t enclosingIdx = MetadataDecodeRowCol(nestedTable, i, 1); // EnclosingClass (1-based)
        nestedToEnclosing[(int)nestedIdx - 1] = (int)enclosingIdx - 1;
    }

    // Build TypeDef info
    struct TypeInfo {
        std::string namespaceName;
        std::string className;
        std::string fullClassName; // includes enclosing types with + separator
        int methodStart;
        int methodEnd;
    };
    std::vector<TypeInfo> types(typeDefCount);

    // First pass: get simple names
    for (int i = 0; i < typeDefCount; i++) {
        uint32_t typeNameIdx = MetadataDecodeRowCol(typeDefTable, i, 1);
        uint32_t typeNsIdx = MetadataDecodeRowCol(typeDefTable, i, 2);
        uint32_t methodList = MetadataDecodeRowCol(typeDefTable, i, 5);

        const char* typeName = MetadataStringHeap(image, typeNameIdx);
        const char* typeNs = MetadataStringHeap(image, typeNsIdx);

        types[i].className = typeName ? typeName : "";
        types[i].namespaceName = typeNs ? typeNs : "";
        types[i].fullClassName = types[i].className;
        types[i].methodStart = methodList;
        types[i].methodEnd = methodCount + 1;
    }

    // Second pass: resolve nested type full names
    for (int i = 0; i < typeDefCount; i++) {
        auto it = nestedToEnclosing.find(i);
        if (it != nestedToEnclosing.end()) {
            // This is a nested type - build full name from enclosing chain
            std::string fullName = types[i].className;
            int enclosing = it->second;
            int depth = 0;
            while (enclosing >= 0 && enclosing < typeDefCount && depth < 10) {
                fullName = types[enclosing].className + "+" + fullName;
                // Check if enclosing is also nested
                auto eit = nestedToEnclosing.find(enclosing);
                if (eit != nestedToEnclosing.end()) {
                    enclosing = eit->second;
                } else {
                    // Found top-level, use its namespace
                    types[i].namespaceName = types[enclosing].namespaceName;
                    break;
                }
                depth++;
            }
            types[i].fullClassName = fullName;
        }
    }

    for (int i = 0; i < typeDefCount - 1; i++) { types[i].methodEnd = types[i + 1].methodStart; }

    // Build method map
    for (int typeIdx = 0; typeIdx < typeDefCount; typeIdx++) {
        const TypeInfo& typeInfo = types[typeIdx];

        for (int methodRow = typeInfo.methodStart - 1; methodRow < typeInfo.methodEnd - 1 && methodRow < methodCount; methodRow++) {

            uint32_t methodNameIdx = MetadataDecodeRowCol(methodTable, methodRow, 3);
            const char* methodName = MetadataStringHeap(image, methodNameIdx);
            if (!methodName || !methodName[0]) continue;

            // Get RVA and IL
            uint32_t rva = MetadataDecodeRowCol(methodTable, methodRow, 0);
            uint32_t ilHash = 0;
            uint32_t ilSize = 0;
            uint32_t rawHash = 0;

            if (rva != 0) {
                const uint8_t* codePtr = nullptr;

                // Use direct PE parsing for vanilla (bypasses Mono's broken lazy RVA mapping)
                if (rawPeBytes && rawPeSize > 0) {
                    codePtr = VanillaRvaToPointer(rawPeBytes, rawPeSize, rva);
                } else {
                    codePtr = (const uint8_t*)ImageRvaMap(image, rva);
                }

                if (codePtr && IsValidPointer((void*)codePtr)) {
                    // Parse method header to get IL
                    uint8_t headerByte = *codePtr;
                    const uint8_t* ilCode = nullptr;

                    if ((headerByte & 0x03) == 0x02) {
                        // Tiny header
                        ilSize = headerByte >> 2;
                        ilCode = codePtr + 1;
                    } else if ((headerByte & 0x03) == 0x03) {
                        // Fat header
                        const uint32_t* fatHeader = (const uint32_t*)codePtr;
                        ilSize = fatHeader[1];
                        ilCode = codePtr + 12;
                    }

                    if (ilCode && ilSize > 0 && ilSize < 1000000) {
                        // Use PE-based token resolution if available (deterministic)
                        // Otherwise fall back to Mono-based resolution
                        if (peCtx && peCtx->valid) {
                            ilHash = HashILNormalizedPE(peCtx, ilCode, ilSize);
                        } else {
                            ilHash = HashILNormalized(image, ilCode, ilSize);
                        }
                        // DEBUG: compute raw hash (no token normalization)
                        rawHash = 0x811c9dc5;
                        for (uint32_t i = 0; i < ilSize; i++) {
                            rawHash ^= ilCode[i];
                            rawHash *= 0x01000193;
                        }
                    }
                }
            }

            MethodKey key;
            key.namespaceName = typeInfo.namespaceName;
            key.className = typeInfo.fullClassName; // Use full nested type path
            key.methodName = methodName;
            key.ilHash = ilHash;
            key.ilSize = ilSize;
            key.rawHash = rawHash;

            std::string fullName = key.FullName();
            methods[fullName] = key;
        }
    }

    return methods;
}

static void* GetLoadedImageByName(const char* assemblyName) {
    if (!assemblyName) return nullptr;

    const auto& images = MonoBridge::Get().GetAllImages();
    for (void* image : images) {
        const char* name = MonoBridge::GetImageName(image);
        if (name && strcmp(name, assemblyName) == 0) { return image; }
    }
    return nullptr;
}

// Load vanilla from disk using vanilla_extractor's infrastructure (same code path as the working original)
static void* LoadVanillaFromDisk(const char* assemblyName) {
    // Check if already loaded via vanilla_extractor
    if (HasVanillaAssembly(assemblyName)) {
        void* cached = GetVanillaImage(assemblyName);
        LOG_INFO("[Diff] Using cached vanilla from vanilla_extractor @ " + ToHex((uintptr_t)cached));
        return cached;
    }

    std::filesystem::path libsPath = ModCompiler::Get().GetLibsPath();
    if (libsPath.empty()) return nullptr;

    // Plain .dll from libs/ (extracted via "Dump Vanilla Libs" button)
    std::vector<std::filesystem::path> tryPaths = {libsPath / (std::string(assemblyName) + ".dll")};

    for (const auto& vanillaDll : tryPaths) {
        if (!std::filesystem::exists(vanillaDll)) continue;

        LOG_INFO("[Diff] Loading vanilla from disk: " + vanillaDll.string());

        std::ifstream file(vanillaDll, std::ios::binary | std::ios::ate);
        if (!file) continue;

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        // Allocate with new[] since StoreVanillaAssembly takes ownership
        uint8_t* bytes = new uint8_t[size];
        if (!file.read(reinterpret_cast<char*>(bytes), size)) {
            delete[] bytes;
            continue;
        }

        // Use vanilla_extractor's infrastructure (same path as original)
        StoreVanillaAssembly(assemblyName, vanillaDll.string().c_str(), bytes, (uint32_t)size);

        // Now get the loaded image
        void* image = GetVanillaImage(assemblyName);
        if (image) {
            LOG_INFO("[Diff] Loaded vanilla MonoImage @ " + ToHex((uintptr_t)image));
            return image;
        }
    }

    return nullptr;
}

// Internal implementation for ComputeAssemblyDiff
static AssemblyDiffResult ComputeDiffInternal(const char* assemblyName, bool logOutput) {
    AssemblyDiffResult result;

    if (!assemblyName) {
        result.error = "Null assembly name";
        return result;
    }

    if (logOutput) { LOG_INFO("[Diff] Comparing vanilla vs loaded: " + std::string(assemblyName)); }

    // Try in-memory cache first, then disk
    void* vanillaImage = GetVanillaImage(assemblyName);
    if (!vanillaImage) { vanillaImage = LoadVanillaFromDisk(assemblyName); }

    void* loadedImage = GetLoadedImageByName(assemblyName);

    if (!vanillaImage) {
        result.error = "No vanilla image for " + std::string(assemblyName) + ". Run 'Extract Vanilla Libs' first.";
        if (logOutput) LOG_WARN("[Diff] " + result.error);
        return result;
    }
    if (!loadedImage) {
        result.error = "No loaded image for " + std::string(assemblyName);
        if (logOutput) LOG_WARN("[Diff] " + result.error);
        return result;
    }

    // Same pointer = no mod replacement
    if (vanillaImage == loadedImage) {
        if (logOutput) LOG_INFO("[Diff] Same image pointer - no mod replacement detected");
        result.valid = true; // Valid but no changes
        return result;
    }

    if (logOutput) {
        LOG_INFO("[Diff] Vanilla: " + ToHex((uintptr_t)vanillaImage) + " (" + std::to_string(MonoBridge::Get().GetTypeDefCount(vanillaImage)) + " types)");
        LOG_INFO("[Diff] Loaded:  " + ToHex((uintptr_t)loadedImage) + " (" + std::to_string(MonoBridge::Get().GetTypeDefCount(loadedImage)) + " types)");
        LOG_INFO("[Diff] Building method maps...");
    }

    // Get raw PE bytes for vanilla - use direct PE parsing to bypass broken refonly
    uint32_t vanillaSize = 0;
    const uint8_t* vanillaBytes = GetVanillaBytes(assemblyName, &vanillaSize);

    // Initialize PE metadata context for deterministic token resolution
    PEMetadata vanillaPE;
    bool havePE = false;
    if (vanillaBytes && vanillaSize > 0) {
        havePE = InitPEMetadata(&vanillaPE, vanillaBytes, vanillaSize);
        if (logOutput) {
            if (havePE) {
                LOG_INFO("[Diff] Using PE-based parsing for vanilla (" + std::to_string(vanillaSize) + " bytes, " + std::to_string(vanillaPE.tableRows[0x06]) + " methods in table)");
            } else {
                LOG_WARN("[Diff] PE metadata init failed - falling back to Mono");
            }
        }
    } else if (logOutput) {
        LOG_WARN("[Diff] No vanilla bytes available - using Mono (may be inconsistent)");
    }

    auto vanillaMethods = BuildMethodMapInternal(vanillaImage, vanillaBytes, vanillaSize, havePE ? &vanillaPE : nullptr);
    auto loadedMethods = BuildMethodMapInternal(loadedImage, nullptr, 0, nullptr);

    if (logOutput) {
        LOG_INFO("[Diff] Vanilla methods: " + std::to_string(vanillaMethods.size()));
        LOG_INFO("[Diff] Loaded methods: " + std::to_string(loadedMethods.size()));
    }

    // Find modified and removed methods
    for (const auto& [name, vanillaKey] : vanillaMethods) {
        auto it = loadedMethods.find(name);
        if (it == loadedMethods.end()) {
            result.removedMethods.push_back(name);
        } else {
            const auto& loadedKey = it->second;
            bool isModified = (vanillaKey.ilSize != loadedKey.ilSize || vanillaKey.ilHash != loadedKey.ilHash);

            if (isModified) {
                result.modifiedMethods.push_back(name);
            } else {
                result.identicalCount++;
            }
        }
    }

    // Find added methods
    for (const auto& [name, loadedKey] : loadedMethods) {
        if (vanillaMethods.find(name) == vanillaMethods.end()) { result.addedMethods.push_back(name); }
    }

    result.valid = true;

    if (logOutput) {
        LOG_INFO("");
        LOG_INFO("=== DIFF SUMMARY ===");
        LOG_INFO("Identical: " + std::to_string(result.identicalCount));
        LOG_INFO("Modified:  " + std::to_string(result.modifiedMethods.size()));
        LOG_INFO("Added:     " + std::to_string(result.addedMethods.size()));
        LOG_INFO("Removed:   " + std::to_string(result.removedMethods.size()));

        if (!result.modifiedMethods.empty()) {
            LOG_INFO("");
            LOG_INFO("=== MODIFIED METHODS ===");
            for (const auto& m : result.modifiedMethods) { LOG_INFO("  [M] " + m); }
        }

        if (!result.addedMethods.empty()) {
            LOG_INFO("");
            LOG_INFO("=== ADDED METHODS ===");
            for (const auto& a : result.addedMethods) { LOG_INFO("  [+] " + a); }
        }

        if (!result.removedMethods.empty()) {
            LOG_INFO("");
            LOG_INFO("=== REMOVED METHODS ===");
            for (const auto& r : result.removedMethods) { LOG_INFO("  [-] " + r); }
        }
    }

    return result;
}

AssemblyDiffResult ComputeAssemblyDiff(const char* assemblyName) {
    return ComputeDiffInternal(assemblyName, false);
}

} // namespace S3MM
