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

    // Hook invocation logging: per-invocation HookManager trace lines (hot paths). Off by default, independent of Verbose.
    bool IsLogHookInvocations() const;
    void SetLogHookInvocations(bool enabled);

    // Persist bridge (modding/persist_bridge.h): register [PersistableStatic] fields of S3MM-loaded mods (vanilla scan runs before mods load). Default true
    bool IsPersistAutoRegister() const;

    // Mod UI toast area (ui/mod_ui.cpp): position as fractions of the display (negative = default, top-right), background alpha 0.1..1
    void GetToastLayout(float& posX, float& posY, float& alpha) const;
    void SetToastLayout(float posX, float posY, float alpha);

    // "S3MM loaded" toast after mod processing (dllmain.cpp OnMonoRuntimeReady). Default true
    bool IsStartupToast() const;

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
    bool m_logHookInvocations = false;     // Per-invocation hook invocation logging; off by default
    bool m_persistAutoRegister = true;     // Late [PersistableStatic] registration for S3MM mods
    bool m_startupToast = true;            // Show the "S3MM loaded" toast once mods are processed
    float m_toastPosX = -1.0f;             // Toast area top-left, fraction of display width (-1 = default)
    float m_toastPosY = -1.0f;             // Toast area top-left, fraction of display height (-1 = default)
    float m_toastAlpha = 0.65f;            // Toast background alpha
    bool m_initialized = false;
};

} // namespace S3MM
