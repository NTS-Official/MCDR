// dx12-mc 原生 D3D12 层（C++）——设备 / 资源 / 命令 / 管线 / surface 的模块接口单元
//
// 本文件由 MCDR\src\dx12\dx12_device.h 逐行改写而来（仅去掉 #pragma once、NOMINMAX
// 守卫、#include 与 namespace 包裹，其余声明保持逐字不变），职责与迁移前的头文件一致。
//
// 模块边界（迁移决策）：
//   * 本模块同时容纳 dx12_device.h 里的全部声明，以及原本只在该头文件里声明、
//     实现在 dx12_surface.cpp 的 Dx12Surface 系列 —— 因为 dx12_device.cpp 与
//     dx12_surface.cpp 互相调用（device 的 submitCommandList 写 gActiveSurface；
//     surface 的 createSurface/configureSurface 调用 deviceContext() 与
//     allocRtvHandle），拆成两个模块会形成 import 环。
//   * 异步基础设施（MainCommandExecutor / BundleRecorderPool /
//     PartitionedDescriptorAllocator）各自独立成模块；本模块**不** import 它们 ——
//     DeviceContext 里没有任何异步成员，异步对象是 dx12_device.cpp 里的文件级
//     静态量，只在实现单元里引用。若此处 import 就会与各 async .cpp（实现单元，
//     需要 deviceContext()）形成 import 环。
//   * Dx12Surface 定义在 gActiveSurface 之前，故无需为它写前向声明；BlitPipeline
//     仍保留前向声明（DeviceContext 持有 std::unique_ptr<BlitPipeline>）。

module;

#include "_d3d12_module_prelude.h"

// RTX（DLSS/NGX + NRD）可用性探测结果类型 mcdr::RtxSupport。接口单元必须自己
// 包含它：模块 purview 外的声明不会自动传给 import 方，而且这个头同时被实现单元
// （d3d12_device.cpp）和普通翻译单元（d3d12_rtx_probe.cpp）使用，所以保持为头文件
// 而不是再切一个模块。放全局模块片段里是为了避免 C5244。
#include "mcdr_rtx_support.h"

// mcdr_dlss_input（P108 的每帧参数包）就在这里定义。全局模块片段里 include 它不
// 会违反 C5244：本文件的 include 全部在 `module;` 与 `export module` 之间。
#include "mcdr_abi.h"

// ---------------------------------------------------------------------------
// P108：DLSS 实现单元（src\advanced\d3d12_dlss.cpp）的 C 接口。
//
// 这些声明必须出现在**全局模块片段**里（也就是 `module;` 与 `export module` 之间），
// 但裸写 extern "C" 声明会触发 C5202「全局模块片段仅可包含预处理器指令」——本项目
// /WX，告警即失败。所以和 mcdr_rtx_support.h 走同一条路：声明放进一个普通头文件，
// 在这里 #include 它。头文件本身就是预处理器指令，合法。
//
// 为什么是 extern "C"：普通 TU 与模块单元对同一个 C++ 函数会算出**不同的修饰名**
// （模块内实体带模块私有后缀 @$$_A...），只有 C 链接能让链接器把两边对上同一个符号。
// ---------------------------------------------------------------------------
#include "mcdr_dlss_impl.h"

export module mcdr.d3d12.device;

// 2026-10-14 现代化改造：模块内部失败上报从 `bool f(..., std::string& err)` 的 out 参数
// 约定改为 mcdr::Result<T>。mcdr.error 是叶子模块（只依赖标准库），import 它不形成环。
import mcdr.error;

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;

// P15: 日志分级 - ERROR/WARNING 始终输出；INFO/DEBUG 默认关闭，需 DX12_LOG_VERBOSE=1
enum class DbgLogLevel : int {
    ERR = 0,
    WARN = 1,
    INFO = 2,
    DEBUG = 3,
};

// 毫秒时间戳（QPC），供诊断插桩打印精确阻塞点（渲染线程卡死排查用）。
double nowMs();

// 统一的 native 日志前缀，与 JVM 侧 log4j 的输出同形，两路日志因此能按时间戳直接对齐：
//   JVM   : [1510月2026 14:27:45.008] [Render thread/INFO] [dev.nts.expdxr.Illusion/]: ...
//   native: [2026-10-15 15:19:42.123] [thread-12345/INFO] [dx12/]: ...
// level 用 log4j 的大写级别名（ERROR / WARN / INFO / DEBUG）；thread-NNN 是 Win32 线程 id
// —— native 拿不到 JVM 的线程名，显式写成 thread- 前缀以免与 logger 字段混淆。
// dbgLog* 自己会调它；其它 sink（[mcdr] last-error、abi_async 的 logFail）也用同一个，
// 保证 native 的每一行都是同一种形状。返回写入的字符数；<=0 或 >= outSize 表示放不下，
// 调用方应丢弃该行（不要写半行前缀）。
int formatLogPrefix(char* out, size_t outSize, const char* level, const char* logger);

// 错误/警告（始终输出）
void dbgLog(const char* fmt, ...);
// 错误级别（ERR=0 时仍然输出；比 dbgLog 更低的门槛，用于"这是错误"的语义）
void dbgLogError(const char* fmt, ...);
// 警告级别（WARN=1 及以上输出，门槛与 dbgLog 相同，仅用于区分"可继续的异常"）
void dbgLogWarn(const char* fmt, ...);
// 信息级别（默认关闭）
void dbgLogInfo(const char* fmt, ...);
// 调试级别（默认关闭）
void dbgLogDebug(const char* fmt, ...);
// 设置日志级别（0=ERR 1=WARN 2=INFO 3=DEBUG）
void setLogLevel(int level);

// PIX GPU 捕获层：请求在进程内载入 WinPixGpuCapturer.dll。必须在**首个 D3D12 设备创建
// 之前**调用（捕获层靠挂钩 D3D12 工作），幂等；返回 1=已载入 0=未请求 -1=请求了但没载入。
int setPixCapture(bool enabled);

// PIX 的 dll 在哪儿（配置项 [debug] pixDirectory，mcdrSetPixDirectory）：给 dll 全路径本身，或
// 它们所在的目录。空字符串 = 清掉、回到自动检测（MCDR.dll 同目录 → %ProgramFiles% 下最新版）。
// 要在 setPixCapture() 之前调用，载入时机在第一次 D3D12 设备创建之前。
// 返回 1=已设置 0=已清空 -1=路径无效（不是 UTF-8，或不存在）。
int setPixDirectory(const char* utf8Path);

// PIX 程序化 timing 捕获（mcdrStartPixTimingCapture / mcdrStopPixTimingCapture）。与 GPU 捕获层
// 不同，这两个是真的让 PIX 去录一段并把结果写成 .wpix —— 因为 PIX 界面上的持续捕获只把数据
// 留在 PIX 会话里，不 Save 就没有文件。路径是 UTF-8 的 .wpix 全路径，目录由调用方建好。
// start 返回 1=已开始 0=已经在录 负值=失败的 HRESULT（调用方直接把码显示给玩家；E_ACCESSDENIED
// 表示进程没提权 —— 本机实测 Performance Log Users 组不够。可读原因仍在日志里）；
// stop 返回 1=已请求 0=没有在录 负值=失败的 HRESULT。stop 不等写盘：PIX 在另一条线程上落盘。
int startPixTimingCapture(const char* utf8Path);
int stopPixTimingCapture();

// P97：VRS 着色粒度控制点。rate = 原始 D3D12_SHADING_RATE（1X1=0x0 1X2=0x1 2X1=0x4
// 2X2=0x5 2X4=0x6 4X2=0x9 4X4=0xA），-1 = 关闭（一个 RSSetShadingRate 都不调用，设备
// 保持默认 1X1）。取值只存进 DeviceContext，真正的落到命令列表由 applyShadingRate()
// 在每条图形命令列表 Reset 之后统一做（Reset 清列表状态，所以只能在那里应用）。
// 返回 1=接受（含"设备还没建，先收下"） 0=设备无法满足该档位（tier 0，或 2X4/4X2/4X4 缺
// AdditionalShadingRatesSupported —— 只有设备已存在时才答得出来） -1=未知速率值。
int setShadingRate(int rate);

// P104：抗锯齿模式（来自 Java 侧的视频设置选项 dxrMcAaMode，与 VRS 一样**不需要重启**）。
// mode 取值与 Kotlin 的 D3D12AntiAliasing 一致：**0=OFF 1=SMAA 2=TAA 3=DLAA**。
// P152 起 1 与 2 都真正实现了：1 = SMAA（applySmaaToTexture 就地三趟，形态学、单帧），
// 2 = TAA（applyTaaToTexture，时域累积，历史纹理乒乓由本模块持有）。3 = DLAA 仍由 DLSS
// 特性实现（JVM 侧把它映射成 OFF 再传下来，见 setDlssQuality 的档位 3），所以这里收到 3
// 也只是"无 native 后处理"。
//
// P108（用户 m10920）：**DLSS 已从这个枚举里拆出去**，改用独立开关 setDlssEnabled / 独立选项
// dxrMcDlss。抗锯齿与 DLSS 现在互相独立：两者都开时先对低分辨率世界层做 SMAA，再由 DLSS
// 升采样到窗口尺寸（见 applySmaaToTexture / applyDlssToTexture 的注释）。
// 返回 1=接受（该模式已生效） 0=该模式尚未实现（DLAA，等价于关） -1=非法取值。
int setAntiAliasing(int mode);
// 模式名（日志用）：OFF / SMAA / TAA / DLAA / UNKNOWN(n)。
const char* antiAliasingName(int mode);

// P108（用户 m10920）：DLSS 升采样的**独立开关**（Java 侧选项 dxrMcDlss，不再借用
// dxrMcAaMode）。默认关闭。与抗锯齿无关，两者可同时开；gate 见 applyDlssToTexture。
// 返回 1=开 0=关（含"本来就是该状态"）。幂等，设备还没建也能调（只记录状态）。
int setDlssEnabled(int enabled);

// P108：**深度快照**入口。JVM 在"世界深度还有效、vanilla 的 clearDepthTexture 之前"那一处
// （与录速度矢量同一挂点）调用它，把当时的深度拷进 DLSS 实现单元的私有 D32 纹理；之后
// applyDlssToTexture 里的 Evaluate 就用这张快照当深度输入，而不是已经清成 0.0 的活深度。
//
// 为什么需要：DLSS 的评估点在世界画完、GUI 之前，而 vanilla 在 GameRenderer.render 里已经
// 把深度清成 0.0（reverse-Z 的"远"）。深度项变成常量 ⇒ 深度感知升采样退化成纯色彩升采样。
//
// depth 的资源状态会被恢复成调用前的状态（本模块按 resourceState 跟踪），所以不会打乱 JVM
// 的世界渲染状态机。返回 1=已建立/复用快照 0=参数不可用 -1=创建或拷贝失败（已 WARN 一次）。
int snapshotDlssDepth(CommandContext* ctx, mcdr_object* depth);

// P108（用户 m11147 / m11796）：DLSS 挡位显式指定。档位**变化要触发特征重建**（即使尺寸没变），
// 所以这里除了改字段还要把实现单元的一次性失败闸门复位，让下一帧重建。
// tier：0=Performance 1=Balanced 2=Quality **3=DLAA**；越界返回 -1（不改变现状）。
// DLAA 复用同一条管线（深度快照 / MV / 输出 handover / 拷回），只是 PerfQuality 不同、且
// 要求 **render == target**（1:1，不放大）；若请求 3 而 render != target，实现单元会打 ERROR 并退回
// Quality 档，绝不静默生成"假 DLAA"。
// 已知限制：DLAA 的质量依赖投影 jitter，而 P75 的 jitter 路径目前冻结为 0（避免米白帧回归），
// 所以这版是"无 jitter 的时间累积 DLAA"——可用但非标准；P75 修好后自动受益，不要为它解冻 jitter。
// 返回归一化后的档位。
// 幂等；设备还没建也能调（只记录状态）。
int setDlssQuality(int tier);

