// mcdr_abi.h — public C ABI of the MCDR native render library.
//
// This header is the *only* contract between the native library and the JVM
// side. It replaces the former JNI bridge (6 files, 2158 lines, 115
// `Java_com_xgdt_dx12_dx12_Dx12Native_*` exports) with a flat C ABI that the
// Java side reaches through the FFM API (java.lang.foreign).
//
// Design rules (see 26.2-rt-experiment-design.md §5.2/§5.3):
//   * C linkage only (`extern "C"`), no name mangling, no generated headers.
//   * Every cross-language aggregate is ONE struct definition, mirrored 1:1 by
//     a `MemoryLayout.structLayout` on the Java side. No hand-rolled byte
//     offsets, no packed blobs, no byte-order ambiguity.
//   * Handles are opaque pointers to native objects — never integers whose
//     meaning the Java side has to guess.
//   * Strings are caller-allocated UTF-8 buffers; the callee returns the
//     required byte count (including the NUL) so the caller can grow and retry.
//   * No exceptions cross the boundary. Boolean entry points return int32_t
//     1/0. mcdrLastError() carries the message.
//   * Array parameters are (pointer, count) pairs, never JNI arrays.
//   * One single upcall stub type (mcdr_fence_callback_fn) carries an integer
//     user handle, so the native layer never holds a JVM object reference.
//
// Windows x64 mapping note for the Java mirror: the MSVC `long` is 32-bit, so
// every parameter written here as int32_t must be JAVA_INT on the Java side
// (only int64_t/size_t/pointers are JAVA_LONG/JAVA_ADDRESS).

#ifndef MCDR_ABI_H
#define MCDR_ABI_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(MCDR_EXPORTS)
#define MCDR_API __declspec(dllexport)
#else
#define MCDR_API __declspec(dllimport)
#endif
#define MCDR_CALL __cdecl
#else
#define MCDR_API __attribute__((visibility("default")))
#define MCDR_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever ANY struct layout or signature below changes. The Java side
// asserts equality at load time and aborts backend creation on mismatch.
//
// P108 待办（这一版**故意仍为 1**，因为下面这组改动全是「只增不改」的追加项，而
// Kotlin 侧当前正被包重构重写，此刻把这里改成 2 会让 MCDR_ABI.kt 的
// `version != MCDR_ABI_VERSION` 断言在设备创建时硬失败）：
//   1. MCDR_ABI.kt 补上 mcdr_dx12CreateDlssFeature / mcdr_dx12EvaluateDlss /
//      mcdr_dx12QueryDlss 三个 FFM 绑定（签名见 mcdr_abi.h 与 P108 报告）；
//   2. MCDR_Layout.kt 登记 MCDR_DLSS_INPUT 并加 EXPECTED_C_SIZES 行（C sizeof = 112）；
//   3. 完成 1、2 之后把这里改成 2u，并同步 MCDR_ABI.kt 的 MCDR_ABI_VERSION = 2。
//
// v2 -> v3（P271，超分抽象层）：
//   - 新增 mcdr_sr_caps（sizeof = 80 / align = 4）与 mcdr_sr_dispatch（sizeof = 136 /
//     align = 8）两张表，以及 8 个 mcdr_dx12Sr* 入口（声明见本文件 P271 段）。
//     注：这里原来写 128，是笔误 —— 结构体自身的注释（P275 段）与 abi_meta.cpp 的
//     MCDR_FIELD 表都按 136 走（末尾 _pad2 在 132，align 8 再补 4）。
//   - 老导出一个都没删、一个签名都没改；DLSS 的既有路径原样保留，SrEvaluate 在 v1 里
//     就是把载荷翻译成 mcdr_dlss_input 后转发给同一条实现。
//   - **必须同一次改齐**：MCDR_ABI.kt 的 MCDR_ABI_VERSION = 3、两张表的 FFM 镜像、
//     MCDR_Layout.kt 的 SR_CAPS / SR_DISPATCH 与 EXPECTED_C_SIZES 行、以及
//     abi_meta.cpp 的 layoutTable()（那边少一个字段，Kotlin 的 verify() 会直接 abort）。
//
// v3 -> v4（P293–P295，帧生成抽象层）：
//   - 新增 mcdr_fg_caps（sizeof = 80 / align = 4）与 mcdr_fg_dispatch（sizeof = 96 /
//     align = 8）两张表，以及 5 个 mcdr_dx12Fg* 入口（声明见本文件 P293 段）。
//   - 老导出仍然一个没删：mcdr_dx12SetDlssgMode / mcdr_dx12GetDlssgPresented 降级为
//     **转发**（组一个 dispatch 调 FgSetMode / 直接回 FgPresented），语义与签名不变，
//     Kotlin 侧 MCDR_ABI.kt 的既有绑定不用改。
//   - 同一次改齐的四件：MCDR_ABI.kt 的 MCDR_ABI_VERSION = 4 + 5 个新绑定、
//     MCDR_Layout.kt 的 FG_CAPS / FG_DISPATCH 与 EXPECTED_C_SIZES 行、
//     abi_meta.cpp 的 layoutTable()（两张新表）、以及本文件的 FG 段。
#define MCDR_ABI_VERSION 4u

// The same value at runtime, so a stale Java-side constant cannot be optimised
// away and a mismatched pair fails at load time rather than at first draw.
MCDR_API uint32_t MCDR_CALL mcdrABIVersion(void);

// ---------------------------------------------------------------------------
// Opaque handles
//
// Each typedef forward-declares the real C++ class; the native side
// reinterpret_casts between them. The Java side only ever sees a MemorySegment
// address and must never invent one.
// Real class names, not invented ones: query the core headers, do not guess.
// Each handle names the real C++ class so the native side can reinterpret_cast
// with no intermediate wrapper object and no runtime cost. In C++ the tag is
// declared with the SAME class-key the definition uses, because MSVC's C4099
// ("type name first seen using 'class' now seen using 'struct'") becomes a
// hard error under this project's TreatWarningAsError=true.
// C++ forward declarations MUST live inside namespace d3d12_mc. A global-scope
// `struct Dx12Object;` is a *different type* from `d3d12_mc::Dx12Object`, so the
// typedef below would silently name an incomplete global type and every adapter
// call site would fail with C2664 "cannot convert argument 1".
#ifdef __cplusplus
namespace d3d12_mc {
class PartitionedDescriptorAllocator;
class BundleRecorderPool;
class MainCommandExecutor;
class AsyncFenceManager;
struct Dx12Object;
struct CommandContext;
struct Dx12Pipeline;
struct Dx12Surface;
struct QueryPool;
}  // namespace d3d12_mc

typedef d3d12_mc::Dx12Object mcdr_object;       // texture / buffer / sampler / texture view
typedef d3d12_mc::CommandContext mcdr_command_ctx;  // one command encoder (3-deep list ring)
typedef d3d12_mc::Dx12Pipeline mcdr_pipeline;
typedef d3d12_mc::Dx12Surface mcdr_surface;
typedef d3d12_mc::QueryPool mcdr_query_pool;
typedef d3d12_mc::PartitionedDescriptorAllocator mcdr_desc_alloc;
typedef d3d12_mc::BundleRecorderPool mcdr_bundle_pool;
typedef d3d12_mc::MainCommandExecutor mcdr_executor;
typedef d3d12_mc::AsyncFenceManager mcdr_fence_mgr;
#else
typedef struct mcdr_object mcdr_object;
typedef struct mcdr_command_ctx mcdr_command_ctx;
typedef struct mcdr_pipeline mcdr_pipeline;
typedef struct mcdr_surface mcdr_surface;
typedef struct mcdr_query_pool mcdr_query_pool;
typedef struct mcdr_desc_alloc mcdr_desc_alloc;
typedef struct mcdr_bundle_pool mcdr_bundle_pool;
typedef struct mcdr_executor mcdr_executor;
typedef struct mcdr_fence_mgr mcdr_fence_mgr;
#endif

// ---------------------------------------------------------------------------
// Cross-language aggregates
//
// Every struct below has an exact Java mirror built from MemoryLayout. Offsets
// and sizes are asserted at runtime by mcdrABILayout() — never by hand.
// ---------------------------------------------------------------------------

// One pipeline binding slot, shared by mcdr_dx12PushDescriptors and the bundle
// recorder. Replaces bType/bBuf/bOff/bLen/bView.
typedef struct mcdr_binding_slot {
    int32_t type;          // 0 = CBV, 1 = SRV (texture view), 2 = SRV (texel buffer)
    int32_t texelFormat;   // type 2 only: GpuFormat ordinal
    mcdr_object* buffer;        // type 0/2 only
    mcdr_object* view;          // type 1 only
    int64_t offset;        // type 0/2 only
    int64_t length;        // type 0/2 only
} mcdr_binding_slot;

// One draw's index/vertex geometry, used by the bundle recorder. Replaces
// idxBuf/idxType/idxCount/firstIdx/baseVert/vbSlot/vbBuf/vbOff/vbStride.
typedef struct mcdr_partition_draw {
    mcdr_object* indexBuffer;
    mcdr_object* vertexBuffer;
    int64_t vertexOffset;
    int32_t indexType;     // 0 = SHORT (R16_UINT), 1 = INT (R32_UINT)
    int32_t indexCount;
    int32_t firstIndex;
    int32_t baseVertex;
    int32_t vertexSlot;
    int32_t vertexStride;
    int32_t _pad0;
} mcdr_partition_draw;

// One colour target of a graphics pipeline. Replaces 8 bytes of the old packed
// descriptor blob. format == -1 means "unused slot" (DXGI_FORMAT_UNKNOWN).
typedef struct mcdr_color_target_desc {
    int32_t format;             // GpuFormat ordinal, or -1
    uint8_t writeMask;
    uint8_t blendEnabled;
    uint8_t srcColor;           // BlendFactor ordinal
    uint8_t dstColor;           // BlendFactor ordinal
    uint8_t colorOp;            // BlendOp ordinal
    uint8_t srcAlpha;           // BlendFactor ordinal
    uint8_t dstAlpha;           // BlendFactor ordinal
    uint8_t alphaOp;            // BlendOp ordinal
} mcdr_color_target_desc;

typedef struct mcdr_depth_state_desc {
    int32_t depthFormat;        // GpuFormat ordinal
    uint8_t depthWrite;
    uint8_t depthCompareOp;     // CompareOp ordinal
} mcdr_depth_state_desc;

// HLSL input element. `semanticName` is a NUL-terminated ASCII string. The old
// bridge synthesised it (empty -> "TEXCOORD"), which the packed blob could not
// express; here it crosses explicitly.
typedef struct mcdr_input_element_desc {
    int32_t location;
    int32_t binding;
    int32_t format;         // GpuFormat ordinal
    int32_t offset;
    int32_t stride;
    int32_t stepRate;       // 0 = per-vertex, 1 = per-instance
    int32_t _pad0;
    const char* semanticName;   // NUL-terminated; NULL or "" -> "TEXCOORD"
} mcdr_input_element_desc;

typedef struct mcdr_pipeline_binding_desc {
    uint8_t type;               // 0 = CBV, 1 = SRV + static sampler, 2 = SRV
    uint8_t reg;
} mcdr_pipeline_binding_desc;

// Full graphics-pipeline description. Replaces the LITTLE_ENDIAN packed
// ByteBuffer that dx12CreateGraphicsPipeline() unpacked by hand.
// `vsData`/`psData` point at DXBC bytecode (not NUL-terminated).
typedef struct mcdr_pipeline_desc {
    const void* vsData;
    int32_t vsLen;
    int32_t _pad0;
    const void* psData;
    int32_t psLen;
    int32_t colorCount;
    const mcdr_color_target_desc* colorTargets;
    int32_t hasDepth;
    int32_t _pad1;
    mcdr_depth_state_desc depth;
    int32_t topology;       // PrimitiveTopology ordinal
    int32_t cullEnabled;
    int32_t polygonMode;    // PolygonMode ordinal
    int32_t inputElementCount;
    const mcdr_input_element_desc* inputElements;
    int32_t bindingCount;
    int32_t _pad2;
    const mcdr_pipeline_binding_desc* bindings;
} mcdr_pipeline_desc;

// ---------------------------------------------------------------------------
// P72: mesh-shader (amplification / mesh) pipeline description.
//
// `asData`/`msData`/`psData` point at DXIL bytecode produced by DXC
// (as_6_x / ms_6_x / ps_6_x) -- NOT HLSL source and NOT DXBC: mesh shaders
// cannot be compiled by D3DCompile/fxc (SM 5.1 ceiling). `msData` is required;
// `asData` may be NULL/0 (no amplification stage) and `psData` may be NULL/0
// (no pixel shader, in which case nothing is written to the colour targets).
//
// Bindings reuse mcdr_pipeline_binding_desc with the SAME layout contract as
// mcdr_pipeline_desc (CBV -> root descriptor, SRV -> the single descriptor
// table at root index cbvCount), so mcdr_dx12PushDescriptors is unchanged.
typedef struct mcdr_mesh_pipeline_desc {
    const void* asData;         // DXIL amplification shader bytecode, or NULL
    int32_t asLen;
    int32_t _pad0;
    const void* msData;         // DXIL mesh shader bytecode (required)
    int32_t msLen;
    int32_t _pad1;
    const void* psData;         // DXIL pixel shader bytecode, or NULL
    int32_t psLen;
    int32_t colorCount;
    const mcdr_color_target_desc* colorTargets;
    int32_t hasDepth;               // same meaning as mcdr_pipeline_desc::hasDepth
    int32_t _pad2;
    mcdr_depth_state_desc depth;    // same layout as mcdr_pipeline_desc::depth
    int32_t cullEnabled;
    int32_t polygonMode;            // PolygonMode ordinal
    int32_t bindingCount;
    int32_t _pad3;
    const mcdr_pipeline_binding_desc* bindings;
} mcdr_mesh_pipeline_desc;

