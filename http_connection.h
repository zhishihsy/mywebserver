#ifndef HTTP_CONNECTION_H
#define HTTP_CONNECTION_H

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

class UserRepository;

class HttpConnection {
 public:
  enum class ReadResult {
    kDataReady,
    kPeerClosed,
    kError,
  };

  enum class ProcessResult {
    kNeedMoreData,
    kResponseReady,
  };

  enum class WriteResult {
    kWantRead,
    kWantWrite,
    kWantProcess,
    kClose,
    kError,
  };

  explicit HttpConnection(
      std::shared_ptr<UserRepository> user_repository = nullptr);
  ~HttpConnection();

  HttpConnection(const HttpConnection&) = delete;
  HttpConnection& operator=(const HttpConnection&) = delete;

  ReadResult readFromSocket(int sockfd, bool edge_triggered);
  ProcessResult processRequest();
  WriteResult writeToSocket(int sockfd);

 private:
  enum class ParseState {
    kRequestLine,
    kHeaders,
    kBody,
  };

  enum class ParseResult {
    kIncomplete,
    kComplete,
    kBadRequest,
    kPayloadTooLarge,
  };

  enum class StaticFileResult {
    kOk,
    kBadRequest,
    kForbidden,
    kNotFound,
    kInternalError,
  };

  ParseResult parseRequest();
  bool parseRequestLine(const std::string& line);
  bool parseHeader(const std::string& line);
  void buildResponse();
  void handleRegister();
  void handleLogin();
  bool parseFormBody(
      std::unordered_map<std::string, std::string>* fields) const;
  void buildErrorResponse(int status, const std::string& reason,
                          const std::string& message);
  void buildMemoryResponse(int status, const std::string& reason,
                           const std::string& content_type,
                           const std::string& body,
                           const std::string& extra_headers = "");
  StaticFileResult buildStaticFileResponse();
  void releaseMappedFile();
  void resetRequest();

  static std::string trim(const std::string& value);
  static std::string toLower(std::string value);
  static bool decodeFormComponent(const std::string& input,
                                  std::string* output);
  static std::string contentTypeForPath(const std::string& path);
  static bool decodeUrlPath(const std::string& input, std::string* output);

  ParseState parse_state_;
  std::string read_buffer_;
  std::string write_buffer_;
  std::size_t parse_position_;
  std::size_t content_length_;
  std::size_t bytes_written_;
  void* mapped_file_;
  std::size_t mapped_file_size_;

  std::string method_;
  std::string target_;
  std::string version_;
  std::string body_;
  std::unordered_map<std::string, std::string> headers_;
  bool keep_alive_;
  std::shared_ptr<UserRepository> user_repository_;
};

#endif
