// dx12-mc 异步渲染基础设施（P33）：Bundle 录制器池
//
// 每个 worker 独占一个 D3D12_COMMAND_LIST_TYPE_BUNDLE 的 allocator + command
// list，负责把一段“同一 pass + 同一 PSO + 同一 scissor”的绘制录制进 bundle。
//
// D3D12 bundle 限制（本类严格不调用以下被禁止的 API）：
//   - 任何 Clear（ClearRenderTargetView / ClearDepthStencilView / ClearUnorderedAccessView*）
//   - 任何 Copy（CopyBufferRegion / CopyTextureRegion / CopyResource / CopyDescriptors*）
//   - ResourceBarrier / ResolveSubresource / BeginQuery / EndQuery
//   - OMSetRenderTargets / RSSetViewports / RSSetScissorRects
//   - ExecuteBundle / render pass begin/end / 二次 SetDescriptorHeaps
//     （bundle 不继承父列表的描述符堆；本类在 begin() 内统一 SetDescriptorHeaps 一次，
//      因为会录制 SetGraphicsRootDescriptorTable；父列表负责在别处的堆设置）
// bundle 内允许：SetPipelineState、SetGraphicsRootSignature、
// SetGraphicsRootDescriptorTable、IASetVertexBuffers/IASetIndexBuffer/
// IASetPrimitiveTopology、DrawInstanced/DrawIndexedInstanced、ExecuteIndirect。
//
// 注意：bundle 内的 PSO 必须与父列表在录制时的 RT 格式 / 深度格式 / 采样数一致，
// 因此调用方需按“同一 pass + 同一 PSO + 同一 scissor”切段后再 begin/end。

module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.async_bundle;

import mcdr.error;  // mcdr::Result<T> / mcdr::Error / mcdr::err()：失败上报改走 std::expected

namespace {

// HRESULT -> "0x%08X"（模块单元不得 import 设备模块，实现单元各自持有一份）。
std::string hrText(HRESULT hr) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return std::string(buf);
}

}  // namespace

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;

// 每个 worker 持有的 (allocator + bundle list) 帧槽数。命令 allocator 只能在其
// 对应的 GPU 工作完成后才能 Reset，因此按帧槽（fenceValue % kBundleFrameSlots）
// 轮转，保证复用某槽时其 N-kBundleFrameSlots 帧前的提交已完成。
constexpr UINT kBundleFrameSlots = 4;

// 单个帧槽内允许的并行 bundle 批次数。同一帧里会对同一命令列表多次调用
// drawMultipleIndexed（地形按 layer / draw group 分批），而已经 ExecuteBundle 进
// 父命令列表的 bundle，在其父列表提交并执行完成前不能 Reset 其 allocator。
// 因此同一帧内的第 b 批使用 (frameSlot, b) 对应的独立槽位；b 超过本上限时
// begin() 返回 false，调用方回退串行录制（功能正确，仅少了这段并行）。
constexpr UINT kBundleBatchesPerFrame = 16;
// (allocator, list) 槽位总数。
constexpr UINT kBundleSlotCount = kBundleFrameSlots * kBundleBatchesPerFrame;

// MC PrimitiveTopology ordinal -> 命令列表级拓扑（bundle 内使用）。
D3D12_PRIMITIVE_TOPOLOGY bundlePrimitiveTopology(int ordinal);

// 前向声明：录制时记录所属管线，用于与同步路径同样规则校正顶点步长
// （Dx12Pipeline::vertexStrides 由 inputElements 实际格式推算，覆盖 Java 侧
// getVertexSize() 的错误值）。
struct Dx12Pipeline;

class BundleRecorder {
public:
    BundleRecorder() = default;
    ~BundleRecorder() = default;

    BundleRecorder(const BundleRecorder&) = delete;
    BundleRecorder& operator=(const BundleRecorder&) = delete;