// ---------------------------------------------------------------------------
// ABI self-check
//
// The native side publishes one entry per mirrored struct field. The Java side
// walks the same list from its own MemoryLayouts and fails fast on the first
// mismatch. This is the check JNI never had: under JNI a package rename or an
// added struct field silently corrupted memory at runtime.
// ---------------------------------------------------------------------------
typedef struct mcdr_abi_field {
    const char* name;   // "mcdr_binding_slot.buffer"
    uint64_t offset;
    uint64_t size;
} mcdr_abi_field;

MCDR_API void MCDR_CALL mcdrABILayout(const mcdr_abi_field** fields, int32_t* count);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// Copies the calling thread's last error message into `buf` as UTF-8 and
// returns the required byte count including the NUL terminator, or 0 when no
// error is pending. buf == NULL / cap == 0 performs a size query only.
MCDR_API int32_t MCDR_CALL mcdrLastError(char* buf, int32_t cap);

// Verbosity for the native log, mirroring DX12_LOG_VERBOSE.
MCDR_API void MCDR_CALL mcdrSetLogVerbose(int32_t enabled);

// Native log level: 0=error, 1=warning, 2=info, 3=debug.
MCDR_API void MCDR_CALL mcdrSetLogLevel(int32_t level);

// P184: progress markers on the shutdown path (destroySurface/destroyDevice) are INFO lines.
// 1 (default) keeps them visible at any log level; 0 makes them plain INFO (filtered by the level).
// Config: `[logging] shutdownTrace`.
MCDR_API void MCDR_CALL mcdrSetShutdownTraceAlways(int32_t always);

// 把每条 native 日志同时镜像到该文件（配置项 `[logging] nativeLogFile`；用户 m03308 之前这条
// 配置只写在 toml 里、没有任何代码读它 —— 它的作用是让"用户端拿不到 native 行"这件事不再发生）。
// `utf8Path` 为 UTF-8 路径，相对路径按进程工作目录（游戏目录）解析，追加写、逐行 flush。
// 传 NULL 或空串 = 关闭镜像并释放已打开的句柄。
// 返回 1=已配置 0=已关闭（能否真的落盘要到下一次写日志时才知道，这里不做同步 open）。
MCDR_API int32_t MCDR_CALL mcdrSetLogFile(const char* utf8Path);

/// Set (or, with a null/empty value, clear) one process-wide environment variable from the
/// Java-side configuration, so the handful of native switches that are still read with
/// getenv()/_wgetenv() (DX12_DEBUG_LAYER, MCDR_DLSS_IMPL, DX12_NATIVE_LOG_FILE, ...) can be
/// driven from config/dxr_mc-client.toml instead of the launcher environment. Must be called
/// before the first device is created; later calls only affect reads that happen afterwards.
MCDR_API void MCDR_CALL mcdr_dx12SetEnvironment(const char* name, const char* value);

// PIX GPU capture: load Microsoft PIX's capture layer (WinPixGpuCapturer.dll) into
// this process. The layer hooks D3D12, so it has to be in place BEFORE the first
// device is created — the mod calls this as soon as its config is read, and the
// device-creation paths retry lazily. Irreversible for the life of the process.
// Returns 1 when the layer is loaded, 0 when capture was not requested, and -1 when
// it was requested but the DLL could not be found or loaded (an ERROR line lists the
// paths that were searched). Idempotent.
MCDR_API int32_t MCDR_CALL mcdrSetPixCapture(int32_t enabled);

// Where PIX's DLLs live: the mod's [debug] pixDirectory config, which may name a DLL itself or
// the directory holding them. This exists because locating the DLLs has to be a documented
// setting, not an environment variable — the project rule is that every switch and every path
// the player can change goes through the NeoForge config and a dedicated entry point here
// (an env var is an entry point nobody can find in the config screen).
// Call it before mcdrSetPixCapture: the layer has to be loaded before the first device is
// created, so the path has to be known that early. An empty string clears the setting and
// restores automatic detection (next to MCDR.dll, then the newest %ProgramFiles%\Microsoft
// PIX\<version>). Returns 1 when the path was accepted, 0 when it was cleared, and -1 when it is
// not valid UTF-8 or does not exist (a WARN line says which).
MCDR_API int32_t MCDR_CALL mcdrSetPixDirectory(const char* utf8Path);

// PIX timing capture, driven by this process instead of by PIX's UI. PIX's own continuous
// ("counters") recording keeps everything inside the PIX session — nothing reaches the disk
// unless the user saves it — while a GPU capture taken by attaching lands in
// %TEMP%\*.wpix. A capture that has to appear under the game directory therefore has to be
// started from inside the process: PIXBeginCapture(PIX_CAPTURE_TIMING, {file_name}) ...
// PIXEndCapture(0). The runtime and capturer DLLs are loaded by mcdrSetPixCapture (they hook
// D3D12/ETW as they load, so they want to be in place before the first device), and
// mcdrStartPixTimingCapture loads them lazily if that has not happened yet.
//
// `utf8Path` is the full path of the .wpix to write; the caller creates the directory. The
// call is made on a thread of its own because PIX's runtime sets up COM on load and the render
// thread's apartment is already fixed (calling from there answers RPC_E_CHANGED_MODE).
// Returns 1 when the capture started, 0 when one was already running, and on failure the
// HRESULT that explains it (always negative), so the caller can show the code as-is:
// E_FAIL when the runtime DLLs could not be loaded, E_INVALIDARG for an empty path,
// HRESULT_FROM_WIN32(ERROR_TIMEOUT) when PIX did not answer in time, and PIX's own code
// otherwise (E_ACCESSDENIED = the process is not elevated, ERROR_ALREADY_EXISTS = a capture
// session from an earlier run is still alive). mcdrLastError() carries the readable reason.
// A timing capture records through ETW sessions. PIX's documentation says membership in the
// built-in `Performance Log Users` group (S-1-5-32-559) is enough; on this machine it is NOT
// (measured 2026-10-21: the group was active and enabled and the call still returned
// E_ACCESSDENIED, while the same call in an elevated process returned S_OK and wrote the
// file). Elevation is therefore required here, and because gradle reuses a daemon it has to
// be an elevated launch with `--no-daemon` (or `gradlew --stop` first).
MCDR_API int32_t MCDR_CALL mcdrStartPixTimingCapture(const char* utf8Path);

// Stops the capture that mcdrStartPixTimingCapture started. PIX writes the file out on its own
// thread, so this returns without waiting (a capture with the memory events on is hundreds of
// megabytes and would otherwise stall the frame that called it). Returns 1 when a stop was
// requested, 0 when nothing was recording, and a negative HRESULT when the writer thread could
// not be started.
MCDR_API int32_t MCDR_CALL mcdrStopPixTimingCapture(void);

// Variable-rate shading: the shading-rate control point. `rate` is a raw
// D3D12_SHADING_RATE value (1X1=0x0, 1X2=0x1, 2X1=0x4, 2X2=0x5, 2X4=0x6, 4X2=0x9,
// 4X4=0xA) and applies to every graphics command list the renderer starts from then
// on; -1 turns the control off, which means no RSSetShadingRate call is made at all
// and the device keeps its default 1X1 rate. Only the per-draw rate exists so far —
// a shading-rate image (tier 2) and per-pass policies hang off this same point later.
// Returns 1 when accepted, 0 when the device cannot honour it (no variable-rate
// shading at all, or 2X4/4X2/4X4 without AdditionalShadingRatesSupported — only
// answerable once the device exists), and -1 for an unknown rate value. A request
// that arrives before the device is created (the usual case: the mod pushes it as
// soon as it reads its config) is accepted and gated later, on the first command
// list, with a WARN naming the reason.
MCDR_API int32_t MCDR_CALL mcdrSetShadingRate(int32_t rate);

// P231: the **scoped** shading-rate control point. `rate` is one of the seven raw
// D3D12_SHADING_RATE values (0x0/0x1/0x4/0x5/0x6/0x9/0xA; -1 is *not* accepted here) and is
// issued as `RSSetShadingRate` on `ctx`'s currently open command list, right now. Unlike
// mcdrSetShadingRate it neither reads nor writes the stored player setting: the caller brackets
// exactly the draws that may be shaded coarsely and restores 1X1 afterwards, which is what makes
// the coarse rate stop at the terrain instead of also hitting the sky, particles and the GUI
// (P232). Because this module never uses ID3D12GraphicsCommandList4::BeginRenderPass — a "pass"
// here is just OMSetRenderTargets — the call is legal in the middle of one.
// Returns 1 = issued, 0 = this device cannot honour the rate (VRS tier 0, or 2X4/4X2/4X4 without
// AdditionalShadingRatesSupported) or the list has no ID3D12GraphicsCommandList5, -1 = unknown
// rate value, -2 = ctx is null or has no open command list (caller bug; WARN once).
MCDR_API int32_t MCDR_CALL mcdr_dx12SetShadingRateNow(mcdr_command_ctx* ctx, int32_t rate);

// P231: the two shading-rate combiners used by every RSSetShadingRate this module issues. Both
// take a D3D12_SHADING_RATE_COMBINER value (PASSTHROUGH=0 OVERRIDE=1 MIN=2 MAX=3 SUM=4). Stage 0
// combines the per-draw rate with the per-primitive rate (a mesh shader's SV_ShadingRate output),
// stage 1 combines that result with the shading-rate image (tier 2). The defaults are
// {PASSTHROUGH, PASSTHROUGH}; the per-draw policy keeps stage 0 at PASSTHROUGH until the mesh
// shader actually writes SV_ShadingRate, because a per-primitive rate of 1X1 under MIN would
// undo every 2X2 the policy asks for. Returns 1 = stored, -1 = a value outside 0..4.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetShadingRateCombiners(int32_t first, int32_t second);

// Anti-aliasing mode, same numbering as the Java-side option
// (**0=OFF, 1=SMAA, 2=TAA, 3=DLAA**). SMAA runs as three full-screen passes
// (edge detection, blend weights, neighbourhood blending) over the world layer, right
// after the world has rendered and before the GUI is drawn — see
// mcdr_dx12ApplySmaaToTexture, which the Java side calls at exactly that point. Applying
// it to the composed frame instead (at the final blit) also softened the GUI.
// TAA (2) has a native resolve as well since P152 — see mcdr_dx12ApplyTaaToTexture; it
// keeps an RGBA16F history ping-pong inside the device and runs at the same call point.
// DLAA (3) is still accepted but treated as OFF (a WARN names it).
//
// P108 (user m10920): DLSS is **no longer** a value of this option. Upscaling and
// anti-aliasing are separate concerns, so DLSS got its own switch — see
// mcdr_dx12SetDlssEnabled and mcdr_dx12EvaluateDlss. The two are now INDEPENDENT: with both
// on, SMAA's three passes run first on the low-resolution world layer and DLSS then
// upscales that result into the window-sized target.
//
// No restart is needed — a mode change takes effect on the next frame. Returns 1 when the
// mode is in effect (OFF, SMAA, TAA), 0 when it was accepted but not implemented (DLAA),
// and -1 for an unknown mode.
MCDR_API int32_t MCDR_CALL mcdrSetAntiAliasing(int32_t mode);

// P108 (user m10920): DLSS upscaling on/off, independent of the anti-aliasing mode and of
// mcdr_dx12SetDlssVendorDir. The Java side owns this as its own option (`dxrMcDlss`, a
// button under the "Illusion settings" section), NOT as a value of `dxrMcAaMode`.
//
// Semantics: 1 enables, 0 disables. Idempotent and callable before the device exists (the
// state is only recorded; the NGX runtime is not touched here). Returns 1 when DLSS is
// enabled afterwards and 0 when it is disabled afterwards, so the caller can echo the
// resulting state. The availability of the feature itself is reported separately by
// mcdr_dx12QueryDlss, and a missing / older DLL simply keeps this switch inert.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssEnabled(int32_t enabled);

