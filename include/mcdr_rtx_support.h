#pragma once
//
// MCDR 超分/降噪 SDK 可用性探测（DLSS / NRD / XeSS / FidelityFX）
// ---------------------------------------------------------------------------
// 这一层只回答「这台机器上哪些 SDK 装好了、装的是哪个版本」，不做任何超分/降噪
// 渲染。结果由 mcdr.d3d12.device 的 capabilitiesText/capabilitiesJson 带出去，
// 供 Java 侧决定 UI 上要不要给出对应的开关。
//
// 四个 SDK 的引入方式各不相同，这正是本文件存在的原因：
//   DLSS       —— 导入库 + 运行期 nvngx_dlss.dll，还要 IDXGIAdapter 才能问硬件
//   NRD        —— 静态库，编译期就定死了版本和降噪器清单
//   XeSS       —— 导入库 + 运行期 libxess.dll，版本由 xessGetVersion 直接给出
//   FidelityFX —— 运行时 LoadLibrary + GetProcAddress，没有任何导入库
// 所以「可用」在这四家里含义不同，各自的字段注释里写清楚了是哪一种。
//
// 为什么是普通头文件而不是模块：
//   设备模块（mcdr.d3d12.device）与探测实现单元（src\core\d3d12_rtx_probe.cpp）
//   都要读它。放进模块就意味着探测实现必须 import 设备模块、设备模块又 import
//   探测模块 —— 循环。所以这里与 mcdr_abi.h 用同一套办法：一个纯数据结构体
//   加几个自由函数，实现留在探测单元里，谁都能调。
//
// 两个 SDK 的引入方式不同，可用性的含义也不同：
//   * DLSS/NGX —— 导入库 nvsdk_ngx_{d,d_dbg}.lib 只提供入口，真正的实现是
//     运行期的 nvngx_dlss.dll。所以「链接成功」不等于「可用」：本探测先动态
//     加载 nvngx_dlss.dll 确认运行时在，再调 NVSDK_NGX_D3D12_GetFeatureRequirements
//     逐个特性问「这台机器支不支持」—— 该调用明确不需要先 NVSDK_NGX_Init，
//     正好适合探测（见 nvsdk_ngx.h 的注释）。
//   * NRD —— 编成静态库直接链进 MCDR.dll，没有运行期 DLL。它的 LibraryDesc
//     由 nrd::GetLibraryDesc() 返回，不需要设备、不需要建实例，因此可以完整
//     报出编译进来的降噪器清单。
//
// 平台：两个 SDK 都只有 64 位，MCDR_HAS_DLSS / MCDR_HAS_NRD 仅在 x64 定义
// （见 MCDR.vcxproj）。Win32 下本结构体依然存在、字段全为「不可用」，调用方
// 不需要写 #ifdef。
#include <dxgi.h>  // probeRtxSupport 形参用到 IDXGIAdapter；项目里 dxgi.h 本来就有
#include <string>
#include <vector>

namespace mcdr
{
    // 探测结果。所有字段都是「查到了什么」而不是「能不能渲染」。
    struct RtxSupport
    {
        // ---- DLSS / NGX ----
        bool dlssProbed = false;     // 探测函数被调用过（区分「没探测」与「探测了但全不支持」）
        bool dlssQueried = false;    // 拿到了真实 IDXGIAdapter 并逐个特性问过硬件/驱动
        std::string dlssQueryNote;   // 没问成时说明原因（此时三个支持位一律为 false）
        bool ngxRuntime = false;     // nvngx_dlss.dll 已找到并成功加载
        std::string ngxRuntimePath;  // 实际加载到的路径（诊断用；失败时为空）
        std::string ngxNote;         // 失败原因 / 加载失败时的错误文字

        // 三个特性的硬件+驱动支持位。含义是「这台机器跑得动」，
        // 与「MCDR 是否已经实现了该特性的渲染路径」是两件事。
        bool dlssSuperResolution = false;    // DLSS-SR  超分
        bool dlssFrameGeneration = false;    // DLSS-FG  帧生成
        bool dlssRayReconstruction = false;  // DLSS-RR  光线重建
        std::string dlssSuperResolutionWhy;  // 不支持时的原因（支持则为空）
        std::string dlssFrameGenerationWhy;
        std::string dlssRayReconstructionWhy;

        // ---- NRD ----
        bool nrdLinked = false;  // x64 构建已静态链接 NRD
        std::string nrdVersion;  // 形如 "4.18.0"
        std::string nrdVersionDate;
        std::vector<std::string> nrdDenoisers;  // 例如 "REBLUR", "RELAX", "SIGMA"

