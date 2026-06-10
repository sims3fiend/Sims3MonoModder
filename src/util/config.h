#pragma once
#include <string>
#include <mutex>
#include <Windows.h>

namespace S3MM {

// CONFIG MANAGER - Persistent settings for S3MM
class Config {
  public:
    static Config& Get();

    // Initialize - call early, loads from TOML
    void Initialize();
    void Save();

    // UI Toggle Key
    UINT GetToggleKey() const;
    void SetToggleKey(UINT key);

    // Theme (stored by name)
    std::string GetTheme() const;
    void SetTheme(const std::string& theme);

    // Font size: 0 = Auto (derived from theme spacing), >0 = explicit px
    int GetFontSize() const;
    void SetFontSize(int size);

    // Verbose logging: when true, lowers the log level to Debug (Release defaults to Info)
    bool IsVerbose() const;
    void SetVerbose(bool verbose);

    // Key name helper for display
    static std::string GetKeyName(UINT vkCode);

    // Get list of common keys for UI dropdown
    struct KeyOption {
        UINT vkCode;
        const char* name;
    };
    static const KeyOption* GetKeyOptions(int* outCount);

  private:
    Config() = default;
    ~Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    mutable std::mutex m_mutex;
    UINT m_toggleKey = VK_HOME;            // Default to HOME (S3SS uses insert)
    std::string m_theme = "Visual Studio"; // Default theme
    int m_fontSize = 13;                   // Explicit px (native = 13)
    bool m_verbose = false;                // Verbose (Debug-level) logging; off by default
    bool m_initialized = false;
};

} // namespace S3MM
