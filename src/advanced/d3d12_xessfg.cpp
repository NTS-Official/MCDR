// d3d12_xessfg.cpp — P304：XeSS 帧生成（XeSS-FG）provider 的实现单元（模块 mcdr.d3d12.device）
//
// 为什么单独一个单元，而不是塞进 `d3d12_xess.cpp`（SR）：
//   * 主体不同。SR 那条每帧往命令列表里录一次 dispatch；XeSS-FG 的主体是**包装交换链**
//     （`xefg_swapchain_d3d12`）：建链之后 `InitFromSwapChain`、之后所有 Present 必须走
//     `xefgSwapChainD3D12GetSwapChainPtr` 换回来的那条、重链之前必须先 `Destroy`。
//     两者的生命周期与失败面完全不重叠，放一个文件里只会让"哪段在 Present 路径上"难看清。
//   * 依赖面不同。SR 只要 `libxess.dll`；FG 要 `libxess_fg.dll`（XeLL 的 `libxell.dll` 是
//     可选的延迟优化）。分开之后"发行包里少了哪个 DLL"在 caps 的 note 里能直接看出来。
//
// 与 DLSS-G 的**结构差异**（见 `mcdr_device.cppm` 的 `wrapSwapChain`）：
//   DLSS-G：SL 在 Present 内部插帧，链是我们自己的（SL 代理工厂建的）；
//   XeSS-FG：SDK 把应用链包起来，Present 走它给的链。SL 的代理工厂与"包装别人的链"是两种
//            互斥的集成方式，所以本后端的 `chainCompatible` 要求链是**应用原生**的
//            （`MCDR_FG_CHAIN_APP_NATIVE`）；注册表在 pick 本后端时会把工厂升级让位掉。
//
// 当前进度（P304）：运行库探测 + 能力表 + provider 骨架已接；**包装与逐帧 tag 是 P305**，
// 所以 `available` 恒 0、note 写明原因 —— 不许把"结构里能填"说成"已支持"（P298 的教训）。

module;

#include "_d3d12_module_prelude.h"

// mcdr_fg_caps / mcdr_fg_dispatch / MCDR_FG_* 与 MCDR_FG_LATENCY_* 都在这里。必须放在全局
// 模块片段：模块 purview 内文本包含头文件会触发 C5244，本项目 /WX 直接失败。
#include "mcdr_abi.h"

// 只有本单元看得到 XeSS-FG 的类型 —— 与 `d3d12_xess.cpp` 收住 `xess.h` 是同一条理由：
// 厂商头不进模块接口，别的单元想用也用不了，依赖方向只有一个。
#include <xess_fg/xefg_swapchain.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell.h>
#include <xell/xell_d3d12.h>

module mcdr.d3d12.device;

namespace d3d12_mc {

namespace {

constexpr const char* kXessFgDll = "libxess_fg.dll";
constexpr const char* kXellDll = "libxell.dll";

// P304：包装路径还没接上。用常量而不是注释来表达，是为了让 `fillXessFgCaps` 的 note
// 与 `wrapSwapChain` 的日志说同一句话 —— 将来接通时只改这里。
// P305u：XeSS-FG 硬性要求一条 XeLL 低延迟通路（`xess_fg_developer_guide_english.md:220`
// "XeSS-FG requires a working XeLL integration on all GPUs"）。少了它，`InitFromSwapChain`
// 会直接返回 `XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED (-15)` —— 这是实测的。
// 上下文按进程单例持有：XeLL 的语义是"一条低延迟通路"，不该每个 swapchain 建一个。

// P305ah：XeLL 低延迟通路的入口（每帧要用，不能每帧 GetProcAddress）。上下文按进程单例持有：
// XeLL 的语义是"一条低延迟通路"，不该每个 swapchain 建一个。
struct XellApi {
    HMODULE module = nullptr;
    decltype(&xellSetSleepMode) setSleepMode = nullptr;
    decltype(&xellSleep) sleep = nullptr;
    decltype(&xellAddMarkerData) addMarkerData = nullptr;