        // ---- XeSS（Intel） ----
        // 与 DLSS 的关键差别：xessGetVersion 是**全局函数**，不需要设备、不需要
        // 上下文，所以版本号一加载 DLL 就能拿到；真正的设备能力要到
        // xessD3D12CreateContext 才知道，那属于「真能跑」那一层，本探测不做。
        bool xessProbed = false;     // 探测函数被调用过
        bool xessRuntime = false;    // libxess.dll 已找到并成功加载
        std::string xessVersion;     // 形如 "2.0.2"，来自 xessGetVersion 的 xess_version_t
        // libxess.dll 的文件版本资源（形如 "2.0.2.68"）。
        // 为什么两个版本都要报：XeSS SDK 的**包名**与运行期版本号并不一致 ——
        // 本仓库落地的包叫 XeSS_SDK_3.0.2，但其中的 libxess.dll 自报 2.0.2
        // （文件版本 2.0.2.68）。只报一个数都会被误读成「装的不是这个版本」，
        // 所以两个都带上，让报告自己解释清楚。
        std::string xessFileVersion;
        std::string xessRuntimePath; // 实际加载到的路径（诊断用）
        std::string xessNote;        // 失败原因 / 加载失败时的错误文字
        // 帧生成与低延迟是**独立的 DLL**（libxess_fg.dll / libxell.dll）。
        // 它们各自存在不代表这台机器跑得动，只代表运行时文件齐了。
        bool xessFrameGenPresent = false;  // libxess_fg.dll 已加载
        bool xessLowLatencyPresent = false; // libxell.dll 已加载

        // ---- FidelityFX（AMD FSR） ----
        // 这一家是插件/加载器架构：amd_fidelityfx_loader_dx12.dll 只导出
        // ffxCreateContext 等 5 个入口，全部效果走描述符链分派；上游**不提供**
        // 任何导入库，所以这里一律 GetProcAddress。版本号取自各 DLL 的
        // 文件版本资源（免设备可取）；具体效果能枚举出哪些版本要 ffxQuery
        // 配合真实设备，同样属于「真能跑」那一层。
        bool ffxProbed = false;        // 探测函数被调用过
        bool ffxLoaderLoaded = false;  // loader DLL 已加载，且 5 个入口全部解析成功
        std::string ffxLoaderVersion;  // 形如 "2.3.0.0"
        std::string ffxLoaderPath;     // 实际加载到的路径（诊断用）
        std::string ffxNote;           // 失败原因
        // 各效果运行时是否随包部署（只查文件在位，不试创建上下文）。
        bool ffxUpscalerPresent = false;    // amd_fidelityfx_upscaler_dx12.dll
        bool ffxFrameGenPresent = false;    // amd_fidelityfx_framegeneration_dx12.dll
        bool ffxDenoiserPresent = false;    // amd_fidelityfx_denoiser_dx12.dll
        bool ffxRadianceCachePresent = false; // amd_fidelityfx_radiancecache_dx12.dll
        // 各自的文件版本，便于一眼看出 SDK 里面装的是 FSR 几。
        std::string ffxUpscalerVersion;   // 例如 FSR 超分 "4.1.1.0"
        std::string ffxFrameGenVersion;   // 例如 "4.0.1.0"
        std::string ffxDenoiserVersion;
        std::string ffxRadianceCacheVersion;

        // 单行摘要，直接拼进 capabilitiesText。形如
        // "ngx=yes(SR,FG) nrd=4.18.0[REBLUR,RELAX] xess=2.0.2.68[fg,ll] ffx=2.3.0.0[sr,fg,dn,rc]"
        // 某个 SDK 不可用时对应段为 "no" / "off"。
        std::string summary() const;

        // 生成 JSON 片段：不带外层花括号、不带前导逗号，调用方负责拼接。
        // 键名一律 rtx_ 前缀，与已有的 caps_* / feature_level 区分开。
        std::string jsonFragment() const;
    };

    // 探测一次并缓存。可重复调用，只有第一次真正干活；适配器在启动后不会变，
    // 所以缓存是安全的。线程安全由调用方保证（只在设备初始化/能力查询路径调）。
    //
    // adapter：设备当前使用的 DXGI 适配器。NGX 的 GetFeatureRequirements 必须
    // 拿到它才能给出「这台机器支不支持」的准确答案，所以在设备建好之后调用时
    // 一定要传（设备模块的 gCtx.adapter 就是它）。传 nullptr 时只报告「运行期
    // DLL 在不在」，不会把「没适配器」误报成「不支持」——见 dlssQueried。
    //
    // XeSS / FidelityFX 这两家不需要适配器，首次调用就会全部做完。
    const RtxSupport& probeRtxSupport(IDXGIAdapter* adapter);

    // 显式放弃缓存，仅供自检使用（例如换了适配器之后重测）。
    void resetRtxSupportCache();
}