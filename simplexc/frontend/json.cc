#include "json.hh"

#include <charconv>
#include <cstddef>

namespace simplexc {
namespace {

/* CPython `json.encoder.py_encode_basestring` 的转义集合（ensure_ascii=False）：
   只处理 " \ 与 0x00–0x1f，其余字节原样（UTF-8 直出）。 */
void escape_to(std::string& out, const std::string& s) {
  static const char* hex = "0123456789abcdef";
  out.push_back('"');
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    switch (u) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (u < 0x20) {
          out += "\\u00";
          out.push_back(hex[(u >> 4) & 0xF]);
          out.push_back(hex[u & 0xF]);
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

/* CPython `repr(float)`：最短往返表示，且整数值带 `.0`（`1.0` 不是 `1`）。 */
std::string float_repr(double v) {
  char buf[64];
  const auto res = std::to_chars(buf, buf + sizeof(buf), v);
  std::string s(buf, res.ptr);
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find("inf") == std::string::npos && s.find("nan") == std::string::npos &&
      s.find('E') == std::string::npos) {
    s += ".0";
  }
  return s;
}

}  // namespace

Json Json::null() { return Json(); }

Json Json::boolean(bool b) {
  Json j;
  j.t_ = T::Bool;
  j.b_ = b;
  return j;
}

Json Json::integer(long long v) {
  Json j;
  j.t_ = T::Int;
  j.i_ = v;
  return j;
}

Json Json::real(double v) {
  Json j;
  j.t_ = T::Real;
  j.d_ = v;
  return j;
}

Json Json::string(std::string s) {
  Json j;
  j.t_ = T::Str;
  j.s_ = std::move(s);
  return j;
}

Json Json::array() {
  Json j;
  j.t_ = T::Arr;
  return j;
}

Json Json::object() {
  Json j;
  j.t_ = T::Obj;
  return j;
}

void Json::push(Json v) { arr_.push_back(std::move(v)); }

void Json::set(std::string key, Json v) {
  for (auto& kv : obj_) {
    if (kv.first == key) {
      kv.second = std::move(v); /* 键已存在 → 就地改值，位置不变（Python dict 的语义） */
      return;
    }
  }
  obj_.emplace_back(std::move(key), std::move(v));
}

std::size_t Json::size() const {
  if (t_ == T::Arr) return arr_.size();
  if (t_ == T::Obj) return obj_.size();
  return 0;
}

std::string Json::dump(int indent) const {
  std::string out;
  dump_to(out, indent, 0);
  return out;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
  switch (t_) {
    case T::Null: out += "null"; return;
    case T::Bool: out += b_ ? "true" : "false"; return;
    case T::Int: out += std::to_string(i_); return;
    case T::Real: out += float_repr(d_); return;
    case T::Str: escape_to(out, s_); return;
    case T::Arr:
    case T::Obj: break;
  }
  const std::size_t n = (t_ == T::Arr) ? arr_.size() : obj_.size();
  if (n == 0) {
    out += (t_ == T::Arr) ? "[]" : "{}";
    return;
  }
  const std::string pad(indent > 0 ? static_cast<std::size_t>(indent) *
                                         static_cast<std::size_t>(depth + 1)
                                   : 0,
                        ' ');
  const std::string pad_close(indent > 0 ? static_cast<std::size_t>(indent) *
                                               static_cast<std::size_t>(depth)
                                         : 0,
                              ' ');
  out += (t_ == T::Arr) ? "[" : "{";
  for (std::size_t k = 0; k < n; ++k) {
    if (k != 0) out += ",";
    if (indent > 0) out += "\n";
    out += pad;
    if (t_ == T::Obj) {
      escape_to(out, obj_[k].first);
      out += ": ";
      obj_[k].second.dump_to(out, indent, depth + 1);
    } else {
      arr_[k].dump_to(out, indent, depth + 1);
    }
  }
  if (indent > 0) {
    out += "\n";
    out += pad_close;
  }
  out += (t_ == T::Arr) ? "]" : "}";
}

}  // namespace simplexc
