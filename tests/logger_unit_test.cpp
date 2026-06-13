#include "../logger.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
std::vector<std::filesystem::path> matchingFiles(
    const std::filesystem::path& directory,
    const std::string& prefix) {
  std::vector<std::filesystem::path> files;
  for (const auto& entry :
       std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().find(prefix) == 0) {
      files.push_back(entry.path());
    }
  }
  return files;
}

std::vector<std::string> readLines(
    const std::filesystem::path& path) {
  std::ifstream input(path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    lines.push_back(line);
  }
  return lines;
}
}  // namespace

int main() {
  namespace fs = std::filesystem;
  fs::path directory =
      fs::temp_directory_path() / "mywebserver_logger_test";
  fs::remove_all(directory);
  fs::create_directories(directory);

  LogConfig async_config;
  async_config.asynchronous = true;
  async_config.level = LogLevel::kDebug;
  async_config.file = (directory / "async.log").string();
  async_config.queue_size = 2;
  async_config.max_lines = 3;

  std::string error;
  assert(Logger::instance().initialize(async_config, &error));
  for (int i = 0; i < 10; ++i) {
    LOG_DEBUG("debug message ", i);
  }
  LOG_INFO("info message");
  Logger::instance().shutdown();

  auto async_files = matchingFiles(directory, "async_");
  assert(async_files.size() == 4);
  std::size_t async_line_count = 0;
  for (const auto& path : async_files) {
    auto lines = readLines(path);
    assert(!lines.empty() && lines.size() <= 3);
    async_line_count += lines.size();
  }
  assert(async_line_count == 11);

  LogConfig filtered_config;
  filtered_config.level = LogLevel::kWarn;
  filtered_config.file = (directory / "filtered.log").string();
  assert(Logger::instance().initialize(filtered_config, &error));
  LOG_DEBUG("hidden debug");
  LOG_INFO("hidden info");
  LOG_WARN("visible warning");
  LOG_ERROR("visible error");
  Logger::instance().shutdown();

  auto filtered_files = matchingFiles(directory, "filtered_");
  assert(filtered_files.size() == 1);
  auto filtered_lines = readLines(filtered_files.front());
  assert(filtered_lines.size() == 2);
  assert(filtered_lines[0].find("[WARN]") != std::string::npos);
  assert(filtered_lines[1].find("[ERROR]") != std::string::npos);

  LogConfig disabled_config;
  disabled_config.enabled = false;
  disabled_config.file = (directory / "disabled.log").string();
  assert(Logger::instance().initialize(disabled_config, &error));
  LOG_ERROR("hidden while disabled");
  Logger::instance().shutdown();
  assert(matchingFiles(directory, "disabled_").empty());

  fs::remove_all(directory);
  return 0;
}
