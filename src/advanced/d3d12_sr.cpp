// d3d12_sr.cpp — P272：超分（SR）Provider 接口层的实现单元（模块 mcdr.d3d12.device）
//
// 三块内容：
//   1. 能力表（`srFillCaps`）—— 唯一的「这家后端能干什么」事实来源；
//   2. `DlssSrProvider` —— DLSS 的实现，把 P271 期间放在 C ABI 层的翻译搬到这里；
//   3. 注册表 —— 期望后端（picked）/ 生效后端（active）/ 一行状态文本。
//
// 为什么翻译搬进来：P271 的 `abi_sr.cpp` 里有一份 `toDlssInput`，那是"ABI 层知道厂商
// 细节"的证据。厂商细节属于 provider，ABI 层只做校验与填表；搬完之后 `abi_sr.cpp` 不再
// 出现任何 dlss 字样，P277 / P278 加后端时也不需要碰 ABI。
//
// 行为等价性：`DlssSrProvider::evaluate` 就是 `applyDlssToTexture(ctx, toDlssInput(desc))`，
// 参数翻译逐字沿用 P271 的实现（包括 `exposureScale = 1.0f`、`depthInverted = 1` 这两个
// 写死的值及其理由）。所以 P272 不改变任何一帧的画面。

module;

#include "_d3d12_module_prelude.h"

// mcdr_sr_caps / mcdr_sr_dispatch / mcdr_dlss_input / MCDR_SR_* 都在这里。必须放在全局
// 模块片段：模块 purview 内文本包含头文件会触发 C5244，而本项目 /WX 直接失败。
// 与 d3d12_device.cpp 同理，**.cppm 全局模块片段里的声明不会传给实现单元**，各实现
// 单元要自己再 include 一次（include guard 保证幂等）。
#include "mcdr_abi.h"

// P284：`swprintf_s` / `wcslen`（PIX 区间名）。放在全局模块片段，理由同上。
#include <cstdio>
#include <cwchar>
#include <string_view>

module mcdr.d3d12.device;

