#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace S3MM {

// ASSEMBLY DIFFING - Compare vanilla vs mod-modified assemblies

// Key for identifying a method across assemblies
struct MethodKey {
    std::string namespaceName;
    std::string className;
    std::string methodName;
    uint32_t ilHash;
    uint32_t ilSize;
    uint32_t rawHash; // DEBUG: raw IL bytes hash (no token normalization)

    std::string FullName() const {
        if (namespaceName.empty()) return className + "::" + methodName;
        return namespaceName + "." + className + "::" + methodName;
    }
};

// Diff result structure for UI display
struct AssemblyDiffResult {
    std::vector<std::string> modifiedMethods;
    std::vector<std::string> addedMethods;
    std::vector<std::string> removedMethods;
    int identicalCount = 0;
    bool valid = false; // false if comparison couldn't be performed
    std::string error;  // error message if not valid
};

// Compare vanilla vs loaded and return structured results (for UI)
AssemblyDiffResult ComputeAssemblyDiff(const char* assemblyName);

} // namespace S3MM
