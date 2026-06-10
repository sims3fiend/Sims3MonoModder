#include "config_paths.h"
#include <Windows.h>
#include <Shlobj.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace S3MM {
namespace ConfigPaths {

// Resolve the localized game folder name (e.g. "Die Sims 3")
// Scans the exe's string table the same way the game does internally.
// String table is organized in blocks of 100 per locale: base+0 = locale code (e.g. "fr-fr"),
// base+1 = display name with TM, base+2 = folder name
static std::wstring ResolveLocalizedGameFolder() {
    HMODULE exe = GetModuleHandle(NULL);
    if (!exe) return L"The Sims 3";

    wchar_t sysLocale[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(sysLocale, LOCALE_NAME_MAX_LENGTH) == 0) { return L"The Sims 3"; }
    _wcslwr_s(sysLocale);

    // Extract language prefix (e.g. "en" from "en-au") for fallback matching
    std::wstring sysLang(sysLocale);
    auto hyphen = sysLang.find(L'-');
    if (hyphen != std::wstring::npos) sysLang.resize(hyphen);

    wchar_t blockLocale[16];
    wchar_t gameName[MAX_PATH];
    UINT prefixMatch = 0;

    for (UINT base = 1000; base < 3600; base += 100) {
        if (LoadStringW(exe, base, blockLocale, 16) > 0) {
            _wcslwr_s(blockLocale);
            if (wcscmp(sysLocale, blockLocale) == 0 && LoadStringW(exe, base + 2, gameName, MAX_PATH) > 0) { return gameName; }
            if (prefixMatch == 0 && wcsncmp(sysLang.c_str(), blockLocale, sysLang.size()) == 0) { prefixMatch = base; }
        }
    }
    if (prefixMatch != 0 && LoadStringW(exe, prefixMatch + 2, gameName, MAX_PATH) > 0) { return gameName; }
    return L"The Sims 3";
}

std::string WideToUtf8(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(size - 1, '\0'); // -1 to exclude null terminator
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::string WideToUtf8(const std::wstring& wide) {
    return WideToUtf8(wide.c_str());
}

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), result.data(), size);
    return result;
}

const std::wstring& GetS3MMDirectory() {
    static std::wstring s3mmDir;
    if (s3mmDir.empty()) {
        wchar_t docPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docPath))) { s3mmDir = std::wstring(docPath) + L"\\Electronic Arts\\" + ResolveLocalizedGameFolder() + L"\\S3MM\\"; }
    }
    return s3mmDir;
}

std::string GetConfigPath() {
    return WideToUtf8(GetS3MMDirectory()) + "S3MM.toml";
}

std::string GetLogPath() {
    return WideToUtf8(GetS3MMDirectory()) + "S3MM.log";
}

bool EnsureDirectoryExists() {
    const std::wstring& dir = GetS3MMDirectory();
    if (dir.empty()) return false;

    std::error_code ec;
    fs::create_directories(dir, ec);
    return !ec && fs::exists(dir);
}

} // namespace ConfigPaths
} // namespace S3MM
