#pragma once
#include <string>

namespace S3MM {

// CONFIG PATHS - Resolves Documents/Electronic Arts/<Localized Sims 3>/S3MM/ directory
// Mirrors S3SS approach: scans exe string table for localized game folder name
namespace ConfigPaths {

// Base S3MM directory: Documents/Electronic Arts/<Localized Game>/S3MM/
const std::wstring& GetS3MMDirectory();

// Individual file paths (UTF-8)
std::string GetConfigPath(); // S3MM.toml
std::string GetLogPath();    // S3MM.log

// Ensure the S3MM directory exists, creating it if needed
bool EnsureDirectoryExists();

// UTF-8 <-> Wide helpers (centralized here to avoid duplication)
std::string WideToUtf8(const std::wstring& wide);
std::string WideToUtf8(const wchar_t* wide);
std::wstring Utf8ToWide(const std::string& utf8);

} // namespace ConfigPaths

} // namespace S3MM
