#include "http_connection.h"

#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace {
constexpr std::size_t kReadChunkSize = 4096;
constexpr std::size_t kMaxRequestSize = 64 * 1024;
const char kDocumentRoot[] = "resources";
}  // namespace

HttpConnection::HttpConnection()
    : parse_state_(ParseState::kRequestLine),
      parse_position_(0),
      content_length_(0),
      bytes_written_(0),
      keep_alive_(false) {}

HttpConnection::ReadResult HttpConnection::readFromSocket(
    int sockfd, bool edge_triggered) {
  char buffer[kReadChunkSize];

  while (true) {
    ssize_t bytes_read = recv(sockfd, buffer, sizeof(buffer), 0);
    if (bytes_read > 0) {
      read_buffer_.append(buffer, static_cast<std::size_t>(bytes_read));
      if (read_buffer_.size() > kMaxRequestSize) {
        buildErrorResponse(413, "Payload Too Large", "Request is too large.");
        return ReadResult::kResponseReady;
      }
      if (!edge_triggered) {
        break;
      }
      continue;
    }

    if (bytes_read == 0) {
      return ReadResult::kPeerClosed;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    return ReadResult::kError;
  }

  ParseResult result = parseRequest();
  if (result == ParseResult::kIncomplete) {
    return ReadResult::kNeedMoreData;
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
  return ReadResult::kResponseReady;
}

HttpConnection::WriteResult HttpConnection::writeToSocket(int sockfd) {
  while (bytes_written_ < write_buffer_.size()) {
    const char* data = write_buffer_.data() + bytes_written_;
    std::size_t remaining = write_buffer_.size() - bytes_written_;
    ssize_t bytes_sent = send(sockfd, data, remaining, MSG_NOSIGNAL);
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

  if (!keep_alive_) {
    return WriteResult::kClose;
  }

  resetRequest();
  ParseResult result = parseRequest();
  if (result == ParseResult::kComplete) {
    buildResponse();
    return WriteResult::kWantWrite;
  }
  if (result == ParseResult::kBadRequest) {
    keep_alive_ = false;
    buildErrorResponse(400, "Bad Request", "Malformed HTTP request.");
    return WriteResult::kWantWrite;
  }
  if (result == ParseResult::kPayloadTooLarge) {
    keep_alive_ = false;
    buildErrorResponse(413, "Payload Too Large", "Request is too large.");
    return WriteResult::kWantWrite;
  }
  return WriteResult::kWantRead;
}

HttpConnection::ParseResult HttpConnection::parseRequest() {
  while (true) {
    if (parse_state_ == ParseState::kBody) {
      if (content_length_ > kMaxRequestSize) {
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
      return ParseResult::kIncomplete;
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
      auto length = headers_.find("content-length");
      if (length != headers_.end()) {
        try {
          std::size_t consumed = 0;
          content_length_ = std::stoull(length->second, &consumed);
          if (consumed != length->second.size()) {
            return ParseResult::kBadRequest;
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
  return !target_.empty() &&
         (version_ == "HTTP/1.0" || version_ == "HTTP/1.1");
}

bool HttpConnection::parseHeader(const std::string& line) {
  std::size_t separator = line.find(':');
  if (separator == std::string::npos || separator == 0) {
    return false;
  }
  std::string name = toLower(trim(line.substr(0, separator)));
  if (name.empty()) {
    return false;
  }
  headers_[name] = trim(line.substr(separator + 1));
  return true;
}

void HttpConnection::buildResponse() {
  if (method_ != "GET") {
    buildErrorResponse(405, "Method Not Allowed", "Only GET is supported.");
    return;
  }
  if (target_ == "/health") {
    buildMemoryResponse(200, "OK", "text/plain; charset=utf-8", "OK\n");
    return;
  }
  if (!buildStaticFileResponse()) {
    buildErrorResponse(404, "Not Found", "The requested file was not found.");
  }
}

void HttpConnection::buildErrorResponse(int status, const std::string& reason,
                                        const std::string& message) {
  std::string body = std::to_string(status) + " " + reason + "\n" + message +
                     "\n";
  buildMemoryResponse(status, reason, "text/plain; charset=utf-8", body);
}

void HttpConnection::buildMemoryResponse(
    int status, const std::string& reason, const std::string& content_type,
    const std::string& body) {
  std::ostringstream response;
  response << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Connection: " << (keep_alive_ ? "keep-alive" : "close")
           << "\r\n\r\n"
           << body;
  write_buffer_ = response.str();
  bytes_written_ = 0;
}

bool HttpConnection::buildStaticFileResponse() {
  std::string raw_path = target_;
  std::size_t query = raw_path.find_first_of("?#");
  if (query != std::string::npos) {
    raw_path.resize(query);
  }

  std::string decoded_path;
  if (!decodeUrlPath(raw_path, &decoded_path) || decoded_path.empty() ||
      decoded_path.front() != '/') {
    return false;
  }
  if (decoded_path == "/") {
    decoded_path = "/index.html";
  }

  std::filesystem::path relative =
      std::filesystem::path(decoded_path.substr(1)).lexically_normal();
  for (const auto& part : relative) {
    if (part == "..") {
      return false;
    }
  }

  std::filesystem::path file_path =
      std::filesystem::path(kDocumentRoot) / relative;
  std::ifstream file(file_path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::string body((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  buildMemoryResponse(200, "OK", contentTypeForPath(file_path.string()), body);
  return true;
}

void HttpConnection::resetRequest() {
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
