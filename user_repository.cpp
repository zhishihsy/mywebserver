#include "user_repository.h"

#include "logger.h"

#ifdef ENABLE_MYSQL
#include <mysql/mysql.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
#ifdef ENABLE_MYSQL
constexpr int kPbkdf2Iterations = 120000;
constexpr std::size_t kSaltSize = 16;
constexpr std::size_t kHashSize = 32;

std::string hexEncode(const unsigned char* data, std::size_t size) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) {
    output << std::setw(2) << static_cast<unsigned int>(data[i]);
  }
  return output.str();
}

bool hexDecode(const std::string& value,
               std::vector<unsigned char>* output) {
  if (value.size() % 2 != 0) {
    return false;
  }
  output->clear();
  output->reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    try {
      std::size_t consumed = 0;
      unsigned long byte =
          std::stoul(value.substr(i, 2), &consumed, 16);
      if (consumed != 2 || byte > 255) {
        return false;
      }
      output->push_back(static_cast<unsigned char>(byte));
    } catch (...) {
      return false;
    }
  }
  return true;
}

bool derivePassword(const std::string& password,
                    const unsigned char* salt, std::size_t salt_size,
                    int iterations,
                    std::array<unsigned char, kHashSize>* hash) {
  return PKCS5_PBKDF2_HMAC(
             password.data(), static_cast<int>(password.size()), salt,
             static_cast<int>(salt_size), iterations, EVP_sha256(),
             static_cast<int>(hash->size()), hash->data()) == 1;
}

bool hashPassword(const std::string& password, std::string* encoded) {
  std::array<unsigned char, kSaltSize> salt{};
  std::array<unsigned char, kHashSize> hash{};
  if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
      !derivePassword(password, salt.data(), salt.size(),
                      kPbkdf2Iterations, &hash)) {
    return false;
  }

  *encoded = "pbkdf2_sha256$" + std::to_string(kPbkdf2Iterations) +
             "$" + hexEncode(salt.data(), salt.size()) + "$" +
             hexEncode(hash.data(), hash.size());
  return true;
}

bool verifyPassword(const std::string& password,
                    const std::string& encoded) {
  std::size_t first = encoded.find('$');
  std::size_t second = encoded.find('$', first + 1);
  std::size_t third = encoded.find('$', second + 1);
  if (first == std::string::npos || second == std::string::npos ||
      third == std::string::npos ||
      encoded.substr(0, first) != "pbkdf2_sha256") {
    return false;
  }

  int iterations = 0;
  try {
    iterations = std::stoi(encoded.substr(first + 1,
                                          second - first - 1));
  } catch (...) {
    return false;
  }
  if (iterations <= 0 || iterations > 10000000) {
    return false;
  }

  std::vector<unsigned char> salt;
  std::vector<unsigned char> expected;
  if (!hexDecode(encoded.substr(second + 1, third - second - 1),
                 &salt) ||
      !hexDecode(encoded.substr(third + 1), &expected) ||
      expected.size() != kHashSize) {
    return false;
  }

  std::array<unsigned char, kHashSize> actual{};
  return derivePassword(password, salt.data(), salt.size(), iterations,
                        &actual) &&
         CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

class Statement {
 public:
  explicit Statement(MYSQL* connection)
      : statement_(mysql_stmt_init(connection)) {}
  ~Statement() {
    if (statement_) {
      mysql_stmt_close(statement_);
    }
  }
  MYSQL_STMT* get() const { return statement_; }

 private:
  MYSQL_STMT* statement_;
};

void bindString(MYSQL_BIND* binding, const std::string& value,
                unsigned long* length) {
  *length = static_cast<unsigned long>(value.size());
  binding->buffer_type = MYSQL_TYPE_STRING;
  binding->buffer = const_cast<char*>(value.data());
  binding->buffer_length = *length;
  binding->length = length;
}

void logStatementError(const char* operation, MYSQL_STMT* statement) {
  LOG_ERROR("Database operation ", operation, " failed, code=",
            mysql_stmt_errno(statement), ", message=",
            mysql_stmt_error(statement));
  std::cerr << "数据库操作“" << operation << "”失败，错误码："
            << mysql_stmt_errno(statement) << "，错误信息："
            << mysql_stmt_error(statement) << '\n';
}
#endif
}  // 匿名命名空间

