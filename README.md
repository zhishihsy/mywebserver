# MyWebServer

一个面向学习与实践的 Linux C++17 Web 服务器，参考
[qinguoyi/TinyWebServer](https://github.com/qinguoyi/TinyWebServer)
重新实现。项目使用非阻塞 socket、epoll、线程池和 HTTP 状态机，
并提供 Reactor/模拟 Proactor、MySQL 用户系统及同步/异步日志。

## 已实现功能

- epoll LT/ET 四种监听与连接组合
- `EPOLLONESHOT` 与有界线程池
- Reactor 和模拟 Proactor 并发模型
- HTTP/1.0、HTTP/1.1、Keep-Alive 和流水线请求
- GET、POST、静态文件、URL 解码及路径穿越防护
- `mmap + writev` 大文件发送
- 400、403、404、405、413、415、500 等错误响应
- 空闲连接超时和 SIGINT/SIGTERM 优雅退出
- MySQL 连接池、RAII、预处理语句、注册和登录
- PBKDF2-HMAC-SHA256 加盐密码存储
- 同步/异步日志、级别过滤及按日期/行数轮转

## 环境

- Linux
- GCC 9 或更高版本
- GNU Make
- Python 3
- 可选：MySQL/MariaDB 客户端开发包、OpenSSL 开发包

Windows 用户需要在 WSL Linux 发行版中构建，因为项目依赖 epoll。

## 构建和运行

```bash
make build
./webserver
```

常用参数：

```text
-p PORT     监听端口，默认 8080
-m MODE     0=LT/LT，1=LT/ET，2=ET/LT，3=ET/ET
-o 0|1      是否启用 SO_LINGER
-t THREADS  工作线程数
-a MODEL    0=模拟 Proactor，1=Reactor
-i SECONDS  空闲连接超时秒数
```

示例：

```bash
./webserver -p 8080 -m 3 -a 1 -t 8 -i 60
```

访问 `http://127.0.0.1:8080/`，健康检查接口为 `/health`。

## 测试

```bash
make test-all
```

也可以单独执行：

```bash
make test-http
make test-concurrency
make test-stability
make test-logging
```

Sanitizer 构建：

```bash
make sanitizer
ASAN_OPTIONS=detect_leaks=1 python3 tests/http_protocol_test.py
```

基础压力测试需要先启动服务器：

```bash
python3 tests/stress_test.py --requests 10000 --concurrency 500
```

万级并发连接验收：

```bash
ulimit -n 30000
./webserver -t 8 -m 3 -a 0
# 在另一个终端执行
make load-test
```

## MySQL

初始化和环境变量配置见 [MySQL功能说明.md](MySQL功能说明.md)。

```bash
make mysql
make test-mysql
```

注册和登录接口：

```bash
curl -i -X POST http://127.0.0.1:8080/register \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data 'username=alice&password=change-me'

curl -i -X POST http://127.0.0.1:8080/login \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data 'username=alice&password=change-me'
```

## 日志

日志配置见 [日志系统说明.md](日志系统说明.md)。主要环境变量包括：

- `LOG_ENABLED`
- `LOG_ASYNC`
- `LOG_LEVEL`
- `LOG_FILE`
- `LOG_QUEUE_SIZE`
- `LOG_MAX_LINES`

## 项目结构

```text
main.cpp                     参数、信号和组件初始化
webserver.*                  epoll 事件循环、并发模型和定时器
http_connection.*            HTTP 解析、路由和静态文件响应
thread_pool.h                有界工作线程池
mysql_connection_pool.*      MySQL 连接池与 RAII
user_repository.*            注册、登录和密码处理
logger.*                     同步/异步日志
tests/                       协议、并发、稳定性和集成测试
```

## 与 TinyWebServer 的差异

本项目保留原项目的关键学习目标，但使用 C++17 标准库重写部分基础设施。
配置主要通过命令行和环境变量完成；定时器使用单调时钟优先队列；
用户密码经过安全哈希，而不是明文保存。
