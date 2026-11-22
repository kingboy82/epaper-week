// 让 ics.h 能在主机上编译的最小 Arduino 桩。
// 只实现 ics.h 用到的那些接口，够跑单元测试即可。
#pragma once
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

#ifndef PROGMEM
#define PROGMEM
#endif

class String {
 public:
  String() {}
  String(const char *s) : s_(s ? s : "") {}
  String(const std::string &s) : s_(s) {}

  unsigned int length() const { return (unsigned int)s_.size(); }
  const char *c_str() const { return s_.c_str(); }
  char operator[](int i) const { return s_[i]; }

  int indexOf(char c, int from = 0) const {
    size_t p = s_.find(c, from);
    return p == std::string::npos ? -1 : (int)p;
  }
  int indexOf(const String &needle, int from = 0) const {
    size_t p = s_.find(needle.s_, from);
    return p == std::string::npos ? -1 : (int)p;
  }
  int indexOf(const char *needle, int from = 0) const {
    return indexOf(String(needle), from);
  }

  String substring(int a) const { return String(s_.substr(a)); }
  String substring(int a, int b) const {
    if (b <= a) return String();
    return String(s_.substr(a, b - a));
  }

  bool startsWith(const char *p) const { return s_.rfind(p, 0) == 0; }
  bool endsWith(const char *p) const {
    size_t n = strlen(p);
    return s_.size() >= n && s_.compare(s_.size() - n, n, p) == 0;
  }

  void trim() {
    size_t a = 0, b = s_.size();
    while (a < b && isspace((unsigned char)s_[a])) a++;
    while (b > a && isspace((unsigned char)s_[b - 1])) b--;
    s_ = s_.substr(a, b - a);
  }

  void replace(const char *from, const char *to) {
    size_t n = strlen(from);
    if (!n) return;
    size_t p = 0;
    while ((p = s_.find(from, p)) != std::string::npos) {
      s_.replace(p, n, to);
      p += strlen(to);
    }
  }

  void remove(int index) { remove(index, 1); }
  void remove(int index, int count) {
    if (index < 0 || index >= (int)s_.size()) return;
    s_.erase(index, count);
  }

  void toUpperCase() {
    for (auto &c : s_) c = (char)toupper((unsigned char)c);
  }

  int toInt() const { return atoi(s_.c_str()); }
  float toFloat() const { return (float)atof(s_.c_str()); }

  String &operator+=(char c) {
    s_ += c;
    return *this;
  }
  String &operator+=(const char *p) {
    s_ += p;
    return *this;
  }
  String &operator+=(const String &o) {
    s_ += o.s_;
    return *this;
  }
  bool operator==(const char *p) const { return s_ == p; }

 private:
  std::string s_;
};

template <typename T>
static T constrain(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

template <typename T>
static T min(T a, T b) {
  return a < b ? a : b;
}
template <typename T>
static T max(T a, T b) {
  return a > b ? a : b;
}