// P108ac（用户 m08033）：DLSS 的**模型**（NGX render preset）。用户反馈"剩下的噪点闪烁是 DLSS
// 自身的问题，所有用 DLSS 的游戏都会出现，优化方法是更换 DLSS 的模型"。
// preset：0=Default（NGX 按挡位自选）、5=E / 6=F（已弃用 CNN）、10=J / 11=K / 12=L / 13=M
// （transformer）。预设是 CreateFeature 的输入 ⇒ 变化要触发特征重建，机制与挡位完全相同。
// 越界返回 -1（不改变现状）；返回记录的预设值。幂等；设备还没建也能调（只记录状态）。
int setDlssRenderPreset(int preset);

// P108（用户 m11147 后续）：登记 JVM 送来的 UAV 输出纹理。本函数负责**解包**（`Dx12Object`
// 的布局只有本模块知道），再把裸 `ID3D12Resource*` 交给 DLSS 实现单元 —— 实现单元绝不
// reinterpret JVM 句柄（那是空 vtable 崩溃的成因）。tex 为 nullptr 表示清除、退回实现单元
// 自建的私有兜底。返回 1=已接受 0=已清除 -1=句柄不是纹理资源。
int setDlssOutput(mcdr_object* tex);

// 高频调试宏：默认静默，DX12_LOG_VERBOSE=1 或编译时 DX12_DEBUG_VERBOSE 宏开启
// 注意：宏展开必须写全命名空间（d3d12_mc::）——否则在 namespace 之外使用该宏的 TU
// （例如 MCDR\src\c_abi\abi_command.cpp 的 mcdr_dx12CopyBuffer）在
// DX12_DEBUG_VERBOSE 打开时会报 C3861「dbgLogDebug: 找不到标识符」。
#ifdef DX12_DEBUG_VERBOSE
#define DBG_LOG_DEBUG(...) ::d3d12_mc::dbgLogDebug(__VA_ARGS__)
#else
#define DBG_LOG_DEBUG(...) ((void)0)
#endif

// P22: 前向声明（DeviceContext 使用 std::unique_ptr<BlitPipeline>，需完整类型）
struct BlitPipeline;
struct SharpenPipeline;

// ---------------------------------------------------------------------------
// 设备上下文（进程内单例，Java 侧首次调用 dx12CreateDevice 时创建）
// ---------------------------------------------------------------------------
struct DeviceContext {
    ComPtr<ID3D12Device> device;
    // P72：mesh shader 管线必须用 D3D12_PIPELINE_STATE_STREAM_DESC，而它只挂在
    // ID3D12Device2 上（D3D12_GRAPHICS_PIPELINE_STATE_DESC/CreateGraphicsPipelineState
    // 无法表达 AS/MS 阶段）。首次创建 mesh 管线时 QueryInterface 一次并缓存。
    ComPtr<ID3D12Device2> device2;

    ComPtr<ID3D12DescriptorHeap> srvHeap;      // SRV（SHADER_VISIBLE）
    // P6：SRV 的 CPU-only 镜像堆（非 SHADER_VISIBLE）。CopyDescriptorsSimple
    // 的源必须是 CPU-only 堆——texture view 的 cpuHandle 指向这里（供
    // pushDescriptors 复制到 drawHeap），srvHeap 的 GPU 句柄仍供直接绑定用。
    ComPtr<ID3D12DescriptorHeap> srvCpuHeap;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;      // RTV（持久：surface backbuffer RTV）
    // P24 fix：帧级瞬态 RTV 专用堆。beginRenderPass/clearColorTexture 每帧从
    // 0 号槽复用，避免覆盖 rtvHeap 上持久化的 backbuffer RTV——曾致 blit 把
    // 颜色纹理当 RT 绑定（资源状态 0xC0 不是 RENDER_TARGET）→ 全程黑屏。
    ComPtr<ID3D12DescriptorHeap> frameRtvHeap; // 帧级瞬态 RTV（每帧从 0 复用）
    ComPtr<ID3D12DescriptorHeap> dsvHeap;      // DSV
    ComPtr<ID3D12DescriptorHeap> samplerHeap;  // Sampler（SHADER_VISIBLE）
    ComPtr<ID3D12DescriptorHeap> drawHeap;     // P6：每帧瞬时 CBV/SRV 描述符（SHADER_VISIBLE，ring x2）

    // P33 async：开启异步分区后，drawHeap 的每个帧段被划出一部分给 worker 分区，
    // 同步 ring 只允许使用每段起始的 syncRingReserve 个槽位。0 = 未开启异步分区
    // （同步 ring 可用整段 kDrawHeapPerFrame）。由 dx12AsyncDescriptorCreate 设置。
    UINT syncRingReserve = 0;

    // P6：ExecuteIndirect 用 command signature（DrawIndexedInstanced / DrawInstanced）
    ComPtr<ID3D12CommandSignature> cmdSigIndexed;
    ComPtr<ID3D12CommandSignature> cmdSigNonIndexed;
    // P139：mesh 段派发的 indirect 变体（D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH，
    // ByteStride 12 = 三个 UINT 的线程组数）。AS 剔除要按 GPU 侧的计数派发，
    // 而 ExecuteIndirect 必须先有这条 command signature。
    ComPtr<ID3D12CommandSignature> cmdSigMesh;

    UINT srvInc = 0, rtvInc = 0, dsvInc = 0, samplerInc = 0;
    UINT drawInc = 0;  // == srvInc（同属 CBV_SRV_UAV 堆类型）


    std::string adapterName;
    ComPtr<IDXGIAdapter> adapter;  // 与 device 绑定的 DXGI adapter（用于 CreateDXGIFactoryForAdapter）
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;

    // P66：设备能力基线。设备创建后立即用 CheckFeatureSupport 填好（见
    // queryDeviceCapabilities），既写进适配器信息 JSON 供 Java 侧读取，也作为
    // mesh shader（ms_6_6/as_6_6）、DXR、VRS 等特性的门控依据。12_2 是这些特性
    // 的共同下限，所以 ensureDevice 现在只接受支持 12_2 的适配器。
    struct Capabilities {
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_2;  // 由 ensureDevice 覆写
        D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;  // 最高 SM；mesh shader 需 6_6+
        D3D12_MESH_SHADER_TIER meshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
        D3D12_SAMPLER_FEEDBACK_TIER samplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
        D3D12_RAYTRACING_TIER raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
        D3D12_VARIABLE_SHADING_RATE_TIER shadingRateTier = D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
        // P97：VRS 的另外两个事实。tier 2 = 支持着色率图像（屏幕空间 VRS），tile 大小决定
        // 那张图像的分辨率；additional = 额外（1x4/4x1 这类）速率是否可用。门控以
        // shadingRateTier 为准 —— 只有 tier >= 1 才能调 RSSetShadingRate。
        UINT shadingRateImageTileSize = 0;
        bool additionalShadingRates = false;
        D3D12_RESOURCE_BINDING_TIER resourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
        bool enhancedBarriers = false;
        bool queried = false;  // false = CheckFeatureSupport 未被调用/全部失败

        // P67：DXC（Shader Model 6.x / mesh shader 编译器）可用性。D3DCompile 只到
        // SM 5.1，mesh shader（ms_6_6/as_6_6）必须由 DXC 编译；设备初始化时会动态
        // 加载 dxcompiler.dll 并做一次 mesh shader 冒烟编译，作为「mesh shader 基础」
        // 的自检（真正的 mesh 管线还需要 AS/MS 的 D3D12_PIPELINE_STATE_STREAM_DESC，
        // 尚未实现）。
        bool dxcAvailable = false;        // dxcompiler.dll 已加载且能拿到 IDxcCompiler3
        bool meshShaderCompileOk = false;  // 冒烟编译（目标 ms_6_x）是否成功
        std::string dxcPath;    // 实际加载到的 dxcompiler.dll 完整路径
        std::string dxcProfile;  // 冒烟编译使用的 target，如 "ms_6_6"
        std::string dxcNote;    // 失败原因或 DXC 版本信息

        constexpr bool meshShadersSupported() const {
            return meshShaderTier >= D3D12_MESH_SHADER_TIER_1;
        }

        // P97：每 draw 速率需要 tier 1；屏幕空间 VRS（着色率图像）需要 tier 2。
        constexpr bool shadingRateSupported() const {
            return shadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_1;
        }
        constexpr bool shadingRateImageSupported() const {
            return shadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2;
        }
    };
    Capabilities caps;

    // RTX（DLSS / NRD）可用性探测结果。设备与适配器都就绪后填一次
    // （见 ensureDevice 里的 probeRtxSupport 调用），之后只读。
    // 单独放在这里而不是并进 Capabilities：Capabilities 是 D3D12 的
    // CheckFeatureSupport 基线，语义是「设备本身会什么」；这一项是「第三方
    // SDK 在这台机器上能不能用」，两者混在一起会让「能力查询失败」和
    // 「SDK 没装」看起来是同一件事。
    mcdr::RtxSupport rtx;

    // P97：VRS 着色粒度控制点。requestedShadingRate = -1 表示关闭（默认值）：设备保持
    // D3D12 默认的 1x1，整条录制路径不产生任何 RSSetShadingRate 调用。>= 0 时由
    // applyShadingRate() 在每条图形命令列表 Reset 之后应用 —— Reset 会清掉列表状态，
    // 所以只能在那里（每条列表）重放，不能在创建时设一次就算完。
    int requestedShadingRate = -1;
    UINT64 shadingRateAppliedLists = 0;  // 已应用过的命令列表数（首次应用打一行 INFO）
    // applyShadingRate() 判出"设备不支持这个档位"时只 WARN 一次（它会在每条列表上被调用）。
    bool shadingRateUnsupportedLogged = false;
    UINT64 timestampFrequency = 0;  // 从 GetTimestampFrequency 获取（DeviceInfo.timestampPeriod 用）
    ComPtr<ID3D12CommandQueue> queue;   // 图形队列（提交命令用）

    // 全局队列 fence（P6 fence token）：官方 createCommandEncoder() 返回共享
    // encoder，createFence() 的语义是"下一次提交完成后完成"。vanilla 在一次性
    // encoder 上创建 fence token（queueFencedTask / StagedVertexBuffer endFrame /
    // MappableRingBuffer rotate），该 encoder 从不 submit——若用 per-ctx fence 则
    // 永不完成 → 渲染线程永久等待（waitFence: value=1 completed=0 黑屏冻结）。
    // 故用设备级队列 fence 复现官方语义：每次 ExecuteCommandLists 后 Signal
    // (queueFence, ++queueFenceValue)，createFence 目标 = 当前值 + 1。
    ComPtr<ID3D12Fence> queueFence;
    HANDLE queueFenceEvent = nullptr;   // waitForQueueFenceValue 用（每调用新建 event，不共享）
    UINT64 queueFenceValue = 0;         // 每次 ExecuteCommandLists 后递增

    // 诊断（调试层）：设备移除时 GetDeviceRemovedReason + InfoQueue 消息定位根因
    ComPtr<ID3D12InfoQueue> infoQueue;

    // P22: blit 全屏四边形管线（懒初始化，device 级单例）
    std::unique_ptr<BlitPipeline> blitPipeline;
    std::unique_ptr<SharpenPipeline> sharpenPipeline;

