#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace S3MM {

// MOD INFO - Represents a discovered mod
struct ModInfo {
    std::string name;                               // Mod folder name
    std::filesystem::path sourceDir;                // Path to mod folder
    std::vector<std::filesystem::path> sourceFiles; // All .cs files (if any)
    std::filesystem::path compiledDll;              // DLL path (pre-compiled or cached)
    bool needsRecompile = false;                    // Source newer than DLL
    bool compiledSuccessfully = false;
    bool hasSourceFiles = false; // True if .cs files exist
};

// MOD COMPILER - Discovers mods and optionally compiles C# sources using the highest available .NET Framework csc.exe
class ModCompiler {
  public:
    // Bump ONLY when managed API shape changes (added/removed/renamed ICALLs, changed signatures, attribute semantics)
    static constexpr int API_VERSION = 2;

    static ModCompiler& Get();

    // Initialize compiler - sets up paths, finds csc.exe
    bool Initialize();

    // Scan mods folder for DLLs and/or source files
    std::vector<ModInfo> DiscoverMods();

    // Compile a single mod (if needed and possible)
    bool CompileMod(ModInfo& mod);

    // Compile a mod to a specific output path (for hot-reload with versioned names)
    bool CompileModTo(const ModInfo& mod, const std::filesystem::path& outputPath);

    // Compile all mods that need it
    int CompileAllMods(std::vector<ModInfo>& mods);

    // Get paths
    const std::filesystem::path& GetModsPath() const { return m_modsPath; }
    const std::filesystem::path& GetLibsPath() const { return m_libsPath; }

  private:
    ModCompiler() = default;
    ~ModCompiler() = default;
    ModCompiler(const ModCompiler&) = delete;
    ModCompiler& operator=(const ModCompiler&) = delete;

    // Run csc.exe
    bool RunCompiler(const ModInfo& mod, const std::filesystem::path& outputPath, std::string& errorOutput);

    // Build reference arguments for csc
    std::string BuildReferenceArgs();

    // Find the best available csc.exe (prefers newer .NET Framework versions)
    std::filesystem::path FindBestCscExe();

    bool m_initialized = false;
    bool m_canCompile = false;
    std::filesystem::path m_basePath;   // S3MM/ folder
    std::filesystem::path m_modsPath;   // S3MM/Mods/ - compiled DLLs go here
    std::filesystem::path m_sourcePath; // S3MM/Source/ - source projects live here
    std::filesystem::path m_libsPath;   // S3MM/Libs/
    std::filesystem::path m_cscPath;    // Path to csc.exe
};

} // namespace S3MM
