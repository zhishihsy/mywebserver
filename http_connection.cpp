#include "http_connection.h"
#include "user_repository.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <filesystem>
#include <limits>
#include <sstream>
#include <utility>

namespace {
constexpr std::size_t kReadChunkSize = 4096;
constexpr std::size_t kMaxHeaderSize = 64 * 1024;
constexpr std::size_t kMaxBodySize = 1024 * 1024;
constexpr std::size_t kMaxBufferedSize = kMaxHeaderSize + kMaxBodySize;
const char kDocumentRoot[] = "resources";

bool pathIsWithin(const std::filesystem::path& root,
                  const std::filesystem::path& candidate) {
  auto root_part = root.begin();
  auto candidate_part = candidate.begin();
  for (; root_part != root.end(); ++root_part, ++candidate_part) {
    if (candidate_part == candidate.end() || *root_part != *candidate_part) {
      return false;
    }
  }
  return true;
}
}  // 匿名命名空间

HttpConnection::HttpConnection(
    std::shared_ptr<UserRepository> user_repository)
    : parse_state_(ParseState::kRequestLine),
      parse_position_(0),
      content_length_(0),
      bytes_written_(0),
      mapped_file_(nullptr),
      mapped_file_size_(0),
      keep_alive_(false),
      user_repository_(std::move(user_repository)) {}

HttpConnection::~HttpConnection() {
  releaseMappedFile();
}

