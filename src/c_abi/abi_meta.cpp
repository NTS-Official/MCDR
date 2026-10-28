// abi_meta.cpp — the ABI's own surface: version, layout self-check, error
// reporting and log verbosity.
//
// The layout self-check is the piece JNI never had. Under JNI every one of the
// 115 entry points took `jlong` handles and hand-packed byte buffers, so a
// renamed package, a reordered struct field or a changed enum ordinal produced
// silent memory corruption at runtime with no compile-time or load-time signal
// (the repo already shipped a stale `com_dx12_dx12` symbol prefix once). Here
// the native side publishes one (name, offset, size) triple per mirrored field
// and the Java side compares it against its own MemoryLayouts before any other
// entry point is called.
module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"
#include <cstdlib>   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

// P108：DLSS 实现单元（普通 TU，src\advanced\d3d12_dlss.cpp）的 C 接口 ——
// mcdr_dx12SetDlssVendorDir 只是它的转发层。同 rtx_probe 的理由：模块看不到 NGX 类型。
#include "mcdr_dlss_impl.h"

import mcdr.d3d12.device;

namespace mcdr::abi {

namespace {

// (name, offset, size) for one mirrored field. sizeof() is applied to an
// unevaluated null dereference because a non-static data member cannot be named
// in a sizeof expression directly.
#define MCDR_FIELD(T, F)                               \
    {#T "." #F, static_cast<uint64_t>(offsetof(T, F)), \
     static_cast<uint64_t>(sizeof((static_cast<T*>(nullptr))->F))}

const std::vector<mcdr_abi_field>& layoutTable() {
    // Function-local static: initialised once, thread-safe under C++11 and
    // later, and never touched again after the first call.
    static const std::vector<mcdr_abi_field> table = {
        MCDR_FIELD(mcdr_binding_slot, type),
        MCDR_FIELD(mcdr_binding_slot, texelFormat),
        MCDR_FIELD(mcdr_binding_slot, buffer),
        MCDR_FIELD(mcdr_binding_slot, view),
        MCDR_FIELD(mcdr_binding_slot, offset),
        MCDR_FIELD(mcdr_binding_slot, length),

        MCDR_FIELD(mcdr_partition_draw, indexBuffer),
        MCDR_FIELD(mcdr_partition_draw, vertexBuffer),
        MCDR_FIELD(mcdr_partition_draw, vertexOffset),
        MCDR_FIELD(mcdr_partition_draw, indexType),
        MCDR_FIELD(mcdr_partition_draw, indexCount),
        MCDR_FIELD(mcdr_partition_draw, firstIndex),
        MCDR_FIELD(mcdr_partition_draw, baseVertex),
        MCDR_FIELD(mcdr_partition_draw, vertexSlot),
        MCDR_FIELD(mcdr_partition_draw, vertexStride),
        MCDR_FIELD(mcdr_partition_draw, _pad0),

        MCDR_FIELD(mcdr_color_target_desc, format),
        MCDR_FIELD(mcdr_color_target_desc, writeMask),
        MCDR_FIELD(mcdr_color_target_desc, blendEnabled),
        MCDR_FIELD(mcdr_color_target_desc, srcColor),
        MCDR_FIELD(mcdr_color_target_desc, dstColor),
        MCDR_FIELD(mcdr_color_target_desc, colorOp),
        MCDR_FIELD(mcdr_color_target_desc, srcAlpha),
        MCDR_FIELD(mcdr_color_target_desc, dstAlpha),
        MCDR_FIELD(mcdr_color_target_desc, alphaOp),

        MCDR_FIELD(mcdr_depth_state_desc, depthFormat),
        MCDR_FIELD(mcdr_depth_state_desc, depthWrite),
        MCDR_FIELD(mcdr_depth_state_desc, depthCompareOp),

        MCDR_FIELD(mcdr_input_element_desc, location),
        MCDR_FIELD(mcdr_input_element_desc, binding),
        MCDR_FIELD(mcdr_input_element_desc, format),
        MCDR_FIELD(mcdr_input_element_desc, offset),
        MCDR_FIELD(mcdr_input_element_desc, stride),
        MCDR_FIELD(mcdr_input_element_desc, stepRate),
        MCDR_FIELD(mcdr_input_element_desc, _pad0),
        MCDR_FIELD(mcdr_input_element_desc, semanticName),

        MCDR_FIELD(mcdr_pipeline_binding_desc, type),
        MCDR_FIELD(mcdr_pipeline_binding_desc, reg),

        MCDR_FIELD(mcdr_pipeline_desc, vsData),
        MCDR_FIELD(mcdr_pipeline_desc, vsLen),
        MCDR_FIELD(mcdr_pipeline_desc, _pad0),
        MCDR_FIELD(mcdr_pipeline_desc, psData),
        MCDR_FIELD(mcdr_pipeline_desc, psLen),
        MCDR_FIELD(mcdr_pipeline_desc, colorCount),
        MCDR_FIELD(mcdr_pipeline_desc, colorTargets),
        MCDR_FIELD(mcdr_pipeline_desc, hasDepth),
        MCDR_FIELD(mcdr_pipeline_desc, _pad1),
        MCDR_FIELD(mcdr_pipeline_desc, depth),
        MCDR_FIELD(mcdr_pipeline_desc, topology),
        MCDR_FIELD(mcdr_pipeline_desc, cullEnabled),
        MCDR_FIELD(mcdr_pipeline_desc, polygonMode),
        MCDR_FIELD(mcdr_pipeline_desc, inputElementCount),
        MCDR_FIELD(mcdr_pipeline_desc, inputElements),
        MCDR_FIELD(mcdr_pipeline_desc, bindingCount),
        MCDR_FIELD(mcdr_pipeline_desc, _pad2),
        MCDR_FIELD(mcdr_pipeline_desc, bindings),

        // P72 mesh-shader pipeline. The Java mirror must carry exactly these
        // fields with these sizes, or MCDRLayout.verify() aborts at load time.
        MCDR_FIELD(mcdr_mesh_pipeline_desc, asData),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, asLen),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, _pad0),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, msData),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, msLen),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, _pad1),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, psData),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, psLen),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, colorCount),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, colorTargets),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, hasDepth),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, _pad2),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, depth),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, cullEnabled),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, polygonMode),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, bindingCount),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, _pad3),
        MCDR_FIELD(mcdr_mesh_pipeline_desc, bindings),

        // P108 DLSS per-frame parameter package. The Kotlin side mirrors this struct in
        // MCDR_Layout.kt, so every named field (including _pad0/_pad1/_pad2) MUST be
        // published here: verifyABI() compares both directions, and a Kotlin-only field
        // aborts backend creation with "missing from the native layout report".
        MCDR_FIELD(mcdr_dlss_input, renderWidth),
        MCDR_FIELD(mcdr_dlss_input, renderHeight),
        MCDR_FIELD(mcdr_dlss_input, targetWidth),
        MCDR_FIELD(mcdr_dlss_input, targetHeight),
        MCDR_FIELD(mcdr_dlss_input, jitterOffsetX),
        MCDR_FIELD(mcdr_dlss_input, jitterOffsetY),
        MCDR_FIELD(mcdr_dlss_input, mvScaleX),
        MCDR_FIELD(mcdr_dlss_input, mvScaleY),
        MCDR_FIELD(mcdr_dlss_input, preExposure),
        MCDR_FIELD(mcdr_dlss_input, exposureScale),
        MCDR_FIELD(mcdr_dlss_input, exposureMode),
        MCDR_FIELD(mcdr_dlss_input, alphaUpscaling),
        MCDR_FIELD(mcdr_dlss_input, nearZ),
        MCDR_FIELD(mcdr_dlss_input, farZ),
        MCDR_FIELD(mcdr_dlss_input, depthInverted),
        MCDR_FIELD(mcdr_dlss_input, reset),
        MCDR_FIELD(mcdr_dlss_input, depthFormatOrdinal),
        MCDR_FIELD(mcdr_dlss_input, _pad0),
        MCDR_FIELD(mcdr_dlss_input, pInColor),
        MCDR_FIELD(mcdr_dlss_input, pInDepth),
        MCDR_FIELD(mcdr_dlss_input, pInMotionVectors),
        MCDR_FIELD(mcdr_dlss_input, pInOutput),
        MCDR_FIELD(mcdr_dlss_input, _pad1),
        MCDR_FIELD(mcdr_dlss_input, _pad2),

        // P271 SR 抽象层的两张新表。与上面同理：Kotlin 的 MCDR_Layout.kt 逐字段镜像，
        // verify() 双向比对，任何一侧漏字段都会在设备创建时 abort。字段名必须与
        // mcdr_abi.h 的 typedef 逐字一致（含 _pad1）。
        MCDR_FIELD(mcdr_sr_caps, backend),
        MCDR_FIELD(mcdr_sr_caps, available),
        MCDR_FIELD(mcdr_sr_caps, coupledDenoise),
        MCDR_FIELD(mcdr_sr_caps, needsExposure),
        MCDR_FIELD(mcdr_sr_caps, takesMasks),
        MCDR_FIELD(mcdr_sr_caps, externalHeap),
        MCDR_FIELD(mcdr_sr_caps, jitterPhaseCount),
        MCDR_FIELD(mcdr_sr_caps, minRenderWidth),
        MCDR_FIELD(mcdr_sr_caps, minRenderHeight),
        MCDR_FIELD(mcdr_sr_caps, maxRenderWidth),
        MCDR_FIELD(mcdr_sr_caps, maxRenderHeight),
        MCDR_FIELD(mcdr_sr_caps, nativeAA),
        MCDR_FIELD(mcdr_sr_caps, reserved),
        MCDR_FIELD(mcdr_sr_caps, note),

        MCDR_FIELD(mcdr_sr_dispatch, renderWidth),
        MCDR_FIELD(mcdr_sr_dispatch, renderHeight),
        MCDR_FIELD(mcdr_sr_dispatch, displayWidth),
        MCDR_FIELD(mcdr_sr_dispatch, displayHeight),
        MCDR_FIELD(mcdr_sr_dispatch, jitterOffsetX),
        MCDR_FIELD(mcdr_sr_dispatch, jitterOffsetY),
        MCDR_FIELD(mcdr_sr_dispatch, motionVectorScaleX),
        MCDR_FIELD(mcdr_sr_dispatch, motionVectorScaleY),
        MCDR_FIELD(mcdr_sr_dispatch, preExposure),
        MCDR_FIELD(mcdr_sr_dispatch, frameTimeDeltaMs),
        MCDR_FIELD(mcdr_sr_dispatch, cameraNear),
        MCDR_FIELD(mcdr_sr_dispatch, cameraFar),
        MCDR_FIELD(mcdr_sr_dispatch, cameraVerticalFov),
        MCDR_FIELD(mcdr_sr_dispatch, sharpness),
        MCDR_FIELD(mcdr_sr_dispatch, reset),
        MCDR_FIELD(mcdr_sr_dispatch, hdrInput),
        MCDR_FIELD(mcdr_sr_dispatch, backend),
        MCDR_FIELD(mcdr_sr_dispatch, flags),
        MCDR_FIELD(mcdr_sr_dispatch, pColorInput),
        MCDR_FIELD(mcdr_sr_dispatch, pColorOutput),
        MCDR_FIELD(mcdr_sr_dispatch, pDepth),
        MCDR_FIELD(mcdr_sr_dispatch, pMotionVectors),
        MCDR_FIELD(mcdr_sr_dispatch, pReactiveMask),
        MCDR_FIELD(mcdr_sr_dispatch, pExposure),
        MCDR_FIELD(mcdr_sr_dispatch, depthFormatOrdinal),
        MCDR_FIELD(mcdr_sr_dispatch, cameraAspectRatio),
        MCDR_FIELD(mcdr_sr_dispatch, viewSpaceToMetersFactor),
        MCDR_FIELD(mcdr_sr_dispatch, _pad2),

        // P295 FG 抽象层的两张新表。与 SR 同理：Kotlin 的 MCDR_Layout.kt 逐字段镜像，
        // verify() 双向比对，任何一侧漏字段都会在设备创建时 abort。字段名必须与
        // mcdr_abi.h 的 typedef 逐字一致（含两个 _pad）。
        MCDR_FIELD(mcdr_fg_caps, backend),
        MCDR_FIELD(mcdr_fg_caps, available),
        MCDR_FIELD(mcdr_fg_caps, runtimeLoaded),
        MCDR_FIELD(mcdr_fg_caps, deviceSupported),
        MCDR_FIELD(mcdr_fg_caps, chainCompatible),
        MCDR_FIELD(mcdr_fg_caps, needsReflex),
        MCDR_FIELD(mcdr_fg_caps, needsUiResource),
        MCDR_FIELD(mcdr_fg_caps, needsHudless),
        MCDR_FIELD(mcdr_fg_caps, transitionsInputs),
        MCDR_FIELD(mcdr_fg_caps, maxGeneratedFrames),
        MCDR_FIELD(mcdr_fg_caps, chainOwner),
        MCDR_FIELD(mcdr_fg_caps, reserved),
        MCDR_FIELD(mcdr_fg_caps, note),
        MCDR_FIELD(mcdr_fg_caps, latencyProvider),

        MCDR_FIELD(mcdr_fg_dispatch, displayWidth),
        MCDR_FIELD(mcdr_fg_dispatch, displayHeight),
        MCDR_FIELD(mcdr_fg_dispatch, backend),
        MCDR_FIELD(mcdr_fg_dispatch, mode),
        MCDR_FIELD(mcdr_fg_dispatch, numFrames),
        MCDR_FIELD(mcdr_fg_dispatch, reset),
        MCDR_FIELD(mcdr_fg_dispatch, appFrameId),
        MCDR_FIELD(mcdr_fg_dispatch, backendFrameId),
        MCDR_FIELD(mcdr_fg_dispatch, presentId),
        MCDR_FIELD(mcdr_fg_dispatch, pBackbuffer),
        MCDR_FIELD(mcdr_fg_dispatch, pHudlessColor),
        MCDR_FIELD(mcdr_fg_dispatch, pDepth),
        MCDR_FIELD(mcdr_fg_dispatch, pMotionVectors),
        MCDR_FIELD(mcdr_fg_dispatch, pUiColor),
        MCDR_FIELD(mcdr_fg_dispatch, _pad2),
        // P305z：FG 自己的帧常量。数组字段也走 MCDR_FIELD（offsetof + sizeof 取到整段）。
        MCDR_FIELD(mcdr_fg_constants, view),
        MCDR_FIELD(mcdr_fg_constants, projection),
        MCDR_FIELD(mcdr_fg_constants, jitterX),
        MCDR_FIELD(mcdr_fg_constants, jitterY),
        MCDR_FIELD(mcdr_fg_constants, mvScaleX),
        MCDR_FIELD(mcdr_fg_constants, mvScaleY),
        MCDR_FIELD(mcdr_fg_constants, resetHistory),
        MCDR_FIELD(mcdr_fg_constants, frameRenderTime),
    };
    return table;
}