    // 创建 kBundleSlotCount 组 BUNDLE allocator + command list（初始为 closed）。
    // 现有 ExecuteIndirect 用的 command signature 从 DeviceContext 读取。
    // heap 为 DeviceContext::drawHeap（shader-visible CBV_SRV_UAV，非拥有）；
    // bundle 内若要编辑描述符表（SetGraphicsRootDescriptorTable），必须自行
    // SetDescriptorHeaps 一次，否则运行时解析 GPU 句柄时空堆指针解引用崩溃。
    // 任一步失败返回 mcdr::Error（含 HRESULT）。
    mcdr::Result<void> init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT workerId,
                            ID3D12CommandSignature* cmdSigIndexed, ID3D12CommandSignature* cmdSigNonIndexed);

    bool valid() const { return m_bundle != nullptr; }
    UINT workerId() const { return m_workerId; }
    bool isRecording() const { return m_recording; }
    ID3D12GraphicsCommandList* commandList() const { return m_bundle.Get(); }

    // 开始 / 结束录制。frameSlot 须为 fenceValue % kBundleFrameSlots；frameValue 为
    // 原始帧号（fenceValue），用于判定「同一帧内的第几批」，从而为每一批选一个独立的
    // allocator 槽位（已 ExecuteBundle 的 bundle 在父列表执行完成前不能被 Reset）。
    // 同一帧内批次数超过 kBundleBatchesPerFrame 时返回 false（调用方回退串行）。
    // end() 成功返回 bundle 命令列表句柄，失败返回 nullptr。
    bool begin(UINT frameSlot, UINT64 frameValue);
    ID3D12GraphicsCommandList* end();

    // ---- 录制命令（仅 bundle 允许的子集）----
    void setRootSignature(ID3D12RootSignature* rootSignature);
    void setPipelineState(ID3D12PipelineState* pso);
    // 记录所属管线的 per-slot 修正 stride（与同步路径 setVertexBuffer 一致）。
    // 说明：这里只保存 (slot → stride) 映射，而不保存 Dx12Pipeline*。异步 bundle 模块
    // 不能 import 设备模块（设备模块的实现单元反向 import 本模块，会成环），所以
    // Dx12Pipeline 在本模块里只有前向声明；保存指针会让 correctedStride 需要完整类型。
    void setCorrectedStride(int slot, UINT stride);
    // 该 slot 的修正 stride；无记录时返回 0（调用方回退传入值）。
    UINT correctedStride(int slot) const;
    void setDescriptorTable(UINT slot, D3D12_GPU_DESCRIPTOR_HANDLE handle);
    // B：CBV 的 root descriptor（地址烘焙进命令列表，不经过描述符堆）。
    void setRootConstantBufferView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS address);
    void setVertexBuffers(UINT startSlot, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views);
    void setIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view);
    void setPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology);
    void drawIndexedInstanced(UINT indexCount, UINT instanceCount, UINT startIndexLocation,
                              INT baseVertexLocation, UINT startInstanceLocation);
    void drawInstanced(UINT vertexCount, UINT instanceCount, UINT startVertexLocation,
                       UINT startInstanceLocation);
    // ExecuteIndirect：drawIndexedIndirect 使用现有 cmdSigIndexed，
    // 若签名为空直接返回 false（绝不向 ExecuteIndirect 传 nullptr）。
    bool drawIndexedIndirect(ID3D12Resource* commands, UINT64 offset, UINT drawCount);
    bool drawIndirect(ID3D12Resource* commands, UINT64 offset, UINT drawCount);

    struct Stats {
        UINT64 commandsRecorded = 0;
        UINT64 drawCalls = 0;
        UINT64 indexCount = 0;
        UINT64 vertexCount = 0;
        UINT64 descriptorUpdates = 0;
    };
    Stats stats() const { return m_stats; }
    void resetStats() { m_stats = Stats{}; }

