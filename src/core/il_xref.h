#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace S3MM {

// IL CROSS-REFERENCE TYPES - Call and field references extracted from IL
struct XRefEntry {
    std::string sourceMethod;    // "Update"
    std::string sourceClass;     // "Actor"
    std::string sourceNamespace; // "Sims3.Gameplay.Actors"
    std::string targetName;      // Resolved target (e.g., "Sim::DoThing")
    uint32_t ilOffset;           // Position in source method's IL
    uint8_t opcode;              // 0x28 (call), 0x6F (callvirt), etc.

    // Convenience: full qualified source name
    std::string SourceFullName() const {
        std::string result;
        if (!sourceNamespace.empty()) { result = sourceNamespace + "."; }
        result += sourceClass + "::" + sourceMethod;
        return result;
    }
};

// Opcode categories for filtering
enum class XRefType : uint8_t {
    Call = 0x28,      // call
    Calli = 0x29,     // calli (indirect call)
    CallVirt = 0x6F,  // callvirt
    NewObj = 0x73,    // newobj
    Ldftn = 0x06,     // FE 06 - ldftn
    LdVirtFtn = 0x07, // FE 07 - ldvirtftn
    Ldfld = 0x7B,     // ldfld
    Ldflda = 0x7C,    // ldflda
    Stfld = 0x7D,     // stfld
    Ldsfld = 0x7E,    // ldsfld
    Ldsflda = 0x7F,   // ldsflda
    Stsfld = 0x80,    // stsfld
};

// IL XREF EXTRACTION API - Assembly-wide caller/callee scanning

// Scan entire assembly for callers, targetMethod as "ClassName::MethodName"
std::vector<XRefEntry> FindCallers(void* image, const std::string& targetMethod);

// Scan all loaded assemblies for callers (slower but comprehensive)
std::vector<XRefEntry> FindCallersAllAssemblies(const std::string& targetMethod);

// Get all callees from a method handle, methodFullName as "ClassName::MethodName"
std::vector<XRefEntry> FindCallees(void* image, void* methodHandle, const std::string& methodFullName);

} // namespace S3MM
