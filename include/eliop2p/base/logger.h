#ifndef ELIOP2P_BASE_LOGGER_H
#define ELIOP2P_BASE_LOGGER_H

#include <elio/log/logger.hpp>
#include <string>
#include <memory>

namespace eliop2p {

// Alias for Elio's log level
using LogLevel = elio::log::level;

enum class LogOutput {
    Stdout,
    Stderr,
    File
};

class Logger {
public:
    ~Logger();

    static Logger& instance();

    // Level control
    void set_level(LogLevel level);
    elio::log::level get_level() const;

    // Output configuration
    void set_output(LogOutput output);
    void set_file_output(const std::string& path);
    void close_file_output();

    // Logging methods - wrapper around Elio logger with backward compatibility
    void log(LogLevel level, const std::string& message);

    template<typename... Args>
    void log(elio::log::level level, fmt::format_string<Args...> fmt_str, Args&&... args) {
        logger_.log(level, "", 0, fmt_str, std::forward<Args>(args)...);
    }

    void debug(const std::string& message);
    void info(const std::string& message);
    void warning(const std::string& message);
    void error(const std::string& message);
    void fatal(const std::string& message);

    // New style with format support
    template<typename... Args>
    void debug(fmt::format_string<Args...> fmt_str, Args&&... args) {
        logger_.log(elio::log::level::debug, "", 0, fmt_str, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(fmt::format_string<Args...> fmt_str, Args&&... args) {
        logger_.log(elio::log::level::info, "", 0, fmt_str, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warning(fmt::format_string<Args...> fmt_str, Args&&... args) {
        logger_.log(elio::log::level::warning, "", 0, fmt_str, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(fmt::format_string<Args...> fmt_str, Args&&... args) {
        logger_.log(elio::log::level::error, "", 0, fmt_str, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void fatal(fmt::format_string<Args...> fmt_str, Args&&... args) {
        logger_.log(elio::log::level::error, "", 0, fmt_str, std::forward<Args>(args)...);
    }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    elio::log::logger& logger_ = elio::log::logger::instance();
    elio::log::level level_ = elio::log::level::info;
    std::unique_ptr<std::ofstream> file_stream_;
};

} // namespace eliop2p

#endif // ELIOP2P_BASE_LOGGER_H
