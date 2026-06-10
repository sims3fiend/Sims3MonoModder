#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <chrono>
#include <deque>

namespace S3MM {

// LOG LEVEL - Severity of a mod log entry. Drives coloring/filtering in the Mods panel
enum class LogLevel { Debug, Info, Warn, Error };

// MOD LOG ENTRY - Single log entry from a mod
struct ModLogEntry {
    std::chrono::system_clock::time_point timestamp;
    std::string source; // Mod name or "[System]"
    std::string message;
    LogLevel level = LogLevel::Info;
};

// MOD LOG - Thread-safe ring buffer for mod log output, displayed in Mods panel UI
class ModLog {
  public:
    static ModLog& Get() {
        static ModLog instance;
        return instance;
    }

    void Initialize() {
        if (m_initialized) return;
        InitializeCriticalSection(&m_cs);
        m_initialized = true;
    }

    void Shutdown() {
        if (!m_initialized) return;
        EnterCriticalSection(&m_cs);
        m_entries.clear();
        LeaveCriticalSection(&m_cs);
        DeleteCriticalSection(&m_cs);
        m_initialized = false;
    }

    // Add a log entry (thread-safe). Level defaults to Info
    void Log(const std::string& source, const std::string& message, LogLevel level = LogLevel::Info) {
        if (!m_initialized) return;

        EnterCriticalSection(&m_cs);

        ModLogEntry entry;
        entry.timestamp = std::chrono::system_clock::now();
        entry.source = source;
        entry.message = message;
        entry.level = level;

        m_entries.push_back(std::move(entry));

        // Trim to max size
        while (m_entries.size() > m_maxEntries) { m_entries.pop_front(); }

        m_version++;
        LeaveCriticalSection(&m_cs);
    }

    // Get a snapshot of entries (thread-safe)
    // Returns entries newer than the given version
    std::vector<ModLogEntry> GetEntries(size_t* outVersion = nullptr) {
        std::vector<ModLogEntry> result;
        if (!m_initialized) return result;

        EnterCriticalSection(&m_cs);
        result.reserve(m_entries.size());
        for (const auto& entry : m_entries) { result.push_back(entry); }
        if (outVersion) *outVersion = m_version;
        LeaveCriticalSection(&m_cs);

        return result;
    }

    // Get current version (for change detection)
    size_t GetVersion() const { return m_version; }

    // Clear all entries
    void Clear() {
        if (!m_initialized) return;
        EnterCriticalSection(&m_cs);
        m_entries.clear();
        m_version++;
        LeaveCriticalSection(&m_cs);
    }

  private:
    ModLog() = default;
    ~ModLog() { Shutdown(); }

    CRITICAL_SECTION m_cs;
    bool m_initialized = false;
    std::deque<ModLogEntry> m_entries;
    size_t m_maxEntries = 1000;
    size_t m_version = 0; // Incremented on each change
};

} // namespace S3MM