    // -----------------------------------------------------------------------
    // P104：SMAA（形态学抗锯齿）状态。全部懒初始化：第一次应用且 aaMode==SMAA 时才建
    // 中间纹理与三条管线；源纹理尺寸变化（窗口 resize）时重建中间纹理。
    //
    // P104b：SMAA 只作用在**世界层**（GameRenderer 画完世界、还没画 GUI 时对主渲染目标
    // 就地做），不再插在最终 blit 上 —— 那样会把 GUI 一起柔化（用户报告的界面发虚）。
    // -----------------------------------------------------------------------
    int aaMode = 0;  // 0=OFF 1=SMAA 2=TAA 3=DLAA（与 Java 的 D3D12AntiAliasing 同序）
    // P108（用户 m10920）：DLSS 的独立开关（Java 侧 dxrMcDlss）。默认关。与 aaMode 无关 ——
    // 两者可同时开，那时先 SMAA（低分辨率世界层）再 DLSS 升采样。
    bool dlssEnabled = false;
    // P108（用户 m11147）：DLSS 挡位**显式指定**（Java 侧新按钮；0=Performance 1=Balanced
    // 2=Quality），不再只靠"按 render/target 比例就近选"。默认 **2** —— 本机 640x360→854x480
    // 按比例就近选到的正是 MaxQuality，所以默认值下行为与加这个字段之前完全一致。
    // -1 = JVM 从未设过 ⇒ 退回 chooseQuality 的比例就近逻辑（旧 DLL/旧 JVM 组合）。
    int dlssQualityTier = 2;
    // P108ac（用户 m08033）：DLSS 的**模型**（NGX render preset）。语义见 setDlssRenderPreset：
    // 0 = Default（NGX 按挡位自选，即历史行为），10 = J / 11 = K / 12 = L / 13 = M。
    int dlssPreset = 0;
    // P108（用户 m11147 后续）：JVM 送来的 **UAV 输出纹理**（它经模块 createTexture 建的）。
    // 非空时 DLSS 写它、而不是实现单元自建的私有纹理 —— 后者被控制组证明"承载不了写入"
    // （同源同调用点：模块建的目标读回 17.8/16.5/11.6/255，自建纹理全 0 且落定后仍 0）。
    // 只登记句柄、不持有引用；实现单元拿到的是本模块解包后的裸 ID3D12Resource*。
    mcdr_object* dlssOutputOverride = nullptr;
    // 中间纹理：edges（边检测输出，源分辨率，RG8）→ weights（混合权重，源分辨率，RG8）。
    ComPtr<ID3D12Resource> smaaEdges;
    ComPtr<ID3D12Resource> smaaWeights;
    D3D12_CPU_DESCRIPTOR_HANDLE smaaEdgesRtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE smaaWeightsRtv{};
    UINT64 smaaWidth = 0;
    UINT64 smaaHeight = 0;
    // P104 官方 LUT（AreaTex 160x560 R8G8_UNORM / SearchTex 64x16 R8_UNORM，字节来自
    // include\mcdr_smaa_luts.h）。权重趟用它们查表算混合权重；初始化失败时 smaaLutsReady
    // 保持 false，权重趟退回解析式的 smaaWeightPso（见 kSmaaWeightPS 的说明），不影响渲染。
    ComPtr<ID3D12Resource> smaaAreaTex;
    ComPtr<ID3D12Resource> smaaSearchTex;
    // 两张 LUT 的持久 SRV：自己的 2 槽 SHADER_VISIBLE 堆（不占每帧的 drawHeap 槽位）。
    ComPtr<ID3D12DescriptorHeap> smaaLutSrvHeap;
    D3D12_GPU_DESCRIPTOR_HANDLE smaaLutSrvGpu{};
    UINT smaaLutSrvGpuInc = 0;
    bool smaaLutsReady = false;
    // 「用的是官方 LUT」那行 INFO 的一次性闸门：applySmaaToTexture 每帧都会走到选路代码，
    // 没有它就会逐帧刷屏（实测刷了 15 秒）。与 smaaLutFallbackWarned 互斥。
    bool smaaLutReadyLogged = false;
    // 两个候选的权重 PSO：LUT 版（官方算法）与解析版（回退）。两者都在 smaaEnsurePipelines
    // 里一次建好，每帧按 smaaLutsReady 选一个，绝不重试。
    ComPtr<ID3D12PipelineState> smaaWeightLutPso;
    bool smaaLutFallbackWarned = false;  // 「LUT 不可用、退回解析式」只 WARN 一次
    // P104b：就地（in-place）路径的第三趟输出。同一资源不能既当 SRV 又当 RTV，所以
    // 「源 + weights → 源」必须落到一张同尺寸同格式的 scratch 上，再 CopyResource 拷回。
    ComPtr<ID3D12Resource> smaaWorldResult;
    D3D12_CPU_DESCRIPTOR_HANDLE smaaWorldResultRtv{};
    DXGI_FORMAT smaaWorldResultFormat = DXGI_FORMAT_UNKNOWN;
    // 管线：rootSig（**3** 张 SRV 表：根参数 0=t0 源色 / 1=t1 辅助纹理 / 2=t2 SearchTex，
    // 静态采样器 s0=smaaSampler / s1=smaaLutSampler）+ 四条 PS（共用 blit 的 VS）。
    // t2 是 LUT 权重趟专用；表数必须覆盖各趟着色器声明的全部寄存器，否则 PSO 创建 E_INVALIDARG。
    bool smaaPipelinesReady = false;
    bool smaaUnavailableLogged = false;  // 建管线失败只报一次，之后每帧静默跳过
    ComPtr<ID3D12RootSignature> smaaRootSig;
    ComPtr<ID3D12PipelineState> smaaEdgePso;
    ComPtr<ID3D12PipelineState> smaaWeightPso;
    ComPtr<ID3D12PipelineState> smaaBlendPso;
    UINT64 smaaAppliedFrames = 0;  // 已应用帧数（首帧打一行 INFO 便于实机确认）
    // SMAA 自己的 3 槽 RTV 堆（NONE-visibility）。不用 allocRtvHandle：那是持久单调分配，
    // 窗口 resize 重建中间纹理时每轮会泄漏槽位。
    ComPtr<ID3D12DescriptorHeap> smaaRtvHeap;
    // 混合阶段的目标格式（blit 路径 = back buffer 格式；就地路径 = 主渲染目标格式）。
    // PSO 的 RTVFormats[0] 必须与之相等；格式变化时三条管线一起重建。
    DXGI_FORMAT smaaBlendFormat = DXGI_FORMAT_UNKNOWN;
    // 一次致命失败（描述符堆耗尽等）后彻底关闭 SMAA：避免每帧重试刷日志，也避免把
    // 中间纹理留在与预期不符的状态上继续用。
    bool smaaDisabled = false;

    // -----------------------------------------------------------------------
    // P152：TAA 的跨帧状态。
    // 历史纹理由**本模块自己持有**（JVM 不再建 2560x1334 的 RGBA16F 乒乓）：按主目标尺寸
    // 懒创建，尺寸变化时整组重建。两张乒乓 —— read 是上一帧的输出，write 是本帧的渲染目标。
    // -----------------------------------------------------------------------
    ComPtr<ID3D12Resource> taaHistory[2];
    D3D12_CPU_DESCRIPTOR_HANDLE taaHistoryRtv[2]{};
    UINT64 taaWidth = 0;
    UINT64 taaHeight = 0;
    // 两张历史纹理的 RTV 堆（2 槽、NONE-visibility）。不用 allocRtvHandle 的理由同 SMAA：
    // 那是持久单调分配，resize 重建中间纹理时每轮都会泄漏槽位。
    ComPtr<ID3D12DescriptorHeap> taaRtvHeap;
    // resolve 管线：根参数 0 = 12 个 32-bit 参数（10 个有效 + 2 个对齐 pad），根参数 1 =
    // t1..t3 三张 SRV；PS 全用 Load + 手算双线性，所以没有采样器。
    ComPtr<ID3D12RootSignature> taaRootSig;
    ComPtr<ID3D12PipelineState> taaPso;
    int taaReadIndex = 0;
    int taaWriteIndex = 1;
    bool taaHistoryValid = false;  // false ⇒ 本帧 resolve 以 reset=1 开始（历史不可信）
    bool taaDisabled = false;      // 一次致命失败后永久关闭，避免每帧重试刷日志
    bool taaUnavailableLogged = false;  // 建管线失败 / 目标不适用只报一次
    bool taaSharpenFallbackWarned = false;  // 锐化趟失败退回纯 blit 只 WARN 一次
    UINT64 taaAppliedFrames = 0;            // 已应用帧数（首帧打一行 INFO 便于实机确认）
};

// ---------------------------------------------------------------------------
// 资源对象（Java long 句柄 = Dx12Object*）
// ---------------------------------------------------------------------------
struct Dx12Object {
    enum class Kind { Texture, Buffer, Sampler, TextureView };

    Kind kind = Kind::Buffer;
    ComPtr<ID3D12Resource> resource;  // Texture / Buffer
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle{};  // View / Sampler 的 CPU 句柄
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};  // View / Sampler 的 GPU 句柄
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;  // Texture 的格式（copy 计算行距用）
    void* mappedPtr = nullptr;  // map 后的指针
    int usage = 0;
    long long size = 0;
    // P6：描述符堆槽位索引（TextureView → SRV 槽位；Sampler → sampler 槽位；
    // -1 = 未占用）。销毁时归还 free-list 复用，防止长会话 SRV 堆耗尽。
    int descSlot = -1;
    // TextureView 指向的底层 Texture（采样前需 transition 到
    // PIXEL_SHADER_RESOURCE；Texture/Buffer 恒为 nullptr）。
    Dx12Object* sourceTexture = nullptr;
};

// ---------------------------------------------------------------------------
// 设备生命周期
// ---------------------------------------------------------------------------
// 创建/复用全局设备上下文；失败时返回 mcdr::Error。
// P66：设备必须建在支持 D3D_FEATURE_LEVEL_12_2 的硬件适配器上（按 GPU 偏好顺序
// 挑选，不再是 D3D12CreateDevice(nullptr, ...) 的默认适配器——混合显卡机器上默认
// 适配器常是核显，既拿不到 12_2，也会让 CreateSwapChainForHwnd 全部 E_ACCESSDENIED）。
mcdr::Result<void> ensureDevice();
void destroyDevice();

// P66：D3D_FEATURE_LEVEL → "12_2" 短名；未知值返回 "unknown"。
const char* featureLevelName(D3D_FEATURE_LEVEL level);
// P66：本工程允许的最高着色器模型 = 6.10。SDK 10.0.26100 的 d3d12.h 只定义到
// D3D_SHADER_MODEL_6_9，6_10 用数值补齐（0x6a）。真正能用多高取驱动上报值与这个
// 上限的较小者（见 effectiveShaderModel）；另外 DXC 也必须够新——SDK 自带的
// DXIL/DXC 1.8 只认到 6_8，6_9/6_10 会直接报 "invalid profile"。
constexpr D3D_SHADER_MODEL kShaderModelCeiling = static_cast<D3D_SHADER_MODEL>(0x6a);
// "6.8" / "6.10"（日志与 JSON 用）与 "6_8" / "6_10"（DXIL target 后缀用）。
const char* shaderModelName(D3D_SHADER_MODEL model);
const char* shaderModelProfileSuffix(D3D_SHADER_MODEL model);
// 驱动上报的最高 SM 与 kShaderModelCeiling 取小；着色器编译/PSO 目标用它。
D3D_SHADER_MODEL effectiveShaderModel(const DeviceContext::Capabilities& caps);
// P66：查一遍 12_2 相关能力（SM / mesh shader tier / DXR / VRS / enhanced barriers）。
void queryDeviceCapabilities(ID3D12Device* device, DeviceContext::Capabilities& caps);
// P66：能力摘要（一行日志用）与 JSON 片段（无前导逗号，供适配器信息 JSON 拼接）。
std::string capabilitiesText(const DeviceContext::Capabilities& caps);
std::string capabilitiesJson(const DeviceContext::Capabilities& caps);

