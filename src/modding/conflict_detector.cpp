#include "conflict_detector.h"
#include "../util/logger.h"
#include <map>
#include <mutex>

namespace S3MM {

// CORE ASSEMBLY LIST - Framework assemblies that normally load from file; .package loading means mod replacement
static const char* kCoreAssemblies[] = {"ScriptCore", // Game\Bin - NRaas replaces this with IL-patched version
    "SimIFace",                                       // Game\Bin
    "mscorlib",                                       // lib\mono
    nullptr};

// Track which core assemblies were loaded from .package (key = assembly name, value = true if from .package)
static std::map<std::string, bool> s_coreAssemblyFromPackage;
static std::mutex s_coreAssemblyMutex;

// Same-version conflicts: identical versions means mod replaced vanilla (maps assembly name -> conflict info)
struct SameVersionConflict {
    std::string assemblyName;
    std::string version;         // The shared version (e.g., "1.0.5129.30674")
    std::string sourcePackage;   // Package path that provided the replacement
    bool firstLoadedFromPackage; // Was the winner loaded from .package?
    uint32_t firstLoadSize;      // Size of the first-loaded assembly
};
static std::map<std::string, SameVersionConflict> s_sameVersionConflicts;
static std::mutex s_sameVersionConflictsMutex;

// IMPLEMENTATION - Core assembly tracking and conflict detection

bool IsCoreAssembly(const std::string& name) {
    for (const char** p = kCoreAssemblies; *p != nullptr; p++) {
        if (name == *p) return true;
    }
    return false;
}

void RecordCoreAssemblySource(const std::string& name, bool fromPackage) {
    std::lock_guard<std::mutex> lock(s_coreAssemblyMutex);
    s_coreAssemblyFromPackage[name] = fromPackage;

    if (fromPackage) { LOG_INFO("[ConflictDetector] Core assembly '" + name + "' loaded from .package (potential mod)"); }
}

void RecordSameVersionConflict(const std::string& name, const std::string& version, bool firstFromPackage, uint32_t firstSize, const std::string& sourcePackage) {
    std::lock_guard<std::mutex> lock(s_sameVersionConflictsMutex);

    SameVersionConflict conflict;
    conflict.assemblyName = name;
    conflict.version = version;
    conflict.sourcePackage = sourcePackage;
    conflict.firstLoadedFromPackage = firstFromPackage;
    conflict.firstLoadSize = firstSize;

    s_sameVersionConflicts[name] = conflict;

    LOG_WARN("[ConflictDetector] Same-version conflict detected for '" + name + "' (version " + version + ") - mod likely replaced vanilla");
}

bool IsCoreAssemblyReplaced(const char* assemblyName) {
    if (!assemblyName) return false;

    std::lock_guard<std::mutex> lock(s_coreAssemblyMutex);
    auto it = s_coreAssemblyFromPackage.find(assemblyName);
    if (it != s_coreAssemblyFromPackage.end()) {
        return it->second; // true = from .package (replaced), false = from file (vanilla)
    }
    return false; // Not tracked yet
}

std::vector<std::string> GetReplacedCoreAssemblies() {
    std::vector<std::string> result;
    std::lock_guard<std::mutex> lock(s_coreAssemblyMutex);
    for (const auto& kv : s_coreAssemblyFromPackage) {
        if (kv.second) { // true = replaced (loaded from .package, not file)
            result.push_back(kv.first);
        }
    }
    return result;
}

std::vector<AssemblyConflict> GetDetectedConflicts() {
    std::vector<AssemblyConflict> result;
    std::lock_guard<std::mutex> lock(s_sameVersionConflictsMutex);
    for (const auto& kv : s_sameVersionConflicts) {
        AssemblyConflict conflict;
        conflict.name = kv.second.assemblyName;
        conflict.existingVersion = kv.second.version;
        conflict.discardedVersion = kv.second.version; // Same version, that's the point
        conflict.sourcePackage = kv.second.sourcePackage;
        conflict.isCoreAssembly = true;
        result.push_back(conflict);
    }
    return result;
}

} // namespace S3MM
