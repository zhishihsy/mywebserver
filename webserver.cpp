#include "webserver.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <iostream>

namespace {
constexpr int kMaxEventNumber = 10000;
constexpr int kListenBacklog = SOMAXCONN;
}  // 匿名命名空间

WebServer::WebServer()
    : port_(8080),
      trig_mode_(0),
      listen_trig_mode_(0),
      connect_trig_mode_(0),
      linger_option_(0),
      thread_count_(8),
      actor_model_(0),
      idle_timeout_(std::chrono::seconds(60)),
      listen_fd_(-1),
      epoll_fd_(-1),
      signal_fd_(-1) {}

WebServer::~WebServer() {
  // 先等待工作任务结束，避免线程继续访问随后释放的连接和 epoll。
  if (thread_pool_) {
    thread_pool_->shutdown();
  }
  if (database_pool_) {
    database_pool_->shutdown();
  }

  // 先关闭所有客户端，再释放监听 socket 和 epoll。
  while (true) {
    ClientPtr client;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      if (clients_.empty()) {
        break;
      }
      client = clients_.begin()->second;
    }
    closeClient(client);
  }

  if (listen_fd_ >= 0) {
    close(listen_fd_);
  }

  if (epoll_fd_ >= 0) {
    close(epoll_fd_);
  }
}

bool WebServer::init(const ServerConfig& config) {
  port_ = config.port;
  trig_mode_ = config.trigger_mode;
  linger_option_ = config.linger;
  thread_count_ = config.thread_count;
  actor_model_ = config.actor_model;
  idle_timeout_ =
      std::chrono::seconds(std::max(1, config.idle_timeout_seconds));

  database_pool_ = std::make_shared<MysqlConnectionPool>();
  std::string database_error;
  if (!database_pool_->initialize(config.database, &database_error)) {
    LOG_ERROR("Database initialization failed: ", database_error);
    std::cerr << "错误：数据库初始化失败："
              << database_error << '\n';
    return false;
  }
  user_repository_ =
      std::make_shared<UserRepository>(database_pool_);

  // 先初始化数据库客户端库和连接，再启动工作线程。
  thread_pool_ = std::make_unique<ThreadPool>(thread_count_);
  return true;
}

void WebServer::setSignalFd(int signal_fd) {
  signal_fd_ = signal_fd;
}

void WebServer::trig_mode() {
  // 组合模式：
  // 0 = 监听 LT，客户端 LT
  // 1 = 监听 LT，客户端 ET
  // 2 = 监听 ET，客户端 LT
  // 3 = 监听 ET，客户端 ET
  switch (trig_mode_) {
    case 0:
      listen_trig_mode_ = 0;
      connect_trig_mode_ = 0;
      break;
    case 1:
      listen_trig_mode_ = 0;
      connect_trig_mode_ = 1;
      break;
    case 2:
      listen_trig_mode_ = 1;
      connect_trig_mode_ = 0;
      break;
    case 3:
      listen_trig_mode_ = 1;
      connect_trig_mode_ = 1;
      break;
    default:
      LOG_WARN("Invalid trigger mode ", trig_mode_,
               ", falling back to LT + LT");
      std::cerr << "Invalid trigger mode, using LT + LT" << std::endl;
      trig_mode_ = 0;
      listen_trig_mode_ = 0;
      connect_trig_mode_ = 0;
      break;
  }
}

