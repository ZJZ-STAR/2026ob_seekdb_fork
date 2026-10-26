/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#pragma once

// Local file readers. They produce rows only; relational operations belong to
// the seekdb executor. Memory is bounded by metadata and one record/page per
// projected column, independent of the number of rows in the input file.
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace oceanbase {
namespace sql {
namespace file_sql {
static const size_t MAX_RECORD = 16 * 1024 * 1024;
static const size_t MAX_PAGE = 16 * 1024 * 1024;
static const size_t MAX_COLUMNS = 1024;
static const size_t MAX_METADATA = 16 * 1024 * 1024;
static const size_t MAX_SCAN_MEMORY = 128 * 1024 * 1024;
enum Type { NIL, INT, REAL, BOOL, TEXT, DATE, DATETIME };
inline const char *type_name(Type t) {
  static const char *names[] = {"NULL",    "BIGINT", "DOUBLE",  "BOOLEAN",
                                "VARCHAR", "DATE",   "DATETIME"};
  return names[t];
}
struct Cell {
  std::string text;
  bool null = true;
  Type hint = NIL;
  bool json_string = false;
  Cell() = default;
  explicit Cell(std::string s, Type t = TEXT)
      : text(std::move(s)), null(false), hint(t) {}
};
struct Column {
  std::string name;
  Type type = NIL;
  bool nullable = false;
  int scale = 0;
};
struct Options {
  std::string format = "csv", nullstr = "";
  char delimiter = ',', quote = '"', escape = '"';
  bool header = true, all_varchar = false;
};
inline bool valid_root(const std::string &root) {
  if (root.empty() || root.find('\0') != std::string::npos)
    return false;
  std::string key = root;
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return key != "null";
}
inline bool utf8(const std::string &s) {
  for (size_t i = 0; i < s.size();) {
    uint32_t c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80)
      continue;
    unsigned n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : 99;
    if (n == 99 || i + n > s.size())
      return false;
    uint32_t v = c & ((1U << (6 - n)) - 1);
    for (unsigned j = 0; j < n; ++j) {
      c = static_cast<unsigned char>(s[i++]);
      if ((c & 0xc0) != 0x80)
        return false;
      v = (v << 6) | (c & 63);
    }
    if ((n == 1 && v < 128) || (n == 2 && v < 2048) || (n == 3 && v < 65536) ||
        v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
      return false;
  }
  return true;
}
inline bool integer(const std::string &s, int64_t &v) {
  if (s.empty())
    return false;
  size_t p = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (p == s.size())
    return false;
  for (size_t i = p; i < s.size(); ++i)
    if (s[i] < '0' || s[i] > '9')
      return false;
  char *end = nullptr;
  errno = 0;
  v = strtoll(s.c_str(), &end, 10);
  return errno != ERANGE && end == s.c_str() + s.size();
}
inline bool real(const std::string &s, double &v) {
  // No locale-specific whitespace, NaN, Inf or hexadecimal numbers.
  if (s.empty())
    return false;
  size_t p = (s[0] == '-' || s[0] == '+') ? 1 : 0, digits = 0;
  while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
    ++p;
    ++digits;
  }
  if (p < s.size() && s[p] == '.') {
    ++p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
      ++p;
      ++digits;
    }
  }
  if (!digits)
    return false;
  if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
    ++p;
    if (p < s.size() && (s[p] == '+' || s[p] == '-'))
      ++p;
    size_t start = p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9')
      ++p;
    if (start == p)
      return false;
  }
  if (p != s.size())
    return false;
  errno = 0;
  char *end = nullptr;
  v = strtod(s.c_str(), &end);
  return errno != ERANGE && std::isfinite(v) && end == s.c_str() + s.size();
}
inline int digits_at(const std::string &s, size_t p, size_t n) {
  int v = 0;
  if (p + n > s.size())
    return -1;
  for (size_t i = p; i < p + n; ++i) {
    if (s[i] < '0' || s[i] > '9')
      return -1;
    v = v * 10 + s[i] - '0';
  }
  return v;
}
inline bool date_value(const std::string &s, int64_t &micros) {
  if (s.size() != 10 && s.size() != 19 && !(s.size() >= 21 && s.size() <= 26))
    return false;
  int y = digits_at(s, 0, 4), m = digits_at(s, 5, 2), d = digits_at(s, 8, 2);
  static const int md[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (s[4] != '-' || s[7] != '-' || y < 1 || m < 1 || m > 12 || d < 1 ||
      d > md[m] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
    return false;
  int h = 0, mi = 0, sec = 0, us = 0;
  if (s.size() > 10) {
    h = digits_at(s, 11, 2);
    mi = digits_at(s, 14, 2);
    sec = digits_at(s, 17, 2);
    if ((s[10] != 'T' && s[10] != ' ') || s[13] != ':' || s[16] != ':' ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59)
      return false;
    if (s.size() > 19) {
      if (s[19] != '.')
        return false;
      us = digits_at(s, 20, s.size() - 20);
      if (us < 0)
        return false;
      for (size_t i = s.size() - 20; i < 6; ++i)
        us *= 10;
    }
  }
  y -= m <= 2;
  int era = y / 400, yo = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t days = era * 146097 + yo * 365 + yo / 4 - yo / 100 + doy - 719468;
  micros = ((days * 24 + h) * 3600 + mi * 60 + sec) * 1000000 + us;
  return true;
}
inline Type infer(const Cell &v) {
  if (v.null)
    return NIL;
  if (v.hint == BOOL)
    return BOOL;
  int64_t i = 0;
  if (v.json_string) {
    if (date_value(v.text, i))
      return v.text.size() == 10 ? DATE : DATETIME;
    return TEXT;
  }
  if (v.text == "true" || v.text == "false" || v.text == "TRUE" ||
      v.text == "FALSE")
    return BOOL;
  if (integer(v.text, i))
    return INT;
  // Integral overflow stays VARCHAR instead of losing precision as DOUBLE.
  if (v.text.find_first_of(".eE") != std::string::npos) {
    double d;
    if (real(v.text, d))
      return REAL;
  }
  if (date_value(v.text, i))
    return v.text.size() == 10 ? DATE : DATETIME;
  return TEXT;
}
inline Type widen(Type a, Type b) {
  if (a == NIL)
    return b;
  if (b == NIL || a == b)
    return a;
  if ((a == INT && b == REAL) || (a == REAL && b == INT))
    return REAL;
  if ((a == DATE && b == DATETIME) || (a == DATETIME && b == DATE))
    return DATETIME;
  return TEXT;
}
struct Fingerprint {
  uint64_t dev = 0, ino = 0, size = 0, signature = 0;
  int64_t mt = 0, mn = 0, ct = 0, cn = 0;
  static Fingerprint from(const struct stat &s) {
    Fingerprint f;
    f.dev = s.st_dev;
    f.ino = s.st_ino;
    f.size = s.st_size;
    f.mt = s.st_mtim.tv_sec;
    f.mn = s.st_mtim.tv_nsec;
    f.ct = s.st_ctim.tv_sec;
    f.cn = s.st_ctim.tv_nsec;
    return f;
  }
  bool operator==(const Fingerprint &f) const {
    return dev == f.dev && ino == f.ino && size == f.size && mt == f.mt &&
           mn == f.mn && ct == f.ct && cn == f.cn && signature == f.signature;
  }
};
class ChangeWatch {
public:
  ~ChangeWatch() { close(); }
  bool open(const std::string &path, bool directory = false) {
    close();
    fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    uint32_t mask = IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB;
    mask |= directory ? IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO
                      : IN_MODIFY | IN_CLOSE_WRITE;
    if (fd_ < 0 || inotify_add_watch(fd_, path.c_str(), mask) < 0) {
      close();
      return false;
    }
    return true;
  }
  void close() {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = -1;
    changed_ = false;
  }
  bool unchanged() {
    if (fd_ < 0 || changed_)
      return false;
    char events[4096];
    ssize_t n;
    do {
      n = ::read(fd_, events, sizeof(events));
    } while (n < 0 && errno == EINTR);
    if (n > 0 || n == 0 || (n < 0 && errno != EAGAIN))
      changed_ = true;
    return !changed_;
  }

private:
  int fd_ = -1;
  bool changed_ = false;
};
class Input {
public:
  Input() = default;
  ~Input() { close(); }
  Input(const Input &) = delete;
  Input &operator=(const Input &) = delete;
  int fd = -1;
  std::string path, error;
  Fingerprint fingerprint;
  ChangeWatch watch;
  uint64_t offset = 0, line = 1, record_line = 1;
  void close() {
    watch.close();
    if (fd >= 0)
      ::close(fd);
    fd = -1;
    pos_ = end_ = 0;
  }
  bool fail(const std::string &s) {
    error = path + ": line " + std::to_string(record_line) + ": " + s;
    return false;
  }
  bool open(const std::string &p, const std::string &root,
            bool hash_content = true) {
    close();
    path = p;
    error.clear();
    offset = 0;
    line = 1;
    record_line = 1;
    char rp[4096], rr[4096];
    if (p.find('\0') != std::string::npos || !valid_root(root) ||
        !realpath(root.c_str(), rr) || !realpath(p.c_str(), rp))
      return fail("path is unavailable or secure_file_priv denies access");
    std::string r(rr), actual(rp);
    if (r != "/" && actual != r &&
        actual.compare(0, r.size() + 1, r + "/") != 0)
      return fail("path is outside secure_file_priv");
    // Open relative to a pinned directory, rejecting symlinks at every
    // component. This also closes the realpath/open TOCTOU window.
    int dir = ::open(rr, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0)
      return fail("cannot open secure directory");
    std::string rel = actual.substr(r == "/" ? 1 : r.size() + 1);
    size_t start = 0;
    for (;;) {
      size_t slash = rel.find('/', start);
      std::string part =
          rel.substr(start, slash == std::string::npos ? slash : slash - start);
      int next = ::openat(dir, part.c_str(),
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                              (slash == std::string::npos ? 0 : O_DIRECTORY));
      ::close(dir);
      if (next < 0)
        return fail("cannot open regular file without following symlinks");
      if (slash == std::string::npos) {
        fd = next;
        break;
      }
      dir = next;
      start = slash + 1;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
      close();
      return fail("input must be a regular file");
    }
    fingerprint = Fingerprint::from(st);
    path = actual;
    if (!watch.open("/proc/self/fd/" + std::to_string(fd)))
      return fail("cannot watch file changes");
    if (hash_content) {
      uint64_t h = 1469598103934665603ULL, off = 0;
      char block[64 * 1024];
      while (off < fingerprint.size) {
        ssize_t n = pread(
            fd, block,
            std::min<uint64_t>(sizeof(block), fingerprint.size - off), off);
        if (n < 0 && errno == EINTR)
          continue;
        if (n <= 0)
          return fail("file changed while fingerprinting");
        for (ssize_t i = 0; i < n; ++i) {
          h ^= uint8_t(block[i]);
          h *= 1099511628211ULL;
        }
        off += n;
      }
      fingerprint.signature = h;
    }
    return check();
  }
  bool check() {
    struct stat a, b;
    if (fd < 0 || fstat(fd, &a) || stat(path.c_str(), &b) || !watch.unchanged())
      return fail("file changed during query");
    Fingerprint fa = Fingerprint::from(a), fb = Fingerprint::from(b);
    fa.signature = fb.signature = fingerprint.signature;
    if (!(fingerprint == fa) || !(fingerprint == fb))
      return fail("file changed during query");
    return true;
  }
  bool rewind() {
    if (!check() || lseek(fd, 0, SEEK_SET) != 0)
      return fail("cannot rescan file");
    pos_ = end_ = 0;
    offset = 0;
    line = 1;
    record_line = 1;
    return true;
  }
  bool skip_bom() {
    if (offset == 0 && peek() == 0xef) {
      get();
      if (get() != 0xbb || get() != 0xbf)
        return fail("invalid UTF-8 BOM");
    }
    return true;
  }
  int get() {
    if (pos_ == end_) {
      ssize_t n;
      do {
        n = ::read(fd, buf_, sizeof(buf_));
      } while (n < 0 && errno == EINTR);
      if (n <= 0) {
        if (n < 0)
          fail("read failed");
        return -1;
      }
      pos_ = 0;
      end_ = static_cast<size_t>(n);
    }
    ++offset;
    int c = static_cast<unsigned char>(buf_[pos_++]);
    if (c == '\n')
      ++line;
    return c;
  }
  int peek() {
    int c = get();
    if (c >= 0) {
      --pos_;
      --offset;
      if (c == '\n')
        --line;
    }
    return c;
  }
  bool at(uint64_t off, size_t n, std::string &out) {
    if (n > MAX_PAGE || off > fingerprint.size || n > fingerprint.size - off)
      return fail("invalid or oversized file section");
    out.resize(n);
    size_t got = 0;
    while (got < n) {
      ssize_t k = pread(fd, &out[got], n - got, off + got);
      if (k < 0 && errno == EINTR)
        continue;
      if (k <= 0)
        return fail("truncated file");
      got += k;
    }
    return true;
  }

private:
  char buf_[64 * 1024];
  size_t pos_ = 0, end_ = 0;
};

inline bool csv_row(Input &in, const Options &o, std::vector<Cell> &row,
                    bool &eof) {
  in.record_line = in.line;
  row.clear();
  eof = false;
  size_t bytes = 0;
  if (in.peek() < 0) {
    eof = true;
    return in.error.empty() && in.check();
  }
  for (;;) {
    Cell v;
    v.null = false;
    bool quoted = false, ended = false;
    if (o.quote && in.peek() == o.quote) {
      quoted = true;
      in.get();
    }
    if (quoted) {
      for (;;) {
        int c = in.get();
        if (c < 0)
          return in.fail("unterminated quoted CSV field");
        if (++bytes > MAX_RECORD)
          return in.fail("CSV record exceeds 16 MiB");
        if (c == o.quote) {
          if (o.escape == o.quote && in.peek() == o.quote) {
            in.get();
            v.text += o.quote;
          } else {
            ended = true;
            break;
          }
        } else if (o.escape && o.escape != o.quote && c == o.escape) {
          int d = in.get();
          if (d < 0)
            return in.fail("incomplete CSV escape");
          v.text += static_cast<char>(d);
        } else if (c == '\r' && in.peek() == '\n') {
          in.get();
          v.text += '\n';
        } else
          v.text += static_cast<char>(c);
      }
      int c = in.peek();
      if (c >= 0 && c != o.delimiter && c != '\r' && c != '\n')
        return in.fail("unexpected character after closing CSV quote");
    } else {
      while (in.peek() >= 0 && in.peek() != o.delimiter && in.peek() != '\r' &&
             in.peek() != '\n') {
        int c = in.get();
        if (o.quote && c == o.quote)
          return in.fail("quote inside unquoted CSV field");
        v.text += static_cast<char>(c);
        if (++bytes > MAX_RECORD)
          return in.fail("CSV record exceeds 16 MiB");
      }
    }
    (void)ended;
    if (!utf8(v.text))
      return in.fail("invalid UTF-8 in CSV field");
    v.null = !quoted && (v.text.empty() || v.text == o.nullstr);
    row.push_back(std::move(v));
    if (row.size() > MAX_COLUMNS)
      return in.fail("too many CSV columns");
    int c = in.get();
    if (c == o.delimiter)
      continue;
    if (c == '\r' && in.peek() == '\n')
      in.get();
    return in.error.empty() && in.check();
  }
}

// A strict flat JSON parser, with decoded keys and strings and bounded records.
class FlatJson {
public:
  explicit FlatJson(const std::string &s) : s_(s) {}
  std::string error;
  bool parse(std::vector<std::pair<std::string, Cell>> &out) {
    out.clear();
    ws();
    if (!take('{'))
      return fail("JSONL record must be an object");
    ws();
    if (take('}')) {
      ws();
      return p_ == s_.size() || fail("trailing JSON data");
    }
    std::set<std::string> keys;
    for (;;) {
      ws();
      std::string key;
      if (!str(key))
        return false;
      ws();
      if (!take(':'))
        return fail("expected ':'");
      ws();
      if (!keys.insert(key).second)
        return fail("duplicate JSON key");
      Cell c;
      if (p_ == s_.size())
        return fail("missing JSON value");
      if (s_[p_] == '"') {
        c.null = false;
        c.hint = TEXT;
        c.json_string = true;
        if (!str(c.text))
          return false;
      } else if (s_[p_] == '{' || s_[p_] == '[')
        return fail("nested JSON objects and arrays are unsupported");
      else {
        size_t begin = p_;
        while (p_ < s_.size() && s_[p_] != ',' && s_[p_] != '}' &&
               s_[p_] != ' ' && s_[p_] != '\t' && s_[p_] != '\r')
          ++p_;
        std::string v = s_.substr(begin, p_ - begin);
        if (v == "null")
          c = Cell();
        else if (v == "true" || v == "false")
          c = Cell(v, BOOL);
        else {
          size_t q = 0;
          if (q < v.size() && v[q] == '-')
            ++q;
          if (q == v.size() || v[q] < '0' || v[q] > '9' ||
              (v[q] == '0' && q + 1 < v.size() && v[q + 1] >= '0' &&
               v[q + 1] <= '9'))
            return fail("invalid JSON number");
          // JSON does not allow a leading plus, .5 or 1.
          double d;
          if (!real(v, d) || v.back() == '.' ||
              v.find(".e") != std::string::npos ||
              v.find(".E") != std::string::npos)
            return fail("invalid JSON scalar");
          c = Cell(v, REAL);
        }
      }
      out.emplace_back(std::move(key), std::move(c));
      if (out.size() > MAX_COLUMNS)
        return fail("too many JSON fields");
      ws();
      if (take('}'))
        break;
      if (!take(','))
        return fail("expected ',' or '}'");
    }
    ws();
    return p_ == s_.size() || fail("trailing JSON data");
  }

private:
  const std::string &s_;
  size_t p_ = 0;
  bool fail(const char *s) {
    error = std::string(s) + " at byte " + std::to_string(p_ + 1);
    return false;
  }
  void ws() {
    while (p_ < s_.size() &&
           (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r'))
      ++p_;
  }
  bool take(char c) {
    if (p_ < s_.size() && s_[p_] == c) {
      ++p_;
      return true;
    }
    return false;
  }
  int hex4() {
    if (p_ + 4 > s_.size())
      return -1;
    int x = 0;
    for (int j = 0; j < 4; ++j) {
      char c = s_[p_++];
      int v = c >= '0' && c <= '9'   ? c - '0'
              : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                     : -1;
      if (v < 0)
        return -1;
      x = x * 16 + v;
    }
    return x;
  }
  void append(std::string &v, uint32_t c) {
    if (c < 128)
      v += char(c);
    else if (c < 2048) {
      v += char(0xc0 | (c >> 6));
      v += char(0x80 | (c & 63));
    } else if (c < 65536) {
      v += char(0xe0 | (c >> 12));
      v += char(0x80 | ((c >> 6) & 63));
      v += char(0x80 | (c & 63));
    } else {
      v += char(0xf0 | (c >> 18));
      v += char(0x80 | ((c >> 12) & 63));
      v += char(0x80 | ((c >> 6) & 63));
      v += char(0x80 | (c & 63));
    }
  }
  bool str(std::string &v) {
    if (!take('"'))
      return fail("expected JSON string");
    while (p_ < s_.size()) {
      unsigned char c = s_[p_++];
      if (c == '"')
        return utf8(v) || fail("invalid UTF-8");
      if (c < 32)
        return fail("control character in JSON string");
      if (c != '\\') {
        v += char(c);
        continue;
      }
      if (p_ == s_.size())
        return fail("incomplete JSON escape");
      c = s_[p_++];
      switch (c) {
      case '"':
      case '\\':
      case '/':
        v += char(c);
        break;
      case 'b':
        v += '\b';
        break;
      case 'f':
        v += '\f';
        break;
      case 'n':
        v += '\n';
        break;
      case 'r':
        v += '\r';
        break;
      case 't':
        v += '\t';
        break;
      case 'u': {
        int u = hex4();
        if (u < 0)
          return fail("invalid Unicode escape");
        uint32_t cp = u;
        if (u >= 0xd800 && u <= 0xdbff) {
          if (!take('\\') || !take('u'))
            return fail("missing low surrogate");
          int lo = hex4();
          if (lo < 0xdc00 || lo > 0xdfff)
            return fail("invalid low surrogate");
          cp = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
        } else if (u >= 0xdc00 && u <= 0xdfff)
          return fail("unpaired low surrogate");
        append(v, cp);
        break;
      }
      default:
        return fail("unknown JSON escape");
      }
    }
    return fail("unterminated JSON string");
  }
};
inline bool json_row(Input &in, std::vector<std::pair<std::string, Cell>> &row,
                     bool &eof) {
  std::string line;
  eof = false;
  for (;;) {
    in.record_line = in.line;
    if (in.peek() < 0) {
      eof = true;
      return in.error.empty() && in.check();
    }
    line.clear();
    int c;
    while ((c = in.get()) >= 0 && c != '\n') {
      line += char(c);
      if (line.size() > MAX_RECORD)
        return in.fail("JSONL record exceeds 16 MiB");
    }
    if (line.find_first_not_of(" \t\r") == std::string::npos)
      continue;
    FlatJson parser(line);
    if (!parser.parse(row))
      return in.fail(parser.error);
    return in.check();
  }
}

// Parquet's compact Thrift metadata. Length and recursion limits are enforced
// before allocation. Unknown fields are skipped according to their wire type.
struct Thrift {
  int64_t number = 0;
  std::string bytes;
  std::map<int, Thrift> fields;
  std::vector<Thrift> list;
  const Thrift &get(int id) const {
    static const Thrift empty;
    auto i = fields.find(id);
    return i == fields.end() ? empty : i->second;
  }
};
class Compact {
public:
  const std::string &s;
  size_t p = 0, nodes = 0;
  bool ok = true;
  explicit Compact(const std::string &v) : s(v) {}
  uint8_t byte() {
    if (p >= s.size()) {
      ok = false;
      return 0;
    }
    return static_cast<uint8_t>(s[p++]);
  }
  uint64_t var() {
    uint64_t x = 0;
    for (unsigned b = 0; b < 64; b += 7) {
      uint8_t c = byte();
      if (!ok || (b == 63 && c > 1)) {
        ok = false;
        return 0;
      }
      x |= uint64_t(c & 127) << b;
      if (!(c & 128))
        return x;
    }
    ok = false;
    return 0;
  }
  int64_t zig() {
    uint64_t x = var();
    return (x >> 1) ^ -static_cast<int64_t>(x & 1);
  }
  Thrift value(int type, int depth = 0) {
    Thrift v;
    if (depth > 16 || ++nodes > 200000) {
      ok = false;
      return v;
    }
    if (type == 1 || type == 2)
      v.number = type == 1;
    else if (type == 3)
      v.number = static_cast<int8_t>(byte());
    else if (type >= 4 && type <= 6)
      v.number = zig();
    else if (type == 7) {
      if (p + 8 > s.size())
        ok = false;
      else
        p += 8;
    } else if (type == 8) {
      uint64_t n = var();
      if (n > MAX_METADATA || n > s.size() - p)
        ok = false;
      else {
        v.bytes = s.substr(p, n);
        p += n;
      }
    } else if (type == 9 || type == 10) {
      uint8_t h = byte();
      uint64_t n = h >> 4;
      if (n == 15)
        n = var();
      if (n > 200000 || n > s.size() - p) {
        ok = false;
        return v;
      }
      for (uint64_t j = 0; ok && j < n; ++j)
        v.list.push_back(value(h & 15, depth + 1));
    } else if (type == 11) {
      uint64_t n = var();
      uint8_t h = n ? byte() : 0;
      if (n > 200000) {
        ok = false;
        return v;
      }
      for (uint64_t j = 0; ok && j < n; ++j) {
        value(h >> 4, depth + 1);
        value(h & 15, depth + 1);
      }
    } else if (type == 12) {
      int last = 0;
      for (;;) {
        uint8_t h = byte();
        if (!ok || h == 0)
          break;
        int id = (h >> 4) ? last + (h >> 4) : int(zig());
        last = id;
        v.fields[id] = value(h & 15, depth + 1);
        if (!ok)
          break;
      }
    } else
      ok = false;
    return v;
  }
};
inline uint32_t le32(const char *p) {
  return uint32_t(uint8_t(p[0])) | (uint32_t(uint8_t(p[1])) << 8) |
         (uint32_t(uint8_t(p[2])) << 16) | (uint32_t(uint8_t(p[3])) << 24);
}
inline uint64_t le64(const char *p) {
  return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32);
}
inline bool snappy(const std::string &src, std::string &out, size_t expected) {
  size_t p = 0;
  uint64_t n = 0;
  unsigned shift = 0;
  for (;;) {
    if (p == src.size() || shift > 28)
      return false;
    uint8_t c = src[p++];
    n |= uint64_t(c & 127) << shift;
    if (!(c & 128))
      break;
    shift += 7;
  }
  if (n != expected || n > MAX_PAGE)
    return false;
  out.clear();
  out.reserve(n);
  while (p < src.size() && out.size() < n) {
    uint8_t tag = src[p++];
    size_t len = 0, off = 0;
    if ((tag & 3) == 0) {
      len = tag >> 2;
      if (len < 60)
        ++len;
      else {
        unsigned k = len - 59;
        if (k > 4 || p + k > src.size())
          return false;
        len = 0;
        for (unsigned i = 0; i < k; ++i)
          len |= size_t(uint8_t(src[p++])) << (8 * i);
        ++len;
      }
      if (len > n - out.size() || len > src.size() - p)
        return false;
      out.append(src, p, len);
      p += len;
    } else {
      if ((tag & 3) == 1) {
        len = 4 + ((tag >> 2) & 7);
        if (p == src.size())
          return false;
        off = ((tag & 0xe0) << 3) | uint8_t(src[p++]);
      } else {
        len = 1 + (tag >> 2);
        unsigned k = (tag & 3) == 2 ? 2 : 4;
        if (p + k > src.size())
          return false;
        for (unsigned i = 0; i < k; ++i)
          off |= size_t(uint8_t(src[p++])) << (8 * i);
      }
      if (off == 0 || off > out.size() || len > n - out.size())
        return false;
      for (size_t i = 0; i < len; ++i)
        out += out[out.size() - off];
    }
  }
  return out.size() == n && p == src.size();
}
inline bool rle(const std::string &s, size_t &p, int bits, size_t count,
                std::vector<uint32_t> &out) {
  if (bits < 0 || bits > 32 || count > MAX_PAGE)
    return false;
  out.clear();
  out.reserve(count);
  while (out.size() < count) {
    uint64_t h = 0;
    unsigned b = 0;
    for (;;) {
      if (p >= s.size() || b > 56)
        return false;
      uint8_t c = s[p++];
      h |= uint64_t(c & 127) << b;
      if (!(c & 128))
        break;
      b += 7;
    }
    if (h == 0)
      return false;
    if (!(h & 1)) {
      uint64_t n = h >> 1;
      unsigned k = (bits + 7) / 8;
      if (p + k > s.size() || n > count - out.size())
        return false;
      uint32_t v = 0;
      for (unsigned i = 0; i < k; ++i)
        v |= uint32_t(uint8_t(s[p++])) << (8 * i);
      for (uint64_t i = 0; i < n; ++i)
        out.push_back(v);
    } else {
      uint64_t n = (h >> 1) * 8, bytes = (n * bits + 7) / 8;
      if (bytes > s.size() - p || n > uint64_t(count - out.size()) + 7)
        return false;
      for (uint64_t i = 0; i < n && out.size() < count; ++i) {
        uint32_t v = 0;
        for (int j = 0; j < bits; ++j) {
          uint64_t bit = i * bits + j;
          v |= uint32_t((uint8_t(s[p + bit / 8]) >> (bit % 8)) & 1) << j;
        }
        out.push_back(v);
      }
      p += bytes;
    }
  }
  return true;
}

struct ParquetColumn {
  int physical = 0, converted = -1, length = 0, timestamp_unit = 0;
  bool optional = false;
  bool unsigned_integer = false;
};
struct Chunk {
  uint64_t offset = 0, end = 0;
  int codec = 0;
};
struct Group {
  uint64_t rows = 0;
  std::vector<Chunk> chunks;
};
class Parquet {
public:
  std::vector<ParquetColumn> types;
  std::vector<Group> groups;
  uint64_t footer_start = 0;
  bool schema(Input &in, std::vector<Column> &cols) {
    std::string tail, magic, footer;
    if (in.fingerprint.size < 12 || !in.at(0, 4, magic) || magic != "PAR1" ||
        !in.at(in.fingerprint.size - 8, 8, tail) || tail.substr(4) != "PAR1")
      return in.fail("invalid Parquet magic");
    uint32_t n = le32(tail.data());
    if (n > MAX_METADATA || n > in.fingerprint.size - 12)
      return in.fail("invalid Parquet footer length");
    footer_start = in.fingerprint.size - 8 - n;
    if (!in.at(footer_start, n, footer))
      return false;
    Compact cp(footer);
    Thrift meta = cp.value(12);
    if (!cp.ok || cp.p != footer.size())
      return in.fail("invalid compact Parquet footer");
    const auto &schema = meta.get(2).list;
    if (schema.empty() || schema.size() > MAX_COLUMNS + 1 ||
        schema[0].get(5).number != int64_t(schema.size() - 1))
      return in.fail("only flat Parquet schemas are supported");
    cols.clear();
    types.clear();
    groups.clear();
    std::set<std::string> names;
    for (size_t i = 1; i < schema.size(); ++i) {
      const Thrift &s = schema[i];
      ParquetColumn t;
      t.physical = s.get(1).number;
      t.length = s.get(2).number;
      t.optional = s.get(3).number == 1;
      t.converted = s.fields.count(6) ? s.get(6).number : -1;
      if (s.get(5).number || s.get(3).number == 2 || !s.fields.count(1))
        return in.fail("nested or repeated Parquet column");
      const Thrift &logical = s.get(10);
      t.unsigned_integer =
          (t.converted >= 11 && t.converted <= 14) ||
          (logical.fields.count(10) && logical.get(10).get(2).number == 0);
      if (logical.fields.count(6))
        t.converted = 6;
      if (logical.fields.count(8)) {
        t.converted = 10;
        const Thrift &unit = logical.get(8).get(2);
        t.timestamp_unit = unit.fields.count(3)   ? 3
                           : unit.fields.count(2) ? 2
                                                  : 1;
      }
      Column c;
      c.name = s.get(4).bytes;
      c.nullable = t.optional;
      if (c.name.empty() || !utf8(c.name) || !names.insert(c.name).second)
        return in.fail("invalid or duplicate Parquet column name");
      if (t.physical == 0)
        c.type = BOOL;
      else if (t.physical == 1 || t.physical == 2)
        c.type = INT;
      else if (t.physical == 4 || t.physical == 5)
        c.type = REAL;
      else if (t.physical == 6 || t.physical == 7)
        c.type = TEXT;
      else if (t.physical == 3)
        c.type = DATETIME;
      else
        return in.fail("unsupported Parquet physical type");
      if (t.converted == 6)
        c.type = DATE;
      if (t.converted == 9 || t.converted == 10)
        c.type = DATETIME;
      if (c.type == DATETIME)
        c.scale = (t.converted == 9 || t.timestamp_unit == 1) ? 3 : 6;
      if (t.unsigned_integer && t.physical == 2)
        c.type = TEXT;
      if (logical.fields.count(11))
        c.type = NIL;
      if (t.converted == 5 || t.converted == 20 || logical.fields.count(5) ||
          logical.fields.count(7))
        return in.fail("unsupported Parquet logical type");
      cols.push_back(c);
      types.push_back(t);
    }
    uint64_t total = 0;
    for (const Thrift &rg : meta.get(4).list) {
      Group g;
      int64_t rows = rg.get(3).number;
      if (rows < 0)
        return in.fail("negative row count");
      g.rows = rows;
      total += g.rows;
      if (rg.get(1).list.size() != cols.size())
        return in.fail("Parquet row group schema mismatch");
      for (size_t i = 0; i < cols.size(); ++i) {
        const Thrift &cc = rg.get(1).list[i];
        if (!cc.get(1).bytes.empty())
          return in.fail("external Parquet column chunks are unsupported");
        const Thrift &m = cc.get(3);
        if (m.get(1).number != types[i].physical || m.get(5).number != rows ||
            m.get(3).list.size() != 1 || m.get(3).list[0].bytes != cols[i].name)
          return in.fail("invalid Parquet column metadata");
        int64_t off = m.get(9).number;
        if (m.fields.count(11))
          off = std::min(off, m.get(11).number);
        int64_t len = m.get(7).number;
        if (off < 4 || len < 0 || uint64_t(off) > footer_start ||
            uint64_t(len) > footer_start - off)
          return in.fail("invalid Parquet chunk bounds");
        Chunk c;
        c.offset = off;
        c.end = off + len;
        c.codec = m.get(4).number;
        g.chunks.push_back(c);
      }
      groups.push_back(std::move(g));
      if (groups.size() > 100000)
        return in.fail("too many Parquet row groups");
    }
    if (meta.get(3).number < 0 || total != uint64_t(meta.get(3).number))
      return in.fail("Parquet row count mismatch");
    return in.check();
  }
};

class PageCursor {
public:
  uint64_t offset = 0;
  size_t pos = 0;
  std::vector<Cell> values, dictionary;
  bool plain(Input &in, const std::string &data, size_t &p, size_t count,
             const ParquetColumn &t, std::vector<Cell> &out) {
    out.clear();
    if (count > MAX_PAGE / sizeof(Cell))
      return in.fail("Parquet page has too many values");
    if (t.physical == 0) {
      size_t bytes = (count + 7) / 8;
      if (bytes > data.size() - p)
        return in.fail("truncated boolean page");
      for (size_t i = 0; i < count; ++i)
        out.emplace_back(
            (uint8_t(data[p + i / 8]) >> (i % 8)) & 1 ? "true" : "false", BOOL);
      p += bytes;
      return true;
    }
    for (size_t i = 0; i < count; ++i) {
      Cell c;
      c.null = false;
      size_t n = t.physical == 1 || t.physical == 4   ? 4
                 : t.physical == 2 || t.physical == 5 ? 8
                 : t.physical == 3                    ? 12
                 : t.physical == 7                    ? t.length
                                                      : 0;
      if (t.physical == 6) {
        if (data.size() - p < 4)
          return in.fail("truncated byte array length");
        n = le32(data.data() + p);
        p += 4;
      }
      if (n > data.size() - p || n > MAX_RECORD)
        return in.fail("truncated or oversized Parquet value");
      const char *v = data.data() + p;
      if (t.physical == 1)
        c.text = t.unsigned_integer ? std::to_string(le32(v))
                                    : std::to_string(int32_t(le32(v)));
      else if (t.physical == 2)
        c.text = t.unsigned_integer ? std::to_string(le64(v))
                                    : std::to_string(int64_t(le64(v)));
      else if (t.physical == 3) {
        uint64_t ns = le64(v);
        int64_t jd = le32(v + 8);
        if (ns >= 86400000000000ULL || jd < 1721426 || jd > 5373484)
          return in.fail("invalid INT96 time");
        c.text = std::to_string((jd - 2440588) * 86400000000LL +
                                static_cast<int64_t>(ns / 1000));
        c.hint = DATETIME;
      } else if (t.physical == 4 || t.physical == 5) {
        double d;
        if (t.physical == 4) {
          uint32_t u = le32(v);
          float f;
          memcpy(&f, &u, 4);
          d = f;
        } else {
          uint64_t u = le64(v);
          memcpy(&d, &u, 8);
        }
        if (!std::isfinite(d))
          return in.fail("nonfinite Parquet number");
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", d);
        c.text = buf;
        c.hint = REAL;
      } else {
        c.text.assign(v, n);
        if (!utf8(c.text))
          return in.fail("invalid UTF-8 Parquet string");
      }
      if (t.converted == 6) {
        c.hint = DATE;
        int64_t days;
        if (!integer(c.text, days) || days < -719162 || days > 2932896)
          return in.fail("Parquet DATE outside supported calendar range");
      } else if (t.converted == 9 || t.converted == 10) {
        int64_t x;
        if (!integer(c.text, x))
          return in.fail("invalid timestamp");
        if (t.timestamp_unit == 3)
          x = x / 1000 - (x < 0 && x % 1000 != 0);
        else if (t.converted == 9 || t.timestamp_unit == 1) {
          if (x > INT64_MAX / 1000 || x < INT64_MIN / 1000)
            return in.fail("timestamp overflow");
          x *= 1000;
        }
        if (x < -62135596800000000LL || x > 253402300799999999LL)
          return in.fail("Parquet timestamp outside supported calendar range");
        c.text = std::to_string(x);
        c.hint = DATETIME;
      }
      out.push_back(std::move(c));
      p += n;
    }
    return true;
  }
  bool next(Input &in, const Chunk &chunk, const ParquetColumn &t,
            Cell &value) {
    while (pos == values.size()) {
      values.clear();
      pos = 0;
      if (offset >= chunk.end)
        return in.fail("missing Parquet data page");
      std::string prefix;
      size_t n = std::min<uint64_t>(64 * 1024, chunk.end - offset);
      if (!in.at(offset, n, prefix))
        return false;
      Compact cp(prefix);
      Thrift h = cp.value(12);
      if (!cp.ok)
        return in.fail("invalid Parquet page header");
      offset += cp.p;
      int64_t compressed = h.get(3).number, uncompressed = h.get(2).number;
      if (compressed < 0 || uncompressed < 0 ||
          compressed > int64_t(MAX_PAGE) || uncompressed > int64_t(MAX_PAGE) ||
          uint64_t(compressed) > chunk.end - offset)
        return in.fail("invalid Parquet page size");
      std::string body, data;
      if (!in.at(offset, compressed, body))
        return false;
      offset += compressed;
      int kind = h.get(1).number;
      if (kind == 1)
        continue;
      if (kind != 0 && kind != 2 && kind != 3)
        return in.fail("unsupported Parquet page type");
      const Thrift &ph = h.get(kind == 3 ? 8 : kind == 2 ? 7 : 5);
      int64_t count = ph.get(1).number;
      if (count < 0 || count > int64_t(MAX_PAGE / sizeof(Cell)))
        return in.fail("invalid Parquet page value count");
      size_t levels = 0;
      bool is_compressed = true;
      if (kind == 3) {
        int64_t rep = ph.get(6).number, def = ph.get(5).number;
        if (rep != 0 || def < 0 || def > compressed)
          return in.fail("invalid V2 level lengths");
        levels = def;
        is_compressed = ph.fields.count(7) ? ph.get(7).number != 0 : true;
      }
      if (levels > size_t(uncompressed))
        return in.fail("invalid V2 uncompressed length");
      if (chunk.codec == 0 || !is_compressed) {
        data = body;
        if (data.size() != size_t(uncompressed))
          return in.fail("incorrect uncompressed page length");
      } else if (chunk.codec == 1) {
        std::string decoded;
        if (!snappy(body.substr(levels), decoded, uncompressed - levels))
          return in.fail("invalid Snappy page");
        data = body.substr(0, levels) + decoded;
      } else
        return in.fail("unsupported Parquet compression codec");
      size_t p = 0;
      if (kind == 2) {
        if (ph.get(2).number != 0 && ph.get(2).number != 2)
          return in.fail("dictionary must use PLAIN encoding");
        if (!plain(in, data, p, count, t, dictionary) || p != data.size())
          return in.fail("invalid Parquet dictionary");
        continue;
      }
      std::vector<uint32_t> defs;
      if (t.optional) {
        if (kind == 0) {
          if (data.size() < 4)
            return in.fail("missing definition levels");
          levels = le32(data.data());
          p = 4;
          if (levels > data.size() - p)
            return in.fail("invalid definition level length");
        }
        std::string leveldata = data.substr(p, levels);
        size_t lp = 0;
        if (!rle(leveldata, lp, 1, count, defs) || lp != leveldata.size())
          return in.fail("invalid definition levels");
        p += levels;
      } else {
        defs.assign(count, 1);
        if (kind == 3 && levels != 0)
          return in.fail("unexpected definition levels");
      }
      size_t present = 0;
      for (uint32_t d : defs) {
        if (d > 1)
          return in.fail("invalid definition level");
        present += d;
      }
      if (kind == 3 && (ph.get(2).number < 0 ||
                        uint64_t(ph.get(2).number) != count - present))
        return in.fail("V2 null count mismatch");
      int encoding = ph.get(kind == 3 ? 4 : 2).number;
      std::vector<Cell> decoded;
      if (encoding == 0) {
        if (!plain(in, data, p, present, t, decoded))
          return false;
      } else if (encoding == 2 || encoding == 8) {
        if (p >= data.size())
          return in.fail("missing dictionary bit width");
        int bits = uint8_t(data[p++]);
        std::vector<uint32_t> ids;
        if (!rle(data, p, bits, present, ids))
          return in.fail("invalid dictionary indices");
        size_t expanded_bytes = present * sizeof(Cell);
        for (uint32_t i : ids) {
          if (i >= dictionary.size())
            return in.fail("dictionary index out of bounds");
          if (dictionary[i].text.size() > MAX_SCAN_MEMORY - expanded_bytes)
            return in.fail("expanded dictionary page exceeds memory limit");
          expanded_bytes += dictionary[i].text.size();
          if (expanded_bytes > MAX_SCAN_MEMORY / 2)
            return in.fail("expanded dictionary page exceeds memory limit");
          decoded.push_back(dictionary[i]);
        }
      } else
        return in.fail("unsupported Parquet encoding");
      if (p != data.size())
        return in.fail("trailing Parquet page bytes");
      size_t j = 0;
      for (uint32_t d : defs)
        values.push_back(d ? std::move(decoded[j++]) : Cell());
      if (!in.check())
        return false;
    }
    value = values[pos++];
    return true;
  }
};

class Reader {
public:
  Input input;
  Options options;
  std::vector<Column> columns;
  Parquet parquet;
  std::vector<std::string> files;
  size_t file_pos = 0, group = 0;
  uint64_t row_in_group = 0, rows = 0;
  std::vector<PageCursor> cursors;
  std::vector<bool> projected;
  std::string root, path, pattern;
  Fingerprint bound;
  bool glob_mode = false;
  ChangeWatch directory_watch;
  bool fail(const std::string &s) { return input.fail(s); }
  bool discover(const std::string &pattern, const std::string &secure) {
    if (pattern.find('\0') != std::string::npos)
      return fail("NUL in glob pattern");
    // Validate the non-wildcard directory before enumeration.
    size_t wildcard = pattern.find_first_of("*?[");
    size_t slash = pattern.rfind('/', wildcard);
    std::string dir = slash == std::string::npos ? "."
                      : slash == 0               ? "/"
                                                 : pattern.substr(0, slash);
    char rr[4096], dd[4096];
    if (!valid_root(secure) || !realpath(secure.c_str(), rr) ||
        !realpath(dir.c_str(), dd))
      return fail("invalid glob secure directory");
    std::string r(rr), d(dd);
    if (r != "/" && d != r && d.compare(0, r.size() + 1, r + "/") != 0)
      return fail("glob outside secure_file_priv");
    struct stat ds;
    if (stat(dd, &ds) || !S_ISDIR(ds.st_mode))
      return fail("invalid glob directory");
    bound = Fingerprint::from(ds);
    path = dd;
    if (!directory_watch.open(path, true))
      return fail("cannot watch glob directory");
    glob_t matches{};
    int rc = ::glob(pattern.c_str(), GLOB_ERR, nullptr, &matches);
    if (rc != 0 && rc != GLOB_NOMATCH) {
      globfree(&matches);
      return fail("glob failed");
    }
    if (matches.gl_pathc > 10000) {
      globfree(&matches);
      return fail("glob exceeds 10000 matches");
    }
    files.clear();
    for (size_t i = 0; i < matches.gl_pathc; ++i) {
      struct stat st;
      if (stat(matches.gl_pathv[i], &st))
        continue;
      if (!S_ISREG(st.st_mode))
        continue;
      Input check;
      if (!check.open(matches.gl_pathv[i], secure, false)) {
        globfree(&matches);
        return fail(check.error);
      }
      files.push_back(check.path);
    }
    globfree(&matches);
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    uint64_t hash = 1469598103934665603ULL;
    for (const auto &file : files) {
      for (unsigned char c : file) {
        hash ^= c;
        hash *= 1099511628211ULL;
      }
      hash ^= 0xff;
      hash *= 1099511628211ULL;
    }
    bound.signature = hash;
    return true;
  }
  bool open(const std::string &p, const std::string &secure, const Options &o) {
    options = o;
    root = secure;
    path = p;
    pattern = p;
    rows = 0;
    group = 0;
    row_in_group = 0;
    if (o.format == "glob") {
      glob_mode = true;
      Column c;
      c.name = "file";
      c.type = TEXT;
      columns = {c};
      return discover(p, secure);
    }
    if (!input.open(p, secure))
      return false;
    path = input.path;
    bound = input.fingerprint;
    if (o.format == "parquet")
      return parquet.schema(input, columns);
    if (o.format != "csv" && o.format != "jsonl")
      return fail("unknown file format");
    if (!input.skip_bom())
      return false;
    columns.clear();
    std::map<std::string, size_t> names;
    bool eof = false;
    if (o.format == "csv") {
      std::vector<Cell> row;
      if (!csv_row(input, options, row, eof))
        return false;
      if (eof)
        return fail("empty CSV has no schema");
      std::set<std::string> used;
      for (size_t i = 0; i < row.size(); ++i) {
        Column c;
        std::string base = o.header && !row[i].text.empty()
                               ? row[i].text
                               : "column" + std::to_string(i + 1);
        c.name = base;
        size_t suffix = 2;
        while (!used.insert(c.name).second)
          c.name = base + "__" + std::to_string(suffix++);
        if (c.name.size() > 256)
          return fail("column name exceeds 256 bytes");
        columns.push_back(c);
      }
      if (!o.header) {
        if (!infer_row(row))
          return false;
        ++rows;
      }
      while (!eof) {
        if (!csv_row(input, options, row, eof))
          return false;
        if (!eof) {
          if (!infer_row(row))
            return false;
          ++rows;
        }
      }
    } else {
      std::vector<std::pair<std::string, Cell>> row;
      while (!eof) {
        if (!json_row(input, row, eof))
          return false;
        if (eof)
          break;
        std::set<size_t> seen;
        for (const auto &v : row) {
          auto it = names.find(v.first);
          size_t i;
          if (it == names.end()) {
            if (columns.size() == MAX_COLUMNS || v.first.empty() ||
                v.first.size() > 256)
              return fail("invalid JSON field name or column limit");
            i = columns.size();
            names[v.first] = i;
            Column c;
            c.name = v.first;
            c.nullable = rows > 0;
            columns.push_back(c);
          } else
            i = it->second;
          seen.insert(i);
          columns[i].type = widen(
              columns[i].type,
              options.all_varchar && !v.second.null ? TEXT : infer(v.second));
          columns[i].nullable |= v.second.null;
          if (infer(v.second) == DATETIME && v.second.text.size() > 19)
            columns[i].scale =
                std::max(columns[i].scale, int(v.second.text.size() - 20));
        }
        for (size_t i = 0; i < columns.size(); ++i)
          if (!seen.count(i))
            columns[i].nullable = true;
        ++rows;
      }
      if (columns.empty())
        return fail("JSONL has no fields");
    }
    return rescan();
  }
  bool infer_row(const std::vector<Cell> &row) {
    if (row.size() != columns.size())
      return fail("CSV field count mismatch: expected " +
                  std::to_string(columns.size()) + ", got " +
                  std::to_string(row.size()));
    for (size_t i = 0; i < row.size(); ++i) {
      columns[i].type =
          widen(columns[i].type,
                options.all_varchar && !row[i].null ? TEXT : infer(row[i]));
      columns[i].nullable |= row[i].null;
      if (infer(row[i]) == DATETIME && row[i].text.size() > 19)
        columns[i].scale =
            std::max(columns[i].scale, int(row[i].text.size() - 20));
    }
    return true;
  }
  bool check() {
    if (glob_mode) {
      struct stat st;
      if (stat(path.c_str(), &st) || !directory_watch.unchanged())
        return fail("glob directory changed during query");
      Fingerprint now = Fingerprint::from(st);
      now.signature = bound.signature;
      return (bound == now) || fail("glob directory changed during query");
    }
    return input.check();
  }
  bool rescan() {
    file_pos = group = 0;
    row_in_group = 0;
    cursors.clear();
    if (glob_mode)
      return check();
    if (!input.rewind())
      return false;
    if (!input.skip_bom())
      return false;
    if (options.format == "csv" && options.header) {
      std::vector<Cell> row;
      bool eof;
      return csv_row(input, options, row, eof);
    }
    return true;
  }
  bool next(std::vector<Cell> &row, bool &eof) {
    eof = false;
    if (!check())
      return false;
    if (glob_mode) {
      if (file_pos == files.size()) {
        eof = true;
        return true;
      }
      row = {Cell(files[file_pos++])};
      return true;
    }
    if (options.format == "csv") {
      if (!csv_row(input, options, row, eof))
        return false;
      return eof || row.size() == columns.size() ||
             fail("CSV field count changed");
    }
    if (options.format == "jsonl") {
      std::vector<std::pair<std::string, Cell>> object;
      if (!json_row(input, object, eof) || eof)
        return eof && input.error.empty();
      row.assign(columns.size(), Cell());
      for (auto &v : object) {
        bool found = false;
        for (size_t i = 0; i < columns.size(); ++i)
          if (columns[i].name == v.first) {
            row[i] = std::move(v.second);
            found = true;
            break;
          }
        if (!found)
          return fail("JSON schema changed");
      }
      return true;
    }
    while (group < parquet.groups.size() &&
           row_in_group == parquet.groups[group].rows) {
      ++group;
      row_in_group = 0;
      cursors.clear();
    }
    if (group == parquet.groups.size()) {
      eof = true;
      return input.check();
    }
    const Group &g = parquet.groups[group];
    if (cursors.empty()) {
      cursors.resize(columns.size());
      for (size_t i = 0; i < cursors.size(); ++i)
        cursors[i].offset = g.chunks[i].offset;
    }
    row.assign(columns.size(), Cell());
    size_t memory = 0;
    for (size_t i = 0; i < columns.size(); ++i) {
      if (!projected.empty() && !projected[i])
        continue;
      if (!cursors[i].next(input, g.chunks[i], parquet.types[i], row[i]))
        return false;
      memory +=
          (cursors[i].values.capacity() + cursors[i].dictionary.capacity()) *
          sizeof(Cell);
      for (const auto &c : cursors[i].values)
        memory += c.text.capacity();
      for (const auto &c : cursors[i].dictionary)
        memory += c.text.capacity();
      if (memory > MAX_SCAN_MEMORY)
        return fail("Parquet scan memory limit exceeded");
    }
    ++row_in_group;
    return input.check();
  }
  bool next_batch(size_t maximum, std::vector<std::vector<Cell>> &batch,
                  bool &eof) {
    batch.clear();
    eof = false;
    if (maximum == 0 || maximum > 1024)
      return fail("batch size must be between 1 and 1024");
    size_t bytes = 0;
    while (batch.size() < maximum && !eof) {
      std::vector<Cell> row;
      if (!next(row, eof))
        return false;
      if (eof)
        break;
      for (const auto &cell : row)
        bytes += sizeof(Cell) + cell.text.size();
      if (bytes > MAX_SCAN_MEMORY)
        return fail("file scan batch exceeds memory limit");
      batch.push_back(std::move(row));
    }
    return true;
  }
};

// Stable binary descriptor carried as a constant expression in the native
// table-function plan; no input data is retained in the plan.
inline void put(std::string &out, const std::string &s) {
  uint32_t n = s.size();
  for (int i = 0; i < 4; ++i)
    out += char(n >> (i * 8));
  out += s;
}
inline bool take(const std::string &s, size_t &p, std::string &v) {
  if (p > s.size() || s.size() - p < 4)
    return false;
  uint32_t n = le32(s.data() + p);
  p += 4;
  if (n > s.size() - p)
    return false;
  v = s.substr(p, n);
  p += n;
  return true;
}
inline std::string descriptor(const Reader &r) {
  std::string out = "FSQL1";
  put(out, r.glob_mode ? r.pattern : r.path);
  put(out, r.root);
  put(out, r.options.format);
  put(out, r.options.nullstr);
  std::string o;
  o += r.options.delimiter;
  o += r.options.quote;
  o += r.options.escape;
  o += char(r.options.header);
  o += char(r.options.all_varchar);
  put(out, o);
  put(out, std::to_string(r.bound.dev));
  put(out, std::to_string(r.bound.ino));
  put(out, std::to_string(r.bound.size));
  put(out, std::to_string(r.bound.mt));
  put(out, std::to_string(r.bound.mn));
  put(out, std::to_string(r.bound.ct));
  put(out, std::to_string(r.bound.cn));
  put(out, std::to_string(static_cast<int64_t>(r.bound.signature)));
  return out;
}
inline bool restore(Reader &r, const std::string &s,
                    const std::string &current_root) {
  if (s.compare(0, 5, "FSQL1") != 0)
    return r.fail("invalid file scan descriptor");
  size_t p = 5;
  std::string path, root, format, nullstr, o;
  Fingerprint f;
  if (!take(s, p, path) || !take(s, p, root) || !take(s, p, format) ||
      !take(s, p, nullstr) || !take(s, p, o) || o.size() != 5)
    return r.fail("invalid scan options");
  int64_t a[8];
  for (int i = 0; i < 8; ++i) {
    std::string v;
    if (!take(s, p, v) || !integer(v, a[i]))
      return r.fail("invalid fingerprint");
  }
  if (p != s.size())
    return r.fail("trailing scan descriptor");
  f.dev = a[0];
  f.ino = a[1];
  f.size = a[2];
  f.mt = a[3];
  f.mn = a[4];
  f.ct = a[5];
  f.cn = a[6];
  f.signature = static_cast<uint64_t>(a[7]);
  Options opt;
  opt.format = format;
  opt.nullstr = nullstr;
  opt.delimiter = o[0];
  opt.quote = o[1];
  opt.escape = o[2];
  opt.header = o[3];
  opt.all_varchar = o[4];
  // Revalidate the current session's privilege even if the plan was cached.
  if (!r.open(path, current_root, opt))
    return false;
  return r.bound == f || r.fail("file changed since schema inference");
}

class CsvExport {
public:
  Options options;
  std::string error;
  ~CsvExport() { abort(); }
  CsvExport() = default;
  CsvExport(const CsvExport &) = delete;
  CsvExport &operator=(const CsvExport &) = delete;
  bool fail(const std::string &s) {
    error = s;
    return false;
  }
  bool open(const std::string &path, const std::string &root,
            const std::string &config) {
    abort();
    if (config.size() < 16 || config.compare(0, 9, "FSQLCOPY1") != 0)
      return fail("invalid COPY descriptor");
    options.delimiter = config[9];
    options.quote = config[10];
    options.escape = config[11];
    options.header = config[12];
    size_t pos = 13;
    if (!take(config, pos, options.nullstr) || pos != config.size())
      return fail("invalid COPY NULL option");
    if (path.empty() || path.find('\0') != std::string::npos)
      return fail("invalid COPY path");
    size_t slash = path.rfind('/');
    std::string parent = slash == std::string::npos ? "."
                         : slash == 0               ? "/"
                                                    : path.substr(0, slash);
    name_ = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name_.empty() || name_ == "." || name_ == "..")
      return fail("invalid COPY filename");
    char rr[4096], pp[4096];
    if (!valid_root(root) || !realpath(root.c_str(), rr) ||
        !realpath(parent.c_str(), pp))
      return fail(
          "COPY directory unavailable or secure_file_priv denies access");
    std::string r(rr), p(pp);
    if (r != "/" && p != r && p.compare(0, r.size() + 1, r + "/") != 0)
      return fail("COPY outside secure_file_priv");
    int dir = ::open(rr, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0)
      return fail("cannot open COPY secure directory");
    std::string rel = p == r ? "" : p.substr(r == "/" ? 1 : r.size() + 1);
    size_t start = 0;
    while (start < rel.size()) {
      size_t end = rel.find('/', start);
      std::string part =
          rel.substr(start, end == std::string::npos ? end : end - start);
      int next = ::openat(dir, part.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      ::close(dir);
      if (next < 0)
        return fail("COPY path changed");
      dir = next;
      if (end == std::string::npos)
        break;
      start = end + 1;
    }
    dir_ = dir;
    struct stat st;
    if (fstatat(dir_, name_.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 ||
        errno != ENOENT)
      return fail("COPY target already exists or is unavailable");
    static uint64_t sequence = 0;
    for (int attempt = 0; attempt < 32; ++attempt) {
      uint64_t id = __atomic_add_fetch(&sequence, 1, __ATOMIC_RELAXED);
      std::string candidate =
          ".seekdb-copy-" + std::to_string(getpid()) + "-" + std::to_string(id);
      fd_ =
          ::openat(dir_, candidate.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd_ >= 0) {
        temporary_ = std::move(candidate);
        return true;
      }
      if (errno != EEXIST)
        return fail("cannot create COPY temporary file");
    }
    return fail("cannot allocate unique COPY temporary file");
  }
  bool row(const std::vector<Cell> &cells) {
    std::string out;
    for (size_t i = 0; i < cells.size(); ++i) {
      if (i)
        out += options.delimiter;
      const Cell &c = cells[i];
      if (c.null)
        out += options.nullstr;
      else {
        bool quote =
            c.text.empty() || c.text == options.nullstr ||
            c.text.find_first_of(std::string("\r\n") + options.delimiter +
                                 options.quote) != std::string::npos;
        if (quote)
          out += options.quote;
        for (char ch : c.text) {
          if (quote &&
              (ch == options.quote ||
               (options.escape != options.quote && ch == options.escape)))
            out += options.escape;
          out += ch;
        }
        if (quote)
          out += options.quote;
      }
      if (out.size() > MAX_RECORD * 2)
        return fail("COPY record exceeds resource limit");
    }
    out += '\n';
    size_t sent = 0;
    while (sent < out.size()) {
      ssize_t n = ::write(fd_, out.data() + sent, out.size() - sent);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return fail("COPY write failed");
      sent += n;
    }
    return true;
  }
  bool commit() {
    if (fd_ < 0 || dir_ < 0)
      return fail("COPY is not open");
    if (fsync(fd_) != 0)
      return fail("COPY sync failed");
    if (::close(fd_) != 0) {
      fd_ = -1;
      return fail("COPY close failed");
    }
    fd_ = -1;
    // RENAME_NOREPLACE publishes atomically and never overwrites an existing
    // target, including one created concurrently after open().
    if (::syscall(SYS_renameat2, dir_, temporary_.c_str(), dir_, name_.c_str(),
                  1) != 0)
      return fail("COPY target exists or could not be published");
    temporary_.clear();
    ::close(dir_);
    dir_ = -1;
    return true;
  }
  void abort() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    if (dir_ >= 0) {
      if (!temporary_.empty())
        unlinkat(dir_, temporary_.c_str(), 0);
      ::close(dir_);
      dir_ = -1;
    }
    temporary_.clear();
  }

private:
  int fd_ = -1, dir_ = -1;
  std::string name_, temporary_;
};
} // namespace file_sql
} // namespace sql
} // namespace oceanbase