// P108: snapshot the world depth into a private D32 texture for DLSS. The Java side calls this
// from exactly the point in the frame where the depth buffer still holds this frame's world
// depth — alongside the motion-vector capture, BEFORE vanilla's `clearDepthTexture`
// (GameRenderer.render) wipes it to 0.0 (reverse-Z "far").
//
// Why: DLSS is evaluated after the world is drawn but before the GUI, and by then the live depth
// is already cleared. Handing that cleared depth to NGX makes the depth term a constant, so
// DLSS's depth-aware upscaling degenerates into a colour-only one. With a snapshot present,
// mcdr_dx12EvaluateDlss feeds the snapshot to NGX instead of the caller's depth.
//
// The state of `depth` is restored to whatever it was on entry (the device module tracks
// per-resource state), so the caller's world-render state machine is not disturbed.
//
// Returns 1 = snapshot taken/reused, 0 = unusable arguments (evaluation keeps working with the
// caller-supplied depth), -1 = creation/copy failed (one WARN, caller's depth used instead).
// Never fails the frame.
MCDR_API int32_t MCDR_CALL mcdr_dx12SnapshotDlssDepth(mcdr_command_ctx* ctx, mcdr_object* depth);

// P108 (user m11147): the DLSS quality tier, as its own button instead of something MCDR derives
// from the render:target ratio.
//   tier: 0 = Performance, 1 = Balanced, 2 = Quality (same order as the Java enum).
// Out-of-range is rejected with -1 and changes nothing; otherwise the normalised tier is
// returned. Idempotent, and callable before the device exists (like the other DLSS options, only
// the state is recorded).
//
// A change REBUILDS the DLSS feature even when the render/target sizes are unchanged, because
// NVSDK_NGX_PerfQuality_Value is an input to CreateFeature, not an Evaluate parameter. The default
// is 2 (Quality), which is what the previous ratio-based selection picked on the 640x360 -> 854x480
// configuration, so behaviour is unchanged until the Java side sets a tier.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssQuality(int32_t tier);
// P213：帧生成（DLSS-G）。mode: 0=off 1=on（固定倍率） 2=dynamic；numFrames 1..5（见 slDLSSGGetState）。
// 选项在渲染线程的下一次 present 生效；Reflex 必须开启，否则 status 会带 eFailReflexNotDetectedAtRuntime。
MCDR_API void MCDR_CALL mcdr_dx12SetDlssgMode(int32_t mode, int32_t numFrames);

/* P213c：上一次 Present 真正提交的帧数（1 = 没插帧）。F3 的 output 用它累积。 */
MCDR_API int32_t MCDR_CALL mcdr_dx12GetDlssgPresented(void);

// P108ac (user m08033): the DLSS *model*, exposed as NGX's "render preset" hint.
//
// The user reported that the remaining noise/flicker is inherent to DLSS (every game using DLSS
// shows it) and that the remedy is to change the model, so this hands the JVM that knob rather
// than having MCDR silently pick a different default.
//   preset: a NVSDK_NGX_DLSS_Hint_Render_Preset value —
//           0  = Default (NGX picks per tier: K for DLAA/Balanced/Quality, M for Performance,
//                L for Ultra Performance) — this is the historical behaviour,
//           5  = E, 6 = F   (deprecated CNN presets),
//           10 = J, 11 = K, 12 = L, 13 = M   (transformer presets).
// Out-of-range is rejected with -1 and changes nothing; otherwise the recorded value is returned.
// Idempotent, and callable before the device exists (only the state is recorded).
//
// A change REBUILDS the DLSS feature even when the render/target sizes are unchanged, because the
// preset is an input to CreateFeature, not an Evaluate parameter.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssRenderPreset(int32_t preset);

// P108 (user m11147 follow-up): hand DLSS the UAV texture the Java side created through the
// module's own texture path, to be used as NGX's output.
//
// Why: this DLL's own `CreateCommittedResource` texture cannot hold a write at all. A control test
// with an identical source, call site and command list showed the module-created target reading
// back `mean=17.8/16.5/11.6/255.0` while the privately created texture read `mean=0/0/0/0` — alpha
// included — both immediately and on the following frame. So the Java side creates a target-sized
// UAV texture (`USAGE_STORAGE = 32`, which `textureFlags()` maps to ALLOW_UNORDERED_ACCESS) and DLSS
// writes there.
//
// Passing NULL clears the override and restores the private fallback. The device module unwraps the
// `mcdr_object` handle (only it knows that layout) and hands the raw resource down; the resource is
// not owned by the native side. A size/format mismatch against the render target is reported as an
// ERROR and falls back rather than failing silently.
//
// Returns 1 = accepted, 0 = cleared, -1 = the handle is not a live texture.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssOutput(mcdr_object* tex);

// ---------------------------------------------------------------------------
// P108: DLSS super-resolution (NVSDK_NGX). Data plane + the real NGX evaluate call.
//
// Everything the per-frame Evaluate needs arrives in ONE struct so that the Java side
// has a single "per-frame parameter package" to fill in and one call to make. Only two
// entry points cross the boundary: mcdr_dx12CreateDlssFeature (once per render/target
// size) and mcdr_dx12EvaluateDlss (once per frame). Both are no-ops when the
// anti-aliasing mode is not 4.
//
// What MCDR does NOT have, and therefore what is deliberately left at 0 here:
//   * a transparency / reactive mask (Minecraft has no per-pixel "this is a moving
//     transparent object" pass) — pInTransparencyMask stays NULL;
//   * a GPU auto-exposure texture (vanilla tone-maps in the shader and never writes an
//     exposure buffer) — exposureMode stays MCDR_DLSS_EXPOSURE_NONE, so DLSS is told to
//     use the constant exposureScale below instead of sampling one;
//   * per-object motion vectors. The velocity pass reprojects the camera only, so moving
//     entities get no velocity and ghost slightly. That is a known, accepted limitation
//     of the current data plane (see planned_implementations.md), not a bug in this call.
//
// Depth convention (verified against the live code, see the P108 report):
// Minecraft 26.x is REVERSE-Z — net.minecraft.client.renderer.Projection.java:70-74 swaps
// near/far before building the matrix, CompareOp.GREATER_THAN_OR_EQUAL is the depth func
// everywhere (RenderPipelines.java), and the depth buffer is cleared to 0.0 (far) in
// GameRenderer.java:443/578. So depthInverted must be true.
// ---------------------------------------------------------------------------

// How the constant exposure value below is interpreted.
#define MCDR_DLSS_EXPOSURE_NONE 0u   // pure constant: exposureScale is the pre-exposure
#define MCDR_DLSS_EXPOSURE_AUTO 1u   // reserved: a GPU exposure texture (not available yet)

// Everything one DLSS Evaluate needs. Field names that start with pIn* are opaque
// mcdr_object* texture handles; every other field is a plain value. All comments live to
// the right of their field so the struct body stays one clean "type name;" per line.
//
// LAYOUT — verified with the real toolchain (cl 19.51 / v145, x64, /std:c++latest):
//   sizeof = 112, alignment = 8, offsets: renderWidth 0, renderHeight 4, targetWidth 8,
//   targetHeight 12, jitterOffsetX 16, jitterOffsetY 20, mvScaleX 24, mvScaleY 28,
//   preExposure 32, exposureScale 36, exposureMode 40, alphaUpscaling 44, nearZ 48,
//   farZ 52, depthInverted 56, reset 60, depthFormatOrdinal 64, _pad0 68, pInColor 72,
//   pInDepth 80, pInMotionVectors 88, pInOutput 96, _pad1 104, _pad2 108.
// _pad0 covers the four bytes the compiler would otherwise insert silently between
// depthFormatOrdinal and the first pointer; _pad1 keeps the total a multiple of 8. Both
// are named fields on purpose: MCDR_Layout.kt mirrors them, and abi_check.py only sees
// named fields (MemoryLayout.structLayout inserts no padding of its own).
typedef struct mcdr_dlss_input {
    // --- identity / geometry of this frame ---
    int32_t renderWidth;   // INPUT size, px = window * dxrMcRenderScale (the DLSS render size)
    int32_t renderHeight;  // INPUT size, px
    int32_t targetWidth;   // OUTPUT size, px = window size = GUI target = swapchain (DLSS target size)
    int32_t targetHeight;  // OUTPUT size, px
    // Jitter in pixels, RENDER space, not normalised and NOT negated. Source: the
    // Halton(2,3) sequence in D3D12Temporal.kt (jitter() returns [-0.5,+0.5) pixels).
    float jitterOffsetX;
    float jitterOffsetY;
    // Motion-vector scale: multiplies the raw MV texture to get pixel-space motion.
    // D3D12Velocity.kt already writes (uv - prevUv) * ScreenSize, i.e. pixels -> 1.0/1.0.
    float mvScaleX;
    float mvScaleY;
    // Pre-exposure: scales the rendering before it reaches DLSS. With a constant exposure
    // the convention is 1.0 = "already in display units", so keep 1.0.
    float preExposure;
    // Constant exposure scale, only read when exposureMode == MCDR_DLSS_EXPOSURE_NONE.
    // 1.0 means "the colour buffer is already exposed correctly".
    float exposureScale;
    uint32_t exposureMode;     // MCDR_DLSS_EXPOSURE_NONE (0) or _AUTO (1, reserved)
    uint32_t alphaUpscaling;   // 1 = upscale alpha too. False for the Minecraft world layer.
    // Markers (diagnostics only — the native side decides the real numbers, but the JVM
    // has the authoritative values, so they are reported back and cross-checked).
    float nearZ;               // camera near plane, from Projection.zNear()
    float farZ;                // camera far plane, from Projection.zFar()
    uint32_t depthInverted;    // 1 = reverse-Z (this renderer). Must stay 1.
    uint32_t reset;            // 1 = drop history this frame (first frame, resize, world switch)
    int32_t depthFormatOrdinal;  // GpuFormat ordinal of the depth attachment (diagnostics)
    int32_t _pad0;
    // --- the textures ---
    mcdr_object* pInColor;          // world layer, render size, colour (SRGB or UNORM)
    mcdr_object* pInDepth;          // main render target depth, render size, D32_FLOAT
    mcdr_object* pInMotionVectors;  // D3D12Velocity.kt output, render size, RG16_FLOAT
    mcdr_object* pInOutput;         // target-size colour target (the GUI's own target)
    int32_t _pad1;
    int32_t _pad2;   // trailing padding: keeps sizeof a multiple of the 8-byte alignment
} mcdr_dlss_input;

// Creates (or recreates, after a size change) the DLSS feature on the currently open
// command list. Call it once per (renderWidth, renderHeight, targetWidth, targetHeight)
// tuple, before the first mcdr_dx12EvaluateDlss for that tuple; the Java side can simply
// call it every frame and let the native side decide whether the descriptor really
// changed. On any failure this logs ONE warning naming the stage and the NGX result, and
// then returns 0 for the rest of the process — the caller keeps the existing path (linear
// blit) and must not retry. Returns 1 when a feature handle is ready.
MCDR_API int32_t MCDR_CALL mcdr_dx12CreateDlssFeature(mcdr_command_ctx* ctx,
                                                      const mcdr_dlss_input* in);

// Runs one DLSS Evaluate. Requires an open command list; the Java side calls it after the
// world has rendered and before the GUI, exactly where mcdr_dx12ApplySmaaToTexture used to
// be called (the two are mutually exclusive — see the anti-aliasing mode note above).
// Returns 1 when the evaluate really ran, 0 when it was skipped (mode != DLSS, a size
// changed without a successful create, or a previous failure disabled DLSS), -1 on
// invalid arguments. Never blocks, never fails the frame: a failure disables DLSS for the
// rest of the process after one WARN.
MCDR_API int32_t MCDR_CALL mcdr_dx12EvaluateDlss(mcdr_command_ctx* ctx,
                                                 const mcdr_dlss_input* in);

// One-line DLSS availability/state query for the UI and the log. `note` receives a
// NUL-terminated human-readable status (the reason when DLSS cannot run); NULL is fine.
// Return value:
//    1  DLSS can run: the NGX runtime is present, the adapter supports super sampling,
//       NGX is initialised and an optimal-setting query succeeded
//    0  not available (or no device yet); `note` says why
//   -1  invalid arguments
// Side-effect free apart from a one-time NGX init on first use; safe to call before the
// device exists (returns 0 with "device not created").
MCDR_API int32_t MCDR_CALL mcdr_dx12QueryDlss(char* note, int32_t noteCap);

// P108: tell the native layer WHERE the vendor runtime DLLs live, as a UTF-8 absolute
// directory. The Java side calls this once, early, with `<user.dir>\dxrmc\` — the very
// directory it extracted MCDR.dll and the vendor DLLs into — because that directory is
// NOT on the default DLL search path: the process working directory is the game directory
// while the executable is the JDK's java.exe, so neither the bare-name lookup nor the
// "next to the executable" lookup can find nvngx_dlss.dll. The native side then widens the
// process DLL search path (SetDefaultDllDirectories + SetDllDirectoryW + AddDllDirectory)
// before it loads the NGX runtime, and logs each of those results as a P108 line.
//
// This is the fix for the observed `ngx=no` / `ERROR_MOD_NOT_FOUND` report: the runtime
// DLL is shipped (external\dlss\bin\x64\nvngx_dlss.dll, staged into run\dxrmc\ by
// gradlew copyVendorDlls) and imports only system DLLs, so the failure was the search
// path, not a missing file or a missing redistribution dependency (NGX's core nvngx.dll
// is a driver component and is deliberately NOT shipped).
//
// Idempotent; safe before the device exists. Returns 1 when the directory was accepted and
// the search path widened, 0 when the input was rejected or empty.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssVendorDir(const char* utf8Dir);