bool WebServer::eventListen() {
  // 创建 TCP 监听 socket。
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    LOG_ERROR("Failed to create listen socket, errno=", errno);
    std::cerr << "Error: failed to create listen socket" << std::endl;
    return false;
  }

  // 允许服务器重启后快速复用处于 TIME_WAIT 状态的地址。
  int reuse_address = 1;
  if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                 sizeof(reuse_address)) < 0) {
    LOG_WARN("SO_REUSEADDR failed, errno=", errno);
    std::cerr << "Warning: SO_REUSEADDR failed" << std::endl;
  }

  // linger_option_ 为 1 时启用 SO_LINGER，最多等待 1 秒关闭连接。
  linger linger_option{linger_option_, 1};
  if (setsockopt(listen_fd_, SOL_SOCKET, SO_LINGER, &linger_option,
                 sizeof(linger_option)) < 0) {
    LOG_WARN("SO_LINGER failed, errno=", errno);
    std::cerr << "Warning: SO_LINGER failed" << std::endl;
  }

  sockaddr_in server_address{};
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = htonl(INADDR_ANY);
  server_address.sin_port = htons(port_);

  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&server_address),
           sizeof(server_address)) < 0) {
    LOG_ERROR("Bind failed on port ", port_, ", errno=", errno);
    std::cerr << "Error: bind failed" << std::endl;
    return false;
  }

  if (listen(listen_fd_, kListenBacklog) < 0) {
    LOG_ERROR("Listen failed, errno=", errno);
    std::cerr << "Error: listen failed" << std::endl;
    return false;
  }

  epoll_fd_ = epoll_create1(0);
  if (epoll_fd_ < 0) {
    LOG_ERROR("epoll_create1 failed, errno=", errno);
    std::cerr << "Error: epoll_create1 failed" << std::endl;
    return false;
  }

  // 监听 socket 不使用 EPOLLONESHOT。
  addFd(epoll_fd_, listen_fd_, false, listen_trig_mode_);
  if (signal_fd_ >= 0) {
    // 信号到达后管道变为可读，从而立即唤醒阻塞中的 epoll_wait。
    addFd(epoll_fd_, signal_fd_, false, 0);
  }
  return true;
}

void WebServer::eventLoop() {
  epoll_event events[kMaxEventNumber];

  LOG_INFO("Server running on port ", port_,
           ", listen_mode=", listen_trig_mode_ ? "ET" : "LT",
           ", connect_mode=", connect_trig_mode_ ? "ET" : "LT",
           ", idle_timeout_seconds=", idle_timeout_.count() / 1000,
           ", actor_model=", actor_model_ == 0 ? "Proactor" : "Reactor",
           ", worker_threads=", thread_count_,
           ", mysql=",
           database_pool_ && database_pool_->isReady()
               ? "enabled"
               : "disabled");
  std::cout << "Server running on port " << port_ << std::endl;
  std::cout << " > Listen Mode: "
            << (listen_trig_mode_ ? "ET" : "LT") << std::endl;
  std::cout << " > Connect Mode: "
            << (connect_trig_mode_ ? "ET" : "LT") << std::endl;
  std::cout << " > Idle Timeout: " << idle_timeout_.count() / 1000
            << "s" << std::endl;
  std::cout << " > Actor Model: "
            << (actor_model_ == 0 ? "Proactor" : "Reactor")
            << std::endl;
  std::cout << " > Worker Threads: " << thread_count_ << std::endl;
  std::cout << " > MySQL: "
            << (database_pool_ && database_pool_->isReady()
                    ? "已启用"
                    : "未启用")
            << std::endl;

  bool running = true;
  while (running) {
    expireIdleConnections();

    // 最多等待到最近一个连接到期，之后执行超时清理。
    int event_count =
        epoll_wait(epoll_fd_, events, kMaxEventNumber,
                   nextTimerTimeout());

    if (event_count < 0) {
      if (errno == EINTR) {
        continue;
      }

      LOG_ERROR("epoll_wait failed, errno=", errno);
      std::cerr << "Error: epoll_wait failed" << std::endl;
      break;
    }

    for (int i = 0; i < event_count; ++i) {
      int sockfd = events[i].data.fd;
      uint32_t event = events[i].events;

      if (sockfd == signal_fd_) {
        // 排空管道，合并处理短时间内到达的多个退出信号。
        char signal_buffer[64];
        while (read(signal_fd_, signal_buffer, sizeof(signal_buffer)) > 0) {
        }
        running = false;
        break;
      } else if (sockfd == listen_fd_) {
        // 监听 socket 可读，说明有新连接到达。
        dealclinetdata();
      } else {
        ClientPtr client = findClient(sockfd);
        if (client && !dispatchClientEvent(client, event)) {
          LOG_WARN("Worker queue is full, closing fd=", sockfd);
          std::cerr << "Warning: worker queue is full, closing fd: "
                    << sockfd << std::endl;
          closeClient(client);
        }
      }
    }
  }

  LOG_INFO("Server stopping gracefully");
  std::cout << "Server stopping gracefully" << std::endl;
}

