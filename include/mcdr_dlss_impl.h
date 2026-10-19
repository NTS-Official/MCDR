// mcdr_dlss_impl.h — P108：DLSS 实现单元（d3d12_dlss.cpp）与设备模块之间的唯一接口。
//
// 为什么是一个头文件而不是在 .cppm 里直接声明：
//   * 实现单元是普通 TU，只有它 #include nvsdk_ngx*.h；设备模块因此永远看不到 NGX
//     类型，DLSS 的许可面收在一个文件内。
//   * 设备模块必须在自己的全局模块片段里看到这些声明（在 `module;` 与 `export module`
//     之间包含本头文件）。全局模块片段里写裸的 extern "C" 声明会触发 C5202
//     「全局模块片段仅可包含预处理器指令」（本项目 /WX，直接构建失败），而头文件本身
//     是预处理器指令 include 进来的，与 mcdr_rtx_support.h 用的是同一条路。
//   * 子集必须是 extern "C"：普通 TU 与模块单元对同一个 C++ 函数会算出不同的修饰名
//     （模块内实体带模块私有后缀），只有 C 链接能让两边对上同一个符号。
//
// 也是唯一允许跨模块边界的东西：签名里只有 void*、POD 与 char* 缓冲。
// **不要**把 std::string 放进这里（两个 TU 各自看到的 std::string 不是同一个类型）。
#ifndef MCDR_DLSS_IMPL_H
#define MCDR_DLSS_IMPL_H

#include "mcdr_abi.h"  // mcdr_dlss_input

