// abi_async.cpp — async rendering infrastructure: partitioned descriptor allocator, bundle recorder pool, fence manager, main-list executor, barrier pre-passes
//
// Group: async infrastructure (originally abi_bridge_p33.cpp).
// 合并自：abi_bridge_p33.cpp
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
// mcdr.error 是叶子模块；这里要用 mcdr::describe() 组装 init 失败原因，
// 而 import 不会被传递（mcdr.abi.common 的 import 不会顺带带进来）。
import mcdr.error;

using namespace d3d12_mc;

// ===========================================================================
// 本文件是合并后的 C ABI 适配单元，按**功能**命名（不再按移植阶段 pX）。
// 共享前言（prelude / mcdr_abi.h / 各模块 import）统一放在文件顶部，下面每一段
// 保留原单元的文件头注释与代码本体，内容逐字节未改。
// ===========================================================================

// ---------------------------------------------------------------------------
// 来源单元：abi_bridge_p33.cpp
// ---------------------------------------------------------------------------

// abi_async.cpp — async rendering infrastructure: partitioned descriptor（合并自 abi_bridge_p33.cpp）
// allocator, bundle recorder pool, fence manager, main-list executor, and the
// barrier pre-passes and bundle execution that glue them onto the sync path.
//
// Replaces jni_bridge_p33.cpp (1031 lines, 42 exports).
//
// The one structural change in this file is the hot path. The JNI version took
// 14 parallel arrays and 20 parameters for a single worker partition, and every
// call had to GetIntArrayElements/GetLongArrayElements 14 times (the
// JIntArrayRO/JLongArrayRO RAII guards existed only to keep that from leaking on
// the error branches). It is now two struct arrays:
//
//   const mcdr_binding_slot*  bindings   -- packed against mcdr_binding_slot
//   const mcdr_partition_draw* draws     -- packed against mcdr_partition_draw
//
// which the Java side hands over as zero-copy slices of one native buffer. A
// parallel worker gets `bindings.asSlice(start * bc * STRIDE, count * bc * STRIDE)`
// and `draws.asSlice(start * DRAW_STRIDE, count * DRAW_STRIDE)` — no array
// marshalling at all, and the two variants (per-draw bindings vs one shared
// block) collapse into the `sharedBindings` flag.
//
// Indexing contract, which is the thing to keep straight:
//   * `draws[i]` is draw number `i` of THIS partition (already advanced past the
//     partition's first draw, so there is no startDraw parameter).
//   * when sharedBindings == 0, binding slot j of draw i is
//     `bindings[i * bindingCount + j]`;
//   * when sharedBindings != 0, every draw reads `bindings[j]` (draw 0's block).
//   * the SRV descriptor table is ALWAYS built from `bindings[j]`, in both
//     variants, because the Java side only reaches the bundle path once it has
//     proved every draw in the batch shares the same SRV set (`srvUniform`).

namespace {

// 统一走 native 日志前缀（与 JVM 侧 log4j 同形），与 dbgLog* 的行外观一致。
void logFail(const char* what) {
    char line[1024];
    const int n = d3d12_mc::formatLogPrefix(line, sizeof(line), "ERROR", "dx12");
    if (n > 0 && static_cast<size_t>(n) < sizeof(line)) {
        std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "%s failed", what);
        std::fprintf(stderr, "%s\n", line);
    } else {
        std::fprintf(stderr, "%s failed\n", what);
    }
    std::fflush(stderr);
}

// The bundle path binds CBVs as root descriptors exactly like the sync path, so it
// inherits the same D3D12 rule: the GPU address must be a multiple of 256. Those
// offsets come from the JVM, and an unaligned one yields shader-side garbage rather
// than a driver error, so report it here instead of recording an invalid list.
void logAlignmentFail(const char* what, long long offset) {
    char line[1024];
    const int n = d3d12_mc::formatLogPrefix(line, sizeof(line), "ERROR", "dx12");
    if (n > 0 && static_cast<size_t>(n) < sizeof(line)) {
        std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                      "%s: CBV is not 256-byte aligned (buffer offset=%lld)", what, offset);
        std::fprintf(stderr, "%s\n", line);
    } else {
        std::fprintf(stderr, "%s: CBV is not 256-byte aligned (offset=%lld)\n", what, offset);
    }
    std::fflush(stderr);
}

