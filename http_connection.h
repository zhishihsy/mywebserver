#ifndef HTTP_CONNECTION_H
#define HTTP_CONNECTION_H

#include <cstddef>
#include <string>
#include <unordered_map>

class HttpConnection {
 public:
  enum class ReadResult {
    kNeedMoreData,
    kResponseReady,
    kPeerClosed,
    kError,
  };

  enum class WriteResult {
    kWantRead,
    kWantWrite,
    kClose,
    kError,
  };

  HttpConnection();

  ReadResult readFromSocket(int sockfd, bool edge_triggered);
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

  ParseResult parseRequest();
  bool parseRequestLine(const std::string& line);
  bool parseHeader(const std::string& line);
  void buildResponse();
  void buildErrorResponse(int status, const std::string& reason,
                          const std::string& message);
  void buildMemoryResponse(int status, const std::string& reason,
                           const std::string& content_type,
                           const std::string& body);
  bool buildStaticFileResponse();
  void resetRequest();

  static std::string trim(const std::string& value);
  static std::string toLower(std::string value);
  static std::string contentTypeForPath(const std::string& path);
  static bool decodeUrlPath(const std::string& input, std::string* output);

  ParseState parse_state_;
  std::string read_buffer_;
  std::string write_buffer_;
  std::size_t parse_position_;
  std::size_t content_length_;
  std::size_t bytes_written_;

  std::string method_;
  std::string target_;
  std::string version_;
  std::string body_;
  std::unordered_map<std::string, std::string> headers_;
  bool keep_alive_;
};

#endif
