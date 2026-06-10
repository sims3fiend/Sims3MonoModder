#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace S3MM {

// ASSEMBLY CONFLICT DETECTION - Tracks core assembly replacements and version conflicts

// Assembly conflict info (for UI display)
struct AssemblyConflict {
    std::string name;             // Assembly name (e.g., "ScriptCore")
    std::string existingVersion;  // Version that was kept (e.g., "1.0.0.0")
    std::string discardedVersion; // Version that was discarded
    std::string sourcePackage;    // Package path that provided the replacement (may be empty)
    bool isCoreAssembly;          // true if this is a core game assembly
};

// Check if a name is a core game assembly (ScriptCore, SimIFace, mscorlib)
bool IsCoreAssembly(const std::string& name);

// Record that a core assembly was loaded from a specific source (fromPackage: true = from .package mod, false = from file).
void RecordCoreAssemblySource(const std::string& name, bool fromPackage);

// Record a same-version conflict (mod replaced vanilla with identical version)
void RecordSameVersionConflict(const std::string& name, const std::string& version, bool firstFromPackage, uint32_t firstSize, const std::string& sourcePackage = "");

// Check if a core assembly was replaced (loaded from .package instead of file)
bool IsCoreAssemblyReplaced(const char* assemblyName);

// Get list of all core assemblies that were replaced by .package mods
std::vector<std::string> GetReplacedCoreAssemblies();

// Get list of detected assembly conflicts (duplicates where one was discarded)
std::vector<AssemblyConflict> GetDetectedConflicts();

} // namespace S3MM
