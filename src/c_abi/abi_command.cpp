

// abi_command.cpp — command layer: encoder lifecycle, copies, clears, render pass, timestamp queries and the async-renderer control channel
//
// Group: command recording (originally abi_bridge_p3.cpp).
// 合并自：abi_bridge_p3.cpp
//
#include "_d3d12_module_prelude.h"

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_fence;
import mcdr.abi.common;

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
    std::string err;
    d3d12_mc::CommandContext* ctx = d3d12_mc::createCommandEncoder(err);
    if (!ctx) {
        return mcdr::abi::failWithPtr<d3d12_mc::CommandContext>("MCDR_D3D12_create_command_encoder", err);
    }
    return ctx;
}

void MCDR_CALL mcdr_dx12DestroyCommandEncoder(mcdr_command_ctx* ctx) { d3d12_mc::destroyCommandEncoder(ctx); }

void MCDR_CALL mcdr_dx12BeginCommandList(mcdr_command_ctx* ctx) {
    std::string err;
    if (!d3d12_mc::beginCommandList(ctx, err)) {
        mcdr::abi::failVoid("mcdr_dx12BeginCommandList", err);
    }
}

void MCDR_CALL mcdr_dx12BeginCommandListWithWait(mcdr_command_ctx* ctx, uint64_t waitForValue) {
    std::string err;
    if (!d3d12_mc::beginCommandListWithWait(ctx, waitForValue, err)) {
        mcdr::abi::failVoid("mcdr_dx12BeginCommandListWithWait", err);
    }
}

void MCDR_CALL mcdr_dx12EndCommandList(mcdr_command_ctx* ctx) {
    std::string err;
    if (!d3d12_mc::endCommandList(ctx, err)) {
        mcdr::abi::failVoid("mcdr_dx12EndCommandList", err);
    }
}

uint64_t MCDR_CALL mcdr_dx12Submit(mcdr_command_ctx* ctx) {
    d3d12_mc::dbgLog("submit: enter ctx=%p", static_cast<void*>(ctx));
    std::string err;
    const UINT64 value = d3d12_mc::submitCommandList(ctx, err);
    // submitCommandList returns 0 only on failure, and only then does it fill
    // err; a 0 with an empty err is the documented "nothing to submit" case.
    if (value == 0 && !err.empty()) {
        mcdr::abi::failVoid("mcdr_dx12Submit", err);
    }
    return static_cast<uint64_t>(value);
}

int32_t MCDR_CALL mcdr_dx12WaitForFence(mcdr_command_ctx* ctx, uint64_t value, int64_t timeoutNs) {
    (void)ctx;  // waits on the device-level queue fence, deliberately not per-ctx
    std::string err;
    // createFence token awaitCompletion: waits on the device-level queue fence
    // (target = queueFenceValue+1 at creation, completed by the next submit from
    // ANY ctx), not the per-ctx fence. A one-shot encoder is never submitted, so
    // a per-ctx wait would never complete — that was the frozen-black-screen root
    // cause, visible as a stream of `waitFence: value=1 completed=0`.
    const bool ok = d3d12_mc::waitForQueueFenceValue(value, static_cast<UINT64>(timeoutNs), err);
    if (!ok && !err.empty() && err.find("timed out") == std::string::npos) {
        mcdr::abi::failVoid("mcdr_dx12WaitForFence", err);
    }
    return ok ? 1 : 0;
}

uint64_t MCDR_CALL mcdr_dx12GetFenceValue(mcdr_command_ctx* ctx) {
    (void)ctx;
    // createFence captures the global queue fence value (target = current + 1),
    // mirroring the official shared encoder's currentSubmitIndex: a one-shot
    // encoder's token also completes with the next submit.
    return static_cast<uint64_t>(d3d12_mc::currentQueueFenceValue());
}

int64_t MCDR_CALL mcdr_dx12GetTimestampNow(mcdr_command_ctx* ctx) {
    std::string err;
    const long long ts = d3d12_mc::getTimestampNow(ctx, err);
    if (ts == 0 && !err.empty()) {
        mcdr::abi::failVoid("mcdr_dx12GetTimestampNow", err);
    }
    return static_cast<int64_t>(ts);
}

int32_t MCDR_CALL mcdr_dx12IsListOpen(mcdr_command_ctx* ctx) { return d3d12_mc::isListOpen(ctx) ? 1 : 0; }

void MCDR_CALL mcdr_dx12CopyBuffer(mcdr_command_ctx* ctx, mcdr_object* src, int64_t srcOffset,
                                   mcdr_object* dst, int64_t dstOffset, int64_t size) {
    DBG_LOG_DEBUG("copyBuffer: src=%p(%lld) dst=%p(%lld) size=%lld", static_cast<void*>(src),
                  (long long)srcOffset, static_cast<void*>(dst), (long long)dstOffset, (long long)size);
    std::string err;
    if (!d3d12_mc::copyBufferToBuffer(ctx, src, srcOffset, dst, dstOffset, size, err)) {
        mcdr::abi::failVoid("mcdr_dx12CopyBuffer", err);
    }
}

