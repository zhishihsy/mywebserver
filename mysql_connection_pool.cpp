#include "mysql_connection_pool.h"

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#endif

#include <utility>

MysqlConnectionPool::Guard::Guard(MysqlConnectionPool* pool,
                                  MYSQL* connection)
    : pool_(pool), connection_(connection) {}

MysqlConnectionPool::Guard::~Guard() {
  reset();
}

MysqlConnectionPool::Guard::Guard(Guard&& other) noexcept
    : pool_(other.pool_), connection_(other.connection_) {
  other.pool_ = nullptr;
  other.connection_ = nullptr;
}

MysqlConnectionPool::Guard& MysqlConnectionPool::Guard::operator=(
    Guard&& other) noexcept {
  if (this != &other) {
    reset();
    pool_ = other.pool_;
    connection_ = other.connection_;
    other.pool_ = nullptr;
    other.connection_ = nullptr;
  }
  return *this;
}

MYSQL* MysqlConnectionPool::Guard::get() const {
  return connection_;
}

MysqlConnectionPool::Guard::operator bool() const {
  return connection_ != nullptr;
}

void MysqlConnectionPool::Guard::reset() {
  if (pool_ && connection_) {
    pool_->release(connection_);
  }
  pool_ = nullptr;
  connection_ = nullptr;
}

MysqlConnectionPool::MysqlConnectionPool() = default;

MysqlConnectionPool::~MysqlConnectionPool() {
  shutdown();
}

bool MysqlConnectionPool::initialize(const DatabaseConfig& config,
                                     std::string* error) {
  shutdown();
  config_ = config;

  if (!config.enabled) {
    if (error) {
      error->clear();
    }
    return true;
  }

#ifndef ENABLE_MYSQL
  if (error) {
    *error = "当前程序未启用 MySQL 支持，请使用 `make mysql` 重新构建";
  }
  return false;
#else
  if (config.pool_size == 0 || config.user.empty() ||
      config.database.empty()) {
    if (error) {
      *error = "数据库配置无效";
    }
    return false;
  }

  if (mysql_library_init(0, nullptr, nullptr) != 0) {
    if (error) {
      *error = "MySQL 客户端库初始化失败";
    }
    return false;
  }
  library_initialized_ = true;
  stopping_ = false;
  for (std::size_t i = 0; i < config.pool_size; ++i) {
    MYSQL* connection = createConnection(error);
    if (!connection) {
      shutdown();
      return false;
    }
    connections_.push_back(connection);
    available_.push(connection);
  }
  ready_ = true;
  return true;
#endif
}

MysqlConnectionPool::Guard MysqlConnectionPool::acquire() {
#ifndef ENABLE_MYSQL
  return {};
#else
  std::unique_lock<std::mutex> lock(mutex_);
  if (!ready_ ||
      !condition_.wait_for(lock, config_.acquire_timeout, [this] {
        return stopping_ || !available_.empty();
      }) ||
      stopping_) {
    return {};
  }

  MYSQL* connection = available_.front();
  available_.pop();
  return Guard(this, connection);
#endif
}

void MysqlConnectionPool::shutdown() {
  std::vector<MYSQL*> connections;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    ready_ = false;
    while (!available_.empty()) {
      available_.pop();
    }
    connections.swap(connections_);
  }
  condition_.notify_all();

#ifdef ENABLE_MYSQL
  for (MYSQL* connection : connections) {
    mysql_close(connection);
  }
  if (library_initialized_) {
    mysql_library_end();
    library_initialized_ = false;
  }
#else
  (void)connections;
#endif
}

bool MysqlConnectionPool::isReady() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ready_ && !stopping_;
}

MYSQL* MysqlConnectionPool::createConnection(std::string* error) const {
#ifndef ENABLE_MYSQL
  (void)error;
  return nullptr;
#else
  MYSQL* connection = mysql_init(nullptr);
  if (!connection) {
    if (error) {
      *error = "MySQL 连接对象初始化失败";
    }
    return nullptr;
  }

  unsigned int connect_timeout = 3;
  bool reconnect = true;
  mysql_options(connection, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
  mysql_options(connection, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  mysql_options(connection, MYSQL_OPT_RECONNECT, &reconnect);

  if (!mysql_real_connect(connection, config_.host.c_str(),
                          config_.user.c_str(), config_.password.c_str(),
                          config_.database.c_str(), config_.port, nullptr,
                          0)) {
    if (error) {
      *error = mysql_error(connection);
    }
    mysql_close(connection);
    return nullptr;
  }
  return connection;
#endif
}

void MysqlConnectionPool::release(MYSQL* connection) {
  if (!connection) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    available_.push(connection);
  }
  condition_.notify_one();
}