private:
    // 按 (帧槽, 帧内批次序号) 轮转，避免 Reset 正在被 GPU 读取的 allocator
    // （详见 kBundleFrameSlots / kBundleBatchesPerFrame）。
    ComPtr<ID3D12CommandAllocator> m_allocators[kBundleSlotCount];
    ComPtr<ID3D12GraphicsCommandList> m_lists[kBundleSlotCount];
    // 当前录制使用的槽位与其命令列表（begin 时指向 m_lists[m_activeSlot]）。
    UINT m_activeSlot = 0;
    // 每个帧槽最近一次录制所属的帧号，以及该帧内已用批次数；帧号变化时批次归零。
    UINT64 m_frameOfSlot[kBundleFrameSlots] = {};
    UINT m_batchOfSlot[kBundleFrameSlots] = {};
    ComPtr<ID3D12GraphicsCommandList> m_bundle;
    ComPtr<ID3D12CommandSignature> m_cmdSigIndexed;     // = DeviceContext::cmdSigIndexed
    ComPtr<ID3D12CommandSignature> m_cmdSigNonIndexed;  // = DeviceContext::cmdSigNonIndexed
    // 本 bundle 内 SetGraphicsRootDescriptorTable 引用的 shader-visible 堆。
    // 必须与父命令列表执行 bundle 时绑定的堆一致，否则行为未定义。
    ComPtr<ID3D12DescriptorHeap> m_heap;

    ID3D12Device* m_device = nullptr;
    UINT m_workerId = 0;
    bool m_recording = false;

    // 状态去重（减少冗余录制命令）。
    ID3D12RootSignature* m_rootSig = nullptr;
    ID3D12PipelineState* m_pso = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY m_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    // 本 bundle 所属管线的 per-slot 修正 stride（由调用方从 Dx12Pipeline 抄进来）；
    // begin 时清空。不持有 Dx12Pipeline*，理由见 setCorrectedStride 的注释。
    std::unordered_map<int, UINT> m_correctedStrides;

    Stats m_stats;
};

// Bundle 录制器池：每个 worker 一个 recorder。
class BundleRecorderPool {
public:
    BundleRecorderPool() = default;
    ~BundleRecorderPool() = default;

    BundleRecorderPool(const BundleRecorderPool&) = delete;
    BundleRecorderPool& operator=(const BundleRecorderPool&) = delete;

    // 为每个 worker 建一个 BundleRecorder（各自 allocator/list）；失败返回 mcdr::Error。
    mcdr::Result<void> init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT workerCount,
                            ID3D12CommandSignature* cmdSigIndexed, ID3D12CommandSignature* cmdSigNonIndexed);
    void shutdown();

    bool valid() const { return !m_recorders.empty(); }
    UINT workerCount() const { return static_cast<UINT>(m_recorders.size()); }

    // worker 越界返回 nullptr。
    BundleRecorder* recorder(UINT worker);

private:
    std::vector<std::unique_ptr<BundleRecorder>> m_recorders;
};

// 在父（DIRECT）命令列表上执行 bundle；mainList 或 bundle 为空时静默跳过。
void executeBundle(ID3D12GraphicsCommandList* mainList, ID3D12GraphicsCommandList* bundle);


// ===========================================================================
// 实现（原 dx12_async_bundle.cpp，随头文件一并并入模块接口单元）
// ===========================================================================

D3D12_PRIMITIVE_TOPOLOGY bundlePrimitiveTopology(int ordinal) {
    switch (ordinal) {
        case 0:
        case 1: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;   // LINES / DEBUG_LINES
        case 2: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;          // DEBUG_LINE_STRIP
        case 3: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;          // POINTS
        case 5: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;      // TRIANGLE_STRIP
        case 6: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLEFAN;        // TRIANGLE_FAN
        case 7: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;       // QUADS（不支持，回退）
        default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;      // TRIANGLES 等
    }
}

