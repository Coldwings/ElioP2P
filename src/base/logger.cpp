#include "eliop2p/base/logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <algorithm>

namespace eliop2p {

namespace {

const char* level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARNING: return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

} // anonymous namespace

Logger::~Logger() {
    close_file_output();
}

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel Logger::get_level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

void Logger::set_output(LogOutput output) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ = output;
}

void Logger::set_file_output(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    close_file_output();
    file_stream_ = std::make_unique<std::ofstream>(path, std::ios::app);
    if (!file_stream_->is_open()) {
        std::cerr << "Failed to open log file: " << path << std::endl;
        file_stream_.reset();
    } else {
        output_ = LogOutput::File;
    }
}

void Logger::close_file_output() {
    if (file_stream_ && file_stream_->is_open()) {
        file_stream_->close();
    }
    file_stream_.reset();
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < level_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    write_log(level, message);
}

void Logger::write_log(LogLevel level, const std::string& message) {
    std::ostringstream oss;
    oss << "[" << get_timestamp() << "] [" << level_to_string(level) << "] " << message;
    std::string formatted = oss.str();

    switch (output_) {
        case LogOutput::Stdout:
            std::cout << formatted << std::endl;
            break;
        case LogOutput::Stderr:
            std::cerr << formatted << std::endl;
            break;
        case LogOutput::File:
            if (file_stream_ && file_stream_->is_open()) {
                *file_stream_ << formatted << std::endl;
                file_stream_->flush();
            }
            break;
    }
}

void Logger::debug(const std::string& message) {
    log(LogLevel::DEBUG, message);
}

void Logger::info(const std::string& message) {
    log(LogLevel::INFO, message);
}

void Logger::warning(const std::string& message) {
    log(LogLevel::WARNING, message);
}

void Logger::error(const std::string& message) {
    log(LogLevel::ERROR, message);
}

void Logger::fatal(const std::string& message) {
    log(LogLevel::FATAL, message);
}

} // namespace eliop2p
