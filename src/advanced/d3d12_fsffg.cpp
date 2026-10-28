// d3d12_fsffg.cpp — P304/P306：AMD FidelityFX 帧生成（FSR-FG）provider 的实现单元（模块 mcdr.d3d12.device）
//
// 为什么单独一个单元（与 `d3d12_fsr.cpp` 的 SR 分开）：
//   * 主体不同。SR 那条是 `ffx_api` 的"每次 dispatch 一个上采样上下文"；FSR-FG 是
//     **包装交换链**（`ffxCreateContextDescFrameGenerationSwapChainWrapDX12` 收应用的
//     `IDXGISwapChain4**`，还回来的那条才是之后要 Present 的），生命周期跟着链走。
//   * 运行库不同。SR 是 `amd_fidelityfx_upscaler_dx12.dll`，FG 是
//     `amd_fidelityfx_framegeneration_dx12.dll`。发行包里两个都不打（AGENTS.md：AMD 运行库
//     不进发行 JAR），所以 caps 的 note 必须能说清"是这个 DLL 不在"。
//
// 与 DLSS-G / XeSS-FG 的关系：
//   * 与 DLSS-G 互斥在**链**上：DLSS-G 要求 SL 代理工厂建的链，FSR-FG 只接受应用原生链；
//   * 与 XeSS-FG 互斥在**所有权**上：两家都要包装同一条链，只能有一个 context 活着。
//     注册表的"一个 provider"约束天然满足这条。
//
// 调用序列（依据 `Kits\FidelityFX\docs\techniques\frame-interpolation-swap-chain.md` 与
// `Kits\FidelityFX\framegeneration\include\*.h`）：
//   1. 链上下文：`...SwapChainWrapDX12{&chain4, gameQueue}` + pNext 链
//      `...SwapChainVersionDX12{FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION}` → 出参 `chain4`
//      被 SDK 原地替换成代理链，之后 GetBuffer/Present 全走它；
//   2. 帧生成上下文：`...FrameGeneration{displaySize,maxRenderSize,backBufferFormat}` + pNext 链
//      `ffxCreateBackendDX12Desc{device}` + `...FrameGenerationVersion` + `...FrameGenerationHudless`；
//   3. 每帧：`Configure`（`swapChain` = **IDXGISwapChain 指针**，外加 `frameGenerationCallback` +
//      `frameGenerationCallbackUserContext`）→ 取 SDK 的插帧命令列表 → `PrepareV2`
//      （深度/MV/jitter/相机）→ Present。
//   4. 插帧工作量本身**不由我们 Dispatch**：swapchain 在 Present 内部回调
//      `onFrameGenerationDispatch`，把已经填好命令列表/输出纹理的 desc 交回来，我们只把 header
//      转给 `ffxDispatch`（官方 `frame-interpolation-api.md:279` 的推荐做法，比应用自己构造
//      dispatch desc 更不容易踩到命令列表/屏障的坑）。
//
// FFX 不替我们转屏障（P277/P278 已经踩过）：`ffxApiGetResourceDX12` 的 state 参数语义是
// "我交给你时它在哪个状态"，转换由我们发；这也是 `transitionsInputs = 1` 的含义。

module;

#include "_d3d12_module_prelude.h"

#include "mcdr_abi.h"

// FidelityFX 的 API 面：`ffx_api.h` 是函数原型与返回码，`ffx_api_loader.h` 给
// `ffxFunctions` + `ffxLoadFunctions`（5 个入口一次性解析），`ffx_api_dx12.h` 给
// `ffxApiGetResourceDX12` / `ffxApiGetSurfaceFormatDX12` 与后端 desc，`ffx_api_framegeneration_dx12.h`
// 给包装交换链的 desc。只有本单元看得到它们。
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_api_dx12.h>
#include <ffx_framegeneration.h>
#include <ffx_api_framegeneration_dx12.h>

module mcdr.d3d12.device;