HttpConnection::ReadResult HttpConnection::readFromSocket(
    int sockfd, bool edge_triggered) {
  char buffer[kReadChunkSize];
  bool received_data = false;

  while (true) {
    ssize_t bytes_read = recv(sockfd, buffer, sizeof(buffer), 0);
    if (bytes_read > 0) {
      received_data = true;
      read_buffer_.append(buffer, static_cast<std::size_t>(bytes_read));
      if (!edge_triggered) {
        break;
      }
      continue;
    }

    if (bytes_read == 0) {
      return received_data ? ReadResult::kDataReady
                           : ReadResult::kPeerClosed;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    return ReadResult::kError;
  }

  return ReadResult::kDataReady;
}

HttpConnection::ProcessResult HttpConnection::processRequest() {
  if (read_buffer_.size() > kMaxBufferedSize) {
    keep_alive_ = false;
    buildErrorResponse(413, "Payload Too Large", "Request is too large.");
    return ProcessResult::kResponseReady;
  }

  ParseResult result = parseRequest();
  if (result == ParseResult::kIncomplete) {
    return ProcessResult::kNeedMoreData;
  }
  if (result == ParseResult::kBadRequest) {
    keep_alive_ = false;
    buildErrorResponse(400, "Bad Request", "Malformed HTTP request.");
  } else if (result == ParseResult::kPayloadTooLarge) {
    keep_alive_ = false;
    buildErrorResponse(413, "Payload Too Large", "Request is too large.");
  } else {
    buildResponse();
  }
  return ProcessResult::kResponseReady;
}

HttpConnection::WriteResult HttpConnection::writeToSocket(int sockfd) {
  const std::size_t total_size = write_buffer_.size() + mapped_file_size_;
  while (bytes_written_ < total_size) {
    iovec vectors[2]{};
    int vector_count = 0;
    std::size_t offset = bytes_written_;

    if (offset < write_buffer_.size()) {
      vectors[vector_count].iov_base =
          const_cast<char*>(write_buffer_.data() + offset);
      vectors[vector_count].iov_len = write_buffer_.size() - offset;
      ++vector_count;
      offset = 0;
    } else {
      offset -= write_buffer_.size();
    }

    if (mapped_file_ && offset < mapped_file_size_) {
      vectors[vector_count].iov_base =
          static_cast<char*>(mapped_file_) + offset;
      vectors[vector_count].iov_len = mapped_file_size_ - offset;
      ++vector_count;
    }

    ssize_t bytes_sent = writev(sockfd, vectors, vector_count);
    if (bytes_sent > 0) {
      bytes_written_ += static_cast<std::size_t>(bytes_sent);
      continue;
    }
    if (bytes_sent < 0 && errno == EINTR) {
      continue;
    }
    if (bytes_sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return WriteResult::kWantWrite;
    }
    return WriteResult::kError;
  }

  releaseMappedFile();
  if (!keep_alive_) {
    return WriteResult::kClose;
  }

  resetRequest();
  return read_buffer_.empty() ? WriteResult::kWantRead
                              : WriteResult::kWantProcess;
}

HttpConnection::ParseResult HttpConnection::parseRequest() {
  while (true) {
    if (parse_state_ == ParseState::kBody) {
      if (content_length_ > kMaxBodySize) {
        return ParseResult::kPayloadTooLarge;
      }
      if (read_buffer_.size() - parse_position_ < content_length_) {
        return ParseResult::kIncomplete;
      }
      body_ = read_buffer_.substr(parse_position_, content_length_);
      parse_position_ += content_length_;
      return ParseResult::kComplete;
    }

    std::size_t line_end = read_buffer_.find("\r\n", parse_position_);
    if (line_end == std::string::npos) {
      if (read_buffer_.size() - parse_position_ > kMaxHeaderSize) {
        return ParseResult::kPayloadTooLarge;
      }
      return ParseResult::kIncomplete;
    }
    if (line_end > kMaxHeaderSize) {
      return ParseResult::kPayloadTooLarge;
    }
    std::string line =
        read_buffer_.substr(parse_position_, line_end - parse_position_);
    parse_position_ = line_end + 2;

    if (parse_state_ == ParseState::kRequestLine) {
      if (!parseRequestLine(line)) {
        return ParseResult::kBadRequest;
      }
      parse_state_ = ParseState::kHeaders;
      continue;
    }

    if (line.empty()) {
      if (version_ == "HTTP/1.1" &&
          (headers_.find("host") == headers_.end() ||
           headers_["host"].empty())) {
        return ParseResult::kBadRequest;
      }
      if (headers_.find("transfer-encoding") != headers_.end()) {
        return ParseResult::kBadRequest;
      }

      auto length = headers_.find("content-length");
      if (length != headers_.end()) {
        try {
          std::size_t consumed = 0;
          content_length_ = std::stoull(length->second, &consumed);
          if (consumed != length->second.size()) {
            return ParseResult::kBadRequest;
          }
          if (content_length_ > kMaxBodySize) {
            return ParseResult::kPayloadTooLarge;
          }
        } catch (...) {
          return ParseResult::kBadRequest;
        }
      }

      std::string connection = toLower(headers_["connection"]);
      keep_alive_ = version_ == "HTTP/1.1";
      if (connection == "close") {
        keep_alive_ = false;
      } else if (connection == "keep-alive") {
        keep_alive_ = true;
      }

      if (content_length_ == 0) {
        return ParseResult::kComplete;
      }
      parse_state_ = ParseState::kBody;
      continue;
    }

    if (!parseHeader(line)) {
      return ParseResult::kBadRequest;
    }
  }
}

bool HttpConnection::parseRequestLine(const std::string& line) {
  std::istringstream stream(line);
  std::string extra;
  if (!(stream >> method_ >> target_ >> version_) || (stream >> extra)) {
    return false;
  }
  if (method_.empty() ||
      !std::all_of(method_.begin(), method_.end(), [](unsigned char ch) {
        return std::isupper(ch);
      })) {
    return false;
  }
  return !target_.empty() && target_.front() == '/' &&
         (version_ == "HTTP/1.0" || version_ == "HTTP/1.1");
}

bool HttpConnection::parseHeader(const std::string& line) {
  std::size_t separator = line.find(':');
  if (separator == std::string::npos || separator == 0) {
    return false;
  }
  std::string name = toLower(trim(line.substr(0, separator)));
  if (name.empty() ||
      !std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-';
      })) {
    return false;
  }
  std::string value = trim(line.substr(separator + 1));
  auto existing = headers_.find(name);
  if (existing != headers_.end()) {
    if (name == "content-length") {
      return existing->second == value;
    }
    existing->second += "," + value;
  } else {
    headers_[name] = std::move(value);
  }
  return true;
}

void HttpConnection::buildResponse() {
  if (method_ == "POST") {
    std::string route = target_;
    std::size_t query = route.find_first_of("?#");
    if (query != std::string::npos) {
      route.resize(query);
    }
    if (route == "/register") {
      handleRegister();
      return;
    }
    if (route == "/login") {
      handleLogin();
      return;
    }
    if (route == "/echo") {
      std::string content_type = "text/plain; charset=utf-8";
      auto requested_type = headers_.find("content-type");
      if (requested_type != headers_.end() &&
          !requested_type->second.empty()) {
        content_type = requested_type->second;
      }
      buildMemoryResponse(200, "OK", content_type, body_);
      return;
    }
    buildErrorResponse(404, "Not Found", "未知的 POST 路由。");
    return;
  }
  if (method_ != "GET") {
    std::string body = "405 Method Not Allowed\nOnly GET and POST are supported.\n";
    buildMemoryResponse(405, "Method Not Allowed",
                        "text/plain; charset=utf-8", body,
                        "Allow: GET, POST\r\n");
    return;
  }
  if (target_ == "/health") {
    buildMemoryResponse(200, "OK", "text/plain; charset=utf-8", "OK\n");
    return;
  }
  switch (buildStaticFileResponse()) {
    case StaticFileResult::kOk:
      return;
    case StaticFileResult::kBadRequest:
      buildErrorResponse(400, "Bad Request", "The request path is invalid.");
      return;
    case StaticFileResult::kForbidden:
      buildErrorResponse(403, "Forbidden", "Access to this path is denied.");
      return;
    case StaticFileResult::kNotFound:
      buildErrorResponse(404, "Not Found",
                         "The requested file was not found.");
      return;
    case StaticFileResult::kInternalError:
      keep_alive_ = false;
      buildErrorResponse(500, "Internal Server Error",
                         "The server could not read the requested file.");
      return;
  }
}

