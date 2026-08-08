#pragma once
#include <Windows.h>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <atomic>

namespace S3MM {

// LOGGER - Thread-safe file logger, uses CRITICAL_SECTION to avoid std::mutex static init issues
class Logger {
  public:
    enum class Level { Debug, Info, Warning, Error };

    static Logger& Get() {
        static Logger instance;
        return instance;
    }

    void Initialize(const std::string& filename = "S3MM.log") {
        if (m_initialized) return;

        // Initialize critical section on first use
        if (!m_csInitialized) {
            InitializeCriticalSection(&m_cs);
            m_csInitialized = true;
        }

        EnterCriticalSection(&m_cs);

        m_file.open(filename, std::ios::out | std::ios::trunc);
        m_initialized = m_file.is_open();

        if (m_initialized) { Log(Level::Info, "=== Sims3MonoModder Log Started ==="); }

        LeaveCriticalSection(&m_cs);
    }

    void Shutdown() {
        if (!m_csInitialized) return;

        EnterCriticalSection(&m_cs);
        if (m_file.is_open()) { m_file.close(); }
        m_initialized = false;
        LeaveCriticalSection(&m_cs);
    }

    void Log(Level level, const std::string& message) {
        if (level < m_minLevel.load(std::memory_order_relaxed)) return;
        Write(level, message);
    }

    // Per-invocation hook invocation logging, gated on the LogHookInvocations config flag alone (independent of the min level, so it works without full Verbose)
    void LogHookTrace(const std::string& message) {
        if (!m_logHookInvocations.load(std::memory_order_relaxed)) return;
        Write(Level::Debug, message);
    }

    // Raw data channel for mods (S3MM.Log.Raw): verbatim write, no timestamp/tag, no level gate, no trimming
    void LogRaw(const std::string& message) {
        if (!m_initialized || !m_csInitialized) return;

        EnterCriticalSection(&m_cs);
        if (!m_initialized) {
            LeaveCriticalSection(&m_cs);
            return;
        }

        m_file << message << std::endl;
        LeaveCriticalSection(&m_cs);
    }

  private:
    void Write(Level level, const std::string& message) {
        if (!m_initialized || !m_csInitialized) return;

        EnterCriticalSection(&m_cs);
        if (!m_initialized) {
            LeaveCriticalSection(&m_cs);
            return;
        }

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm_buf;
        localtime_s(&tm_buf, &time);

        // std::endl flushes; do NOT add a second flush here (a double flush per line was murder on hot paths)
        m_file << std::put_time(&tm_buf, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count() << " [" << LevelToString(level) << "] " << message << std::endl;
        LeaveCriticalSection(&m_cs);
    }

    Logger() = default;
    ~Logger() {
        Shutdown();
        if (m_csInitialized) { DeleteCriticalSection(&m_cs); }
    }

  public:
    // Minimum level gate. Release defaults to Info (DEBUG suppressed); opt back in via Config "Verbose".
    // Atomic: read on every log call from any thread, written from the UI/config thread.
    void SetMinLevel(Level level) { m_minLevel.store(level, std::memory_order_relaxed); }
    Level GetMinLevel() const { return m_minLevel.load(std::memory_order_relaxed); }
    bool IsLevelEnabled(Level level) const { return level >= m_minLevel.load(std::memory_order_relaxed); }

    // Per-invocation hook invocation logging gate (Config "LogHookInvocations", default off)
    void SetLogHookInvocations(bool enabled) { m_logHookInvocations.store(enabled, std::memory_order_relaxed); }
    bool IsLogHookInvocationsEnabled() const { return m_logHookInvocations.load(std::memory_order_relaxed); }

  private:
    static const char* LevelToString(Level level) {
        switch (level) {
        case Level::Debug:
            return "DEBUG";
        case Level::Info:
            return "INFO";
        case Level::Warning:
            return "WARN";
        case Level::Error:
            return "ERROR";
        default:
            return "?";
        }
    }

    std::ofstream m_file;
    CRITICAL_SECTION m_cs;
    bool m_csInitialized = false;
    bool m_initialized = false;
    std::atomic<bool> m_logHookInvocations{false}; // Config "LogHookInvocations" - per-invocation hook invocation logging, off by default
#ifdef NDEBUG
    std::atomic<Level> m_minLevel{Level::Info}; // Release: suppress DEBUG by default (Config "Verbose" re-enables)
#else
    std::atomic<Level> m_minLevel{Level::Debug}; // Debug builds: full verbosity
#endif
};

// LOGGING MACROS - level-checked before the message expression is evaluated, so suppressed logging never builds the string
#define LOG_DEBUG(msg)                                                           \
    do {                                                                         \
        if (S3MM::Logger::Get().IsLevelEnabled(S3MM::Logger::Level::Debug)) { S3MM::Logger::Get().Log(S3MM::Logger::Level::Debug, msg); } \
    } while (0)
#define LOG_INFO(msg)                                                           \
    do {                                                                        \
        if (S3MM::Logger::Get().IsLevelEnabled(S3MM::Logger::Level::Info)) { S3MM::Logger::Get().Log(S3MM::Logger::Level::Info, msg); } \
    } while (0)
#define LOG_WARN(msg)                                                              \
    do {                                                                           \
        if (S3MM::Logger::Get().IsLevelEnabled(S3MM::Logger::Level::Warning)) { S3MM::Logger::Get().Log(S3MM::Logger::Level::Warning, msg); } \
    } while (0)
#define LOG_ERROR(msg)                                                           \
    do {                                                                         \
        if (S3MM::Logger::Get().IsLevelEnabled(S3MM::Logger::Level::Error)) { S3MM::Logger::Get().Log(S3MM::Logger::Level::Error, msg); } \
    } while (0)

// Per-invocation hook invocation logging (hot paths in HookManager). Gated on Config "LogHookInvocations" alone, emits at DEBUG level.
#define LOG_HOOK_TRACE(msg)                                       \
    do {                                                          \
        if (S3MM::Logger::Get().IsLogHookInvocationsEnabled()) { S3MM::Logger::Get().LogHookTrace(msg); } \
    } while (0)

// HEX FORMATTING HELPER
inline std::string ToHex(uintptr_t value) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << value;
    return ss.str();
}

} // namespace S3MM