UserRepository::UserRepository(
    std::shared_ptr<MysqlConnectionPool> connection_pool)
    : connection_pool_(std::move(connection_pool)) {}

RegisterResult UserRepository::registerUser(
    const std::string& username, const std::string& password) {
#ifndef ENABLE_MYSQL
  (void)username;
  (void)password;
  return RegisterResult::kUnavailable;
#else
  if (!connection_pool_ || !connection_pool_->isReady()) {
    return RegisterResult::kUnavailable;
  }

  std::string password_hash;
  if (!hashPassword(password, &password_hash)) {
    LOG_ERROR("Password hashing failed");
    std::cerr << "密码哈希计算失败\n";
    return RegisterResult::kError;
  }

  auto connection = connection_pool_->acquire();
  if (!connection) {
    return RegisterResult::kUnavailable;
  }

  Statement statement(connection.get());
  const char query[] =
      "INSERT INTO users(username, password_hash) VALUES (?, ?)";
  if (!statement.get() ||
      mysql_stmt_prepare(statement.get(), query, sizeof(query) - 1) != 0) {
    if (statement.get()) {
      logStatementError("准备注册语句", statement.get());
    }
    return RegisterResult::kError;
  }

  MYSQL_BIND parameters[2]{};
  unsigned long username_length = 0;
  unsigned long hash_length = 0;
  bindString(&parameters[0], username, &username_length);
  bindString(&parameters[1], password_hash, &hash_length);

  if (mysql_stmt_bind_param(statement.get(), parameters) != 0 ||
      mysql_stmt_execute(statement.get()) != 0) {
    if (mysql_stmt_errno(statement.get()) == 1062) {
      return RegisterResult::kDuplicateUsername;
    }
    logStatementError("注册用户", statement.get());
    return RegisterResult::kError;
  }
  return RegisterResult::kSuccess;
#endif
}

LoginResult UserRepository::login(const std::string& username,
                                  const std::string& password) {
#ifndef ENABLE_MYSQL
  (void)username;
  (void)password;
  return LoginResult::kUnavailable;
#else
  if (!connection_pool_ || !connection_pool_->isReady()) {
    return LoginResult::kUnavailable;
  }

  auto connection = connection_pool_->acquire();
  if (!connection) {
    return LoginResult::kUnavailable;
  }

  Statement statement(connection.get());
  const char query[] =
      "SELECT password_hash FROM users WHERE username = ? LIMIT 1";
  if (!statement.get() ||
      mysql_stmt_prepare(statement.get(), query, sizeof(query) - 1) != 0) {
    if (statement.get()) {
      logStatementError("准备登录语句", statement.get());
    }
    return LoginResult::kError;
  }

  MYSQL_BIND parameter{};
  unsigned long username_length = 0;
  bindString(&parameter, username, &username_length);
  if (mysql_stmt_bind_param(statement.get(), &parameter) != 0 ||
      mysql_stmt_execute(statement.get()) != 0) {
    logStatementError("查询登录用户", statement.get());
    return LoginResult::kError;
  }

  std::array<char, 256> hash_buffer{};
  unsigned long hash_length = 0;
  MYSQL_BIND result{};
  using MysqlBool =
      std::remove_pointer_t<decltype(result.is_null)>;
  MysqlBool is_null{};
  MysqlBool truncated{};
  result.buffer_type = MYSQL_TYPE_STRING;
  result.buffer = hash_buffer.data();
  result.buffer_length = hash_buffer.size();
  result.length = &hash_length;
  result.is_null = &is_null;
  result.error = &truncated;

  if (mysql_stmt_bind_result(statement.get(), &result) != 0 ||
      mysql_stmt_store_result(statement.get()) != 0) {
    logStatementError("绑定登录查询结果", statement.get());
    return LoginResult::kError;
  }

  int fetch_result = mysql_stmt_fetch(statement.get());
  if (fetch_result == MYSQL_NO_DATA) {
    return LoginResult::kInvalidCredentials;
  }
  if (fetch_result != 0 || is_null || truncated ||
      hash_length > hash_buffer.size()) {
    logStatementError("读取登录查询结果", statement.get());
    return LoginResult::kError;
  }

  std::string stored_hash(hash_buffer.data(), hash_length);
  return verifyPassword(password, stored_hash)
             ? LoginResult::kSuccess
             : LoginResult::kInvalidCredentials;
#endif
}
