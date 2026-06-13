#include "logger.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>

namespace {
std::tm toLocalTime(std::time_t time) {
  std::tm local_time {};
#ifdef _WIN32
  localtime_s(&local_time, &time);
#else
  localtime_r(&time, &local_time);
#endif
  return local_time;
}
}  // namespace

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

Logger::Logger()
    : enabled_(false),
      initialized_(false),
      current_index_(0),
      current_lines_(0) {}

Logger::~Logger() {
  shutdown();
}

bool Logger::initialize(const LogConfig& config, std::string* error) {
  shutdown();
  config_ = config;
  if (!config_.enabled) {
    initialized_.store(true);
    return true;
  }
  if (config_.file.empty()) {
    if (error) {
      *error = "LOG_FILE cannot be empty";
    }
    return false;
  }
  if (config_.max_lines == 0) {
    if (error) {
      *error = "LOG_MAX_LINES must be greater than zero";
    }
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (!openLogFile(currentDate(), error)) {
      return false;
    }
  }

  enabled_.store(true);
  initialized_.store(true);
  if (config_.asynchronous) {
    queue_ = std::make_unique<BlockingQueue<std::string>>(
        config_.queue_size);
    worker_ = std::thread([this] { workerLoop(); });
  }
  return true;
}

void Logger::shutdown() {
  enabled_.store(false);
  if (queue_) {
    queue_->close();
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  queue_.reset();

  std::lock_guard<std::mutex> lock(file_mutex_);
  if (output_.is_open()) {
    output_.flush();
    output_.close();
  }
  current_date_.clear();
  current_index_ = 0;
  current_lines_ = 0;
  initialized_.store(false);
}

bool Logger::shouldLog(LogLevel level) const {
  return initialized_.load() && enabled_.load() &&
         static_cast<int>(level) >= static_cast<int>(config_.level);
}

std::string Logger::formatLine(LogLevel level,
                               const std::string& message) const {
  auto now = std::chrono::system_clock::now();
  std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time = toLocalTime(time);
  auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch()) %
      1000;

  std::ostringstream line;
  line << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3)
       << milliseconds.count() << " [" << levelName(level)
       << "] [tid " << std::this_thread::get_id() << "] "
       << message << '\n';
  return line.str();
}

void Logger::submit(std::string line) {
  if (config_.asynchronous && queue_) {
    queue_->push(std::move(line));
    return;
  }
  writeLine(line);
}

void Logger::workerLoop() {
  std::string line;
  while (queue_ && queue_->pop(&line)) {
    writeLine(line);
  }
}

bool Logger::writeLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(file_mutex_);
  const std::string date = currentDate();
  if (date != current_date_ || current_lines_ >= config_.max_lines) {
    std::string error;
    if (!openLogFile(date, &error)) {
      std::cerr << "Log rotation failed: " << error << '\n';
      return false;
    }
  }

  output_ << line;
  output_.flush();
  if (!output_) {
    std::cerr << "Failed to write log file\n";
    return false;
  }
  ++current_lines_;
  return true;
}

bool Logger::openLogFile(const std::string& date, std::string* error) {
  namespace fs = std::filesystem;
  if (output_.is_open()) {
    output_.flush();
    output_.close();
  }

  fs::path configured(config_.file);
  fs::path parent = configured.parent_path();
  std::error_code filesystem_error;
  if (!parent.empty()) {
    fs::create_directories(parent, filesystem_error);
    if (filesystem_error) {
      if (error) {
        *error = "cannot create log directory: " +
                 filesystem_error.message();
      }
      return false;
    }
  }

  if (date != current_date_) {
    current_index_ = 0;
  }
  current_date_ = date;

  while (true) {
    const std::string path = buildLogPath(date, current_index_);
    current_lines_ = 0;
    std::ifstream existing(path);
    std::string ignored;
    while (std::getline(existing, ignored)) {
      ++current_lines_;
    }
    if (current_lines_ < config_.max_lines) {
      output_.open(path, std::ios::out | std::ios::app);
      if (!output_) {
        if (error) {
          *error = "cannot open " + path;
        }
        return false;
      }
      return true;
    }
    ++current_index_;
  }
}

std::string Logger::buildLogPath(const std::string& date,
                                 std::size_t index) const {
  namespace fs = std::filesystem;
  fs::path configured(config_.file);
  std::ostringstream filename;
  filename << configured.stem().string() << '_' << date;
  if (index > 0) {
    filename << '.' << index;
  }
  filename << configured.extension().string();
  return (configured.parent_path() / filename.str()).string();
}

std::string Logger::currentDate() {
  std::time_t now = std::time(nullptr);
  std::tm local_time = toLocalTime(now);
  std::ostringstream date;
  date << std::put_time(&local_time, "%Y-%m-%d");
  return date.str();
}

const char* Logger::levelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarn:
      return "WARN";
    case LogLevel::kError:
      return "ERROR";
  }
  return "UNKNOWN";
}
