#include "mod_compiler.h"
#include "../util/logger.h"
#include "../util/config_paths.h"
#include <fstream>
#include <sstream>
#include <set>
#include <Windows.h>

namespace S3MM {

ModCompiler& ModCompiler::Get() {
    static ModCompiler instance;
    return instance;
}

bool ModCompiler::Initialize() {
    if (m_initialized) return true;

    LOG_INFO("[ModCompiler] Initializing...");

    // Set up folder structure under Documents/EA/<Sims 3>/S3MM/
    m_basePath = ConfigPaths::GetS3MMDirectory();
    m_modsPath = m_basePath / "Mods";
    m_sourcePath = m_basePath / "Source";
    m_libsPath = m_basePath / "Libs";

    // Create directories if they don't exist
    std::error_code ec;
    std::filesystem::create_directories(m_modsPath, ec);
    std::filesystem::create_directories(m_sourcePath, ec);
    std::filesystem::create_directories(m_libsPath, ec);

    // Find the best available .NET Framework csc.exe
    m_cscPath = FindBestCscExe();
    if (m_cscPath.empty()) {
        LOG_WARN("[ModCompiler] No .NET Framework csc.exe found");
        LOG_WARN("[ModCompiler] C# source compilation not available - use pre-compiled DLLs");
        LOG_WARN("[ModCompiler] Install .NET Framework or provide pre-compiled .dll files");
        m_canCompile = false;
    } else {
        LOG_DEBUG("[ModCompiler] Found csc.exe: " + m_cscPath.string());
        m_canCompile = true;
    }

    m_initialized = true;
    LOG_INFO("[ModCompiler] Initialized (compilation " + std::string(m_canCompile ? "available" : "unavailable") + ")");
    return true;
}

std::vector<ModInfo> ModCompiler::DiscoverMods() {
    std::vector<ModInfo> mods;

    // Pre-scan Source/ project names. A source project's compiled output lands in Mods/ under the same name, so we must NOT let that output shadow the source h-haha...
    std::set<std::string> sourceProjectNames;
    if (std::filesystem::exists(m_sourcePath)) {
        for (const auto& entry : std::filesystem::directory_iterator(m_sourcePath)) {
            if (!entry.is_directory()) continue;
            bool hasCs = false;
            for (const auto& file : std::filesystem::recursive_directory_iterator(entry.path())) {
                if (file.is_regular_file() && file.path().extension() == ".cs") {
                    hasCs = true;
                    break;
                }
            }
            if (hasCs) sourceProjectNames.insert(entry.path().filename().string());
        }
    }

    // 1. Scan Mods/ for pre-compiled DLLs (flat structure). Skip any DLL that is the build output of a Source/ project - that one is handled by the source scan so source edits aren't shadowed.
    if (std::filesystem::exists(m_modsPath)) {
        for (const auto& entry : std::filesystem::directory_iterator(m_modsPath)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".dll") continue;

            std::string name = entry.path().stem().string(); // "MyMod.dll" -> "MyMod"
            if (sourceProjectNames.count(name)) {
                LOG_DEBUG("[ModCompiler] '" + name + "' in Mods/ is a Source/ build output - deferring to source scan");
                continue;
            }

            ModInfo mod;
            mod.name = name;
            mod.compiledDll = entry.path();
            mod.compiledSuccessfully = true;
            mod.needsRecompile = false;

            LOG_DEBUG("[ModCompiler] Found pre-compiled mod: " + mod.name);
            mods.push_back(mod);
        }
    }

    // 2. Scan Source/ for source projects (each subdirectory is a project)
    if (std::filesystem::exists(m_sourcePath)) {
        for (const auto& entry : std::filesystem::directory_iterator(m_sourcePath)) {
            if (!entry.is_directory()) continue;

            ModInfo mod;
            mod.name = entry.path().filename().string();
            mod.sourceDir = entry.path();
            mod.compiledDll = m_modsPath / (mod.name + ".dll"); // Output to Mods/

            // Find all .cs files in the source project
            for (const auto& file : std::filesystem::recursive_directory_iterator(entry.path())) {
                if (file.is_regular_file() && file.path().extension() == ".cs") { mod.sourceFiles.push_back(file.path()); }
            }

            if (mod.sourceFiles.empty()) {
                LOG_WARN("[ModCompiler] Source project '" + mod.name + "' has no .cs files, skipping");
                continue;
            }

            mod.hasSourceFiles = true;

            // Check if we can compile
            if (!m_canCompile) {
                LOG_WARN("[ModCompiler] Source '" + mod.name + "' has .cs files but csc.exe not available");
                LOG_WARN("[ModCompiler] Please compile manually or install .NET Framework");
                continue;
            }

            // Check if compiled DLL exists and is newer than all sources
            if (std::filesystem::exists(mod.compiledDll)) {
                auto dllTime = std::filesystem::last_write_time(mod.compiledDll);
                bool sourceNewer = false;

                for (const auto& src : mod.sourceFiles) {
                    if (std::filesystem::last_write_time(src) > dllTime) {
                        sourceNewer = true;
                        break;
                    }
                }

                if (!sourceNewer) {
                    mod.compiledSuccessfully = true;
                    mod.needsRecompile = false;
                    mods.push_back(mod);
                    continue;
                }
            }

            mod.needsRecompile = true;
            mods.push_back(mod);
        }
    }