bool WebServer::dealclinetdata() {
  if (listen_trig_mode_ == 1) {
    return acceptConnectionET();
  }

  return acceptConnectionLT();
}

bool WebServer::acceptConnectionLT() {
  sockaddr_in client_address{};
  socklen_t client_length = sizeof(client_address);

  // LT 模式每次事件只接收一个连接，其余连接会继续触发通知。
  int client_fd =
      accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_address),
             &client_length);
  if (client_fd < 0) {
    LOG_ERROR("accept failed in LT mode, errno=", errno);
    std::cerr << "Error: accept failed, errno: " << errno << std::endl;
    return false;
  }

  if (linger_option_ == 1) {
    linger option{1, 1};
    setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &option, sizeof(option));
  }

  ClientPtr client =
      std::make_shared<ClientData>(user_repository_);
  client->address = client_address;
  client->sockfd = client_fd;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.emplace(client_fd, client);
  }

  // 客户端连接使用 EPOLLONESHOT，避免同一连接被重复处理。
  addFd(epoll_fd_, client_fd, true, connect_trig_mode_);
  refreshTimer(client);

  LOG_DEBUG("Accepted connection fd=", client_fd);
  std::cout << "New connection (fd: " << client_fd << ")" << std::endl;
  return true;
}

bool WebServer::acceptConnectionET() {
  // ET 模式下一次事件通知可能对应多个待处理连接，
  // 因此必须循环 accept，直到返回 EAGAIN。
  while (true) {
    sockaddr_in client_address{};
    socklen_t client_length = sizeof(client_address);

    int client_fd =
        accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_address),
               &client_length);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }

      LOG_ERROR("accept failed in ET mode, errno=", errno);
      std::cerr << "Error: accept failed, errno: " << errno << std::endl;
      return false;
    }

    if (linger_option_ == 1) {
      linger option{1, 1};
      setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &option,
                 sizeof(option));
    }

    ClientPtr client =
        std::make_shared<ClientData>(user_repository_);
    client->address = client_address;
    client->sockfd = client_fd;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      clients_.emplace(client_fd, client);
    }
    addFd(epoll_fd_, client_fd, true, connect_trig_mode_);
    refreshTimer(client);

    LOG_DEBUG("Accepted connection fd=", client_fd);
    std::cout << "New connection (fd: " << client_fd << ")"
              << std::endl;
  }
}

WebServer::ClientPtr WebServer::findClient(int sockfd) {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  auto client = clients_.find(sockfd);
  if (client == clients_.end()) {
    return nullptr;
  }
  return client->second;
}

bool WebServer::dispatchClientEvent(const ClientPtr& client,
                                    uint32_t event) {
  bool expected = false;
  if (!client->busy.compare_exchange_strong(expected, true)) {
    return true;
  }

  refreshTimer(client);
  if (actor_model_ == 0) {
    return handleProactorEvent(client, event);
  }

  bool enqueued = thread_pool_ && thread_pool_->enqueue([this, client, event] {
    if (client->closed.load()) {
      client->busy.store(false);
      return;
    }
    handleClientEvent(client, event);
  });

  if (!enqueued) {
    client->busy.store(false);
  }
  return enqueued;
}

