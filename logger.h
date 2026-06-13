#ifndef LOGGER_H
#define LOGGER_H

#include "blocking_queue.h"

#include <atomic>
#include <cstddef>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

enum class LogLevel {
  kDebug = 0,
  kInfo = 1,
  kWarn = 2,
  kError = 3,
};

struct LogConfig {
  bool enabled = true;
  bool asynchronous = false;
  LogLevel level = LogLevel::kInfo;
  std::string file = "server.log";
  std::size_t queue_size = 8192;
  std::size_t max_lines = 50000;
};

class Logger {
 public:
  static Logger& instance();

  bool initialize(const LogConfig& config, std::string* error);
  void shutdown();

  template <typename... Args>
  void log(LogLevel level, Args&&... args) {
    if (!shouldLog(level)) {
      return;
    }
    std::ostringstream message;
    (message << ... << std::forward<Args>(args));
    submit(formatLine(level, message.str()));
  }

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

 private:
  Logger();
  ~Logger();

  bool shouldLog(LogLevel level) const;
  std::string formatLine(LogLevel level,
                         const std::string& message) const;
  void submit(std::string line);
  void workerLoop();
  bool writeLine(const std::string& line);
  bool openLogFile(const std::string& date, std::string* error);
  std::string buildLogPath(const std::string& date,
                           std::size_t index) const;
  static std::string currentDate();
  static const char* levelName(LogLevel level);

  LogConfig config_;
  std::atomic<bool> enabled_;
  std::atomic<bool> initialized_;
  std::unique_ptr<BlockingQueue<std::string>> queue_;
  std::thread worker_;
  std::mutex file_mutex_;
  std::ofstream output_;
  std::string current_date_;
  std::size_t current_index_;
  std::size_t current_lines_;
};

#define LOG_DEBUG(...) \
  Logger::instance().log(LogLevel::kDebug, __VA_ARGS__)
#define LOG_INFO(...) \
  Logger::instance().log(LogLevel::kInfo, __VA_ARGS__)
#define LOG_WARN(...) \
  Logger::instance().log(LogLevel::kWarn, __VA_ARGS__)
#define LOG_ERROR(...) \
  Logger::instance().log(LogLevel::kError, __VA_ARGS__)

#endif