// ---------------------------------------------------------------------------
// P271：超分（Super Resolution）抽象层的 v3 契约
// ---------------------------------------------------------------------------
// 为什么不复用 mcdr_dlss_input：那张表的布局（sizeof=112）被 MCDR_Layout.kt 与
// _port\abi_check.py 钉住，而且它的语义只服务 DLSS 一家。SR 层要同时容纳
// DLSS（Streamline）/ FSR（ffx_api）/ XeSS，因此把「渲染尺寸 + 显示尺寸 + jitter +
// MV 尺度 + 相机参数 + 若干可选资源」收敛成一张与厂商无关的载荷，厂商差异下沉到
// native 的 Provider（方案见仓库根 sr_plan.md 的 P272）。**纯追加**：老导出一个字节
// 都没动，P271 期间新入口的行为与老路径等价（DLSS 转发，FSR/XeSS 如实报不可用）。
//
// 调用时序（与既有 DLSS 路径同点：世界层画完、GUI 之前，命令列表开着）：
//   mcdr_dx12SrPick()      记录期望后端（只记录，不重建；重建发生在下一帧帧首）
//   mcdr_dx12SrQueryCaps() 查询某后端在本机的能力面（可在无命令列表时调用）
//   mcdr_dx12SrCreate()    建/唤醒后端（要求命令列表开着，与 mcdr_dx12CreateDlssFeature 同）
//   mcdr_dx12SrResize()    渲染/显示尺寸变化时通知
//   mcdr_dx12SrEvaluate()  逐帧提交
//   mcdr_dx12SrJitter()    该后端偏好的亚像素 jitter（0 = 无意见，沿用 D3D12Temporal）
//   mcdr_dx12SrDestroy()   释放（帧首、无打开命令列表时才可调用）
//   mcdr_dx12SrQuery()     一行可用性文本（日志与 UI）
//
// 资源指针一律是 **JVM 创建的纹理句柄**（Dx12Object*，即 D3D12GpuTexture.handle() 的值），
// 不是 ID3D12Resource*：实现单元自建的纹理会掉出模块的 resourceState 表（P108 控制组实测
// 读回全 0），解包只在模块侧做（P108 的 mv->GetDesc() 空 vtable 就是这么崩的）。
// 传 0 表示「这张资源没有」。

#define MCDR_SR_BACKEND_NONE 0
#define MCDR_SR_BACKEND_DLSS 1
#define MCDR_SR_BACKEND_FSR 2
#define MCDR_SR_BACKEND_XESS 3

// mcdr_sr_caps.takesMasks / mcdr_sr_dispatch 的掩码能力位。三家语义不同（SL 的
// kBufferTypeReactiveMaskHint、FFX 的 reactive/transparencyAndComposition、XeSS 的
// responsive pixel mask），这里是并集，具体怎么喂由 Provider 决定。
#define MCDR_SR_MASK_REACTIVE 1
#define MCDR_SR_MASK_RESPONSIVE 2

// 后端能力面。sizeof = 80、align = 4。Kotlin 侧镜像见 MCDR_Layout.kt 的 SR_CAPS。
// note 是给 UI 与日志的短原因（UTF-8，NUL 结尾），不是给机器判断的。
typedef struct mcdr_sr_caps {
    int32_t backend;         // 0  = MCDR_SR_BACKEND_*
    int32_t available;       // 4  1 = 本机此刻可用
    int32_t coupledDenoise;  // 8  1 = 该后端自带降噪（DLSS-D / FSR4 ML）
    int32_t needsExposure;   // 12 1 = 需要曝光纹理
    int32_t takesMasks;      // 16 MCDR_SR_MASK_* 位集
    int32_t externalHeap;    // 20 1 = 接受调用方提供的临时堆
    int32_t jitterPhaseCount;// 24 0 = 无意见（沿用 D3D12Temporal 的 8）
    int32_t minRenderWidth;  // 28
    int32_t minRenderHeight; // 32
    int32_t maxRenderWidth;  // 36
    int32_t maxRenderHeight; // 40
    int32_t nativeAA;        // 44 1 = 允许 1.0x（DLAA 式）
    int32_t reserved;        // 48 保留，必须为 0（给后续 flags 留位，不动 sizeof）
    char note[28];           // 52 短原因，NUL 结尾（52 + 28 = 80）
} mcdr_sr_caps;

// 与厂商无关的每帧载荷。sizeof = 136、align = 8（六个指针在 72..119，正好落在 8 边界）。
// Kotlin 侧镜像见 MCDR_Layout.kt 的 SR_DISPATCH。
//
// 约定（与 mcdr_dlss_input 一致，别改）：渲染尺寸是**渲染目标**尺寸，显示尺寸是**窗口**尺寸；
// jitter 是**渲染空间像素**、不归一化、不取负；motionVectorScale 在「MV 已是像素位移」时给 1.0。
// P275：`cameraAspectRatio` / `viewSpaceToMetersFactor` 是**可选**字段 —— 0 一律表示"未填"，
// 后端按各自的安全默认处理（aspect = displayWidth/displayHeight、meters = 1.0）。
// `cameraVerticalFov` 就是 FSR 的 `cameraFovAngleVertical`（弧度），DLSS 侧走 `mcdr_dlss_constants`
// 的同义字段，两处必须同源（都取自 `GameRenderer.getFov` 的实际值，不要用视场设置的标称值）。
typedef struct mcdr_sr_dispatch {
    int32_t renderWidth;          // 0
    int32_t renderHeight;         // 4
    int32_t displayWidth;         // 8
    int32_t displayHeight;        // 12
    float jitterOffsetX;          // 16
    float jitterOffsetY;          // 20
    float motionVectorScaleX;     // 24
    float motionVectorScaleY;     // 28
    float preExposure;            // 32
    float frameTimeDeltaMs;       // 36
    float cameraNear;             // 40
    float cameraFar;              // 44
    float cameraVerticalFov;      // 48 弧度
    float sharpness;              // 52 0 = 不锐化
    int32_t reset;                // 56 1 = 丢弃历史
    int32_t hdrInput;             // 60 1 = 输入是 HDR 线性色
    int32_t backend;              // 64 MCDR_SR_BACKEND_*（0 = 用 SrPick 选的那个）
    int32_t flags;                // 68 保留，必须为 0
    void* pColorInput;            // 72 渲染尺寸颜色
    void* pColorOutput;           // 80 显示尺寸颜色（UAV 写）
    void* pDepth;                 // 88 渲染尺寸深度
    void* pMotionVectors;         // 96 渲染尺寸 RG16F
    void* pReactiveMask;          // 104 R8，可 0
    void* pExposure;              // 112 R32F，可 0
    int32_t depthFormatOrdinal;   // 120 与 mcdr_dlss_input 的同名字段同义
    float cameraAspectRatio;      // 124 P275：显示尺寸的宽高比（FSR 可选、DLSS 常量里有同义字段）
    float viewSpaceToMetersFactor;// 128 P275：1.0 = 一个世界单位算 1 米；0 = 未填（后端按 1.0 处理）
    int32_t _pad2;                // 132（补到 136 = align 8 的整数倍）
} mcdr_sr_dispatch;

// 记录期望后端。纯记录：不建、不销毁、不重建（重建在下一帧帧首，见 P280）。
// 返回 1 = 已记录，0 = 后端编号未知，-1 = 参数非法。Never throws.
MCDR_API int32_t MCDR_CALL mcdr_dx12SrPick(int32_t backend);

// 查询后端能力面。out 必须非空；*out 会被整体覆盖（先清零再填）。
// 返回 1 = 已填（available 字段才是结论），-1 = 参数非法（含 backend 越界）。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrQueryCaps(int32_t backend, mcdr_sr_caps* out);

// 建/唤醒后端。与 mcdr_dx12CreateDlssFeature 同点调用：必须在命令列表打开时。
// maxRenderWidth/Height 是**上界**（热切换与 resize 时用来判要不要重建），displayWidth/Height
// 是本帧的窗口尺寸。返回 1 = 就绪（可以接着 Evaluate），0 = 本机不可用（已 log 一次），
// -1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrCreate(mcdr_command_ctx* ctx, int32_t backend,
                                              int32_t maxRenderWidth, int32_t maxRenderHeight,
                                              int32_t displayWidth, int32_t displayHeight);

// 尺寸变化通知。返回 1 = 已接受，0 = 后端未就绪（忽略），-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrResize(mcdr_command_ctx* ctx, int32_t renderWidth,
                                              int32_t renderHeight, int32_t displayWidth,
                                              int32_t displayHeight);

// 逐帧提交。返回 1 = 已执行，0 = 跳过（后端未就绪 / 本帧不适用），-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrEvaluate(mcdr_command_ctx* ctx, const mcdr_sr_dispatch* desc);

// 该后端偏好的亚像素 jitter。返回 1 = 已写 outX/outY，0 = 无意见（沿用 D3D12Temporal 的
// Halton(2,3)，这时 outX/outY 不被修改），-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrJitter(int32_t frameIndex, float* outX, float* outY);

// P274：该后端期望的 jitter 相位长度（0 = 不适用）。参数非法返回 -1。
// DLSS / XeSS 报 8；FSR 报 `8 * (displayWidth / renderWidth)^2` 并夹到 [1, 256]
// （P277 接了真后端之后改用 `ffxQueryDescUpscaleGetJitterPhaseCount` 问运行库）。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrJitterPhaseCount(int32_t backend, int32_t renderWidth,
                                                      int32_t displayWidth);

// 释放后端资源。只允许在没有打开命令列表时调用（帧首）；重复调用安全。
MCDR_API void MCDR_CALL mcdr_dx12SrDestroy(void);

// 一行可用性文本：当前/期望后端、各自 available、最近一次失败原因。
// 返回 1 = 已写（buf 为 NUL 结尾），0 = 没有内容，-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12SrQuery(char* note, int32_t noteCap);

// ---------------------------------------------------------------------------
// P293：帧生成（Frame Generation）抽象层 —— ABI v4
//
// 与 SR 段同构：一张能力表 + 一份逐帧载荷 + 5 个入口。区别在于**没有 Evaluate**：
// 帧生成的工作发生在 Present 的两侧（DLSS-G 自己插在 SL 的呈现路径里），不由我们的
// 命令列表录制，所以这里没有"提交一帧"的入口，只有"设期望值"与"问上次真提交了几帧"。
//
// 三家的形态差异（详见 fg_plan.md §2.2）：
//   - DLSS-G（Streamline）：占走 Present，链必须是 SL 代理工厂产出的；必须 Reflex；
//     HUDless 复用 SR 的输出（v1 常态，不需要外部喂）。
//   - FSR-FG / XeSS-FG：**接管交换链**（Wrap/New 或 InitFromSwapChain），与 SL 代理链
//     二选一。v1 只有编号与能力位，不实现（见 fg_plan.md 的 v1 范围裁剪）。
// ---------------------------------------------------------------------------

#define MCDR_FG_BACKEND_NONE 0
#define MCDR_FG_BACKEND_DLSSG 1
#define MCDR_FG_BACKEND_FSRFG 2
#define MCDR_FG_BACKEND_XESSFG 3

// 呈现链当前归谁（可查询状态，不再是隐含的 if）。v1 里只有前两态可达。
#define MCDR_FG_CHAIN_APP_NATIVE 0
#define MCDR_FG_CHAIN_SL_PROXY 1
#define MCDR_FG_CHAIN_FSR_WRAP 2  // 占位：v1 不实现
#define MCDR_FG_CHAIN_XESS_WRAP 3 // 占位：v1 不实现

// 本后端架构上对应的低延迟方案（P298；能力面，不是「运行时可用」）。
// 现实核对（2026-10-27）：Reflex 现役且 FG 依赖它；Anti-Lag 2 只有头文件（`external\fidelityfx`
// 下两份 `ffx_antilag2_dx12.h`），全仓没有任何 antilag DLL；XeLL 的 DLL 在
// （`external\xess\bin\libxell.dll`）但契约没接。所以后两者的位**恒为 0 / 原因写清**。
#define MCDR_FG_LATENCY_NONE 0
#define MCDR_FG_LATENCY_REFLEX 1
#define MCDR_FG_LATENCY_ANTILAG2 2
#define MCDR_FG_LATENCY_XELL 3