void MCDR_CALL mcdr_dx12WriteToTexture(mcdr_command_ctx* ctx, mcdr_object* stagingBuffer,
                                       int64_t stagingOffset, int32_t width, int32_t height,
                                       mcdr_object* dstTexture, int32_t mip, int32_t layer, int32_t dstX,
                                       int32_t dstY) {
    std::string err;
    // Source and destination extent are the same here by definition: the caller
    // hands over a staging buffer holding exactly one width x height image.
    if (!d3d12_mc::copyBufferToTexture(ctx, stagingBuffer, stagingOffset, width, height, dstTexture, mip,
                                       layer, dstX, dstY, width, height, err)) {
        mcdr::abi::failVoid("mcdr_dx12WriteToTexture", err);
    }
}

void MCDR_CALL mcdr_dx12CopyBufferToTexture(mcdr_command_ctx* ctx, mcdr_object* srcBuffer, int64_t srcOffset,
                                            int32_t srcWidth, int32_t srcHeight, mcdr_object* dstTexture,
                                            int32_t mip, int32_t layer, int32_t dstX, int32_t dstY,
                                            int32_t width, int32_t height) {
    std::string err;
    if (!d3d12_mc::copyBufferToTexture(ctx, srcBuffer, srcOffset, srcWidth, srcHeight, dstTexture, mip, layer,
                                       dstX, dstY, width, height, err)) {
        mcdr::abi::failVoid("mcdr_dx12CopyBufferToTexture", err);
    }
}

void MCDR_CALL mcdr_dx12CopyTextureToBuffer(mcdr_command_ctx* ctx, mcdr_object* srcTexture, int32_t mip,
                                            int32_t layer, int32_t srcX, int32_t srcY, int32_t width,
                                            int32_t height, mcdr_object* dstBuffer, int64_t dstOffset) {
    std::string err;
    if (!d3d12_mc::copyTextureToBuffer(ctx, srcTexture, mip, layer, srcX, srcY, width, height, dstBuffer,
                                       dstOffset, err)) {
        mcdr::abi::failVoid("mcdr_dx12CopyTextureToBuffer", err);
    }
}

void MCDR_CALL mcdr_dx12CopyTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
                                             mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                             int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY,
                                             int32_t width, int32_t height) {
    std::string err;
    if (!d3d12_mc::copyTextureToTexture(ctx, srcTexture, dstTexture, mip, layer, srcX, srcY, dstX, dstY,
                                        width, height, err)) {
        mcdr::abi::failVoid("mcdr_dx12CopyTextureToTexture", err);
    }
}

void MCDR_CALL mcdr_dx12ClearColorTexture(mcdr_command_ctx* ctx, mcdr_object* texture, float r, float g,
                                          float b, float a) {
    std::string err;
    if (!d3d12_mc::clearColorTexture(ctx, texture, r, g, b, a, err)) {
        mcdr::abi::failVoid("mcdr_dx12ClearColorTexture", err);
    }
}

void MCDR_CALL mcdr_dx12ClearDepthTexture(mcdr_command_ctx* ctx, mcdr_object* texture, double depth) {
    std::string err;
    if (!d3d12_mc::clearDepthTexture(ctx, texture, depth, err)) {
        mcdr::abi::failVoid("mcdr_dx12ClearDepthTexture", err);
    }
}

// MC 26.2: CommandEncoderBackend gained clearStencilTexture(GpuTexture, int).
int32_t MCDR_CALL mcdr_dx12ClearStencilTexture(mcdr_command_ctx* ctx, mcdr_object* texture, int32_t value) {
    std::string err;
    if (!d3d12_mc::clearStencilTexture(ctx, texture, static_cast<int>(value), err)) {
        return mcdr::abi::failWith("mcdr_dx12ClearStencilTexture", err);
    }
    return 1;
}

// P3b: region clear. GuiItemAtlas clears one slot before a STALE redraw; the
// original whole-texture clear wiped already-baked item icons, which then never
// re-rendered after paging back, so items disappeared.
void MCDR_CALL mcdr_dx12ClearColorTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture, float r, float g,
                                                float b, float a, int32_t x, int32_t y, int32_t width,
                                                int32_t height) {
    std::string err;
    if (!d3d12_mc::clearColorTextureRegion(ctx, texture, r, g, b, a, x, y, width, height, err)) {
        mcdr::abi::failVoid("mcdr_dx12ClearColorTextureRegion", err);
    }
}

void MCDR_CALL mcdr_dx12ClearDepthTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture, double depth,
                                                int32_t x, int32_t y, int32_t width, int32_t height) {
    std::string err;
    if (!d3d12_mc::clearDepthTextureRegion(ctx, texture, depth, x, y, width, height, err)) {
        mcdr::abi::failVoid("mcdr_dx12ClearDepthTextureRegion", err);
    }
}