namespace d3d12_mc {

namespace {

constexpr const char* kFsrFgDll = "amd_fidelityfx_framegeneration_dx12.dll";

// 产品安全阀（2026-10-27）：包装/配置/PrepareV2/Dispatch 都能返回 0x0，但插帧跑几帧后进程
// 出现 RIP 落在堆地址的 AV，拿不到可用的崩溃栈，而这条路径已经崩掉过用户的游戏。在崩溃能被
// 干净定位、且实机稳定之前一律视为不可用：注册表不包链、不派发。这不是"功能没写完"的占位——
// 包装、逐帧派发、关停顺序都已实现。要复测时改回 true，并先读 `known_bugs.md` 的 P306 条。
// 产品门：打开 = 允许 `wrapSwapChain` 真的接管链并插帧。P306 的崩溃点（PrepareV2）已经修掉，
// 回调式集成也接好了，按用户要求打开做实机验证；再出问题就把这里关回去，比在配置里留个开关干净。
// P306ab（2026-10-28）：画面上"整屏冻结 + 半透明闪 + HUD 双份"的真因是 `presentCallback` 指到
// 一个只计数、什么都不录的 `onPresentComposed`，而 FFX 的 `presentWithUiComposition` 把
// "backbuffer → 交换链图像 + 叠 UI"整件事都委托给这个回调；同时注册成 UI 资源的又是"世界+GUI
// 合成目标"。当时把这里关掉（能力表 available=0 + note）是正确的止损。
// P306ac（2026-10-28）：呈现路径已改成 hudless —— present 只含世界层，UI 由
// `syncUiResourceLocked` 注册的独立 UI 层（透明底 + 预乘 alpha，`flags=0x3`）交给 SDK 合成一次，
// `presentCallback` 交给 SDK 内部 wrapper（传 nullptr，见 `applyFrameLocked` 的注释）。
// `_port\fg_harness --sdk-ui --gap 5 --frames 600` 跑通（0 失败、119 帧不插帧、presents_cb=0），
// 用户批准解闸做实测画面验证。若画面再出问题，把这里改回 false 即可整体退回（不动别的代码），
// 并把现象记进 `known_bugs.md` 的 P306ac。
constexpr bool kWrapWired = true;

// 设备级支持（= 至少成功建过一次链上下文 + 帧生成上下文）。`fillFsrFgCaps` 是 const 且有多处
// 调用点，用文件级原子量传递，避免为了一个布尔量给 provider 加可变的"已建"状态。
std::atomic<bool> gDeviceSupported{false};

// 句柄来自 JVM。世界切换（维度加载）的那一帧，Java 侧的纹理对象可能已经 `close()`（native
// 资源已释放）却还挂在主目标的字段上；`ffxApiGetResourceDX12` 内部第一件事就是
// `pRes->GetDesc()`，对着野指针立刻是一次 AV（2026-10-28 12:58 的 hs_err 就是这个形状：
// `ffxApiGetResourceDX12+0x4f` ← `applyFrameLocked` ← `tagFrameInputs`）。
//
// 这里用 SEH 做一次"这个指针还能不能当资源用"的探测：能拿到描述就用，拿不到就当作本帧没有
// 输入（把插帧关掉、跳过这一帧）。探测函数刻意不带任何需要析构的局部对象，避免 /EHsc 下的
// C2712。宁可少一帧插帧，也不能让整个游戏进程被一个过期句柄带走。
__declspec(noinline) bool resourceIsUsable(ID3D12Resource* pRes, D3D12_RESOURCE_DESC& out) {
    __try {
        out = pRes->GetDesc();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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

// 本模块所在目录（`GetModuleFileNameA(nullptr)` 拿到的是 java.exe，见 P108 的教训）。
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

// 进程级只尝试一次。`ffxLoadFunctions` 自己会把缺的入口留成 nullptr，所以"加载了但缺入口"
// 与"没加载"要分开记 —— 这两种情况用户要做的事不一样（补 DLL vs 换版本）。
struct FsrFgRuntime {
    std::mutex lock;
    bool attempted = false;
    HMODULE module = nullptr;
    ffxFunctions fn{};
    std::string problem;  // 空 = 运行库与五个入口都在
};

FsrFgRuntime& runtime() {
    static FsrFgRuntime instance;
    return instance;
}

int32_t loadLocked(FsrFgRuntime& rt) {
    if (rt.attempted) {
        return rt.problem.empty() ? 1 : 0;
    }
    rt.attempted = true;

    std::wstring candidate = selfDirectory();
    HMODULE module = nullptr;
    if (!candidate.empty()) {
        candidate += L"\\";
        candidate += std::wstring(kFsrFgDll, kFsrFgDll + std::strlen(kFsrFgDll));
        module = ::LoadLibraryW(candidate.c_str());
    }
    if (module == nullptr) {
        module = ::LoadLibraryA(kFsrFgDll);
    }
    if (module == nullptr) {
        rt.problem = "framegeneration dll missing";
        return 0;
    }

    rt.module = module;
    ffxLoadFunctions(&rt.fn, module);
    if (rt.fn.CreateContext == nullptr || rt.fn.DestroyContext == nullptr ||
        rt.fn.Configure == nullptr || rt.fn.Query == nullptr || rt.fn.Dispatch == nullptr) {
        rt.problem = "framegeneration dll entry pts";
        return 0;
    }
    return 1;
}

// D3D12 状态 → FFX 的"交给你时它在哪"标志。只映射本仓真正会交出去的几种；认不出来就按
// compute-read 交（最保守，SDK 会自己插屏障）。
uint32_t toFfxState(D3D12_RESOURCE_STATES state) {
    switch (state) {
        case D3D12_RESOURCE_STATE_RENDER_TARGET:
            return FFX_API_RESOURCE_STATE_RENDER_TARGET;
        case D3D12_RESOURCE_STATE_DEPTH_WRITE:
        case D3D12_RESOURCE_STATE_DEPTH_READ:
            return FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT;
        case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
        case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
            return FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        case D3D12_RESOURCE_STATE_COPY_SOURCE:
            return FFX_API_RESOURCE_STATE_COPY_SRC;
        case D3D12_RESOURCE_STATE_PRESENT:
            return FFX_API_RESOURCE_STATE_PRESENT;
        default:
            return FFX_API_RESOURCE_STATE_COMPUTE_READ;
    }
}

FfxApiResource makeResource(void* resource, D3D12_RESOURCE_STATES state) {
    if (resource == nullptr) {
        return FfxApiResource{};
    }
    return ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(resource), toFfxState(state));
}

// FSR-FG 的 provider。生命周期：`wrapSwapChain` 建两个 context（链 + 帧生成），
// `unwrapSwapChain` 按官方 Shutdown 顺序销毁（先停插帧 → WaitForPresents → 先 FG 上下文、
// 后链上下文，见 frame-interpolation-swap-chain.md 的 "Shutdown"）。
class FsrFgFrameGenProvider final : public IFrameGenProvider {
public:
    FgBackendId backend() const override { return FgBackendId::FsrFg; }

    void fillCaps(mcdr_fg_caps& out) const override { fillFsrFgCaps(out); }

    bool setMode(const mcdr_fg_dispatch& desc) override {
        // 与 DLSS-G / XeSS-FG 同一条约定：只记录，真正下发在 beforePresent。
        std::lock_guard<std::mutex> lk(lock_);
        desiredMode_ = desc.mode;
        desiredFrames_ = desc.numFrames < 1 ? 1 : desc.numFrames;
        reset_ = desc.reset != 0;
        return true;
    }

    // swapchain 在 Present 内部回调这个函数（`frame-interpolation-api.md:279` 的推荐用法）：
    // SDK 已经把一个填好命令列表与输出纹理的 `ffxDispatchDescFrameGeneration` 递过来，我们唯一
    // 要做的是把它的 header 原样转给 `ffxDispatch`。
    //
    // `userCtx` 是 provider 自己（不是 `&fgCtx_`）：SDK 在这里告诉我们**这一帧要生成几张**，
    // 只有 provider 拿到这个数，F3 的 presented 才是真的（以前把 `&fgCtx_` 传进来，回调只能
    // 转发，拿不到“到底插了几帧”，于是 F3 永远显示 1×）。
    static ffxReturnCode_t onFrameGenerationDispatch(ffxDispatchDescFrameGeneration* params, void* userCtx) {
        auto* self = static_cast<FsrFgFrameGenProvider*>(userCtx);
        if (params == nullptr || self == nullptr) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        const ffxReturnCode_t rc = runtime().fn.Dispatch(&self->fgCtx_, &params->header);
        self->noteDispatch(params, rc);
        return rc;
    }

    // SDK 这一帧实际要求生成几张 —— F3 的 presented 只信这个数，不信我们自己推的"倍率"。
    // 失败时回落到 1（= 这一帧没插），并把连续失败次数按 1/600 的节奏打出来：这是"包装成功但
    // 不插帧"的唯一可见证据（以前 dispatch 返回值被扔掉，日志里什么都没有）。
    void noteDispatch(const ffxDispatchDescFrameGeneration* params, ffxReturnCode_t rc) {
        if (rc == FFX_API_RETURN_OK) {
            int32_t generated = static_cast<int32_t>(params->numGeneratedFrames);
            if (generated < 0) {
                generated = 0;
            }
            if (generated > 3) {
                generated = 3;
            }
            presented_.store(generated + 1);
            dispatchFailures_.store(0);
            // 真正"插了几帧"的硬证据：每成功派发一次 +1。`presented_` 只是这一帧的瞬时值，
            // 看不出频率；`app_frames` 与它的比值才是能判读的指标。
            dispatchCount_.fetch_add(1, std::memory_order_relaxed);
            generatedFrames_.fetch_add(static_cast<uint64_t>(generated), std::memory_order_relaxed);
            if (!loggedRunning_) {
                loggedRunning_ = true;
                // DEBUG：控制台默认只看到 WARN 以上；要看这行需要 nativeVerbose/logLevel=DEBUG。
                dbgLogDebug("fsr-fg: interpolation running (generated=%d display=%ux%u)", generated,
                            displayWidth_, displayHeight_);
            }
            return;
        }
        presented_.store(1);
        const int32_t failures = ++dispatchFailures_;
        if (failures == 1 || failures % 600 == 0) {
            dbgLogWarn("fsr-fg: dispatch failed 0x%X (frame=%llu, consecutive=%d)",
                       static_cast<unsigned>(rc), static_cast<unsigned long long>(params->frameID), failures);
        }
    }

    // P306ac：把"UI 资源注册"同步到 `swapCtx_`。调用方必须持有 `lock_`。
    //
    // 注册规则：只有 hudless 模式（`hudlessUi_`）下推过来的纹理才算 UI-only 层；其它情况一律
    // 不注册（P306aa：注册"世界+GUI 合成目标"会让 SDK 在已经含 HUD 的画面上再叠一遍 HUD）。
    //
    // **每帧都要重发一次**（P306ae）：SDK 的 `copyUiResource()` 在拷贝完成后会把
    // `presentInfo.currentUiSurface.resource` 置空（`FrameInterpolationSwapchainDX12.cpp:2145`），
    // 也就是"一次 Present 用一次"。只注册一次的话，第一帧之后 SDK 就当成没有 UI：内部副本被
    // 释放、合成退化成纯拷贝，画面上的 HUD 整层消失且没有任何报错（release 版 FFX_ASSERT 是空宏）。
    // 官方 sample 同样是在每帧的 Configure 里带 `uiResource`（`fsrapirendermodule.cpp:1594-1604`）。
    //
    // 撤销规则：模式关掉 / 纹理被撤销 / 链上下文重建 —— 空注册一次。因为 SDK 的 UI 合成
    // **与插帧无关**：只要还登记着 UI，连不插帧的 Present 也会叠一次
    //（`frame-interpolation-api.md:215`）。
    void syncUiResourceLocked(FsrFgRuntime& rt) {
        const bool wantUi = hudlessUi_ && uiTexture_ != nullptr;
        if (!wantUi && registeredUi_ != nullptr) {
            needUiUnregister_ = true;
        }
        if (wantUi) {
            if (swapCtx_ == nullptr || rt.fn.Configure == nullptr) {
                return;
            }
            D3D12_RESOURCE_DESC uiDesc{};
            if (!resourceIsUsable(static_cast<ID3D12Resource*>(uiTexture_), uiDesc)) {
                // 句柄过期（换世界 / resize 那一帧）：撤销注册、退回"整帧 hudless 插值"，
                // 别把坏指针交给 SDK 的 double-buffer 拷贝。下一帧由 JVM 重新推纹理。
                dbgLogWarn("fsr-fg: ui texture %p is gone, unregistering", uiTexture_);
                uiTexture_ = nullptr;
                needUiUnregister_ = registeredUi_ != nullptr;
                return;
            }
            ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiCfg{};
            uiCfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
            // 状态声明成 COMMON：后端每帧收尾把所有已知资源都推回 COMMON、下帧从 COMMON 起步
            //（`d3d12_device.cpp` 的 endCommandList / beginCommandListWithWait），而 FFX 合成完
            // 也会把资源恢复成这里声明的状态 —— 两边语义这样才对得上。
            uiCfg.uiResource = ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(uiTexture_),
                                                     FFX_API_RESOURCE_STATE_COMMON);
            // USE_PREMUL_ALPHA：GUI 画进"清成 (0,0,0,0)"的目标，RGB 天然已按自身 alpha 预乘
            //（原版 `BlendFunction.TRANSLUCENT` 的 alpha 通道是 ONE/INV_SRC_ALPHA）。
            // DOUBLE_BUFFERING：UI 目标每帧都被重画，不能让 SDK 的异步 present 线程直接引用它。
            uiCfg.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA |
                          FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
            const ffxReturnCode_t rc = rt.fn.Configure(&swapCtx_, &uiCfg.header);
            if (rc == FFX_API_RETURN_OK) {
                needUiUnregister_ = false;
                uiFailLogged_ = false;
                if (registeredUi_ != uiTexture_) {
                    registeredUi_ = uiTexture_;
                    dbgLogWarn("fsr-fg: ui resource registered (ui=%p premul+doublebuffer, per present)",
                               uiTexture_);
                }
            } else {
                // 注册失败就别再试撤销：这一帧的 Present 会走"没有 UI"的合成路径，UI 会缺一帧。
                needUiUnregister_ = false;
                logOnce(uiFailLogged_, "fsr-fg: register ui resource failed 0x%X", static_cast<unsigned>(rc));
            }
            return;
        }
        if (needUiUnregister_ && swapCtx_ != nullptr && rt.fn.Configure != nullptr) {
            ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiCfg{};
            uiCfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
            uiCfg.uiResource = {};  // 空资源 = 撤销（`ffx_api_framegeneration_dx12.h:67` 允许为空）
            uiCfg.flags = 0;
            if (rt.fn.Configure(&swapCtx_, &uiCfg.header) == FFX_API_RETURN_OK) {
                needUiUnregister_ = false;
                registeredUi_ = nullptr;
                dbgLogWarn("fsr-fg: ui resource unregistered (present = world + ui)");
            }
        }
    }

    // P306ac：原来的 `onPresentComposed` 回调已删除 —— 它只计数、什么都不录，而 FFX 在
    // "本帧不插帧"的 present 路径上把整件事（backbuffer → 交换链 + 叠 UI）委托给回调，
    // 空实现等于交换链图像一帧都没被写过（P306ab 的根因）。现在 `cfg.presentCallback`
    // 传 nullptr，由 SDK 内部的 `ffxFrameInterpolationUiCompositionWrapper` 负责 copy + 叠 UI。
    //
    // 逐帧的 FFX 录制全部在 `applyFrameLocked`（录制期、命令列表还开着）。Present 之前
    // 不再往任何命令列表里录东西。这里只做两件不需要命令列表的事：
    //   1) UI 资源的注册/撤销（`RegisterUiResource`，渲染线程、Present 之前）；
    //   2) 停用时的 Configure(false)/没准备好时的 Configure(false) 收尾。
    void beforePresent() override {
        // 每帧都会走到这里（Present 之前），无论开关状态 —— 用它数"应用帧数"。
        // 第 120 帧报一次结论，之后每 600 帧一次：ratio≈2 说明 FFX 真的在插帧，
        // ratio≈1 说明它一次都没回调我们（那时 generated 也恒为 1）。
        // 用 WARN 而不是 INFO：这是用户控制台里唯一能看见的通道（native INFO 只在
        // logLevel>=2 时输出），且只在两个节点各打一行，不构成刷屏。
        const uint64_t appPresents = appPresents_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (everEnabled_) {
            const uint64_t since = ++presentsAfterEnable_;
        if (since == 600) {
            const uint64_t dispatches = dispatchCount_.load(std::memory_order_relaxed);
            const uint64_t generatedTotal = generatedFrames_.load(std::memory_order_relaxed);
            // `dispatches_per_app` 才是"插帧有没有在跑"的判据（应≈1.0）。
            // P306ac 起 `swapchain_presents`/`ratio` 不再可读：present 回调已交给 SDK 内部实现
            //（我们不再收到回调），换成 `hudless_ui`（呈现形态）与 `ui`（UI 是否已注册）。
            dbgLogWarn("fsr-fg: app_frames=%llu dispatches=%llu "
                       "dispatches_per_app=%.2f generated_total=%llu generated=%d enabled=%d prepared=%d "
                       "hudless_ui=%d ui_registered=%d dispatch_failures=%d frame_id=%u",
                       static_cast<unsigned long long>(appPresents),
                       static_cast<unsigned long long>(dispatches),
                       static_cast<double>(dispatches) / static_cast<double>(since),
                       static_cast<unsigned long long>(generatedTotal),
                       presented_.load(std::memory_order_relaxed), enabledInFg_ ? 1 : 0, applied_ ? 1 : 0,
                       hudlessUi_ ? 1 : 0, registeredUi_ != nullptr ? 1 : 0,
                       dispatchFailures_.load(std::memory_order_relaxed), frameId_);
        }
        }
        const bool wantOn = desiredMode_ != 0;
        FsrFgRuntime& rt = runtime();
        std::lock_guard<std::mutex> lk(lock_);
        // UI 资源的注册/撤销每帧同步一次，且必须在下面的早退**之前** —— `wantOn && applied_`
        // 的常态帧正是最需要它生效的那些帧。
        syncUiResourceLocked(rt);
        if (wantOn && applied_) {
            return;
        }
        if (!enabledInFg_ || swapCtx_ == nullptr || fgCtx_ == nullptr || rt.fn.Configure == nullptr) {
            return;
        }
        ffxConfigureDescFrameGeneration cfg{};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = proxyChain_;
        cfg.frameGenerationEnabled = false;
        cfg.frameID = frameId_;
        if (rt.fn.Configure(&fgCtx_, &cfg.header) == FFX_API_RETURN_OK) {
            enabledInFg_ = false;
            applied_ = false;
            presented_.store(1);
        }
    }

    // 每帧的完整派发：配置 → PrepareV2（把 dilate/optical-flow 的 GPU job 录进**本帧自己的**
    // 命令列表）→ 之后由 swapchain 在 Present 内部回调 `onFrameGenerationDispatch` 触发真正的
    // `ffxDispatch`。任何一个返回码不为 0 就放弃这一帧（不插帧本身是合法状态，宁可少一帧
    // 也不要让 SDK 吃半套数据）。
    //
    // 时序是官方 sample 的形状（`fsrapirendermodule.cpp:1494-1520`）：Configure 与 PrepareV2 都在
    // 应用自己的命令列表还开着的时候做，随本帧一起提交。**不能**放在 `beforePresent` 里做 ——
    // 那时命令列表早已 Close+Execute，往里面录东西是非法的（同一个教训见
    // `d3d12_surface.cpp:1837-1842`）。所以这个函数由 `tagFrameInputs` 在录制期调用。
    void applyFrameLocked(ID3D12GraphicsCommandList* cmd) {
        FsrFgRuntime& rt = runtime();
        if (cmd == nullptr || swapCtx_ == nullptr || fgCtx_ == nullptr || rt.fn.Configure == nullptr) {
            applied_ = false;
            return;
        }

        const bool wantOn = desiredMode_ != 0;
        // 停用时只把"关"下发一次，不必每帧 configure。
        if (!wantOn && !enabledInFg_) {
            applied_ = false;
            return;
        }

        // PrepareV2 里空资源会在 SDK 内部直接炸（无 hs_err）。第一帧（加载屏）根本没经过
        // SR/FG 的 tag，所以这里必须等输入齐了再下发。
        if (wantOn && (hudlessColor_ == nullptr || depth_ == nullptr || motionVectors_ == nullptr)) {
            logOnce(inputsMissingLogged_, "fsr-fg: waiting for frame inputs (color=%p depth=%p mv=%p)",
                    hudlessColor_, depth_, motionVectors_);
            applied_ = false;
            return;
        }

        // 句柄可能已经过期（世界切换那一帧，Java 侧对象活着但 native 资源已释放；见
        // `resourceIsUsable` 的注释）。拿不到描述就先把自己关掉并跳过这一帧。
        if (wantOn) {
            bool stale = false;
            void* bad = nullptr;
            // UI 纹理是可选装饰：它过期了就把注册撤掉、退回"整帧 hudless 插值"，不该连累插帧本身。
            // GUI 目标在换世界时会重建，而 JVM 只在句柄变化时推一次，很容易留下一个已释放的旧句柄。
            if (uiTexture_ != nullptr) {
                D3D12_RESOURCE_DESC uiDesc{};
                if (!resourceIsUsable(static_cast<ID3D12Resource*>(uiTexture_), uiDesc)) {
                    dbgLogWarn("fsr-fg: ui texture %p was released, dropping it", uiTexture_);
                    uiTexture_ = nullptr;
                    registeredUi_ = nullptr;
                }
            }
            for (void* const p : {hudlessColor_, depth_, motionVectors_}) {
                if (p == nullptr) {
                    continue;
                }
                D3D12_RESOURCE_DESC desc{};
                if (!resourceIsUsable(static_cast<ID3D12Resource*>(p), desc)) {
                    stale = true;
                    bad = p;
                    break;
                }
            }
            if (stale) {
                logOnce(staleInputsLogged_,
                        "fsr-fg: frame inputs were released under us (color=%p depth=%p mv=%p bad=%p)",
                        hudlessColor_, depth_, motionVectors_, bad);
                ffxConfigureDescFrameGeneration off{};
                off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
                off.swapChain = proxyChain_;
                off.frameGenerationEnabled = false;
                off.frameID = frameId_;
                if (rt.fn.Configure(&fgCtx_, &off.header) == FFX_API_RETURN_OK) {
                    enabledInFg_ = false;
                }
                applied_ = false;
                return;
            }
        }

        ffxConfigureDescFrameGeneration cfg{};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        // `swapChain` 要的是 **IDXGISwapChain 指针**，不是链上下文句柄（`ffx_framegeneration.h:118`）；
        // 传上下文会崩在 DLL 里（P306 第四修）。
        cfg.swapChain = proxyChain_;
        cfg.frameGenerationEnabled = wantOn;
        cfg.allowAsyncWorkloads = false;
        cfg.onlyPresentGenerated = false;
        cfg.generationRect.top = 0;
        cfg.generationRect.left = 0;
        cfg.generationRect.width = static_cast<int32_t>(displayWidth_);
        cfg.generationRect.height = static_cast<int32_t>(displayHeight_);
        // ---- P306ac：hudless 呈现 / 复合呈现，二选一 ----
        //
        // 由 JVM 的 `mcdr_dx12FgSetUiComposition` 选择形态：
        //
        //  a) hudless（`hudlessUi_` = true）：应用 Present 的是**世界层**（GUI 画在另一张 UI-only
        //     目标上，就是 `uiTexture_`）。UI 由 SDK 用 `RegisterUiResource` 合成，所以这里
        //     **不给** HUDLessColor —— backbuffer 本身就不含 UI，再喂一张 hudless 只会让 SDK
        //     多做一次 UI 提取（官方 UiTexture sample 也是空，见 `fsrapirendermodule.cpp:1535`）。
        //     UI 资源的注册/撤销在 `beforePresent` 里做（同一次加锁、同一个 Present 之前）。
        //
        //  b) 复合（`hudlessUi_` = false，老路径）：backbuffer = 世界+GUI。此时**绝不能注册
        //     UI 纹理** —— 注册表里那张 `uiTexture` 就是同一张合成目标，SDK 会在已经含 HUD 的
        //     画面上再叠一遍 HUD（用户实测：HUD 两份、半透明层随插帧闪，P306aa）。插值源改用
        //     HUDLessColor（世界层），由 SDK 自己做 UI 提取。
        const bool uiActive = hudlessUi_ && uiTexture_ != nullptr;
        if (uiActive) {
            cfg.HUDLessColor = {};
        } else {
            cfg.HUDLessColor = makeResource(hudlessColor_, hudlessColorState_);
        }
        cfg.frameID = frameId_;
        // P306ac：**present 回调留空**，让 FFX 用自己内部的 UI 合成实现。
        //
        // 之前这里挂的是"只计数、什么都不录"的 `onPresentComposed`，而 `Present()` 里
        // "本帧不插帧"的分支（`presentWithUiComposition`，FrameInterpolationSwapchainDX12.cpp:1844）
        // 把"backbuffer → 交换链图像 + 叠 UI"**整件事委托给回调** —— 空实现等于什么都不做，
        // 交换链图像一帧都没被写过：加载屏就开始闪、进游戏整屏冻结（P306ab 的真正根因）。
        //
        // 传 nullptr 是官方支持的用法：`getPresentCallback`（:1470）在解析这一步把空指针换成
        // SDK 内部的 `ffxFrameInterpolationUiCompositionWrapper`（:1133），由它自己 copy +
        // 按 flags 叠 UI。`FFX_ASSERT(presentCallback != nullptr)`（:2200）检查的是**解析之后**
        // 的那个内部函数，不是这里这个字段 —— 以前"传 nullptr 就是非法跳转"的结论把两者混为一谈了。
        cfg.presentCallback = nullptr;
        cfg.presentCallbackUserContext = nullptr;
        // P306 回调式集成：插帧工作量不再由我们在这里构造 dispatch desc —— 官方文档
        //（`frame-interpolation-api.md:279`）明确建议把 `ffxDispatch` 交给 swapchain 在 Present
        // 内部回调，让它写进正确的命令列表并（可选）与其它工作重叠。我们只负责把回调原样转发。
        cfg.frameGenerationCallback = &FsrFgFrameGenProvider::onFrameGenerationDispatch;
        // 用户上下文是 provider 本身：回调里要拿 `fgCtx_` + 记录"这一帧生成几张"（见 noteDispatch）。
        cfg.frameGenerationCallbackUserContext = this;
        const ffxReturnCode_t cfgRc = rt.fn.Configure(&fgCtx_, &cfg.header);
        if (cfgRc != FFX_API_RETURN_OK) {
            logOnce(configureFailLogged_, "fsr-fg: configure failed 0x%X (on=%d hudless=%p frame=%llu)",
                    static_cast<unsigned>(cfgRc), wantOn ? 1 : 0, hudlessColor_,
                    static_cast<unsigned long long>(frameId_));
            applied_ = false;
            return;
        }
        enabledInFg_ = wantOn;
        if (wantOn && !everEnabled_) {
            everEnabled_ = true;
            dbgLogWarn("fsr-fg: enabled (frame_id=%u render=%ux%u display=%ux%u)",
                       frameId_, renderWidth_, renderHeight_, displayWidth_, displayHeight_);
        }
        if (!wantOn) {
            applied_ = false;
            presented_.store(1);
            return;
        }

        // 官方 sample（`fsrapirendermodule.cpp:1499`）把 PrepareV2 录在**应用自己的**命令列表上，
        // 文档对 `commandList` 的说明也是 "the command list on which to register render commands"。
        // 插帧那条 SDK 列表只用于「手动派发 ffxDispatchDescFrameGeneration」（文档明确说这是
        // 不推荐的做法），不参与 prepare。
        //
        // PrepareV2 是 3.1.5+ 的唯一非废弃入口。
        ffxDispatchDescFrameGenerationPrepareV2 prep{};
        prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prep.frameID = frameId_;
        prep.commandList = cmd;
        prep.renderSize.width = renderWidth_;
        prep.renderSize.height = renderHeight_;
        prep.jitterOffset.x = jitterX_;
        prep.jitterOffset.y = jitterY_;
        prep.motionVectorScale.x = mvScaleX_;
        prep.motionVectorScale.y = mvScaleY_;
        prep.frameTimeDelta = frameTimeDeltaMs_;
        prep.reset = reset_;
        prep.cameraNear = cameraNear_;
        prep.cameraFar = cameraFar_;
        prep.cameraFovAngleVertical = cameraFovVertical_;
        prep.viewSpaceToMetersFactor = viewSpaceToMeters_;
        prep.depth = makeResource(depth_, depthState_);
        // 深度资源常常是 **R32_TYPELESS**：typeless 不能建 SRV，FFX 的 DX12 后端会失败并留下空
        // 描述符，插帧 compute 一执行就崩（现象是 DLL 里的 AV、无 SDK 消息）。同一块资源按
        // R32_FLOAT（D32_FLOAT 的标准读取格式）描述即可，官方 sample 也是给 typed 资源。
        if (prep.depth.description.format == FFX_API_SURFACE_FORMAT_R32_TYPELESS) {
            prep.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
        }
        prep.motionVectors = makeResource(motionVectors_, motionVectorState_);
        // `renderSize` 以深度/MV 的实际尺寸为准：DLSS 超分路径给的 `renderWidth` 可能是 0（退化成
        // 显示尺寸），而 FFX 要求 `renderSize` 与深度/MV 的描述一致，否则 PrepareV2 返回 0x1。
        if (prep.depth.description.width > 0 && prep.depth.description.height > 0) {
            prep.renderSize.width = prep.depth.description.width;
            prep.renderSize.height = prep.depth.description.height;
        }
        const ffxReturnCode_t prepRc = rt.fn.Dispatch(&fgCtx_, &prep.header);
        if (prepRc != FFX_API_RETURN_OK) {
            logOnce(prepareFailLogged_, "fsr-fg: prepare failed 0x%X (render=%ux%u depth=%p mv=%p)",
                    static_cast<unsigned>(prepRc), renderWidth_, renderHeight_, depth_, motionVectors_);
            applied_ = false;
            return;
        }

        applied_ = true;
        reset_ = false;
        ++frameId_;
        // presented 由 `noteDispatch` 写：这里只代表"这一帧的 prepare 成功了"，不代表插了几帧。
    }

    void afterPresent() override {
        // FFX 的插帧发生在它接管的 Present 里，这一侧无事可做（与另外两家一致）。
    }

    int32_t presented() const override { return static_cast<int32_t>(presented_.load()); }

    void onResize(int32_t displayWidth, int32_t displayHeight) override {
        std::lock_guard<std::mutex> lk(lock_);
        displayWidth_ = displayWidth;
        displayHeight_ = displayHeight;
        // 代理链自己的 ResizeBuffers 由 surface 层调用；这里只更新 generationRect。
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lk(lock_);
        desiredMode_ = 0;
        applied_ = false;
        presented_.store(1);
    }

    // UI 合成：JVM 侧把 GUI 层纹理推过来时才启用（`D3D12FgSetUiTexture`）。FSR-FG 的 UI 是
    // 注册到**链上下文**上的（`ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12`），
    // 与 XeSS-FG 用 tag 的 UI 资源是两条完全不同的路径。
    void setUiTexture(void* object) override {
        std::lock_guard<std::mutex> lk(lock_);
        // 注册表已经把 `Dx12Object*` 解包成 `ID3D12Resource*`（P306m）。这里只记指针并在下一帧
        // 注册到链上下文；资源生命周期由 JVM 侧的 GUI 目标持有，撤销时注册表传 nullptr。
        if (uiTexture_ != object && registeredUi_ != nullptr) {
            // 旧纹理可能已经随 resize 释放：先让 SDK 松手（`beforePresent` 里发空注册），
            // 否则带 double-buffer 的每次 Present 拷贝都会踩到已释放的资源。
            needUiUnregister_ = true;
        }
        uiTexture_ = object;
        registeredUi_ = nullptr;
    }

    // P306ac：hudless 呈现开关（JVM 在"当前后端是 FSR-FG + 本帧有 3D 世界层"时置 1）。
    // 切模式时强制下一帧重下发 Configure：`HUDLessColor` 的给法与 UI 注册都随模式变。
    void setUiComposition(int32_t enabled) override {
        std::lock_guard<std::mutex> lk(lock_);
        const bool on = enabled != 0;
        if (hudlessUi_ == on) {
            return;
        }
        hudlessUi_ = on;
        if (!on && registeredUi_ != nullptr) {
            needUiUnregister_ = true;
        }
        applied_ = false;
        dbgLogWarn("fsr-fg: hudless ui composition %s",
                   on ? "on (present = world layer, ui composed by sdk)" : "off (present = world + ui)");
    }

    // "这次 Present 会由 SDK 合成 UI"吗？注意**不要求 UI 已注册**：注册就发生在同一个 Present
    // 之前的 `beforePresent` 里，先注册再 Present 才不会掉一帧 UI。也**不要求 hudlessUi_ 已打开**：
    // JVM 必须先问"你能不能合成"才敢打开（否则第一次永远打不开，见 P306ac 的 latch 顺序）。
    // 链没包上 / 没有 UI 纹理时回 0 —— 调用方据此继续走"自己把 UI 画进 backbuffer"的老路径。
    bool uiCompositionReady() const override {
        std::lock_guard<std::mutex> lk(lock_);
        // P306ad：必须有**真的发生过插帧 dispatch** 才算"SDK 正在合成 UI"。
        // `proxyChain_` 只证明"链被包过"，不证明"游戏现在 Present 的是代理链"：包链与启用插帧
        // 是两步，没有逐帧输入（没开超分 / 倍率 100%）时 provider 只是建了上下文，FFX 的
        // `Present` 一次都没跑过，UI 注册就落在一条没人 Present 的链上。实测后果：JVM 照样
        // "武装" hudless，present 只送世界层、UI 没人合成 ⇒ 界面整层消失（2026-10-28 19:26
        // 的日志里 `P306ac: hudless presentation on` 之后既没有 `app_frames` 也没有
        // `ui resource registered`）。
        // `dispatchCount_` 由 FFX → `onFrameGenerationDispatch` 累加，只有真在插帧时才增长；
        // 它 > 0 就证明代理链在 Present、`presentInterpolated` / `compositeSwapChainFrame` 在跑，
        // UI 一定有人合成。
        return uiTexture_ != nullptr && swapCtx_ != nullptr && proxyChain_ != nullptr &&
               dispatchCount_.load(std::memory_order_relaxed) > 0;
    }

    void setFrameConstants(const mcdr_fg_constants& constants) override {
        std::lock_guard<std::mutex> lk(lock_);
        jitterX_ = constants.jitterX;
        jitterY_ = constants.jitterY;
        mvScaleX_ = constants.mvScaleX;
        mvScaleY_ = constants.mvScaleY;
        hasConstants_ = true;
    }

    void tagFrameInputs(const FgFrameInputs& inputs) override {
        std::lock_guard<std::mutex> lk(lock_);
        if (!tagLogged_) {
            tagLogged_ = true;
            // 打的是**本帧输入**的尺寸，不是成员里的 `displayWidth_` —— 后者在 wrap 阶段才填，
            // 首帧 tag 时还是 0，写进日志只会让人以为链的尺寸是 0。
            dbgLogWarn("fsr-fg: first tag color=%p depth=%p mv=%p render=%dx%d display=%dx%d cmd=%p",
                       inputs.pColorOutput, inputs.pDepth, inputs.pMotionVectors, inputs.renderWidth,
                       inputs.renderHeight, inputs.displayWidth, inputs.displayHeight,
                       static_cast<void*>(inputs.cmdList));
        }
        cmdList_ = inputs.cmdList;
        hudlessColor_ = inputs.pColorOutput;
        hudlessColorState_ = inputs.colorState;
        depth_ = inputs.pDepth;
        depthState_ = inputs.depthState;
        motionVectors_ = inputs.pMotionVectors;
        motionVectorState_ = inputs.motionVectorState;
        renderWidth_ = inputs.renderWidth > 0 ? inputs.renderWidth : displayWidth_;
        renderHeight_ = inputs.renderHeight > 0 ? inputs.renderHeight : displayHeight_;
        // 相机与帧时：只接受有效值（0 表示上游没填），保留成员里的默认值。
        if (inputs.cameraNear > 0.0f && inputs.cameraFar > inputs.cameraNear) {
            cameraNear_ = inputs.cameraNear;
            cameraFar_ = inputs.cameraFar;
        }
        if (inputs.cameraVerticalFov > 0.0f) {
            cameraFovVertical_ = inputs.cameraVerticalFov;
        }
        if (inputs.frameTimeDeltaMs > 0.0f) {
            frameTimeDeltaMs_ = inputs.frameTimeDeltaMs;
        }
        if (inputs.viewSpaceToMeters > 0.0f) {
            viewSpaceToMeters_ = inputs.viewSpaceToMeters;
        }
        // 输入齐了就在本帧的命令列表上做 Configure + Prepare（见 applyFrameLocked 的时序说明）。
        applyFrameLocked(inputs.cmdList);
    }

    int32_t wrapSwapChain(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain* appChain,
                          int32_t width, int32_t height, DXGI_FORMAT format,
                          IDXGISwapChain** outPresentChain) override {
        if (outPresentChain != nullptr) {
            *outPresentChain = nullptr;
        }
        if (device == nullptr || appChain == nullptr) {
            return 0;
        }
        FsrFgRuntime& rt = runtime();
        std::lock_guard<std::mutex> lk(lock_);
        if (loadLocked(rt) != 1) {
            dbgLogWarn("fsr-fg: %s", rt.problem.c_str());
            return 0;
        }
        if (swapCtx_ != nullptr) {
            destroyContextsLocked(rt);
        }

        // 交给 SDK 的链必须只有我们这一个引用（P305 在 XeSS 上踩过同样的坑）。QueryInterface
        // 会 +1，所以建 context 前把调用方那一份放掉。
        IDXGISwapChain4* chain4 = nullptr;
        if (FAILED(appChain->QueryInterface(__uuidof(IDXGISwapChain4), reinterpret_cast<void**>(&chain4))) ||
            chain4 == nullptr) {
            dbgLogWarn("fsr-fg: app chain is not IDXGISwapChain4, skip wrap");
            return 0;
        }
        appChain->Release();

        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 swapVersion{};
        swapVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        swapVersion.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

        ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrapDesc{};
        wrapDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
        wrapDesc.header.pNext = &swapVersion.header;
        wrapDesc.swapchain = &chain4;
        wrapDesc.gameQueue = queue;

        ffxContext swapCtx = nullptr;
        const ffxReturnCode_t wrapRc = rt.fn.CreateContext(&swapCtx, &wrapDesc.header, nullptr);
        if (wrapRc != FFX_API_RETURN_OK || swapCtx == nullptr) {
            chain4->Release();
            dbgLogWarn("fsr-fg: swapchain wrap failed 0x%X", static_cast<unsigned>(wrapRc));
            return 0;
        }

        ffxCreateContextDescFrameGenerationVersion fgVersion{};
        fgVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        fgVersion.version = FFX_FRAMEGENERATION_VERSION;

        ffxCreateBackendDX12Desc backendDesc{};
        backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backendDesc.header.pNext = &fgVersion.header;
        backendDesc.device = device;

        // HUDless 格式必须声明给 FG 上下文：provider 用它设 `previousInterpolationSourceFormat`。
        const uint32_t bbFormat = ffxApiGetSurfaceFormatDX12(format);
        ffxCreateContextDescFrameGenerationHudless hudlessDesc{};
        hudlessDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hudlessDesc.hudlessBackBufferFormat = bbFormat;
        fgVersion.header.pNext = &hudlessDesc.header;

        ffxCreateContextDescFrameGeneration fgDesc{};
        fgDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        fgDesc.header.pNext = &backendDesc.header;
        // 深度约定必须和 SR 一致：本项目是 reverse-Z（`d3d12_fsr.cpp:21` 的同一结论），Far 面
        // 自 26.3 起有限，所以只置 DEPTH_INVERTED、**不**置 DEPTH_INFINITE。不置 inverted 时
        // FFX 会把反向深度当普通深度，光流/深度测试全错（官方 sample 同样按 `s_InvertedDepth`
        // 条件置位，`fsrapirendermodule.cpp:1107-1110`）。
        // DEBUG_CHECKING 是官方 UI 里的调试开关（sample `:1118-1121`，默认关）：它会在运行时
        // 校验 API 入参，Release 路径开着只会拖慢并放大“半套输入”的失败面，这里不带。
        fgDesc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        fgDesc.displaySize.width = static_cast<uint32_t>(width > 0 ? width : 1);
        fgDesc.displaySize.height = static_cast<uint32_t>(height > 0 ? height : 1);
        fgDesc.maxRenderSize = fgDesc.displaySize;
        fgDesc.backBufferFormat = bbFormat;

        ffxContext fgCtx = nullptr;
        const ffxReturnCode_t fgRc = rt.fn.CreateContext(&fgCtx, &fgDesc.header, nullptr);
        if (fgRc != FFX_API_RETURN_OK || fgCtx == nullptr) {
            rt.fn.DestroyContext(&swapCtx, nullptr);
            chain4->Release();
            dbgLogWarn("fsr-fg: frame generation context failed 0x%X", static_cast<unsigned>(fgRc));
            return 0;
        }

        swapCtx_ = swapCtx;
        fgCtx_ = fgCtx;
        // 新的链上下文不认旧的 UI 注册；不清掉的话 `registeredUi_ == uiTexture_` 会让我们
        // 以为已经注册过，实际上新上下文里没有 UI —— 插帧照常出，只是画面里没有 GUI。
        registeredUi_ = nullptr;
        // SDK 在内部把 `chain4` 原地替换成代理链（provider 的 `*desc->swapchain = ...`），所以这里
        // 拿到的就是之后要 Present 的那条链；surface 负责持有它的引用。
        proxyChain_ = reinterpret_cast<IDXGISwapChain*>(chain4);
        proxyChain3_ = nullptr;
        if (chain4 != nullptr) {
            (void)chain4->QueryInterface(__uuidof(IDXGISwapChain3),
                                         reinterpret_cast<void**>(&proxyChain3_));
        }
        displayWidth_ = fgDesc.displaySize.width;
        displayHeight_ = fgDesc.displaySize.height;
        renderWidth_ = displayWidth_;
        renderHeight_ = displayHeight_;
        frameId_ = 1;
        enabledInFg_ = false;
        configureFailLogged_ = false;
        cmdFailLogged_ = false;
        prepareFailLogged_ = false;
        inputsMissingLogged_ = false;
        loggedRunning_ = false;
        gDeviceSupported.store(true);

        // proxy 链在第一次 GetBuffer/Present 就进 FFX 的呈现路径，那条路径要读 generationRect 与
        // 帧号。包装成功后立刻下发一次 "关"，让 proxy 从一开始就有完整描述。
        {
            ffxConfigureDescFrameGeneration initial{};
            initial.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            initial.swapChain = proxyChain_;
            initial.frameGenerationEnabled = false;
            initial.allowAsyncWorkloads = false;
            initial.onlyPresentGenerated = false;
            initial.generationRect.top = 0;
            initial.generationRect.left = 0;
            initial.generationRect.width = static_cast<int32_t>(displayWidth_);
            initial.generationRect.height = static_cast<int32_t>(displayHeight_);
            initial.frameID = frameId_;
            const ffxReturnCode_t initRc = rt.fn.Configure(&fgCtx_, &initial.header);
            if (initRc != FFX_API_RETURN_OK) {
                dbgLogWarn("fsr-fg: initial configure failed 0x%X", static_cast<unsigned>(initRc));
            }
        }

        if (outPresentChain != nullptr) {
            // 所有权交给 surface（它会 Attach）；provider 不再持有代理链的引用，否则 surface 的
            // Reset() 之后代理链析构不掉。
            *outPresentChain = reinterpret_cast<IDXGISwapChain*>(chain4);
        } else {
            chain4->Release();
        }

        dbgLogInfo("fsr-fg: wrapped display=%ux%u format=%d proxy=%p ctx(swap=%p fg=%p)",
                   displayWidth_, displayHeight_, static_cast<int>(format),
                   static_cast<void*>(chain4), swapCtx, fgCtx);
        return 1;
    }

    void unwrapSwapChain() override {
        FsrFgRuntime& rt = runtime();
        std::lock_guard<std::mutex> lk(lock_);
        destroyContextsLocked(rt);
    }

private:
    // 一次性日志：同一类失败只在第一次出现时落一行，避免每帧刷屏。
    static void logOnce(bool& flag, const char* fmt, ...) {
        if (flag) {
            return;
        }
        flag = true;
        char buffer[256] = {};
        va_list args;
        va_start(args, fmt);
        (void)std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        dbgLogWarn("%s", buffer);
    }


    void destroyContextsLocked(FsrFgRuntime& rt) {
        if (fgCtx_ != nullptr && rt.fn.Configure != nullptr) {
            ffxConfigureDescFrameGeneration off{};
            off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            off.swapChain = proxyChain_;
            off.frameGenerationEnabled = false;
            rt.fn.Configure(&fgCtx_, &off.header);
        }
        if (swapCtx_ != nullptr && rt.fn.Dispatch != nullptr) {
            ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
            wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
            rt.fn.Dispatch(&swapCtx_, &wait.header);
        }
        // 顺序按官方 Shutdown：先帧生成上下文，再链上下文。
        if (fgCtx_ != nullptr) {
            rt.fn.DestroyContext(&fgCtx_, nullptr);
            fgCtx_ = nullptr;
        }
        if (swapCtx_ != nullptr) {
            rt.fn.DestroyContext(&swapCtx_, nullptr);
            swapCtx_ = nullptr;
        }
        gDeviceSupported.store(false);
        enabledInFg_ = false;
        applied_ = false;
        loggedRunning_ = false;
        // 新链上下文要重新注册 UI 纹理（同一块纹理在新上下文里没有登记过）。
        registeredUi_ = nullptr;
        needUiUnregister_ = false;
        if (proxyChain3_ != nullptr) {
            proxyChain3_->Release();
            proxyChain3_ = nullptr;
        }
        proxyChain_ = nullptr;
    }

    mutable std::mutex lock_;
    ffxContext swapCtx_ = nullptr;
    ffxContext fgCtx_ = nullptr;
    // proxy 链的裸指针（surface 持有它的引用；这里只借来填 Configure 的 swapChain 字段）。
    IDXGISwapChain* proxyChain_ = nullptr;
    // 同一个代理链的 3 代接口，只为 `GetCurrentBackBufferIndex()` / `GetBuffer()`。
    IDXGISwapChain3* proxyChain3_ = nullptr;

    void* hudlessColor_ = nullptr;
    void* uiTexture_ = nullptr;
    // 已经成功注册到当前 `swapCtx_` 上的 UI 纹理（裸指针只用于同一性比较，不持有引用）。
    void* registeredUi_ = nullptr;
    // P306ac：hudless 呈现模式（present 的世界层不含 UI、UI 由 SDK 合成）。
    bool hudlessUi_ = false;
    // 需要在下一个 `beforePresent` 里显式撤销 UI 注册（空资源）。
    bool needUiUnregister_ = false;
    void* depth_ = nullptr;
    void* motionVectors_ = nullptr;
    D3D12_RESOURCE_STATES hudlessColorState_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList* cmdList_ = nullptr;
    D3D12_RESOURCE_STATES depthState_ = D3D12_RESOURCE_STATE_DEPTH_READ;
    D3D12_RESOURCE_STATES motionVectorState_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    uint32_t displayWidth_ = 0;
    uint32_t displayHeight_ = 0;
    uint32_t renderWidth_ = 0;
    uint32_t renderHeight_ = 0;
    uint64_t frameId_ = 1;
    float jitterX_ = 0.0f;
    float jitterY_ = 0.0f;
    float mvScaleX_ = 1.0f;
    float mvScaleY_ = 1.0f;
    float frameTimeDeltaMs_ = 16.6f;
    // 相机参数：PrepareV2 要用（官方 sample 逐帧从相机取）。上游没给时保留这组保守默认值，
    // 至少不会因为 0 近平面 / 0 视场让 SDK 判成非法输入。
    float cameraNear_ = 0.05f;
    float cameraFar_ = 1024.0f;
    float cameraFovVertical_ = 1.0f;
    float viewSpaceToMeters_ = 1.0f;
    bool hasConstants_ = false;
    bool reset_ = false;

    int32_t desiredMode_ = 0;
    int32_t desiredFrames_ = 1;
    bool applied_ = false;
    bool enabledInFg_ = false;
    bool configureFailLogged_ = false;
    bool cmdFailLogged_ = false;
    bool prepareFailLogged_ = false;
    bool inputsMissingLogged_ = false;
    bool staleInputsLogged_ = false;
    bool uiFailLogged_ = false;
    bool loggedRunning_ = false;
    std::atomic<int32_t> presented_{1};
    // 连续 dispatch 失败次数（成功清零）。用来区分"回调没发生"（= 一直是 0，presented 永远 1）
    // 与"回调发生了但每帧都失败"（持续增长）。
    std::atomic<int32_t> dispatchFailures_{0};
    // 应用侧 Present 次数。
    std::atomic<uint64_t> appPresents_{0};
    // 插帧确实发生的硬计数：成功派发次数、累计生成帧数（`noteDispatch` 里累加）。
    std::atomic<uint64_t> dispatchCount_{0};
    std::atomic<uint64_t> generatedFrames_{0};
    // 只有在真的向 SDK 下发过 enabled=true 之后才开始数统计帧：加载屏/标题页的 600 帧里 SR
    // 根本不跑，在那里打 ratio 只会给出误导性的 enabled=0（P306u 实测）。
    bool everEnabled_ = false;
    bool tagLogged_ = false;
    uint64_t presentsAfterEnable_ = 0;
};

}  // namespace

bool fsrFgRuntimeAvailable() {
    FsrFgRuntime& rt = runtime();
    std::lock_guard<std::mutex> lk(rt.lock);
    return loadLocked(rt) == 1;
}

void fillFsrFgCaps(mcdr_fg_caps& out) {
    out.backend = MCDR_FG_BACKEND_FSRFG;

    FsrFgRuntime& rt = runtime();
    bool loaded = false;
    std::string problem;
    {
        std::lock_guard<std::mutex> lk(rt.lock);
        loaded = loadLocked(rt) == 1;
        problem = rt.problem;
    }

    out.runtimeLoaded = loaded ? 1 : 0;
    // 设备支持 = 至少成功建过一次链上下文 + 帧生成上下文（`wrapSwapChain` 里置位）。
    out.deviceSupported = gDeviceSupported.load() ? 1 : 0;
    // 链要求：FSR-FG 要一条**应用原生**链（SDK 自己包），而工厂是在每次建链时按 `picked` 选的
    // （`createDxgiFactory` → `fgWantsAppNativeChain`）。所以"兼容"必须按**将要建的链**判断，
    // 不能按**现在这条链**：FG 默认关着进场时链可能是 DLSS-G 的 SL 代理链，若这里返回 0，
    // `ensureProviderLocked` 永远建不出 provider → `fgSetMode` 直接返回 → 连重建都不会挂，
    // 用户点开 FSR-FG 就永远是"没有反应"（这个死锁 2026-10-28 查出来）。与 XeSS-FG 的
    // `out.chainCompatible = 1` 保持一致：真正的失败路径（独占全屏）由重建时的
    // `rebuildSwapChainForFg` 拒绝并通过 `lastReason` 报给 UI。
    out.chainCompatible = 1;
    // FSR-FG 不依赖 Reflex；它的低延迟方案是 Anti-Lag 2，而仓库里只有头、没有 DLL，这里只是架构位。
    out.needsReflex = 0;
    out.needsHudless = 1;
    out.needsUiResource = 1;
    out.transitionsInputs = 1;
    // 4.0.1 的 swapchain 支持一帧插多张（最多 3 张），本仓 UI 只暴露 2x；不虚报上限。
    out.maxGeneratedFrames = 2;
    out.latencyProvider = MCDR_FG_LATENCY_ANTILAG2;
    out.reserved = 0;
    // 可用 = 运行库在 + 链兼容 + 产品门打开。`deviceSupported` 只有真建过上下文才为真，不参与这里：
    // 用户第一次打开开关时正是"还没建过"的状态，不能因此报不可用。
    out.available = (loaded && kWrapWired) ? 1 : 0;

    if (!loaded) {
        writeCapsNote(out.note, sizeof(out.note), problem.empty() ? "fsr-fg runtime missing" : problem.c_str());
    } else if (!kWrapWired) {
        // 只在有人把产品门改回 false 时走到。P306ab 已由 P306ac 的 hudless 呈现解决，所以这里
        // 不再宣称"呈现路径有问题"，只如实说明这条后端在本构建里被关了、以及去哪儿看复测步骤。
        writeCapsNote(out.note, sizeof(out.note), "FSR-FG gated off in this build (kWrapWired=false); see known_bugs.md P306ac");
    } else {
        writeCapsNote(out.note, sizeof(out.note), "FSR-FG ready");
    }

    static std::atomic<bool> loggedCaps{false};
    if (!loggedCaps.exchange(true)) {
        dbgLogInfo("fsr-fg caps: runtime=%d device=%d chainCompat=%d available=%d latency=%d note=\"%s\"",
                   out.runtimeLoaded, out.deviceSupported, out.chainCompatible, out.available, out.latencyProvider,
                   out.note);
    }
}

std::unique_ptr<IFrameGenProvider> makeFsrFgProvider() {
    return std::make_unique<FsrFgFrameGenProvider>();
}

}  // namespace d3d12_mc
