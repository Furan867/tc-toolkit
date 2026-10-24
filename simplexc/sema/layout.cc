/* sema/layout.cc —— struct 布局与 enum 底层宽度（规则与依据见头文件）。
 *
 * 逐句对应 `lower.cc` 的 `Program::scan_decls`（struct 位宽）与 `Program::make_types`
 * （字段偏移）：两遍在那里，这里合成一遍 —— 因为偏移与位宽是同一个累加量的两种读法。
 */
#include "layout.hh"

#include "errors.hh"

namespace simplexc {
namespace sema {
namespace {

[[noreturn]] void layout_error(int line, const std::string& detail) {
    Span span;
    span.line = line;
    throw CompileError(Responsibility::Unimplemented, Stage::Sema, span, detail);
}

}  // namespace

int enum_storage_bits(std::size_t member_count) {
    /* U-L5：两档。第三档（4 字节）在参考实现里不可达 —— 65536 个成员时它自己断言崩溃
     * （`enum_names.len < 1 shl 16`），所以这里报错而不是给一个没验证过的宽度。 */
    if (member_count <= 256) return 8;
    if (member_count <= 65535) return 16;
    layout_error(0, "枚举有 " + std::to_string(member_count) +
                        " 个成员（U-L5：参考实现在 >65535 时自己崩溃，4/8 字节档不可达）");
}

TyRef layout_enum(const std::string& name, std::size_t member_count) {
    /* `make_enum` 是建枚举类型的唯一入口（`types.cc`），但它不给 `bits` —— 底层宽度只有这里
     * 算得出来（要成员数）。`TyRef` 是 `shared_ptr<const Ty>`，所以改幅度得拷一份。 */
    const TyRef base = make_enum(name);
    auto sized = std::make_shared<Ty>(*base);
    sized->bits = enum_storage_bits(member_count);
    return sized;
}

StructLayout layout_struct(const std::string& name,
                           const std::vector<std::pair<std::string, TyRef>>& fields) {
    StructLayout out;
    if (fields.empty()) {
        /* 空 struct（rv32im 的 `Input`/`Output`）：1 字节占位，零宽度聚合没验证过。 */
        out.ty = make_struct(name, 8, {});
        return out;
    }
    int offset = 0;
    for (const auto& f : fields) {
        if (f.second->kind == TyKind::Struct) {
            layout_error(0, "struct " + name + " 的字段 " + f.first + " 是 struct " + f.second->name +
                                "（U-L4 嵌套布局未验）");
        }
        int bytes = 0;
        /* 字段类型没有字节宽度（`None`）时 `storage_bytes` 抛 `CompileError`：这里就地换成
         * 「哪个 struct 的哪个字段」，把位置从类型层抬回声明层（`layout_*` 本身没有 span）。 */
        try {
            bytes = storage_bytes(*f.second, Span{});
        } catch (const CompileError&) {
            layout_error(0, "struct " + name + " 的字段 " + f.first + " 是 " +
                                ty_kind_name(f.second->kind) + "（没有字节宽度）");
        }
        out.fields.push_back(HirField{f.first, f.second, offset});
        offset += bytes;
    }
    out.ty = make_struct(name, offset * 8, fields);
    return out;
}

}  // namespace sema
}  // namespace simplexc
