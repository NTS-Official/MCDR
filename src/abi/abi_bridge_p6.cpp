// abi_bridge_p6.cpp — draw-command recording inside a render pass.
//
// Replaces jni_bridge_p6.cpp (172 lines). The translation is mechanical except
// for mcdr_dx12PushDescriptors, which used to receive six parallel Java arrays
// (types/buffers/offsets/lengths/texelFormats/views). Each call therefore cost
// six Get*ArrayRegion copies plus a fresh std::vector<DrawBinding>, on a path
// that runs once per draw in the serial fallback. It is now a single struct
// array the Java side writes straight into native memory, and the conversion
// reuses a per-thread vector instead of allocating.

#include "abi_common.h"

#include "../dx12/dx12_device.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

// The adapter runs on the render thread and, during parallel bundle recording,
// on the descriptor workers. Neither re-enters the adapter for a second
// pushDescriptors, so one scratch vector per thread is both correct and
// allocation-free after the first frame.
thread_local std::vector<d3d12_mc::DrawBinding> gBindingScratch;

}  // namespace

extern "C" {

int32_t MCDR_CALL mcdr_dx12SetPipeline(mcdr_command_ctx* ctx, mcdr_pipeline* pipeline, int32_t hasDepth) {
    std::string err;
    if (!d3d12_mc::setPipeline(ctx, pipeline, hasDepth != 0, err)) {
        return mcdr::abi::failWith("mcdr_dx12SetPipeline", err);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetScissor(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                      int32_t height) {
    std::string err;
    if (!d3d12_mc::setScissor(ctx, x, y, width, height, err)) {
        return mcdr::abi::failWith("mcdr_dx12SetScissor", err);
    }
    return 1;
}

// MC 26.2: RenderPassBackend gained setViewport(int,int,int,int).
int32_t MCDR_CALL mcdr_dx12SetViewport(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                       int32_t height) {
    std::string err;
    if (!d3d12_mc::setViewport(ctx, x, y, width, height, err)) {
        return mcdr::abi::failWith("mcdr_dx12SetViewport", err);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetVertexBuffer(mcdr_command_ctx* ctx, int32_t slot, mcdr_object* buffer,
                                           int64_t offset, int32_t stride) {
    std::string err;
    if (!d3d12_mc::setVertexBuffer(ctx, slot, buffer, offset, stride, err)) {
        return mcdr::abi::failWith("mcdr_dx12SetVertexBuffer", err);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetIndexBuffer(mcdr_command_ctx* ctx, mcdr_object* buffer, int32_t indexType) {
    std::string err;
    if (!d3d12_mc::setIndexBuffer(ctx, buffer, indexType, err)) {
        return mcdr::abi::failWith("mcdr_dx12SetIndexBuffer", err);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12PushDescriptors(mcdr_command_ctx* ctx, const mcdr_binding_slot* bindings,
                                           int32_t count) {
    // The JNI version returned JNI_TRUE immediately for an empty array, before
    // touching ctx: a pipeline with no bindings must stay a no-op, not a failure.
    if (count <= 0) {
        return 1;
    }
    if (bindings == nullptr) {
        return mcdr::abi::failWith("mcdr_dx12PushDescriptors", "bindings is null");
    }

    gBindingScratch.resize(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        const mcdr_binding_slot& src = bindings[i];
        d3d12_mc::DrawBinding& dst = gBindingScratch[static_cast<size_t>(i)];
        dst.type = static_cast<uint8_t>(src.type);
        dst.buffer = src.buffer;
        dst.offset = src.offset;
        dst.length = src.length;
        dst.texelFormat = src.texelFormat;
        dst.view = src.view;
    }

    std::string err;
    if (!d3d12_mc::pushDescriptors(ctx, gBindingScratch, err)) {
        // Issue #10 in the source project: the Java call site could only report
        // "dx12PushDescriptors failed" because the real reason (descriptor heap
        // exhausted / stale view / illegal CBV) never left native code. The
        // message now crosses the boundary and Java raises it.
        return mcdr::abi::failWith("mcdr_dx12PushDescriptors", err);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12DrawIndexed(mcdr_command_ctx* ctx, int32_t indexCount, int32_t instanceCount,
                                       int32_t firstIndex, int32_t baseVertex, int32_t firstInstance) {
    std::string err;
    if (!d3d12_mc::drawIndexedInstanced(ctx, static_cast<UINT>(indexCount), static_cast<UINT>(instanceCount),
                                        static_cast<INT>(firstIndex), static_cast<INT>(baseVertex),
                                        static_cast<UINT>(firstInstance), err)) {
        return mcdr::abi::failWith("mcdr_dx12DrawIndexed", err);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12Draw(mcdr_command_ctx* ctx, int32_t vertexCount, int32_t instanceCount,
                                int32_t firstVertex, int32_t firstInstance) {
    std::string err;
    if (!d3d12_mc::drawInstanced(ctx, static_cast<UINT>(vertexCount), static_cast<UINT>(instanceCount),
                                 static_cast<UINT>(firstVertex), static_cast<UINT>(firstInstance), err)) {
        return mcdr::abi::failWith("mcdr_dx12Draw", err);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12DrawIndexedIndirect(mcdr_command_ctx* ctx, mcdr_object* commands, int64_t offset,
                                               int32_t drawCount) {
    std::string err;
    if (!d3d12_mc::drawIndexedIndirect(ctx, commands, offset, static_cast<UINT>(drawCount), err)) {
        return mcdr::abi::failWith("mcdr_dx12DrawIndexedIndirect", err);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12DrawIndirect(mcdr_command_ctx* ctx, mcdr_object* commands, int64_t offset,
                                        int32_t drawCount) {
    std::string err;
    if (!d3d12_mc::drawIndirect(ctx, commands, offset, static_cast<UINT>(drawCount), err)) {
        return mcdr::abi::failWith("mcdr_dx12DrawIndirect", err);
    }
    return 1;
}

}  // extern "C"
