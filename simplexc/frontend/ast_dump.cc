#include "ast_dump.hh"

#include <cstddef>
#include <vector>

#include "parser.hh" /* node_py_name */

namespace simplexc {
namespace {

Json str_or_null(const std::string& s) { return s.empty() ? Json::null() : Json::string(s); }

/* 列表字段 → JSON 数组（元素类型随字段而变，所以做成模板）。 */
template <class T>
Json node_list_json(const std::vector<std::unique_ptr<T>>& v) {
  Json a = Json::array();
  for (const std::unique_ptr<T>& x : v) a.push(node_to_json(x.get()));
  return a;
}

Json type_list_json(const std::vector<TypeRefPtr>& v) {
  Json a = Json::array();
  for (const TypeRefPtr& x : v) a.push(type_ref_to_json(x.get()));
  return a;
}

/* ---- 人读树用的小件（Python repr 的近似：`{v!r}`） */

std::string py_repr_str(const std::string& s) {
  static const char* hex = "0123456789abcdef";
  std::string out = "'";
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\'') {
      out += "\\'";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (u < 0x20 || u == 0x7f) {
      out += "\\x";
      out.push_back(hex[(u >> 4) & 0xF]);
      out.push_back(hex[u & 0xF]);
    } else {
      out.push_back(c);
    }
  }
  out += "'";
  return out;
}

std::string pad_of(int indent) { return std::string(static_cast<std::size_t>(indent) * 2, ' '); }

std::string join_scalars(const std::vector<std::string>& scal) {
  std::string out;
  for (std::size_t i = 0; i < scal.size(); ++i) {
    if (i) out += " ";
    out += scal[i];
  }
  return out;
}

/* 列表字段的渲染（元素类型随字段而变，所以做成模板；`text` 已带各元素自己的缩进）。 */
template <class T>
std::string render_node_list(const std::vector<std::unique_ptr<T>>& v, int indent) {
  std::string text;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i) text += "\n";
    text += render_tree(v[i].get(), indent);
  }
  return text;
}

std::string render_type_list(const std::vector<TypeRefPtr>& v, int indent) {
  std::string text;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i) text += "\n";
    text += render_type_ref(v[i].get(), indent);
  }
  return text;
}

std::string kid_list(const std::string& pad, const std::string& name, std::size_t n) {
  return pad + "  " + name + "[" + std::to_string(n) + "]";
}

std::string kid_field(const std::string& pad, const std::string& name) { return pad + "  " + name + ":"; }

}  // namespace

Json type_ref_to_json(const TypeRef* t) {
  if (t == nullptr) return Json::null();
  Json d = Json::object();
  d.set("kind", Json::string("TypeRef"));
  d.set("src_line", Json::integer(t->src_line));
  d.set("name", Json::string(t->name));
  d.set("at", Json::boolean(t->at));
  d.set("elem", t->elem ? type_ref_to_json(t->elem.get()) : Json::null());
  return d;
}

