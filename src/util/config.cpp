#include "config.h"
#include "config_paths.h"
#include "logger.h"
#include <fstream>
#include <toml++/toml.hpp>

namespace S3MM {

// Common keys for the dropdown
static const Config::KeyOption s_keyOptions[] = {
    {VK_HOME, "Home"},
    {VK_INSERT, "Insert"},
    {VK_END, "End"},
    {VK_DELETE, "Delete"},
    {VK_PRIOR, "Page Up"},
    {VK_NEXT, "Page Down"},
    {VK_F1, "F1"},
    {VK_F2, "F2"},
    {VK_F3, "F3"},
    {VK_F4, "F4"},
    {VK_F5, "F5"},
    {VK_F6, "F6"},
    {VK_F7, "F7"},
    {VK_F8, "F8"},
    {VK_F10, "F10"},
    {VK_F11, "F11"},
    {VK_F12, "F12"},
    {VK_PAUSE, "Pause"},
    {VK_SCROLL, "Scroll Lock"},
    {VK_OEM_3, "~ (Tilde)"},
};
static const int s_keyOptionsCount = sizeof(s_keyOptions) / sizeof(s_keyOptions[0]);

Config& Config::Get() {
    static Config instance;
    return instance;
}

void Config::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return;

    std::string path = ConfigPaths::GetConfigPath();
    LOG_INFO("[Config] Loading from: " + path);

    try {
        auto tbl = toml::parse_file(path);

        if (auto v = tbl["ToggleKey"].value<int64_t>()) {
            m_toggleKey = static_cast<UINT>(*v);
            LOG_INFO("[Config] ToggleKey = " + std::to_string(m_toggleKey) + " (" + GetKeyName(m_toggleKey) + ")");
        }
        if (auto v = tbl["Theme"].value<std::string>()) {
            m_theme = *v;
            LOG_INFO("[Config] Theme = " + m_theme);
        } else if (auto v = tbl["Theme"].value<int64_t>()) {
            // Legacy: migrate old integer index to name
            LOG_INFO("[Config] Migrating legacy Theme index " + std::to_string(*v) + " -> keeping default name");
        }
        if (auto v = tbl["FontSize"].value<int64_t>()) {
            m_fontSize = static_cast<int>(*v);
            if (m_fontSize <= 0) m_fontSize = 13; // Migrate old "Auto" setting
            LOG_INFO("[Config] FontSize = " + std::to_string(m_fontSize));
        }
        if (auto v = tbl["Verbose"].value<bool>()) {
            m_verbose = *v;
            LOG_INFO("[Config] Verbose = " + std::string(m_verbose ? "true" : "false"));
        }
        if (auto v = tbl["LogHookInvocations"].value<bool>()) {
            m_logHookInvocations = *v;
            LOG_INFO("[Config] LogHookInvocations = " + std::string(m_logHookInvocations ? "true" : "false"));
        }
        if (auto v = tbl["PersistAutoRegister"].value<bool>()) {
            m_persistAutoRegister = *v;
            LOG_INFO("[Config] PersistAutoRegister = " + std::string(m_persistAutoRegister ? "true" : "false"));
        }
    } catch (const toml::parse_error& err) { LOG_INFO("[Config] Could not parse config (" + std::string(err.what()) + "), using defaults"); }

    // Apply verbose flag to logger. Note: config loads after Logger::Initialize, so the earliest startup lines (address resolution, image hooks) already emitted at the build default
    if (m_verbose) { Logger::Get().SetMinLevel(Logger::Level::Debug); }
    Logger::Get().SetLogHookInvocations(m_logHookInvocations);

    m_initialized = true;
    LOG_INFO("[Config] Loaded successfully");
}

void Config::Save() {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string path = ConfigPaths::GetConfigPath();
    LOG_INFO("[Config] Saving to: " + path);

    std::ofstream file(path);
    if (!file.is_open()) {
        LOG_ERROR("[Config] Failed to open config file for writing");
        return;
    }

    toml::table tbl;
    tbl.insert("ToggleKey", static_cast<int64_t>(m_toggleKey));
    tbl.insert("Theme", m_theme);
    tbl.insert("FontSize", static_cast<int64_t>(m_fontSize));
    tbl.insert("Verbose", m_verbose);
    tbl.insert("LogHookInvocations", m_logHookInvocations);
    tbl.insert("PersistAutoRegister", m_persistAutoRegister);
    file << tbl;

    LOG_INFO("[Config] Saved successfully");
}

UINT Config::GetToggleKey() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_toggleKey;
}

void Config::SetToggleKey(UINT key) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_toggleKey == key) return;
        m_toggleKey = key;
    }
    LOG_INFO("[Config] Toggle key changed to: " + GetKeyName(key));
    Save();
}

std::string Config::GetTheme() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_theme;
}

void Config::SetTheme(const std::string& theme) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_theme == theme) return;
        m_theme = theme;
    }
    LOG_INFO("[Config] Theme changed to: " + theme);
    Save();
}

int Config::GetFontSize() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_fontSize;
}

void Config::SetFontSize(int size) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fontSize == size) return;
        m_fontSize = size;
    }
    LOG_INFO("[Config] Font size changed to: " + std::to_string(size));
    Save();
}

bool Config::IsVerbose() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_verbose;
}

void Config::SetVerbose(bool verbose) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_verbose == verbose) return;
        m_verbose = verbose;
    }
    // Explicit runtime toggle. on -> Debug, off -> Info (applies regardless of build type).
    Logger::Get().SetMinLevel(verbose ? Logger::Level::Debug : Logger::Level::Info);
    LOG_INFO("[Config] Verbose logging " + std::string(verbose ? "enabled" : "disabled"));
    Save();
}

bool Config::IsPersistAutoRegister() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_persistAutoRegister;
}

bool Config::IsLogHookInvocations() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_logHookInvocations;
}

void Config::SetLogHookInvocations(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_logHookInvocations == enabled) return;
        m_logHookInvocations = enabled;
    }
    Logger::Get().SetLogHookInvocations(enabled);
    LOG_INFO("[Config] Hook invocation logging " + std::string(enabled ? "enabled" : "disabled"));
    Save();
}

std::string Config::GetKeyName(UINT vkCode) {
    // Check our known keys first
    for (int i = 0; i < s_keyOptionsCount; i++) {
        if (s_keyOptions[i].vkCode == vkCode) { return s_keyOptions[i].name; }
    }

    // Letter keys
    if (vkCode >= 'A' && vkCode <= 'Z') { return std::string(1, static_cast<char>(vkCode)); }

    // Number keys
    if (vkCode >= '0' && vkCode <= '9') { return std::string(1, static_cast<char>(vkCode)); }

    // Numpad
    if (vkCode >= VK_NUMPAD0 && vkCode <= VK_NUMPAD9) { return "Numpad " + std::to_string(vkCode - VK_NUMPAD0); }

    // Default
    return "Key " + std::to_string(vkCode);
}

const Config::KeyOption* Config::GetKeyOptions(int* outCount) {
    if (outCount) *outCount = s_keyOptionsCount;
    return s_keyOptions;
}

} // namespace S3MM
