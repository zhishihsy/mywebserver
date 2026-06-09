#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "http_connection.h"
#include "thread_pool.h"

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

class WebServer {
 public:
  WebServer();
  ~WebServer();

  // 保存服务器配置，此时不会创建 socket。
  void init(int port, int trig_mode, int linger_option,
            int idle_timeout_seconds = 60);

  // 根据组合模式设置监听 socket 和客户端 socket 的 LT/ET 模式。
  void trig_mode();

  // 创建监听 socket 和 epoll 实例，并注册监听事件。
  bool eventListen();

  // 持续等待并分发 epoll 事件。
  void eventLoop();

 private:
  // 保存一个客户端连接对应的地址和文件描述符。
  struct ClientData {
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

  // 根据监听 socket 的触发模式接收新连接。
  bool dealclinetdata();
  bool acceptConnectionLT();
  bool acceptConnectionET();

  // 将客户端事件交给线程池，并在工作线程中处理读写。
  ClientPtr findClient(int sockfd);
  bool dispatchClientEvent(const ClientPtr& client, uint32_t event);
  ClientAction dealwithread(const ClientPtr& client);
  ClientAction dealwithwrite(const ClientPtr& client);

  // 刷新连接期限，并清理已经到期的空闲连接。
  void refreshTimer(const ClientPtr& client);
  void expireIdleConnections();
  int nextTimerTimeout();

  // 从 epoll 和客户端表中移除连接，并关闭 socket。
  void closeClient(const ClientPtr& client);

  // epoll 文件描述符操作辅助函数。
  static int setNonblocking(int fd);
  static void addFd(int epollfd, int fd, bool one_shot, int trig_mode);
  static void removeFd(int epollfd, int fd);
  static void modFd(int epollfd, int fd, int event, int trig_mode);

  // 服务器配置。
  int port_;
  int trig_mode_;
  int listen_trig_mode_;
  int connect_trig_mode_;
  int linger_option_;
  std::chrono::milliseconds idle_timeout_;

  // 服务器运行期间持有的文件描述符。
  int listen_fd_;
  int epoll_fd_;

  // 以客户端 socket 为键保存连接信息。
  ThreadPool thread_pool_;
  std::mutex clients_mutex_;
  std::map<int, ClientPtr> clients_;

  std::mutex timers_mutex_;
  std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerCompare>
      timers_;
};

#endif
