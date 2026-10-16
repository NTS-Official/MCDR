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

// 在**已打开**的命令列表上创建/重建 DLSS 特征（尺寸变化时自动重建）。
// `tl`/`tlCtx` 非空时所有资源过渡都通过它请求（见上），为空则不做过过渡。
int32_t mcdr_dlss_create(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                         void* tlCtx);
// 每帧一次的真实 NGX 评价。commandList 是 ID3D12GraphicsCommandList*。
// 返回 1=真的跑了 0=跳过（没特征/参数为空）-1=失败或已关闭。
int32_t mcdr_dlss_evaluate(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                           void* tlCtx);

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