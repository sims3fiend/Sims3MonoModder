#include "il_xref.h"
#include "il_parser.h"
#include "mono_bridge.h"
#include "../util/logger.h"
#include <unordered_map>
#include <unordered_set>

namespace S3MM {

// TOKEN RESOLUTION CACHE - Avoids redundant string heap lookups per scan
class TokenCache {
  public:
    TokenCache(void* image) : m_image(image) {}

    const std::string& Resolve(uint32_t token) {
        auto it = m_cache.find(token);
        if (it != m_cache.end()) {
            m_hits++;
            return it->second;
        }
        m_misses++;
        auto& entry = m_cache[token];
        entry = ResolveMetadataToken(m_image, token);
        return entry;
    }

    void LogStats() {
        if (m_hits + m_misses > 0) { LOG_INFO("[XRef] Token cache: " + std::to_string(m_hits) + " hits, " + std::to_string(m_misses) + " misses (" + std::to_string(m_cache.size()) + " unique tokens)"); }
    }

  private:
    void* m_image;
    std::unordered_map<uint32_t, std::string> m_cache;
    size_t m_hits = 0;
    size_t m_misses = 0;
};

// IL XREF EXTRACTION - Walk IL and collect token references
// Opcodes that reference metadata tokens we care about for XREFs
static bool IsXRefOpcode(uint8_t opcode) {
    switch (opcode) {
    case 0x28: // call
    case 0x29: // calli
    case 0x6F: // callvirt
    case 0x73: // newobj
    case 0x7B: // ldfld
    case 0x7C: // ldflda
    case 0x7D: // stfld
    case 0x7E: // ldsfld
    case 0x7F: // ldsflda
    case 0x80: // stsfld
        return true;
    default:
        return false;
    }
}

// FE-prefixed opcodes that reference tokens
static bool IsXRefOpcodeFE(uint8_t op2) {
    return op2 == 0x06 || op2 == 0x07; // ldftn, ldvirtftn
}

static std::vector<XRefEntry> ExtractMethodXRefs(void* image, const uint8_t* code, uint32_t codeSize, const std::string& sourceMethod, const std::string& sourceClass, const std::string& sourceNamespace) {
    std::vector<XRefEntry> results;

    if (!code || codeSize == 0 || !image) return results;

    uint32_t pos = 0;
    while (pos < codeSize) {
        uint32_t instructionOffset = pos;
        uint8_t op = code[pos++];

        // Two-byte prefix (0xFE xx)
        if (op == 0xFE) {
            if (pos >= codeSize) break;
            uint8_t op2 = code[pos++];

            ILOperandType opType = GetOpcodeOperandTypeFE(op2);

            if (opType == OP_TOKEN && pos + 4 <= codeSize) {
                uint32_t token = *(uint32_t*)(code + pos);

                // Check if this is an XREF opcode (ldftn, ldvirtftn)
                if (IsXRefOpcodeFE(op2)) {
                    std::string resolved = ResolveMetadataToken(image, token);

                    XRefEntry entry;
                    entry.sourceMethod = sourceMethod;
                    entry.sourceClass = sourceClass;
                    entry.sourceNamespace = sourceNamespace;
                    entry.targetName = resolved;
                    entry.ilOffset = instructionOffset;
                    entry.opcode = op2; // Store the FE-prefixed opcode second byte

                    results.push_back(entry);
                }
                pos += 4;
            } else {
                pos += GetOperandSize(opType);
            }
            continue;
        }

        // Single-byte opcode
        ILOperandType opType = GetOpcodeOperandType(op);

        if (opType == OP_TOKEN && pos + 4 <= codeSize) {
            uint32_t token = *(uint32_t*)(code + pos);

            // Check if this is an XREF opcode we care about
            if (IsXRefOpcode(op)) {
                std::string resolved = ResolveMetadataToken(image, token);

                XRefEntry entry;
                entry.sourceMethod = sourceMethod;
                entry.sourceClass = sourceClass;
                entry.sourceNamespace = sourceNamespace;
                entry.targetName = resolved;
                entry.ilOffset = instructionOffset;
                entry.opcode = op;

                results.push_back(entry);
            }
            pos += 4;
        } else if (opType == OP_SWITCH && pos + 4 <= codeSize) {
            // switch: 4-byte count + 4*count branch targets
            uint32_t count = *(uint32_t*)(code + pos);
            pos += 4;
            pos += count * 4;
        } else {
            pos += GetOperandSize(opType);
        }
    }

    return results;
}

// Cached extraction with early target filter for FindCallers optimization
static void ExtractMatchingCallsToTarget(TokenCache& cache, const uint8_t* code, uint32_t codeSize, const std::string& sourceMethod, const std::string& sourceClass, const std::string& sourceNamespace,
    const std::string& targetFilter, std::vector<XRefEntry>& results) {
    if (!code || codeSize == 0) return;

    uint32_t pos = 0;
    while (pos < codeSize) {
        uint32_t instructionOffset = pos;
        uint8_t op = code[pos++];

        // Two-byte prefix (0xFE xx)
        if (op == 0xFE) {
            if (pos >= codeSize) break;
            uint8_t op2 = code[pos++];
            ILOperandType opType = GetOpcodeOperandTypeFE(op2);

            if (opType == OP_TOKEN && pos + 4 <= codeSize) {
                uint32_t token = *(uint32_t*)(code + pos);
                // Only check ldftn/ldvirtftn for method refs
                if (IsXRefOpcodeFE(op2)) {
                    const std::string& resolved = cache.Resolve(token);
                    if (resolved.find(targetFilter) != std::string::npos) {
                        XRefEntry entry;
                        entry.sourceMethod = sourceMethod;
                        entry.sourceClass = sourceClass;
                        entry.sourceNamespace = sourceNamespace;
                        entry.targetName = resolved;
                        entry.ilOffset = instructionOffset;
                        entry.opcode = op2;
                        results.push_back(entry);
                    }
                }
                pos += 4;
            } else {
                pos += GetOperandSize(opType);
            }
            continue;
        }

        // Single-byte opcode
        ILOperandType opType = GetOpcodeOperandType(op);

        if (opType == OP_TOKEN && pos + 4 <= codeSize) {
            uint32_t token = *(uint32_t*)(code + pos);
            // Only check call opcodes (skip field refs - faster)
            if (op == 0x28 || op == 0x6F || op == 0x73 || op == 0x29) { // call, callvirt, newobj, calli
                const std::string& resolved = cache.Resolve(token);
                if (resolved.find(targetFilter) != std::string::npos) {
                    XRefEntry entry;
                    entry.sourceMethod = sourceMethod;
                    entry.sourceClass = sourceClass;
                    entry.sourceNamespace = sourceNamespace;
                    entry.targetName = resolved;
                    entry.ilOffset = instructionOffset;
                    entry.opcode = op;
                    results.push_back(entry);
                }
            }
            pos += 4;
        } else if (opType == OP_SWITCH && pos + 4 <= codeSize) {
            uint32_t count = *(uint32_t*)(code + pos);
            pos += 4 + count * 4;
        } else {
            pos += GetOperandSize(opType);
        }
    }
}

// ASSEMBLY-WIDE SCANNING - Iterate all types and methods in an image
// Type info for iteration (mirrored from assembly_diff.cpp pattern)
struct TypeInfo {
    std::string namespaceName;
    std::string className;
    std::string fullClassName; // For nested types: Outer/Inner
    int methodStart;
    int methodEnd;
};

// Build type list from metadata tables
static std::vector<TypeInfo> BuildTypeList(void* image) {
    std::vector<TypeInfo> types;

    void* typeDefTable = GetTableInfo(image, MONO_TABLE_TYPEDEF);
    int typeDefCount = GetTableRowCount(typeDefTable);
    if (typeDefCount <= 0) return types;

    void* methodTable = GetTableInfo(image, MONO_TABLE_METHOD);
    int methodCount = GetTableRowCount(methodTable);

    // First pass: collect basic type info
    for (int t = 0; t < typeDefCount; t++) {
        uint32_t nameIdx = MetadataDecodeRowCol(typeDefTable, t, 1);
        uint32_t nsIdx = MetadataDecodeRowCol(typeDefTable, t, 2);
        uint32_t methodListStart = MetadataDecodeRowCol(typeDefTable, t, 5);

        const char* name = MetadataStringHeap(image, nameIdx);
        const char* ns = MetadataStringHeap(image, nsIdx);

        TypeInfo info;
        info.namespaceName = ns ? ns : "";
        info.className = name ? name : "";
        info.fullClassName = info.className;
        info.methodStart = (int)methodListStart;

        // Method end is next type's start, or end of table
        if (t + 1 < typeDefCount) {
            info.methodEnd = (int)MetadataDecodeRowCol(typeDefTable, t + 1, 5);
        } else {
            info.methodEnd = methodCount + 1;
        }

        types.push_back(info);
    }

    return types;
}

std::vector<XRefEntry> FindCallers(void* image, const std::string& targetMethod) {
    std::vector<XRefEntry> results;

    if (!image) return results;

    LOG_INFO("[XRef] Finding callers of: " + targetMethod);

    // Create token cache for this scan
    TokenCache cache(image);

    std::vector<TypeInfo> types = BuildTypeList(image);
    void* methodTable = GetTableInfo(image, MONO_TABLE_METHOD);
    int methodCount = GetTableRowCount(methodTable);

    // Scan all methods in the assembly
    for (const auto& type : types) {
        for (int methodRow = type.methodStart - 1; methodRow < type.methodEnd - 1 && methodRow < methodCount; methodRow++) {

            uint32_t methodNameIdx = MetadataDecodeRowCol(methodTable, methodRow, 3);
            const char* methodName = MetadataStringHeap(image, methodNameIdx);
            if (!methodName || !methodName[0]) continue;

            // Get method IL
            uint32_t rva = MetadataDecodeRowCol(methodTable, methodRow, 0);
            if (rva == 0) continue;

            const uint8_t* codePtr = (const uint8_t*)ImageRvaMap(image, rva);
            if (!codePtr) continue;

            // Parse method header
            uint32_t ilSize = 0;
            const uint8_t* ilCode = nullptr;
            uint8_t headerByte = *codePtr;

            if ((headerByte & 0x03) == 0x02) {
                ilSize = headerByte >> 2;
                ilCode = codePtr + 1;
            } else if ((headerByte & 0x03) == 0x03) {
                const uint32_t* fatHeader = (const uint32_t*)codePtr;
                ilSize = fatHeader[1];
                ilCode = codePtr + 12;
            }

            if (!ilCode || ilSize == 0 || ilSize > 1000000) continue;

            // Use cached extraction with early filter
            ExtractMatchingCallsToTarget(cache, ilCode, ilSize, methodName, type.fullClassName, type.namespaceName, targetMethod, results);
        }
    }

    cache.LogStats();
    LOG_INFO("[XRef] Found " + std::to_string(results.size()) + " callers");
    return results;
}

std::vector<XRefEntry> FindCallersAllAssemblies(const std::string& targetMethod) {
    std::vector<XRefEntry> results;

    auto& bridge = MonoBridge::Get();
    const auto& images = bridge.GetAllImages();

    LOG_INFO("[XRef] Finding callers across " + std::to_string(images.size()) + " assemblies: " + targetMethod);

    for (void* image : images) {
        if (!image) continue;

        const char* name = MonoBridge::GetImageName(image);
        LOG_INFO("[XRef] Scanning: " + std::string(name ? name : "???"));

        auto imageResults = FindCallers(image, targetMethod);

        // Tag results with assembly name for display
        for (auto& entry : imageResults) {
            // Prepend assembly name to namespace for grouping
            if (name && name[0]) { entry.sourceNamespace = std::string("[") + name + "] " + entry.sourceNamespace; }
        }

        results.insert(results.end(), imageResults.begin(), imageResults.end());
    }

    LOG_INFO("[XRef] Total callers across all assemblies: " + std::to_string(results.size()));
    return results;
}

std::vector<XRefEntry> FindCallees(void* image, void* methodHandle, const std::string& methodFullName) {
    std::vector<XRefEntry> results;

    if (!image || !methodHandle) return results;

    LOG_INFO("[XRef] Finding callees of: " + methodFullName);

    // Parse methodFullName (format: "ClassName::MethodName")
    std::string className = "?";
    std::string methodName = "?";
    std::string nsName;

    size_t colonPos = methodFullName.find("::");
    if (colonPos != std::string::npos) {
        std::string fullClass = methodFullName.substr(0, colonPos);
        methodName = methodFullName.substr(colonPos + 2);

        // Split namespace from class name
        size_t lastDot = fullClass.rfind('.');
        if (lastDot != std::string::npos) {
            nsName = fullClass.substr(0, lastDot);
            className = fullClass.substr(lastDot + 1);
        } else {
            className = fullClass;
        }
    }

    // Get IL from method handle using MonoBridge
    auto& bridge = MonoBridge::Get();
    void* header = bridge.GetMethodHeader(methodHandle);
    if (!header) {
        LOG_WARN("[XRef] Could not get method header");
        return results;
    }

    uint32_t ilSize = 0;
    const uint8_t* ilCode = bridge.GetMethodIL(header, &ilSize);
    if (!ilCode || ilSize == 0) {
        LOG_WARN("[XRef] Could not get IL for method");
        return results;
    }

    results = ExtractMethodXRefs(image, ilCode, ilSize, methodName, className, nsName);

    LOG_INFO("[XRef] Found " + std::to_string(results.size()) + " callees");
    return results;
}

} // namespace S3MM
