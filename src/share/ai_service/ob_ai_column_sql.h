#pragma once

#include "lib/string/ob_string.h"
#include <string>

namespace oceanbase { namespace share {
class AiColumnSQL
{
public:
  static std::string text(const common::ObString &s)
  { return s.empty() ? std::string() : std::string(s.ptr(), s.length()); }
  static std::string ident(const common::ObString &s) { return ident(text(s)); }
  static std::string ident(const std::string &s)
  {
    std::string out = "`";
    for (char c : s) { out += c; if (c == '`') { out += c; } }
    return out + "`";
  }
  // Hex literals preserve embedded quotes, backslashes and NUL independently of sql_mode.
  static std::string literal(const common::ObString &s) { return literal(text(s)); }
  static std::string literal(const std::string &s)
  {
    static const char hex[] = "0123456789abcdef";
    std::string out = "CONVERT(X'";
    for (unsigned char c : s) { out += hex[c >> 4]; out += hex[c & 15]; }
    return out + "' USING utf8mb4)";
  }
};
} }
