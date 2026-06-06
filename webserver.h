#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <netinet/in.h>

#include <map>

class WebServer {
 public:
  WebServer();
  ~WebServer();

  // 保存服务器配置，此时不会创建 socket。
  void init(int port, int trig_mode, int linger_option);

  // 根据组合模式设置监听 socket 和客户端 socket 的 LT/ET 模式。
  void trig_mode();

  // 创建监听 socket 和 epoll 实例，并注册监听事件。
  bool eventListen();

  // 持续等待并分发 epoll 事件。
  void eventLoop();

 private:
  // 保存一个客户端连接对应的地址和文件描述符。
  struct ClientData {
    sockaddr_in address;
    int sockfd;
  };

  // 根据监听 socket 的触发模式接收新连接。
  bool dealclinetdata();
  bool acceptConnectionLT();
  bool acceptConnectionET();

  // 处理客户端 socket 的可读、可写事件。
  bool dealwithread(int sockfd);
  bool dealwithwrite(int sockfd);
  bool readLT(int sockfd);
  bool readET(int sockfd);

  // 从 epoll 和客户端表中移除连接，并关闭 socket。
  void closeClient(int sockfd);

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

  // 服务器运行期间持有的文件描述符。
  int listen_fd_;
  int epoll_fd_;

  // 以客户端 socket 为键保存连接信息。
  std::map<int, ClientData> clients_;
};

#endif
