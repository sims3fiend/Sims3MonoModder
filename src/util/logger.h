#pragma once
#include <Windows.h>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>

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
        if (level < m_minLevel) return;
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

        m_file << std::put_time(&tm_buf, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count() << " [" << LevelToString(level) << "] " << message << std::endl;

        m_file.flush();
        LeaveCriticalSection(&m_cs);
    }

  private:
    Logger() = default;
    ~Logger() {
        Shutdown();
        if (m_csInitialized) { DeleteCriticalSection(&m_cs); }
    }

  public:
    // Minimum level gate. Release defaults to Info (DEBUG suppressed); opt back in via Config "Verbose".
    void SetMinLevel(Level level) { m_minLevel = level; }
    Level GetMinLevel() const { return m_minLevel; }

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
#ifdef NDEBUG
    Level m_minLevel = Level::Info; // Release: suppress DEBUG by default (Config "Verbose" re-enables)
#else
    Level m_minLevel = Level::Debug; // Debug builds: full verbosity
#endif
};

// LOGGING MACROS
#define LOG_DEBUG(msg) S3MM::Logger::Get().Log(S3MM::Logger::Level::Debug, msg)
#define LOG_INFO(msg) S3MM::Logger::Get().Log(S3MM::Logger::Level::Info, msg)
#define LOG_WARN(msg) S3MM::Logger::Get().Log(S3MM::Logger::Level::Warning, msg)
#define LOG_ERROR(msg) S3MM::Logger::Get().Log(S3MM::Logger::Level::Error, msg)

// HEX FORMATTING HELPER
inline std::string ToHex(uintptr_t value) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << value;
    return ss.str();
}

} // namespace S3MM