    bool ready() const { return sleep != nullptr && addMarkerData != nullptr; }
};

std::mutex gXellMutex;
void* gXellContext = nullptr;
bool gXellAttempted = false;

XellApi& xellApi() {
    static XellApi api;
    return api;
}

void* xellContext(ID3D12Device* device)
{
    std::lock_guard<std::mutex> lk(gXellMutex);
    if (gXellContext != nullptr || gXellAttempted)
    {
        return gXellContext;
    }
    gXellAttempted = true;
    HMODULE module = ::LoadLibraryA("libxell.dll");
    if (module == nullptr)
    {
        return nullptr;
    }
    auto create = reinterpret_cast<decltype(&xellD3D12CreateContext)>(::GetProcAddress(module, "xellD3D12CreateContext"));
    if (create == nullptr)
    {
        return nullptr;
    }
    // P305ah：把低延迟通路的其余入口一并取好（每帧要用，不能每帧 GetProcAddress）。
    XellApi& xell = xellApi();
    xell.module = module;
    xell.setSleepMode = reinterpret_cast<decltype(xell.setSleepMode)>(::GetProcAddress(module, "xellSetSleepMode"));
    xell.sleep = reinterpret_cast<decltype(xell.sleep)>(::GetProcAddress(module, "xellSleep"));
    xell.addMarkerData =
        reinterpret_cast<decltype(xell.addMarkerData)>(::GetProcAddress(module, "xellAddMarkerData"));
    xell_context_handle_t context = nullptr;
    const xell_result_t created = create(device, &context);
    if (created == 0)
    {
        gXellContext = context;
        // 只在上下文刚建好时设一次：`bLowLatencyMode = 1` 是"启用 latency reduction"的开关，
        // 官方 sample 也是在 SetEnabled 之前先 `xellSetSleepMode(params{bLowLatencyMode=1})`
        // （`basic_sample.cpp:290-296`）。`minimumIntervalUs = 0` 表示不做限帧。
        if (xell.ready())
        {
            xell_sleep_params_t params{};
            params.bLowLatencyMode = 1;
            const xell_result_t sleepMode = xell.setSleepMode(context, &params);
            if (sleepMode != 0)
            {
                dbgLogWarn("xell: SetSleepMode(lowLatency=1) failed 0x%X",
                           static_cast<unsigned>(sleepMode));
            }
        }
    }
    return gXellContext;
}
constexpr bool kWrapWired = true;
// P305s：告诉 SDK 我们的真实输入语义 —— 深度是 reverse-Z（GREATER_THAN_OR_EQUAL、清 0 = 远），
// MV 含 jitter（与 DLSS 侧同时置 MVJittered + motionVectorJitterCancellation 一致）。
// 头文件要求 `BuildPipelines` 的 initFlags "must be identical" 到初始化用的值，所以只留一个常量。
constexpr uint32_t kInitFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH | XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV;

// P305ae：逐帧 tag 已接通（presentId / 五资源 / 帧常量 / UI 合成 / enabled+插帧数），
// 加上链所有权（计数=1）修好，这一位可以翻 true —— 包装失败的原因会由 caps 的 note 说清。
constexpr bool kTaggingWired = true;

// P305ao 实验开关：`true` 时逐帧也 tag HUDLESS_COLOR（世界层颜色）。
// 实机持续 `fgResult = -14 (INCORRECT_INPUT_RESOURCES)`、`presented = 1`（不插帧），
// 已排除：HUDless 格式/尺寸（F28:854x480 与 swapchain 完全一致）、深度格式（换成 D32_FLOAT 也 -14）、
// `resourceSize` 缺省、XeLL 未启用。只 tag MV + DEPTH 的最小契约同样 -14 ——
// 真正的根因是**帧常量从未到达 native**（JVM 的 `D3D12FgSetConstants` 绑定写成了 `as Int`，
// void 句柄抛 WrongMethodTypeException 被吞，表现是帧常量一直是 0，见 P305ap）。
// 常量修好后恢复 HUDless。保留这个开关，下次怀疑某条资源时不用改代码结构。
constexpr bool kTagHudless = true;

// P305：context 是否已经建好（包装成功）。caps 是自由函数，看不到 provider 实例，用一个
// 文件级原子做镜像 —— 与 `gChainOwnerSnapshot` 同一手法（都是"锁内被调用，不能回头加锁"）。
// P305n：两块由本模块持有的临时堆（交给 XeSS-FG 用，所有权不出这个翻译单元）。
ComPtr<ID3D12Heap> gTempBufferHeap;
ComPtr<ID3D12Heap> gTempTextureHeap;
UINT64 gTempBufCapacity = 0;   // P305q：已建堆的容量，尺寸变大才重建
UINT64 gTempTexCapacity = 0;
int32_t gWrapCount = 0;         // P305q：第几次 wrap（多尺寸重建会多次进来）
// P305ah：XeLL 的 frame_id 必须**单调递增**，换链也不能回到 0 —— 实测重新 wrap 后
// `xellSleep(ctx, 1)` 直接返回 -4（INVALID_ARGUMENT），因为 XeLL 上下文是进程单例、记着
// 之前用过的号。所以计数器放文件级、只增不减；presentId 与它共用一个号（guide :240）。
std::atomic<uint32_t>& markerFrameCounter() {
    static std::atomic<uint32_t> counter{0};
    return counter;
}
std::atomic<bool>& contextReady() {
    static std::atomic<bool> ready{false};
    return ready;
}

// P305ae：init 时 SDK 报的最大插帧数的镜像。caps 是自由函数，看不到 provider 实例，
// 与 `contextReady()` 同一手法（锁内被调用，不能回头加锁）。
std::atomic<int32_t>& maxInterpMirror() {
    static std::atomic<int32_t> value{0};
    return value;
}

// 往固定长度 note 缓冲写一句话（NUL 结尾、超长截断）。与 `d3d12_framegen.cpp` 里那份
// 同形但各自独立：两份都在匿名命名空间，跨 TU 共享就得提到头文件里，而那会把
// "note 怎么填"变成又一个跨模块接口 —— 不值得。
void writeCapsNote(char* dst, size_t cap, const char* text) {
    if (dst == nullptr || cap == 0) {
        return;
    }
    size_t i = 0;
    if (text != nullptr) {
        for (; i + 1 < cap && text[i] != '\0'; ++i) {
            dst[i] = text[i];
        }
    }
    dst[i] = '\0';
}

// 本模块所在目录。P108 的教训：`GetModuleFileNameA(nullptr)` 拿到的是宿主 exe（java.exe），
// 必须用 `GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS` 问本模块自己。发行包里 `libxess_fg.dll`
// 由 `MCDR_ABI.extractVendorResources()` 解到 `dxrmc\`，与本 DLL 同目录。
std::wstring selfDirectory() {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&selfDirectory), &self) == FALSE ||
        self == nullptr || ::GetModuleFileNameW(self, path, MAX_PATH) == 0) {
        return {};
    }
    std::wstring dir(path);
    const std::wstring::size_type slash = dir.find_last_of(L"\\/");
    dir.resize(slash == std::wstring::npos ? 0 : slash);
    return dir;
}

// 从 DLL 里取到的入口。前六条是**必需**（少了任何一条整条路都跑不起来），其余有就用。
// 用 `decltype(&...)` 而不是手写签名：头文件是唯一的类型来源，改 SDK 版本时编译器会替我们
// 检查类型有没有变（`decltype` 是未求值语境，不会引入对导入库的链接依赖）。
struct XessFgApi {
    HMODULE module = nullptr;
    decltype(&xefgSwapChainGetVersion) getVersion = nullptr;
    decltype(&xefgSwapChainD3D12CreateContext) createContext = nullptr;
    decltype(&xefgSwapChainD3D12BuildPipelines) buildPipelines = nullptr;
    decltype(&xefgSwapChainD3D12GetProperties) getProperties = nullptr;
    decltype(&xefgSwapChainD3D12InitFromSwapChain) initFromSwapChain = nullptr;
    decltype(&xefgSwapChainD3D12GetSwapChainPtr) getSwapChainPtr = nullptr;
    decltype(&xefgSwapChainSetLatencyReduction) setLatencyReduction = nullptr;
    decltype(&xefgSwapChainSetLoggingCallback) setLoggingCallback = nullptr;
    decltype(&xefgSwapChainD3D12TagFrameResource) tagFrameResource = nullptr;
    decltype(&xefgSwapChainD3D12SetDescriptorHeap) setDescriptorHeap = nullptr;
    decltype(&xefgSwapChainTagFrameConstants) tagFrameConstants = nullptr;
    decltype(&xefgSwapChainSetEnabled) setEnabled = nullptr;
    decltype(&xefgSwapChainSetPresentId) setPresentId = nullptr;
    decltype(&xefgSwapChainSetNumInterpolatedFrames) setNumInterpolatedFrames = nullptr;
    decltype(&xefgSwapChainSetUiCompositionState) setUiCompositionState = nullptr;
    decltype(&xefgSwapChainGetLastPresentStatus) getLastPresentStatus = nullptr;
    decltype(&xefgSwapChainDestroy) destroy = nullptr;

    bool ready() const {
        return createContext != nullptr && buildPipelines != nullptr && initFromSwapChain != nullptr &&
               getSwapChainPtr != nullptr && setEnabled != nullptr && setPresentId != nullptr &&
               destroy != nullptr;
    }
};

// 进程级只尝试加载一次；失败原因留在 problem 里，caps 的 note[] 会透给 JVM。
// 为什么只加载一次：`LoadLibraryW` 在失败路径上会走一遍搜索（含网络路径时可能很慢），而
// `fillCaps` 每 600 帧就被调用一次 —— 与 P108 的 XeSS/FSR 探测保持同一策略。
struct XessFgRuntime {
    std::mutex lock;
    bool attempted = false;
    XessFgApi api{};
    std::string problem;  // 空 = 运行库与必需入口都在
};

XessFgRuntime& runtime() {
    static XessFgRuntime instance;
    return instance;
}