Json node_to_json(const Node* n) {
  if (n == nullptr) return Json::null();
  Json d = Json::object();
  d.set("kind", Json::string(std::string(node_py_name(n->kind))));
  d.set("src_line", Json::integer(n->src_line));
  switch (n->kind) {
    case NK::Literal: {
      const auto& x = static_cast<const LiteralExpr&>(*n);
      d.set("kind", Json::string(x.lit_kind)); /* 与类别名撞车：原型里字段覆盖类别 */
      d.set("text", Json::string(x.text));
      d.set("quote", Json::string(x.quote));
      d.set("value", x.has_value ? Json::integer(x.value) : Json::null());
      break;
    }
    case NK::Ident:
      d.set("name", Json::string(static_cast<const IdentExpr&>(*n).name));
      break;
    case NK::Cast: {
      const auto& x = static_cast<const CastExpr&>(*n);
      d.set("target", type_ref_to_json(x.target.get()));
      d.set("expr", node_to_json(x.expr.get()));
      break;
    }
    case NK::Unary: {
      const auto& x = static_cast<const UnaryExpr&>(*n);
      d.set("op", Json::string(x.op));
      d.set("expr", node_to_json(x.expr.get()));
      break;
    }
    case NK::Binary: {
      const auto& x = static_cast<const BinaryExpr&>(*n);
      d.set("op", Json::string(x.op));
      d.set("left", node_to_json(x.left.get()));
      d.set("right", node_to_json(x.right.get()));
      break;
    }
    case NK::Cond: {
      const auto& x = static_cast<const CondExpr&>(*n);
      d.set("cond", node_to_json(x.cond.get()));
      d.set("then", node_to_json(x.then_.get()));
      d.set("else_", node_to_json(x.else_.get()));
      break;
    }
    case NK::Call: {
      const auto& x = static_cast<const CallExpr&>(*n);
      d.set("callee", node_to_json(x.callee.get()));
      d.set("args", node_list_json(x.args));
      d.set("type_args", type_list_json(x.type_args));
      break;
    }
    case NK::Member: {
      const auto& x = static_cast<const MemberExpr&>(*n);
      d.set("obj", node_to_json(x.obj.get()));
      d.set("name", Json::string(x.name));
      break;
    }
    case NK::Index: {
      const auto& x = static_cast<const IndexExpr&>(*n);
      d.set("obj", node_to_json(x.obj.get()));
      d.set("index", node_to_json(x.index.get()));
      break;
    }
    case NK::ArrayLit: {
      const auto& x = static_cast<const ArrayLitExpr&>(*n);
      d.set("items", node_list_json(x.items));
      d.set("close_line", Json::integer(x.close_line));
      break;
    }
    case NK::FieldInit: {
      const auto& x = static_cast<const FieldInit&>(*n);
      d.set("name", Json::string(x.name));
      d.set("value", node_to_json(x.value.get()));
      break;
    }
    case NK::CompositeLit: {
      const auto& x = static_cast<const CompositeLitExpr&>(*n);
      d.set("typ", type_ref_to_json(x.typ.get()));
      Json fields = Json::array();
      for (const std::unique_ptr<FieldInit>& f : x.fields) {
        fields.push(node_to_json(f.get()));
      }
      d.set("fields", std::move(fields));
      d.set("close_line", Json::integer(x.close_line));
      break;
    }
    case NK::Template: {
      const auto& x = static_cast<const TemplateExpr&>(*n);
      Json texts = Json::array();
      for (const std::string& s : x.texts) texts.push(Json::string(s));
      d.set("texts", std::move(texts));
      d.set("parts", node_list_json(x.parts));
      break;
    }
    case NK::Block: {
      const auto& x = static_cast<const Block&>(*n);
      d.set("stmts", node_list_json(x.stmts));
      d.set("close_line", Json::integer(x.close_line));
      break;
    }
    case NK::Param: {
      const auto& x = static_cast<const Param&>(*n);
      d.set("name", Json::string(x.name));
      d.set("typ", x.typ ? type_ref_to_json(x.typ.get()) : Json::null());
      break;
    }
    case NK::VarDecl: {
      const auto& x = static_cast<const VarDeclStmt&>(*n);
      d.set("kw", Json::string(x.kw));
      d.set("name", Json::string(x.name));
      d.set("init", node_to_json(x.init.get()));
      break;
    }
    case NK::Assign: {
      const auto& x = static_cast<const AssignStmt&>(*n);
      d.set("target", node_to_json(x.target.get()));
      d.set("op", Json::string(x.op));
      d.set("value", node_to_json(x.value.get()));
      break;
    }
    case NK::ExprStmt:
      d.set("expr", node_to_json(static_cast<const ExprStmt&>(*n).expr.get()));
      break;
    case NK::If:
    case NK::PpIf: {
      const auto& x = static_cast<const IfStmt&>(*n);
      Json cases = Json::array();
      for (const std::unique_ptr<IfCase>& c : x.cases) {
        cases.push(node_to_json(c.get()));
      }
      d.set("cases", std::move(cases));
      d.set("else_block", node_to_json(x.else_block.get()));
      break;
    }
    case NK::IfCase: {
      const auto& x = static_cast<const IfCase&>(*n);
      d.set("cond", node_to_json(x.cond.get()));
      d.set("block", node_to_json(x.block.get()));
      break;
    }
    case NK::While: {
      const auto& x = static_cast<const WhileStmt&>(*n);
      d.set("cond", node_to_json(x.cond.get()));
      d.set("block", node_to_json(x.block.get()));
      d.set("label", str_or_null(x.label));
      break;
    }
    case NK::SwitchCase: {
      const auto& x = static_cast<const SwitchCase&>(*n);
      d.set("block", node_to_json(x.block.get()));
      d.set("value", node_to_json(x.value.get()));
      break;
    }
    case NK::Switch: {
      const auto& x = static_cast<const SwitchStmt&>(*n);
      d.set("subject", node_to_json(x.subject.get()));
      Json cases = Json::array();
      for (const std::unique_ptr<SwitchCase>& c : x.cases) {
        cases.push(node_to_json(c.get()));
      }
      d.set("cases", std::move(cases));
      break;
    }
    case NK::Return:
      d.set("value", node_to_json(static_cast<const ReturnStmt&>(*n).value.get()));
      break;
    case NK::Break:
      d.set("label", str_or_null(static_cast<const BreakStmt&>(*n).label));
      break;
    case NK::Def: {
      const auto& x = static_cast<const DefStmt&>(*n);
      d.set("name", Json::string(x.name));
      Json params = Json::array();
      for (const std::unique_ptr<Param>& p : x.params) params.push(node_to_json(p.get()));
      d.set("params", std::move(params));
      d.set("ret", x.ret ? type_ref_to_json(x.ret.get()) : Json::null());
      d.set("body", node_to_json(x.body.get()));
      break;
    }
    case NK::Function: {
      const auto& x = static_cast<const FunctionDef&>(*n);
      d.set("name", Json::string(x.name));
      d.set("form", Json::string(x.form));
      Json params = Json::array();
      for (const std::unique_ptr<Param>& p : x.params) params.push(node_to_json(p.get()));
      d.set("params", std::move(params));
      d.set("ret", x.ret ? type_ref_to_json(x.ret.get()) : Json::null());
      d.set("body", node_to_json(x.body.get()));
      d.set("lo", Json::integer(x.lo));
      d.set("hi", Json::integer(x.hi));
      break;
    }
  }
  return d;
}

