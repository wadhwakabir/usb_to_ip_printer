#pragma once

#include <stdint.h>

#include <cstdio>
#include <string>

namespace http_status_utils {

enum class RequestRoute {
  kBadRequest,
  kRoot,
  kStatusJson,
  kNotFound,
};

inline std::string EscapeHtml(const std::string &input) {
  std::string output;
  output.reserve(input.size() + 16);
  for (const char c : input) {
    switch (c) {
      case '&':
        output += "&amp;";
        break;
      case '<':
        output += "&lt;";
        break;
      case '>':
        output += "&gt;";
        break;
      case '"':
        output += "&quot;";
        break;
      default:
        output += c;
        break;
    }
  }
  return output;
}

inline std::string EscapeJson(const std::string &input) {
  std::string output;
  output.reserve(input.size() + 16);
  for (const char c : input) {
    switch (c) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output += c;
        break;
    }
  }
  return output;
}

inline RequestRoute ClassifyRequestLine(const std::string &request_line) {
  if (request_line.rfind("GET ", 0) != 0) {
    return RequestRoute::kBadRequest;
  }

  const size_t first_space = request_line.find(' ');
  if (first_space == std::string::npos) {
    return RequestRoute::kBadRequest;
  }

  const size_t second_space = request_line.find(' ', first_space + 1);
  if (second_space == std::string::npos) {
    return RequestRoute::kBadRequest;
  }

  const std::string path =
      request_line.substr(first_space + 1, second_space - first_space - 1);
  const std::string version = request_line.substr(second_space + 1);

  if (version != "HTTP/1.0" && version != "HTTP/1.1") {
    return RequestRoute::kBadRequest;
  }

  if (path == "/") {
    return RequestRoute::kRoot;
  }
  if (path == "/status.json") {
    return RequestRoute::kStatusJson;
  }
  return RequestRoute::kNotFound;
}

inline std::string FormatHex16(uint16_t value) {
  char buffer[5];
  std::snprintf(buffer, sizeof(buffer), "%04x", value);
  return std::string(buffer);
}

}  // namespace http_status_utils