// 载入运行库。搜索顺序：本模块目录 → 裸 DLL 名（标准搜索路径，发行包里由 P108 拓宽过）。
// 返回 1 = 已载入且必需入口齐备。
int32_t loadLocked(XessFgRuntime& rt) {
    if (rt.attempted) {
        return rt.problem.empty() ? 1 : 0;
    }
    rt.attempted = true;

    std::wstring candidate = selfDirectory();
    HMODULE module = nullptr;
    if (!candidate.empty()) {
        candidate += L"\\";
        candidate += std::wstring(kXessFgDll, kXessFgDll + std::strlen(kXessFgDll));
        module = ::LoadLibraryW(candidate.c_str());
    }
    if (module == nullptr) {
        module = ::LoadLibraryA(kXessFgDll);
    }
    if (module == nullptr) {
        rt.problem = "libxess_fg.dll not found";
        return 0;
    }

    XessFgApi& api = rt.api;
    api.module = module;
    api.getVersion = reinterpret_cast<decltype(api.getVersion)>(
        ::GetProcAddress(module, "xefgSwapChainGetVersion"));
    api.createContext = reinterpret_cast<decltype(api.createContext)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12CreateContext"));
    api.buildPipelines = reinterpret_cast<decltype(api.buildPipelines)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12BuildPipelines"));
    if (api.buildPipelines == nullptr) {
        rt.problem = "xefgSwapChainD3D12BuildPipelines missing";
        return 0;
    }
    api.getProperties = reinterpret_cast<decltype(api.getProperties)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12GetProperties"));
    api.initFromSwapChain = reinterpret_cast<decltype(api.initFromSwapChain)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChain"));
    api.getSwapChainPtr = reinterpret_cast<decltype(api.getSwapChainPtr)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12GetSwapChainPtr"));
    api.setLatencyReduction = reinterpret_cast<decltype(api.setLatencyReduction)>(
        ::GetProcAddress(module, "xefgSwapChainSetLatencyReduction"));
    api.setLoggingCallback = reinterpret_cast<decltype(api.setLoggingCallback)>(
::GetProcAddress(module, "xefgSwapChainSetLoggingCallback"));
    api.tagFrameResource = reinterpret_cast<decltype(api.tagFrameResource)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12TagFrameResource"));
    api.setDescriptorHeap = reinterpret_cast<decltype(api.setDescriptorHeap)>(
        ::GetProcAddress(module, "xefgSwapChainD3D12SetDescriptorHeap"));
    api.tagFrameConstants = reinterpret_cast<decltype(api.tagFrameConstants)>(
        ::GetProcAddress(module, "xefgSwapChainTagFrameConstants"));
    api.setEnabled =
        reinterpret_cast<decltype(api.setEnabled)>(::GetProcAddress(module, "xefgSwapChainSetEnabled"));
    api.setPresentId = reinterpret_cast<decltype(api.setPresentId)>(
        ::GetProcAddress(module, "xefgSwapChainSetPresentId"));
    api.setNumInterpolatedFrames = reinterpret_cast<decltype(api.setNumInterpolatedFrames)>(
        ::GetProcAddress(module, "xefgSwapChainSetNumInterpolatedFrames"));
    api.setUiCompositionState = reinterpret_cast<decltype(api.setUiCompositionState)>(
        ::GetProcAddress(module, "xefgSwapChainSetUiCompositionState"));
    api.getLastPresentStatus = reinterpret_cast<decltype(api.getLastPresentStatus)>(
        ::GetProcAddress(module, "xefgSwapChainGetLastPresentStatus"));
    api.destroy =
        reinterpret_cast<decltype(api.destroy)>(::GetProcAddress(module, "xefgSwapChainDestroy"));

    if (!api.ready()) {
        rt.problem = "libxess_fg.dll missing entry points";
        return 0;
    }
    return 1;
}

// XeSS-FG 的 provider。
//
// 现在（P304）它能做的事：记期望值、把"已启用/未启用"落到 SDK（wrap 之后才有 context，
// 所以这条路径在 P305 之前是空转）、回答 presented()。`wrapSwapChain` 在 P305 里接上
// `xefgSwapChainD3D12CreateContext` + `InitFromSwapChain` + `GetSwapChainPtr`。
//
// `presented()` 的语义与 DLSS-G 对齐：返回"上一次 Present 真正提交的帧数"（1 = 没插帧）。
// XeSS-FG 这边只能从 `xefgSwapChainGetLastPresentStatus` 的 `numFramesActuallyPresented`
// 读；context 没建时恒 1。
class XessFgFrameGenProvider final : public IFrameGenProvider {
public:
    FgBackendId backend() const override { return FgBackendId::XessFg; }

    void fillCaps(mcdr_fg_caps& out) const override { fillXessFgCaps(out); }

    bool setMode(const mcdr_fg_dispatch& desc) override {
        // 与 DLSS-G 同一条约定：setMode 只记录（它可能被 JVM 的设置线程调），真正下发在
        // 下一次 beforePresent。这样"选项生效点不早于下一次 Present"对三家都成立。
        desiredMode_ = desc.mode;
        desiredFrames_ = desc.numFrames < 1 ? 1 : desc.numFrames;
        return true;
    }

    void frameStart() override {
        // P305ah：XeLL 的帧首序列。语义与官方 sample 的 `OnSleep()` 同一位置（一帧开始、
        // 还没采输入/记命令之前）。`xell_developer_guide_english.md:296-300` 是硬顺序：
        //   "`xellSleep` must be the first XeLL API function called with a newly assigned
        //    `frame_id` ... The call must return before any markers can be submitted."
        // 顺序反了 `xellAddMarkerData` 直接返回 -4（实测 0xFFFFFFFC）。
        // 之后 `XELL_SIMULATION_START` 必须是本帧第一条 marker（同 guide :325）。
        //
        // 我们把模拟段压成零长：这个钩子挂在 `acquireSurface`（tick 已经跑完、渲染还没开始），
        // 真正的模拟段在 JVM 侧，native 拿不到它的起止。零长只是让 XeLL 的模拟耗时估计偏低，
        // 不影响协议正确性 —— 四个阶段 marker 每个帧都齐。
        if (handle_ == nullptr || xell_ == nullptr || mode_ == 0) {
            return;
        }
        if (markerOpen_) {
            // 上一帧的 PRESENT_END 没发出去（Present 抛异常/帧被跳过）：补一条，否则 XeLL
            // 的 present 窗口会一直开着，低延迟估算按一个不断变长的窗口算。
            noteMarker(XELL_PRESENT_END, frameId_, "PRESENT_END(late)");
        }
        // 帧号从进程级计数器取，保证跨链、跨 provider 实例都单调递增。
        frameId_ = ++markerFrameCounter();
        markerOpen_ = true;
        noteSleep();
        noteMarker(XELL_SIMULATION_START, frameId_, "SIMULATION_START");
        noteMarker(XELL_SIMULATION_END, frameId_, "SIMULATION_END");
        noteMarker(XELL_RENDERSUBMIT_START, frameId_, "RENDERSUBMIT_START");
    }

    void beforePresent() override {
        // 期望值 → 生效值。`desc.numFrames` 在 ABI 里是**额外帧数**（`FrameGeneration.kt` 的
        // `nativeFrames()`：1 = 多插 1 张 = 2x），跟 XeSS 的"插几帧"是同一个口径，不用换算。
        // 每次 Present 前重算（设置线程随时可能改），真正的 SDK 下发在 tagFrameInputs ——
        // 那里才知道链还在、命令列表可用。
        if (desiredMode_ <= 0) {
            mode_ = 0;
            frames_ = 0;
            return;
        }
        mode_ = 1;
        const int32_t cap = maxInterp_ > 0 ? maxInterp_ : 1;
        int32_t interp = desiredFrames_;
        if (interp < 1) {
            interp = 1;
        }
        if (interp > cap) {
            interp = cap;
        }
        frames_ = interp;
        // P305ah：帧尾两段（顺序同 guide :250-255）：渲染提交结束 → present 开始 → 调用方
        // Present → afterPresent 里的 PRESENT_END。`xellSleep` 不在这里 —— 它必须是"本帧
        // 第一条 XeLL 调用"，已经放在 frameStart 了。`frameId_ == 0` = 本帧还没经过
        // frameStart（开关刚打开的第一次 Present），此时任何 marker 都会因为"没有 sleep
        // 打头的帧号"被 XeLL 拒掉，干脆不发。
        if (xell_ != nullptr && frameId_ != 0) {
            noteMarker(XELL_RENDERSUBMIT_END, frameId_, "RENDERSUBMIT_END");
            noteMarker(XELL_PRESENT_START, frameId_, "PRESENT_START");
        }
    }