// ---------------------------------------------------------------- 人读树

namespace {

/* 标量与子节点分开：原型 `render_tree` 的规则（`isinstance(v, Node)` 或「非空且首元素是 Node」
   的列表算子节点，其余算标量，空列表因此打成 `name=[]`）。 */
struct RenderParts {
  std::vector<std::string> scal;
  /* 子节点块：{名字, 是否列表, 列表长度, 渲染出来的文本} */
  struct Kid {
    std::string name;
    bool is_list = false;
    std::size_t len = 0;
    std::string text;
  };
  std::vector<Kid> kids;
};

std::string render_kids(const std::string& pad, const RenderParts& parts) {
  std::string out;
  for (const RenderParts::Kid& k : parts.kids) {
    out += "\n";
    out += k.is_list ? kid_list(pad, k.name, k.len) : kid_field(pad, k.name);
    out += "\n";
    out += k.text;
  }
  return out;
}

std::string node_head(const Node& n, const RenderParts& parts) {
  std::string head = std::string(node_py_name(n.kind)) + " @" + std::to_string(n.src_line);
  if (!parts.scal.empty()) head += "  " + join_scalars(parts.scal);
  return head;
}

}  // namespace

std::string render_type_ref(const TypeRef* t, int indent) {
  if (t == nullptr) return pad_of(indent) + "None";
  const std::string pad = pad_of(indent);
  std::string head = "TypeRef @" + std::to_string(t->src_line) + "  name=" + py_repr_str(t->name) +
                     " at=" + (t->at ? "True" : "False");
  std::string out = pad + head;
  if (t->elem) {
    out += "\n" + kid_field(pad, "elem") + "\n" + render_type_ref(t->elem.get(), indent + 2);
  } else {
    out += " elem=None";
  }
  return out;
}

