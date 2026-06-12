#include "webserver.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
// 信号处理函数通过管道通知 epoll 主循环，避免在信号上下文中执行复杂清理。
volatile sig_atomic_t g_signal_write_fd = -1;

void handleSignal(int signal_number) {
  // write 是异步信号安全函数；保存 errno，避免影响被中断的正常代码。
  int saved_errno = errno;
  if (g_signal_write_fd >= 0) {
    unsigned char value = static_cast<unsigned char>(signal_number);
    ssize_t ignored = write(g_signal_write_fd, &value, sizeof(value));
    (void)ignored;
  }
  errno = saved_errno;
}

bool parseInteger(const char* text, int minimum, int maximum,
                  int* value) {
  if (!text || !value) {
    return false;
  }

  char* end = nullptr;
  errno = 0;
  long parsed = std::strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' ||
      parsed < minimum || parsed > maximum) {
    return false;
  }

  *value = static_cast<int>(parsed);
  return true;
}

bool loadDatabaseConfig(ServerConfig* config, std::string* error) {
  const char* enabled = std::getenv("MYSQL_ENABLED");
  if (!enabled || std::string(enabled) == "0") {
    config->database.enabled = false;
    config->database.pool_size = config->thread_count;
    return true;
  }
  if (std::string(enabled) != "1") {
    *error = "MYSQL_ENABLED 只能设置为 0 或 1";
    return false;
  }

  config->database.enabled = true;
  config->database.pool_size = config->thread_count;

  if (const char* value = std::getenv("MYSQL_HOST")) {
    config->database.host = value;
  }
  if (const char* value = std::getenv("MYSQL_USER")) {
    config->database.user = value;
  }
  if (const char* value = std::getenv("MYSQL_PASSWORD")) {
    config->database.password = value;
  }
  if (const char* value = std::getenv("MYSQL_DATABASE")) {
    config->database.database = value;
  }

  int parsed = 0;
  if (const char* value = std::getenv("MYSQL_PORT")) {
    if (!parseInteger(value, 1, 65535, &parsed)) {
      *error = "MYSQL_PORT 必须在 1-65535 范围内";
      return false;
    }
    config->database.port = static_cast<unsigned int>(parsed);
  }
  if (const char* value = std::getenv("MYSQL_POOL_SIZE")) {
    if (!parseInteger(value, 1, 1024, &parsed)) {
      *error = "MYSQL_POOL_SIZE 必须在 1-1024 范围内";
      return false;
    }
    config->database.pool_size = static_cast<std::size_t>(parsed);
  }

  if (config->database.user.empty()) {
    *error = "MYSQL_ENABLED=1 时必须设置 MYSQL_USER";
    return false;
  }
  if (config->database.database.empty()) {
    *error = "MYSQL_DATABASE 不能为空";
    return false;
  }
  return true;
}

void printUsage(const char* program) {
  std::cout
      << "Usage: " << program << " [options]\n"
      << "  -p PORT     Listen port (1-65535, default: 8080)\n"
      << "  -m MODE     Trigger mode: 0=LT/LT, 1=LT/ET, "
         "2=ET/LT, 3=ET/ET (default: 0)\n"
      << "  -o 0|1      Enable SO_LINGER (default: 0)\n"
      << "  -t THREADS  Worker threads (1-1024, default: 8)\n"
      << "  -a MODEL    0=Proactor, 1=Reactor (default: 0)\n"
      << "  -h          Show this help\n";
}

bool installSignalHandlers() {
  // SIGINT 对应 Ctrl+C，SIGTERM 用于服务管理程序请求正常退出。
  struct sigaction action {};
  sigemptyset(&action.sa_mask);
  action.sa_handler = handleSignal;
  action.sa_flags = 0;

  if (sigaction(SIGINT, &action, nullptr) < 0 ||
      sigaction(SIGTERM, &action, nullptr) < 0) {
    return false;
  }

  // 客户端提前断开时，send/write 不应让整个服务器因 SIGPIPE 退出。
  struct sigaction ignore {};
  sigemptyset(&ignore.sa_mask);
  ignore.sa_handler = SIG_IGN;
  return sigaction(SIGPIPE, &ignore, nullptr) == 0;
}
}  // namespace

int main(int argc, char* argv[]) {
  ServerConfig config;

  // 解析启动参数，并在进入服务器初始化前完成全部范围校验。
  int option = 0;
  while ((option = getopt(argc, argv, "p:m:o:t:a:h")) != -1) {
    int value = 0;
    switch (option) {
      case 'p':
        if (!parseInteger(optarg, 1, 65535, &config.port)) {
          std::cerr << "Invalid port: " << optarg << '\n';
          return 2;
        }
        break;
      case 'm':
        if (!parseInteger(optarg, 0, 3, &config.trigger_mode)) {
          std::cerr << "Invalid trigger mode: " << optarg << '\n';
          return 2;
        }
        break;
      case 'o':
        if (!parseInteger(optarg, 0, 1, &config.linger)) {
          std::cerr << "Invalid linger option: " << optarg << '\n';
          return 2;
        }
        break;
      case 't':
        if (!parseInteger(optarg, 1, 1024, &value)) {
          std::cerr << "Invalid thread count: " << optarg << '\n';
          return 2;
        }
        config.thread_count = static_cast<std::size_t>(value);
        break;
      case 'a':
        if (!parseInteger(optarg, 0, 1, &config.actor_model)) {
          std::cerr << "Invalid actor model: " << optarg << '\n';
          return 2;
        }
        break;
      case 'h':
        printUsage(argv[0]);
        return 0;
      default:
        printUsage(argv[0]);
        return 2;
    }
  }

  if (optind != argc) {
    std::cerr << "Unexpected argument: " << argv[optind] << '\n';
    printUsage(argv[0]);
    return 2;
  }

  std::string database_error;
  if (!loadDatabaseConfig(&config, &database_error)) {
    std::cerr << "数据库配置无效："
              << database_error << '\n';
    return 2;
  }

  // 管道读端交给 epoll，写端只由信号处理函数使用。
  int signal_pipe[2];
  if (pipe2(signal_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
    std::cerr << "Error: failed to create signal pipe\n";
    return 1;
  }

  g_signal_write_fd = signal_pipe[1];
  if (!installSignalHandlers()) {
    std::cerr << "Error: failed to install signal handlers\n";
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    return 1;
  }

  {
    WebServer server;

    // 配置、触发模式和信号 fd 准备完成后再创建监听 socket。
    if (!server.init(config)) {
      g_signal_write_fd = -1;
      close(signal_pipe[0]);
      close(signal_pipe[1]);
      return 1;
    }
    server.trig_mode();
    server.setSignalFd(signal_pipe[0]);

    if (!server.eventListen()) {
      g_signal_write_fd = -1;
      return 1;
    }

    server.eventLoop();
  }

  // WebServer 析构完成后，主线程再释放信号管道。
  g_signal_write_fd = -1;
  close(signal_pipe[0]);
  close(signal_pipe[1]);
  return 0;
}
