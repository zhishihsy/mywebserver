#ifndef MYSQL_CONNECTION_POOL_H
#define MYSQL_CONNECTION_POOL_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#else
struct st_mysql;
using MYSQL = st_mysql;
#endif

struct DatabaseConfig {
  bool enabled = false;
  std::string host = "127.0.0.1";
  unsigned int port = 3306;
  std::string user;
  std::string password;
  std::string database = "mywebserver";
  std::size_t pool_size = 8;
  std::chrono::milliseconds acquire_timeout{2000};
};

class MysqlConnectionPool {
 public:
  class Guard {
   public:
    Guard() = default;
    Guard(MysqlConnectionPool* pool, MYSQL* connection);
    ~Guard();

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&& other) noexcept;
    Guard& operator=(Guard&& other) noexcept;

    MYSQL* get() const;
    explicit operator bool() const;

   private:
    void reset();

    MysqlConnectionPool* pool_ = nullptr;
    MYSQL* connection_ = nullptr;
  };

  MysqlConnectionPool();
  ~MysqlConnectionPool();

  MysqlConnectionPool(const MysqlConnectionPool&) = delete;
  MysqlConnectionPool& operator=(const MysqlConnectionPool&) = delete;

  bool initialize(const DatabaseConfig& config, std::string* error);
  Guard acquire();
  void shutdown();
  bool isReady() const;

 private:
  friend class Guard;

  MYSQL* createConnection(std::string* error) const;
  void release(MYSQL* connection);

  DatabaseConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::queue<MYSQL*> available_;
  std::vector<MYSQL*> connections_;
  bool library_initialized_ = false;
  bool ready_ = false;
  bool stopping_ = false;
};

#endif