// P67：探测 DXC 编译器（运行时动态加载，绝不静态链接 dxcompiler.lib），并做一次
// mesh shader 冒烟编译，结果写回 caps 的 dxc* 字段。dxcompiler.dll 搜索顺序：
// 环境变量 MCDR_DXC_DIR → MCDR.dll 同目录（将来随模组分发 DXC 时放这里）→
// Windows SDK 最新的 bin\<ver>\x64。整段探测失败不致命，只记录原因。
void probeDxc(DeviceContext::Capabilities& caps);

// P72：运行期 HLSL → DXIL 编译入口（DXC，进程内缓存 dxcompiler.dll 与接口）。
// mesh shader（as_6_x / ms_6_x）只能由 DXC 产出，Java 侧把源码 + 入口点 + profile
// 交给这里，拿回 DXIL 字节码再喂给 createMeshPipeline。失败时 Error 带 DXC 诊断首行。
mcdr::Result<void> compileHlslToDxil(const std::string& source, const std::string& entry,
                                    const std::string& profile, std::vector<uint8_t>& outDxil);

// JNI 层访问设备上下文（读 adapterName/featureLevel）。
DeviceContext& deviceContext();




// P33 fix：flush 延迟删除对象，在 destroySurface 等 present fence 后调用，
// 确保 GPU 工作完成后再释放资源。
// P46：默认只释放 queue fence 已确认完成的条目（不阻塞）；force=true 时
// 忽略 fence 门槛全部释放（仅用于进程退出等 GPU 已空闲的场景）。
void flushPendingDeletes(bool force = false);

// ---------------------------------------------------------------------------
// P33 async：把瞬时描述符直接写到 drawHeap 的「绝对槽位」（供 worker 线程在
// 自己的分区区域内写 CBV / 纹理 SRV / texel buffer SRV）。
//
// 与 pushDescriptors 的区别：不做 transition（bundle 内禁止 ResourceBarrier），
// 调用方须先用 transitionBufferTo / transitionTextureTo 在主列表上完成过渡；
// 槽位由 PartitionedDescriptorAllocator 分配，保证与其他 worker 互不重叠。
// ---------------------------------------------------------------------------
// buffer[offset, offset+size) 写为 CBV（size 向上取整 256 对齐）。
mcdr::Result<void> writeCBVToDrawHeap(const UINT& slot, const Dx12Object*& buf, const long long& offset,
                                     const long long& size);
// 复制纹理视图已有的 SRV 描述符到 slot。
mcdr::Result<void> writeTextureSRVToDrawHeap(const UINT& slot, const Dx12Object*& view);
// buffer[offset, offset+length) 写为 texel buffer SRV（texelFormat = GpuFormat ordinal）。
mcdr::Result<void> writeBufferSRVToDrawHeap(UINT slot, Dx12Object* buf, long long offset, long long length,
                                           int texelFormat);

// 返回设备/队列的原始 COM 指针（用于 Java 侧持有句柄）。
uintptr_t getDeviceHandle();
uintptr_t getQueueHandle();

// 创建一个隐藏的 Win32 窗口（用于测试渲染循环）；返回 HWND (uintptr_t)。
uintptr_t createHiddenWindow(int width, int height);
// 销毁隐藏的 Win32 窗口。
void destroyHiddenWindow(uintptr_t hwnd);

// ---------------------------------------------------------------------------
// 资源创建（返回 new 的 Dx12Object*，句柄 = 指针；失败返回 mcdr::Error）
// ---------------------------------------------------------------------------
// 句柄一律以 void* 返回（而非 Dx12Object* 等模块内类型）：MSVC 19.51 无法为
// std::expected<模块内类型*, mcdr::Error> 在模块边界外产生一致的实例化符号，
// 会以 LNK2001 报未解析（消费方 TU 侧的名字带 ::<!mcdr.d3d12.device> 归属后缀）。
// 返回 void* 后符号一致，C ABI 侧本来就把句柄当不透明指针用。
mcdr::Result<void*> createTexture(int usage, int format, int width, int height, int depthOrLayers,
                                  int mipLevels);
mcdr::Result<void*> createBuffer(int usage, long long size);
mcdr::Result<void*> createSampler(int addressU, int addressV, int minFilter, int magFilter,
                                  int maxAnisotropy, float maxLod);
mcdr::Result<void*> createTextureView(Dx12Object* texture, int baseMipLevel, int mipLevels);
void destroyObject(Dx12Object* obj);

// ---------------------------------------------------------------------------
// Buffer 映射
// ---------------------------------------------------------------------------
// 对应官方 GpuBuffer.map(offset, length, read, write)：校验 usage 位后
// ID3D12Resource::Map，返回连续指针（供 NewDirectByteBuffer 使用）。
mcdr::Result<void*> mapBuffer(Dx12Object* buffer, long long offset, long long length, bool read, bool write);
void unmapBuffer(Dx12Object* buffer);

// ---------------------------------------------------------------------------
// 命令层（P3）：CommandContext 对应官方 VulkanCommandEncoder。
// 官方用 timeline semaphore + 双 command pool（MAX_SUBMITS_IN_FLIGHT=2）；
// D3D12 用 ID3D12Fence（timeline 语义）+ 三 command allocator 一一对应：
//   signalSemaphore(idx)      -> queue->Signal(fence, idx)
//   awaitSubmitCompletion(idx)-> fence->GetCompletedValue()/SetEventOnCompletion
//   currentCommandPool().reset-> allocators[fenceValue % 3]->Reset()
// 三 ring：帧 N 用 index (N-1)%3，帧 N+3 复用同一 index 时 submit 已等 N 完成。
// （双 ring + 等 value-2 会差一帧：帧 N+2 复用帧 N 的 allocator 但只等了 N-1。）
// ---------------------------------------------------------------------------
// Dx12Pipeline 的完整定义提前到这里：CommandContext / BundleRecorder 都按值或
// 按名引用它，且不应各自前向声明（模块内前向声明会得到另一个模块归属的类型，
// 链接期报 LNK2001）。原本它在 PipelineDesc 之前，现前移到 CommandContext 之前。
struct Dx12Pipeline {
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> withDepth;  // 总是创建；DSV=D32_FLOAT（镜像官方 depthAttachmentFormat=126）
    ComPtr<ID3D12PipelineState> withoutDepth;  // 仅 depthState==null 时创建；DSV=UNKNOWN
    int topology = 4;  // MC PrimitiveTopology ordinal（setPipeline 时 IASetPrimitiveTopology 用）
    // P6：per-slot 修正 stride（由 inputElements 实际格式推算，覆盖 Java 侧 getVertexSize() 的错误值）
    std::unordered_map<int, UINT> vertexStrides;
    // P72：mesh 管线（AS/MS）标记。mesh PSO 没有 IA 阶段，setPipeline 不再设置
    // IASetPrimitiveTopology（IA 状态对 mesh 管线无效，且 PSO 里本就不含输入布局）。
    bool isMesh = false;
    // P137：mesh 尾参在根签名里的参数索引（建根签名时按 CBV 稠密编号 + 地形 SRV 表
    // 的实际排版算出来，再经 createMeshPipeline 抄进来）。PushDescriptors 只设到地形
    // SRV 表（索引 = CBV 个数），b8 段表与 t14/t15/t16 表由 drawMeshSegments 补。
    // 0 表示该 PSO 没有 mesh 尾参。
    UINT segmentParam = 0;
    UINT geometryParam = 0;
    // P140：meshlet 剔除。cullParam != 0 表示根签名里有 b9 root 常量（视锥 + 本批基址/组数）；
    // hasAs 表示 PSO 真的带 amplification shader —— 此时派发的线程组数是 ceil(组数/128)，
    // 由 AS 再压缩成实际的 mesh 组数。
    UINT cullParam = 0;
    bool hasAs = false;
};


// P64：帧级瞬态 RTV/DSV 缓存键。(资源指针, mip) 唯一决定视图内容（见 makeRtvDesc）。
struct RtvDsvKey {
    ID3D12Resource* resource;
    UINT mip;
    bool operator==(const RtvDsvKey& o) const { return resource == o.resource && mip == o.mip; }
};
struct RtvDsvKeyHash {
    size_t operator()(const RtvDsvKey& k) const {
        // 黄金比例常数按 size_t 取：x64 下与 0x9E3779B97F4A7C15ULL 等价，x86 下让乘法
        // 直接落在 32 位域内，避免 C4244（size_t 只有 32 位时 64 位常量会被截断）。
        return std::hash<void*>()(reinterpret_cast<void*>(k.resource))
               ^ (std::hash<UINT>()(k.mip) * static_cast<size_t>(0x9E3779B97F4A7C15ULL));
    }
};

struct CommandContext {
    ComPtr<ID3D12CommandAllocator> allocators[3];   // 三帧飞行，等待 value-2 后复用
    // P55：command list 也改为 3 路 ring（此前只有单条 list，导致主线程必须等上一帧
    // GPU 执行完才能 Reset 同一条 list → CPU 录制与 GPU 执行被完全串行化，
    // 实测 GPU 仅 33%、CPU 仅 29% 却只跑到 ~680 FPS）。现在 list 与 allocator 一一
    // 对应同一槽位，主线程录制帧 N 时用的是与帧 N-3 相同的槽位，只需等该槽位的 fence。
    ComPtr<ID3D12GraphicsCommandList> commandLists[3];
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;              // 最近一次 Signal 的值（从 1 开始递增）
    HANDLE fenceEvent = nullptr;        // SetEventOnCompletion 用
    int listOpen = 0;                   // command list 是否已 begin
    int inRenderPass = 0;               // 渲染 pass 是否打开

    // P55：本帧使用的 ring 槽位（begin 时按 fenceValue%3 固定，submit 时据此记录 fence）。
    UINT currentListSlot = 0;
    // P55：各槽位最后一次提交的 per-list fence 值。begin 时等本槽位的值，
    // 保证该槽位的 list/allocator 不再被 GPU 引用后再 Reset。
    UINT64 slotFence[3] = {0, 0, 0};
    // P55：指向 commandLists[currentListSlot] 的裸指针，供全部录制调用点直接使用
    // （保持原有 `ctx->commandList->Foo()` 写法不变）。由 beginCommandListWithWait 指向当前槽位。
    ID3D12GraphicsCommandList* commandList = nullptr;

    // P11：当前渲染 pass 的活跃附件。endRenderPass 必须把它们从
    // RENDER_TARGET/DEPTH_WRITE 显式回切 COMMON——这两种状态属"非可提升状态"，
    // 命令列表执行完成时【不会】隐式 decay 回 COMMON（只有 COPY_SOURCE/
    // COPY_DEST/UAV 等可提升状态才 decay）。若省略回切，下一 command list 的
    // barrier 按 COMMON 写 Before 状态会与资源实际状态错配（每帧验证 ERROR →
    // GPU 状态错乱 → TDR 冻结：游戏挂死，启动器同 GPU 渲染也卡死）。
    std::vector<Dx12Object*> activeColorTargets;
    // P17：标记每个 color target 在本 command list 中是否被 draw 写入过
    std::vector<bool> activeColorTargetsTouched;
    // P17：整个 command list 是否有过任何 draw 写入（endRenderPass 后仍有效，供 blitSurface 判断）
    bool colorTargetsWritten = false;
    Dx12Object* activeDepthTarget = nullptr;