    void afterPresent() override {
        // P305ah：PRESENT_END 先发 —— 它在语义上属于上一次 Present，与下面的状态读取无关，
        // 不能因为读状态失败就把它漏掉（guide :255 的调用点在 Present 之后紧邻）。
        if (xell_ != nullptr && markerOpen_) {
            noteMarker(XELL_PRESENT_END, frameId_, "PRESENT_END");
            markerOpen_ = false;
        }
        // 插帧在 SDK 的 Present 内部完成，这一侧只能读结果。`framesPresented` = 上一次 Present
        // 真正提交了几帧（1 = 只有本体，2 = 1 张插帧），`frameGenResult` 是插帧本身的结果码。
        if (handle_ == nullptr || api_.getLastPresentStatus == nullptr) {
            presented_ = 1;
            return;
        }
        xefg_swapchain_present_status_t status{};
        const xefg_swapchain_result_t r = api_.getLastPresentStatus(handle_, &status);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            presented_ = 1;
            if (presentStatusFails_++ == 0) {
                dbgLogWarn("xess-fg: GetLastPresentStatus failed result=0x%X",
                           static_cast<unsigned>(r));
            }
            return;
        }
        presented_ = status.framesPresented > 0 ? static_cast<int32_t>(status.framesPresented) : 1;
        // P306z：XeSS-FG 没有"成功"日志，SDK 只在状态里回答"到底开没开"。每 300 帧留一行，
        // 这是判断"tag 都通了但 SDK 仍 disabled"的唯一入口。
        if (++statusPolls_ % 300 == 0) {
            dbgLogInfo("xess-fg: presents=%llu enabled=%u presented=%d fgResult=0x%X frame_id=%u",
                       static_cast<unsigned long long>(statusPolls_),
                       static_cast<unsigned>(status.isFrameGenEnabled), presented_,
                       static_cast<unsigned>(status.frameGenResult), frameId_);
        }
        if (mode_ != 0 && status.isFrameGenEnabled == 0 && !notedDisabled_) {
            notedDisabled_ = true;
            dbgLogWarn("xess-fg: frame generation requested (frames=%d) but the SDK reports it "
                       "disabled (frameGenResult=0x%X)",
                       frames_, static_cast<unsigned>(status.frameGenResult));
        }
        if (statusLogCounter_++ % 120 == 0) {
            // DEBUG：默认 INFO 级别下不进控制台（每 120 帧一行，放 INFO 就是刷屏）。
            dbgLogDebug("xess-fg: presented=%d enabled=%u fgResult=0x%X (mode=%d frames=%d)",
                        presented_, status.isFrameGenEnabled, static_cast<unsigned>(status.frameGenResult),
                        mode_, frames_);
        }
    }

    int32_t presented() const override { return presented_; }

    void onResize(int32_t displayWidth, int32_t displayHeight) override {
        // 链重建后必须重新 InitFromSwapChain（P305）：XeSS-FG 的 context 绑在链上，
        // 尺寸变了旧 context 就作废。这里先只记下来备诊断。
        width_ = displayWidth;
        height_ = displayHeight;
    }

    void shutdown() override {
        // 顺序：先 `SetEnabled(0)` 让 SDK 停止插帧，再 `Destroy`（链的引用由 surface 持有，
        // 这里不碰）。没 context 时什么也不用做 —— 幂等。
        // P305ae：状态全部归零，否则下一个 provider 实例会带着"已下发过 mode/frames"的假象
        // 跳过 SetEnabled，链上的插帧状态就永远停在上一轮的期望值。
        desiredMode_ = 0;
        desiredFrames_ = 1;
        mode_ = 0;
        frames_ = 0;
        maxInterp_ = 0;
        appliedMode_ = -1;
        appliedFrames_ = -1;
        uiEnabled_ = false;
        notedDisabled_ = false;
        presentId_ = 0;
        presented_ = 1;
        // P305ah：marker 状态跟着归零。XeLL 上下文本身是进程单例，不在这里销毁（guide
        // :247 要求它活得比 XeSS-FG 上下文久）。
        xell_ = nullptr;
        frameId_ = 0;
        markerOpen_ = false;
        markerFails_ = 0;
    }

// P305v：`ERROR_HRESULT_FAILURE (-17)` 只说"某个 D3D12 调用失败"，不说哪一个。
// D3D12 调试层会在 InfoQueue 里留下具体文本 —— 那是唯一能定位失败调用的证据。
void xefgLogCallback(const char* message, xefg_swapchain_logging_level_t level, void* userData)
{
    (void)userData;
    if (message != nullptr) {
        dbgLogInfo("sdk[%d] %.*s", static_cast<int>(level), 420, message);
    }
}