// 后端能力面。sizeof = 80、align = 4。Kotlin 侧镜像见 MCDR_Layout.kt 的 FG_CAPS。
// 与 mcdr_sr_caps 一样：note 是给 UI 与日志的短原因（UTF-8，NUL 结尾），机器判断只看数值。
typedef struct mcdr_fg_caps {
    int32_t backend;            // 0  MCDR_FG_BACKEND_*
    int32_t available;          // 4  1 = 本机此刻可启用（唯一结论位）
    int32_t runtimeLoaded;      // 8  运行库已加载（头文件在 ≠ 这个）
    int32_t deviceSupported;    // 12 设备/驱动支持
    int32_t chainCompatible;    // 16 当前呈现链兼容
    int32_t needsReflex;        // 20 1 = 必须 Reflex 开（DLSS-G 恒 1）
    int32_t needsUiResource;    // 24 1 = 需要独立 UI 资源（FSR/XeSS）
    int32_t needsHudless;       // 28 1 = 需要独立 HUDless（FSR/XeSS）
    int32_t transitionsInputs;  // 32 1 = provider 自己转输入输出屏障
    int32_t maxGeneratedFrames; // 36 1..N（DLSS-G 当前 5）
    int32_t chainOwner;         // 40 MCDR_FG_CHAIN_*（当前实际状态，不是期望）
    int32_t reserved;           // 44 保留，必须为 0
    char note[28];              // 48 短原因，NUL 结尾（48 + 28 = 76）
    // P298：本后端**架构上**该配哪个低延迟方案。注意这是「该用哪个」而不是「能不能用 ——
    // 能不能用看 available / note（原稿 §1.3 的要求：不许把架构上可表达说成运行时已支持）。
    // 值取 MCDR_FG_LATENCY_*。v4 就地扩这一格（v4 未发布，80 B / align 4 不变）。
    int32_t latencyProvider;    // 76 MCDR_FG_LATENCY_*
} mcdr_fg_caps;

// 与厂商无关的"设期望值"载荷。sizeof = 96、align = 8。
// Kotlin 侧镜像见 MCDR_Layout.kt 的 FG_DISPATCH。
//
// 语义约定（别改）：
//   - mode 与老的 mcdr_dx12SetDlssgMode 完全一致：0=off 1=固定倍率 2=dynamic；
//     numFrames 只在 mode=1 时有意义，mode=2 由 provider 自己算（P225/P226）。
//   - 三轨帧号（fg_plan.md §3.5）：appFrameId 是应用帧、backendFrameId 是给后端用
//     （严格 +1）、presentId 是给 present 用（严格 +1）。三者不同源，别混用。
//   - 资源指针仍然是**JVM 创建的纹理句柄**，与 SR 段一致；0 表示"这张没有"。
//   - pHudlessColor 给 0 是 v1 的常态：DLSS-G 复用 SR 的输出当 HUDless（P213），
//     只有 FSR/XeSS 才需要外部喂。
/* P305z：帧常量（`xess_fg_developer_guide_english.md:678-681` 把 View/Projection 列为必需输入）。
 * 归属 FG 抽象层自己 —— 不去翻 DLSS 的 mcdr_dlss_constants，那是另一条通路的私有状态。
 * 矩阵按行主序（与 XeSS-FG 的 `xefg_swapchain_frame_constant_data_t` 一致），JVM 侧直接交。 */
typedef struct mcdr_fg_constants
{
    float view[16];
    float projection[16];
    float jitterX;
    float jitterY;
    float mvScaleX;
    float mvScaleY;
    int32_t resetHistory;
    float frameRenderTime;
} mcdr_fg_constants;
typedef struct mcdr_fg_dispatch {
    int32_t displayWidth;    // 0
    int32_t displayHeight;   // 4
    int32_t backend;         // 8  MCDR_FG_BACKEND_*（0 = 用 FgPick 记下的那个）
    int32_t mode;            // 12 0/1/2
    int32_t numFrames;       // 16 仅 mode=1 有意义
    int32_t reset;           // 20 1 = 丢弃历史
    uint64_t appFrameId;     // 24
    uint64_t backendFrameId; // 32
    uint64_t presentId;      // 40
    void* pBackbuffer;       // 48
    void* pHudlessColor;     // 56 可 0
    void* pDepth;            // 64
    void* pMotionVectors;    // 72
    void* pUiColor;          // 80 可 0
    int32_t _pad2[2];        // 88 / 92：align 8 要显式填满（只留一个 int32 会多出 4 B 尾填充）
} mcdr_fg_dispatch;

// 记录期望后端。纯记录：不建、不销毁（与 SrPick 一致）。返回 1 = 已记录，0 = 未知编号。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgPick(int32_t backend);

// 查询后端能力面。out 必须非空；*out 整体覆盖（先清零再填）。返回 1 = 已填，-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgQueryCaps(int32_t backend, mcdr_fg_caps* out);

// 设期望值（mode/numFrames/帧号/资源）。**只记录，不立即下发**：真正的
// slDLSSGSetOptions 在渲染线程的下一次 present 前下发（P296），这样跨线程调用不会
// 落到不确定的那次 Present 上（SL 官方文档 §6 明确说了这一点）。
// 返回 1 = 已记录 0 = 后端不可用（原因进 FgStatus）-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgSetMode(const mcdr_fg_dispatch* desc);

// 上一次 Present 真正提交的帧数（1 = 没插帧，与 mcdr_dx12GetDlssgPresented 同义）。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgPresented(void);
MCDR_API void MCDR_CALL mcdr_dx12FgSetConstants(const mcdr_fg_constants* constants);

// 一行状态文本：期望/生效后端、可用性、链归属、最近一次失败原因。
// 返回 1 = 已写（NUL 结尾），0 = 没有内容，-1 = 参数非法。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgStatus(char* note, int32_t noteCap);

// P300：帧生成的状态，压进一个 int32（位布局就是契约，JVM 侧按位读，不要解析文本）。
//   [ 0.. 7] desiredMode   0=关 1=开（固定倍率） 2=dynamic —— **native 侧的期望值**
//   [ 8..15] chainMode     链是在哪个 mode 下建成的（0=关的链 1=开的链）
//   [16..23] active        1 = 现在有一个活着的后端（0 = 没有后端，也就没有插帧）
//   [24..31] rebuilds      min(重建次数, 255)
// 为什么需要它：P300 的回滚会在 native 侧把 desiredMode 清成 0（例如 SR 后端从 DLSS 换成
// FSR/XeSS 之后 DLSS-G 再也拿不到合法输入），而 JVM 的 options.txt 仍然写着「开」—— 只看
// options.txt 的界面就会说假话。返回 -1 不会发生（这是纯读）。
MCDR_API int32_t MCDR_CALL mcdr_dx12FgState(void);

/* P305c：把 GUI 合成目标交给帧生成后端当「UI 纹理」用（用户 2026-10-27 裁决）。
 *
 * 包装型后端（XeSS-FG / FSR-FG）要把「无 HUD 的世界层」与「UI」分开插值，否则准星/文字会被
 * 插帧拉扯。我们的 GUI 合成进 D3D12GuiSplit 的窗口尺寸目标，没有独立 UI 纹理，所以直接把那张
 * 目标当 UI 纹理交出去（XeSS 用 `XEFG_SWAPCHAIN_UI_MODE_BACKTEXTURE_UITEXTURE`，FSR 用
 * `RegisterUiResourceDX12`）。
 *
 * `object` 是 `Dx12Object*` 句柄（就是 `D3D12GpuTexture.handle()` 那个值），传 0 = 撤销。
 * **只在 GPU 目标创建/重建时推一次**，不要每帧推。 */
MCDR_API void MCDR_CALL mcdr_dx12FgSetUiTexture(void* object);

/* P306ac：hudless 呈现开关，以及"UI 会由 SDK 合成"的可查询状态。
 *
 * `mcdr_dx12FgSetUiComposition(1)` 的含义：应用接下来 Present 的 backbuffer **只含世界层**
 *（不含 GUI），UI 单独作为一张"透明底 + 预乘 alpha"的纹理交给后端合成 —— 那张纹理就是
 * `mcdr_dx12FgSetUiTexture` 推过来的句柄。0 = 老路径：应用自己把 GUI 画进 backbuffer，
 * 后端**不得**再注册 UI（否则同一份 HUD 会被合成两遍，P306aa）。
 *
 * `mcdr_dx12FgUiCompositionReady()` 返回 1 时，调用方才敢 Present 一张不含 UI 的图；0 表示
 * "没有人替我们合成 UI"，必须继续 Present 合成层。它**不保证**这一帧 UI 已经登记（登记发生在
 * 同一个 Present 之前），所以调用方要用它做**每帧一次**的 latch，不要在帧中途换模式。
 *
 * P306ad 起判据收紧成"**链已包 + UI 纹理已交过去 + 已经发生过一次插帧 dispatch**"：只包链不算数，
 * 因为包链与启用插帧是两步 —— 没有逐帧输入（没开超分 / 倍率还是 100%）时 provider 只是建了上下文，
 * 代理链一次 Present 都没跑过。那种状态下若 Present 世界层，UI 注册落在一条没人 Present 的链上，
 * 界面会整层消失（2026-10-28 19:26 实测）。
 *
 * 为什么由 JVM 决定：只有它知道"这一帧有没有 3D 世界层"。菜单 / 加载屏没有可插帧的世界层，
 * 应当直通（合成层呈现），而不是让 SDK 对着一张纯 UI 图插帧。 */
MCDR_API void MCDR_CALL mcdr_dx12FgSetUiComposition(int32_t enabled);
MCDR_API int32_t MCDR_CALL mcdr_dx12FgUiCompositionReady(void);

/* P305ag：native **内部**调用口，不是给 JVM 的（JVM 不要绑这个符号）。
 *
 * 为什么需要它：逐帧 tag 原来只有 `srEvaluateFrame` 成功后一个触发点，而 `d3d12_dlss.cpp`
 * 是普通翻译单元（故意不 import 设备模块，保持 DLSS 那条路的隔离），DLSS 超分路径因此
 * 交不出本帧输入 —— `sr.backend = DLSS` 时 XeSS-FG 一帧都拿不到（实机：4561 帧 tagCalls=0）。
 *
 * 参数全是 POD（命令列表 + 三个资源 + 各自**当前**状态），实现见 `abi_framegen.cpp`；
 * 状态值就是 `D3D12_RESOURCE_STATES` 的整数形式。 */
MCDR_API void MCDR_CALL mcdr_fgTagDlssInputs(void* cmdList, void* color, int32_t colorState,
                                             void* depth, int32_t depthState,
                                             void* motionVectors, int32_t motionVectorState);

// ---------------------------------------------------------------------------
// P112：DLSS 的每帧相机常量（Streamline 的 sl::Constants 逐字段对应；
//       ProgrammingGuide.md §2.11.1 / ProgrammingGuideDLSS.md §6.0）
// ---------------------------------------------------------------------------
// 为什么不塞进 mcdr_dlss_input：那个结构的布局（sizeof=112）被 MCDR_Layout.kt 与
// abi_check.py 钉住，加字段会破坏既有 ABI。这里用**新导出 + 新结构**增量扩展：
// 老 DLL 没有该导出时，JVM 侧按 `hasDx12SetDlssConstants` 特性探测跳过。
// 矩阵全部 **行主序** 且 **不含 jitter**（jitter 单独给，文档明确要求）。
// P112/P118/P119/P120：Streamline `sl::Constants` 的数据源（每帧由 JVM 经 `mcdr_dx12SetDlssConstants` 推来）。
//
// 矩阵约定（三条都是踩过的坑）：
//   1. 三张矩阵都是**列主序**（JOML `Matrix4f.get(float[])` 的原生布局）；native 在喂给
//      `sl::Constants`（其 float4x4 要求行主序）之前必须逐元素转置一次。
//   2. `viewProjection`/`prevViewProjection` 是 world->clip，且**已经烘进相机平移**
//      （`V_world = V_rot * T(-camPos)`）。凭这两张派生的 `clipToPrevClip` 才含相机走动。
//   3. `projection` 是 view->clip 的投影矩阵本身，`sl::Constants::cameraViewToClip` 要的是它，
//      不是 world->clip 的 VP —— 塞 VP 会让 SL 的反投影多带一层世界平移。
//
// `motionVectors3D` 必须是 0：本项目 MV 是屏幕空间像素位移，不是 3D 世界速度。
typedef struct mcdr_dlss_constants {
    float viewProjection[16];
    float prevViewProjection[16];
    float jitterOffsetX;
    float jitterOffsetY;
    float mvecScaleX;
    float mvecScaleY;
    float cameraPosX;
    float cameraPosY;
    float cameraPosZ;
    float _pad0;
    float cameraUpX;
    float cameraUpY;
    float cameraUpZ;
    float _pad1;
    float cameraRightX;
    float cameraRightY;
    float cameraRightZ;
    float _pad2;
    float cameraFwdX;
    float cameraFwdY;
    float cameraFwdZ;
    float _pad3;
    float cameraNear;
    float cameraFar;
    float cameraFovRadians;
    float cameraAspectRatio;
    uint32_t depthInverted;
    uint32_t cameraMotionIncluded;
    uint32_t motionVectors3D;
    uint32_t reset;
    uint32_t renderingGameFrames;
    uint32_t motionVectorsJittered;
    float projection[16];
} mcdr_dlss_constants;

// 每帧一次、DLSS 评价之前调用；返回 1 已缓存，0 参数无效。幂等，设备创建前也可调用。
MCDR_API int32_t MCDR_CALL mcdr_dx12SetDlssConstants(const mcdr_dlss_constants* constants);