    // P6：本帧 drawHeap 瞬时描述符分配（ring：fenceValue%2 交替两个半区；
    // 帧 N+2 重写帧 N 半区时 submit 已等 N 完成，故 x2 足够）
    UINT drawHeapSlotBase = 0;
    UINT nextDrawSlot = 0;

    // P33 async：记录上一次 submitCommandList 写入的 queue fence 值。
    // beginCommandListWithWait 据此等待 GPU 完成上一帧后再 Reset allocator，
    // 避免异步线程并发提交时的 use-after-free（ACCESS_VIOLATION 根因）。
    // 初始值 0 表示"无需等待"（首帧或 createBuffer 一次性路径）。
    UINT64 lastSubmitQueueFence = 0;

    // P40：记录上一帧 submit 时写入的 per-command-list fence 值。
    // beginCommandListWithWait 等待此 fence 完成，确保 GPU 已实际执行完
    // 引用该 allocator 的所有命令，再 Reset allocator——这是避免 hang 的关键。
    // 与 lastSubmitQueueFence 的区别：queue fence 仅表示命令已入队，
    // per-command-list fence 表示 GPU 已完成执行。
    UINT64 prevFenceValue = 0;

    // 本 command list 内已过渡的资源状态（资源指针 -> 当前 D3D12 状态）。
    // 初始态 = 资源创建时的状态（texture=COMMON，buffer=initialStateFor）。
    // beginCommandList 清空：因为 submit 同步等待完成，上一 command list
    // 执行结束后所有提升状态已隐式 decay 回 COMMON，故每个新 list 一切资源
    // 都从初始态开始。绝不在 list 内显式回退 COMMON（D3D12 禁止）。
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> resourceState;
    // 本 command list 内被 transition 到 COPY_SOURCE 的 UPLOAD heap staging buffer。
    // endCommandList 必须将它们回切 GENERIC_READ（而非 COMMON），因为下一帧
    // beginCommandList 以 GENERIC_READ 为初始态假设，GPU 实际状态必须匹配。
    std::unordered_set<ID3D12Resource*> stagingCopySourceResources;

    // P109（#538 根因修复）：本 ctx 的帧级 RTV/DSV 句柄缓存属于描述符堆的哪一代（见
    // d3d12_device.cpp 的 gFrameViewGeneration / UINT gNextFrameRtv 的说明）。代不一致 ⇒
    // 缓存里的句柄可能已被别的 ctx 覆盖成另一张资源的视图，必须整体重建。
    unsigned int frameViewGeneration = 0;

    
    // P6：当前绑定的管线，用于 setVertexBuffer 查找修正 stride
    Dx12Pipeline* currentPipeline = nullptr;

    // P64：setPipeline 快速路径。同一命令列表内以相同 PSO 重复调用时，root signature /
    // PSO / topology 均未变，可跳过 3 次驱动调用。ExecuteBundle 后置空（bundle 内设置的
    // PSO 不回滚到父列表，父列表状态可能已变）。
    ID3D12PipelineState* cachedPso = nullptr;

    // P64：帧级瞬态 RTV/DSV 描述符缓存。beginRenderPass/clear* 在本命令列表内对同一
    // (资源, mip) 只创建一次视图；此前每次调用都 CreateRenderTargetView/
    // CreateDepthStencilView（数百次/帧的驱动调用）。beginCommandList 清空
    // （gNextFrameRtv/gNextDsv 同点清零，句柄随之失效）。
    std::unordered_map<RtvDsvKey, D3D12_CPU_DESCRIPTOR_HANDLE, RtvDsvKeyHash> rtvCache;
    std::unordered_map<RtvDsvKey, D3D12_CPU_DESCRIPTOR_HANDLE, RtvDsvKeyHash> dsvCache;

    ComPtr<ID3D12CommandAllocator>& currentAllocator() { return allocators[currentListSlot]; }
};

// 创建命令上下文：3 个 allocator + command list + fence + event。
mcdr::Result<void*> createCommandEncoder();
void destroyCommandEncoder(CommandContext* ctx);

// 开始录制：先等待 GPU 完成 waitForValue 对应的提交（若 waitForValue > 0），
// 再 Reset 当前 allocator + list。用于异步多线程场景，确保 allocator 不被
// 还在使用的 GPU 命令列表引用时覆写。waitForValue == 0 时跳过等待（兼容原有
// 单线程路径及 createBuffer 一次性路径）。
mcdr::Result<void> beginCommandList(CommandContext* ctx);
mcdr::Result<void> beginCommandListWithWait(CommandContext* ctx, UINT64 waitForValue);
// 结束录制：Close list（之后可提交）。
mcdr::Result<void> endCommandList(CommandContext* ctx);
// 提交：ExecuteCommandLists + Signal(fence, ++fenceValue)。
// 非阻塞（GPU 异步执行）；fence 信号值同时写入当前 active surface 的 per-backbuffer 记录。
// 返回本次提交的 fence value。
mcdr::Result<UINT64> submitCommandList(CommandContext* ctx);
// 等待 fence 值达到 value；timeoutNs 为纳秒。返回是否完成。
mcdr::Result<void> waitForFenceValue(CommandContext* ctx, UINT64 value, UINT64 timeoutNs);
// 当前 fence value（Java 侧 createFence 记录用）。
UINT64 currentFenceValue(CommandContext* ctx);
// 等待**该 ctx 自己**的 per-command-list fence 达到 value（Kotlin GpuFence.awaitCompletion
// 的落点，BUG-03）。token 只由这个 ctx 的提交推进，与其它 encoder 解耦；timeoutNs 为纳秒，
// 超时返回 err("… timed out after …ns")，由调用方决定是否当作失败。
mcdr::Result<void> waitForContextFenceValue(CommandContext* ctx, UINT64 value, UINT64 timeoutNs);

// ---------------------------------------------------------------------------
// P33 async：独立 C++ 渲染线程（渲染工作从主线程剥离）
// ---------------------------------------------------------------------------
//
// 架构：独立的 C++ 渲染线程负责 GPU 同步（acquire/present/fence wait），
// 主线程只负责 push 绘制命令，不阻塞等 GPU。
//
// 四阶段状态机（通过 Windows events 协调）：
//   1. BEGIN_FRAME      — 主线程发信号，请求开始新帧
//   2. RECORDING_READY  — 渲染线程发信号，告知 GPU 已就绪，可以 push 命令
//   3. COMMANDS_READY   — 主线程发信号，所有绘制命令已入队
//   4. SUBMIT_DONE      — 渲染线程发信号，命令已提交，回到步骤 1
// ---------------------------------------------------------------------------
bool initAsyncRenderer(UINT workerCount);
void destroyAsyncRenderer();
// 等待渲染线程完成当前帧（不销毁线程，仅同步到 gEvtSubmitDone 触发后）
void waitForRenderThreadSubmit();
// P48（多帧飞行）：等待所有已提交帧完成 Present。acquireSurface 在真正调用
// GetCurrentBackBufferIndex 之前必须调用它——Present 未完成时 index 不会轮转，
// 提前 acquire 会拿到同一个 back buffer（两帧写同一处 → 闪帧/撕裂）。
// 正常情况（主线程录制耗时 ≫ 渲染线程 Present 耗时）返回 0ms。
//
// 返回 true = 已同步（此时 acquire 是安全的，index 在本次 acquire 期间不会轮转）；
// false = 超过 timeoutMs 仍未同步。**超时绝不能放行**：Present 一旦在 acquire 之后
// 完成，back buffer index 就轮转，本帧的 blit 会写到已经翻转走的 buffer 上，D3D12
// 直接以 DXGI_ERROR_ACCESS_DENIED 移除设备。实测（GLFW_NO_API 冷启动第一帧）：
// 渲染线程还在 initBlitPipeline，主线程 100ms 超时放行 → D3D12 ERROR #907
// "A command list, which writes to a swapchain back buffer, may only be executed when
// that back buffer is the current back buffer" → RemoveDevice → 0x887A0005 崩溃。
bool waitForPendingPresents(unsigned timeoutMs = 100);

// 诊断：把 ID3D12Device::GetDeviceRemovedReason() 与 InfoQueue 最近若干条消息打进日志。
// 设备移除（Present 返回 0x887A0005 等）时调用——没有这一行的话，未开调试层时日志里
// 只剩 "presentSurface: FAILED DXGI_ERROR_DEVICE_REMOVED"，看不到真正原因
// （实测漏掉的是上面那条 ERROR #907 / DXGI_ERROR_ACCESS_DENIED）。
void logDeviceStatus(const char* tag);
// 非阻塞：请求渲染线程开始下一帧。返回 true 表示成功排队，false 表示正在处理上一帧。
mcdr::Result<void> asyncRenderBeginFrame(CommandContext* ctx);
// 阻塞：等到本帧提交完成（或超时 ms）。
mcdr::Result<void> asyncRenderWaitComplete(CommandContext* ctx, UINT64 timeoutMs);
// 非阻塞：查询渲染线程是否已到达 "recording ready" 阶段。
bool asyncRenderIsRecordingReady(CommandContext* ctx);
// 阻塞：等待渲染线程到达 "recording ready" 阶段（超时 ms）。
// 取代 Java 侧 Thread.sleep(1) 轮询：消除每帧 ~1ms 的调度器休眠开销。
bool asyncRenderWaitRecordingReady(CommandContext* ctx, UINT64 timeoutMs);
// 主线程通知渲染线程：所有命令已入队（设置 gEvtCommandsReady）。
void asyncSendCommandsReady(CommandContext* ctx);
// 主动清除 gAsyncRenderCtx（供 Java 侧超时降级时同步调用）。
void clearAsyncRenderCtx();
// 查询命令列表是否已打开。
bool isListOpen(CommandContext* ctx);

// P33 async：当前帧使用的 drawHeap 段号（= fenceValue % kDrawHeapSections）。
// 与 beginCommandListWithWait 中 drawHeapSlotBase 的段号一致；Java 侧据此把
// worker 的 (frameSlot, worker) 描述符区域对齐到同一段，避免与同步 ring 重叠。
UINT currentDrawFrameSlot(CommandContext* ctx);

// ---------------------------------------------------------------------------
// 全局队列 fence（P6 fence token；对应官方共享 encoder 的 submit index）
// ---------------------------------------------------------------------------
// 当前全局队列 fence 值（Java 侧 createFence 记录用：目标 = 当前值 + 1，
// 下一次任意 ctx 的提交完成后达成——官方语义"共享 encoder 的下一次 submit"）。
UINT64 currentQueueFenceValue();
// 等待全局队列 fence 达到 value（createFence token 的 awaitCompletion 用；
// 等待对象是设备级 queueFence，而非 per-ctx fence）。
mcdr::Result<void> waitForQueueFenceValue(UINT64 value, UINT64 timeoutNs);

// 立即读 GPU 时间戳（等价官方 getTimestampNow：临时 query + 提交 + 读回）。
mcdr::Result<long long> getTimestampNow(CommandContext* ctx);

// ---------------------------------------------------------------------------
// 命令录制（P3）：copy / clear / render pass / timestamp
// ---------------------------------------------------------------------------
// buffer -> buffer 拷贝（CopyBufferRegion）。
mcdr::Result<void> copyBufferToBuffer(CommandContext* ctx, Dx12Object* src, long long srcOffset, Dx12Object* dst,
                                     long long dstOffset, long long size);

// 状态追踪的纹理过渡：把 tex 过渡到 to（以本 command list 已跟踪状态为准，
// 绝不回退 COMMON——decay 由命令列表完成时隐式处理）。blitSurface 也要用它
// 把渲染后的源纹理过渡到 COPY_SOURCE。
void transitionTextureTo(CommandContext* ctx, Dx12Object* tex, D3D12_RESOURCE_STATES to);