// Shared body of mcdr_dx12AsyncBundleRecordPartition for both indexing modes.
int32_t recordPartition(PartitionedDescriptorAllocator* a, BundleRecorder* r, int32_t baseSlot,
                        int32_t bindingCount, int32_t sharedBindings, const mcdr_binding_slot* bindings,
                        const mcdr_partition_draw* draws, int32_t drawCount, const char* what) {
    const int bcount = bindingCount;
    const int n = drawCount;

    // CBV -> root parameter index (dense, in binding order); SRV -> SRV table index.
    int cbvCount = 0;
    int srvCount = 0;
    for (int j = 0; j < bcount; ++j) {
        if (bindings[j].type == 0) {
            ++cbvCount;
        } else {
            ++srvCount;
        }
    }

    // Scheme B (split root signature): the SRV descriptor block and its root
    // descriptor table are written once for the whole partition, not once per
    // draw. Every draw in the partition is known to share the same SRV set.
    if (srvCount > 0) {
        int s = 0;
        for (int j = 0; j < bcount; ++j) {
            if (bindings[j].type == 0) {
                continue;
            }
            Dx12Object* v = bindings[j].view;
            if (!v || v->cpuHandle.ptr == 0) {
                logFail(what);
                return 0;
            }
            if (!a->writeDescriptor(static_cast<UINT>(baseSlot + s), v->cpuHandle)) {
                return 0;
            }
            ++s;
        }
        r->setDescriptorTable(static_cast<UINT>(cbvCount), a->gpuHandle(static_cast<UINT>(baseSlot)));
    }

    // P64 constant-CBV hoisting: of the CBVs in a batch only one typically varies
    // per draw; the rest are constant for the whole batch and are set once before
    // the loop (a root descriptor bakes its address straight into the command
    // list). Bit i of varyMask = the i-th CBV binding (dense numbering) varies.
    // Above 64 bindings the optimisation is skipped rather than made to fit.
    UINT64 varyMask = 0;
    if (sharedBindings) {
        // Every draw shares one block, so nothing can vary by construction --
        // the caller records one bundle per distinct binding set.
    } else if (cbvCount <= 64) {
        int ci = 0;
        for (int j = 0; j < bcount; ++j) {
            if (bindings[j].type != 0) {
                continue;
            }
            const mcdr_binding_slot& b0 = bindings[j];
            for (int i = 1; i < n; ++i) {
                const mcdr_binding_slot& bi = bindings[i * bcount + j];
                if (bi.buffer != b0.buffer || bi.offset != b0.offset) {
                    varyMask |= static_cast<UINT64>(1) << ci;
                    break;
                }
            }
            ++ci;
        }
    } else {
        varyMask = ~static_cast<UINT64>(0);
    }

    // Constant CBVs: set once before the loop.
    if (!sharedBindings) {
        int ci = 0;
        for (int j = 0; j < bcount; ++j) {
            if (bindings[j].type != 0) {
                continue;
            }
            if (varyMask & (static_cast<UINT64>(1) << ci)) {
                ++ci;
                continue;
            }
            const mcdr_binding_slot& b = bindings[j];
            Dx12Object* buf = b.buffer;
            if (!buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) {
                logFail(what);
                return 0;
            }
            if (b.offset < 0) {
                return 0;
            }
            const D3D12_GPU_VIRTUAL_ADDRESS cbvVa =
                buf->resource->GetGPUVirtualAddress() + static_cast<UINT64>(b.offset);
            if (cbvVa % 256ULL != 0) {
                logAlignmentFail(what, b.offset);
                return 0;
            }
            r->setRootConstantBufferView(static_cast<UINT>(ci), cbvVa);
            ++ci;
        }
    }

    // P63: the batch is uniformly one VB + one IB (drawMultipleIndexed's whole
    // semantics is "one VB and one IB, many (indexCount, baseVertex) groups"), so
    // probe for uniformity and hoist the two IASet* calls out of the loop.
    bool vbIbUniform = true;
    if (!sharedBindings) {
        for (int i = 1; i < n; ++i) {
            if (draws[i].indexBuffer != draws[0].indexBuffer || draws[i].indexType != draws[0].indexType
                || draws[i].vertexBuffer != draws[0].vertexBuffer
                || draws[i].vertexOffset != draws[0].vertexOffset
                || draws[i].vertexStride != draws[0].vertexStride
                || draws[i].vertexSlot != draws[0].vertexSlot) {
                vbIbUniform = false;
                break;
            }
        }
    }

    for (int i = 0; i < n; ++i) {
        if (!sharedBindings) {
            // Scheme B: CBVs go through root descriptors, so there are zero
            // descriptor-heap writes per draw. Skip the constant ones (P64).
            int cbvIdx = 0;
            for (int j = 0; j < bcount; ++j) {
                if (bindings[j].type != 0) {
                    continue;
                }
                if (!(varyMask & (static_cast<UINT64>(1) << cbvIdx))) {
                    ++cbvIdx;
                    continue;
                }
                const mcdr_binding_slot& b = bindings[i * bcount + j];
                Dx12Object* buf = b.buffer;
                if (!buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) {
                    logFail(what);
                    return 0;
                }
                if (b.offset < 0) {
                    return 0;
                }
                const D3D12_GPU_VIRTUAL_ADDRESS cbvVa =
                    buf->resource->GetGPUVirtualAddress() + static_cast<UINT64>(b.offset);
                if (cbvVa % 256ULL != 0) {
                    logAlignmentFail(what, b.offset);
                    return 0;
                }
                r->setRootConstantBufferView(static_cast<UINT>(cbvIdx), cbvVa);
                ++cbvIdx;
            }
        }

        if (sharedBindings || !vbIbUniform || i == 0) {
            Dx12Object* idx = draws[i].indexBuffer;
            if (!idx || idx->kind != Dx12Object::Kind::Buffer || !idx->resource) {
                logFail(what);
                return 0;
            }
            D3D12_INDEX_BUFFER_VIEW ib{};
            ib.BufferLocation = idx->resource->GetGPUVirtualAddress();
            ib.SizeInBytes = static_cast<UINT>(idx->size);
            ib.Format = draws[i].indexType == 1 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
            r->setIndexBuffer(&ib);

            Dx12Object* vb = draws[i].vertexBuffer;
            const int64_t voff = draws[i].vertexOffset;
            if (!vb || vb->kind != Dx12Object::Kind::Buffer || !vb->resource || voff < 0
                || static_cast<UINT64>(voff) >= static_cast<UINT64>(vb->size)) {
                logFail(what);
                return 0;
            }
            D3D12_VERTEX_BUFFER_VIEW vbv{};
            vbv.BufferLocation = vb->resource->GetGPUVirtualAddress() + static_cast<UINT64>(voff);
            vbv.SizeInBytes = static_cast<UINT>(vb->size - static_cast<UINT64>(voff));
            // Same rule as the sync path and dx12AsyncBundleSetVertexBuffer: the
            // pipeline's corrected stride wins over the Java-supplied value,
            // because Java's getVertexSize() is wrong for some pipelines.
            UINT effStride = static_cast<UINT>(draws[i].vertexStride);
            const UINT corrected = r->correctedStride(draws[i].vertexSlot);
            if (corrected > 0) {
                effStride = corrected;
            }
            vbv.StrideInBytes = effStride;
            r->setVertexBuffers(static_cast<UINT>(draws[i].vertexSlot), 1, &vbv);
        }

        r->drawIndexedInstanced(static_cast<UINT>(draws[i].indexCount), 1,
                                static_cast<UINT>(draws[i].firstIndex), static_cast<INT>(draws[i].baseVertex),
                                0);
    }
    return 1;
}

