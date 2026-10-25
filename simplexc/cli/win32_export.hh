/* cli/win32_export.hh —— 装配期未定义符号 → 宿主模块里的导出地址。
 *
 * 为什么不能只看 `kernel32`：`extern windows_x64 kernel32` 那个 DLL 名既不校验也不烘进产物，
 * 而里面的名字有相当一部分不是 kernel32 的导出 —— `_write` 在 CRT、`memcpy` 在 CRT，
 * `_virtual_alloc` / `_virtual_free` / `_get_system_time_precise_as_file_time` 三个模块里都没有
 * （实测见 `work/tmp/res-k32/report.md`）。绑定因此只能按**名字**去找，找的顺序定死在这一处。
 *
 * 顺序 kernel32 → ucrtbase → msvcrt：kernel32 排第一是为了让原本就解析得到的符号
 * （`ExitThread` / `Sleep` / `ExitProcess` …）落点不变；`memcpy` / `_write` / `memset` 这两个
 * CRT 模块都有导出，ucrtbase 排在 msvcrt 前面是因为 Win10 起 CRT 的真身在 ucrtbase、
 * msvcrt.dll 是留给旧二进制的兼容层。这个顺序是**选定的**，不是从参考实现读出来的
 * —— 参考产物只把绝对地址烘进 blob，看不出它查了哪个模块。
 *
 * 只 `GetModuleHandle` 不 `LoadLibrary`：装配期不该往宿主进程里塞新模块。代价是「没加载就当没有」，
 * 所以失败信息把每个模块的状态都写出来，缺的标 `(未加载)`。
 */
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace simplexc_cli {

/* mingw 对 dllimport 的写法是 `__imp_X`：装配器给它造 8 字节数据槽，要查的仍是 `X`。 */
inline std::string_view strip_import_prefix(std::string_view symbol) {
  constexpr std::string_view kImp = "__imp_";
  return symbol.rfind(kImp, 0) == 0 ? symbol.substr(kImp.size()) : symbol;
}

/* 按上面的顺序找名字为 `name` 的导出。找不到返回 nullptr，`looked` 记下查过哪些模块
   （以及其中哪些没加载）—— 失败信息要用它，不然「查不到」说不清是名字错还是模块缺。 */
inline FARPROC find_export(std::string_view name, std::string& looked) {
  const std::string fn(name);
  looked.clear();
  for (const char* mod : {"kernel32.dll", "ucrtbase.dll", "msvcrt.dll"}) {
    std::string mod_name(mod);
    mod_name.resize(mod_name.size() - 4); /* 去掉 ".dll"：报错里写短名更好认 */
    if (!looked.empty()) looked += "、";
    const HMODULE h = ::GetModuleHandleA(mod);
    if (h == nullptr) {
      looked += mod_name + "(未加载)";
      continue;
    }
    looked += mod_name;
    if (const FARPROC p = ::GetProcAddress(h, fn.c_str())) return p;
  }
  return nullptr;
}

/* 解析一个装配期未定义符号并写 `addr`；失败时把「查过哪些模块」打到 stderr。 */
inline bool resolve_win32_export(std::string_view symbol, uint64_t& addr) {
  const std::string name(strip_import_prefix(symbol));
  std::string looked;
  const FARPROC p = find_export(name, looked);
  if (p == nullptr) {
    std::fprintf(stderr, "外部符号解析失败：%s（查过 %s）\n", name.c_str(), looked.c_str());
    return false;
  }
  addr = reinterpret_cast<uint64_t>(p);
  return true;
}

}  // namespace simplexc_cli
