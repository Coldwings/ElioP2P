#ifndef ELIOP2P_BASE_LOGGER_H
#define ELIOP2P_BASE_LOGGER_H

#include <string>
#include <sstream>
#include <memory>
#include <mutex>
#include <fstream>
#include <vector>

namespace eliop2p {

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

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
    LogLevel get_level() const;

    // Output configuration
    void set_output(LogOutput output);
    void set_file_output(const std::string& path);
    void close_file_output();

    // Logging methods
    void log(LogLevel level, const std::string& message);

    void debug(const std::string& message);
    void info(const std::string& message);
    void warning(const std::string& message);
    void error(const std::string& message);
    void fatal(const std::string& message);

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void write_log(LogLevel level, const std::string& message);

    LogLevel level_ = LogLevel::INFO;
    LogOutput output_ = LogOutput::Stdout;
    std::unique_ptr<std::ofstream> file_stream_;
    mutable std::mutex mutex_;
};

} // namespace eliop2p

#endif // ELIOP2P_BASE_LOGGER_H
