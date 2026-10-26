// abi_sr.cpp — P271 / P272：超分（SR）抽象层的 C ABI 适配单元
//
// 这一层只做三件事：校验参数、把 ABI 结构填好、把调用交给 `mcdr.d3d12.device` 里的
// Provider 注册表（`src\core\d3d12_sr.cpp`）。
//
// **这里不许出现任何厂商名**：P271 期间本文件还有一份 `toDlssInput` 翻译，那是"ABI 层
// 知道 DLSS 细节"的证据；P272 把所有厂商语义搬进了 provider，于是加 FSR（P277）/
// XeSS（P278）时这个文件一行都不用改。
//
// 资源指针约定见 mcdr_abi.h：一律是 JVM 创建的纹理句柄（`mcdr_object*`），不是
// `ID3D12Resource*`。解包（`Dx12Object` 的真实布局只有设备模块知道）发生在模块侧。

module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

import mcdr.d3d12.device;
// mcdr.error 是叶子模块，不 import 就取不到 mcdr::Error（import 不传递）。
import mcdr.error;

using namespace d3d12_mc;

extern "C" {

// P271：记录期望后端。不建也不销毁 —— 重建在下一帧帧首，见 P280。
int32_t MCDR_CALL mcdr_dx12SrPick(int32_t backend) {
    return srPickBackend(backend);
}

// P271：能力面。`available` 才是结论，返回值只表示"这张表填好了"。
int32_t MCDR_CALL mcdr_dx12SrQueryCaps(int32_t backend, mcdr_sr_caps* out) {
    if (out == nullptr) {
        return -1;
    }
    // 0..3 之外一律 -1。注册表内部也会挡，这里先挡一层，免得非法 id 把表填成
    // "unknown backend id" 却返回成功（调用方会当成"表有效"）。
    if (backend < MCDR_SR_BACKEND_NONE || backend > MCDR_SR_BACKEND_XESS) {
        return -1;
    }
    srFillCaps(backend, *out);
    return 1;
}

// P271：建 / 唤醒。真正建后端上下文的是 provider 的 `initialize`；DLSS 的 feature
// 创建仍推迟到第一帧 evaluate（NGX 的 CreateFeature 会录制自己的命令，必须等命令列表
// 打开）。重复调用同一个后端是幂等的，只更新尺寸。
int32_t MCDR_CALL mcdr_dx12SrCreate(mcdr_command_ctx* ctx, int32_t backend, int32_t maxRenderWidth,
                                    int32_t maxRenderHeight, int32_t displayWidth,
                                    int32_t displayHeight) {
    return srCreateProvider(ctx, backend, maxRenderWidth, maxRenderHeight, displayWidth,
                            displayHeight);
}

// P271：尺寸变化。当前后端若不需要通知（DLSS 的尺寸来自每帧载荷），provider 的实现是
// 记录型；返回 0 只表示"没有生效的后端"，不是错误。
int32_t MCDR_CALL mcdr_dx12SrResize(mcdr_command_ctx* ctx, int32_t renderWidth, int32_t renderHeight,
                                    int32_t displayWidth, int32_t displayHeight) {
    static_cast<void>(ctx);
    return srResizeProvider(renderWidth, renderHeight, displayWidth, displayHeight);
}

// P271：逐帧提交。`desc->backend` 为 0 时用 SrPick 记下的那个。
int32_t MCDR_CALL mcdr_dx12SrEvaluate(mcdr_command_ctx* ctx, const mcdr_sr_dispatch* desc) {
    if (ctx == nullptr || desc == nullptr) {
        return -1;
    }
    return srEvaluateFrame(ctx, *desc);
}

// P271：jitter 相位。返回 0 = 该后端没有自己的相位表，调用方沿用 D3D12Temporal 的
// Halton(2,3) —— **这是刻意的**：本项目亚像素 jitter 只有一个源（P75 / P218），
// 在这里再发一个源就会让画面抖成 P218 那个样子。
int32_t MCDR_CALL mcdr_dx12SrJitter(int32_t frameIndex, float* outX, float* outY) {
    static_cast<void>(frameIndex);
    if (outX == nullptr || outY == nullptr) {
        return -1;
    }
    return 0;
}

// P274：相位长度查询。0 = 不适用（None），-1 = 参数非法（与其余入口的约定一致）。
int32_t MCDR_CALL mcdr_dx12SrJitterPhaseCount(int32_t backend, int32_t renderWidth,
                                              int32_t displayWidth) {
    if (backend < MCDR_SR_BACKEND_NONE || backend > MCDR_SR_BACKEND_XESS) {
        return -1;
    }
    return srJitterPhaseCount(backend, renderWidth, displayWidth);
}

// P271：释放。只允许在没有打开命令列表时调用；重复调用安全。期望后端也一起清掉，
// 否则"销毁后再 Pick 都没做过就 Evaluate"会用一个已经不存在的后端。
void MCDR_CALL mcdr_dx12SrDestroy(void) {
    srReleaseProvider();
    (void)srPickBackend(MCDR_SR_BACKEND_NONE);
}

// P271：一行可用性文本。与 mcdr_dx12QueryDlss 同样走 writeString（NUL 结尾 + 返回长度）。
int32_t MCDR_CALL mcdr_dx12SrQuery(char* note, int32_t noteCap) {
    if (noteCap < 0 || (noteCap > 0 && note == nullptr)) {
        return -1;
    }
    const std::string text = srStatusText();
    if (note == nullptr || noteCap == 0) {
        // 只问长度：与 mcdrLastError 的约定一致，不写缓冲。
        return static_cast<int32_t>(text.size());
    }
    return mcdr::abi::writeString(text, note, noteCap);
}

}  // extern "C"