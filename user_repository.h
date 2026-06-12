#ifndef USER_REPOSITORY_H
#define USER_REPOSITORY_H

#include "mysql_connection_pool.h"

#include <memory>
#include <string>

enum class RegisterResult {
  kSuccess,
  kDuplicateUsername,
  kUnavailable,
  kError,
};

enum class LoginResult {
  kSuccess,
  kInvalidCredentials,
  kUnavailable,
  kError,
};

class UserRepository {
 public:
  explicit UserRepository(
      std::shared_ptr<MysqlConnectionPool> connection_pool);

  RegisterResult registerUser(const std::string& username,
                              const std::string& password);
  LoginResult login(const std::string& username,
                    const std::string& password);

 private:
  std::shared_ptr<MysqlConnectionPool> connection_pool_;
};

#endif
