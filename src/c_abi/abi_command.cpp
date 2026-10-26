// abi_command.cpp — command layer: encoder lifecycle, copies, clears, render pass, timestamp queries and the async-renderer control channel
//
// Group: command recording (originally abi_bridge_p3.cpp).
// 合并自：abi_bridge_p3.cpp
//
module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_fence;
// mcdr.error 是叶子模块，不 import 就取不到 mcdr::Error（import 不传递）。
import mcdr.error;

using namespace d3d12_mc;

// ===========================================================================
// 本文件是合并后的 C ABI 适配单元，按**功能**命名（不再按移植阶段 pX）。
// 共享前言（prelude / mcdr_abi.h / 各模块 import）统一放在文件顶部，下面每一段
// 保留原单元的文件头注释与代码本体，内容逐字节未改。
// ===========================================================================

// ---------------------------------------------------------------------------
// 来源单元：abi_bridge_p3.cpp
// ---------------------------------------------------------------------------

// abi_command.cpp — command layer: encoder lifecycle, copies, clears, render（合并自 abi_bridge_p3.cpp）
// pass, timestamp queries and the async-renderer control channel.
//
// Replaces jni_bridge_p3.cpp (421 lines). The translation is mechanical, with
// three things worth calling out:
//
//  1. Handles need no conversion at all. mcdr_object IS d3d12_mc::Dx12Object (see
//     mcdr_abi.h), so the old fromHandle(jlong) reinterpret_cast helpers and the
//     per-call `jlong -> pointer` marshalling are gone.
//
//  2. mcdr_dx12BeginRenderPass no longer copies four Java arrays. The JNI version
//     did GetLongArrayRegion + GetByteArrayRegion + GetIntArrayRegion +
//     GetFloatArrayRegion, then three `new[]` allocations and three `delete[]`.
//     The Java side now arranges the caller's data in native memory once and
//     passes pointers, so this function only forwards them.
//
//  3. dx12WaitForFence keeps its unusual contract: a *timeout* is success-path
//     semantics, not an error. Official GpuFence.awaitCompletion returns false on
//     a zero timeout rather than throwing, and StagedVertexBuffer reclamation
//     depends on that. Only a genuine failure (queue fence uninitialised,
//     CreateEvent failure) sets the last error.

extern "C" {

int64_t MCDR_CALL MCDR_D3D12_get_timestamp_frequency(void) {
    return static_cast<int64_t>(d3d12_mc::getTimestampFrequency());
}

mcdr_command_ctx* MCDR_CALL MCDR_D3D12_create_command_encoder(void) {
    const auto r = d3d12_mc::createCommandEncoder();
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::CommandContext>("MCDR_D3D12_create_command_encoder",
                                                               r.error().message);
    }
    return static_cast<d3d12_mc::CommandContext*>(*r);
}

void MCDR_CALL mcdr_dx12DestroyCommandEncoder(mcdr_command_ctx* ctx) { d3d12_mc::destroyCommandEncoder(ctx); }

