// mcdr.error —— 内部 C++ 错误约定的最小词表（Error / Result<T>）
//
// 背景（2026-10-14 现代化改造）：
//   改造前，模块内部的失败通过 `bool f(..., std::string& err)` 这个 out 参数约定上报，
//   全仓 134 处签名。它的问题不是慢，而是**失败可以被静默忽略** —— 调用方漏看返回值时
//   会拿着默认构造/未初始化的结果继续跑，直到在很远的地方炸掉。
//
//   改用 std::expected 之后，失败在类型里：`Result<T>` 要么是 T，要么是 Error，
//   取 `value()` 而未检查 `has_value()` 会抛 std::bad_expected_access，而不是给出垃圾值。
//
// 设计约束：
//   * 本模块是**叶子**：只依赖标准库，不 import 任何 mcdr.* 模块，因此谁都能 import 它
//     而不形成 import 环（见 README 的模块硬约束）。标准库经全局模块片段里的
//     `#include "_d3d12_module_prelude.h"` 取得 —— 不能用 `import std;`，那条语句放在
//     全局模块片段里会被判为 C5202（该片段仅可包含预处理器指令）。
//   * Error 里只出现 std::string，不出现别的模块拥有的类型 —— 否则导出到签名里会踩
//     MSVC v145 的模块私有名修饰问题（`Type@$$_A<module>` 与消费方 `@1` 对不上）。
//   * 不强制错误码：改造前的约定只有文本，没有可判别的代码。硬塞一个 int code 会逼出
//     134 处假的赋值，所以只保留文本 + 出错的函数名。将来真需要分类时再加字段。
//
// C ABI 边界（src\c_abi\*.cpp）不走 std::expected：那里必须继续是
// `extern "C" int32_t` + 线程局部的 mcdrLastError 文本，JVM/FFM 侧只认这套。

module;

#include "_d3d12_module_prelude.h"

export module mcdr.error;

export namespace mcdr {

// 一次失败的全部信息。message 是给人看的（日志、mcdrLastError）；
// where 是出错的函数名（字面量，生命周期不归 Error 管）。
struct Error {
    std::string message;
    const char* where = "";

    Error() = default;
    Error(const char* w, std::string m) : message(std::move(m)), where(w) {}
};

// 统一返回类型：成功是 T，失败是 Error。void 用 std::expected<void, Error>。
template <typename T>
using Result = std::expected<T, Error>;

// 构造失败值；与返回类型无关，故不必每次写 std::unexpected(...)。
// 刻意**不叫** fail()：避免和从 std::string& err 时代沿用下来的局部变量名相撞。
inline std::unexpected<Error> err(const char* where, std::string message) {
    return std::unexpected(Error{where, std::move(message)});
}

// 成功返回；T 有显式默认构造也行，这个只是省一处的样板。
inline std::expected<void, Error> ok() { return {}; }

// 把 Error 摊平成一行文本，供 C ABI 的 setLastError / failWith 使用。
// 带 where 时输出 "where: message"，与改造前 err 字符串的排版习惯一致。
inline std::string describe(const Error& e) {
    if (e.where == nullptr || e.where[0] == '\0') return e.message;
    return std::string(e.where) + ": " + e.message;
}

}  // namespace mcdr