// 状态追踪的 buffer 过渡（同上，初始锚点 = initialStateFor(heapType)）。
// 异步路径在主列表上为 bundle 批量预过渡 CBV/SRV 资源时复用。
void transitionBufferTo(CommandContext* ctx, Dx12Object* buf, D3D12_RESOURCE_STATES to);

// 底层过渡：直接接受 ID3D12Resource*，用于 swapchain back buffer 等
// 不在 Dx12Object 包装中的资源。blitSurface 用它跟踪 back buffer 状态。
void transitionTo(CommandContext* ctx, ID3D12Resource* res, D3D12_RESOURCE_STATES fromInitial,
                  D3D12_RESOURCE_STATES to);

// clear 颜色纹理（整纹理，RENDER_ATTACHMENT）。
mcdr::Result<void> clearColorTexture(CommandContext* ctx, Dx12Object* tex, float r, float g, float b, float a);
// clear 深度纹理（整纹理）。
mcdr::Result<void> clearDepthTexture(CommandContext* ctx, Dx12Object* tex, double depth);

// clear 颜色纹理矩形区域（P3b fix：GuiItemAtlas 槽位 STALE 重绘前只清该槽位。
// 原简化实现忽略 region 整图清空，滚动时会把其它槽位已烘好的物品图标一并抹掉，
// 翻页回来后这些区域不再触发重绘 → 物品消失）。
mcdr::Result<void> clearColorTextureRegion(CommandContext* ctx, Dx12Object* tex, float r, float g, float b,
                                          float a, int x, int y, int w, int h);
// clear 深度纹理矩形区域（同上）。
mcdr::Result<void> clearDepthTextureRegion(CommandContext* ctx, Dx12Object* tex, double depth, int x, int y,
                                          int w, int h);
// clear stencil 位（MC 26.2 新增的 CommandEncoderBackend::clearStencilTexture）。
// clearDepthTexture 一直隐式把 stencil 清成 0；这里只清 STENCIL 位并写入调用方
// 指定的 value，深度位保持不动（ClearDepthStencilView 只带 STENCIL flag 时
// depth 参数被忽略）。
mcdr::Result<void> clearStencilTexture(CommandContext* ctx, Dx12Object* tex, int value);

// ---------------------------------------------------------------------------
// 纹理拷贝（P3）：CopyTextureRegion 系列
// ---------------------------------------------------------------------------
// staging/通用 buffer -> texture 子区域（writeToTexture / copyBufferToTexture）。
// srcWidth/srcHeight 为源 buffer 的行宽/行高（texel），用于 footprint 计算；
// w/h 为实际拷贝尺寸。
mcdr::Result<void> copyBufferToTexture(CommandContext* ctx, Dx12Object* srcBuf, long long srcOffset,
                                      int srcWidth, int srcHeight, Dx12Object* dstTex, int mip, int layer,
                                      int dstX, int dstY, int w, int h);

// texture 子区域 -> readback buffer（copyTextureToBuffer）。
mcdr::Result<void> copyTextureToBuffer(CommandContext* ctx, Dx12Object* srcTex, int mip, int layer, int srcX,
                                      int srcY, int w, int h, Dx12Object* dstBuf, long long dstOffset);

// texture -> texture 子区域（copyTextureToTexture）。
mcdr::Result<void> copyTextureToTexture(CommandContext* ctx, Dx12Object* srcTex, Dx12Object* dstTex, int mip,
                                       int layer, int srcX, int srcY, int dstX, int dstY, int w, int h);

// 设备 timestamp 频率（GetTimestampFrequency；0 = 未知/未初始化）。
unsigned long long getTimestampFrequency();

// 渲染 pass 生命周期（P3：OMSetRenderTargets + viewport/scissor + clear；
// draw 命令录制依赖 P4 pipeline）。
//   colorViews      ：颜色纹理数组（允许空指针占位，对应 withUnusedColorAttachment）
//   colorMips       ：每附件要绑定的 mip slice（长度 colorCount；可为 null=0。
//                     官方 TextureAtlas 对 mipViews[level] 逐级发起 render pass 写
//                     各 mip —— 原 RTV 恒绑 mip0 使 1+ 级图集内容从未写入：远处/大
//                     LOD 采样到未初始化 mip → 黑色方块、4px 模糊块等）
//   colorClearFlags ：0=保留(Load)，1=clear（对应 clearValue 存在）
//   clearColors     ：每附件 4 个 float（r,g,b,a），长度 colorCount*4
//   depthView       ：深度纹理（可为空）
//   depthMip        ：深度附件绑定的 mip slice（默认 0）
//   depthClearFlag  ：0=Load，1=clear
//   depthClearValue ：深度清除值
//   x/y/w/h         ：renderArea
mcdr::Result<void> beginRenderPass(CommandContext* ctx, Dx12Object* const* colorViews, int colorCount,
                                  const int* colorMips, const int* colorClearFlags, const float* clearColors,
                                  Dx12Object* depthView, int depthMip, int depthClearFlag, double depthClearValue,
                                  int x, int y, int w, int h);
mcdr::Result<void> endRenderPass(CommandContext* ctx);

// ---------------------------------------------------------------------------
// Timestamp query pool（P3）
// ---------------------------------------------------------------------------
struct QueryPool {
    ComPtr<ID3D12QueryHeap> heap;
    int size = 0;
};

mcdr::Result<void*> createQueryPool(int size);
void destroyQueryPool(QueryPool* pool);
// 录制 EndQuery(TIMESTAMP) 到 pool[index]。
mcdr::Result<void> writeTimestampToPool(CommandContext* ctx, QueryPool* pool, int index);
// 阻塞读回单个/多个 timestamp（等待 GPU 完成）。
mcdr::Result<void> readQueryValue(QueryPool* pool, int index, long long& out);
mcdr::Result<void> readQueryValues(QueryPool* pool, int start, int count, long long* out);

// ---------------------------------------------------------------------------
// 格式映射（官方 GpuFormat 枚举值 -> DXGI_FORMAT）
// ---------------------------------------------------------------------------
DXGI_FORMAT toDxgiFormat(int gpuFormat);
// 顶点输入布局专用：RGB32_* 用精确的三分量格式（R32G32B32_*，DXGI 合法
// 输入格式；纹理用的 toDxgiFormat 会把 RGB32 加宽成 RGBA32，输入布局不能加宽）。
DXGI_FORMAT toDxgiVertexFormat(int gpuFormat);

// ---------------------------------------------------------------------------
// 图形管线（P4）：D3DCompile(vs_5_1/ps_5_1) + root signature + 双 PSO
// ---------------------------------------------------------------------------
// Java 侧 dx12CreateGraphicsPipeline 的 desc 解析产物（字段语义见 Dx12Native Javadoc）。
struct PipelineDesc {
    struct ColorTarget {
        int format = 0;  // GpuFormat ordinal；-1 = 未使用槽位
        uint8_t writeMask = 0;
        bool blendEnabled = false;
        uint8_t srcColor = 0, dstColor = 0, colorOp = 0;
        uint8_t srcAlpha = 0, dstAlpha = 0, alphaOp = 0;
    };
    struct InputElement {
        int location = 0, binding = 0, format = 0;
        int offset = 0, stride = 0, stepRate = 0;
        std::string semanticName;  // HLSL 声明的语义名（如 "POSITION"、"TEXCOORD0"）；空串则回退到 "TEXCOORD"
    };
    struct Binding {
        uint8_t type = 0;  // 0=CBV，1=SRV+static sampler，2=SRV
        uint8_t reg = 0;
    };

    std::vector<uint8_t> vsBytes;  // HLSL 源码（vs_5_1 编译）
    std::vector<uint8_t> psBytes;  // HLSL 源码（ps_5_1 编译）
    int colorCount = 0;
    std::vector<ColorTarget> colorTargets;
    bool hasDepth = false;
    int depthFormat = 0;
    bool depthWrite = false;
    int depthCompareOp = 0;
    int topology = 0;  // PrimitiveTopology ordinal
    bool cullEnabled = false;
    int polygonMode = 0;  // PolygonMode ordinal（0=FILL, 1=WIREFRAME）
    std::vector<InputElement> inputElements;
    std::vector<Binding> bindings;  // 按 shader 声明顺序（register = 序号）
};

// 创建管线（D3DCompile + root signature + 双 PSO）；失败返回 mcdr::Error。
mcdr::Result<void*> createGraphicsPipeline(const PipelineDesc& desc);
void destroyPipeline(Dx12Pipeline* pipeline);

// ---------------------------------------------------------------------------
// P22: Blit 管线（全屏四边形绘制，绕过 CopyTextureRegion 格式不兼容问题）
// 源纹理（R32G32B32A32_FLOAT 等）→ backbuffer（R8G8B8A8_UNORM），由 GPU
// 光栅器自动做格式转换，无需手动 resolve。
// ---------------------------------------------------------------------------
struct BlitPipeline {
    ComPtr<ID3D12RootSignature> rootSig;  // SRV(t0) + sampler(s0)
    ComPtr<ID3D12PipelineState> pso;  // noDepth, noBlend, noCull
    ComPtr<ID3D12Resource> vertBuf;  // 4 vertices, R32G32 + R32G32
    ComPtr<ID3D12Resource> idxBuf;  // 6 indices, R16_UINT
    D3D12_VERTEX_BUFFER_VIEW vbView{};
    D3D12_INDEX_BUFFER_VIEW ibView{};
};

// 懒初始化 blit 管线（线程安全：第一次调用创建，后续直接返回）。
mcdr::Result<void> initBlitPipeline();
const BlitPipeline* getBlitPipeline();

// P121: sharpen pipeline (TAA / DLAA / DLSS post pass). Reuses the blit VS + vertex buffer,
// adds one 32-bit constant block (rcpSize.xy + amount).
struct SharpenPipeline {
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> vertBuf;
    ComPtr<ID3D12Resource> idxBuf;
    D3D12_VERTEX_BUFFER_VIEW vbView{};
    D3D12_INDEX_BUFFER_VIEW ibView{};
};
mcdr::Result<void> initSharpenPipeline();
const SharpenPipeline* getSharpenPipeline();
// P22: 在 srvHeap 为 srcTex 分配 SRV 槽位并设置根描述符表（srvGpu 已绑定）。
// 调用方无需关心 srvHeap 内部细节。
mcdr::Result<void> blitBindSourceTexture(CommandContext* ctx, Dx12Object* srcTex, ID3D12GraphicsCommandList* cmd);

// P104b：SMAA。对一张已渲染完成的纹理**就地**应用三个全屏阶段（边检测 → 混合权重 →
// 邻域混合 + 拷回）。调用点在世界层画完、GUI 还没画的位置（GameRenderer.render 里
// GuiRenderer.render() 之前），所以只有世界层被抗锯齿，界面保持锐利。
// 返回 true = 已应用；未启用（aaMode != SMAA）/ 目标不适用 / 中途失败返回 false。
//
// P104/P108：**只在 aaMode == 1 时工作**（0/2/3 一律跳过）。DLSS 已从抗锯齿枚举里拆成独立
// 开关（用户 m10920），所以本函数**不再**和 DLSS 互斥：两者都开时，这三趟先跑在低分辨率
// 世界层上，然后由 applyDlssToTexture 把它升采样到窗口尺寸。
bool applySmaaToTexture(CommandContext* ctx, Dx12Object* tex);