std::string render_tree(const Node* n, int indent) {
  if (n == nullptr) return pad_of(indent) + "None";
  const std::string pad = pad_of(indent);
  RenderParts parts;

  auto kid_node = [&](const std::string& name, const Node* child) {
    parts.kids.push_back({name, false, 0, render_tree(child, indent + 2)});
  };
  auto kid_type = [&](const std::string& name, const TypeRef* t) {
    parts.kids.push_back({name, false, 0, render_type_ref(t, indent + 2)});
  };
  auto kid_nodes = [&](const std::string& name, const auto& v) {
    if (v.empty()) {
      parts.scal.push_back(name + "=[]");
      return;
    }
    parts.kids.push_back({name, true, v.size(), render_node_list(v, indent + 2)});
  };
  auto kid_types = [&](const std::string& name, const std::vector<TypeRefPtr>& v) {
    if (v.empty()) {
      parts.scal.push_back(name + "=[]");
      return;
    }
    parts.kids.push_back({name, true, v.size(), render_type_list(v, indent + 2)});
  };

  switch (n->kind) {
    case NK::Literal: {
      const auto& x = static_cast<const LiteralExpr&>(*n);
      parts.scal.push_back("kind=" + py_repr_str(x.lit_kind));
      parts.scal.push_back("text=" + py_repr_str(x.text));
      parts.scal.push_back("quote=" + py_repr_str(x.quote));
      parts.scal.push_back(x.has_value ? "value=" + std::to_string(x.value) : "value=None");
      break;
    }
    case NK::Ident:
      parts.scal.push_back("name=" + py_repr_str(static_cast<const IdentExpr&>(*n).name));
      break;
    case NK::Cast: {
      const auto& x = static_cast<const CastExpr&>(*n);
      kid_type("target", x.target.get());
      kid_node("expr", x.expr.get());
      break;
    }
    case NK::Unary: {
      const auto& x = static_cast<const UnaryExpr&>(*n);
      parts.scal.push_back("op=" + py_repr_str(x.op));
      kid_node("expr", x.expr.get());
      break;
    }
    case NK::Binary: {
      const auto& x = static_cast<const BinaryExpr&>(*n);
      parts.scal.push_back("op=" + py_repr_str(x.op));
      kid_node("left", x.left.get());
      kid_node("right", x.right.get());
      break;
    }
    case NK::Cond: {
      const auto& x = static_cast<const CondExpr&>(*n);
      kid_node("cond", x.cond.get());
      kid_node("then", x.then_.get());
      kid_node("else_", x.else_.get());
      break;
    }
    case NK::Call: {
      const auto& x = static_cast<const CallExpr&>(*n);
      kid_node("callee", x.callee.get());
      kid_nodes("args", x.args);
      kid_types("type_args", x.type_args);
      break;
    }
    case NK::Member: {
      const auto& x = static_cast<const MemberExpr&>(*n);
      kid_node("obj", x.obj.get());
      parts.scal.push_back("name=" + py_repr_str(x.name));
      break;
    }
    case NK::Index: {
      const auto& x = static_cast<const IndexExpr&>(*n);
      kid_node("obj", x.obj.get());
      kid_node("index", x.index.get());
      break;
    }
    case NK::ArrayLit: {
      const auto& x = static_cast<const ArrayLitExpr&>(*n);
      kid_nodes("items", x.items);
      parts.scal.push_back("close_line=" + std::to_string(x.close_line));
      break;
    }
    case NK::FieldInit: {
      const auto& x = static_cast<const FieldInit&>(*n);
      parts.scal.push_back("name=" + py_repr_str(x.name));
      kid_node("value", x.value.get());
      break;
    }
    case NK::CompositeLit: {
      const auto& x = static_cast<const CompositeLitExpr&>(*n);
      kid_type("typ", x.typ.get());
      kid_nodes("fields", x.fields);
      parts.scal.push_back("close_line=" + std::to_string(x.close_line));
      break;
    }
    case NK::Template: {
      const auto& x = static_cast<const TemplateExpr&>(*n);
      std::string texts = "texts=[";
      for (std::size_t k = 0; k < x.texts.size(); ++k) {
        if (k != 0) texts += ", ";
        texts += py_repr_str(x.texts[k]);
      }
      texts += "]";
      parts.scal.push_back(texts);
      kid_nodes("parts", x.parts);
      break;
    }
    case NK::Block: {
      const auto& x = static_cast<const Block&>(*n);
      kid_nodes("stmts", x.stmts);
      parts.scal.push_back("close_line=" + std::to_string(x.close_line));
      break;
    }
    case NK::Param: {
      const auto& x = static_cast<const Param&>(*n);
      parts.scal.push_back("name=" + py_repr_str(x.name));
      if (x.typ) {
        kid_type("typ", x.typ.get());
      } else {
        parts.scal.push_back("typ=None");
      }
      break;
    }
    case NK::VarDecl: {
      const auto& x = static_cast<const VarDeclStmt&>(*n);
      parts.scal.push_back("kw=" + py_repr_str(x.kw));
      parts.scal.push_back("name=" + py_repr_str(x.name));
      kid_node("init", x.init.get());
      break;
    }
    case NK::Assign: {
      const auto& x = static_cast<const AssignStmt&>(*n);
      kid_node("target", x.target.get());
      parts.scal.push_back("op=" + py_repr_str(x.op));
      kid_node("value", x.value.get());
      break;
    }
    case NK::ExprStmt:
      kid_node("expr", static_cast<const ExprStmt&>(*n).expr.get());
      break;
    case NK::If:
    case NK::PpIf: {
      const auto& x = static_cast<const IfStmt&>(*n);
      kid_nodes("cases", x.cases);
      if (x.else_block) {
        kid_node("else_block", x.else_block.get());
      } else {
        parts.scal.push_back("else_block=None");
      }
      break;
    }
    case NK::IfCase: {
      const auto& x = static_cast<const IfCase&>(*n);
      kid_node("cond", x.cond.get());
      kid_node("block", x.block.get());
      break;
    }
    case NK::While: {
      const auto& x = static_cast<const WhileStmt&>(*n);
      kid_node("cond", x.cond.get());
      kid_node("block", x.block.get());
      parts.scal.push_back(x.label.empty() ? "label=None" : "label=" + py_repr_str(x.label));
      break;
    }
    case NK::SwitchCase: {
      const auto& x = static_cast<const SwitchCase&>(*n);
      kid_node("block", x.block.get());
      if (x.value) {
        kid_node("value", x.value.get());
      } else {
        parts.scal.push_back("value=None");
      }
      break;
    }
    case NK::Switch: {
      const auto& x = static_cast<const SwitchStmt&>(*n);
      kid_node("subject", x.subject.get());
      kid_nodes("cases", x.cases);
      break;
    }
    case NK::Return: {
      const auto& x = static_cast<const ReturnStmt&>(*n);
      if (x.value) {
        kid_node("value", x.value.get());
      } else {
        parts.scal.push_back("value=None");
      }
      break;
    }
    case NK::Break: {
      const std::string& label = static_cast<const BreakStmt&>(*n).label;
      parts.scal.push_back(label.empty() ? "label=None" : "label=" + py_repr_str(label));
      break;
    }
    case NK::Def: {
      const auto& x = static_cast<const DefStmt&>(*n);
      parts.scal.push_back("name=" + py_repr_str(x.name));
      kid_nodes("params", x.params);
      if (x.ret) {
        kid_type("ret", x.ret.get());
      } else {
        parts.scal.push_back("ret=None");
      }
      kid_node("body", x.body.get());
      break;
    }
    case NK::Function: {
      const auto& x = static_cast<const FunctionDef&>(*n);
      parts.scal.push_back("name=" + py_repr_str(x.name));
      parts.scal.push_back("form=" + py_repr_str(x.form));
      kid_nodes("params", x.params);
      if (x.ret) {
        kid_type("ret", x.ret.get());
      } else {
        parts.scal.push_back("ret=None");
      }
      kid_node("body", x.body.get());
      parts.scal.push_back("lo=" + std::to_string(x.lo));
      parts.scal.push_back("hi=" + std::to_string(x.hi));
      break;
    }
  }

  return pad + node_head(*n, parts) + render_kids(pad, parts);
}

}  // namespace simplexc
