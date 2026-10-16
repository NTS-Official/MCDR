// abi_draw.cpp — draw-command recording inside a render pass
//
// Group: draw calls (originally abi_bridge_p6.cpp).
// 合并自：abi_bridge_p6.cpp
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
// 来源单元：abi_bridge_p6.cpp
// ---------------------------------------------------------------------------

// abi_draw.cpp — draw-command recording inside a render pass.（合并自 abi_bridge_p6.cpp）
//
// Replaces jni_bridge_p6.cpp (172 lines). The translation is mechanical except
// for mcdr_dx12PushDescriptors, which used to receive six parallel Java arrays
// (types/buffers/offsets/lengths/texelFormats/views). Each call therefore cost
// six Get*ArrayRegion copies plus a fresh std::vector<DrawBinding>, on a path
// that runs once per draw in the serial fallback. It is now a single struct
// array the Java side writes straight into native memory, and the conversion
// reuses a per-thread vector instead of allocating.

namespace {

// The adapter runs on the render thread and, during parallel bundle recording,
// on the descriptor workers. Neither re-enters the adapter for a second
// pushDescriptors, so one scratch vector per thread is both correct and
// allocation-free after the first frame.
thread_local std::vector<d3d12_mc::DrawBinding> gBindingScratch;

}  // namespace

extern "C" {

int32_t MCDR_CALL mcdr_dx12SetPipeline(mcdr_command_ctx* ctx, mcdr_pipeline* pipeline, int32_t hasDepth) {
    if (const auto r = d3d12_mc::setPipeline(ctx, pipeline, hasDepth != 0); !r) {
        return mcdr::abi::failWith("mcdr_dx12SetPipeline", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetScissor(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                      int32_t height) {
    if (const auto r = d3d12_mc::setScissor(ctx, x, y, width, height); !r) {
        return mcdr::abi::failWith("mcdr_dx12SetScissor", r.error().message);
    }
    return 1;
}

// MC 26.2: RenderPassBackend gained setViewport(int,int,int,int).
int32_t MCDR_CALL mcdr_dx12SetViewport(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                       int32_t height) {
    if (const auto r = d3d12_mc::setViewport(ctx, x, y, width, height); !r) {
        return mcdr::abi::failWith("mcdr_dx12SetViewport", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetVertexBuffer(mcdr_command_ctx* ctx, int32_t slot, mcdr_object* buffer,
                                           int64_t offset, int32_t stride) {
    if (const auto r = d3d12_mc::setVertexBuffer(ctx, slot, buffer, offset, stride); !r) {
        return mcdr::abi::failWith("mcdr_dx12SetVertexBuffer", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12SetIndexBuffer(mcdr_command_ctx* ctx, mcdr_object* buffer, int32_t indexType) {
    if (const auto r = d3d12_mc::setIndexBuffer(ctx, buffer, indexType); !r) {
        return mcdr::abi::failWith("mcdr_dx12SetIndexBuffer", r.error().message);
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

    if (const auto r = d3d12_mc::pushDescriptors(ctx, gBindingScratch); !r) {
        // Issue #10 in the source project: the Java call site could only report
        // "dx12PushDescriptors failed" because the real reason (descriptor heap
        // exhausted / stale view / illegal CBV) never left native code. The
        // message now crosses the boundary and Java raises it.
        return mcdr::abi::failWith("mcdr_dx12PushDescriptors", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12DrawIndexed(mcdr_command_ctx* ctx, int32_t indexCount, int32_t instanceCount,
                                       int32_t firstIndex, int32_t baseVertex, int32_t firstInstance) {
    if (const auto r = d3d12_mc::drawIndexedInstanced(ctx, static_cast<UINT>(indexCount),
                                                      static_cast<UINT>(instanceCount),
                                                      static_cast<INT>(firstIndex), static_cast<INT>(baseVertex),
                                                      static_cast<UINT>(firstInstance));
        !r) {
        return mcdr::abi::failWith("mcdr_dx12DrawIndexed", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12Draw(mcdr_command_ctx* ctx, int32_t vertexCount, int32_t instanceCount,
                                int32_t firstVertex, int32_t firstInstance) {
    if (const auto r = d3d12_mc::drawInstanced(ctx, static_cast<UINT>(vertexCount),
                                               static_cast<UINT>(instanceCount), static_cast<UINT>(firstVertex),
                                               static_cast<UINT>(firstInstance));
        !r) {
        return mcdr::abi::failWith("mcdr_dx12Draw", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12DrawIndexedIndirect(mcdr_command_ctx* ctx, mcdr_object* commands, int64_t offset,
                                               int32_t drawCount) {
    if (const auto r = d3d12_mc::drawIndexedIndirect(ctx, commands, offset, static_cast<UINT>(drawCount)); !r) {
        return mcdr::abi::failWith("mcdr_dx12DrawIndexedIndirect", r.error().message);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12DrawIndirect(mcdr_command_ctx* ctx, mcdr_object* commands, int64_t offset,
                                        int32_t drawCount) {
    if (const auto r = d3d12_mc::drawIndirect(ctx, commands, offset, static_cast<UINT>(drawCount)); !r) {
        return mcdr::abi::failWith("mcdr_dx12DrawIndirect", r.error().message);
    }
    return 1;
}

}  // extern "C"