// P152：TAA（时域抗锯齿）—— 与 applySmaaToTexture 同一个调用点（世界层画完、GUI 之前），
// 区别是状态在**本模块**里跨帧保留（历史纹理乒乓 + jitter），JVM 每帧只送参数。
//
// 两趟：① scene(t1) + history[read](t2) + motion(t3) → history[write]（RGBA16F，在 YCoCg
// 空间做 variance clipping，混合权重恒为 1-blend）；② history[write] 回填进 scene（锐化量
// > 0 时走 sharpen PSO，否则纯采样 blit —— 历史是 16F、主目标是 8 位，不能 CopyResource）。
//
// jitter 是**投影矩阵里已经加进去的**亚像素偏移（像素单位，与 D3D12Temporal 同约定），本函数
// 不改矩阵：它把本帧颜色重采样回未抖动网格，再按运动矢量把历史取回来。reset != 0（首帧 /
// 传送 / 改分辨率 / 历史代变化）时本帧直接输出原始样本、不参与混合。
//
// 返回 1 = 已应用；0 = 跳过（参数不适用 / 场景格式不是 RGBA8 / 已被一次性闸门关掉）；
// -1 = 失败。任何失败都只报一次并永久关闭，调用方保持既有画面即可。
int applyTaaToTexture(CommandContext* ctx, Dx12Object* scene, Dx12Object* motion, float jitterX,
                      float jitterY, float prevJitterX, float prevJitterY, float blend, int reset,
                      float sharpenAmount);

// P108：DLSS 的跨帧上采样入口（真实 NVSDK_NGX 评价调用）。
//
// 与 applySmaaToTexture **同一个调用点**（世界画完、GUI 之前），但目标不同：SMAA 是
// 就地抗锯齿，DLSS 是把 render 尺寸的世界层升采样进 target 尺寸的目标纹理，因此它还
// 顺带取代了 P106 那次线性 blit。
//
// 输入全部来自 in（每帧参数包 mcdr_dlss_input，字段与来源见 mcdr_abi.h 的注释）。
// 成功返回后：in.pInColor 停在 PIXEL_SHADER_RESOURCE、in.pInOutput 停在 RENDER_TARGET，
// GUI 的 render pass 可以接着把 pInOutput 当附件用。
//
// 任何一步失败（NGX 缺失 / Init / 建特征 / Evaluate）都只 WARN 或 ERROR 一次并永久关掉
// DLSS，返回 false —— 调用方保持既有路径（线性 blit），绝不重试、绝不黑屏。
// 静默跳过的条件：独立开关关闭（!gCtx.dlssEnabled，见 setDlssEnabled）或参数不全。
bool applyDlssToTexture(CommandContext* ctx, const mcdr_dlss_input& in);

// P108：DLSS 可用性与状态的单行查询（UI / 日志用）。输出的一行文字写进 out
// （NUL 结尾；out 可为 nullptr）。返回 1=可用 0=不可用 -1=参数非法。
// 副作用：首次查询会做一次 NGX init（成败都只做一次），所以它同时是"探测"入口。
int queryDlss(char* out, int cap);

// ---------------------------------------------------------------------------
// Draw 命令录制（P6）：渲染 pass 内的 draw 全链路。
// 对应官方 VulkanRenderPass：setPipeline / bindTexture / setUniform /
// enableScissor / setVertexBuffer / setIndexBuffer / drawIndexed / draw /
// multiDraw* / drawIndexedIndirect / drawIndirect。
// ---------------------------------------------------------------------------
// 绑定图形管线（hasDepth 决定用 withDepth / withoutDepth PSO）。
mcdr::Result<void> setPipeline(CommandContext* ctx, Dx12Pipeline* pipeline, bool hasDepth);
// RSSetScissorRects(1)（x,y,w,h）。
mcdr::Result<void> setScissor(CommandContext* ctx, int x, int y, int w, int h);
// RSSetViewports(1)（x,y,w,h；depth 恒 0..1）。MC 26.2 把 viewport 提升为
// RenderPassBackend 上的独立抽象方法 setViewport(int,int,int,int)，原先只有
// beginRenderPass 会设置视口，因此这里补一个等价入口。
mcdr::Result<void> setViewport(CommandContext* ctx, int x, int y, int w, int h);
// IASetVertexBuffers：slot 0..15；stride 来自管线的 vertex format。
mcdr::Result<void> setVertexBuffer(CommandContext* ctx, int slot, Dx12Object* buffer, long long offset,
                                   int stride);
// IASetIndexBuffer（indexType：0=SHORT(R16_UINT)，1=INT(R32_UINT)）。
mcdr::Result<void> setIndexBuffer(CommandContext* ctx, Dx12Object* buffer, int indexType);
// 瞬时描述符绑定（对应官方 pushDescriptors）：
//   type 0 = CBV（buffer + offset + length，offset 须 256 对齐）
//   type 1 = SRV（复制 texture view 的现有描述符）
//   type 2 = SRV（texel buffer，按 texelFormat 建 Buffer SRV）
struct DrawBinding {
    uint8_t type = 0;
    Dx12Object* buffer = nullptr;  // CBV / TEXEL 的 buffer
    long long offset = 0;  // CBV offset
    long long length = 0;  // CBV length（内部向上取整 256）
    int texelFormat = 0;  // TEXEL SRV 的 GpuFormat ordinal
    Dx12Object* view = nullptr;  // SAMPLED_IMAGE 的 texture view
};
// B（拆根签名）：CBV 走 root descriptor（SetGraphicsRootConstantBufferView，不占堆
// 槽位），SRV 写入本帧 drawHeap 瞬时槽位并 SetGraphicsRootDescriptorTable(cbvCount)。
mcdr::Result<void> pushDescriptors(CommandContext* ctx, const std::vector<DrawBinding>& bindings);

mcdr::Result<void> drawIndexedInstanced(CommandContext* ctx, UINT indexCount, UINT instanceCount,
                                        INT startIndexLocation, INT baseVertexLocation,
                                        UINT startInstanceLocation);
mcdr::Result<void> drawInstanced(CommandContext* ctx, UINT vertexCount, UINT instanceCount, UINT firstVertex,
                                 UINT startInstanceLocation);
// ExecuteIndirect（DrawIndexedInstanced / DrawInstanced command signature）。
mcdr::Result<void> drawIndexedIndirect(CommandContext* ctx, Dx12Object* commands, long long offset,
                                       UINT drawCount);
mcdr::Result<void> drawIndirect(CommandContext* ctx, Dx12Object* commands, long long offset, UINT drawCount);

// P137：mesh 段派发。sections = 96 B/段（t16）、segments = 32 B/组（b8，≤1024 组/批）、
// vertexBuffer/indexBuffer 走 t14/t15 的 RAW SRV（R32_TYPELESS + BUFFER_SRV_FLAG_RAW，
// 供 ByteAddressBuffer 读取）。普通绑定仍由 pushDescriptors 负责，本函数只补 tail 参数。
mcdr::Result<void> drawMeshSegments(CommandContext* ctx, Dx12Object* vertexBuffer, long long vertexOffset,
                                    Dx12Object* indexBuffer, long long indexOffset, Dx12Object* sections,
                                    long long sectionsOffset, long long sectionsLength, Dx12Object* segments,
                                    long long segmentsOffset, int groupCount);

// P140：带 meshlet 剔除的段派发。meshletBounds = 16 B/组的包围球表（t17，与段表同序，
// 索引是"全局组号"）；frustumPlanes = 6 个 float4 视空间平面（内侧 dot(n,c)+d >= 0），
// 只用前 cullPlanes 个。已绑定的 PSO 必须 hasAs（AS 才会按压缩后的数量派发 mesh 组）。
// 段表之外的四个缓冲都可以为 null（null = 该 PSO 没有对应尾参）。
mcdr::Result<void> drawMeshSegmentsCulled(CommandContext* ctx, Dx12Object* vertexBuffer, long long vertexOffset,
                                          Dx12Object* indexBuffer, long long indexOffset, Dx12Object* sections,
                                          long long sectionsOffset, long long sectionsLength, Dx12Object* segments,
                                          long long segmentsOffset, Dx12Object* meshletBounds,
                                          long long meshletBoundsOffset, const float* frustumPlanes, int cullPlanes,
                                          int groupCount);

// P139：同一个段派发的 indirect 版本。args 里每条 12 字节（DISPATCH_MESH 的
// ThreadGroupCountX/Y/Z），maxCommandCount 条命令都读同一个 b8 段表基址 ⇒ 调用方
// 每次只覆盖一批（≤1024 组），大于一批时按批多次调用。语义与 drawMeshSegments 一致，
// 只是线程组数改由 GPU 侧缓冲给出（AS 剔除后要按 GPU 计数派发就走这条）。
mcdr::Result<void> drawMeshSegmentsIndirect(CommandContext* ctx, Dx12Object* vertexBuffer, long long vertexOffset,
                                            Dx12Object* indexBuffer, long long indexOffset, Dx12Object* sections,
                                            long long sectionsOffset, long long sectionsLength, Dx12Object* segments,
                                            long long segmentsOffset, Dx12Object* args, long long argsOffset,
                                            int maxCommandCount);

// P141：用 compute 管线为每个 meshlet 组生成对象空间包围球（16 B：center.xyz + radius），
// 写进 bounds（必须是用 usage 位 512 建的 UAV 缓冲）。pipeline 是 createMeshBoundsPipeline
// 交出的句柄（= Dx12Pipeline*，withDepth 承载 CS PSO）。groupCount 与段表的组数一致：
// 第 i 组读 meshSegments[i]，结果写 bounds[boundsOffset + i*16]。
// 结束后 bounds 处于 NON_PIXEL_SHADER_RESOURCE，可直接作为 AS 的 t17 读取。
mcdr::Result<void> buildMeshletBounds(CommandContext* ctx, Dx12Pipeline* pipeline, Dx12Object* vertexBuffer,
                                      long long vertexOffset, Dx12Object* indexBuffer, long long indexOffset,
                                      Dx12Object* segments, long long segmentsOffset, Dx12Object* bounds,
                                      long long boundsOffset, int groupCount);

// ---------------------------------------------------------------------------
// Surface（P5）：DXGI swapchain（镜像官方 VulkanGpuSurface）。
// PresentMode 序数 = 官方枚举 ordinal：IMMEDIATE=0, MAILBOX=1, FIFO=2,
// FIFO_RELAXED=3。DXGI FLIP 模型支持 {0, 2, 3}（MAILBOX 无直接对应）。
// ---------------------------------------------------------------------------
constexpr UINT kSurfaceBufferCount = 3;  // 镜像官方 minImageCount max(3, min)