void MCDR_CALL mcdr_dx12BeginCommandList(mcdr_command_ctx* ctx) {
    if (const auto r = d3d12_mc::beginCommandList(ctx); !r) {
        mcdr::abi::failVoid("mcdr_dx12BeginCommandList", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12BeginCommandListWithWait(mcdr_command_ctx* ctx, uint64_t waitForValue) {
    if (const auto r = d3d12_mc::beginCommandListWithWait(ctx, waitForValue); !r) {
        mcdr::abi::failVoid("mcdr_dx12BeginCommandListWithWait", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12EndCommandList(mcdr_command_ctx* ctx) {
    if (const auto r = d3d12_mc::endCommandList(ctx); !r) {
        mcdr::abi::failVoid("mcdr_dx12EndCommandList", r.error().message);
    }
}

uint64_t MCDR_CALL mcdr_dx12Submit(mcdr_command_ctx* ctx) {
    d3d12_mc::dbgLogDebug("submit: enter ctx=%p", static_cast<void*>(ctx));
    const auto r = d3d12_mc::submitCommandList(ctx);
    // submitCommandList 只在失败时返回 err；返回值是本 ctx 的 per-command-list 序号
    // （BUG-03，可直接交给 mcdr_dx12WaitForFence），「无可提交」时是 0。
    if (!r && !r.error().message.empty()) {
        mcdr::abi::failVoid("mcdr_dx12Submit", r.error().message);
    }
    return static_cast<uint64_t>(r.value_or(0));
}

int32_t MCDR_CALL mcdr_dx12WaitForFence(mcdr_command_ctx* ctx, uint64_t value, int64_t timeoutNs) {
    // BUG-03：等待的是**这个 ctx 自己的** per-command-list fence。value 来自
    // mcdr_dx12GetFenceValue / mcdr_dx12Submit，三者属于同一条 per-ctx 时间线，
    // 因此帧 encoder 的 token 只会被本帧提交满足，一次性 encoder（createBuffer(data)
    // 的上传）不会提前把它推进。
    const auto r = d3d12_mc::waitForContextFenceValue(ctx, static_cast<UINT64>(value),
                                                     static_cast<UINT64>(timeoutNs));
    // 超时不是错误（GpuFence.awaitCompletion 语义），只有真正的失败才设 lastError。
    if (!r && r.error().message.find("timed out") == std::string::npos) {
        mcdr::abi::failVoid("mcdr_dx12WaitForFence", r.error().message);
    }
    return r ? 1 : 0;
}

uint64_t MCDR_CALL mcdr_dx12GetFenceValue(mcdr_command_ctx* ctx) {
    // createFence() 捕获的是本 ctx 的当前 per-command-list 序号，target = 该值 + 1；
    // 等待对象是同一个 ctx 的 fence（BUG-03）。
    return static_cast<uint64_t>(d3d12_mc::currentFenceValue(ctx));
}

int64_t MCDR_CALL mcdr_dx12GetTimestampNow(mcdr_command_ctx* ctx) {
    const auto r = d3d12_mc::getTimestampNow(ctx);
    if (!r && !r.error().message.empty()) {
        mcdr::abi::failVoid("mcdr_dx12GetTimestampNow", r.error().message);
    }
    return static_cast<int64_t>(r.value_or(0));
}

int32_t MCDR_CALL mcdr_dx12IsListOpen(mcdr_command_ctx* ctx) { return d3d12_mc::isListOpen(ctx) ? 1 : 0; }

void MCDR_CALL mcdr_dx12CopyBuffer(mcdr_command_ctx* ctx, mcdr_object* src, int64_t srcOffset,
                                   mcdr_object* dst, int64_t dstOffset, int64_t size) {
    DBG_LOG_DEBUG("copyBuffer: src=%p(%lld) dst=%p(%lld) size=%lld", static_cast<void*>(src),
                  (long long)srcOffset, static_cast<void*>(dst), (long long)dstOffset, (long long)size);
    if (const auto r = d3d12_mc::copyBufferToBuffer(ctx, src, srcOffset, dst, dstOffset, size); !r) {
        mcdr::abi::failVoid("mcdr_dx12CopyBuffer", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12WriteToTexture(mcdr_command_ctx* ctx, mcdr_object* stagingBuffer,
                                       int64_t stagingOffset, int32_t width, int32_t height,
                                       mcdr_object* dstTexture, int32_t mip, int32_t layer, int32_t dstX,
                                       int32_t dstY) {
    if (const auto r = d3d12_mc::copyBufferToTexture(ctx, stagingBuffer, stagingOffset, width, height, dstTexture,
                                                     mip, layer, dstX, dstY, width, height);
        !r) {
        mcdr::abi::failVoid("mcdr_dx12WriteToTexture", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12CopyBufferToTexture(mcdr_command_ctx* ctx, mcdr_object* srcBuffer, int64_t srcOffset,
                                            int32_t srcWidth, int32_t srcHeight, mcdr_object* dstTexture,
                                            int32_t mip, int32_t layer, int32_t dstX, int32_t dstY,
                                            int32_t width, int32_t height) {
    if (const auto r =
            d3d12_mc::copyBufferToTexture(ctx, srcBuffer, srcOffset, srcWidth, srcHeight, dstTexture, mip, layer,
                                          dstX, dstY, width, height);
        !r) {
        mcdr::abi::failVoid("mcdr_dx12CopyBufferToTexture", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12CopyTextureToBuffer(mcdr_command_ctx* ctx, mcdr_object* srcTexture, int32_t mip,
                                            int32_t layer, int32_t srcX, int32_t srcY, int32_t width,
                                            int32_t height, mcdr_object* dstBuffer, int64_t dstOffset) {
    if (const auto r = d3d12_mc::copyTextureToBuffer(ctx, srcTexture, mip, layer, srcX, srcY, width, height,
                                                    dstBuffer, dstOffset);
        !r) {
        mcdr::abi::failVoid("mcdr_dx12CopyTextureToBuffer", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12CopyTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
                                             mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                             int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY,
                                             int32_t width, int32_t height) {
    if (const auto r = d3d12_mc::copyTextureToTexture(ctx, srcTexture, dstTexture, mip, layer, srcX, srcY, dstX,
                                                      dstY, width, height);
        !r) {
        mcdr::abi::failVoid("mcdr_dx12CopyTextureToTexture", r.error().message);
    }
}

// P104b: in-place SMAA on a texture (the world layer). The Java side calls this from a
// GameRenderer mixin, right after the world has rendered and before the GUI is drawn, so
// the anti-aliasing never touches UI. Silence here is normal: mode != SMAA is a no-op.
int32_t MCDR_CALL mcdr_dx12ApplySmaaToTexture(mcdr_command_ctx* ctx, mcdr_object* texture) {
    return d3d12_mc::applySmaaToTexture(ctx, texture) ? 1 : 0;
}

// P106: GUI target split. The Java side calls this once per frame to hand the finished world
// layer over to the GUI's own window-sized render target (see blitTextureToTexture).
int32_t MCDR_CALL mcdr_dx12BlitTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* src,
                                                mcdr_object* dst) {
    const auto r = d3d12_mc::blitTextureToTexture(ctx, src, dst);
    if (!r) {
        mcdr::abi::failVoid("mcdr_dx12BlitTextureToTexture", r.error().message);
        return 0;
    }
    return 1;
}

// P121: 后置锐化（TAA / DLAA / DLSS 共用）。src -> dst 的 5-tap unsharp，amount 取 [0,1]。
// 与 blit 同一调用点语义，但要求 src != dst（同资源就地锐化不合法，native 会返回 0）。
int32_t MCDR_CALL mcdr_dx12SharpenTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* src,
                                                   mcdr_object* dst, float amount) {
    const auto r = d3d12_mc::sharpenTextureToTexture(ctx, src, dst, amount);
    if (!r) {
        mcdr::abi::failVoid("mcdr_dx12SharpenTextureToTexture", r.error().message);
        return 0;
    }
    return 1;
}

// P152: TAA resolve, native side. Same call point as SMAA (world drawn, GUI not yet), but the
// history textures, the resolve pipeline and the jitter bookkeeping live in the device context,
// so the JVM only pushes the per-frame values. Returns 1 = applied, 0 = skipped (mode is not
// TAA / unsuitable target / already disabled), -1 = failed (one ERROR log, TAA disabled).
int32_t MCDR_CALL mcdr_dx12ApplyTaaToTexture(mcdr_command_ctx* ctx, mcdr_object* scene,
                                             mcdr_object* motion, float jitterX, float jitterY,
                                             float prevJitterX, float prevJitterY, float blend,
                                             int32_t reset, float sharpenAmount) {
    return d3d12_mc::applyTaaToTexture(ctx, scene, motion, jitterX, jitterY, prevJitterX, prevJitterY,
                                       blend, reset, sharpenAmount);
}

// P108: DLSS. Same call point as mcdr_dx12ApplySmaaToTexture (world done, GUI not yet):
// the world layer is upscaled from the render size into the window-sized target, which is
// what the linear blit used to do. Silenced by the independent DLSS switch being off
// (mcdr_dx12SetDlssEnabled — P108 / user m10920 decoupled it from the AA mode), and by any
// earlier failure (which has already been logged exactly once).
int32_t MCDR_CALL mcdr_dx12CreateDlssFeature(mcdr_command_ctx* ctx, const mcdr_dlss_input* in) {
    if (ctx == nullptr || in == nullptr) {
        return -1;
    }
    // Create and Evaluate are one entry point on purpose: NGX may only create the feature
    // while a command list is open, and the Java side has exactly that one window per
    // frame (after the world, before the GUI).
    return d3d12_mc::applyDlssToTexture(ctx, *in) ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12EvaluateDlss(mcdr_command_ctx* ctx, const mcdr_dlss_input* in) {
    if (ctx == nullptr || in == nullptr) {
        return -1;
    }
    return d3d12_mc::applyDlssToTexture(ctx, *in) ? 1 : 0;
}

// P108: one-line DLSS availability/state query for the UI and the log.
int32_t MCDR_CALL mcdr_dx12QueryDlss(char* note, int32_t noteCap) {
    if (noteCap < 0 || (noteCap > 0 && note == nullptr)) {
        return -1;
    }
    return d3d12_mc::queryDlss(note, static_cast<int>(noteCap));
}

void MCDR_CALL mcdr_dx12ClearColorTexture(mcdr_command_ctx* ctx, mcdr_object* texture, float r, float g,
                                          float b, float a) {
    const auto res = d3d12_mc::clearColorTexture(ctx, texture, r, g, b, a);
    if (!res) {
        mcdr::abi::failVoid("mcdr_dx12ClearColorTexture", res.error().message);
    }
}

void MCDR_CALL mcdr_dx12ClearDepthTexture(mcdr_command_ctx* ctx, mcdr_object* texture, double depth) {
    if (const auto r = d3d12_mc::clearDepthTexture(ctx, texture, depth); !r) {
        mcdr::abi::failVoid("mcdr_dx12ClearDepthTexture", r.error().message);
    }
}

// MC 26.2: CommandEncoderBackend gained clearStencilTexture(GpuTexture, int).
int32_t MCDR_CALL mcdr_dx12ClearStencilTexture(mcdr_command_ctx* ctx, mcdr_object* texture, int32_t value) {
    if (const auto r = d3d12_mc::clearStencilTexture(ctx, texture, static_cast<int>(value)); !r) {
        return mcdr::abi::failWith("mcdr_dx12ClearStencilTexture", r.error().message);
    }
    return 1;
}

// P3b: region clear. GuiItemAtlas clears one slot before a STALE redraw; the
// original whole-texture clear wiped already-baked item icons, which then never
// re-rendered after paging back, so items disappeared.
void MCDR_CALL mcdr_dx12ClearColorTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture, float r, float g,
                                                float b, float a, int32_t x, int32_t y, int32_t width,
                                                int32_t height) {
    const auto res =
        d3d12_mc::clearColorTextureRegion(ctx, texture, r, g, b, a, x, y, width, height);
    if (!res) {
        mcdr::abi::failVoid("mcdr_dx12ClearColorTextureRegion", res.error().message);
    }
}

void MCDR_CALL mcdr_dx12ClearDepthTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture, double depth,
                                                int32_t x, int32_t y, int32_t width, int32_t height) {
    if (const auto r = d3d12_mc::clearDepthTextureRegion(ctx, texture, depth, x, y, width, height); !r) {
        mcdr::abi::failVoid("mcdr_dx12ClearDepthTextureRegion", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12BeginRenderPass(mcdr_command_ctx* ctx, mcdr_object* const* colorTextures,
                                        int32_t colorCount, const int32_t* colorMips,
                                        const int32_t* colorClearFlags, const float* clearColors,
                                        mcdr_object* depthTexture, int32_t depthMip, int32_t depthClearFlag,
                                        double depthClearValue, int32_t x, int32_t y, int32_t width,
                                        int32_t height) {
    if (const auto r = d3d12_mc::beginRenderPass(ctx, colorTextures, colorCount, colorMips, colorClearFlags,
                                                 clearColors, depthTexture, depthMip, depthClearFlag,
                                                 depthClearValue, x, y, width, height);
        !r) {
        mcdr::abi::failVoid("mcdr_dx12BeginRenderPass", r.error().message);
    }
}

void MCDR_CALL mcdr_dx12EndRenderPass(mcdr_command_ctx* ctx) {
    if (const auto r = d3d12_mc::endRenderPass(ctx); !r) {
        mcdr::abi::failVoid("mcdr_dx12EndRenderPass", r.error().message);
    }
}

mcdr_object* MCDR_CALL mcdr_dx12GetActiveColorTexture(mcdr_command_ctx* ctx) {
    return reinterpret_cast<mcdr_object*>(d3d12_mc::getActiveColorTextureHandle(ctx));
}

mcdr_query_pool* MCDR_CALL mcdr_dx12CreateQueryPool(int32_t size) {
    const auto r = d3d12_mc::createQueryPool(size);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::QueryPool>("mcdr_dx12CreateQueryPool", r.error().message);
    }
    return static_cast<d3d12_mc::QueryPool*>(*r);
}

void MCDR_CALL mcdr_dx12DestroyQueryPool(mcdr_query_pool* pool) { d3d12_mc::destroyQueryPool(pool); }

void MCDR_CALL mcdr_dx12WriteTimestamp(mcdr_command_ctx* ctx, mcdr_query_pool* pool, int32_t index) {
    if (const auto r = d3d12_mc::writeTimestampToPool(ctx, pool, index); !r) {
        mcdr::abi::failVoid("mcdr_dx12WriteTimestamp", r.error().message);
    }
}

uint64_t MCDR_CALL mcdr_dx12ReadQueryValue(mcdr_query_pool* pool, int32_t index) {
    long long out = 0;
    if (const auto r = d3d12_mc::readQueryValue(pool, index, out); !r) {
        mcdr::abi::failVoid("mcdr_dx12ReadQueryValue", r.error().message);
        return 0;
    }
    return static_cast<uint64_t>(out);
}

int32_t MCDR_CALL mcdr_dx12ReadQueryValues(mcdr_query_pool* pool, int32_t start, int32_t count,
                                           int64_t* out) {
    if (count <= 0) {
        return 1;
    }
    if (!out) {
        return mcdr::abi::failWith("mcdr_dx12ReadQueryValues", "out buffer is null");
    }
    if (const auto r =
            d3d12_mc::readQueryValues(pool, start, count, reinterpret_cast<long long*>(out));
        !r) {
        return mcdr::abi::failWith("mcdr_dx12ReadQueryValues", r.error().message);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// P33 async: dedicated render thread control channel
// ---------------------------------------------------------------------------

void MCDR_CALL mcdr_dx12InitAsyncRenderer(int32_t workerCount) {
    // initAsyncRenderer 仍然返回 bool（设备侧把它归入「无法失败」一类）。
    if (!d3d12_mc::initAsyncRenderer(static_cast<UINT>(workerCount))) {
        mcdr::abi::failVoid("mcdr_dx12InitAsyncRenderer", std::string());
    }
}

void MCDR_CALL mcdr_dx12DestroyAsyncRenderer(void) { d3d12_mc::destroyAsyncRenderer(); }

int32_t MCDR_CALL mcdr_dx12AsyncRenderBeginFrame(mcdr_command_ctx* ctx) {
    const auto r = d3d12_mc::asyncRenderBeginFrame(ctx);
    if (!r && !r.error().message.empty()) {
        mcdr::abi::failVoid("mcdr_dx12AsyncRenderBeginFrame", r.error().message);
    }
    return r ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitComplete(mcdr_command_ctx* ctx, int64_t timeoutMs) {
    const auto r = d3d12_mc::asyncRenderWaitComplete(ctx, static_cast<UINT64>(timeoutMs));
    if (!r && !r.error().message.empty()) {
        mcdr::abi::failVoid("mcdr_dx12AsyncRenderWaitComplete", r.error().message);
    }
    return r ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncRenderIsRecordingReady(mcdr_command_ctx* ctx) {
    return d3d12_mc::asyncRenderIsRecordingReady(ctx) ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitRecordingReady(mcdr_command_ctx* ctx, int64_t timeoutMs) {
    // Replaces a Java-side Thread.sleep(1) poll worth ~1 ms/frame.
    return d3d12_mc::asyncRenderWaitRecordingReady(ctx, static_cast<UINT64>(timeoutMs)) ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncSendCommandsReady(mcdr_command_ctx* ctx) {
    d3d12_mc::asyncSendCommandsReady(ctx);
    return 1;
}

void MCDR_CALL mcdr_dx12ClearAsyncRenderCtx(void) { d3d12_mc::clearAsyncRenderCtx(); }

// P234：compute 派发。返回码 1/0/-2 由 native 决定（-2 = 没有打开的命令列表）。
int32_t MCDR_CALL mcdr_dx12DispatchCompute(mcdr_command_ctx* ctx, mcdr_object* pipeline,
                                           mcdr_object* uavTexture, const int32_t* constants,
                                           int32_t constantCount, int32_t x, int32_t y, int32_t z) {
    return d3d12_mc::dispatchCompute(ctx, pipeline, uavTexture, constants, constantCount, x, y, z);
}


// P235：着色率图像的派发与绑定（1=成功 0=参数/设备问题 -1=坏句柄 -2=列表没开）。
// P276：通用 compute 纹理 pass 的派发（转发给 SRI 那条实现，根签名相同）。
int32_t MCDR_CALL mcdr_dx12DispatchComputeTexture3(mcdr_command_ctx* ctx, mcdr_object* pipeline,
                                              mcdr_object* srv0, mcdr_object* srv1,
                                              mcdr_object* srv2, mcdr_object* image,
                                              const int32_t* constants, int32_t constantCount,
                                              int32_t x, int32_t y, int32_t z) {
    return d3d12_mc::dispatchComputeShadingRate(ctx, pipeline, srv0, srv1, srv2, image, constants,
                                                constantCount, x, y, z);
}

int32_t MCDR_CALL mcdr_dx12DispatchComputeShadingRate(mcdr_command_ctx* ctx, mcdr_object* pipeline,
                                                      mcdr_object* srvDepth, mcdr_object* srvMotion,
                                                      mcdr_object* srvPrevious,
                                                      mcdr_object* image, const int32_t* constants,
                                                      int32_t constantCount, int32_t x, int32_t y,
                                                      int32_t z) {
    return d3d12_mc::dispatchComputeShadingRate(ctx, pipeline, srvDepth, srvMotion, srvPrevious,
                                                image, constants, constantCount, x, y, z);
}

int32_t MCDR_CALL mcdr_dx12SetShadingRateImage(mcdr_command_ctx* ctx, mcdr_object* image) {
    return d3d12_mc::setShadingRateImage(ctx, image);
}
}  // extern "C"