bool WebServer::handleProactorEvent(const ClientPtr& client,
                                    uint32_t event) {
  ClientAction action = ClientAction::kClose;

  if (event & EPOLLIN) {
    HttpConnection::ReadResult result = client->connection.readFromSocket(
        client->sockfd, connect_trig_mode_ == 1);
    if (result == HttpConnection::ReadResult::kDataReady) {
      return enqueueBusiness(client);
    }
  } else if (event & EPOLLOUT) {
    HttpConnection::WriteResult result =
        client->connection.writeToSocket(client->sockfd);
    if (result == HttpConnection::WriteResult::kWantProcess) {
      return enqueueBusiness(client);
    }
    if (result == HttpConnection::WriteResult::kWantRead) {
      action = ClientAction::kWaitForRead;
    } else if (result == HttpConnection::WriteResult::kWantWrite) {
      action = ClientAction::kWaitForWrite;
    }
  }

  finishClientAction(client, action);
  return true;
}

bool WebServer::enqueueBusiness(const ClientPtr& client) {
  bool enqueued = thread_pool_ && thread_pool_->enqueue([this, client] {
    if (client->closed.load()) {
      client->busy.store(false);
      return;
    }
    finishClientAction(client, processClient(client));
  });

  if (!enqueued) {
    client->busy.store(false);
  }
  return enqueued;
}

void WebServer::handleClientEvent(const ClientPtr& client,
                                  uint32_t event) {
  if (client->closed.load()) {
    client->busy.store(false);
    return;
  }

  ClientAction action = ClientAction::kClose;
  if (event & EPOLLIN) {
    action = dealwithread(client);
  } else if (event & EPOLLOUT) {
    action = dealwithwrite(client);
  } else if (event & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
    LOG_DEBUG("Connection event requested close, fd=", client->sockfd,
              ", events=", event);
    std::cout << "Connection closed (fd: " << client->sockfd << ")"
              << std::endl;
  }

  finishClientAction(client, action);
}

WebServer::ClientAction WebServer::processClient(
    const ClientPtr& client) {
  if (!client || client->closed.load()) {
    return ClientAction::kClose;
  }

  HttpConnection::ProcessResult result =
      client->connection.processRequest();
  return result == HttpConnection::ProcessResult::kResponseReady
             ? ClientAction::kWaitForWrite
             : ClientAction::kWaitForRead;
}

void WebServer::finishClientAction(const ClientPtr& client,
                                   ClientAction action) {
  if (action == ClientAction::kClose) {
    closeClient(client);
    client->busy.store(false);
    return;
  }

  refreshTimer(client);
  client->busy.store(false);

  if (!client->closed.load()) {
    int next_event =
        action == ClientAction::kWaitForRead ? EPOLLIN : EPOLLOUT;
    modFd(epoll_fd_, client->sockfd, next_event, connect_trig_mode_);
  }
}

WebServer::ClientAction WebServer::dealwithread(
    const ClientPtr& client) {
  if (!client || client->closed.load()) {
    return ClientAction::kClose;
  }

  HttpConnection::ReadResult result = client->connection.readFromSocket(
      client->sockfd, connect_trig_mode_ == 1);
  if (result == HttpConnection::ReadResult::kDataReady) {
    return processClient(client);
  }
  return ClientAction::kClose;
}

WebServer::ClientAction WebServer::dealwithwrite(
    const ClientPtr& client) {
  if (!client || client->closed.load()) {
    return ClientAction::kClose;
  }

  HttpConnection::WriteResult result =
      client->connection.writeToSocket(client->sockfd);
  if (result == HttpConnection::WriteResult::kWantRead) {
    return ClientAction::kWaitForRead;
  }
  if (result == HttpConnection::WriteResult::kWantWrite) {
    return ClientAction::kWaitForWrite;
  }
  if (result == HttpConnection::WriteResult::kWantProcess) {
    return processClient(client);
  }
  return ClientAction::kClose;
}