    LOG_INFO("[ModCompiler] Discovered " + std::to_string(mods.size()) + " mod(s)");
    return mods;
}

bool ModCompiler::CompileMod(ModInfo& mod) {
    if (!mod.needsRecompile) { return true; }

    if (!m_canCompile) {
        LOG_ERROR("[ModCompiler] Cannot compile '" + mod.name + "' - csc.exe not available");
        return false;
    }

    if (mod.sourceFiles.empty()) {
        LOG_ERROR("[ModCompiler] Cannot compile '" + mod.name + "' - no source files");
        return false;
    }

    LOG_INFO("[ModCompiler] Compiling mod: " + mod.name);

    std::string errorOutput;
    if (!RunCompiler(mod, mod.compiledDll, errorOutput)) {
        LOG_ERROR("[ModCompiler] Compilation failed for '" + mod.name + "'");
        if (!errorOutput.empty()) { LOG_ERROR("[ModCompiler] Errors:\n" + errorOutput); }
        return false;
    }

    mod.compiledSuccessfully = true;
    mod.needsRecompile = false;
    LOG_INFO("[ModCompiler] Successfully compiled: " + mod.name);
    return true;
}

bool ModCompiler::CompileModTo(const ModInfo& mod, const std::filesystem::path& outputPath) {
    if (!m_canCompile) {
        LOG_ERROR("[ModCompiler] Cannot compile '" + mod.name + "' - csc.exe not available");
        return false;
    }

    if (mod.sourceFiles.empty()) {
        LOG_ERROR("[ModCompiler] Cannot compile '" + mod.name + "' - no source files");
        return false;
    }

    LOG_INFO("[ModCompiler] Compiling mod to: " + outputPath.string());

    std::string errorOutput;
    if (!RunCompiler(mod, outputPath, errorOutput)) {
        LOG_ERROR("[ModCompiler] Compilation failed for '" + mod.name + "'");
        if (!errorOutput.empty()) { LOG_ERROR("[ModCompiler] Errors:\n" + errorOutput); }
        return false;
    }

    LOG_INFO("[ModCompiler] Successfully compiled: " + mod.name);
    return true;
}

int ModCompiler::CompileAllMods(std::vector<ModInfo>& mods) {
    int successCount = 0;
    int failCount = 0;
    int skippedCount = 0;

    for (auto& mod : mods) {
        if (!mod.needsRecompile) {
            if (mod.compiledSuccessfully) { skippedCount++; }
            continue;
        }

        if (CompileMod(mod)) {
            successCount++;
        } else {
            failCount++;
        }
    }

    LOG_INFO("[ModCompiler] Compilation complete: " + std::to_string(successCount) + " compiled, " + std::to_string(skippedCount) + " up-to-date, " + std::to_string(failCount) + " failed");

    return successCount + skippedCount;
}