void HttpConnection::handleRegister() {
  auto content_type = headers_.find("content-type");
  if (content_type == headers_.end() ||
      toLower(content_type->second).find(
          "application/x-www-form-urlencoded") != 0) {
    buildMemoryResponse(
        415, "Unsupported Media Type", "application/json; charset=utf-8",
        "{\"error\":\"请求类型必须是 "
        "application/x-www-form-urlencoded\"}\n");
    return;
  }

  std::unordered_map<std::string, std::string> fields;
  if (!parseFormBody(&fields)) {
    buildMemoryResponse(400, "Bad Request",
                        "application/json; charset=utf-8",
                        "{\"error\":\"表单请求体格式错误\"}\n");
    return;
  }

  const std::string& username = fields["username"];
  const std::string& password = fields["password"];
  if (username.empty() || username.size() > 64 ||
      password.size() < 8 || password.size() > 1024) {
    buildMemoryResponse(
        400, "Bad Request", "application/json; charset=utf-8",
        "{\"error\":\"用户名长度必须为 1-64 字节，密码长度必须为 "
        "8-1024 字节\"}\n");
    return;
  }
  if (!user_repository_) {
    buildMemoryResponse(503, "Service Unavailable",
                        "application/json; charset=utf-8",
                        "{\"error\":\"数据库暂不可用\"}\n");
    return;
  }

  switch (user_repository_->registerUser(username, password)) {
    case RegisterResult::kSuccess:
      buildMemoryResponse(201, "Created",
                          "application/json; charset=utf-8",
                          "{\"message\":\"注册成功\"}\n");
      return;
    case RegisterResult::kDuplicateUsername:
      buildMemoryResponse(409, "Conflict",
                          "application/json; charset=utf-8",
                          "{\"error\":\"用户名已存在\"}\n");
      return;
    case RegisterResult::kUnavailable:
      buildMemoryResponse(503, "Service Unavailable",
                          "application/json; charset=utf-8",
                          "{\"error\":\"数据库暂不可用\"}\n");
      return;
    case RegisterResult::kError:
      buildMemoryResponse(500, "Internal Server Error",
                          "application/json; charset=utf-8",
                          "{\"error\":\"注册失败\"}\n");
      return;
  }
}

void HttpConnection::handleLogin() {
  auto content_type = headers_.find("content-type");
  if (content_type == headers_.end() ||
      toLower(content_type->second).find(
          "application/x-www-form-urlencoded") != 0) {
    buildMemoryResponse(
        415, "Unsupported Media Type", "application/json; charset=utf-8",
        "{\"error\":\"请求类型必须是 "
        "application/x-www-form-urlencoded\"}\n");
    return;
  }

  std::unordered_map<std::string, std::string> fields;
  if (!parseFormBody(&fields)) {
    buildMemoryResponse(400, "Bad Request",
                        "application/json; charset=utf-8",
                        "{\"error\":\"表单请求体格式错误\"}\n");
    return;
  }

  const std::string& username = fields["username"];
  const std::string& password = fields["password"];
  if (username.empty() || username.size() > 64 ||
      password.empty() || password.size() > 1024) {
    buildMemoryResponse(400, "Bad Request",
                        "application/json; charset=utf-8",
                        "{\"error\":\"用户名或密码格式错误\"}\n");
    return;
  }
  if (!user_repository_) {
    buildMemoryResponse(503, "Service Unavailable",
                        "application/json; charset=utf-8",
                        "{\"error\":\"数据库暂不可用\"}\n");
    return;
  }

  switch (user_repository_->login(username, password)) {
    case LoginResult::kSuccess:
      buildMemoryResponse(200, "OK",
                          "application/json; charset=utf-8",
                          "{\"message\":\"登录成功\"}\n");
      return;
    case LoginResult::kInvalidCredentials:
      buildMemoryResponse(
          401, "Unauthorized", "application/json; charset=utf-8",
          "{\"error\":\"用户名或密码错误\"}\n");
      return;
    case LoginResult::kUnavailable:
      buildMemoryResponse(503, "Service Unavailable",
                          "application/json; charset=utf-8",
                          "{\"error\":\"数据库暂不可用\"}\n");
      return;
    case LoginResult::kError:
      buildMemoryResponse(500, "Internal Server Error",
                          "application/json; charset=utf-8",
                          "{\"error\":\"登录失败\"}\n");
      return;
  }
}