void WebServer::refreshTimer(const ClientPtr& client) {
  if (!client || client->closed.load()) {
    return;
  }

  uint64_t generation = client->timer_generation.fetch_add(1) + 1;
  TimerEntry entry{Clock::now() + idle_timeout_, client, generation};

  std::lock_guard<std::mutex> lock(timers_mutex_);
  timers_.push(std::move(entry));
}

void WebServer::expireIdleConnections() {
  std::vector<ClientPtr> expired_clients;

  {
    std::lock_guard<std::mutex> lock(timers_mutex_);
    Clock::time_point now = Clock::now();

    while (!timers_.empty() && timers_.top().expires_at <= now) {
      TimerEntry entry = timers_.top();
      timers_.pop();

      ClientPtr client = entry.client.lock();
      if (!client || client->closed.load() ||
          client->timer_generation.load() != entry.generation) {
        continue;
      }

      bool expected = false;
      if (!client->busy.compare_exchange_strong(expected, true)) {
        uint64_t generation =
            client->timer_generation.fetch_add(1) + 1;
        timers_.push(
            TimerEntry{now + idle_timeout_, client, generation});
        continue;
      }

      expired_clients.push_back(std::move(client));
    }
  }

  for (const ClientPtr& client : expired_clients) {
    LOG_INFO("Idle connection timed out, fd=", client->sockfd);
    std::cout << "Idle connection timed out (fd: " << client->sockfd
              << ")" << std::endl;
    closeClient(client);
  }
}

int WebServer::nextTimerTimeout() {
  std::lock_guard<std::mutex> lock(timers_mutex_);

  while (!timers_.empty()) {
    const TimerEntry& entry = timers_.top();
    ClientPtr client = entry.client.lock();
    if (!client || client->closed.load() ||
        client->timer_generation.load() != entry.generation) {
      timers_.pop();
      continue;
    }

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        entry.expires_at - Clock::now());
    if (remaining.count() <= 0) {
      return 0;
    }
    return static_cast<int>(
        std::min<int64_t>(remaining.count(), INT_MAX));
  }

  return -1;
}

void WebServer::closeClient(const ClientPtr& client) {
  if (!client || client->closed.exchange(true)) {
    return;
  }

  client->timer_generation.fetch_add(1);

  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto current = clients_.find(client->sockfd);
    if (current != clients_.end() && current->second == client) {
      clients_.erase(current);
    }
  }

  removeFd(epoll_fd_, client->sockfd);
  LOG_DEBUG("Closed connection fd=", client->sockfd);
}

int WebServer::setNonblocking(int fd) {
  int old_options = fcntl(fd, F_GETFL);
  if (old_options < 0) {
    return -1;
  }

  if (fcntl(fd, F_SETFL, old_options | O_NONBLOCK) < 0) {
    return -1;
  }

  return old_options;
}

void WebServer::addFd(int epollfd, int fd, bool one_shot,
                      int trig_mode) {
  epoll_event event{};
  event.data.fd = fd;
  event.events = EPOLLIN | EPOLLRDHUP;

  if (trig_mode == 1) {
    event.events |= EPOLLET;
  }

  if (one_shot) {
    event.events |= EPOLLONESHOT;
  }

  // ET 模式要求非阻塞；这里统一设置，简化 LT/ET 两套流程。
  setNonblocking(fd);
  epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
}

void WebServer::removeFd(int epollfd, int fd) {
  epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr);
  close(fd);
}

void WebServer::modFd(int epollfd, int fd, int event_type,
                      int trig_mode) {
  epoll_event event{};
  event.data.fd = fd;
  event.events = event_type | EPOLLONESHOT | EPOLLRDHUP;

  if (trig_mode == 1) {
    event.events |= EPOLLET;
  }

  // 重新激活 EPOLLONESHOT，并切换下一次关注的读写事件。
  epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}
