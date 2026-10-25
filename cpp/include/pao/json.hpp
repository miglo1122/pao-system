// Мінімальний JSON (RFC 8259) без зовнішніх залежностей: розбір і серіалізація.
// Достатній для потоків подій JSON Lines, якими обмінюються ядро і Python-шар.
#pragma once
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace pao::json {

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class Value {
 public:
  using Storage = std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<Array>,
                               std::shared_ptr<Object>>;
  Value() : v_(nullptr) {}
  Value(std::nullptr_t) : v_(nullptr) {}
  Value(bool b) : v_(b) {}
  Value(int i) : v_(static_cast<double>(i)) {}
  Value(long i) : v_(static_cast<double>(i)) {}
  Value(long long i) : v_(static_cast<double>(i)) {}
  Value(unsigned long i) : v_(static_cast<double>(i)) {}
  Value(double d) : v_(d) {}
  Value(const char* s) : v_(std::string(s)) {}
  Value(std::string s) : v_(std::move(s)) {}
  Value(Array a) : v_(std::make_shared<Array>(std::move(a))) {}
  Value(Object o) : v_(std::make_shared<Object>(std::move(o))) {}

  bool is_null() const { return std::holds_alternative<std::nullptr_t>(v_); }
  bool is_bool() const { return std::holds_alternative<bool>(v_); }
  bool is_number() const { return std::holds_alternative<double>(v_); }
  bool is_string() const { return std::holds_alternative<std::string>(v_); }
  bool is_array() const { return std::holds_alternative<std::shared_ptr<Array>>(v_); }
  bool is_object() const { return std::holds_alternative<std::shared_ptr<Object>>(v_); }

  bool as_bool() const { return std::get<bool>(v_); }
  double as_number() const { return std::get<double>(v_); }
  const std::string& as_string() const { return std::get<std::string>(v_); }
  const Array& as_array() const { return *std::get<std::shared_ptr<Array>>(v_); }
  const Object& as_object() const { return *std::get<std::shared_ptr<Object>>(v_); }
  Object& as_object() { return *std::get<std::shared_ptr<Object>>(v_); }

  bool has(const std::string& k) const {
    return is_object() && as_object().count(k) && !as_object().at(k).is_null();
  }
  const Value& operator[](const std::string& k) const {
    static const Value null_value;
    if (!is_object()) return null_value;
    auto it = as_object().find(k);
    return it == as_object().end() ? null_value : it->second;
  }
  std::string str(const std::string& k, const std::string& def = "") const {
    const Value& x = (*this)[k];
    return x.is_string() ? x.as_string() : def;
  }
  double num(const std::string& k, double def = 0.0) const {
    const Value& x = (*this)[k];
    return x.is_number() ? x.as_number() : def;
  }
  bool flag(const std::string& k, bool def = false) const {
    const Value& x = (*this)[k];
    return x.is_bool() ? x.as_bool() : def;
  }

  std::string dump() const {
    std::ostringstream os;
    write(os);
    return os.str();
  }

 private:
  Storage v_;

  static void write_string(std::ostream& os, const std::string& s) {
    os << '"';
    for (unsigned char c : s) {
      switch (c) {
        case '"':
          os << "\\\"";
          break;
        case '\\':
          os << "\\\\";
          break;
        case '\n':
          os << "\\n";
          break;
        case '\r':
          os << "\\r";
          break;
        case '\t':
          os << "\\t";
          break;
        default:
          if (c < 0x20) {
            const char* hex = "0123456789abcdef";
            os << "\\u00" << hex[c >> 4] << hex[c & 15];
          } else {
            os << c;  // UTF-8 передається як є
          }
      }
    }
    os << '"';
  }

  void write(std::ostream& os) const {
    if (is_null())
      os << "null";
    else if (is_bool())
      os << (as_bool() ? "true" : "false");
    else if (is_number()) {
      double d = as_number();
      if (!std::isfinite(d))
        os << "null";
      else if (d == std::floor(d) && std::fabs(d) < 9.007e15)
        os << static_cast<long long>(d);
      else {
        std::ostringstream t;
        t.precision(6);
        t << std::fixed << d;
        std::string s = t.str();
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
        os << s;
      }
    } else if (is_string())
      write_string(os, as_string());
    else if (is_array()) {
      os << '[';
      bool first = true;
      for (const auto& x : as_array()) {
        if (!first) os << ',';
        first = false;
        x.write(os);
      }
      os << ']';
    } else {
      os << '{';
      bool first = true;
      for (const auto& [k, x] : as_object()) {
        if (!first) os << ',';
        first = false;
        write_string(os, k);
        os << ':';
        x.write(os);
      }
      os << '}';
    }
  }
};

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}
  Value parse() {
    Value v = value();
    ws();
    if (i_ != s_.size()) fail("зайві символи після JSON");
    return v;
  }

 private:
  const std::string& s_;
  size_t i_ = 0;

  [[noreturn]] void fail(const std::string& m) const {
    throw std::runtime_error("JSON: " + m + " (позиція " + std::to_string(i_) + ")");
  }
  void ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
  }
  char peek() {
    ws();
    if (i_ >= s_.size()) fail("несподіваний кінець");
    return s_[i_];
  }
  void expect(char c) {
    if (peek() != c) fail(std::string("очікувався '") + c + "'");
    ++i_;
  }

  Value value() {
    char c = peek();
    if (c == '{') return object();
    if (c == '[') return array();
    if (c == '"') return Value(string());
    if (c == 't' && s_.compare(i_, 4, "true") == 0) {
      i_ += 4;
      return Value(true);
    }
    if (c == 'f' && s_.compare(i_, 5, "false") == 0) {
      i_ += 5;
      return Value(false);
    }
    if (c == 'n' && s_.compare(i_, 4, "null") == 0) {
      i_ += 4;
      return Value(nullptr);
    }
    return number();
  }
  Value object() {
    expect('{');
    Object o;
    if (peek() == '}') {
      ++i_;
      return Value(std::move(o));
    }
    while (true) {
      if (peek() != '"') fail("очікувався ключ");
      std::string k = string();
      expect(':');
      o[k] = value();
      char c = peek();
      ++i_;
      if (c == '}') break;
      if (c != ',') fail("очікувалась ',' або '}'");
    }
    return Value(std::move(o));
  }
  Value array() {
    expect('[');
    Array a;
    if (peek() == ']') {
      ++i_;
      return Value(std::move(a));
    }
    while (true) {
      a.push_back(value());
      char c = peek();
      ++i_;
      if (c == ']') break;
      if (c != ',') fail("очікувалась ',' або ']'");
    }
    return Value(std::move(a));
  }
  static void utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80)
      out += static_cast<char>(cp);
    else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  uint32_t hex4() {
    if (i_ + 4 > s_.size()) fail("обрізаний \\u");
    uint32_t v = static_cast<uint32_t>(std::stoul(s_.substr(i_, 4), nullptr, 16));
    i_ += 4;
    return v;
  }
  std::string string() {
    expect('"');
    std::string out;
    while (true) {
      if (i_ >= s_.size()) fail("незакритий рядок");
      char c = s_[i_++];
      if (c == '"') break;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i_ >= s_.size()) fail("обрізаний escape");
      char e = s_[i_++];
      switch (e) {
        case '"':
          out += '"';
          break;
        case '\\':
          out += '\\';
          break;
        case '/':
          out += '/';
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u': {
          uint32_t cp = hex4();
          if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
            i_ += 2;
            uint32_t lo = hex4();
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(out, cp);
          break;
        }
        default:
          fail("невідомий escape");
      }
    }
    return out;
  }
  Value number() {
    size_t start = i_;
    if (s_[i_] == '-') ++i_;
    while (i_ < s_.size() && (isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' || s_[i_] == 'e' ||
                              s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-'))
      ++i_;
    if (start == i_) fail("некоректне значення");
    try {
      return Value(std::stod(s_.substr(start, i_ - start)));
    } catch (...) {
      fail("некоректне число");
    }
  }
};

inline Value parse(const std::string& s) {
  return Parser(s).parse();
}

}  // namespace pao::json
