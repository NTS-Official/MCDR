// abi_framegen.cpp — P295：帧生成（FG）抽象层的 C ABI 适配单元（模块 mcdr.abi.common）
//
// 与 `abi_sr.cpp`（P271）同形：只校验参数、填表、把调用交给 `mcdr.d3d12.device` 里的
// 注册表（`src\core\d3d12_framegen.cpp`）。**这里不许出现任何厂商名** —— 加 FSR-FG /
// XeSS-FG 时本文件应当一行都不用改（P272 对 SR 已经验证过这条分层纪律）。
//
// 资源指针约定与 SR 段一致：一律是 JVM 创建的纹理句柄（`mcdr_object*`），不是
// `ID3D12Resource*`；解包发生在模块侧。

module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

import mcdr.d3d12.device;
// mcdr.error 是叶子模块，不 import 就取不到 mcdr::Error（import 不传递）。
import mcdr.error;

using namespace d3d12_mc;

extern "C" {

// P293：记录期望后端。不建也不销毁 —— 重建在下一帧的 fgEnsureProvider 里，与 P280 同构。
int32_t MCDR_CALL mcdr_dx12FgPick(int32_t backend) {
    return fgPickBackend(backend);
}

// P293：能力面。`available` 才是结论，返回值只表示"这张表填好了"。
int32_t MCDR_CALL mcdr_dx12FgQueryCaps(int32_t backend, mcdr_fg_caps* out) {
    if (out == nullptr) {
        return -1;
    }
    // 0..3 之外一律 -1：注册表内部也会挡，但这里先挡一层，免得非法 id 把表填成
    // "unknown backend" 却返回成功（调用方会当成"表有效"）。
    if (backend < MCDR_FG_BACKEND_NONE || backend > MCDR_FG_BACKEND_XESSFG) {
        return -1;
    }
    fgFillCaps(backend, *out);
    return 1;
}

// P293：设期望值。**只记录**，真正的 slDLSSGSetOptions 在渲染线程的下一次 present 前
// 下发（SL 官方文档 §6 明确说选项在下一次 Present 生效，跨线程调用会落到不确定的那次）。
int32_t MCDR_CALL mcdr_dx12FgSetMode(const mcdr_fg_dispatch* desc) {
    if (desc == nullptr) {
        return -1;
    }
    return fgSetMode(*desc);
}

// P293c：上一次 Present 真正提交的帧数（1 = 没插帧）。老入口
// `mcdr_dx12GetDlssgPresented` 读到的是同一个数（两者都落到 provider->presented()）。
int32_t MCDR_CALL mcdr_dx12FgPresented(void) {
    return fgPresented();
}

// P293：一行状态文本。与 mcdr_dx12SrQuery 一样走 writeString（NUL 结尾 + 返回长度）。
int32_t MCDR_CALL mcdr_dx12FgStatus(char* note, int32_t noteCap) {
    if (noteCap < 0 || (noteCap > 0 && note == nullptr)) {
        return -1;
    }
    const std::string text = fgStatusText();
    if (note == nullptr || noteCap == 0) {
        // 只问长度：与 mcdrLastError 的约定一致，不写缓冲。
        return static_cast<int32_t>(text.size());
    }
    return mcdr::abi::writeString(text, note, noteCap);
}

// P300：把「期望 / 链 / 是否真的有一个活着的后端 / 重建次数」压进一个 int32 返回。
// 存在理由：P300 的回滚会在 native 侧把期望的 mode 清成 0，而 JVM 的 `options.txt` 仍然写着
// 「开」—— 界面就会说假话。让 JVM 读一个整数比让它解析 `mcdr_dx12FgStatus` 的文本可靠。
// 位布局写在 `include\mcdr_abi.h` 的声明处（那是唯一契约），这里不做二次解释。
int32_t MCDR_CALL mcdr_dx12FgState(void) {
    return fgStatePacked();
}

}  // extern "C"