bool HttpConnection::parseFormBody(
    std::unordered_map<std::string, std::string>* fields) const {
  fields->clear();
  std::size_t position = 0;
  while (position <= body_.size()) {
    std::size_t end = body_.find('&', position);
    if (end == std::string::npos) {
      end = body_.size();
    }
    std::string pair = body_.substr(position, end - position);
    std::size_t separator = pair.find('=');
    if (separator == std::string::npos) {
      return false;
    }

    std::string name;
    std::string value;
    if (!decodeFormComponent(pair.substr(0, separator), &name) ||
        !decodeFormComponent(pair.substr(separator + 1), &value) ||
        name.empty() || fields->find(name) != fields->end()) {
      return false;
    }
    fields->emplace(std::move(name), std::move(value));

    if (end == body_.size()) {
      break;
    }
    position = end + 1;
  }
  return true;
}

void HttpConnection::buildErrorResponse(int status, const std::string& reason,
                                        const std::string& message) {
  std::string body = std::to_string(status) + " " + reason + "\n" + message +
                     "\n";
  buildMemoryResponse(status, reason, "text/plain; charset=utf-8", body);
}

void HttpConnection::buildMemoryResponse(
    int status, const std::string& reason, const std::string& content_type,
    const std::string& body, const std::string& extra_headers) {
  releaseMappedFile();
  std::ostringstream response;
  response << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << extra_headers
           << "Connection: " << (keep_alive_ ? "keep-alive" : "close")
           << "\r\n\r\n"
           << body;
  write_buffer_ = response.str();
  bytes_written_ = 0;
}

HttpConnection::StaticFileResult HttpConnection::buildStaticFileResponse() {
  releaseMappedFile();
  std::string raw_path = target_;
  std::size_t query = raw_path.find_first_of("?#");
  if (query != std::string::npos) {
    raw_path.resize(query);
  }

  std::string decoded_path;
  if (!decodeUrlPath(raw_path, &decoded_path) || decoded_path.empty() ||
      decoded_path.front() != '/') {
    return StaticFileResult::kBadRequest;
  }
  if (decoded_path == "/") {
    decoded_path = "/index.html";
  }

  std::filesystem::path requested(decoded_path.substr(1));
  for (const auto& part : requested) {
    if (part == "..") {
      return StaticFileResult::kForbidden;
    }
  }
  std::filesystem::path relative = requested.lexically_normal();
  for (const auto& part : relative) {
    if (part == "..") {
      return StaticFileResult::kForbidden;
    }
  }

  std::filesystem::path file_path =
      std::filesystem::path(kDocumentRoot) / relative;
  std::error_code path_error;
  std::filesystem::path canonical_root =
      std::filesystem::canonical(kDocumentRoot, path_error);
  if (path_error) {
    return StaticFileResult::kInternalError;
  }
  std::filesystem::path canonical_file =
      std::filesystem::canonical(file_path, path_error);
  if (path_error) {
    if (path_error == std::errc::no_such_file_or_directory ||
        path_error == std::errc::not_a_directory) {
      return StaticFileResult::kNotFound;
    }
    if (path_error == std::errc::permission_denied) {
      return StaticFileResult::kForbidden;
    }
    return StaticFileResult::kInternalError;
  }
  if (!pathIsWithin(canonical_root, canonical_file)) {
    return StaticFileResult::kForbidden;
  }

  int file_fd = open(canonical_file.c_str(), O_RDONLY | O_CLOEXEC);
  if (file_fd < 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      return StaticFileResult::kNotFound;
    }
    if (errno == EACCES || errno == EPERM || errno == ELOOP) {
      return StaticFileResult::kForbidden;
    }
    return StaticFileResult::kInternalError;
  }

  struct stat file_stat {};
  if (fstat(file_fd, &file_stat) < 0) {
    close(file_fd);
    return StaticFileResult::kInternalError;
  }
  if (!S_ISREG(file_stat.st_mode) ||
      (file_stat.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) == 0) {
    close(file_fd);
    return StaticFileResult::kForbidden;
  }
  if (file_stat.st_size < 0 ||
      static_cast<unsigned long long>(file_stat.st_size) >
          std::numeric_limits<std::size_t>::max()) {
    close(file_fd);
    return StaticFileResult::kInternalError;
  }

  mapped_file_size_ = static_cast<std::size_t>(file_stat.st_size);
  if (mapped_file_size_ > 0) {
    mapped_file_ =
        mmap(nullptr, mapped_file_size_, PROT_READ, MAP_PRIVATE, file_fd, 0);
    if (mapped_file_ == MAP_FAILED) {
      mapped_file_ = nullptr;
      mapped_file_size_ = 0;
      close(file_fd);
      return StaticFileResult::kInternalError;
    }
  }
  close(file_fd);

  std::ostringstream response;
  response << "HTTP/1.1 200 OK\r\n"
           << "Content-Length: " << mapped_file_size_ << "\r\n"
           << "Content-Type: " << contentTypeForPath(file_path.string())
           << "\r\n"
           << "Connection: " << (keep_alive_ ? "keep-alive" : "close")
           << "\r\n\r\n";
  write_buffer_ = response.str();
  bytes_written_ = 0;
  return StaticFileResult::kOk;
}