// P117：PCL stats —— 帧开始（建本帧 token + eSimulationStart）与通用标记（eSimulationEnd=1 /
// eRenderSubmitStart=2 等，取值见 sl::PCLMarker）。
MCDR_API void MCDR_CALL mcdr_dx12ReflexFrameBegin(void);
MCDR_API void MCDR_CALL mcdr_dx12ReflexMarker(int32_t marker);

// P122：NVIDIA Reflex 模式（0=off 1=on 2=on+boost）与可用性（1=可用）。
MCDR_API void MCDR_CALL mcdr_dx12SetReflexMode(int32_t mode);
MCDR_API int32_t MCDR_CALL mcdr_dx12ReflexAvailable(void);
// P208: 1 = DLSS 可用（Streamline 的 DLSS 插件已加载，或非 Streamline 路径未探测到失败）。
// 0 = 明确探测到 Streamline 的 DLSS 插件没有初始化（NGX 版本不匹配时），界面据此置灰。
MCDR_API int32_t MCDR_CALL mcdr_dx12DlssAvailable(void);

// P131：Reflex 自己的限帧器（微秒/帧，0 = 关闭）。配置项 [dlss] reflexFrameLimitUs 的唯一入口。
MCDR_API void MCDR_CALL mcdr_dx12SetReflexFrameLimit(int32_t micros);

// ---------------------------------------------------------------------------
// Device and resources  (was jni_bridge.cpp — 13 exports)
// ---------------------------------------------------------------------------

// Writes a human-readable device/adapter summary into `buf`.
MCDR_API int32_t MCDR_CALL mcdr_dx12CreateDevice(char* buf, int32_t cap);

// P31：释放设备上下文中的全部 D3D12/DXGI 对象（device / queue / 描述符堆 /
// DXGI adapter）。**必须在进程退出前调用**：这些 ComPtr 若留到
// DLL_PROCESS_DETACH 才析构，Release() 会在 dxgi.dll 已经 detach 之后执行，
// 产生原生异常并以异常码 2170 作为进程退出码。幂等，重复调用安全。
MCDR_API void MCDR_CALL mcdr_dx12DestroyDevice(void);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateTexture(int32_t usage, int32_t format, int32_t width,
                                                       int32_t height, int32_t depthOrLayers,
                                                       int32_t mipLevels);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateBuffer(int32_t usage, int64_t size);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateSampler(int32_t addressU, int32_t addressV, int32_t minFilter,
                                                       int32_t magFilter, int32_t maxAnisotropy,
                                                       float maxLod);

// P220：带显式 mip LOD bias 的采样器。只给**地形图集**（vanilla LevelRenderer 的
// chunkLayerSampler）用；bias 由超分比例决定（log2(render/display) - 0.5），不超分时调用方
// 必须传 0，保持与 mcdr_dx12CreateSampler 完全一致。合法区间 [-16, 15.99]，越界由 native 夹取。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateSamplerWithMipBias(
    int32_t addressU, int32_t addressV, int32_t minFilter, int32_t magFilter, int32_t maxAnisotropy,
    float maxLod, float mipLodBias);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateTextureView(mcdr_object* texture, int32_t baseMipLevel,
                                                           int32_t mipLevels);

MCDR_API void MCDR_CALL mcdr_dx12DestroyResource(mcdr_object* object);

// Returns the CPU-visible base address of a mapped buffer, or NULL on failure.
// `outLength` receives the number of bytes actually mapped.
MCDR_API void* MCDR_CALL mcdr_dx12MapBuffer(mcdr_object* buffer, int64_t offset, int64_t length, int32_t read,
                                            int32_t write, int64_t* outLength);

MCDR_API void MCDR_CALL mcdr_dx12UnmapBuffer(mcdr_object* buffer);

// Writes a JSON array describing every adapter into `buf`.
MCDR_API int32_t MCDR_CALL mcdr_dx12EnumerateAdapters(char* buf, int32_t cap);

// Writes a flat, single-line JSON object describing the adapter that the ACTIVE
// device is bound to. Every key is always present:
//   name / vendor / vendor_id / device_id / subsys_id / revision /
//   feature_level / feature_level_value / driver_version / driver_description /
//   device_type / vram_bytes / shared_bytes / dedicated_system_bytes / luid /
//   is_hardware / is_detachable / dxcore
// `vendor` is mapped through the same vendor table vanilla uses in
// VulkanPhysicalDevice.java. `driver_version`, `driver_description`,
// `is_hardware` and `is_detachable` come from DXCore, which is loaded at run
// time (this DLL has no link-time dependency on dxcore.dll); when DXCore is
// unavailable those fields fall back to ""/false and `dxcore` is false.
//
// Same buffer convention as mcdr_dx12EnumerateAdapters: buf == nullptr or
// cap <= 0 returns the required byte count INCLUDING the terminating NUL;
// otherwise a NUL-terminated UTF-8 string is written and the number of bytes
// written EXCLUDING the NUL is returned. Returns 0 when there is no active
// device (not created yet, or already destroyed).
MCDR_API int32_t MCDR_CALL mcdr_dx12GetAdapterInfo(char* buf, int32_t cap);

MCDR_API uint64_t MCDR_CALL mcdr_dx12GetQueueHandle(void);
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetDeviceHandle(void);

MCDR_API uint64_t MCDR_CALL mcdr_dx12CreateHiddenWindow(int32_t width, int32_t height);
MCDR_API void MCDR_CALL mcdr_dx12DestroyHiddenWindow(uint64_t hwnd);

// ---------------------------------------------------------------------------
// Command layer  (was jni_bridge_p3.cpp — 36 exports, +1 for MC 26.2 clearStencilTexture)
// ---------------------------------------------------------------------------

MCDR_API int64_t MCDR_CALL MCDR_D3D12_get_timestamp_frequency(void);

MCDR_API mcdr_command_ctx* MCDR_CALL MCDR_D3D12_create_command_encoder(void);
MCDR_API void MCDR_CALL mcdr_dx12DestroyCommandEncoder(mcdr_command_ctx* ctx);
MCDR_API void MCDR_CALL mcdr_dx12BeginCommandList(mcdr_command_ctx* ctx);
MCDR_API void MCDR_CALL mcdr_dx12BeginCommandListWithWait(mcdr_command_ctx* ctx, uint64_t waitForValue);
MCDR_API void MCDR_CALL mcdr_dx12EndCommandList(mcdr_command_ctx* ctx);
// Fence 语义（per-command-ctx，BUG-03）：mcdr_dx12GetFenceValue 返回该 ctx 的
// per-command-list 序号，mcdr_dx12WaitForFence 只等待该 ctx 自己的提交，
// mcdr_dx12Submit 返回的也是同一条时间线上的序号。因此帧 encoder 的 fence token
// 不会被一次性 encoder（createBuffer(data) 的临时上传）提前满足。
MCDR_API uint64_t MCDR_CALL mcdr_dx12Submit(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12WaitForFence(mcdr_command_ctx* ctx, uint64_t value, int64_t timeoutNs);
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetFenceValue(mcdr_command_ctx* ctx);
MCDR_API int64_t MCDR_CALL mcdr_dx12GetTimestampNow(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12IsListOpen(mcdr_command_ctx* ctx);

MCDR_API void MCDR_CALL mcdr_dx12CopyBuffer(mcdr_command_ctx* ctx, mcdr_object* src, int64_t srcOffset,
                                            mcdr_object* dst, int64_t dstOffset, int64_t size);

MCDR_API void MCDR_CALL mcdr_dx12WriteToTexture(mcdr_command_ctx* ctx, mcdr_object* stagingBuffer,
                                                int64_t stagingOffset, int32_t width, int32_t height,
                                                mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                int32_t dstX, int32_t dstY);

MCDR_API void MCDR_CALL mcdr_dx12CopyBufferToTexture(mcdr_command_ctx* ctx, mcdr_object* srcBuffer,
                                                     int64_t srcOffset, int32_t srcWidth, int32_t srcHeight,
                                                     mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                     int32_t dstX, int32_t dstY, int32_t width,
                                                     int32_t height);

MCDR_API void MCDR_CALL mcdr_dx12CopyTextureToBuffer(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
                                                     int32_t mip, int32_t layer, int32_t srcX, int32_t srcY,
                                                     int32_t width, int32_t height, mcdr_object* dstBuffer,
                                                     int64_t dstOffset);

MCDR_API void MCDR_CALL mcdr_dx12CopyTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
                                                      mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                      int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY,
                                                      int32_t width, int32_t height);

MCDR_API void MCDR_CALL mcdr_dx12ClearColorTexture(mcdr_command_ctx* ctx, mcdr_object* texture, float r,
                                                   float g, float b, float a);
MCDR_API void MCDR_CALL mcdr_dx12ClearDepthTexture(mcdr_command_ctx* ctx, mcdr_object* texture, double depth);
MCDR_API void MCDR_CALL mcdr_dx12ClearColorTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture, float r,
                                                         float g, float b, float a, int32_t x, int32_t y,
                                                         int32_t width, int32_t height);
MCDR_API void MCDR_CALL mcdr_dx12ClearDepthTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture,
                                                         double depth, int32_t x, int32_t y, int32_t width,
                                                         int32_t height);

// MC 26.2 addition: CommandEncoderBackend::clearStencilTexture(GpuTexture, int).
// Clears only the stencil plane to `value`; the depth plane is untouched.
MCDR_API int32_t MCDR_CALL mcdr_dx12ClearStencilTexture(mcdr_command_ctx* ctx, mcdr_object* texture,
                                                        int32_t value);

// P104b: apply SMAA to `texture` in place, on the open command list. Three full-screen
// passes (edge detection, blend weights, neighbourhood blending) run over the texture,
// then the result is copied back into it and the texture is left in RENDER_TARGET so a
// render pass can still use it as an attachment. Call it once the world has finished
// rendering and before the GUI is drawn — that keeps the anti-aliasing on the world layer
// and leaves UI text and panels sharp (running it on the final blit softened the GUI too).
// No-op when the anti-aliasing mode is not SMAA; a failure disables SMAA and logs an ERROR.
// Returns 1 when the three passes really ran this frame, 0 when they were skipped (mode is not
// SMAA, the texture is not a single-sampled 2D texture, or an earlier failure disabled SMAA).
MCDR_API int32_t MCDR_CALL mcdr_dx12ApplySmaaToTexture(mcdr_command_ctx* ctx, mcdr_object* texture);

// P106: draw `src` onto `dst` (a full-screen triangle with a linear sampler, so the two may
// differ in size) and leave `dst` in RENDER_TARGET. The GUI's own window-sized target uses
// this to take over the world layer, which lives in a scaled main render target whenever
// renderScale is not 100%. A 1:1 copyTextureToTexture cannot do it: the scale factor is not 1.
MCDR_API int32_t MCDR_CALL mcdr_dx12BlitTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* src,
                                                         mcdr_object* dst);
// P121：后置锐化（TAA / DLAA / DLSS 共用）。src -> dst 的 5-tap unsharp，amount 取 [0,1]；
// src == dst 会被拒绝（返回 0），调用方需准备第二张同尺寸纹理。
MCDR_API int32_t MCDR_CALL mcdr_dx12SharpenTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* src,
                                                        mcdr_object* dst, float amount);

// P152: TAA resolve, native side. Same call point as mcdr_dx12ApplySmaaToTexture (world drawn,
// GUI not yet), but the cross-frame state lives here: two RGBA16F history textures (ping-pong),
// the resolve pipeline and the jitter/history bookkeeping are owned by the device context, so
// the JVM only pushes per-frame values. Two passes: scene + history[read] + motion ->
// history[write], then a blit (or a 5-tap sharpen when sharpenAmount > 0) back into `scene`,
// which is left in RENDER_TARGET so the GUI pass can keep using it as an attachment.
//
// `motion` may be a texture view (the velocity target is handed over as one) — the native side
// unwraps it. The jitter values are in pixels and are expected to be already applied to the
// projection matrix; `reset` != 0 (first frame, teleport, resize, history-generation change)
// makes this frame bypass the blend. `blend` is the weight of the current frame (1/16 is the
// usual value); `sharpenAmount` <= 0 selects the plain blit.
//
// Returns 1 when TAA really ran this frame, 0 when it was skipped (mode is not TAA, the textures
// are unsuitable, or an earlier failure disabled it) and -1 when it failed — a failure disables
// TAA and logs one ERROR. A no-op when the anti-aliasing mode is not TAA.
MCDR_API int32_t MCDR_CALL mcdr_dx12ApplyTaaToTexture(mcdr_command_ctx* ctx, mcdr_object* scene,
                                                     mcdr_object* motion, float jitterX, float jitterY,
                                                     float prevJitterX, float prevJitterY, float blend,
                                                     int32_t reset, float sharpenAmount);