// 同一件事的另一半：D3D12 调试层在 InfoQueue 里留下具体文本（P305v 用它定位过失败调用）。
void dumpInfoQueue(ID3D12Device* device, const char* tag)
{
    if (device == nullptr) {
        return;
    }
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr) {
        return;
    }
    const UINT64 stored = infoQueue->GetNumStoredMessages();
    const UINT64 first = stored > 8 ? stored - 8 : 0;
    for (UINT64 i = first; i < stored; ++i)
    {
        SIZE_T size = 0;
        if (FAILED(infoQueue->GetMessage(i, nullptr, &size)) || size == 0)
        {
            continue;
        }
        std::vector<char> buffer(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(infoQueue->GetMessage(i, message, &size)))
        {
            continue;
        }
        dbgLogWarn("iq[%s] sev=%d id=%d %.*s", tag,
                   static_cast<int>(message->Severity), static_cast<int>(message->ID),
                   static_cast<int>(message->DescriptionByteLength > 240 ? 240 : message->DescriptionByteLength),
                   message->pDescription != nullptr ? message->pDescription : "");
    }
    infoQueue->ClearStoredMessages();
}

    int32_t wrapSwapChain(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain* appChain,
                          int32_t width, int32_t height, DXGI_FORMAT format,
                          IDXGISwapChain** outPresentChain) override {
        // P305 的入口。现在明确拒绝而不是"假装成功"：假装成功会让 Present 走错链，
        // 表现是黑屏/卡死，而且原因很难从日志看出来。
        if (outPresentChain != nullptr) {
            *outPresentChain = nullptr;
        }
        if (!kWrapWired) {
            return 0;
        }
        if (device == nullptr || queue == nullptr || appChain == nullptr || outPresentChain == nullptr) {
            return 0;
        }
        if (handle_ != nullptr) {
            // 已经包装过：surface 在同尺寸重建前会先 unwrap，走到这里说明契约被违反了。
            dbgLogWarn("xess-fg: wrapSwapChain called twice without unwrap (wanted %dx%d)", width, height);
            return 0;
        }

        XessFgRuntime& rt = runtime();
        XessFgApi api{};
        {
            std::lock_guard<std::mutex> lk(rt.lock);
            if (loadLocked(rt) != 1) {
                dbgLogWarn("xess-fg: cannot wrap the chain, runtime unavailable: %s", rt.problem.c_str());
                return 0;
            }
            api = rt.api;
        }

        xefg_swapchain_handle_t handle = nullptr;
        xefg_swapchain_result_t result = api.createContext(device, &handle);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS || handle == nullptr) {
            dbgLogWarn("xess-fg: CreateContext failed result=0x%X", static_cast<unsigned>(result));
            return 0;
        }
        // blocking=1：pipeline 编译完再往下走。非阻塞会把等待挪到 initFromSwapChain，
        // 等于把一次卡顿换成一次更难定位的卡顿，而这里本来就是链重建（已等过 GPU 空闲）。
        result = api.buildPipelines(handle, nullptr, 1, kInitFlags);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            dbgLogWarn("xess-fg: BuildPipelines failed result=0x%X", static_cast<unsigned>(result));
            api.destroy(handle);
            return 0;
        }

        xefg_swapchain_d3d12_init_params_t params{};
        params.pApplicationSwapChain = appChain;
        params.initFlags = kInitFlags;
        // P305ae：这里给的是"上限"而不是"当前值" —— `SetNumInterpolatedFrames` 只能在 init
        // 之后调，且不能超过 init 时声明的上限。以前钉死 1，等于永远只能插 1 帧。
        params.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
        params.creationNodeMask = 0;
        params.visibleNodeMask = 0;
        params.pTempBufferHeap = nullptr;
        params.bufferHeapOffset = 0;
        params.pTempTextureHeap = nullptr;
        params.textureHeapOffset = 0;
        params.pPipelineLibrary = nullptr;
        // AUTO：让 SDK 按我们 tag 的资源自己选合成方式。我们要 tag UI 纹理（needsUiResource=1），
        // 但目前还没有独立 UI 纹理，AUTO 是唯一不说假话的选项。
        // P305am：uiMode 必须和"实际会 tag 什么"一致。声明 HUDLESS_UITEXTURE 就等于承诺
        // 每帧都有一条 UI 纹理进来；没有 UI 纹理时 SDK 会判 -14 INCORRECT_INPUT_RESOURCES
        // （实测：enabled=1 但 fgResult 恒为 0xFFFFFFF2，且界面纹理从未 tag 过）。
        // 所以有纹理才用 HUDLESS_UITEXTURE，否则退回 AUTO（官方 sample 也是 AUTO）。
        params.uiMode = (uiTexture_ != nullptr) ? XEFG_SWAPCHAIN_UI_MODE_HUDLESS_UITEXTURE
                                                : XEFG_SWAPCHAIN_UI_MODE_AUTO;

        // P305j：SDK 的 GetProperties 会告诉我们两块临时堆各需要多大（`tempBufferHeapSize` /
        // `tempTextureHeapSize`，见 xefg_swapchain.h:182/184）。我们此前既不查也不给堆，这是
        // "纯包装、零 tag 也 fastfail" 的首要嫌疑。这里先只查、只打日志，拿到真实数字再决定建多大。
        {
            xefg_swapchain_properties_t props{};
            const xefg_swapchain_result_t pr =
                api.getProperties(handle, &params, 0, 0, DXGI_FORMAT_UNKNOWN, &props);
            dbgLogWarn("xess-fg: stage1 GetProperties result=0x%X tempBufferHeapSize=%llu "
                       "tempTextureHeapSize=%llu maxSupportedInterpolations=%u",
                       static_cast<unsigned>(pr),
                       static_cast<unsigned long long>(props.tempBufferHeapSize),
                       static_cast<unsigned long long>(props.tempTextureHeapSize),
                       props.maxSupportedInterpolations);
        }

        // P305n：`InitFromSwapChain` 要求应用提供两块临时堆，尺寸由 SDK 的 GetProperties 给出
        //（`xefg_swapchain.h:182/184` 的 tempBufferHeapSize / tempTextureHeapSize）。这里再查一次，
        // 拿到的值立刻用来建堆并写回 `params`。堆的所有权留在本模块内（业务模块各自封装）。
        {
            xefg_swapchain_properties_t heaps{};
            const xefg_swapchain_result_t pr2 =
                api.getProperties(handle, &params, static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<DXGI_FORMAT>(format), &heaps);
            dbgLogInfo("stage1: GetProperties result=0x%X bufHeap=%llu texHeap=%llu maxInterp=%u",
                       static_cast<unsigned>(pr2),
                       static_cast<unsigned long long>(heaps.tempBufferHeapSize),
                       static_cast<unsigned long long>(heaps.tempTextureHeapSize),
                       heaps.maxSupportedInterpolations);
            if (pr2 == XEFG_SWAPCHAIN_RESULT_SUCCESS && device != nullptr) {
                if (heaps.tempBufferHeapSize > gTempBufCapacity) {
                    gTempBufferHeap.Reset();
                    D3D12_HEAP_DESC desc{};
                    desc.SizeInBytes = heaps.tempBufferHeapSize;
                    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
                    const HRESULT hr = device->CreateHeap(&desc, IID_PPV_ARGS(&gTempBufferHeap));
                    if (SUCCEEDED(hr)) { gTempBufCapacity = heaps.tempBufferHeapSize; }
                }
                if (heaps.tempTextureHeapSize > gTempTexCapacity) {
                    gTempTextureHeap.Reset();
                    D3D12_HEAP_DESC desc{};
                    desc.SizeInBytes = heaps.tempTextureHeapSize;
                    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
                    const HRESULT hr = device->CreateHeap(&desc, IID_PPV_ARGS(&gTempTextureHeap));
                    if (SUCCEEDED(hr)) { gTempTexCapacity = heaps.tempTextureHeapSize; }
                }
                params.pTempBufferHeap = gTempBufferHeap.Get();
                params.bufferHeapOffset = 0;
                params.pTempTextureHeap = gTempTextureHeap.Get();
                params.textureHeapOffset = 0;
                // P305ae：上限只在这里能问到（init 之前）—— 记下来，SetNumInterpolatedFrames
                // 和 caps 的 maxGeneratedFrames 都用它。
                maxInterp_ = static_cast<int32_t>(heaps.maxSupportedInterpolations);
                maxInterpMirror().store(maxInterp_);
            }
        }
    // P305u：先接 XeLL，再 init —— 顺序由 `xess_fg_developer_guide_english.md:223` 给出。
    // P305x：guide 的 Quick Start 把「注册日志回调」排在 InitFromSwapChain 之前。让 SDK 自己
    // 说出 init 失败的原因，比从 D3D12 调试层反推直接。
    if (api.setLoggingCallback != nullptr)
    {
        const xefg_swapchain_result_t logged = api.setLoggingCallback(
            handle, XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG,
            [](const char* message, xefg_swapchain_logging_level_t level, void* userData)
            {
                (void)userData;
                dbgLogInfo("sdk[%d] %.*s", static_cast<int>(level), 420,
                           message != nullptr ? message : "");
            },
            nullptr);
        if (logged != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            dbgLogWarn("xess-fg: SetLoggingCallback failed result=0x%X", static_cast<unsigned>(logged));
        }
    }
    if (api.setLatencyReduction != nullptr)
    {
        void* xell = xellContext(device);
        const xefg_swapchain_result_t latency = api.setLatencyReduction(handle, xell);
        if (latency != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            dbgLogWarn("xess-fg: SetLatencyReduction failed result=0x%X", static_cast<unsigned>(latency));
            api.destroy(handle);
            return 0;
        }
    }

    result = api.initFromSwapChain(handle, queue, &params);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        dbgLogWarn("xess-fg: InitFromSwapChain failed result=0x%X", static_cast<unsigned>(result));
        dumpInfoQueue(device, "init");
        api.destroy(handle);
        return 0;
    }

    void* presentChain = nullptr;
    result = api.getSwapChainPtr(handle, __uuidof(IDXGISwapChain4), &presentChain);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS || presentChain == nullptr)
    {
        dbgLogWarn("xess-fg: GetSwapChainPtr failed result=0x%X", static_cast<unsigned>(result));
        api.destroy(handle);
        return 0;
    }

        handle_ = handle;
        api_ = api;
        contextReady().store(true);
        width_ = width;
        height_ = height;
        // P305ak：把 proxy 链的格式记下来 —— SDK 要求 HUD-less / UI 的格式与 back buffer
        // 完全一致（guide :860），而我们的世界层是 RGBA8；只有拿到后者的真实格式才能判断
        // -14 是不是格式对不上。
        {
            ComPtr<IDXGISwapChain1> sc1;
            DXGI_SWAP_CHAIN_DESC1 scDesc{};
            if (SUCCEEDED(static_cast<IDXGISwapChain*>(presentChain)->QueryInterface(IID_PPV_ARGS(&sc1))) &&
                sc1 != nullptr && SUCCEEDED(sc1->GetDesc1(&scDesc))) {
            }
        }
        // 新链 = 新会话：SDK 侧的 enabled/插帧数/UI 合成状态都回到初始值，本地记账必须跟着清，
        // 否则下一次 tag 会因为"appliedMode_ 已经是 1"而跳过 SetEnabled。
        appliedMode_ = -1;
        appliedFrames_ = -1;
        uiEnabled_ = false;
        tagFailLogged_ = false;
        notedDisabled_ = false;
        presentStatusFails_ = 0;
        presentId_ = 0;
        presented_ = 1;
        // P305ah：XeLL 上下文是进程单例，换链不换它；帧号也**不能**归零（XeLL 要求单调
        // 递增，见 markerFrameCounter）。只关掉"上一帧还开着"的标记。
        xell_ = xellContext(device);
        markerOpen_ = false;
        markerFails_ = 0;
        dbgLogInfo("xess-fg: wrapped the app chain %dx%d (ui=HUDLESS_UITEXTURE maxInterp=%d)",
                   width, height, maxInterp_);
        *outPresentChain = static_cast<IDXGISwapChain*>(presentChain);
        return 1;
    }

    void setFrameConstants(const mcdr_fg_constants& constants) override
    {
        constants_ = constants;
        hasConstants_ = true;
    }

    void setUiTexture(void* object) override {
        // P305c：用户裁决 = GUI 合成目标当 UI 纹理（XeSS 的 uiMode 用 HUDLESS_UITEXTURE）。
        // 逐帧 `TagFrameResource(type=UI)` 在 tagFrameInputs 里；第一次拿到纹理时顺带把
        // `SetUiCompositionState(ENABLED)` 打开（init 的 uiMode 只是声明，状态初始是 DISABLED）。
        uiTexture_ = object;
    }

    void tagFrameInputs(const FgFrameInputs& in) override {
        // 逐帧 tag 的完整序列（顺序不能颠倒，P305 那次 0xC0000409 就是"包装了但一步没做"）：
        //   1) SetPresentId（严格 +1，跳号会被 SDK 静默丢帧）+ 帧常量；
        //   2) 四个资源各一条 TagFrameResource（HUDless 颜色 / 深度 / MV / UI）；
        //   3) UI 合成开关（第一次拿到 UI 纹理时一次）；
        //   4) SetEnabled / SetNumInterpolatedFrames（只在生效值变化时）。
        ++tagCalls_;
        if (handle_ == nullptr || in.cmdList == nullptr) {
            return;
        }
        // P305d：四步顺序不能颠倒（P305 那次 0xC0000409 就是"包装了但一步没做"）。
        // 1) presentId 严格 +1 —— 跳号会被 SDK 静默丢帧。
        // P305ah：present id 与 XeLL marker 共用一个帧号，由 frameStart() 推进（guide :240）。
        // 这里不再自增：自增会让 marker 的 id 比 present 的 id 慢一帧，XeLL 侧的延迟窗口就
        // 永远对不上。frameId_ == 0 只可能出现在 frameStart 没被调用的路径上，退回自增保底。
        const uint32_t presentId = frameId_ != 0 ? frameId_ : ++presentId_;
        presentId_ = presentId;
        api_.setPresentId(handle_, presentId);
            // xefgSwapChainTagFrameConstants 是必需输入（文档 :678-681）；presentId 必须与
            // 下面 TagFrameResource 用同一个，否则 SDK 会把常量算到另一帧上。
            if (api_.tagFrameConstants != nullptr && hasConstants_)
            {
                xefg_swapchain_frame_constant_data_t frameConstants = {};
                for (int32_t i = 0; i < 16; ++i)
                {
                    frameConstants.viewMatrix[i] = constants_.view[i];
                    frameConstants.projectionMatrix[i] = constants_.projection[i];
                }
                frameConstants.jitterOffsetX = constants_.jitterX;
                frameConstants.jitterOffsetY = constants_.jitterY;
                frameConstants.motionVectorScaleX = constants_.mvScaleX;
                frameConstants.motionVectorScaleY = constants_.mvScaleY;
                frameConstants.resetHistory = constants_.resetHistory != 0 ? 1u : 0u;
                frameConstants.frameRenderTime = constants_.frameRenderTime;
                api_.tagFrameConstants(handle_, presentId, &frameConstants);
            }
        // 2) 四个资源各一条 tag。incomingState 必须是调用方给的**当前真实**状态：
        //    SDK 会按它记录屏障，写错就会让资源实际状态与调用方的跟踪表错位。
        //    没有 BACKBUFFER 那条：`xess_fg_developer_guide_english.md:980` 说明它只在
        //    letterboxing（部分区域插帧）时用来定义插帧矩形，并且"其它字段被忽略"；官方
        //    sample 正常路径也只 tag HUDLESS/MV/DEPTH 三条（`basic_sample.cpp:1266-1329`）。
        // `lazy = true` → `RV_UNTIL_NEXT_PRESENT` 且不传命令列表：SDK 在 Present 时自己拷贝。
        // 官方 sample 只对 HUDLESS_COLOR 用这种（`basic_sample.cpp:1266-1290`），并注明
        // "the SDK copies the texture at present time"。其余资源（MV/深度）用 RV_ONLY_NOW。
        auto tagResource = [&](xefg_swapchain_resource_type_t type, void* resource, D3D12_RESOURCE_STATES state,
                               bool lazy = false) {
            if (api_.tagFrameResource == nullptr) {
                return;
            }
            if (resource == nullptr) {
                return;
            }
            xefg_swapchain_d3d12_resource_data_t data{};
            data.type = type;
            data.validity = lazy ? XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT : XEFG_SWAPCHAIN_RV_ONLY_NOW;
            data.pResource = static_cast<ID3D12Resource*>(resource);
            data.incomingState = state;
            // P305aj：`resourceSize` 必须显式给。结构体注释是 "Valid extent of the resource"，
            // 全零在 SDK 眼里是 0x0 的合法区域 —— 它按这个尺寸分配/拷贝就会失败（实测：
            // `Failed to allocate memory for a copy of HUDLESS_COLOR` + 每帧 -14
            // INCORRECT_INPUT_RESOURCES）。官方 sample 对每条资源都写了
            // `resourceSize = {desc.Width, desc.Height}`（`basic_sample.cpp:1296-1301`）。
            // 约束（guide Resource Tagging）：HUDLESS / UI / back buffer 同尺寸；
            // MV 与 depth 同尺寸 —— 两边各自取整条资源即可。
            const D3D12_RESOURCE_DESC desc =
                static_cast<ID3D12Resource*>(resource)->GetDesc();
            data.resourceBase = {0, 0};
            data.resourceSize = {static_cast<uint32_t>(desc.Width), desc.Height};
            const xefg_swapchain_result_t tagged = api_.tagFrameResource(handle_, lazy ? nullptr : in.cmdList, presentId, &data);
            if (tagged != XEFG_SWAPCHAIN_RESULT_SUCCESS && !tagFailLogged_) {
                tagFailLogged_ = true;
                dbgLogWarn("xess-fg: TagFrameResource(type=%d) failed result=0x%X",
                           static_cast<int>(type), static_cast<unsigned>(tagged));
            }
        };
        // 深度直接用调用方给的真实资源：P305an 那条 D32_FLOAT 探针已经证伪（换格式后 -14 仍在），
        // 而且它 tag 的是一张从未写入的深度，语义上就是错的 —— 已删除，避免它成为新的崩溃点。
        tagResource(XEFG_SWAPCHAIN_RES_DEPTH, in.pDepth, in.depthState);
        tagResource(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, in.pMotionVectors, in.motionVectorState);
        // 世界层 = 无 HUD 的那一帧，就是 SR 的输出（P213 的 HUDless 约定同此）。
        // HUDLESS 用懒拷贝（sample 的写法）：给 SDK 一个"Present 时再拷"的句柄，避免在这一帧
        // 的渲染列表里插拷贝；这条也是 P305an 的单变量实验 —— 之前四条全是 RV_ONLY_NOW。
        // P305ao：默认不发这条（见 `kTagHudless` 的说明），只保留最小契约。
        if (kTagHudless) {
            tagResource(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, in.pColorOutput, in.colorState, /*lazy=*/true);
        }
        // UI 合成状态：init 的 `uiMode` 只是"打算这么合成"的声明，合成开关本身初始是
        // DISABLED（`xefg_swapchain.h:495-511`），必须显式打开，否则 tag 的 UI 纹理不参与合成，
        // 插出来的帧会把 HUD 一起当成世界层去插（重影）。没有 UI 纹理就先不打开。
        if (uiTexture_ != nullptr && !uiEnabled_ && api_.setUiCompositionState != nullptr) {
            const xefg_swapchain_result_t ui =
                api_.setUiCompositionState(handle_, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
            uiEnabled_ = (ui == XEFG_SWAPCHAIN_RESULT_SUCCESS);
            dbgLogInfo("xess-fg: UI composition %s (result=0x%X texture=%p)",
                       uiEnabled_ ? "enabled" : "rejected", static_cast<unsigned>(ui), uiTexture_);
        }
        // UI 纹理 = GUI 合成目标（用户 2026-10-27 裁决，见 P305c）。
        tagResource(XEFG_SWAPCHAIN_RES_UI, uiTexture_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // 3) 描述符堆：只有 `initFlags` 里要了 `XEFG_SWAPCHAIN_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP`
        //    时才需要 `setDescriptorHeap` 交外部堆。我们建链时没要（initFlags=0），SDK 自管它的堆，
        //    所以这里不调 —— 免得拿一个它不认识的偏移去绑。
        // 4) 开关与倍率只在**生效值**变化时下发（P296b：每帧重设会被 SDK 当成状态抖动）。
        //    顺序不能反：SDK 在 enabled 之前设的插帧数会被忽略。
        if (appliedMode_ != mode_) {
            const xefg_swapchain_result_t on =
                api_.setEnabled(handle_, mode_ != 0 ? 1u : 0u);
            appliedMode_ = mode_;
            // P306y：这个返回值以前被直接丢掉，于是"包装成功、SDK 却一直报 disabled"只能在
            // afterPresent 的 warn 里看到结果、看不到原因。
            if (on != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                dbgLogWarn("xess-fg: SetEnabled(%d) failed result=0x%X",
                           mode_ != 0 ? 1 : 0, static_cast<unsigned>(on));
            }
        }
        if (mode_ != 0 && appliedFrames_ != frames_) {
            const xefg_swapchain_result_t nf =
                api_.setNumInterpolatedFrames(handle_, static_cast<uint32_t>(frames_ < 1 ? 1 : frames_));
            appliedFrames_ = frames_;
            if (nf != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                dbgLogWarn("xess-fg: SetNumInterpolatedFrames(%d) failed result=0x%X",
                           frames_, static_cast<unsigned>(nf));
            }
        }
        if (tagCalls_ == 1) {
            dbgLogInfo("xess-fg: tagging through the wrapped chain cmdList=%p color=%p depth=%p "
                       "mv=%p ui=%p mode=%d frames=%d",
                       static_cast<void*>(in.cmdList),
                       static_cast<const void*>(in.pColorOutput), static_cast<const void*>(in.pDepth),
                       static_cast<const void*>(in.pMotionVectors), uiTexture_, mode_, frames_);
        }
    }

    void unwrapSwapChain() override {
        if (handle_ == nullptr) {
            return;
        }
        // P305ae：销毁前先放掉自己这边的引用，别让 SDK 看到"引用计数 ≠ 0"。
        // 这里能放的只有 UI 纹理（GUI 合成目标的借用指针，不是我们的所有权），真正的
        // 交换链引用在 surface 侧 —— 调用点必须在进这里之前 `swapChain.Reset()` +
        // `backBuffers.clear()`（见 `rebuildSwapChainForFg` / `configureSurface` 的注释）。
        uiTexture_ = nullptr;
        uiEnabled_ = false;
        if (api_.destroy != nullptr) {
            api_.destroy(handle_);
        }
        handle_ = nullptr;
        api_ = XessFgApi{};
        contextReady().store(false);
        maxInterpMirror().store(0);
        maxInterp_ = 0;
        dbgLogInfo("xess-fg: released the wrapped chain");
    }

private:
    // P305ah：四个 required marker 里的三个在这里发（RENDERSUBMIT_END / PRESENT_START /
    // PRESENT_END），第四个 RENDERSUBMIT_START 在 frameStart()。失败只在头几次落日志：
    // marker 是"尽力而为"的诊断量，丢一条不该让渲染路径卡住。
    // XeLL 的 sleep：低延迟模式下由库算出"这一帧该等多久"，把 CPU 让给插帧队列。
    // 必须是本帧第一条带新 frame_id 的调用（guide :296-300），失败也继续 —— 少睡一帧
    // 只影响延迟，不影响正确性。前几次失败落日志。
    void noteSleep()
    {
        XellApi& xell = xellApi();
        if (xell_ == nullptr || !xell.ready())
        {
            return;
        }
        const xell_result_t r = xell.sleep(static_cast<xell_context_handle_t>(xell_), frameId_);
        if (r != 0 && sleepFails_ < 4)
        {
            ++sleepFails_;
        }
    }

    void noteMarker(xell_latency_marker_type_t marker, uint32_t frameId, const char* name)
    {
        XellApi& xell = xellApi();
        if (xell_ == nullptr || !xell.ready())
        {
            return;
        }
        const xell_result_t r = xell.addMarkerData(static_cast<xell_context_handle_t>(xell_), frameId, marker);
        if (r != 0 && markerFails_ < 4)
        {
            ++markerFails_;
        }
    }

    xefg_swapchain_handle_t handle_ = nullptr;
    // P305an（诊断开关）：-14 INCORRECT_INPUT_RESOURCES 在 HUDless/MV/深度三条资源里到底
    // 是哪条"不对"，光看格式与尺寸已经排除不掉 —— 真实深度是 R32_TYPELESS（P76f 要求如此，
    // 否则它不能被采样），而 SDK 文档只承诺支持 D32_FLOAT / D24_UNORM 这类**有类型**的深度。
    // 置 true 时改用一条自建的 D32_FLOAT 深度（内容未清、仅供判定）去 tag：如果 fgResult
    // 不再是 -14，就证明格式校验卡在这里。取证完把结论写进 known_bugs.md 并删掉这个开关。
    void* xell_ = nullptr;           // P305ah：XeLL 上下文（进程单例，不持有所有权）
    uint32_t frameId_ = 0;           // XeLL marker 与 SetPresentId 共用的帧号（guide :240）
    bool markerOpen_ = false;        // 本帧的 RENDERSUBMIT_START 已发、PRESENT_END 未发
    int32_t markerFails_ = 0;
    int32_t sleepFails_ = 0;
    int32_t tagCalls_ = 0;
    uint32_t presentId_ = 0;
    int32_t mode_ = 0;
    int32_t frames_ = 1;
    int32_t maxInterp_ = 0;          // init 时 SDK 报的最大插帧数（P305ae）
    bool uiEnabled_ = false;         // SetUiCompositionState(ENABLED) 是否已成功下发
    bool tagFailLogged_ = false;
    bool notedDisabled_ = false;
    int32_t presentStatusFails_ = 0;
    uint64_t statusPolls_ = 0;
    uint32_t statusLogCounter_ = 0;
    int32_t appliedMode_ = -1;
    int32_t appliedFrames_ = -1;
    void* uiTexture_ = nullptr;
    mcdr_fg_constants constants_ = {};
    bool hasConstants_ = false;
    XessFgApi api_{};
    int32_t desiredMode_ = 0;
    int32_t desiredFrames_ = 1;
    int32_t width_ = 0;
    int32_t height_ = 0;
    int32_t presented_ = 1;
};

}  // namespace