void HttpConnection::releaseMappedFile() {
  if (mapped_file_) {
    munmap(mapped_file_, mapped_file_size_);
    mapped_file_ = nullptr;
  }
  mapped_file_size_ = 0;
}

void HttpConnection::resetRequest() {
  releaseMappedFile();
  read_buffer_.erase(0, parse_position_);
  write_buffer_.clear();
  parse_position_ = 0;
  content_length_ = 0;
  bytes_written_ = 0;
  method_.clear();
  target_.clear();
  version_.clear();
  body_.clear();
  headers_.clear();
  keep_alive_ = false;
  parse_state_ = ParseState::kRequestLine;
}

std::string HttpConnection::trim(const std::string& value) {
  std::size_t begin = value.find_first_not_of(" \t");
  if (begin == std::string::npos) {
    return "";
  }
  std::size_t end = value.find_last_not_of(" \t");
  return value.substr(begin, end - begin + 1);
}

std::string HttpConnection::toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return std::tolower(ch); });
  return value;
}

bool HttpConnection::decodeFormComponent(const std::string& input,
                                         std::string* output) {
  output->clear();
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '+') {
      output->push_back(' ');
      continue;
    }
    if (input[i] != '%') {
      output->push_back(input[i]);
      continue;
    }
    if (i + 2 >= input.size() ||
        !std::isxdigit(static_cast<unsigned char>(input[i + 1])) ||
        !std::isxdigit(static_cast<unsigned char>(input[i + 2]))) {
      return false;
    }
    auto hex = [](unsigned char ch) {
      return std::isdigit(ch) ? ch - '0' : std::tolower(ch) - 'a' + 10;
    };
    char decoded =
        static_cast<char>((hex(input[i + 1]) << 4) | hex(input[i + 2]));
    if (decoded == '\0') {
      return false;
    }
    output->push_back(decoded);
    i += 2;
  }
  return true;
}

std::string HttpConnection::contentTypeForPath(const std::string& path) {
  std::string extension =
      toLower(std::filesystem::path(path).extension().string());
  if (extension == ".html" || extension == ".htm") {
    return "text/html; charset=utf-8";
  }
  if (extension == ".css") return "text/css; charset=utf-8";
  if (extension == ".js") return "application/javascript; charset=utf-8";
  if (extension == ".json") return "application/json; charset=utf-8";
  if (extension == ".png") return "image/png";
  if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
  if (extension == ".gif") return "image/gif";
  if (extension == ".svg") return "image/svg+xml";
  if (extension == ".mp4") return "video/mp4";
  if (extension == ".webm") return "video/webm";
  if (extension == ".txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

bool HttpConnection::decodeUrlPath(const std::string& input,
                                   std::string* output) {
  output->clear();
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] != '%') {
      output->push_back(input[i]);
      continue;
    }
    if (i + 2 >= input.size() ||
        !std::isxdigit(static_cast<unsigned char>(input[i + 1])) ||
        !std::isxdigit(static_cast<unsigned char>(input[i + 2]))) {
      return false;
    }
    auto hex = [](unsigned char ch) {
      return std::isdigit(ch) ? ch - '0' : std::tolower(ch) - 'a' + 10;
    };
    char decoded =
        static_cast<char>((hex(input[i + 1]) << 4) | hex(input[i + 2]));
    if (decoded == '\0' || decoded == '\\') {
      return false;
    }
    output->push_back(decoded);
    i += 2;
  }
  return true;
}