#ifdef __cplusplus
extern "C" {
#endif

// 注入设备模块的日志出口。两个 TU 各自看到的 dbgLogInfo 是**不同的修饰名**，不能跨
// 边界直接调用；只有从模块那一侧取地址传过来，指针才指向真正那条实现。
// 必须在任何其它入口之前调用一次（设备就绪时做）。
void mcdr_dlss_install_log(void* logInfo, void* logWarn, void* logError);

// 把 RTX 探测得到的 nvngx_dlss.dll 完整路径告诉 DLSS 实现（Init 失败时那是最有用的一条
// 诊断）。path 不被持有，调用内即复制；cap 含结尾 NUL。
void mcdr_dlss_register_runtime_path(const char* path, int32_t cap);

// 已登记的运行期路径（可能为空串；永不返回 NULL）。
const char* mcdr_dlss_runtime_path(void);

// NGX 初始化（幂等，成败都只做一次）。device 是 ID3D12Device*。返回 1=可用 0=不可用。
int32_t mcdr_dlss_can_run(void* device);

// 状态查询。note / optimal / runtime 为调用方提供的 (缓冲, 容量)，可为 NULL；
// flagsOut 收到状态位（1=INIT_TRIED 2=INIT_OK 4=PARAMS_OK 8=OPTIMAL_OK 16=FEATURE_OK
// 32=DISABLED）。
void mcdr_dlss_query(char* note, int32_t noteCap, char* optimal, int32_t optimalCap, char* runtime,
                     int32_t runtimeCap, uint32_t* flagsOut);

// P108 阻塞 B：把厂商运行库目录（`<user.dir>\dxrmc\`）告诉实现，并据此拓宽进程 DLL 搜索
// 路径（SetDefaultDllDirectories + SetDllDirectoryW + AddDllDirectory）。这样探测里那次
// 裸名 LoadLibraryW 才找得到 nvngx_dlss.dll —— 开发跑时进程 cwd 是游戏目录、exe 是 JDK 的
// java.exe，默认搜索路径里根本没有 run\dxrmc。utf8Dir 是 NUL 结尾的 UTF-8 绝对路径。
// 幂等；设备还没建也能调。返回 1=已接受并拓宽 0=空/非法输入。
int32_t mcdr_dlss_register_vendor_dir(const char* utf8Dir, int32_t cap);

// P108：加载 nvngx_dlss.dll（探测单元改调这里，好让每一次尝试都带上自己的 GetLastError
// 落进 native 日志）。返回模块句柄（void*），失败返回 NULL。
void* mcdr_dlss_load_runtime(void);

// P108（用户 m11147）：DLSS 挡位显式指定（Java 侧已把"性能/平衡/质量"做成独立按钮）。
// tier：0=Performance 1=Balanced 2=Quality；>= 0 会让下一次 `ensureFeature` 即使尺寸没变也
// **重建特征**（PerfQualityValue 是 CreateFeature 的输入）；-1 退回"按 render:target 比例就近"
// 的老行为（旧 DLL/旧 JVM 组合）。返回归一化后的档位。幂等。
int32_t mcdr_dlss_request_quality(int32_t tier);

// P108ac（用户 m08033）：DLSS 的**模型**（NGX 的 render preset 提示）。用户反馈"剩下的噪点闪烁
// 是 DLSS 自身的问题，所有用 DLSS 的游戏都有，优化方法是更换 DLSS 的模型"，所以把这条做成旋钮，
// 而不是由我们擅自改默认值。
//
// preset 取 `NVSDK_NGX_DLSS_Hint_Render_Preset`：0 = Default（交给 NGX 按挡位决定）、5 = E /
// 6 = F（已弃用的 CNN 预设）、10 = J / 11 = K / 12 = L / 13 = M（transformer）。预设是
// **CreateFeature 的输入**，所以值一变化，下一次 `ensureFeature` 会重建特征（与挡位同一机制）。
// 越界（<0 或 >15）返回 -1 且不改变现状；返回实际记录的预设值。幂等。
int32_t mcdr_dlss_request_preset(int32_t preset);

// P108（用户 m11147 后续）：把 **JVM 经模块 `createTexture` 建的 UAV 输出纹理**交给 DLSS。
//
// 为什么需要：本单元自建的私有 UAV 纹理**承载不了写入** —— 控制组实测（同源、同调用点、
// 同一条命令列表）：把输入色拷进模块建的目标读回 `mean=17.8/16.5/11.6/255.0`，拷进本单元的
// 私有纹理读回 `mean=0.0/0.0/0.0/0.0`（连 alpha 都是 0），落定后仍为 0。所以改由 JVM 走那条
// 已被证明可写的创建路径建纹理，本单元只负责用它；私有纹理仅作旧组合的兜底。
//
// resource 是**模块侧解包后的裸 `ID3D12Resource*`**（`Dx12Object` 的布局只有模块知道；
// 绝不要在这里 reinterpret JVM 句柄 —— 那正是 mv->GetDesc() 空 vtable 崩溃的成因）。
// 传 nullptr 清除覆盖、退回私有兜底。资源**不归本单元所有**，生命周期由模块保证。
// 返回 1=已接受 0=已清除。
int32_t mcdr_dlss_set_output_texture(void* resource);

// P113：Reflex —— 在 Present 之前调用（slReflexSleep），内部懒加载 interposer。
void mcdr_reflex_before_present(void);
// P115：Present 之后发 PCL 的 ePresentEnd 标记。
void mcdr_reflex_after_present(void);
// P118：当前是否启用 Streamline 路径（默认 auto：运行库存在即启用；`MCDR_DLSS_IMPL=ngx` 可强制回退）。
int32_t mcdr_dlss_streamline_requested(void);

// P122：Reflex 模式（0=off 1=eLowLatency 2=eLowLatencyWithBoost）与可用性查询。
void mcdr_reflex_set_mode(int32_t mode);
int32_t mcdr_reflex_available(void);

// P117：PCL stats —— 帧开始（建本帧 token + eSimulationStart）与通用标记。
void mcdr_reflex_frame_begin(void);
void mcdr_reflex_marker(int32_t marker);

// P115：Reflex 帧率限制器（微秒/帧，0 = 关闭）。
void mcdr_reflex_set_frame_limit(uint32_t micros);

// P112：缓存每帧相机常量（Streamline 的 sl::Constants 所需；结构见 mcdr_abi.h 的
// mcdr_dlss_constants）。JVM 每帧在 evaluate 之前调用。返回 1=已缓存，0=参数为空。
int32_t mcdr_dlss_set_constants(const mcdr_dlss_constants* constants);

// ---------------------------------------------------------------------------
// P108：**跟踪型状态过渡**。
//
// 为什么不再自己写 barrier 的 Before 状态：`d3d12_device.cpp` 的 `endRenderPass`（P11）在
// 每次 pass 结束时会把每个附件**显式回切 COMMON**，所以"上一个 pass 把自己留成
// RENDER_TARGET/DEPTH_WRITE"这个假设并不成立。旧代码在这里手写
// `RENDER_TARGET/DEPTH_WRITE → PIXEL_SHADER_RESOURCE`，Before 与实际状态错配 —— release 下
// 静默、debug 层会报 "Before state ... does not match"，而这类错配正是 P11 注释里记过的
// TDR 冻结成因。
//
// 正确做法：本单元不猜状态，把过渡请求交给**设备模块**（它按 `ctx->resourceState` 逐资源
// 跟踪，`transitionTextureTo` 在 from==to 时自动跳过，幂等）。`tlCtx` 是模块给的不透明
// 指针，本单元只负责原样回传。
using DlssTransitionFn = void (*)(void* ctx, void* resource, int32_t toState);
using DlssQueryStateFn = int32_t (*)(void* ctx, void* resource);

// B2：纹理读回的返回值（POD，**不要**在这里放容器 —— 模块与普通 TU 看到的
// std::string/vector 不是同一个类型）。mean/min/max 都是**原始字节**统计，与
// dumpManyResources 原来打印的 `mean=… min=… max=…` 同一口径，便于对照旧日志。
typedef struct DlssTexStats {
    double mean[4];
    uint32_t min[4];
    uint32_t max[4];
} DlssTexStats;

// B2：模块提供的**读回回调**。实现单元不再自建 command list/queue/fence/staging
// （那正是 #525/#527/#838 与 RemoveDevice 的来源），而是把"读这张纹理的统计"交给
// 设备模块：它用**当前这个 ctx 的 command list** 记录一次 tracking 内的
// CopyTextureRegion 到 readback buffer，并在下一次调用时用模块 fence 等它完成。
//
// 语义（必须按此读）：
//   * 返回 1 = out 已填好（**上一帧**记录的那份拷贝；诊断读数延迟一帧是设计选择，
//     因为调用时 list 还开着、本帧的 copy 尚未提交）；
//   * 返回 0 = 还没有可用的读数（第一次调用）或参数不可用；
//   * 返回 -1 = 失败（创建/映射失败）。
// resource 是**模块侧解包后的裸 ID3D12Resource***（见 mcdr_dlss_set_output_texture 的说明）。
using DlssReadbackFn = int32_t (*)(void* ctx, void* resource, DlssTexStats* out);

// ---------------------------------------------------------------------------
// B4：私有纹理由**模块**创建（实现单元不再自建任何 D3D12 纹理）。
//
// 为什么必须这样：实现单元自己 `CreateCommittedResource` 出来的纹理不在模块的
// `ctx->resourceState` 表里（那张表以裸 `ID3D12Resource*` 为键），过渡只能按 COMMON 兜底，
// `stateOf` 也就答不出真实状态 —— "谁真正管状态机"于是有了两个答案。P108x 那串
// #525/#527/#838 与 RemoveDevice 就是这个分叉的产物。现在创建走这里：
//   * 模块建资源、模块持有 `Dx12Object`，并把资源**创建即登记**进当前 ctx 的跟踪表
//     （初始 COMMON），所以本列表里的第一次过渡就有真实锚点；
//   * 销毁走模块的延迟删除路径（`destroyObject`），不会在有打开的命令列表时裸
//     `Release()`（那会报 "deleted prior to closing the command list"）；
//   * 实现单元只拿两个不透明值：句柄（只能交回 destroy）与裸资源指针（给 NGX / 拷贝用）。
//
// `flags` 是 D3D12_RESOURCE_FLAGS 的位（1=ALLOW_RENDER_TARGET, 2=ALLOW_DEPTH_STENCIL,
// 4=ALLOW_UNORDERED_ACCESS）；`dxgiFormat` 是 **DXGI_FORMAT 的数值**（不是 JVM 那边的
// 格式 ordinal，别混）。
// ---------------------------------------------------------------------------
typedef struct DlssTextureDesc {
    uint32_t width;
    uint32_t height;
    uint32_t dxgiFormat;
    uint32_t mipLevels;
    uint32_t arraySize;
    uint32_t sampleCount;
    uint32_t flags;
} DlssTextureDesc;

// 返回 1 = *outObject / *outResource 都已填好；0 = 参数或设备不可用；-1 = 创建失败。
using DlssCreateTextureFn = int32_t (*)(void* ctx, const DlssTextureDesc* desc, void** outObject,
                                       void** outResource);
// `ctx` 只用于把资源从该 ctx 的跟踪表里摘掉，可以为 nullptr（shutdown 时就是这样）。
using DlssDestroyTextureFn = void (*)(void* ctx, void* object);
// 注入创建/销毁入口。模块在 wireDlssImpl() 里调一次；返回 1 = 两个入口都装上了。
int32_t mcdr_dlss_install_texture_ops(void* createFn, void* destroyFn);

// 在**已打开**的命令列表上创建/重建 DLSS 特征（尺寸变化时自动重建）。
// `tl`/`tlCtx` 非空时所有资源过渡都通过它请求（见上），为空则不做过过渡。
int32_t mcdr_dlss_create(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                         void* tlCtx);
// 每帧一次的真实 NGX 评价。commandList 是 ID3D12GraphicsCommandList*。
// 返回 1=真的跑了 0=跳过（没特征/参数为空）-1=失败或已关闭。
int32_t mcdr_dlss_evaluate(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                           void* tlCtx, DlssReadbackFn rb);

// P108：**深度快照**。DLSS 的评估点在世界画完、GUI 之前，而 vanilla 在
// `GameRenderer.render` 的 clearDepthTexture 已把深度清成 0.0（reverse-Z 的"远"），所以
// 直接把 JVM 传来的深度喂给 DLSS 等于没有深度 —— 深度感知升采样会退化成纯色彩升采样。
// JVM 因此在**清深度之前**（录速度矢量那一处）调这个函数，把当时有效的深度拷进我们自己的
// 私有 D32 纹理；之后 evaluate 若存在快照就用它，没有则退回 JVM 传来的深度。
//
// depthResource 的状态会被**恢复成调用前的状态**（JVM 在世界渲染中途调用，不能打乱
// vanilla 的状态机）。返回 1=已建立/复用快照 0=参数不可用（INFO）-1=创建或拷贝失败（WARN 一次）。
int32_t mcdr_dlss_snapshot_depth(void* commandList, void* depthResource, DlssTransitionFn tl,
                                 void* tlCtx, DlssQueryStateFn stateOf);

// P108（用户 m10920）：DLSS 升采样的独立开关。**只记录**实现侧的开关状态并打一行 P108
// 日志，不触碰已加载的 NGX 运行库（关掉再打开不该丢掉 Init/CreateFeature 的结果）。
// 返回入参归一化后的状态（1=开 0=关）。幂等；设备还没建也能调。
// 真正的 gate 在设备模块的 applyDlssToTexture（读 gCtx.dlssEnabled）；这里只是把同一状态
// 同步给实现侧，供状态查询与日志使用。
int32_t mcdr_dlss_set_enabled(int32_t enabled);

// 释放特征与自建资源并 Shutdown NGX。必须在**设备还活着**时调用（destroyDevice 里）。
void mcdr_dlss_shutdown(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MCDR_DLSS_IMPL_H