void MCDR_CALL mcdr_dx12BeginRenderPass(mcdr_command_ctx* ctx, mcdr_object* const* colorTextures,
                                        int32_t colorCount, const int32_t* colorMips,
                                        const int32_t* colorClearFlags, const float* clearColors,
                                        mcdr_object* depthTexture, int32_t depthMip, int32_t depthClearFlag,
                                        double depthClearValue, int32_t x, int32_t y, int32_t width,
                                        int32_t height) {
    std::string err;
    // colorCount == 0 is legal (a depth-only pass), and NULL entries inside
    // colorTextures are legal too — they mirror the official
    // withUnusedColorAttachment() and are skipped by the core.
    if (!d3d12_mc::beginRenderPass(ctx, colorTextures, colorCount, colorMips, colorClearFlags, clearColors,
                                   depthTexture, depthMip, depthClearFlag, depthClearValue, x, y, width,
                                   height, err)) {
        mcdr::abi::failVoid("mcdr_dx12BeginRenderPass", err);
    }
}

void MCDR_CALL mcdr_dx12EndRenderPass(mcdr_command_ctx* ctx) {
    std::string err;
    // This must return every attachment from RENDER_TARGET/DEPTH_WRITE to COMMON:
    // those are non-promotable states and do NOT decay when a command list
    // completes (only COPY_SOURCE/COPY_DEST/UAV do). Skipping it desynchronises
    // the next list's barrier Before states — a validation ERROR every frame,
    // then GPU state corruption and a TDR freeze that takes the launcher down too.
    if (!d3d12_mc::endRenderPass(ctx, err)) {
        mcdr::abi::failVoid("mcdr_dx12EndRenderPass", err);
    }
}

mcdr_object* MCDR_CALL mcdr_dx12GetActiveColorTexture(mcdr_command_ctx* ctx) {
    return reinterpret_cast<mcdr_object*>(d3d12_mc::getActiveColorTextureHandle(ctx));
}

mcdr_query_pool* MCDR_CALL mcdr_dx12CreateQueryPool(int32_t size) {
    std::string err;
    d3d12_mc::QueryPool* pool = d3d12_mc::createQueryPool(size, err);
    if (!pool) {
        return mcdr::abi::failWithPtr<d3d12_mc::QueryPool>("mcdr_dx12CreateQueryPool", err);
    }
    return pool;
}

void MCDR_CALL mcdr_dx12DestroyQueryPool(mcdr_query_pool* pool) { d3d12_mc::destroyQueryPool(pool); }

void MCDR_CALL mcdr_dx12WriteTimestamp(mcdr_command_ctx* ctx, mcdr_query_pool* pool, int32_t index) {
    std::string err;
    if (!d3d12_mc::writeTimestampToPool(ctx, pool, index, err)) {
        mcdr::abi::failVoid("mcdr_dx12WriteTimestamp", err);
    }
}

uint64_t MCDR_CALL mcdr_dx12ReadQueryValue(mcdr_query_pool* pool, int32_t index) {
    std::string err;
    long long out = 0;
    if (!d3d12_mc::readQueryValue(pool, index, out, err)) {
        mcdr::abi::failVoid("mcdr_dx12ReadQueryValue", err);
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
    std::string err;
    // The JNI version read into a std::vector<long long> and then copied it into
    // the Java array. int64_t and long long are the same type on MSVC, so the
    // core can now write straight into the caller's buffer.
    if (!d3d12_mc::readQueryValues(pool, start, count, reinterpret_cast<long long*>(out), err)) {
        return mcdr::abi::failWith("mcdr_dx12ReadQueryValues", err);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// P33 async: dedicated render thread control channel
// ---------------------------------------------------------------------------

void MCDR_CALL mcdr_dx12InitAsyncRenderer(int32_t workerCount) {
    std::string err;
    if (!d3d12_mc::initAsyncRenderer(static_cast<UINT>(workerCount))) {
        mcdr::abi::failVoid("mcdr_dx12InitAsyncRenderer", err);
    }
}

void MCDR_CALL mcdr_dx12DestroyAsyncRenderer(void) { d3d12_mc::destroyAsyncRenderer(); }

int32_t MCDR_CALL mcdr_dx12AsyncRenderBeginFrame(mcdr_command_ctx* ctx) {
    std::string err;
    const bool ok = d3d12_mc::asyncRenderBeginFrame(ctx, err);
    if (!ok && !err.empty()) {
        mcdr::abi::failVoid("mcdr_dx12AsyncRenderBeginFrame", err);
    }
    return ok ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitComplete(mcdr_command_ctx* ctx, int64_t timeoutMs) {
    std::string err;
    const bool ok = d3d12_mc::asyncRenderWaitComplete(ctx, static_cast<UINT64>(timeoutMs), err);
    if (!ok && !err.empty()) {
        mcdr::abi::failVoid("mcdr_dx12AsyncRenderWaitComplete", err);
    }
    return ok ? 1 : 0;
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

}  // extern "C"