mcdr::Result<void> BundleRecorder::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT workerId,
                                       ID3D12CommandSignature* cmdSigIndexed,
                                       ID3D12CommandSignature* cmdSigNonIndexed) {
    if (!device) return mcdr::err("BundleRecorder::init", "device is null");

    m_device = device;
    m_workerId = workerId;
    m_heap = heap;  // 非拥有；bundle 内 SetGraphicsRootDescriptorTable 前须自设该堆

    for (UINT i = 0; i < kBundleSlotCount; ++i) {
        HRESULT hr =
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&m_allocators[i]));
        if (FAILED(hr))
            return mcdr::err("BundleRecorder::init", "CreateCommandAllocator failed: " + hrText(hr));

        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, m_allocators[i].Get(), nullptr,
                                       IID_PPV_ARGS(&m_lists[i]));
        if (FAILED(hr))
            return mcdr::err("BundleRecorder::init", "CreateCommandList failed: " + hrText(hr));

        // 创建即为 open，先 Close 到初始状态，之后 begin() 再 Reset。
        m_lists[i]->Close();
    }
    m_recording = false;
    for (UINT i = 0; i < kBundleFrameSlots; ++i) {
        m_frameOfSlot[i] = UINT64_MAX;  // 未使用过（fenceValue 从 1 开始递增）
        m_batchOfSlot[i] = 0;
    }

    // 复用现有 ExecuteIndirect 的 command signature（drawIndexed 用 indexed）。
    // 复用现有 ExecuteIndirect 的 command signature（drawIndexed 用 indexed）。
    m_cmdSigIndexed = cmdSigIndexed;
    m_cmdSigNonIndexed = cmdSigNonIndexed;
    return {};
}

bool BundleRecorder::begin(UINT frameSlot, UINT64 frameValue) {
    if (m_recording) return false;
    const UINT fs = frameSlot % kBundleFrameSlots;
    // 新的一帧：该帧槽内的批次序号归零，重新从这批的第一个 allocator 槽开始。
    if (m_frameOfSlot[fs] != frameValue) {
        m_frameOfSlot[fs] = frameValue;
        m_batchOfSlot[fs] = 0;
    }
    const UINT batch = m_batchOfSlot[fs];
    if (batch >= kBundleBatchesPerFrame) return false;  // 本帧配额用尽，回退串行
    m_batchOfSlot[fs] = batch + 1;

    const UINT slot = fs * kBundleBatchesPerFrame + batch;
    m_activeSlot = slot;
    m_bundle = m_lists[slot];
    if (!m_bundle || !m_allocators[slot]) return false;
    HRESULT hr = m_allocators[slot]->Reset();
    if (FAILED(hr)) return false;
    hr = m_bundle->Reset(m_allocators[slot].Get(), nullptr);
    if (FAILED(hr)) return false;

    // D3D12 bundle 不继承父命令列表的描述符堆。本 bundle 会录制
    // SetGraphicsRootDescriptorTable，必须在此（任何描述符表命令之前、且整段
    // bundle 仅一次）显式绑定 shader-visible 堆；否则运行时解析 GPU 句柄时
    // 读到空堆指针（命令列表成员 +0x28）而访问冲突崩溃。
    // 该堆必须与父命令列表执行本 bundle 时绑定的堆完全一致。
    if (m_heap) {
        ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
        m_bundle->SetDescriptorHeaps(1, heaps);
    }

    m_recording = true;
    m_rootSig = nullptr;
    m_pso = nullptr;
    m_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    m_correctedStrides.clear();
    resetStats();
    return true;
}

ID3D12GraphicsCommandList* BundleRecorder::end() {
    if (!m_bundle || !m_recording) return nullptr;
    m_recording = false;
    if (FAILED(m_bundle->Close())) return nullptr;
    return m_bundle.Get();
}

void BundleRecorder::setRootSignature(ID3D12RootSignature* rootSignature) {
    if (!m_recording || !rootSignature || m_rootSig == rootSignature) return;
    m_bundle->SetGraphicsRootSignature(rootSignature);
    m_rootSig = rootSignature;
    ++m_stats.commandsRecorded;
}

void BundleRecorder::setPipelineState(ID3D12PipelineState* pso) {
    if (!m_recording || !pso || m_pso == pso) return;
    m_bundle->SetPipelineState(pso);
    m_pso = pso;
    ++m_stats.commandsRecorded;
}

void BundleRecorder::setCorrectedStride(int slot, UINT stride) { m_correctedStrides[slot] = stride; }


void BundleRecorder::setDescriptorTable(UINT slot, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    if (!m_recording) return;
    m_bundle->SetGraphicsRootDescriptorTable(slot, handle);
    ++m_stats.commandsRecorded;
    ++m_stats.descriptorUpdates;
}

