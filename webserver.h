#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "http_connection.h"
#include "logger.h"
#include "mysql_connection_pool.h"
#include "thread_pool.h"
#include "user_repository.h"

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

// 服务器启动配置，由 main 解析命令行后统一传入。
struct ServerConfig {
  int port = 8080;                    // 监听端口。
  int trigger_mode = 0;               // 0~3，对应监听/连接的 LT、ET 组合。
  int linger = 0;                     // 是否启用 SO_LINGER。
  std::size_t thread_count = 8;       // 业务/事件工作线程数。
  int actor_model = 0;                // 0=Proactor，1=Reactor。
  int idle_timeout_seconds = 60;      // 空闲连接超时时间。
  LogConfig log;
  DatabaseConfig database;
};

class WebServer {
 public:
  WebServer();
  ~WebServer();

  // 保存配置并按并发模型创建所需线程池。
  bool init(const ServerConfig& config);

  // signal_fd 由 main 持有，服务器仅将其注册到 epoll，不负责关闭。
  void setSignalFd(int signal_fd);

  // 根据 trigger_mode 拆分监听 socket 和客户端 socket 的触发模式。
  void trig_mode();

  // 创建监听 socket、epoll，并注册监听 fd 与信号通知 fd。
  bool eventListen();

  // 运行事件循环，收到退出信号后返回。
  void eventLoop();

 private:
  struct ClientData {
    explicit ClientData(std::shared_ptr<UserRepository> repository)
        : connection(std::move(repository)) {}

    sockaddr_in address{};
    int sockfd{-1};
    HttpConnection connection;
    std::atomic<bool> closed{false};
    std::atomic<bool> busy{false};
    std::atomic<uint64_t> timer_generation{0};
  };

  using ClientPtr = std::shared_ptr<ClientData>;
  using Clock = std::chrono::steady_clock;

  enum class ClientAction {
    kWaitForRead,
    kWaitForWrite,
    kClose,
  };

  struct TimerEntry {
    Clock::time_point expires_at;
    std::weak_ptr<ClientData> client;
    uint64_t generation;
  };

  struct TimerCompare {
    bool operator()(const TimerEntry& left,
                    const TimerEntry& right) const {
      return left.expires_at > right.expires_at;
    }
  };

  bool dealclinetdata();
  bool acceptConnectionLT();
  bool acceptConnectionET();

  ClientPtr findClient(int sockfd);

  // 根据 actor_model 决定在事件线程还是工作线程中处理连接事件。
  bool dispatchClientEvent(const ClientPtr& client, uint32_t event);
  bool handleProactorEvent(const ClientPtr& client, uint32_t event);
  bool enqueueBusiness(const ClientPtr& client);
  void handleClientEvent(const ClientPtr& client, uint32_t event);
  ClientAction processClient(const ClientPtr& client);
  void finishClientAction(const ClientPtr& client, ClientAction action);
  ClientAction dealwithread(const ClientPtr& client);
  ClientAction dealwithwrite(const ClientPtr& client);

  void refreshTimer(const ClientPtr& client);
  void expireIdleConnections();
  int nextTimerTimeout();
  void closeClient(const ClientPtr& client);

  static int setNonblocking(int fd);
  static void addFd(int epollfd, int fd, bool one_shot, int trig_mode);
  static void removeFd(int epollfd, int fd);
  static void modFd(int epollfd, int fd, int event, int trig_mode);

  int port_;
  int trig_mode_;
  int listen_trig_mode_;
  int connect_trig_mode_;
  int linger_option_;
  std::size_t thread_count_;
  int actor_model_;
  std::chrono::milliseconds idle_timeout_;

  int listen_fd_;
  int epoll_fd_;
  // 自管道读端，用于让信号处理函数唤醒 epoll_wait。
  int signal_fd_;

  // 两种模型都使用线程池，区别在于网络 I/O 由哪一层执行。
  std::unique_ptr<ThreadPool> thread_pool_;
  std::shared_ptr<MysqlConnectionPool> database_pool_;
  std::shared_ptr<UserRepository> user_repository_;
  std::mutex clients_mutex_;
  std::map<int, ClientPtr> clients_;

  std::mutex timers_mutex_;
  std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerCompare>
      timers_;
};

#endif
