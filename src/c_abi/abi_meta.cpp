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

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

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

int32_t MCDR_CALL mcdrSetPixCapture(int32_t enabled) {
    // 1=loaded, 0=not requested, -1=requested but not loaded. Loading the capture
    // layer is irreversible for the process (PIX hooks D3D12 from inside it), so the
    // decision belongs to the Java side and has to arrive before the first device.
    return d3d12_mc::setPixCapture(enabled != 0);
}

int32_t MCDR_CALL mcdrSetShadingRate(int32_t rate) {
    // P97. rate is a raw D3D12_SHADING_RATE (1X1=0x0 1X2=0x1 2X1=0x4 2X2=0x5 2X4=0x6
    // 4X2=0x9 4X4=0xA), -1 = off. Returns 1=accepted (including "the device does not
    // exist yet, so it will be gated on the first command list"), 0=this device cannot
    // honour the rate (tier 0, or an additional rate that is not supported), -1=unknown
    // rate value.
    return d3d12_mc::setShadingRate(rate);
}

int32_t MCDR_CALL mcdrSetAntiAliasing(int32_t mode) {
    // P104/P108. mode mirrors the Java-side option: **0=OFF 1=SMAA 2=TAA 3=DLAA**. Only SMAA
    // is implemented (three passes glued in place); TAA/DLAA are accepted and downgraded to
    // OFF with a WARN.
    // P108 (user m10920): DLSS is NO LONGER a value here — it has its own switch, see
    // mcdr_dx12SetDlssEnabled. Anti-aliasing and DLSS are independent.
    // Returns 1=in effect, 0=accepted but not implemented, -1=unknown mode.
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

int32_t MCDR_CALL mcdr_dx12SetDlssVendorDir(const char* utf8Dir) {
    // P108. The Java side announces <user.dir>\dxrmc\ (the directory it extracted MCDR.dll
    // and the vendor DLLs into). The native side widens the process DLL search path with it,
    // because that directory is not on the default search order: the process working
    // directory is the game directory while the executable is the JDK's java.exe, so the
    // bare-name and "next to the executable" lookups both miss nvngx_dlss.dll.
    // Returns 1=accepted and the search path widened, 0=empty/rejected input.
    // Implemented in src\advanced\d3d12_dlss.cpp (the plain TU that owns the NGX types).
    return mcdr_dlss_register_vendor_dir(utf8Dir, utf8Dir != nullptr ? static_cast<int32_t>(std::strlen(utf8Dir) + 1) : 0);
}

}  // extern "C"
