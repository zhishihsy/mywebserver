#include "webserver.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>

namespace {
constexpr int kMaxEventNumber = 10000;
constexpr int kListenBacklog = 5;
}  // namespace

WebServer::WebServer()
    : port_(8080),
      trig_mode_(0),
      listen_trig_mode_(0),
      connect_trig_mode_(0),
      linger_option_(0),
      listen_fd_(-1),
      epoll_fd_(-1) {}

WebServer::~WebServer() {
  // 先关闭所有客户端，再释放监听 socket 和 epoll。
  while (!clients_.empty()) {
    closeClient(clients_.begin()->first);
  }

  if (listen_fd_ >= 0) {
    close(listen_fd_);
  }

  if (epoll_fd_ >= 0) {
    close(epoll_fd_);
  }
}

void WebServer::init(int port, int trig_mode, int linger_option) {
  port_ = port;
  trig_mode_ = trig_mode;
  linger_option_ = linger_option;
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
    std::cerr << "Error: failed to create listen socket" << std::endl;
    return false;
  }

  // 允许服务器重启后快速复用处于 TIME_WAIT 状态的地址。
  int reuse_address = 1;
  if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                 sizeof(reuse_address)) < 0) {
    std::cerr << "Warning: SO_REUSEADDR failed" << std::endl;
  }

  // linger_option_ 为 1 时启用 SO_LINGER，最多等待 1 秒关闭连接。
  linger linger_option{linger_option_, 1};
  if (setsockopt(listen_fd_, SOL_SOCKET, SO_LINGER, &linger_option,
                 sizeof(linger_option)) < 0) {
    std::cerr << "Warning: SO_LINGER failed" << std::endl;
  }

  sockaddr_in server_address{};
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = htonl(INADDR_ANY);
  server_address.sin_port = htons(port_);

  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&server_address),
           sizeof(server_address)) < 0) {
    std::cerr << "Error: bind failed" << std::endl;
    return false;
  }

  if (listen(listen_fd_, kListenBacklog) < 0) {
    std::cerr << "Error: listen failed" << std::endl;
    return false;
  }

  epoll_fd_ = epoll_create1(0);
  if (epoll_fd_ < 0) {
    std::cerr << "Error: epoll_create1 failed" << std::endl;
    return false;
  }

  // 监听 socket 不使用 EPOLLONESHOT。
  addFd(epoll_fd_, listen_fd_, false, listen_trig_mode_);
  return true;
}

void WebServer::eventLoop() {
  epoll_event events[kMaxEventNumber];

  std::cout << "Server running on port " << port_ << std::endl;
  std::cout << " > Listen Mode: "
            << (listen_trig_mode_ ? "ET" : "LT") << std::endl;
  std::cout << " > Connect Mode: "
            << (connect_trig_mode_ ? "ET" : "LT") << std::endl;

  while (true) {
    // 阻塞等待事件；被信号中断时继续等待。
    int event_count =
        epoll_wait(epoll_fd_, events, kMaxEventNumber, -1);

    if (event_count < 0) {
      if (errno == EINTR) {
        continue;
      }

      std::cerr << "Error: epoll_wait failed" << std::endl;
      break;
    }

    for (int i = 0; i < event_count; ++i) {
      int sockfd = events[i].data.fd;
      uint32_t event = events[i].events;

      if (sockfd == listen_fd_) {
        // 监听 socket 可读，说明有新连接到达。
        dealclinetdata();
      } else if (event & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        // 对端关闭连接或 socket 出现异常。
        std::cout << "Connection closed (fd: " << sockfd << ")"
                  << std::endl;
        closeClient(sockfd);
      } else if (event & EPOLLIN) {
        // 客户端数据到达。
        dealwithread(sockfd);
      } else if (event & EPOLLOUT) {
        // 客户端 socket 当前可写。
        dealwithwrite(sockfd);
      }
    }
  }
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
    std::cerr << "Error: accept failed, errno: " << errno << std::endl;
    return false;
  }

  if (linger_option_ == 1) {
    linger option{1, 1};
    setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &option, sizeof(option));
  }

  clients_.emplace(client_fd,
                   ClientData{client_address, client_fd, HttpConnection()});

  // 客户端连接使用 EPOLLONESHOT，避免同一连接被重复处理。
  addFd(epoll_fd_, client_fd, true, connect_trig_mode_);

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

      std::cerr << "Error: accept failed, errno: " << errno << std::endl;
      return false;
    }

    if (linger_option_ == 1) {
      linger option{1, 1};
      setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &option,
                 sizeof(option));
    }

    clients_.emplace(client_fd,
                     ClientData{client_address, client_fd, HttpConnection()});
    addFd(epoll_fd_, client_fd, true, connect_trig_mode_);

    std::cout << "New connection (fd: " << client_fd << ")"
              << std::endl;
  }
}

bool WebServer::dealwithread(int sockfd) {
  auto client = clients_.find(sockfd);
  if (client == clients_.end()) {
    return false;
  }

  HttpConnection::ReadResult result =
      client->second.connection.readFromSocket(sockfd,
                                               connect_trig_mode_ == 1);
  if (result == HttpConnection::ReadResult::kNeedMoreData) {
    modFd(epoll_fd_, sockfd, EPOLLIN, connect_trig_mode_);
    return true;
  }
  if (result == HttpConnection::ReadResult::kResponseReady) {
    modFd(epoll_fd_, sockfd, EPOLLOUT, connect_trig_mode_);
    return true;
  }

  closeClient(sockfd);
  return false;
}

bool WebServer::dealwithwrite(int sockfd) {
  auto client = clients_.find(sockfd);
  if (client == clients_.end()) {
    return false;
  }

  HttpConnection::WriteResult result =
      client->second.connection.writeToSocket(sockfd);
  if (result == HttpConnection::WriteResult::kWantRead) {
    modFd(epoll_fd_, sockfd, EPOLLIN, connect_trig_mode_);
    return true;
  }
  if (result == HttpConnection::WriteResult::kWantWrite) {
    modFd(epoll_fd_, sockfd, EPOLLOUT, connect_trig_mode_);
    return true;
  }

  bool clean_close = result == HttpConnection::WriteResult::kClose;
  closeClient(sockfd);
  return clean_close;
}

void WebServer::closeClient(int sockfd) {
  clients_.erase(sockfd);
  removeFd(epoll_fd_, sockfd);
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