namespace d3d12_mc {
// P280：销毁 provider 前等 GPU 追平（定义在文件末尾，`ensureProvider` 在匿名 namespace 里先声明）。
void releaseProviderWithWait(CommandContext* ctx);

namespace {

// MCDR_SR_BACKEND_* → SrBackendId 的**唯一**转换点。加后端只改这里。
SrBackendId toId(int32_t backendId) {
    switch (backendId) {
        case MCDR_SR_BACKEND_DLSS:
            return SrBackendId::Dlss;
        case MCDR_SR_BACKEND_FSR:
            return SrBackendId::Fsr;
        case MCDR_SR_BACKEND_XESS:
            return SrBackendId::XeSS;
        default:
            return SrBackendId::None;
    }
}

bool validBackend(int32_t backendId) {
    return backendId == MCDR_SR_BACKEND_NONE || backendId == MCDR_SR_BACKEND_DLSS ||
           backendId == MCDR_SR_BACKEND_FSR || backendId == MCDR_SR_BACKEND_XESS;
}

const char* nameOf(int32_t backendId) {
    switch (backendId) {
        case MCDR_SR_BACKEND_DLSS:
            return "dlss";
        case MCDR_SR_BACKEND_FSR:
            return "fsr";
        case MCDR_SR_BACKEND_XESS:
            return "xess";
        default:
            return "none";
    }
}

// 固定长度字段的 NUL 结尾拷贝。`note` 是 char[28]，不能指望调用方先清零，也不能假设
// 源串比它短。
void copyNote(char* dst, size_t cap, const char* src) {
    if (cap == 0) {
        return;
    }
    size_t n = 0;
    while (n + 1 < cap && src[n] != '\0') {
        ++n;
    }
    for (size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
    dst[n] = '\0';
}

// P271 的能力表内容原样搬来（数值都是查过 SDK 文档 / 头文件的，改动要同时改
// `sr_plan.md` 的 §8 质量细则表）。
void fillDlssCaps(mcdr_sr_caps& out) {
    char buf[192] = {};
    const bool up = queryDlss(buf, static_cast<int>(sizeof(buf))) == 1;
    out.available = up ? 1 : 0;
    out.coupledDenoise = 0;    // DLSS-SR 不含降噪；DLSS-D 是另一个后端（v1 不做）
    out.needsExposure = 0;
    out.takesMasks = MCDR_SR_MASK_REACTIVE;
    out.externalHeap = 1;      // Streamline 可通过 slAllocateResources 用外部堆
    out.jitterPhaseCount = 8;  // Streamline 的 8 相位（与 D3D12Temporal 的 Halton(2,3) 对齐）
    out.minRenderWidth = 320;
    out.minRenderHeight = 180;
    out.maxRenderWidth = 7680;
    out.maxRenderHeight = 4320;
    out.nativeAA = 1;          // DLAA：1.0x
    copyNote(out.note, sizeof(out.note), up ? buf : "dlss unavailable");
}

// 把与厂商无关的载荷翻译成 DLSS 的老表（P271 期间这段在 abi_sr.cpp，逐字搬来）。
//
// P275：**0 = 未填**。`motionVectorScale` / `preExposure` 传 0 会让 DLSS 把运动矢量当零位移
// 处理（静止画面正常、一运动就拖影），而 0 又是 JVM 侧零初始化结构体的自然结果，所以这里把
// 0 解释成"没填"并给安全默认 1.0，而不是把 0 原样传下去。
mcdr_dlss_input toDlssInput(const mcdr_sr_dispatch& d) {
    mcdr_dlss_input in{};
    in.renderWidth = d.renderWidth;
    in.renderHeight = d.renderHeight;
    in.targetWidth = d.displayWidth;
    in.targetHeight = d.displayHeight;
    in.jitterOffsetX = d.jitterOffsetX;
    in.jitterOffsetY = d.jitterOffsetY;
    // D3D12Velocity 写出的 MV 已经是像素位移（(uv - prevUv) * screenSize），倍率就该是 1.0。
    in.mvScaleX = d.motionVectorScaleX != 0.0f ? d.motionVectorScaleX : 1.0f;
    in.mvScaleY = d.motionVectorScaleY != 0.0f ? d.motionVectorScaleY : 1.0f;
    in.preExposure = d.preExposure != 0.0f ? d.preExposure : 1.0f;
    // mcdr_sr_dispatch v1 不带曝光纹理：显式声明 NONE，等 P289 再接。
    in.exposureScale = 1.0f;
    in.exposureMode = MCDR_DLSS_EXPOSURE_NONE;
    // 世界层颜色不含 UI alpha（GUI 有自己的目标），所以不做 alpha 升采样。
    in.alphaUpscaling = 0;
    in.nearZ = d.cameraNear;
    in.farZ = d.cameraFar;
    // Minecraft 26.3 是 reverse-Z（Projection.java 清深度 0.0 + GREATER_THAN_OR_EQUAL），
    // 老表的这条是硬要求，SR 载荷不单独暴露它。
    in.depthInverted = 1;
    in.reset = d.reset;
    in.depthFormatOrdinal = d.depthFormatOrdinal;
    in.pInColor = static_cast<mcdr_object*>(d.pColorInput);
    in.pInDepth = static_cast<mcdr_object*>(d.pDepth);
    in.pInMotionVectors = static_cast<mcdr_object*>(d.pMotionVectors);
    in.pInOutput = static_cast<mcdr_object*>(d.pColorOutput);
    return in;
}

// ---- 注册表状态（全部由渲染线程写；热切换由 ensureProvider 懒执行，见 P280）----
int32_t gSrPicked = MCDR_SR_BACKEND_NONE;
int32_t gSrActive = MCDR_SR_BACKEND_NONE;
std::string gSrNote;  // 最近一次失败 / 降级原因，给 SrQuery 用
std::unique_ptr<IUpscaleProvider> gSrProvider;

// P284：诊断计数。只给 `mcdr_dx12SrQuery` 的文本用 —— 升采样"到底跑没跑"最容易看错的
// 信号是"日志里有一条 provider ready"，而它只在初始化成功时打一次；这里记的是逐帧结果。
uint64_t gSrEvalOk = 0;
uint64_t gSrEvalFail = 0;
int32_t gSrLastRenderWidth = 0;
int32_t gSrLastRenderHeight = 0;
int32_t gSrLastDisplayWidth = 0;
int32_t gSrLastDisplayHeight = 0;

// ---------------------------------------------------------------------------
// DLSS 的实现。
//
// 它刻意**不持有**任何 Streamline / NGX 状态：那一切仍在 `src\advanced\d3d12_dlss.cpp`
// 的单例里，`applyDlssToTexture` 已经是那条路径的唯一入口（含 Dx12Object 解包、render
// pass 收尾、barrier 走跟踪状态）。P272 只把"哪家后端、能力是什么、怎么翻译参数"从
// ABI 层挪到这里，不碰实现单元一句代码 —— 动它等于在没有实机验证的条件下重写一条
// 已经能出画的分辨率路径。
// ---------------------------------------------------------------------------
class DlssSrProvider final : public IUpscaleProvider {
public:
    SrBackendId backend() const override { return SrBackendId::Dlss; }

    void fillCaps(mcdr_sr_caps& out) const override { fillDlssCaps(out); }

    bool initialize(CommandContext* ctx, const mcdr_sr_dispatch& seed) override {
        // ctx 目前用不到（DLSS 的建特征发生在 evaluate 内部，因为 NGX 的
        // CreateFeature 会录制自己的命令，必须等命令列表打开）。保留参数是为了让
        // FSR / XeSS 的实现能在这里建自己的上下文。
        static_cast<void>(ctx);
        mMaxRenderWidth = seed.renderWidth;
        mMaxRenderHeight = seed.renderHeight;
        mDisplayWidth = seed.displayWidth;
        mDisplayHeight = seed.displayHeight;
        char buf[192] = {};
        if (queryDlss(buf, static_cast<int>(sizeof(buf))) != 1) {
            gSrNote = buf[0] != '\0' ? buf : "dlss unavailable";
            return false;
        }
        return true;
    }

    void onResize(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                  int32_t displayHeight) override {
        // DLSS 的尺寸来自每帧载荷（renderWidth / targetWidth），不需要单独通知；
        // 这里只记下来，等 P279 的内存中介真的持有临时资源时才会有重建动作。
        mMaxRenderWidth = renderWidth;
        mMaxRenderHeight = renderHeight;
        mDisplayWidth = displayWidth;
        mDisplayHeight = displayHeight;
    }

    bool evaluate(CommandContext* ctx, const mcdr_sr_dispatch& desc) override {
        if (ctx == nullptr || desc.pColorInput == nullptr || desc.pColorOutput == nullptr) {
            return false;
        }
        return applyDlssToTexture(ctx, toDlssInput(desc));
    }

    void shutdown() override {
        // **空操作是刻意的**：`mcdr_dlss_shutdown` 会把实现单元的全局状态整块清掉，
        // 连带把还没有切到 SR 路径的老 DLSS 调用一起关掉（P108 的 hand-off 会因此
        // 丢输出纹理）。真正的释放在设备销毁 / 换设备时由既有路径做。
    }

private:
    int32_t mMaxRenderWidth = 0;
    int32_t mMaxRenderHeight = 0;
    int32_t mDisplayWidth = 0;
    int32_t mDisplayHeight = 0;
};

// 工厂。每个后端一个实现单元；不加 case 的后端 `srCreateProvider` 会返回 0 并把原因写进
// gSrNote，ABI 层据此如实报 unavailable。
std::unique_ptr<IUpscaleProvider> makeProvider(SrBackendId id) {
    switch (id) {
        case SrBackendId::Dlss:
            return std::make_unique<DlssSrProvider>();
        case SrBackendId::Fsr:
            // P277：FFX 后端。运行库不在时工厂返回 nullptr，原因由 makeFsrProvider 打日志。
            return makeFsrProvider();
        case SrBackendId::XeSS:
            // P278：Intel 后端，同上。
            return makeXessProvider();
        default:
            return nullptr;
    }
}

// 懒建 / 换后端。调用点：srCreateProvider（JVM 显式建）与 srEvaluateFrame（JVM 只
// Pick + Evaluate 时也要能工作，P271 的验证探针就是这种用法）。
bool ensureProvider(CommandContext* ctx, int32_t backendId, const mcdr_sr_dispatch& seed) {
    if (gSrProvider != nullptr && gSrActive == backendId) {
        return true;
    }
    if (gSrProvider != nullptr) {
        const int32_t previous = gSrActive;
        releaseProviderWithWait(ctx);
        dbgLogInfo("SR provider switch %s -> %s", nameOf(previous), nameOf(backendId));
    }
    std::unique_ptr<IUpscaleProvider> provider = makeProvider(toId(backendId));
    if (provider == nullptr) {
        gSrNote = "backend not implemented";
        return false;
    }
    if (!provider->initialize(ctx, seed)) {
        return false;  // initialize 已经写过 gSrNote
    }
    gSrActive = backendId;
    gSrProvider = std::move(provider);
    dbgLogInfo("SR provider ready: %s (render %dx%d display %dx%d)", nameOf(backendId),
               seed.renderWidth, seed.renderHeight, seed.displayWidth, seed.displayHeight);
    return true;
}

// 从 create 的参数造一个"种子载荷"：只填尺寸，资源指针留空。DLSS 的 initialize 只读
// 尺寸；将来 FSR / XeSS 若要在建上下文时就知道 HDR / 深度格式，也是往这里加字段。
mcdr_sr_dispatch seedFromSizes(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                               int32_t displayHeight) {
    mcdr_sr_dispatch seed{};
    seed.renderWidth = renderWidth;
    seed.renderHeight = renderHeight;
    seed.displayWidth = displayWidth;
    seed.displayHeight = displayHeight;
    return seed;
}

}  // namespace

int32_t srPickBackend(int32_t backendId) {
    if (!validBackend(backendId)) {
        gSrNote = "unknown backend id";
        return 0;
    }
    gSrPicked = backendId;
    return 1;
}

// P300（补 P281 的已知缺口）：让帧生成那边能问「超分现在**跑着**的是哪家」。
// 为什么需要：SR=FSR/XeSS 时 DLSS-G 拿不到 Streamline 的 `ScalingOutputColor` tag
// （那条 tag 只有走 SL 的超分才有），于是它会静默不插帧。界面挡住了，配置文件挡不住 ——
// `[sr] backend` 与 `[dlss] framegen` 是两个独立键，所以要在 native 侧给一句可用性判断。
//
// 为什么问 `gSrActive` 而不是 `gSrPicked`（2026-10-27 实测纠正）：JVM 侧**从不调**
// `mcdr_dx12SrPick`，它走的是 `mcdr_dx12SrCreate(encoder, backend, …)`（`SuperResolution.kt:951`），
// 于是 `gSrPicked` 永远是 NONE —— 拿它做门控等于没有门控。`gSrActive` 才是 create 成功后真正
// 在用的那家；SR 还没建出来时它是 NONE，此时放行帧生成是对的（启动早期），后面真换成 FSR/XeSS
// 时由 P300 的自动回滚（60 帧滞回）收场。
// 用户选的那一家（`mcdr_dx12SrPick` 写进来的）。JVM 现在会在 `load` / `currentBackend` /
// `requestBackend` 三处推一次（`pushBackendToNative`），所以这个值在启动早期就可信。
int32_t srSelectedBackend(void) { return gSrPicked; }

int32_t srActiveBackend(void) { return gSrActive; }

void srFillCaps(int32_t backendId, mcdr_sr_caps& out) {
    out = mcdr_sr_caps{};
    out.backend = backendId;
    switch (static_cast<int>(backendId)) {
        case MCDR_SR_BACKEND_NONE:
            // "不超分"也是一个合法选择：UI 关掉 SR 走这条，不是错误。
            out.available = 1;
            out.nativeAA = 1;
            copyNote(out.note, sizeof(out.note), "off");
            return;
        case MCDR_SR_BACKEND_DLSS:
            fillDlssCaps(out);
            return;
        case MCDR_SR_BACKEND_FSR:
            // P277：caps 由 FFX 运行库的加载结果决定（不建上下文，见 fillFsrCaps 的注释）。
            fillFsrCaps(out);
            return;
        case MCDR_SR_BACKEND_XESS:
            // P278：caps 由 libxess.dll 的加载结果决定（不建上下文）。
            fillXessCaps(out);
            return;
        default:
            copyNote(out.note, sizeof(out.note), "unknown backend id");
            return;
    }
}

int32_t srCreateProvider(CommandContext* ctx, int32_t backendId, int32_t maxRenderWidth,
                         int32_t maxRenderHeight, int32_t displayWidth, int32_t displayHeight) {
    if (ctx == nullptr || !validBackend(backendId) || maxRenderWidth <= 0 || maxRenderHeight <= 0 ||
        displayWidth <= 0 || displayHeight <= 0) {
        return -1;
    }
    if (backendId == MCDR_SR_BACKEND_NONE) {
        srReleaseProvider();
        gSrActive = MCDR_SR_BACKEND_NONE;
        return 1;
    }
    const mcdr_sr_dispatch seed =
        seedFromSizes(maxRenderWidth, maxRenderHeight, displayWidth, displayHeight);
    // 幂等：同一个后端重复 create 只更新尺寸，不重建（P280 的热切换会靠这个语义）。
    if (gSrProvider != nullptr && gSrActive == backendId) {
        gSrProvider->onResize(maxRenderWidth, maxRenderHeight, displayWidth, displayHeight);
        return 1;
    }
    return ensureProvider(ctx, backendId, seed) ? 1 : 0;
}

int32_t srResizeProvider(int32_t renderWidth, int32_t renderHeight, int32_t displayWidth,
                         int32_t displayHeight) {
    if (renderWidth <= 0 || renderHeight <= 0 || displayWidth <= 0 || displayHeight <= 0) {
        return -1;
    }
    if (gSrProvider == nullptr) {
        return 0;  // 没有生效的后端：不是错误，只是没东西可通知
    }
    gSrProvider->onResize(renderWidth, renderHeight, displayWidth, displayHeight);
    return 1;
}

int32_t srEvaluateFrame(CommandContext* ctx, const mcdr_sr_dispatch& desc) {
    if (ctx == nullptr) {
        return -1;
    }
    int32_t backendId = static_cast<int32_t>(desc.backend);
    if (backendId == MCDR_SR_BACKEND_NONE) {
        backendId = gSrPicked;
    }
    if (!validBackend(backendId)) {
        return -1;
    }
    if (backendId == MCDR_SR_BACKEND_NONE) {
        return 0;  // 明确关掉：安静跳过
    }
    if (!ensureProvider(ctx, backendId, desc)) {
        return 0;  // 不可用 / 未实现：由 SrQuery / SrQueryCaps 报原因
    }
    // P284：给 PIX / RenderDoc 一个可读的区间名 —— 抓帧时能直接看到"这一帧的升采样是
    // 哪家后端、从多大升到多大"，不用去翻日志对时间戳。`BeginEvent` / `EndEvent` 只在
    // 抓帧工具附加时才有开销，正常游玩是空操作。
    wchar_t eventName[96] = {};
    swprintf_s(eventName, L"SR:%hs %dx%d->%dx%d", nameOf(backendId), desc.renderWidth,
               desc.renderHeight, desc.displayWidth, desc.displayHeight);
    ctx->commandList->BeginEvent(0, eventName, static_cast<UINT>(wcslen(eventName)));
    const bool ok = gSrProvider->evaluate(ctx, desc);
    ctx->commandList->EndEvent();
    gSrLastRenderWidth = desc.renderWidth;
    gSrLastRenderHeight = desc.renderHeight;
    gSrLastDisplayWidth = desc.displayWidth;
    gSrLastDisplayHeight = desc.displayHeight;
    if (ok) {
        ++gSrEvalOk;
        // "last" 的语义是"最近一次失败的原因"：这一帧成功了就该消失，否则诊断行会长期挂着
        // 一条早已恢复的旧错误，比没有这条信息更误导。
        gSrNote.clear();
    } else {
        ++gSrEvalFail;
        gSrNote.assign(nameOf(backendId)).append(" evaluate failed");
    }
    // P273：无论成败都把主描述符堆绑回来。三个厂商 SDK 的 Evaluate 内部都可能
    // SetDescriptorHeaps（Streamline 的 evaluate 就是典型），而堆绑定是命令列表级状态、
    // 不随根签名 / PSO 复位 —— 不绑回来，后续任何走瞬时 SRV 的绘制都会撞 P24 那条
    // "descriptor heap containing handle is different from currently set descriptor heap"。
    restoreDrawHeap(ctx);
    if (!ok) {
        return 0;
    }
    gSrActive = backendId;
    // P305b：包装型帧后端的输入 tag 必须落在**这一帧的命令列表**上（XeSS 的
    // `TagFrameResource(cmdList, presentId, …)`、FSR 的 `InterpolationCommandListDX12` 都是
    // 这个时机），而不是 Present 前后。DLSS-G 不覆写这个钩子 —— 它在 `d3d12_dlss.cpp` 里
    // 自己用同一次 `slSetTagForFrame` 交 tag，所以这里对它是一次空调用。
    fgTagFrameInputs(fgMakeFrameInputs(ctx, desc));
    return 1;
}

// P274：相位长度由后端回答，而不是让上层写死一个常数。
// - DLSS(SL)：8（`sl::Constants` 的 jitter 序列；这也是 caps.jitterPhaseCount 的值）。
// - FSR：官方公式 `JitterPhaseCount = 8 * (displayWidth / renderWidth)^2`，取整后夹到
//   [1, 256]。P277 接了真后端之后应当改用 `ffxQueryDescUpscaleGetJitterPhaseCount`
//   （`ffx_upscale.h:133-140`）问运行库，公式只是当前的无后端近似。
// - XeSS：8（SDK 自己按 `xessGetJitterScale` 缩放我们给的像素偏移，序列长度不变）。
// - None：0 = 不适用（没有超分在累积，jitter 与它无关）。
int32_t srJitterPhaseCount(int32_t backendId, int32_t renderWidth, int32_t displayWidth) {
    if (!validBackend(backendId)) {
        return 0;
    }
    switch (toId(backendId)) {
        case SrBackendId::None:
            return 0;
        case SrBackendId::Dlss:
            return 8;
        case SrBackendId::Fsr: {
            if (renderWidth <= 0 || displayWidth <= 0) {
                return 8;
            }
            const double ratio = static_cast<double>(displayWidth) / static_cast<double>(renderWidth);
            long long count = static_cast<long long>(8.0 * ratio * ratio + 0.5);
            if (count < 1) count = 1;
            if (count > 256) count = 256;
            return static_cast<int32_t>(count);
        }
        case SrBackendId::XeSS:
            return 8;
    }
    return 0;
}

void srReleaseProvider(void) {
    if (gSrProvider != nullptr) {
        // 这条路径没有命令上下文可等（ABI 的 `mcdr_dx12SrDestroy` 就是这么调的），所以改用
        // 队列级栅栏：等"此刻已经提交的全部工作"退役。它覆盖的正是"上一帧提交的命令列表还
        // 引用着 FSR / XeSS 上下文持有的临时资源"这种情况。
        const UINT64 queueValue = currentQueueFenceValue();
        if (queueValue > 0) {
            const auto waited = waitForQueueFenceValue(queueValue, 5'000'000'000ULL);
            if (!waited.has_value()) {
                dbgLogWarn("the queue did not retire in time, releasing the SR provider anyway");
            }
        }
        gSrProvider->shutdown();
        gSrProvider.reset();
    }
    gSrActive = MCDR_SR_BACKEND_NONE;
}

// P280：要销毁 provider 时先等上一帧的提交落地。
//
// 为什么必须等：FFX 的 `ffxDestroyContext` / XeSS 的 `xessDestroyContext` 会把上下文持有的
// GPU 资源整块释放，而那些资源可能还留在上一帧已提交、GPU 尚未执行完的命令列表里
// （`ctx->fenceValue` 是"最近一次 Signal 的值"，恰好覆盖这种情况）。不等就直接销毁，
// 典型表现是设备移除，或者画面在切换的那一帧整块变黑。
//
// 只等 `ctx->fenceValue`、不等 `+1`：切换发生在帧内（命令列表可能已经打开），本帧的提交
// 还没发生，等 `+1` 等于等一个永远不会被 Signal 的值（走满超时才返回）。
void releaseProviderWithWait(CommandContext* ctx) {
    if (gSrProvider == nullptr) {
        return;
    }
    if (ctx != nullptr && ctx->fenceValue > 0) {
        const auto waited = waitForFenceValue(ctx, ctx->fenceValue, 5'000'000'000ULL);
        if (!waited.has_value()) {
            dbgLogWarn("the previous frame did not retire in time, releasing the SR provider anyway");
        }
    }
    gSrProvider->shutdown();
    gSrProvider.reset();
    gSrActive = MCDR_SR_BACKEND_NONE;
}

// P284：一行诊断文本。这一行的读者是"怀疑升采样没在跑"的人，所以四件事都要能读到：
//   ① 期望后端 picked 与真正跑过的 active 是否一致（不一致 = 切换失败或被拒）；
//   ② 逐帧计数 ok/fail —— 只有 provider ready 一条日志完全区分不了"建起来了但每帧都失败"；
//   ③ 生效后端自己的能力（masks / jitter / max），用来解释"为什么这段画面有残影"；
//   ④ 最近一次的降级原因（gSrNote）与上一帧的尺寸。
// 注意 `srFillCaps` 会去问运行库（FSR / XeSS 是一次 LoadLibrary 尝试，DLSS 走 queryDlss），
// 所以这条只该在诊断 / 界面刷新时调，不要挂到每帧路径上。
std::string srStatusText(void) {
    std::string text = "sr active=";
    text += nameOf(gSrActive);
    text += " picked=";
    text += nameOf(gSrPicked);
    text += " eval=";
    text += std::to_string(gSrEvalOk);
    text += "/";
    text += std::to_string(gSrEvalFail);
    if (gSrLastDisplayWidth > 0) {
        text += " size=";
        text += std::to_string(gSrLastRenderWidth);
        text += "x";
        text += std::to_string(gSrLastRenderHeight);
        text += "->";
        text += std::to_string(gSrLastDisplayWidth);
        text += "x";
        text += std::to_string(gSrLastDisplayHeight);
    }
    mcdr_sr_caps caps{};
    srFillCaps(gSrActive, caps);
    text += " caps[";
    text += caps.available != 0 ? "ok" : "unavailable";
    text += " masks=";
    text += std::to_string(caps.takesMasks);
    text += " jitter=";
    text += std::to_string(caps.jitterPhaseCount);
    text += " max=";
    text += std::to_string(caps.maxRenderWidth);
    text += "x";
    text += std::to_string(caps.maxRenderHeight);
    text += "]";
    // NONE 档的 note 固定是 "off"，那是正常状态而不是理由，别往诊断行里塞（会让人以为出错）。
    if (caps.available == 0 || std::string_view(caps.note) != "off") {
        if (caps.note[0] != 0) {
            text += " why=";
            text += caps.note;
        }
    }
    if (!gSrNote.empty()) {
        text += " last=";
        text += gSrNote;
    }
    return text;
}

}  // namespace d3d12_mc