void BundleRecorder::setRootConstantBufferView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS address) {
    if (!m_recording) return;
    m_bundle->SetGraphicsRootConstantBufferView(rootIndex, address);
    ++m_stats.commandsRecorded;
}

void BundleRecorder::setVertexBuffers(UINT startSlot, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) {
    if (!m_recording || !views || count == 0) return;
    m_bundle->IASetVertexBuffers(startSlot, count, views);
    ++m_stats.commandsRecorded;
}

void BundleRecorder::setIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) {
    if (!m_recording || !view) return;
    m_bundle->IASetIndexBuffer(view);
    ++m_stats.commandsRecorded;
}

void BundleRecorder::setPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
    if (!m_recording || m_topology == topology) return;
    m_bundle->IASetPrimitiveTopology(topology);
    m_topology = topology;
    ++m_stats.commandsRecorded;
}

void BundleRecorder::drawIndexedInstanced(UINT indexCount, UINT instanceCount, UINT startIndexLocation,
                                          INT baseVertexLocation, UINT startInstanceLocation) {
    if (!m_recording || indexCount == 0) return;
    m_bundle->DrawIndexedInstanced(indexCount, instanceCount, startIndexLocation, baseVertexLocation,
                                   startInstanceLocation);
    ++m_stats.drawCalls;
    m_stats.indexCount += indexCount;
}

void BundleRecorder::drawInstanced(UINT vertexCount, UINT instanceCount, UINT startVertexLocation,
                                   UINT startInstanceLocation) {
    if (!m_recording || vertexCount == 0) return;
    m_bundle->DrawInstanced(vertexCount, instanceCount, startVertexLocation, startInstanceLocation);
    ++m_stats.drawCalls;
    m_stats.vertexCount += vertexCount;
}

bool BundleRecorder::drawIndexedIndirect(ID3D12Resource* commands, UINT64 offset, UINT drawCount) {
    if (!m_recording || !commands || drawCount == 0) return false;
    // D3D12 禁止向 ExecuteIndirect 传 nullptr command signature。
    if (!m_cmdSigIndexed) return false;
    m_bundle->ExecuteIndirect(m_cmdSigIndexed.Get(), drawCount, commands, offset, nullptr, 0);
    m_stats.drawCalls += drawCount;
    return true;
}

bool BundleRecorder::drawIndirect(ID3D12Resource* commands, UINT64 offset, UINT drawCount) {
    if (!m_recording || !commands || drawCount == 0) return false;
    if (!m_cmdSigNonIndexed) return false;
    m_bundle->ExecuteIndirect(m_cmdSigNonIndexed.Get(), drawCount, commands, offset, nullptr, 0);
    m_stats.drawCalls += drawCount;
    return true;
}

mcdr::Result<void> BundleRecorderPool::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT workerCount,
                                            ID3D12CommandSignature* cmdSigIndexed,
                                            ID3D12CommandSignature* cmdSigNonIndexed) {
    if (!device || workerCount == 0)
        return mcdr::err("BundleRecorderPool::init", "device is null or workerCount == 0");
    shutdown();
    m_recorders.reserve(workerCount);
    for (UINT i = 0; i < workerCount; ++i) {
        std::unique_ptr<BundleRecorder> rec(new BundleRecorder());
        const mcdr::Result<void> r = rec->init(device, heap, i, cmdSigIndexed, cmdSigNonIndexed);
        if (!r) {
            shutdown();
            return mcdr::err("BundleRecorderPool::init", "BundleRecorder::init failed for worker " +
                                                             std::to_string(i) + ": " + r.error().message);
        }
        m_recorders.push_back(std::move(rec));
    }
    return {};
}

void BundleRecorderPool::shutdown() { m_recorders.clear(); }

BundleRecorder* BundleRecorderPool::recorder(UINT worker) {
    if (worker >= m_recorders.size()) return nullptr;
    return m_recorders[worker].get();
}

void executeBundle(ID3D12GraphicsCommandList* mainList, ID3D12GraphicsCommandList* bundle) {
    if (!mainList || !bundle) return;
    mainList->ExecuteBundle(bundle);
}

}  // namespace d3d12_mc