bool xessFgRuntimeAvailable() {
    XessFgRuntime& rt = runtime();
    std::lock_guard<std::mutex> lk(rt.lock);
    return loadLocked(rt) == 1;
}

void fillXessFgCaps(mcdr_fg_caps& out) {
    out.backend = MCDR_FG_BACKEND_XESSFG;

    XessFgRuntime& rt = runtime();
    bool loaded = false;
    std::string problem;
    {
        std::lock_guard<std::mutex> lk(rt.lock);
        loaded = loadLocked(rt) == 1;
        problem = rt.problem;
    }

    out.runtimeLoaded = loaded ? 1 : 0;
    // deviceSupported：XeSS-FG 的硬性要求在 `CreateContext(device)` 里检查（SM 版本、
    // 硬件是否支持插帧）。那一步需要真设备，P305 接通后才有真实答案；在那之前不能假称 1。
    // P305：context 建于 `wrapSwapChain` 里，caps 只如实报告"现在有没有"。
    out.deviceSupported = contextReady().load() ? 1 : 0;
    static_cast<void>(0);
    // 链兼容：包装型后端只能在**应用原生链**上工作。SL 代理工厂建的链交不进去
    // （`eUseDXGIFactoryProxy` 与"包装别人的链"互斥），所以这一位是硬门控。
    // P305：包装型后端会要求链回到应用原生（`fgSetMode` 已挂 rebuildPending），所以这里恒 1 ——
    // 真正的"现在这条链能不能用"由 chainOwner 字段与 available 表达，不再重复判一次。
    out.chainCompatible = 1;
    // XeSS-FG 不依赖 Reflex：它自己的低延迟走 XeLL（`xefgSwapChainSetLatencyReduction`），
    // 而 `libxell.dll` 是可选的 —— 所以 needsReflex = 0 是结构事实，不是"还没测"。
    out.needsReflex = 0;
    // UI 合成与 HUDless 是 XeSS-FG 的输入之一（`xefg_swapchain_ui_mode_t` 的 HUDLESS 模式）。
    // 我们的世界层与 GUI 已经分流（D3D12GuiSplit），所以这两份都拿得到 —— 但要由调用方
    // 逐帧喂，所以这两个能力位的含义是"调用方必须提供"。
    out.needsHudless = 1;
    out.needsUiResource = 1;
    // XeSS-FG 不替我们转屏障：`xefg_swapchain_d3d12_resource_data_t` 里的 `incomingState`
    // 是我们声明"交给它时在哪个状态"，转换由我们发（与 FSR-FG 同一条契约，DLSS-G 相反）。
    out.transitionsInputs = 1;
    // `xefgSwapChainD3D12GetProperties` 才能给出真实上限；context 未建时按最常见的 1 报
    //（不虚报，免得让界面提供 SDK 会拒绝的档位）。
    const int32_t knownMax = maxInterpMirror().load();
    out.maxGeneratedFrames = knownMax > 0 ? knownMax : 1;
    out.latencyProvider = MCDR_FG_LATENCY_XELL;
    out.reserved = 0;
    // available = "接线完成 + 运行库在"，不代表此刻已经有 context：包装发生在链重建时，
    // 而 JVM 要靠 available 才会去选这一家（先有鸡还是先有蛋，只能这样定义）。
    out.available = (loaded && kWrapWired && kTaggingWired) ? 1 : 0;

    if (!loaded) {
        // note 只有 28 B，这句话压到 27 字符以内。
        writeCapsNote(out.note, sizeof(out.note), problem.empty() ? "libxess_fg.dll missing" : problem.c_str());
    } else if (out.chainCompatible == 0) {
        writeCapsNote(out.note, sizeof(out.note), "needs app-native chain");
    } else if (!kWrapWired) {
        // 压到 27 字符以内（note 只有 28 B）。
        writeCapsNote(out.note, sizeof(out.note), "XeSS-FG tags not wired");
    } else {
        writeCapsNote(out.note, sizeof(out.note), "XeSS-FG ready");
    }

    // P304：探测结果只在这里能看到（界面读 caps 但不落日志，`sl.log` 也没有）。一次性打一行，
    // 冒烟与实机取证都靠它 —— 三家后端各有一行，格式对齐 `[FG]` 那行。
    static std::atomic<bool> loggedCaps{false};
    if (!loggedCaps.exchange(true)) {
        dbgLogInfo("xess-fg caps: runtime=%d device=%d chainCompat=%d available=%d latency=%d note=\"%s\"",
                   out.runtimeLoaded, out.deviceSupported, out.chainCompatible, out.available, out.latencyProvider,
                   out.note);
    }
}

std::unique_ptr<IFrameGenProvider> makeXessFgProvider() {
    return std::make_unique<XessFgFrameGenProvider>();
}

}  // namespace d3d12_mc

