// 极小的有序 JSON 值 + 序列化器（**只给 ast_dump 用**，不进产品路径）。
//
// 为什么要自己写而不是找个库：`ast_dump` 的 JSON 要与原型 `json.dump(obj, f,
// ensure_ascii=False, indent=1)` 逐字节相同（判据 J1 用 `cmp` 就够），那么
// 缩进、分隔符、转义集合、浮点写法都得照 CPython 的来 —— 现成库的默认值都对不上。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace simplexc {

class Json {
 public:
  enum class T { Null, Bool, Int, Real, Str, Arr, Obj };

  Json() = default;
  static Json null();
  static Json boolean(bool b);
  static Json integer(long long v);
  static Json real(double v);
  static Json string(std::string s);
  static Json array();
  static Json object();

  T type() const { return t_; }
  void push(Json v);                        /* Arr */
  void set(std::string key, Json v);        /* Obj：键已存在则就地改值（与 Python dict 一致） */
  std::size_t size() const;

  /* = json.dumps(self, ensure_ascii=False, indent=<indent>) */
  std::string dump(int indent) const;

 private:
  void dump_to(std::string& out, int indent, int depth) const;

  T t_ = T::Null;
  bool b_ = false;
  long long i_ = 0;
  double d_ = 0.0;
  std::string s_;
  std::vector<Json> arr_;
  std::vector<std::pair<std::string, Json>> obj_;
};

}  // namespace simplexc