// `colorTextures` may contain NULL entries (unused attachments); they are
// skipped. `colorMips` is the per-attachment RTV mip slice. `colorClearFlags`
// is 0 = load, 1 = clear. `clearColors` holds 4 floats per attachment.
MCDR_API void MCDR_CALL mcdr_dx12BeginRenderPass(mcdr_command_ctx* ctx, mcdr_object* const* colorTextures,
                                                 int32_t colorCount, const int32_t* colorMips,
                                                 const int32_t* colorClearFlags, const float* clearColors,
                                                 mcdr_object* depthTexture, int32_t depthMip,
                                                 int32_t depthClearFlag, double depthClearValue, int32_t x,
                                                 int32_t y, int32_t width, int32_t height);

MCDR_API void MCDR_CALL mcdr_dx12EndRenderPass(mcdr_command_ctx* ctx);
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12GetActiveColorTexture(mcdr_command_ctx* ctx);

MCDR_API mcdr_query_pool* MCDR_CALL mcdr_dx12CreateQueryPool(int32_t size);
MCDR_API void MCDR_CALL mcdr_dx12DestroyQueryPool(mcdr_query_pool* pool);
MCDR_API void MCDR_CALL mcdr_dx12WriteTimestamp(mcdr_command_ctx* ctx, mcdr_query_pool* pool, int32_t index);
MCDR_API uint64_t MCDR_CALL mcdr_dx12ReadQueryValue(mcdr_query_pool* pool, int32_t index);
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadQueryValues(mcdr_query_pool* pool, int32_t start, int32_t count,
                                                    int64_t* out);