bool ModCompiler::RunCompiler(const ModInfo& mod, const std::filesystem::path& outputPath, std::string& errorOutput) {
    // Build command line for csc.exe with settings:
    //   /target:library /nostdlib /platform:x86 /unsafe
    //   /reference:Libs\mscorlib.dll /reference:Libs\SimIFace.dll ...
    //   /out:Mods\ModName.dll Source\ModName\*.cs

    std::stringstream cmd;
    cmd << "\"" << m_cscPath.string() << "\"";
    cmd << " /noconfig"; // Don't use csc.rsp (which auto-adds System.dll etc)
    cmd << " /target:library";
    cmd << " /nostdlib";         // Don't use system mscorlib
    cmd << " /platform:x86";     // Game is 32-bit
    cmd << " /unsafe";           // Allow unsafe code
    cmd << " /nowarn:1701,1702"; // Suppress binding redirect warnings
    cmd << " /out:\"" << outputPath.string() << "\"";

    // Add reference DLLs from Libs folder
    std::string refArgs = BuildReferenceArgs();
    if (refArgs.empty()) {
        LOG_ERROR("[ModCompiler] No reference DLLs found in Libs folder");
        LOG_ERROR("[ModCompiler] Need at least mscorlib.dll from the game");
        return false;
    }
    cmd << refArgs;

    // Add source files
    for (const auto& src : mod.sourceFiles) { cmd << " \"" << src.string() << "\""; }

    // Generate API version metadata file, injected into every mod compilation. This embeds the API version the mod was compiled against, readable by the native loader.
    std::filesystem::path versionFile = m_basePath / "_s3mm_version.cs";
    {
        std::ofstream vf(versionFile);
        vf << "internal static class S3MM_CompilerMeta { internal static int ApiVersion = " << API_VERSION << "; }" << std::endl;
    }
    cmd << " \"" << versionFile.string() << "\"";

    std::string cmdLine = cmd.str();
    LOG_DEBUG("[ModCompiler] Command: " + cmdLine);

    // Create process to run csc
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    // Create pipes for stdout/stderr
    HANDLE hStdoutRead, hStdoutWrite;
    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0)) {
        LOG_ERROR("[ModCompiler] Failed to create pipe");
        return false;
    }
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdError = hStdoutWrite;
    si.hStdOutput = hStdoutWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};

    // Need mutable string for CreateProcess
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    BOOL success = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        m_basePath.string().c_str(), // Working directory
        &si, &pi);

    CloseHandle(hStdoutWrite);

    if (!success) {
        LOG_ERROR("[ModCompiler] Failed to start csc.exe: " + std::to_string(GetLastError()));
        CloseHandle(hStdoutRead);
        return false;
    }

    // Wait for completion (with timeout)
    DWORD waitResult = WaitForSingleObject(pi.hProcess, 60000); // 60 second timeout

    // Read output
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hStdoutRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        errorOutput += buffer;
    }
    CloseHandle(hStdoutRead);

    DWORD exitCode = 1;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    } else {
        LOG_ERROR("[ModCompiler] Compilation timed out");
        TerminateProcess(pi.hProcess, 1);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitCode == 0;
}

std::string ModCompiler::BuildReferenceArgs() {
    std::stringstream args;

    if (!std::filesystem::exists(m_libsPath)) { return ""; }

    bool foundMscorlib = false;

    // Add all DLLs in Libs folder as references
    for (const auto& entry : std::filesystem::directory_iterator(m_libsPath)) {
        if (entry.is_regular_file() && entry.path().extension() == ".dll") {
            args << " /reference:\"" << entry.path().string() << "\"";

            std::string filename = entry.path().filename().string();
            if (filename == "mscorlib.dll") { foundMscorlib = true; }
        }
    }

    if (!foundMscorlib) {
        LOG_WARN("[ModCompiler] mscorlib.dll not found in Libs folder!");
        LOG_WARN("[ModCompiler] Copy mscorlib.dll from game installation to S3MM/Libs/");
    }

    return args.str();
}

std::filesystem::path ModCompiler::FindBestCscExe() {
    // Scan for .NET Framework csc.exe, preferring newer versions.
    // Newer compilers support more C# syntax but can still target .NET 2.0 when using /nostdlib and referencing the game's assemblies.

    const std::filesystem::path frameworkBase = "C:\\Windows\\Microsoft.NET\\Framework";

    // Check in order of preference (newest first): v4.0.30319 (.NET 4.x/C# 5.0), v3.5 (.NET 3.5/C# 3.0), v2.0.50727 (.NET 2.0/C# 2.0).
    const std::vector<std::string> versions = {"v4.0.30319", "v3.5", "v2.0.50727"};

    for (const auto& ver : versions) {
        std::filesystem::path cscPath = frameworkBase / ver / "csc.exe";
        if (std::filesystem::exists(cscPath)) {
            LOG_INFO("[ModCompiler] Found .NET Framework " + ver + " compiler");
            return cscPath;
        }
    }

    // Also check 64-bit framework folder (though game is 32-bit, compiler works fine)
    const std::filesystem::path framework64Base = "C:\\Windows\\Microsoft.NET\\Framework64";
    for (const auto& ver : versions) {
        std::filesystem::path cscPath = framework64Base / ver / "csc.exe";
        if (std::filesystem::exists(cscPath)) {
            LOG_INFO("[ModCompiler] Found .NET Framework " + ver + " compiler (64-bit)");
            return cscPath;
        }
    }

    return {};
}

} // namespace S3MM
