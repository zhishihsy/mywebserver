#include "webserver.h"

int main() {
  WebServer server;

  // 参数依次为：监听端口、触发组合模式、是否启用 SO_LINGER。
  // 触发模式：0 = LT + LT，1 = LT + ET，2 = ET + LT，3 = ET + ET。
  server.init(8080, 0, 0);
  server.trig_mode();

  // 初始化监听 socket 和 epoll 后进入事件循环。
  if (!server.eventListen()) {
    return 1;
  }

  server.eventLoop();
  return 0;
}