// Shared body of the three barrier pre-passes. `apply` is invoked once per valid
// object; a null or wrong-kind entry is skipped, exactly as the JNI version did.
template <typename Fn>
int32_t prepareObjects(mcdr_command_ctx* ctx, mcdr_object* const* objects, int32_t count, const char* what,
                       Fn&& apply) {
    if (!ctx || !ctx->listOpen || !objects) {
        return mcdr::abi::failWith(what, "invalid args");
    }
    if (count <= 0) {
        return 1;
    }
    for (int32_t i = 0; i < count; ++i) {
        apply(objects[i]);
    }
    return 1;
}

}  // namespace

// BundleRecorder::correctedStride 读取本 bundle 记下的 per-slot 修正 stride 映射。
// 映射由 setCorrectedStride 在 mcdr_dx12AsyncBundleSetPipelineState 里从 Dx12Pipeline
// 抄入；本模块（异步 bundle）不 import 设备模块，故不持有 Dx12Pipeline*。
UINT BundleRecorder::correctedStride(int slot) const {
    auto it = m_correctedStrides.find(slot);
    return it == m_correctedStrides.end() ? 0u : it->second;
}

extern "C" {

// ===========================================================================
// Partitioned descriptor allocator
// ===========================================================================

mcdr_desc_alloc* MCDR_CALL mcdr_dx12AsyncDescriptorCreate(int32_t workerCount) {
    DeviceContext& dc = deviceContext();
    if (!dc.device || !dc.drawHeap) {
        mcdr::abi::setLastError("mcdr_dx12AsyncDescriptorCreate", "device/drawHeap not ready");
        logFail("dx12AsyncDescriptorCreate: device/drawHeap not ready");
        return nullptr;
    }
    const D3D12_DESCRIPTOR_HEAP_DESC desc = dc.drawHeap->GetDesc();
    auto* alloc = new PartitionedDescriptorAllocator();
    // 全限定 mcdr::abi:: 见 abi_mesh.cpp 同处的说明：ADL 不会把 mcdr::abi 拉进来。
    if (const auto r = alloc->init(dc.device.Get(), dc.drawHeap.Get(), desc.NumDescriptors,
                                   static_cast<UINT>(workerCount), kAsyncSyncRingReserve);
        !r) {
        // describe(e) == "<where>: <message>"，所以前缀仍是原来的
        // "PartitionedDescriptorAllocator::init failed"，后面多了真实失败原因。
        mcdr::abi::setLastError("mcdr_dx12AsyncDescriptorCreate",
                                "PartitionedDescriptorAllocator::init failed: " +
                                    mcdr::describe(r.error()));
        logFail("dx12AsyncDescriptorCreate: init");
        delete alloc;
        return nullptr;
    }
    // P33: tell the synchronous path that only kAsyncSyncRingReserve slots at the
    // start of each frame section belong to the sync ring, so that
    // pushDescriptors/blitBindSourceTexture's capacity check lines up with the
    // worker partitions and the sync ring cannot silently overwrite worker
    // descriptors. Must equal the reservedPerSection used by alloc->init above.
    dc.syncRingReserve = kAsyncSyncRingReserve;
    return alloc;
}

void MCDR_CALL mcdr_dx12AsyncDescriptorDestroy(mcdr_desc_alloc* alloc) {
    if (alloc != nullptr) {
        // P33: async partitioning goes offline, so the sync path may use the whole
        // drawHeap section again.
        deviceContext().syncRingReserve = 0;
    }
    delete alloc;
}

int32_t MCDR_CALL mcdr_dx12AsyncDescriptorRegionBase(mcdr_desc_alloc* alloc, int32_t frameSlot,
                                                     int32_t worker) {
    if (!alloc) {
        return -1;
    }
    const UINT base = alloc->regionBaseSlot(static_cast<UINT>(frameSlot), static_cast<UINT>(worker));
    return base == UINT_MAX ? -1 : static_cast<int32_t>(base);
}

int32_t MCDR_CALL mcdr_dx12AsyncDescriptorAllocate(mcdr_desc_alloc* alloc, int32_t frameSlot, int32_t worker,
                                                   int32_t count) {
    if (!alloc || count <= 0) {
        return -1;
    }
    const UINT slot =
        alloc->allocate(static_cast<UINT>(frameSlot), static_cast<UINT>(worker), static_cast<UINT>(count));
    return slot == UINT_MAX ? -1 : static_cast<int32_t>(slot);
}

void MCDR_CALL mcdr_dx12AsyncDescriptorResetFrame(mcdr_desc_alloc* alloc, int32_t frameSlot) {
    if (alloc) {
        alloc->resetFrame(static_cast<UINT>(frameSlot));
    }
}

uint64_t MCDR_CALL mcdr_dx12AsyncDescriptorGpuHandle(mcdr_desc_alloc* alloc, int32_t slot) {
    if (!alloc || slot < 0) {
        return 0;
    }
    return static_cast<uint64_t>(alloc->gpuHandle(static_cast<UINT>(slot)).ptr);
}

int32_t MCDR_CALL mcdr_dx12AsyncDescriptorWriteCBV(mcdr_desc_alloc* alloc, int32_t slot, mcdr_object* buffer,
                                                   int64_t offset, int64_t size) {
    Dx12Object* buf = buffer;
    if (!alloc || !buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) {
        return mcdr::abi::failWith("mcdr_dx12AsyncDescriptorWriteCBV", "invalid args");
    }
    if (slot < 0 || offset < 0 || size < 0) {
        return 0;
    }
    // Descriptor-heap CBV, same 256-byte address rule as the root-descriptor paths.
    if ((buf->resource->GetGPUVirtualAddress() + static_cast<UINT64>(offset)) % 256ULL != 0) {
        return mcdr::abi::failWith("mcdr_dx12AsyncDescriptorWriteCBV",
                                   "CBV is not 256-byte aligned (buffer offset=" + std::to_string(offset) + ")");
    }
    return alloc->writeCBV(static_cast<UINT>(slot), buf->resource.Get(), static_cast<UINT64>(offset),
                           static_cast<UINT64>(size))
               ? 1
               : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncDescriptorWriteSRV(mcdr_desc_alloc* alloc, int32_t slot, mcdr_object* view) {
    Dx12Object* v = view;
    if (!alloc || !v || v->cpuHandle.ptr == 0 || slot < 0) {
        return mcdr::abi::failWith("mcdr_dx12AsyncDescriptorWriteSRV", "invalid args");
    }
    return alloc->writeDescriptor(static_cast<UINT>(slot), v->cpuHandle) ? 1 : 0;
}

// ===========================================================================
// Bundle recorder pool
// ===========================================================================

mcdr_bundle_pool* MCDR_CALL mcdr_dx12AsyncBundlePoolCreate(int32_t workerCount) {
    DeviceContext& dc = deviceContext();
    if (!dc.device) {
        mcdr::abi::setLastError("mcdr_dx12AsyncBundlePoolCreate", "device not ready");
        logFail("dx12AsyncBundlePoolCreate: device not ready");
        return nullptr;
    }
    auto* pool = new BundleRecorderPool();
    // A bundle records SetGraphicsRootDescriptorTable, so it has to bind a
    // shader-visible descriptor heap itself: pass the same (non-owned) drawHeap
    // the parent command list uses.
    if (const auto r = pool->init(dc.device.Get(), dc.drawHeap.Get(), static_cast<UINT>(workerCount),
                                  dc.cmdSigIndexed.Get(), dc.cmdSigNonIndexed.Get());
        !r) {
        mcdr::abi::setLastError("mcdr_dx12AsyncBundlePoolCreate",
                                "BundleRecorderPool::init failed: " + mcdr::describe(r.error()));
        logFail("dx12AsyncBundlePoolCreate: init");
        delete pool;
        return nullptr;
    }
    return pool;
}

void MCDR_CALL mcdr_dx12AsyncBundlePoolDestroy(mcdr_bundle_pool* pool) { delete pool; }

int32_t MCDR_CALL mcdr_dx12AsyncBundleBegin(mcdr_bundle_pool* pool, int32_t worker, int32_t frameSlot,
                                            uint64_t frameValue) {
    BundleRecorder* r = pool ? pool->recorder(static_cast<UINT>(worker)) : nullptr;
    if (!r || frameSlot < 0 || !r->begin(static_cast<UINT>(frameSlot), frameValue)) {
        return mcdr::abi::failWith("mcdr_dx12AsyncBundleBegin", "begin failed");
    }
    return 1;
}

uint64_t MCDR_CALL mcdr_dx12AsyncBundleEnd(mcdr_bundle_pool* pool, int32_t worker) {
    BundleRecorder* r = pool ? pool->recorder(static_cast<UINT>(worker)) : nullptr;
    if (!r) {
        return 0;
    }
    return reinterpret_cast<uint64_t>(r->end());
}

int32_t MCDR_CALL mcdr_dx12AsyncBundleSetPipelineState(mcdr_bundle_pool* pool, int32_t worker,
                                                       mcdr_pipeline* pipeline, int32_t hasDepth) {
    BundleRecorder* r = pool ? pool->recorder(static_cast<UINT>(worker)) : nullptr;
    Dx12Pipeline* pl = pipeline;
    if (!r || !pl) {
        return mcdr::abi::failWith("mcdr_dx12AsyncBundleSetPipelineState", "invalid args");
    }
    ID3D12PipelineState* pso = hasDepth ? pl->withDepth.Get() : pl->withoutDepth.Get();
    if (!pso) {
        pso = pl->withDepth.Get();
    }
    if (!pso || !pl->rootSignature) {
        return 0;
    }
    r->setRootSignature(pl->rootSignature.Get());
    r->setPipelineState(pso);
    r->setPrimitiveTopology(bundlePrimitiveTopology(pl->topology));
    // P33: remember the pipeline's corrected per-slot strides so setVertexBuffer can
    // apply the same override the sync path uses.
    for (const auto& kv : pl->vertexStrides) {
        r->setCorrectedStride(kv.first, kv.second);
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12AsyncBundleSetDescriptorTable(mcdr_bundle_pool* pool, int32_t worker, int32_t slot,
                                                         uint64_t gpuHandle) {
    BundleRecorder* r = pool ? pool->recorder(static_cast<UINT>(worker)) : nullptr;
    if (!r) {
        return mcdr::abi::failWith("mcdr_dx12AsyncBundleSetDescriptorTable", "invalid recorder");
    }
    D3D12_GPU_DESCRIPTOR_HANDLE h{};
    h.ptr = gpuHandle;
    r->setDescriptorTable(static_cast<UINT>(slot), h);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12AsyncBundleSetVertexBuffer(mcdr_bundle_pool* pool, int32_t worker, int32_t slot,
                                                      mcdr_object* buffer, int64_t offset, int32_t stride) {
    BundleRecorder* r = pool ? pool->recorder(static_cast<UINT>(worker)) : nullptr;
    Dx12Object* buf = buffer;
    if (!r || !buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource || slot < 0 || offset < 0
        || offset >= buf->size) {
        return mcdr::abi::failWith("mcdr_dx12AsyncBundleSetVertexBuffer", "invalid args");
    }
    D3D12_VERTEX_BUFFER_VIEW vb{};
    vb.BufferLocation = buf->resource->GetGPUVirtualAddress() + static_cast<UINT64>(offset);
    vb.SizeInBytes = static_cast<UINT>(buf->size - offset);
    // P33: match the sync setVertexBuffer — prefer the pipeline's corrected stride,
    // because Java's getVertexSize() does not equal the real vertex pitch on every
    // pipeline.
    UINT effectiveStride = static_cast<UINT>(stride);
    const UINT corrected = r->correctedStride(slot);
    if (corrected > 0) {
        effectiveStride = corrected;
    }
    vb.StrideInBytes = effectiveStride;
    r->setVertexBuffers(static_cast<UINT>(slot), 1, &vb);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12AsyncBundleSetIndexBuffer(mcdr_bundle_pool* pool, int32_t worker,
                                                     mcdr_object* buffer, int32_t indexType) {
    BundleRecorder* r = pool ? pool->recorder(static_cast<UINT>(worker)) : nullptr;
    Dx12Object* buf = buffer;
    if (!r || !buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) {
        return mcdr::abi::failWith("mcdr_dx12AsyncBundleSetIndexBuffer", "invalid args");
    }
    D3D12_INDEX_BUFFER_VIEW ib{};
    ib.BufferLocation = buf->resource->GetGPUVirtualAddress();
    ib.SizeInBytes = static_cast<UINT>(buf->size);
    ib.Format = indexType == 1 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    r->setIndexBuffer(&ib);
    return 1;
}

int32_t MCDR_CALL mcdr_dx12AsyncBundleDrawIndexed(mcdr_bundle_pool* pool, int32_t worker, int32_t indexCount,
                                                  int32_t instanceCount, int32_t firstIndex,
                                                  int32_t baseVertex, int32_t firstInstance) {
    BundleRecorder* r = pool ? pool->recorder(static_cast<UINT>(worker)) : nullptr;
    if (!r) {
        return 0;
    }
    r->drawIndexedInstanced(static_cast<UINT>(indexCount), static_cast<UINT>(instanceCount),
                            static_cast<UINT>(firstIndex), static_cast<INT>(baseVertex),
                            static_cast<UINT>(firstInstance));
    return 1;
}

int32_t MCDR_CALL mcdr_dx12AsyncBundleDraw(mcdr_bundle_pool* pool, int32_t worker, int32_t vertexCount,
                                           int32_t instanceCount, int32_t firstVertex,
                                           int32_t firstInstance) {
    BundleRecorder* r = pool ? pool->recorder(static_cast<UINT>(worker)) : nullptr;
    if (!r) {
        return 0;
    }
    r->drawInstanced(static_cast<UINT>(vertexCount), static_cast<UINT>(instanceCount),
                     static_cast<UINT>(firstVertex), static_cast<UINT>(firstInstance));
    return 1;
}

int32_t MCDR_CALL mcdr_dx12AsyncBundleDrawIndexedIndirect(mcdr_bundle_pool* pool, int32_t worker,
                                                          mcdr_object* commands, int64_t offset,
                                                          int32_t drawCount) {
    BundleRecorder* r = pool ? pool->recorder(static_cast<UINT>(worker)) : nullptr;
    Dx12Object* cmds = commands;
    if (!r || !cmds || cmds->kind != Dx12Object::Kind::Buffer || !cmds->resource) {
        return mcdr::abi::failWith("mcdr_dx12AsyncBundleDrawIndexedIndirect", "invalid args");
    }
    if (!r->drawIndexedIndirect(cmds->resource.Get(), static_cast<UINT64>(offset),
                                static_cast<UINT>(drawCount))) {
        return mcdr::abi::failWith("mcdr_dx12AsyncBundleDrawIndexedIndirect", "no command signature");
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12AsyncBundleRecordPartition(mcdr_desc_alloc* alloc, mcdr_bundle_pool* pool,
                                                      int32_t worker, int32_t baseSlot, int32_t bindingCount,
                                                      int32_t sharedBindings,
                                                      const mcdr_binding_slot* bindings,
                                                      const mcdr_partition_draw* draws, int32_t drawCount) {
    BundleRecorder* r = pool ? pool->recorder(static_cast<UINT>(worker)) : nullptr;
    if (!alloc || !r || bindingCount <= 0 || drawCount <= 0 || baseSlot < 0 || !bindings || !draws) {
        return mcdr::abi::failWith("mcdr_dx12AsyncBundleRecordPartition", "invalid args");
    }
    const int32_t ok = recordPartition(alloc, r, baseSlot, bindingCount, sharedBindings, bindings, draws,
                                       drawCount, "dx12AsyncBundleRecordPartition");
    if (!ok) {
        mcdr::abi::setLastError("mcdr_dx12AsyncBundleRecordPartition", "invalid buffer/view in partition");
    }
    return ok;
}

// ===========================================================================
// Fence manager
//
// The only place the native layer calls back into the JVM. Under JNI this needed
// a JavaVM*, AttachCurrentThread/DetachCurrentThread around every invocation, a
// NewGlobalRef, and a string method lookup for the "(JZ)V" signature. Under FFM
// the Java side installs one upcall stub for the whole library and identifies
// the registration with an integer handle, so this is a plain function pointer
// call with no JVM bookkeeping at all.
// ===========================================================================

mcdr_fence_mgr* MCDR_CALL mcdr_dx12AsyncFenceCreate(void) {
    DeviceContext& dc = deviceContext();
    if (!dc.device || !dc.queue) {
        mcdr::abi::setLastError("mcdr_dx12AsyncFenceCreate", "device/queue not ready");
        logFail("dx12AsyncFenceCreate: device/queue not ready");
        return nullptr;
    }
    auto* mgr = new AsyncFenceManager();
    if (const auto r = mgr->init(dc.device.Get(), dc.queue.Get()); !r) {
        mcdr::abi::setLastError("mcdr_dx12AsyncFenceCreate",
                                "AsyncFenceManager::init failed: " + mcdr::describe(r.error()));
        logFail("dx12AsyncFenceCreate: init");
        delete mgr;
        return nullptr;
    }
    return mgr;
}

void MCDR_CALL mcdr_dx12AsyncFenceDestroy(mcdr_fence_mgr* manager) { delete manager; }

int32_t MCDR_CALL mcdr_dx12AsyncFenceSignal(mcdr_fence_mgr* manager, uint64_t value) {
    return (manager && manager->signal(value)) ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncFenceWait(mcdr_fence_mgr* manager, uint64_t value, int64_t timeoutMs) {
    if (!manager) {
        return 0;
    }
    // timeoutMs == 0 means wait forever; a negative value means the same.
    return manager->waitFence(value, static_cast<UINT64>(timeoutMs < 0 ? 0 : timeoutMs)) ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncFenceIsComplete(mcdr_fence_mgr* manager, uint64_t value) {
    return (manager && manager->isFenceComplete(value)) ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncFenceRegisterCallback(mcdr_fence_mgr* manager, uint64_t value,
                                                      mcdr_fence_callback_fn cb, int64_t userHandle,
                                                      int64_t timeoutMs) {
    if (!manager || !cb) {
        return mcdr::abi::failWith("mcdr_dx12AsyncFenceRegisterCallback", "invalid args");
    }
    // The callback fires on the monitor thread, outside the registry lock, so it
    // may legally re-register or unregister. The stub must not throw -- the Java
    // side wraps it with MethodHandles.catchException, because an escaping
    // exception out of an upcall terminates the JVM abruptly.
    manager->registerCallback(
        value, [cb, userHandle](UINT64 v, bool ok) { cb(userHandle, v, ok ? 1 : 0); },
        static_cast<UINT64>(timeoutMs < 0 ? 0 : timeoutMs));
    return 1;
}

// ===========================================================================
// Executing bundles on an existing (synchronous) command list
//
// The main list is still recorded by the DeviceContext's CommandContext (render
// pass boundaries, viewport/scissor, clears, copies and barriers all live on the
// main list); only the draw-dense regions are recorded into bundles by workers,
// then replayed onto the main list in order here.
// ===========================================================================

int32_t MCDR_CALL mcdr_dx12AsyncCurrentFrameSlot(mcdr_command_ctx* ctx) {
    return ctx ? static_cast<int32_t>(currentDrawFrameSlot(ctx)) : -1;
}

uint64_t MCDR_CALL mcdr_dx12AsyncCurrentFrameValue(mcdr_command_ctx* ctx) {
    // The raw frame number (ctx->fenceValue). The bundle allocator picks its slot
    // from (frameSlot, batch), but needs the raw number to tell "a new frame
    // started" apart from "the same slot came round again" -- frameSlot alone
    // (%4) misreads when idle frames skip whole cycles.
    // Returns (uint64_t)-1 for a null ctx, which the Java side reads as -1.
    return ctx ? static_cast<uint64_t>(ctx->fenceValue) : ~static_cast<uint64_t>(0);
}

int32_t MCDR_CALL mcdr_dx12ExecuteBundle(mcdr_command_ctx* ctx, uint64_t bundle) {
    auto* b = reinterpret_cast<ID3D12GraphicsCommandList*>(static_cast<uintptr_t>(bundle));
    if (!ctx || !ctx->commandList || !ctx->listOpen || !b) {
        return mcdr::abi::failWith("mcdr_dx12ExecuteBundle", "invalid args");
    }
    ctx->commandList->ExecuteBundle(b);
    // P64: a PSO/root signature set inside a bundle does not roll back to the
    // parent list, so the parent's binding state is no longer knowable --
    // invalidate the setPipeline fast-path cache to force a full re-issue.
    ctx->cachedPso = nullptr;
    return 1;
}

// The main thread transitions this batch's CBV buffers to
// VERTEX_AND_CONSTANT_BUFFER before dispatching bundles (a bundle may not contain
// ResourceBarrier). Same semantics as pushDescriptors' type == 0 handling;
// transitionBufferTo is a no-op when the state already matches.
int32_t MCDR_CALL mcdr_dx12AsyncPrepareCBVBuffers(mcdr_command_ctx* ctx, mcdr_object* const* buffers,
                                                  int32_t count) {
    return prepareObjects(ctx, buffers, count, "mcdr_dx12AsyncPrepareCBVBuffers", [ctx](mcdr_object* o) {
        Dx12Object* buf = o;
        if (!buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) {
            return;
        }
        transitionBufferTo(ctx, buf, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    });
}

// Same origin: transition this batch's index buffers to INDEX_BUFFER. Vertex
// buffers are handled by PrepareCBVBuffers because their target state is the same
// as a CBV's.
int32_t MCDR_CALL mcdr_dx12AsyncPrepareIndexBuffers(mcdr_command_ctx* ctx, mcdr_object* const* buffers,
                                                    int32_t count) {
    return prepareObjects(ctx, buffers, count, "mcdr_dx12AsyncPrepareIndexBuffers", [ctx](mcdr_object* o) {
        Dx12Object* buf = o;
        if (!buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) {
            return;
        }
        transitionBufferTo(ctx, buf, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    });
}

// Same origin: transition this batch's sampled texture views to
// PIXEL/NON_PIXEL_SHADER_RESOURCE. Same semantics as pushDescriptors' type == 1
// handling; a view with no sourceTexture is skipped.
int32_t MCDR_CALL mcdr_dx12AsyncPrepareTextureViews(mcdr_command_ctx* ctx, mcdr_object* const* views,
                                                    int32_t count) {
    return prepareObjects(ctx, views, count, "mcdr_dx12AsyncPrepareTextureViews", [ctx](mcdr_object* o) {
        Dx12Object* v = o;
        if (!v || !v->sourceTexture) {
            return;
        }
        transitionTextureTo(ctx, v->sourceTexture,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    });
}

// ===========================================================================
// Main command list executor
// ===========================================================================

mcdr_executor* MCDR_CALL mcdr_dx12AsyncExecutorCreate(void) {
    DeviceContext& dc = deviceContext();
    if (!dc.device || !dc.queue) {
        mcdr::abi::setLastError("mcdr_dx12AsyncExecutorCreate", "device/queue not ready");
        logFail("dx12AsyncExecutorCreate: device/queue not ready");
        return nullptr;
    }
    auto* exec = new MainCommandExecutor();
    if (const auto r = exec->init(dc.device.Get(), dc.queue.Get(), dc.drawHeap.Get()); !r) {
        mcdr::abi::setLastError("mcdr_dx12AsyncExecutorCreate",
                                "MainCommandExecutor::init failed: " + mcdr::describe(r.error()));
        logFail("dx12AsyncExecutorCreate: init");
        delete exec;
        return nullptr;
    }
    return exec;
}

void MCDR_CALL mcdr_dx12AsyncExecutorDestroy(mcdr_executor* executor) { delete executor; }

int32_t MCDR_CALL mcdr_dx12AsyncExecutorTryBeginFrame(mcdr_executor* executor) {
    return (executor && executor->tryBeginFrame()) ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncExecutorAddBundle(mcdr_executor* executor, uint64_t bundle) {
    if (!executor) {
        return 0;
    }
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(static_cast<uintptr_t>(bundle));
    return executor->addBundle(list) ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncExecutorAddTransition(mcdr_executor* executor, mcdr_object* object,
                                                      int32_t stateBefore, int32_t stateAfter) {
    Dx12Object* obj = object;
    if (!executor || !obj || !obj->resource) {
        return mcdr::abi::failWith("mcdr_dx12AsyncExecutorAddTransition", "invalid args");
    }
    return executor->addTransition(obj->resource.Get(), static_cast<D3D12_RESOURCE_STATES>(stateBefore),
                                   static_cast<D3D12_RESOURCE_STATES>(stateAfter))
               ? 1
               : 0;
}

uint64_t MCDR_CALL mcdr_dx12AsyncExecutorEndFrame(mcdr_executor* executor) {
    return executor ? static_cast<uint64_t>(executor->endFrame()) : 0;
}

uint64_t MCDR_CALL mcdr_dx12AsyncExecutorLastFenceValue(mcdr_executor* executor) {
    return executor ? static_cast<uint64_t>(executor->lastFenceValue()) : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncExecutorIsFrameInProgress(mcdr_executor* executor) {
    return (executor && executor->isFrameInProgress()) ? 1 : 0;
}

int32_t MCDR_CALL mcdr_dx12AsyncExecutorWaitFrame(mcdr_executor* executor, uint64_t fenceValue,
                                                  int64_t timeoutMs) {
    if (!executor) {
        return 0;
    }
    return executor->waitForFrame(fenceValue, static_cast<UINT64>(timeoutMs < 0 ? 0 : timeoutMs)) ? 1 : 0;
}

}  // extern "C"