// Async renderer control channel.
MCDR_API void MCDR_CALL mcdr_dx12InitAsyncRenderer(int32_t workerCount);
MCDR_API void MCDR_CALL mcdr_dx12DestroyAsyncRenderer(void);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderBeginFrame(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitComplete(mcdr_command_ctx* ctx, int64_t timeoutMs);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderIsRecordingReady(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitRecordingReady(mcdr_command_ctx* ctx, int64_t timeoutMs);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncSendCommandsReady(mcdr_command_ctx* ctx);
MCDR_API void MCDR_CALL mcdr_dx12ClearAsyncRenderCtx(void);

// ---------------------------------------------------------------------------
// Pipeline  (was jni_bridge_p4.cpp — 2 exports)
// ---------------------------------------------------------------------------

MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateGraphicsPipeline(const mcdr_pipeline_desc* desc);
MCDR_API void MCDR_CALL mcdr_dx12DestroyPipeline(mcdr_pipeline* pipeline);

// ---------------------------------------------------------------------------
// Compute (P234: 最小通路 —— 着色率图像 / NRC 的硬前置)
//
// 本模块没有 D3D12_COMMAND_LIST_TYPE_COMPUTE：compute 走普通的 DIRECT 列表
// （SetPipelineState(compute PSO) + SetComputeRootSignature + Dispatch），
// 这也是"同一条列表里既录制图形又录制 compute"的前提。
//
// 根签名是固定的：
//   [0] descriptor table (1×UAV, u0) —— 目标纹理，创建时必须带 usage 位 32
//       （ALLOW_UNORDERED_ACCESS）。**必须是描述符表**：root descriptor 的 UAV 只
//       支持 raw / structured buffer，typed UAV（RWTexture2D / R8_UINT 的 SRI）
//       挂 root UAV 会让 CreateComputePipelineState 返回 E_INVALIDARG（实测）。
//       描述符堆由模块自己懒建（64 槽 ring），调用方不用也拿不到堆。
//   [1] root constants   (b0) —— 至多 8 个 int32
// 因此调用方只需要「DXIL 字节码」和「一张带 UAV 位的纹理」两样东西；将来接 SRI 时
// 换成 R8_UINT 纹理即可，签名不用动。
// ---------------------------------------------------------------------------

// 从 DXIL 建 compute PSO。DXIL 由 mcdr_dx12CompileShaderDxc(..., "cs_6_x", ...) 产出
// （cs_6_8 起支持 Wave 内在函数；本机实测 6.9 可用）。失败返回 NULL，原因见 mcdrLastError()。
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipeline(const void* dxil, int32_t dxilSize);

// 在已打开的命令列表上派发一次。内部把目标纹理 COMMON → UNORDERED_ACCESS、
// Dispatch、再转回 COMMON（跨列表的后续拷贝因此仍按 COMMON 处理）。
// 返回 1=已派发 0=参数或资源状态不对（会写一条 ERROR 说明是哪一条） -2=没有打开的命令列表。
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchCompute(mcdr_command_ctx* ctx, mcdr_object* pipeline,
                                                     mcdr_object* uavTexture, const int32_t* constants,
                                                     int32_t constantCount, int32_t x, int32_t y, int32_t z);

// ---------------------------------------------------------------------------
// Mesh shaders (P72: amplification/mesh pipeline + HLSL->DXIL compilation)
//
// Mesh shaders are unreachable through D3DCompile (SM 5.1 ceiling), so the
// native side compiles them with DXC, loaded dynamically from dxcompiler.dll
// next to the library or from the installed Windows SDK.
// ---------------------------------------------------------------------------

// Compiles one HLSL entry point to DXIL. Returns the bytecode size in bytes, or
// 0 on failure (reason via mcdrLastError()). `profile` is e.g. "as_6_6" /
// "ms_6_6" / "ps_6_0"; an empty `entry` means "main". Pass (NULL, 0) -- or a
// buffer smaller than the result -- to learn the required size first and then
// call again with a buffer of that size.
MCDR_API int32_t MCDR_CALL mcdr_dx12CompileShaderDxc(const char* source, int32_t sourceLen,
                                                     const char* entry, const char* profile, void* outDxil,
                                                     int32_t outCapacity);

// P124（mesh 引入 Step 1）：能力与原因的统一查询 —— 与 UI 置灰的判据同源。
// 返回 1=支持 0=不支持；outReason 写入 UTF-8 原因（可为 NULL/capacity=0，只查能力）。
MCDR_API int32_t MCDR_CALL mcdr_dx12QueryMeshSupport(char* outReason, int32_t capacity);

// Creates an amplification/mesh pipeline from DXIL bytecode. Returns NULL and
// sets the last error when the device does not support mesh shaders (tier 0).
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateMeshPipeline(const mcdr_mesh_pipeline_desc* desc);

// 与 mcdr_dx12CreateMeshPipeline 相同，但复用调用方提供的根签名
// （ID3D12RootSignature* 的地址值，0 = 退回内部按 bindings 新建）。
//
// 用途（P129/P217）：地形 mesh 变体要复用现有地形的绑定点布局（b0..b3 / t4 / t5），
// 再由 JVM 侧补 b8 小常量与 t14/t15/t16 的 mesh 尾参。JVM 侧按“地形签名 + 尾参”
// 建好扩展签名后交进来，mesh PSO 与地形 PSO 才能共用同一套 draw 绑定。
MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateMeshPipelineWithRootSignature(
    const mcdr_mesh_pipeline_desc* desc, uint64_t rootSignature);

// 按绑定点布局建根签名（返回 ID3D12RootSignature* 的地址值，失败返回 0）。
// meshExtras != 0 时追加 mesh 专用尾参：root CBV b8（小常量/段表基址）+ 第二个描述符表
// （t14 顶点 / t15 段表 / t16 meshlet 表，固定 3 个 range）。
// 非 meshExtras 时带 ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT（图形管线用）；meshExtras 时不能带。
// 调用方必须用 mcdr_dx12ReleaseRootSignature 释放。
MCDR_API uint64_t MCDR_CALL mcdr_dx12CreateMeshRootSignature(
    const mcdr_pipeline_binding_desc* bindings, int32_t count, int32_t meshExtras);
MCDR_API void MCDR_CALL mcdr_dx12ReleaseRootSignature(uint64_t rootSignature);

// DispatchMesh with thread-group counts. Requires a mesh pipeline bound through
// mcdr_dx12SetPipeline, an open render pass, and bindings pushed with
// mcdr_dx12PushDescriptors. Returns 1 on success.
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchMesh(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t z);

// P217：meshlet 版地形派发（索引由 MS 算术生成）。
//
// 绑定布局与 mcdr_dx12CreateMeshRootSignature(meshExtras=1) 一致：
//   * root CBV b8  = 小常量（uint meshletCount + 3 个 pad，256 B 对齐）
//   * 描述符表     = t14 顶点池（28 B/顶点）、t15 段表（32 B/段）、t16 meshlet 表（32 B/条）
// 三个缓冲都是 RAW SRV（ByteAddressBuffer 着色器侧用 Load 读取），因此结构体步长由 HLSL
// 自己按 32 B 解析，不需要 SRV 的 StructureByteStride。
// 一次 DispatchMesh(meshletCount, 1, 1) 覆盖整批；段号写在 meshlet 记录里。
MCDR_API int32_t MCDR_CALL mcdr_dx12MeshDrawMeshlets(
    mcdr_command_ctx* ctx, mcdr_object* vertexBuffer, int64_t vertexOffset, mcdr_object* sections,
    int64_t sectionsOffset, int64_t sectionsLength, mcdr_object* meshlets, int64_t meshletsOffset,
    int64_t meshletsLength, mcdr_object* globals, int64_t globalsOffset, int32_t meshletCount);

// ---------------------------------------------------------------------------
// Surface  (was jni_bridge_p5.cpp — 13 exports)
// ---------------------------------------------------------------------------

MCDR_API mcdr_surface* MCDR_CALL mcdr_dx12CreateSurface(uint64_t hwnd);

// Writes the supported present-mode ordinals into `out` (up to `cap`) and
// returns the total count, so a caller can size the buffer first.
MCDR_API int32_t MCDR_CALL mcdr_dx12SurfacePresentModes(int32_t* out, int32_t cap);

MCDR_API int32_t MCDR_CALL mcdr_dx12ConfigureSurface(mcdr_surface* surface, int32_t width, int32_t height,
                                                     int32_t presentMode);
// P196：真独占全屏（1 = 进独占，0 = 退回窗口/无边框）。失败返回 0，画面不受影响。
MCDR_API int32_t MCDR_CALL mcdr_dx12SetExclusiveFullscreen(mcdr_surface* surface, int32_t on);

MCDR_API int32_t MCDR_CALL mcdr_dx12AcquireSurface(mcdr_surface* surface);
MCDR_API void MCDR_CALL mcdr_dx12BlitSurface(mcdr_command_ctx* ctx, mcdr_surface* surface,
                                             mcdr_object* texture);
MCDR_API void MCDR_CALL mcdr_dx12PresentSurface(mcdr_surface* surface);
// Returns the raw ID3D12Resource* of the currently acquired back buffer as an
// opaque integer (NOT a mcdr_object* -- the Java side only forwards it).
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetBackBufferHandle(mcdr_surface* surface);
MCDR_API int32_t MCDR_CALL mcdr_dx12IsSurfaceSuboptimal(mcdr_surface* surface);

// P197（原生 HDR）：0 = auto（按窗口所在输出的 DXGI 色彩空间判定），1 = 强制开，2 = 强制关。
// 只改进程状态，真正生效在下次创建/重建交换链时（格式是建链参数，不能原地改）。
MCDR_API void MCDR_CALL mcdr_dx12SetHdrMode(int32_t mode);
MCDR_API int32_t MCDR_CALL mcdr_dx12GetHdrAppliedMode(void);

// P117: 光追可见性分层总开关（0=off, 1=ao, 2=ao_shadows, 3=full）；不支持的设备上记下但不生效。
MCDR_API void MCDR_CALL mcdr_dx12SetRaytracingMode(int32_t mode);

// P251（交换链建链选项，下次建链时生效）：
//   allowTearing       0 = 不建 ALLOW_TEARING 的链（强制 DWM 合成翻页），非 0 = 允许（默认）。
//                      某些覆盖层（RTSS/Afterburner 的 OSD）只在合成翻页下画得出来。
//   forcePresentChild  非 0 = 优先把链建在自建呈现子窗口上（P196 之前的默认），0 = 默认（顶层窗口）。
MCDR_API void MCDR_CALL mcdr_dx12SetSwapchainOptions(int32_t allowTearing, int32_t forcePresentChild);

MCDR_API void MCDR_CALL mcdr_dx12DestroySurface(mcdr_surface* surface);
MCDR_API mcdr_surface* MCDR_CALL mcdr_dx12GetActiveSurfaceHandle(void);

// Both readback entry points write 36 int32s (9 pixels x RGBA) into `out` and
// return 1 on success, 0 on failure.
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadbackSurfacePixels(mcdr_surface* surface, int32_t* out);
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadbackTexturePixels(mcdr_object* texture, int32_t* out);

/* P291e：已知起始状态的读回（ABI v4 纯追加导出）。
 * `fromState` 是这张纹理**当前真实所处**的 D3D12_RESOURCE_STATES 数值，不是"我希望它在哪"。
 * 传错 → barrier 的 StateBefore 不匹配 → 读回内容未定义（实测恒 0）。上面那个不带状态的
 * 旧导出按 COMMON 处理，只对"建完没被 GPU 写过"的纹理成立；DLSS 的输出纹理在帧末停在
 * COPY_SOURCE（FG 关，d3d12_dlss.cpp）或 UNORDERED_ACCESS（FG 开），必须走这一条。
 * 读回结束后纹理被恢复成 fromState，对 native 的状态跟踪没有净影响。 */
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadbackTexturePixelsIn(mcdr_object* texture, int32_t fromState,
                                                            int32_t* out);

/* P291e：DLSS 输出纹理上一帧结束时停在哪个状态（D3D12_RESOURCE_STATES 数值）。
 * 0 (COMMON) = 这一帧没有输出纹理/未知 —— 调用方不要据此采样。 */
MCDR_API int32_t MCDR_CALL mcdr_dx12DlssOutputState(void);

MCDR_API void MCDR_CALL mcdr_dx12DumpTextureToFile(mcdr_object* texture, const char* tag);

// ---------------------------------------------------------------------------
// Draw  (was jni_bridge_p6.cpp — 9 exports, +1 for MC 26.2 setViewport)
// ---------------------------------------------------------------------------

MCDR_API int32_t MCDR_CALL mcdr_dx12SetPipeline(mcdr_command_ctx* ctx, mcdr_pipeline* pipeline,
                                                int32_t hasDepth);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetScissor(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                               int32_t height);
// MC 26.2 addition: RenderPassBackend::setViewport(int,int,int,int). Previously
// the viewport was only ever set inside mcdr_dx12BeginRenderPass.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetViewport(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                                int32_t height);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetVertexBuffer(mcdr_command_ctx* ctx, int32_t slot, mcdr_object* buffer,
                                                    int64_t offset, int32_t stride);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetIndexBuffer(mcdr_command_ctx* ctx, mcdr_object* buffer,
                                                   int32_t indexType);

// `bindings` holds `count` entries, one per pipeline binding in declaration
// order. Replaces the six parallel arrays of the JNI version.
MCDR_API int32_t MCDR_CALL mcdr_dx12PushDescriptors(mcdr_command_ctx* ctx, const mcdr_binding_slot* bindings,
                                                    int32_t count);

MCDR_API int32_t MCDR_CALL mcdr_dx12DrawIndexed(mcdr_command_ctx* ctx, int32_t indexCount,
                                                int32_t instanceCount, int32_t firstIndex, int32_t baseVertex,
                                                int32_t firstInstance);
MCDR_API int32_t MCDR_CALL mcdr_dx12Draw(mcdr_command_ctx* ctx, int32_t vertexCount, int32_t instanceCount,
                                         int32_t firstVertex, int32_t firstInstance);
MCDR_API int32_t MCDR_CALL mcdr_dx12DrawIndexedIndirect(mcdr_command_ctx* ctx, mcdr_object* commands,
                                                        int64_t offset, int32_t drawCount);
MCDR_API int32_t MCDR_CALL mcdr_dx12DrawIndirect(mcdr_command_ctx* ctx, mcdr_object* commands, int64_t offset,
                                                 int32_t drawCount);

// ---------------------------------------------------------------------------
// Async: partitioned descriptor allocator
// (was jni_bridge_p33.cpp — 42 exports across the four families below)
// ---------------------------------------------------------------------------

MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncCurrentFrameSlot(mcdr_command_ctx* ctx);
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncCurrentFrameValue(mcdr_command_ctx* ctx);

MCDR_API mcdr_desc_alloc* MCDR_CALL mcdr_dx12AsyncDescriptorCreate(int32_t workerCount);
MCDR_API void MCDR_CALL mcdr_dx12AsyncDescriptorDestroy(mcdr_desc_alloc* alloc);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncDescriptorRegionBase(mcdr_desc_alloc* alloc, int32_t frameSlot,
                                                              int32_t worker);
// Returns the base descriptor slot, or -1 when the region is exhausted.
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncDescriptorAllocate(mcdr_desc_alloc* alloc, int32_t frameSlot,
                                                            int32_t worker, int32_t count);
MCDR_API void MCDR_CALL mcdr_dx12AsyncDescriptorResetFrame(mcdr_desc_alloc* alloc, int32_t frameSlot);
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncDescriptorGpuHandle(mcdr_desc_alloc* alloc, int32_t slot);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncDescriptorWriteCBV(mcdr_desc_alloc* alloc, int32_t slot,
                                                            mcdr_object* buffer, int64_t offset,
                                                            int64_t size);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncDescriptorWriteSRV(mcdr_desc_alloc* alloc, int32_t slot,
                                                            mcdr_object* view);

// ---------------------------------------------------------------------------
// Async: bundle recorder pool
// ---------------------------------------------------------------------------

MCDR_API mcdr_bundle_pool* MCDR_CALL mcdr_dx12AsyncBundlePoolCreate(int32_t workerCount);
MCDR_API void MCDR_CALL mcdr_dx12AsyncBundlePoolDestroy(mcdr_bundle_pool* pool);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleBegin(mcdr_bundle_pool* pool, int32_t worker,
                                                     int32_t frameSlot, uint64_t frameValue);
// Returns the recorded ID3D12GraphicsCommandList* as an opaque integer, 0 on failure.
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncBundleEnd(mcdr_bundle_pool* pool, int32_t worker);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleSetPipelineState(mcdr_bundle_pool* pool, int32_t worker,
                                                                mcdr_pipeline* pipeline, int32_t hasDepth);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleSetDescriptorTable(mcdr_bundle_pool* pool, int32_t worker,
                                                                  int32_t slot, uint64_t gpuHandle);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleSetVertexBuffer(mcdr_bundle_pool* pool, int32_t worker,
                                                               int32_t slot, mcdr_object* buffer,
                                                               int64_t offset, int32_t stride);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleSetIndexBuffer(mcdr_bundle_pool* pool, int32_t worker,
                                                              mcdr_object* buffer, int32_t indexType);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleDrawIndexed(mcdr_bundle_pool* pool, int32_t worker,
                                                           int32_t indexCount, int32_t instanceCount,
                                                           int32_t firstIndex, int32_t baseVertex,
                                                           int32_t firstInstance);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleDraw(mcdr_bundle_pool* pool, int32_t worker,
                                                    int32_t vertexCount, int32_t instanceCount,
                                                    int32_t firstVertex, int32_t firstInstance);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleDrawIndexedIndirect(mcdr_bundle_pool* pool, int32_t worker,
                                                                   mcdr_object* commands, int64_t offset,
                                                                   int32_t drawCount);

// The hot path (P57/P63/P64): one call records a whole worker partition.
//
//   `bindings` — when `sharedBindings` is non-zero, `bindingCount` entries
//                describing draw 0's block, reused for every draw. Otherwise
//                `drawCount * bindingCount` entries indexed as
//                bindings[draw * bindingCount + slot].
//   `draws`    — `drawCount` entries for the partition, already advanced to
//                the partition's first draw.
//
// Semantics match the per-call sequence exactly: any invalid argument returns
// 0 and the Java side discards the bundle and re-records serially.
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleRecordPartition(mcdr_desc_alloc* alloc, mcdr_bundle_pool* pool,
                                                               int32_t worker, int32_t baseSlot,
                                                               int32_t bindingCount, int32_t sharedBindings,
                                                               const mcdr_binding_slot* bindings,
                                                               const mcdr_partition_draw* draws,
                                                               int32_t drawCount);

MCDR_API int32_t MCDR_CALL mcdr_dx12ExecuteBundle(mcdr_command_ctx* ctx, uint64_t bundle);

// Barrier pre-passes: a bundle may not contain barriers, so every CBV buffer,
// index buffer and texture view is transitioned on the main list first.
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncPrepareCBVBuffers(mcdr_command_ctx* ctx, mcdr_object* const* buffers,
                                                           int32_t count);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncPrepareIndexBuffers(mcdr_command_ctx* ctx,
                                                             mcdr_object* const* buffers, int32_t count);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncPrepareTextureViews(mcdr_command_ctx* ctx, mcdr_object* const* views,
                                                             int32_t count);

// ---------------------------------------------------------------------------
// Async: executor
// ---------------------------------------------------------------------------

MCDR_API mcdr_executor* MCDR_CALL mcdr_dx12AsyncExecutorCreate(void);
MCDR_API void MCDR_CALL mcdr_dx12AsyncExecutorDestroy(mcdr_executor* executor);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorTryBeginFrame(mcdr_executor* executor);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorAddBundle(mcdr_executor* executor, uint64_t bundle);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorAddTransition(mcdr_executor* executor, mcdr_object* object,
                                                               int32_t stateBefore, int32_t stateAfter);
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncExecutorEndFrame(mcdr_executor* executor);
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncExecutorLastFenceValue(mcdr_executor* executor);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorIsFrameInProgress(mcdr_executor* executor);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorWaitFrame(mcdr_executor* executor, uint64_t fenceValue,
                                                           int64_t timeoutMs);

// ---------------------------------------------------------------------------
// Async: fence manager
//
// The only place the native layer calls back into the JVM. `cb` is a single
// FFM upcall stub created once for the whole library; `userHandle` identifies
// the Java-side registration, so no JVM object reference is ever retained here.
// The callback is invoked OUTSIDE the registry lock, so it may re-register or
// unregister. `timeoutMs == 0` means wait forever.
// ---------------------------------------------------------------------------

typedef void(MCDR_CALL* mcdr_fence_callback_fn)(int64_t userHandle, uint64_t value, int32_t completed);

MCDR_API mcdr_fence_mgr* MCDR_CALL mcdr_dx12AsyncFenceCreate(void);
MCDR_API void MCDR_CALL mcdr_dx12AsyncFenceDestroy(mcdr_fence_mgr* manager);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncFenceSignal(mcdr_fence_mgr* manager, uint64_t value);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncFenceWait(mcdr_fence_mgr* manager, uint64_t value,
                                                   int64_t timeoutMs);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncFenceIsComplete(mcdr_fence_mgr* manager, uint64_t value);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncFenceRegisterCallback(mcdr_fence_mgr* manager, uint64_t value,
                                                               mcdr_fence_callback_fn cb, int64_t userHandle,
                                                               int64_t timeoutMs);

// ---------------------------------------------------------------------------
// P235：着色率图像（tier 2 SRI）
//
// compute 读上一帧深度（t0）与上一帧速度（t1），写一张 R8_UINT 的 tile 表（u0）：
// 每个 texel 管一整块 tile，值是 D3D12_SHADING_RATE_* 的枚举（0=1X1 … 0xA=4X4）。
// 图像尺寸是 **tile 数**（宽高各自 ceil(pixel / tile)），tile 边长看设备能力
// （NVIDIA 16、AMD 8）。
// ---------------------------------------------------------------------------
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineShadingRate(const void* dxil,
                                                                         int32_t dxilSize);
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateShadingRateImage(int32_t width, int32_t height);

// 在已打开的命令列表上生成一次。srvDepth/srvMotion/srvPrevious 是 TextureView 句柄
// （mcdr_dx12CreateTextureView 的产物）：t0 深度、t1 速度、t2 上一帧的着色率图像
// （P236 时间钳制用，读不到历史时可传同一张或任意有效视图，并在 constants 里关掉钳制）。
// image 是要写的那张图像。
// 返回 1=已派发 0=参数或资源不对 -2=没有打开的命令列表。
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchComputeShadingRate(
    mcdr_command_ctx* ctx, mcdr_object* pipeline, mcdr_object* srvDepth, mcdr_object* srvMotion,
    mcdr_object* srvPrevious, mcdr_object* image, const int32_t* constants, int32_t constantCount,
    int32_t x, int32_t y, int32_t z);

// ---------------------------------------------------------------------------
// P276：通用 compute 纹理 pass（3×SRV t0/t1/t2 + 1×UAV u0 + b0 的 8 个 int）
//
// 根签名与 P235 的着色率图像那条**完全相同**，只是用途从"算 SRI 图像"泛化成
// "读几张纹理、写一张纹理"。所以实现直接复用那两个函数，这里只给一对不带
// "ShadingRate" 字样的名字 —— 否则以后读掩码的代码会写成
// `dispatchComputeShadingRate(..., mask, ...)`，对不上号。
//
// image 必须带 usage 位 32（ALLOW_UNORDERED_ACCESS），否则返回 0。
// srv0/srv1/srv2 必须是带 CPU 描述符句柄的 TextureView；用不到的槽传同一张有效视图
// （着色器不读即可）—— 三张都必须有，这是固定根签名。
// ---------------------------------------------------------------------------
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineTexture3(const void* dxil,
                                                                     int32_t dxilSize);
MCDR_API int32_t MCDR_CALL mcdr_dx12DispatchComputeTexture3(
    mcdr_command_ctx* ctx, mcdr_object* pipeline, mcdr_object* srv0, mcdr_object* srv1,
    mcdr_object* srv2, mcdr_object* image, const int32_t* constants, int32_t constantCount,
    int32_t x, int32_t y, int32_t z);

// 把图像挂到列表的 SRI 槽位（image 传 NULL 摘掉）。内部把图像从 COMMON 转到
// SHADING_RATE_SOURCE；返回 1=已绑定 0=列表不是 ID3D12GraphicsCommandList5 -1=不是纹理
// -2=没有打开的命令列表。
MCDR_API int32_t MCDR_CALL mcdr_dx12SetShadingRateImage(mcdr_command_ctx* ctx, mcdr_object* image);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MCDR_ABI_H