#undef MCDR_FIELD

}  // namespace

}  // namespace mcdr::abi

extern "C" {

uint32_t MCDR_CALL mcdrABIVersion(void) { return MCDR_ABI_VERSION; }

void MCDR_CALL mcdrABILayout(const mcdr_abi_field** fields, int32_t* count) {
    const std::vector<mcdr_abi_field>& table = mcdr::abi::layoutTable();
    if (fields) {
        *fields = table.data();
    }
    if (count) {
        *count = static_cast<int32_t>(table.size());
    }
}

int32_t MCDR_CALL mcdrLastError(char* buf, int32_t cap) {
    const std::string message = mcdr::abi::lastErrorText();
    if (message.empty()) {
        return 0;
    }
    return mcdr::abi::writeString(message, buf, cap);
}

void MCDR_CALL mcdrSetLogVerbose(int32_t enabled) {
    // Mirrors the old getenv("DX12_LOG_VERBOSE") read, which JNI performed
    // inside dx12CreateDevice(). The decision now belongs to the Java side so
    // the native layer no longer reaches into the process environment.
    d3d12_mc::setLogLevel(enabled ? 3 : 1);
}

void MCDR_CALL mcdrSetLogLevel(int32_t level) {
    // 0=error, 1=warning, 2=info, 3=debug. d3d12_mc::setLogLevel() clamps to 0..3.
    // mcdrSetLogVerbose() above keeps its original 1-vs-3 mapping.
    d3d12_mc::setLogLevel(level);
}

void MCDR_CALL mcdrSetShutdownTraceAlways(int32_t always) {
    // P184 补记：配置项 `[logging] shutdownTrace`。1（默认）= destroySurface/destroyDevice 的
    // 进度标记无视 logLevel 恒定输出（退出卡死时唯一的线索）；0 = 它们退回普通 INFO。
    // 日志语义（INFO）不变，只是"是否被 logLevel 过滤"由 Java 侧决定。
    d3d12_mc::setShutdownTraceAlways(always != 0);
}

int32_t MCDR_CALL mcdrSetLogFile(const char* utf8Path) {
    // P250：配置项 `[logging] nativeLogFile`。启动器（Prism 等）不把子进程 stdout/stderr 落盘，
    // 所以"用户端出问题却拿不到 native 行"是常态；这条导出让 Java 侧把镜像文件的路径推进来。
    // 真正的 fopen 发生在下一次写日志时（见 d3d12_device.cpp 的 mirrorLogToFile）。
    d3d12_mc::setLogFile(utf8Path);
    return (utf8Path != nullptr && utf8Path[0] != '\0') ? 1 : 0;
}

void MCDR_CALL mcdr_dx12SetEnvironment(const char* name, const char* value) {
    // A small, deliberate escape hatch: the Java side owns every user-visible setting, while the
    // native layer keeps its plain getenv() reads. _putenv_s updates the CRT environment block,
    // which is the copy getenv()/_wgetenv() in this DLL reads; an empty value removes the entry.
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    (void)_putenv_s(name, value != nullptr ? value : "");
}
int32_t MCDR_CALL mcdrSetPixCapture(int32_t enabled) {
    // 1=loaded, 0=not requested, -1=requested but not loaded. Loading the capture
    // layer is irreversible for the process (PIX hooks D3D12 from inside it), so the
    // decision belongs to the Java side and has to arrive before the first device.
    return d3d12_mc::setPixCapture(enabled != 0);
}

int32_t MCDR_CALL mcdrSetPixDirectory(const char* utf8Path) {
    // Where to find PIX's DLLs, from the [debug] pixDirectory config. Must arrive before
    // mcdrSetPixCapture: the layer is loaded before the first D3D12 device exists.
    return d3d12_mc::setPixDirectory(utf8Path);
}

int32_t MCDR_CALL mcdrStartPixTimingCapture(const char* utf8Path) {
    // The one capture PIX's UI cannot produce for us: its continuous recording stays inside
    // the PIX session, so a file only exists once the process drives PIXBeginCapture itself.
    return d3d12_mc::startPixTimingCapture(utf8Path);
}

int32_t MCDR_CALL mcdrStopPixTimingCapture(void) {
    return d3d12_mc::stopPixTimingCapture();
}

int32_t MCDR_CALL mcdrSetShadingRate(int32_t rate) {
    // P97. rate is a raw D3D12_SHADING_RATE (1X1=0x0 1X2=0x1 2X1=0x4 2X2=0x5 2X4=0x6
    // 4X2=0x9 4X4=0xA), -1 = off. Returns 1=accepted (including "the device does not
    // exist yet, so it will be gated on the first command list"), 0=this device cannot
    // honour the rate (tier 0, or an additional rate that is not supported), -1=unknown
    // rate value.
    return d3d12_mc::setShadingRate(rate);
}

int32_t MCDR_CALL mcdr_dx12SetShadingRateNow(mcdr_command_ctx* ctx, int32_t rate) {
    // P231. The per-draw policy brackets the draw batches that may be shaded coarsely with this
    // call and restores 1X1 afterwards, so the coarse rate never reaches the sky, particles or the
    // GUI. Legal mid-"pass": this module's beginRenderPass is only OMSetRenderTargets, never
    // ID3D12GraphicsCommandList4::BeginRenderPass.
    // Returns 1=issued, 0=the device cannot honour it (tier 0 / missing additional rates) or the
    // list is not an ID3D12GraphicsCommandList5, -1=unknown rate, -2=ctx null or no open list.
    return d3d12_mc::setShadingRateNow(ctx, rate);
}

int32_t MCDR_CALL mcdr_dx12SetShadingRateCombiners(int32_t first, int32_t second) {
    // P231. Stage 0 combines the per-draw rate with the per-primitive rate (SV_ShadingRate),
    // stage 1 with the shading-rate image. Defaults stay {PASSTHROUGH, PASSTHROUGH} until the mesh
    // shader writes SV_ShadingRate: under MIN a per-primitive 1X1 would undo every requested 2X2.
    // Returns 1=stored, -1=a value outside 0..4.
    return d3d12_mc::setShadingRateCombiners(first, second);
}

int32_t MCDR_CALL mcdrSetAntiAliasing(int32_t mode) {
    // P104/P108/P152. mode mirrors the Java-side option: **0=OFF 1=SMAA 2=TAA 3=DLAA**. SMAA
    // (three passes glued in place) and TAA (P152: a native resolve with an RGBA16F history
    // ping-pong, see mcdr_dx12ApplyTaaToTexture) are implemented; DLAA is still accepted and
    // downgraded to OFF with a WARN.
    // P108 (user m10920): DLSS is NO LONGER a value here — it has its own switch, see
    // mcdr_dx12SetDlssEnabled. Anti-aliasing and DLSS are independent.
    // Returns 1=in effect (OFF/SMAA/TAA), 0=accepted but not implemented (DLAA), -1=unknown mode.
    return d3d12_mc::setAntiAliasing(mode);
}

int32_t MCDR_CALL mcdr_dx12SetDlssEnabled(int32_t enabled) {
    // P108 (user m10920). DLSS upscaling gets its own option (Java `dxrMcDlss`) instead of
    // borrowing a value of `dxrMcAaMode`. Two things happen:
    //   1. the authoritative gate the renderer reads — gCtx.dlssEnabled (device module);
    //   2. a mirror inside the DLSS implementation so the status query and the P108 log can
    //      report the switch. The NGX runtime and any live feature handle are deliberately
    //      left alone, so OFF -> ON does not redo Init/CreateFeature.
    // Returns the normalised state: 1=enabled afterwards, 0=disabled afterwards.
    const int32_t gate = d3d12_mc::setDlssEnabled(enabled);
    mcdr_dlss_set_enabled(enabled);
    return gate;
}

int32_t MCDR_CALL mcdr_dx12SnapshotDlssDepth(mcdr_command_ctx* ctx, mcdr_object* depth) {
    // P108. The Java side calls this where this frame's depth is still valid (alongside the
    // motion-vector capture, before vanilla clears the depth buffer). The device module unwraps
    // the Dx12Object handle and hands the transition callbacks to the DLSS implementation, which
    // owns the private D32 snapshot texture (see mcdr_dlss_impl.h for the full rationale).
    // Returns 1=taken/reused, 0=unusable args, -1=creation/copy failed (WARN once).
    return d3d12_mc::snapshotDlssDepth(ctx, depth);
}

int32_t MCDR_CALL mcdr_dx12SetDlssQuality(int32_t tier) {
    // P108 (user m11147). The Java side turned the DLSS tier into its own button, so MCDR no
    // longer has to guess it from the render:target ratio. The device module owns the state
    // (`gCtx.dlssQualityTier`) because the renderer reads it, and it forwards the change to the
    // DLSS implementation so `ensureFeature` rebuilds the feature — PerfQualityValue is a
    // CreateFeature input, so a tier change cannot be applied by editing Evaluate parameters.
    // Returns the normalised tier, or -1 for an out-of-range value (nothing changes).
    return d3d12_mc::setDlssQuality(tier);
}

void MCDR_CALL mcdr_dx12ReflexFrameBegin(void) { mcdr_reflex_frame_begin(); }

void MCDR_CALL mcdr_dx12ReflexMarker(int32_t marker) { mcdr_reflex_marker(marker); }

void MCDR_CALL mcdr_dx12SetReflexMode(int32_t mode) { mcdr_reflex_set_mode(mode); }

int32_t MCDR_CALL mcdr_dx12ReflexAvailable(void) { return mcdr_reflex_available(); }
// P208：Streamline 的 DLSS 插件加载状态（见 mcdr_dlss_available 的注释）。
int32_t MCDR_CALL mcdr_dx12DlssAvailable(void) { return mcdr_dlss_available(); }

// P213：帧生成（DLSS-G）的开关。0 = off，1 = on（固定倍率），2 = dynamic。
// 这里只存状态；真正的 slDLSSGSetOptions 在渲染线程的 evaluate 里下发（见 d3d12_dlss.cpp）。
void MCDR_CALL mcdr_dx12SetDlssgMode(int32_t mode, int32_t numFrames) {
    mcdr_dlss_set_fg_mode(mode, numFrames);
}

// P213c：上一次 Present 真正提交的帧数，给 JVM 的 F3 帧率行用。
int32_t MCDR_CALL mcdr_dx12GetDlssgPresented(void) {
    return mcdr_dlss_fg_presented();
}

void MCDR_CALL mcdr_dx12SetReflexFrameLimit(int32_t micros) {
    mcdr_reflex_set_frame_limit(static_cast<uint32_t>(micros < 0 ? 0 : micros));
}

int32_t MCDR_CALL mcdr_dx12SetDlssConstants(const mcdr_dlss_constants* constants) {
    // P112：Streamline 的 sl::Constants 需要相机矩阵/向量/近远平面等，这些数据只在 JVM 侧
    // （D3D12Temporal）有。这里原样转交给 DLSS 实现单元缓存，等 evaluate 时喂给 slSetConstants。
    // 返回 1=已缓存，0=参数为空。
    return mcdr_dlss_set_constants(constants);
}

int32_t MCDR_CALL mcdr_dx12SetDlssRenderPreset(int32_t preset) {
    // P108ac (user m08033). The remaining noise/flicker is DLSS's own behaviour, and the user's
    // remedy is to swap the model — so the JVM gets a knob instead of us picking a default.
    // `preset` is a NVSDK_NGX_DLSS_Hint_Render_Preset value (0 = Default, 10 = J, 11 = K, ...);
    // the device module owns the state and forwards the change so `ensureFeature` rebuilds the
    // feature (the preset is baked in at CreateFeature time).
    // Returns the recorded preset, or -1 for an out-of-range value (nothing changes).
    return d3d12_mc::setDlssRenderPreset(preset);
}

int32_t MCDR_CALL mcdr_dx12SetDlssOutput(mcdr_object* tex) {
    // P108 (user m11147 follow-up). The Java side creates a target-sized UAV texture through the
    // module's own texture path (`USAGE_STORAGE = 32`) and registers it here, because the DLSS
    // implementation's own `CreateCommittedResource` texture proved unwritable (control test: same
    // source, site and command list — the module-created target read 17.8/16.5/11.6/255, the private
    // texture read all zero, alpha included, and still zero in the settled read).
    // The device module unwraps the handle because only it knows the `Dx12Object` layout; the DLSS
    // unit must never reinterpret a JVM handle (that caused the null-vtable crash).
    // Returns 1=accepted, 0=cleared, -1=not a live texture.
    return d3d12_mc::setDlssOutput(tex);
}

int32_t MCDR_CALL mcdr_dx12SetDlssVendorDir(const char* utf8Dir) {    // P108. The Java side announces <user.dir>\dxrmc\ (the directory it extracted MCDR.dll
    // and the vendor DLLs into). The native side widens the process DLL search path with it,
    // because that directory is not on the default search order: the process working
    // directory is the game directory while the executable is the JDK's java.exe, so the
    // bare-name and "next to the executable" lookups both miss nvngx_dlss.dll.
    // Returns 1=accepted and the search path widened, 0=empty/rejected input.
    // Implemented in src\advanced\d3d12_dlss.cpp (the plain TU that owns the NGX types).
    return mcdr_dlss_register_vendor_dir(utf8Dir, utf8Dir != nullptr ? static_cast<int32_t>(std::strlen(utf8Dir) + 1) : 0);
}

}  // extern "C"