struct Dx12Surface {
    ComPtr<IDXGISwapChain3> swapChain;
    uintptr_t hwnd = 0;  // 重建 swapchain 时需要；ResizeBuffers 失败时销毁重建用
    // RTSS/OSD 修复：GLFW 主窗口一旦用 GL 呈现过画面，DXGI 就永久拒绝对该 HWND
    // 创建 flip-model swapchain（CreateSwapChainForHwnd → E_ACCESSDENIED
    // 0x80070005，实测见 _port\scprobe2.cpp）。因此呈现改用本 surface 自建的子
    // 窗口（无 GL、类样式不含 CS_OWNDC），它同时让 OSD 注入器（RTSS/Afterburner）
    // 能从 GetDesc1().OutputWindow 拿到窗口句柄——composition swapchain 的
    // OutputWindow 为空，RTSS 因此无法定位覆盖层。为 nullptr 时走旧行为（直接
    // 用 hwnd，或 composition 回退）。
    HWND presentWindow = nullptr;
    UINT width = 1;
    UINT height = 1;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    int presentMode = 2;  // 默认 FIFO
    int currentImageIndex = -1;  // 最近 acquire 的 back buffer
    // P6 诊断：最近一次 blit 写入的 back buffer 下标。present 后 currentImageIndex
    // 被重置为 -1（防 ResizeBuffers 误判），而 readback 需要读"本帧已 blit 的
    // buffer"才能看到真实画面——用 GetCurrentBackBufferIndex() 兜底会读到下一帧
    // 尚未写入的 buffer（全 0 假黑屏）。blit 成功时记录此处。
    int lastBlitIndex = -1;
    // P95（叠帧/闪烁）：本帧是否真的把内容写进过当前 back buffer。presentSurface 据此
    // 决定是否 Present —— vanilla 的 blit 门/present 门只看包装层的 hasBlittedTexture /
    // isAcquired，与我们后端是否真的 blit 成功脱钩；P48 推迟 acquire 失败时会静默跳过
    // blit，而 vanilla 仍会 present，于是 flip 到一张本帧从未写过的 buffer（启动期是
    // 未初始化内存，稳态是 2~3 帧前的旧画面）→「上一帧和这一帧叠在一起」，挂 Present
    // 的 OSD 也会同屏出现新旧两遍文字。
    // 不变式：acquireSurface 成功置 false → blitSurface 成功置 true → presentSurface 后置 false。
    // P95b：改为原子量——写方是主线程（blitSurface）、读方是渲染线程（presentSurface），
    // 普通 bool 在这里是数据竞争（异步呈现路径）。
    std::atomic<bool> blittedThisFrame{false};
    // P95b（叠帧 + OSD 重叠）：本 surface 是否真的持有"已 acquire 但尚未 present"的
    // back buffer。acquireSurface 成功置 true，presentSurface 末尾置 false；configureSurface
    // 只有在 exchange(false) 返回 true 时才用 Present(0,0) 释放它。
    // 旧实现用 currentImageIndex >= 0 判断：该值由渲染线程在 present 末尾写 -1，主线程读
    // 属于数据竞争，可能读到过期值 → 对一张**已经 present 过**的 buffer 再 Present 一次：
    //   1) flip 链多推一帧 → 上一帧再次上屏（"上一帧和这一帧叠在一起"）；
    //   2) 挂在 Present 上的 OSD 注入器（RTSS/Afterburner）对同一张 buffer 再画一遍 OSD
    //      （错位双份）——这正是用户给的判据"OSD 重叠"。
    // 该 bug 只在重配置瞬间（进入世界、启动加载界面切换）触发，与用户报告的时刻一致。
    std::atomic<bool> bufferHeld{false};
    bool suboptimal = false;
    // 临时 OSD / resize 诊断字段（debugReadbackIn、debugDumpAfterMs、debugDumpDone、
    // debugDumpCreateTick）已于 P70 清理（known_bugs.md §5）。需要回读画面时用 ABI 的
    // mcdr_dx12ReadbackSurfacePixels 手动触发，不要再往每帧路径里塞诊断代码。
    // P65：实际创建的 swapchain 是否带 DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING（tier1 成功）。
    // 仅当为 true 时才能在 Present 时传 DXGI_PRESENT_ALLOW_TEARING，否则 Present
    // 返回 DXGI_ERROR_INVALID_CALL。窗口/全屏切换可能重建 swapchain，故每次创建后更新。
    // P70：ResizeBuffers 的 flags 也必须与之保持一致——曾无条件传 ALLOW_TEARING，
    // 当成功 tier 不带 tearing 时每次 resize 都返回 INVALID_CALL。
    bool allowTearing = false;
    // P33 fallback：CreateSwapChainForHwnd 被拒（E_ACCESSDENIED 0x80070005，见
    // Issue #11）时改用 CreateSwapChainForComposition + DirectComposition 呈现。
    // compositionMode=true 时 swapChain 由 CreateSwapChainForComposition 创建，
    // 画面经 dcompVisual 合成到窗口；此时不支持 ALLOW_TEARING。
    // 声明顺序保证析构时先释放 visual/target/device 再释放 swapChain。
    bool compositionMode = false;
    ComPtr<IDCompositionDevice> dcompDevice;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> dcompVisual;
    std::vector<ComPtr<ID3D12Resource>> backBuffers;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;  // 由 rtvHeap 分配
    // P18：per-backbuffer fence 值。submitCommandList 记录本帧写入的 fence 值，
    // acquireSurface 据此判断重用的 back buffer 是否仍被 GPU 使用。
    std::vector<UINT64> surfaceFences;
    // P33 fix：per-surface present fence。渲染线程每次 presentSurface() 后递增并
    // signal 此 fence，确保 Display Controller 完成 flip 后再销毁 surface。
    // IMMEDIATE 模式 + FLIP_DISCARD 下 deviceWaitIdle 不等待 display controller，
    // 此 fence 填补该空档，避免 backbuffer 被 DWM/显示器读取时释放导致 CORRUPTION。
    std::vector<UINT64> surfacePresentFences;
};

// P18：当前 active surface（单例，渲染线程访问）。submit 时写入 per-backbuffer fence。
extern Dx12Surface* gActiveSurface;
inline void setActiveSurface(Dx12Surface* s) { gActiveSurface = s; }
inline Dx12Surface* getActiveSurface() { return gActiveSurface; }
// P33：async 渲染线程 handle（供 destroySurface 在关闭渲染线程后等待 GPU 空闲使用）
extern std::thread gRenderThread;
// P33：渲染线程运行标志（供 destroySurface 判断是否需要等待线程退出）
extern bool gRenderRunning;
// P33：当前正在处理的 ctx（nullptr = 无活动帧），供诊断读回函数检测 ASYNC 管道活跃。
extern CommandContext* gAsyncRenderCtx;
// P33：本帧的 Present 是否已由渲染线程执行（true = Java 侧 present() 必须跳过，
// 否则会重复 Present：一个渲染完成的帧 + 一个未渲染的 back buffer 交替显示，
// 表现为窗口闪烁各种颜色）。由渲染线程 present 后置位，Java present() 消费后清位。
extern bool gAsyncOwnsPresent;

// 从 rtvHeap 分配一个 RTV CPU 句柄（surface 的 back buffer 用）。
mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> allocRtvHandle();

// 创建 swapchain（FLIP_DISCARD + ALLOW_TEARING，1x1 占位，configure 时 ResizeBuffers）。
mcdr::Result<void*> createSurface(uintptr_t hwnd);
// 取该 surface 支持的 present modes（官方枚举序数数组，MAILBOX 不支持）。
std::vector<int> surfacePresentModes();
// ResizeBuffers + 重新取 back buffers + 重建 RTV。
mcdr::Result<void> configureSurface(Dx12Surface* s, int width, int height, int presentMode);
// 获取当前 back buffer index（DXGI FLIP 模型下 Present 内部同步）。
mcdr::Result<void> acquireSurface(Dx12Surface* s);
// 录制 blit（源纹理 -> back buffer）：在 command list 录制状态下调用；
// 源与目标格式须一致（RGBA8/BGRA8 族可直接拷贝）。返回 Error 表示未录制。
mcdr::Result<void> blitSurface(CommandContext* ctx, Dx12Surface* s, Dx12Object* srcTex);
// P106：把一张纹理画到另一张纹理上（整屏三角形 + 线性采样，允许两边尺寸不同）。
// GUI 的独立全尺寸目标需要把世界层结果从缩放后的主渲染目标搬到窗口尺寸的 GUI 目标，
// 缩放比不是 1，所以不能走 1:1 的 copyTextureToTexture。
// 结束状态：源回 RENDER_TARGET，目标停在 RENDER_TARGET（GUI pass 直接接着往上画）。
mcdr::Result<void> blitTextureToTexture(CommandContext* ctx, Dx12Object* src, Dx12Object* dst);
// P121: src -> dst 5-tap unsharp. amount <= 0 -> caller should use blitTextureToTexture.
mcdr::Result<void> sharpenTextureToTexture(CommandContext* ctx, Dx12Object* src, Dx12Object* dst,
                                          float amount);
// P106：取（或创建）纹理的帧级瞬态 RTV —— blitTextureToTexture 要把目标当渲染目标用。
mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> acquireTextureRtv(CommandContext* ctx, Dx12Object* tex);
// Present(interval, flags)：FIFO=Present(1,0)；IMMEDIATE=Present(0,0)；
// FIFO_RELAXED=Present(1,ALLOW_TEARING)。
void presentSurface(Dx12Surface* s);
void destroySurface(Dx12Surface* s);
// 仅清理表面，不调用 deviceWaitIdle（供 C++ 内部调用，在 render thread 已停止后使用）
void destroySurfaceNoWaitIdle(Dx12Surface* s);
// P6 诊断：把当前 back buffer 读回 CPU 并打印 3x3 采样点 RGBA（每 ~60 帧调用
// 一次，内部先等 GPU 空闲；用于确认画面实际颜色/内容——纯色=渲染未生效）。
mcdr::Result<void> readbackSurfacePixels(Dx12Surface* s);
// 返回当前 acquire 的 back buffer 原始 ID3D12Resource 指针（uintptr_t），
// 供 Java 侧直接用于 blit 源或 readback（避免使用缓存的旧 lastColorTextureHandle）。
uintptr_t getBackBufferHandle(Dx12Surface* s);
// 返回当前 command context 的渲染 pass 中第一个活跃颜色附件的纹理句柄；
// 若不在渲染 pass 中则返回 0。用于替代缓存的 lastColorTextureHandle，
// 避免 resize 后句柄指向旧纹理的问题。
uintptr_t getActiveColorTextureHandle(CommandContext* ctx);
// P6 诊断：读回任意纹理 3x3 采样点 RGBA（tag 标注来源，如 blit 源=渲染目标，
// 用于区分 draw 未写入 vs blit 丢失）。
void dbgReadbackTexturePixels(Dx12Object* tex, const char* tag);
// P6 诊断：把 RGBA8 像素数据保存为 BMP 文件（32bpp，覆盖写当前工作目录
// dx12_dump_<tag>.bmp）并在日志打印 ASCII 缩略图（亮度字符 + 色相字母）。
// 用于把画面完整可视化，定位 GUI 元素绘制目标是否与 blit 源一致。
void dbgDumpPixelsToFile(const uint8_t* rgba, UINT w, UINT h, UINT64 pitch, const char* tag);
// P6 诊断：读回 buffer 内容（前若干 float + hex），UPLOAD 直接 Map、DEFAULT 经
// readback staging；用于确认顶点/UBO 数据是否真正写入 GPU 资源。
void dbgReadbackBufferBytes(Dx12Object* buf, long long offset, int len, const char* tag);

// 阻塞等待 GPU 队列上所有已提交命令执行完成（销毁 swapchain/设备前调用，
// 避免 backbuffer 等资源在被 GPU 使用时释放导致 DXGI_ERROR_DEVICE_REMOVED）。
mcdr::Result<void> deviceWaitIdle();

// 自检：创建 texture/buffer/sampler/view 各一，验证资源层可用后销毁。
// 返回描述字符串（成功/失败明细）。
std::string runResourceSelfTest();

// 枚举所有 D3D12 适配器，返回 JSON 数组字符串：
// [{"name":"...","luid":0x...,"vid":0x...,"did":0x...,"vram_gb":N,...},...]
std::string enumerateAdaptersJson();

// 活跃设备所绑定那块适配器的完整描述，返回单行扁平 JSON 对象（键与含义见
// mcdr_abi.h 里 mcdr_dx12GetAdapterInfo 的说明）。没有活跃设备时返回空串。
// 驱动版本 / 驱动描述 / 硬件·集显·可拆卸分类来自运行期动态加载的 DXCore，
// DXCore 不可用时这些字段回落为 ""/false，其余字段全部来自 DXGI。
std::string adapterInfoJson();
}  // namespace d3d12_mc

