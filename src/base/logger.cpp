#include "eliop2p/base/logger.h"
#include <fmt/core.h>

namespace eliop2p {

Logger::~Logger() {
    close_file_output();
}

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::set_level(LogLevel level) {
    level_ = level;
    logger_.set_level(level);
}

elio::log::level Logger::get_level() const {
    return logger_.get_level();
}

void Logger::set_output(LogOutput output) {
    // Note: Elio logger outputs to stderr by default
    // File output is handled separately if needed
    (void)output;
}

void Logger::set_file_output(const std::string& path) {
    close_file_output();
    file_stream_ = std::make_unique<std::ofstream>(path, std::ios::app);
}

void Logger::close_file_output() {
    if (file_stream_ && file_stream_->is_open()) {
        file_stream_->close();
    }
    file_stream_.reset();
}

void Logger::log(LogLevel level, const std::string& message) {
    logger_.log(level, "", 0, "{}", message);
}

void Logger::debug(const std::string& message) {
    logger_.log(elio::log::level::debug, "", 0, "{}", message);
}

void Logger::info(const std::string& message) {
    logger_.log(elio::log::level::info, "", 0, "{}", message);
}

void Logger::warning(const std::string& message) {
    logger_.log(elio::log::level::warning, "", 0, "{}", message);
}

void Logger::error(const std::string& message) {
    logger_.log(elio::log::level::error, "", 0, "{}", message);
}

void Logger::fatal(const std::string& message) {
    logger_.log(elio::log::level::error, "", 0, "FATAL: {}", message);
}

} // namespace eliop2p
