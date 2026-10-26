module;

#include "_d3d12_module_prelude.h"
#include "mcdr_rt_state.h"  // P117: 光追模式（创建期日志/后续 G-Buffer 都要读）

// RTX（DLSS/NRD）可用性探测接口。和 dxcapi.h 同理必须放在全局模块片段里，
// 否则 MSVC 报 C5244（模块 purview 内不得文本包含头文件）。探测实现在
// src\core\d3d12_rtx_probe.cpp，是普通翻译单元，所以这里只是拿到声明。
#include "mcdr_rtx_support.h"

// P67：DXC（IDxcUtils / IDxcCompiler3 / CLSID_DxcUtils / CLSID_DxcCompiler）。SM 6.x 与
// mesh shader 只能由 DXC 编译（D3DCompile/fxc 停在 SM 5.1）。只用头文件拿接口定义与
// GUID，运行时经 GetProcAddress 调 DxcCreateInstance，不链接 dxcompiler.lib；
// 必须放在全局模块片段里，否则 MSVC 报 C5244（模块 purview 内文本包含头文件）。
#if __has_include(<dxcapi.h>)
#include <dxcapi.h>
#define MCDR_HAVE_DXCAPI 1
#else
#define MCDR_HAVE_DXCAPI 0
#endif

// P104 官方 LUT：SMAA 的 AreaTex / SearchTex 像素字节（定义在
// src\advanced\smaa_luts.cpp，普通翻译单元，见 include\mcdr_smaa_luts.h）。
// 同样必须放在全局模块片段里（否则 C5244）；只取符号，不引入任何依赖。
#include "mcdr_smaa_luts.h"

// P108：DLSS 实现单元（src\advanced\d3d12_dlss.cpp）的 C 接口。与 mcdr_rtx_support.h
// 同理：实现在普通 TU 里，这里只需要声明；必须放在全局模块片段里，否则 C5244。
// 注意：**.cppm 的全局模块片段里的声明不会传给 import 方**，所以本实现单元要自己
// 再 include 一次（include guard 保证幂等）。
#include "mcdr_dlss_impl.h"
#include <cstdio>        // P111 探针：_wfopen / fwprintf（诊断文件，绕开尚未接管的日志 sink）
#include <cwchar>
// P199c：内存分类采样需要 GetProcessMemoryInfo（psapi）。
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

// P111：Streamline（SL）interposer。头文件来自 external\streamline（git 子模块 @v2.14.16），
// 导入库与运行时 DLL 来自 NVIDIA 的 SDK 压缩包（解到 lib\streamline，git 不记录这些二进制）。
// 用 __has_include 判定：全新克隆若还没跑 scripts\sync-external.ps1，这里保持关闭而不是让编译炸。
// 与其它 probing 头同理，必须留在全局模块片段（模块 purview 内文本包含头文件 → C5244）。
#if __has_include(<sl.h>)
#include <sl.h>
#include <sl_dlss.h>   // P112：sl::DLSSMode / sl::DLSSOptions（sl.h 只含 core）
#include <sl_dlss_g.h> // P213：sl::DLSSGOptions / sl::DLSSGState（帧生成）
#define MCDR_HAVE_STREAMLINE 1
#else
#define MCDR_HAVE_STREAMLINE 0
#endif

// JSON 一律用 vcpkg 里的 nlohmann/json（vcpkg.json 已声明 nlohmann-json）。手工拼字符串
// 只要漏一处转义、少一个逗号或多一个引号就是非法 JSON，而解析方
// （D3D12AdapterInfo.parseFlatObject）是宽松的平坦扫描器 —— 非法文本会被当合法值读走，
// 错误完全不报。enumerateAdaptersJson 的 "vid":"0x… 就少了开引号，实测一年多没人发现。
// 头必须留在全局模块片段里（模块 purview 内文本包含头文件 → C5244）。
#include <nlohmann/json.hpp>

module mcdr.d3d12.device;

import mcdr.error;
import mcdr.d3d12.async_executor;   // 文件级 MainCommandExecutor
import mcdr.d3d12.async_bundle;     // BundleRecorder / BundleRecorderPool
import mcdr.d3d12.async_descriptor; // PartitionedDescriptorAllocator
import mcdr.d3d12.async_fence;      // AsyncFenceManager

// P66：IDXGIFactory6 / DXGI_GPU_PREFERENCE（按 GPU 偏好顺序选适配器）。
// P67：DXC（IDxcCompiler3 / IDxcUtils）。SM 6.x 与 mesh shader 只能由 DXC 编译
// （D3DCompile/fxc 停在 SM 5.1）。这里只用头文件拿接口定义，运行时通过
// GetProcAddress 调用 DxcCreateInstance，不链接 dxcompiler.lib —— DXC 是可选依赖，
// 缺失时退化为「没有 mesh shader 编译能力」，不影响既有渲染路径。

namespace d3d12_mc {

Dx12Surface* gActiveSurface = nullptr;

namespace {

DeviceContext* makeDeviceContext() {
    return new DeviceContext();
}
DeviceContext& gCtx = *makeDeviceContext();

struct PendingDelete {
    Dx12Object* obj;
    UINT64 requiredFence;
};
std::vector<PendingDelete> gPendingDeletes;
int gOpenListCount = 0;
constexpr UINT64 kFenceBlockedByOpenList = UINT64_MAX;

Dx12Object* volatile gPdDeletingObj = nullptr;
volatile long gPdDeletingIndex = -1;
volatile long gPdDeletingTotal = 0;
volatile long gPdInProgress = 0;

std::vector<UINT> gFreeSrvSlots;
std::vector<UINT> gFreeSamplerSlots;

std::mutex gPendingMutex;
std::mutex gFreeSlotMutex;
// P175：设备初始化的串行化锁。ensureDevice() 有 18 个懒调用点（渲染线程 / native async 渲染
// 线程 / JVM 侧），没有这把锁时两个线程可能同时走慢路径：`gCtx.device` 在 heap 之前就写进去，
// 另一边会拿到「新 device + 旧 heap」并拿去建描述符 —— 跨设备使用 D3D12 对象是未定义行为。
std::mutex gDeviceInitMutex;

struct PooledBuffer {
    Dx12Object* obj;
};
std::vector<PooledBuffer> gFreeBuffers;
size_t gFreeBufferBytes = 0;
std::mutex gFreeBufferMutex;
constexpr size_t kMaxFreeBufferBytes = 96ull * 1024 * 1024;

UINT64 bufferCapacity(const Dx12Object* o) {
    if (o->resource) {
        return o->resource->GetDesc().Width;
    }
    return (static_cast<UINT64>(o->size) + 255ULL) & ~255ULL;
}

Dx12Object* takePooledBuffer(D3D12_HEAP_TYPE heapType, UINT64 capacity) {
    std::lock_guard<std::mutex> lk(gFreeBufferMutex);
    size_t best = SIZE_MAX;
    UINT64 bestCap = 0;
    for (size_t i = 0; i < gFreeBuffers.size(); ++i) {
        PooledBuffer& pb = gFreeBuffers[i];
        if (pb.obj->heapType != heapType) continue;
        UINT64 cap = bufferCapacity(pb.obj);
        if (cap < capacity) continue;
        if (best == SIZE_MAX || cap < bestCap) {
            best = i;
            bestCap = cap;
        }
    }
    if (best == SIZE_MAX) return nullptr;
    Dx12Object* obj = gFreeBuffers[best].obj;
    // 记账量被 kMaxFreeBufferBytes（96 MiB）封顶，32 位 size_t 也不会溢出，故显式收窄
    // 以免 x86 下触发 C4244（x64 下与原先完全等价）。
    gFreeBufferBytes -= static_cast<size_t>(bestCap);
    gFreeBuffers[best] = gFreeBuffers.back();
    gFreeBuffers.pop_back();
    return obj;
}

// P199h：下面的释放路径要改记账，先把声明提到使用点之前（定义在后面）。
extern std::atomic<long long> gLiveBuf;
void accountLiveBuffer(D3D12_HEAP_TYPE heapType, long long bytes);

void recyclePooledBuffer(Dx12Object* obj) {
    UINT64 capacity = bufferCapacity(obj);
    // P199h（内存泄露根因）：READBACK 堆缓冲**不进池**。它们基本都是「一次性读回」
    // （典型：vanilla 每秒一次的 Screenshot buffer，1.6MB，MAP_READ|COPY_DST），
    // 复用一个就必须保证尺寸完全一致，实际几乎从不命中；进池只会让 96MiB 的上限
    // 被这种大块一次性缓冲占满，池满后每个新块都再分配一张新的（进程 nonLocal/私有
    // 字节随之线性上涨，旧块只能等池淘汰）。读回缓冲创建频率低、CPU 可见、也不适合
    // 跨帧复用，直接释放即可。
    const bool poolable = (obj->heapType != D3D12_HEAP_TYPE_READBACK);
    if (poolable) {
        std::lock_guard<std::mutex> lk(gFreeBufferMutex);
        if (gFreeBufferBytes + static_cast<size_t>(capacity) <= kMaxFreeBufferBytes) {
            gFreeBuffers.push_back(PooledBuffer{obj});
            gFreeBufferBytes += static_cast<size_t>(capacity);
            return;
        }
    }
    // 真正的释放路径：记账必须同步减掉，否则 P199c 的 up/def/rb 三个数只增不减
    // （之前 lib 计数 bufTotal==liveBuf 恒等就是这么来的）。
    --gLiveBuf;
    accountLiveBuffer(obj->heapType, -static_cast<long long>(capacity));
    delete obj;
}

UINT gNextSrv = 0;
UINT gNextRtv = 0;
UINT gNextFrameRtv = 0;
UINT gNextDsv = 0;
// P109（#538 根因修复）：帧级 RTV/DSV 描述符堆的游标是**设备级**的（一次只有一个），而
// (资源, mip) -> 句柄缓存是**每个 CommandContext** 一份。任何 ctx 的 beginCommandList 都会把
// 游标倒回 0，于是它之后创建的描述符会覆盖别的 ctx 缓存里仍然指向旧槽位的句柄 —— 缓存命中
// 拿到的句柄已经指向**另一张资源**，pass 就把那张资源当成 RTV 绑上去。实机（p76btest，
// DX12_DEBUG_LAYER=1）抓到的正是这一步：
//   .850 passBind color0Res=0000029CA3141060 rtvHandle=000002BACECFE0E0（.839 为它 slot=0 创建）
//   .859 passBind color0Res=0000029460399BF0 rtvHandle=000002BACECFE0E0（同一个句柄，另一张资源）
// ⇒ #538「resource state 0xC0 invalid for use as a render target」。
// 修法：给描述符堆一个**代**（generation）。游标每被倒回一次就 ++，缓存按代失效，
// 句柄绝不会跨代复用。
UINT gFrameViewGeneration = 1;
UINT gNextSampler = 0;

constexpr UINT kSrvHeapSize = 262144;
constexpr UINT kRtvHeapSize = 2048;
constexpr UINT kFrameRtvHeapSize = 256;
constexpr UINT kDsvHeapSize = 256;
constexpr UINT kSamplerHeapSize = 2048;
constexpr UINT kDrawHeapPerFrame = kAsyncSyncRingReserve + kAsyncWorkerSlotsPerSection;
constexpr UINT kDrawHeapSections = 4;
constexpr UINT kDrawHeapSize = kDrawHeapPerFrame * kDrawHeapSections;

inline UINT syncRingCapacity() { return gCtx.syncRingReserve > 0 ? gCtx.syncRingReserve : kDrawHeapPerFrame; }

// TEMP-P199d（泄露取证，验证完必须删）：每类 D3D12 创建调用的累计次数。对比相邻两次
// P199b 采样的差值，就能判断常驻内存上涨是不是"每帧重复创建资源"造成的。计数器放在
// TU 里（模块单元那侧访问不到），只有诊断用途，不进任何发布路径。
std::atomic<uint64_t> gStatTexCreate{0};
std::atomic<uint64_t> gStatBufCreate{0};
std::atomic<uint64_t> gStatPsoCreate{0};
// P199d: 存活对象数（create++ / delete--）。P199b 只统计“创建过的总数”，对象泄漏时看不出来。
std::atomic<long long> gLiveTex{0};
std::atomic<long long> gLiveBuf{0};
// P199f: 存活缓冲字节数按堆类型分组（UPLOAD / DEFAULT / READBACK）。泄漏到底是
// “CPU 可见的上传缓冲”还是“默认堆纹理/网格缓冲”，看这三个数就够。
std::atomic<long long> gLiveUploadBytes{0};
std::atomic<long long> gLiveDefaultBytes{0};
std::atomic<long long> gLiveReadbackBytes{0};
static void accountLiveBuffer(D3D12_HEAP_TYPE heapType, long long bytes) {
    if (heapType == D3D12_HEAP_TYPE_UPLOAD) gLiveUploadBytes += bytes;
    else if (heapType == D3D12_HEAP_TYPE_READBACK) gLiveReadbackBytes += bytes;
    else gLiveDefaultBytes += bytes;
}
std::atomic<uint64_t> gStatEncoderCreate{0};

size_t pendingDeletesCount() {
    std::lock_guard<std::mutex> lk(gPendingMutex);
    return gPendingDeletes.size();
}
int popFreeSrvSlot() {
    std::lock_guard<std::mutex> lk(gFreeSlotMutex);
    if (gFreeSrvSlots.empty()) return -1;
    int slot = static_cast<int>(gFreeSrvSlots.back());
    gFreeSrvSlots.pop_back();
    return slot;
}
int popFreeSamplerSlot() {
    std::lock_guard<std::mutex> lk(gFreeSlotMutex);
    if (gFreeSamplerSlots.empty()) return -1;
    int slot = static_cast<int>(gFreeSamplerSlots.back());
    gFreeSamplerSlots.pop_back();
    return slot;
}
size_t freeSrvSlotCount() {
    std::lock_guard<std::mutex> lk(gFreeSlotMutex);
    return gFreeSrvSlots.size();
}
size_t freeSamplerSlotCount() {
    std::lock_guard<std::mutex> lk(gFreeSlotMutex);
    return gFreeSamplerSlots.size();
}

mcdr::Result<int> allocSrvSlot() {
    int slot = popFreeSrvSlot();
    if (slot >= 0) return slot;
    if (gNextSrv >= kSrvHeapSize && gOpenListCount == 0 && pendingDeletesCount() > 0) {
        flushPendingDeletes();
        slot = popFreeSrvSlot();
        if (slot >= 0) return slot;
    }
    if (gNextSrv >= kSrvHeapSize) {
        return mcdr::err("allocSrvSlot",
                         "srv heap exhausted (next=" + std::to_string(static_cast<long long>(gNextSrv)) +
                             " free=" + std::to_string(static_cast<long long>(freeSrvSlotCount())) +
                             " pending=" + std::to_string(static_cast<long long>(pendingDeletesCount())) +
                             " openLists=" + std::to_string(gOpenListCount) + ")");
    }
    return static_cast<int>(gNextSrv++);
}

mcdr::Result<int> allocSamplerSlot() {
    int slot = popFreeSamplerSlot();
    if (slot >= 0) return slot;
    if (gNextSampler >= kSamplerHeapSize && gOpenListCount == 0 && pendingDeletesCount() > 0) {
        flushPendingDeletes();
        slot = popFreeSamplerSlot();
        if (slot >= 0) return slot;
    }
    if (gNextSampler >= kSamplerHeapSize) {
        return mcdr::err("allocSamplerSlot",
                         "sampler heap exhausted (next=" + std::to_string(static_cast<long long>(gNextSampler)) +
                             " free=" + std::to_string(static_cast<long long>(freeSamplerSlotCount())) +
                             " pending=" + std::to_string(static_cast<long long>(pendingDeletesCount())) +
                             " openLists=" + std::to_string(gOpenListCount) + ")");
    }
    return static_cast<int>(gNextSampler++);
}

bool createHeap(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count,
                D3D12_DESCRIPTOR_HEAP_FLAGS flags, ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = flags;
    return SUCCEEDED(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&out)));
}

std::string hrText(HRESULT hr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

std::string queryAdapterName(ID3D12Device* dev) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return "<factory failed>";
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(factory->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
        return "<enum adapter failed>";
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) return "<get desc failed>";
    char name[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    return name;
}

std::string deviceStatusText() {
    std::string s;
    if (!gCtx.device) return s;
    HRESULT reason = gCtx.device->GetDeviceRemovedReason();
    s += "GetDeviceRemovedReason=" + hrText(reason);
    if (gCtx.infoQueue) {
        UINT64 count = gCtx.infoQueue->GetNumStoredMessages();
        UINT64 start = count > 24 ? count - 24 : 0;
        for (UINT64 i = start; i < count; ++i) {
            SIZE_T len = 0;
            if (FAILED(gCtx.infoQueue->GetMessage(static_cast<UINT>(i), nullptr, &len))) continue;
            std::vector<char> buf(len > 0 ? len : 1);
            D3D12_MESSAGE* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(gCtx.infoQueue->GetMessage(static_cast<UINT>(i), msg, &len))) {
                s += " | msg[" + std::to_string(static_cast<long long>(i)) + "](";
                s += msg->pDescription ? msg->pDescription : "";
                s += ")";
            }
        }
    }
    return s;
}

void dumpInfoQueueMessages() {
    if (!gCtx.infoQueue) return;
    UINT64 count = gCtx.infoQueue->GetNumStoredMessages();
    if (count == 0) return;
    UINT64 start = count > 16 ? count - 16 : 0;
    for (UINT64 i = start; i < count; ++i) {
        SIZE_T len = 0;
        if (FAILED(gCtx.infoQueue->GetMessage(static_cast<UINT>(i), nullptr, &len))) continue;
        std::vector<char> buf(len > 0 ? len : 1);
        D3D12_MESSAGE* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(gCtx.infoQueue->GetMessage(static_cast<UINT>(i), msg, &len))) {
            const char* sev = "?";
            switch (msg->Severity) {
                case D3D12_MESSAGE_SEVERITY_CORRUPTION: sev = "CORRUPTION"; break;
                case D3D12_MESSAGE_SEVERITY_ERROR: sev = "ERROR"; break;
                case D3D12_MESSAGE_SEVERITY_WARNING: sev = "WARNING"; break;
                case D3D12_MESSAGE_SEVERITY_INFO: sev = "INFO"; break;
                default: break;
            }
            // 按调试层 severity 分级：CORRUPTION/ERROR -> error（默认可见），
            // WARNING -> warn，INFO/MESSAGE -> info（需 DX12_LOG_VERBOSE）。
            const int sevInt = static_cast<int>(msg->Severity);
            if (sevInt <= static_cast<int>(D3D12_MESSAGE_SEVERITY_ERROR)) {
                dbgLogError("InfoQueue[%s] %s", sev, msg->pDescription ? msg->pDescription : "");
            } else if (sevInt == static_cast<int>(D3D12_MESSAGE_SEVERITY_WARNING)) {
                dbgLogWarn("InfoQueue[%s] %s", sev, msg->pDescription ? msg->pDescription : "");
            } else {
                dbgLogInfo("InfoQueue[%s] %s", sev, msg->pDescription ? msg->pDescription : "");
            }
        }
    }
    gCtx.infoQueue->ClearStoredMessages();
}

}  // namespace

// 诊断：设备移除原因 + InfoQueue 最近消息。presentSurface 失败时调用（见 dx12_surface.cpp），
// 未开调试层时这是唯一能说明「为什么 0x887A0005」的证据。
void logDeviceStatus(const char* tag) {
    const std::string s = deviceStatusText();
    if (s.empty()) return;
    dbgLogError("%s: device status: %s", tag, s.c_str());
}

void flushPendingDeletes(bool force) {
    if (InterlockedExchange(&gPdInProgress, 1) != 0) {
        dbgLog("flushPendingDeletes: concurrent/re-entrant call skipped (pending=%lld)",
               static_cast<long long>(pendingDeletesCount()));
        return;
    }
    UINT64 gpuDone = UINT64_MAX;
    if (!force && gCtx.queueFence) {
        gpuDone = gCtx.queueFence->GetCompletedValue();
    }
    const bool canResolveBlocked = (gOpenListCount == 0);
    const UINT64 resolveFence = gCtx.queueFenceValue;
    std::vector<PendingDelete> local;
    {
        std::lock_guard<std::mutex> lk(gPendingMutex);
        std::vector<PendingDelete> keep;
        keep.reserve(gPendingDeletes.size());
        for (PendingDelete& pd : gPendingDeletes) {
            if (force) {
                local.push_back(pd);
                continue;
            }
            if (pd.requiredFence == kFenceBlockedByOpenList) {
                if (!canResolveBlocked) {
                    keep.push_back(pd);
                    continue;
                }
                pd.requiredFence = resolveFence;
            }
            if (pd.requiredFence <= gpuDone) {
                local.push_back(pd);
            } else {
                keep.push_back(pd);
            }
        }
        gPendingDeletes.swap(keep);
    }
    gPdDeletingTotal = static_cast<long>(local.size());
    if (gPdDeletingTotal > 0) {
        dbgLogDebug("flushPendingDeletes: begin count=%ld deferred=%lld gpuDone=%llu", gPdDeletingTotal,
                    static_cast<long long>(pendingDeletesCount()), static_cast<unsigned long long>(gpuDone));
    }
    long idx = 0;
    for (const PendingDelete& pd : local) {
        Dx12Object* o = pd.obj;
        gPdDeletingObj = o;
        gPdDeletingIndex = idx++;
        if (o->descSlot >= 0) {
            std::lock_guard<std::mutex> lk(gFreeSlotMutex);
            if (o->kind == Dx12Object::Kind::TextureView) {
                gFreeSrvSlots.push_back(static_cast<UINT>(o->descSlot));
            } else if (o->kind == Dx12Object::Kind::Sampler) {
                gFreeSamplerSlots.push_back(static_cast<UINT>(o->descSlot));
            }
        }
        if (!force && o->kind == Dx12Object::Kind::Buffer) {
            recyclePooledBuffer(o);
            continue;
        }
        if (o->kind == Dx12Object::Kind::Texture) --gLiveTex;
        else if (o->kind == Dx12Object::Kind::Buffer) {
            --gLiveBuf;
            if (o->resource) accountLiveBuffer(o->heapType, -static_cast<long long>(o->resource->GetDesc().Width));
        }
        delete o;
    }
    gPdDeletingObj = nullptr;
    gPdDeletingIndex = -1;
    gPdDeletingTotal = 0;
    InterlockedExchange(&gPdInProgress, 0);
    dbgLogDebug("flushPendingDeletes: done");
}

double nowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// 0=error … 3=debug. The default matches the mod's own default (hide everything
// but errors): the JVM pushes its configured level through `mcdrSetLogLevel`
// before the device is created, so this value only covers the window in between.
static int gLogLevel = 0;

// P184 补记：关停/初始化这类"进度标记"是否**恒定输出**（true = 无视 gLogLevel；false = 当普通
// INFO，被 logLevel 过滤）。由配置项 `[logging] shutdownTrace` 经 `mcdrSetShutdownTraceAlways`
// 推进来。默认 true：destroyDevice/destroySurface 一旦卡住，这些行是唯一线索。
static bool gShutdownTraceAlways = true;

// P250：配置项 `[logging] nativeLogFile`（JVM 经 mcdrSetLogFile 推进来）。原生日志默认只走
// stderr，而 Prism 之类的启动器不把子进程 stdout/stderr 落盘 —— 于是"用户端出问题，却拿不到
// native 行"成了常态。这里提供一条文件镜像：设了路径就逐行追加（带 fflush，进程被强杀也不丢）。
static std::mutex gMirrorMutex;
static std::string gConfiguredLogPath;
static FILE* gMirrorFile = nullptr;

static void mirrorLogToFile(const char* line) {
    const std::lock_guard<std::mutex> lock(gMirrorMutex);
    // 配置项优先；旧环境变量 `DX12_NATIVE_LOG_FILE` 只作为**已弃用的逃生口**保留，且仅在配置为空
    // 时生效（AGENTS.md：环境变量不得成为功能的唯一入口）。两者都为空 ⇒ 不写文件。
    const char* path = !gConfiguredLogPath.empty() ? gConfiguredLogPath.c_str()
                                                  : std::getenv("DX12_NATIVE_LOG_FILE");
    if (path == nullptr || path[0] == '\0') return;
    if (gMirrorFile == nullptr) {
        gMirrorFile = std::fopen(path, "a");
        if (gMirrorFile == nullptr) return;
    }
    std::fprintf(gMirrorFile, "%s\n", line);
    std::fflush(gMirrorFile);
}

void setLogFile(const char* utf8Path) {
    // 空路径 = 关闭镜像并释放句柄；换路径同样先关旧句柄，下一次写日志时按新路径重开。
    const std::lock_guard<std::mutex> lock(gMirrorMutex);
    gConfiguredLogPath = (utf8Path != nullptr) ? utf8Path : "";
    if (gMirrorFile != nullptr) {
        std::fclose(gMirrorFile);
        gMirrorFile = nullptr;
    }
}

// JVM 侧是 log4j 的行格式；这里逐字复刻同样的形状（见 mcdr_device.cppm 的声明），
// 让 native 与 JVM 两路输出在同一个控制台里可以按时间戳直接对齐。旧前缀
// `[dx12][t=<QPC 毫秒>]` 做不到这一点：那个「进程运行到第几毫秒」在 JVM 行里不存在。
//
// 时间块逐字对齐 JVM 的实际输出（本机 log4j 的 pattern 是 `ddMM月yyyy HH:mm:ss.SSS`，
// 实测行首 `[1510月2026 15:29:44.942]`：日 15 + 月 10 + 「月」+ 年 2026），字段顺序
// 与分隔符都不能改，否则两路输出排在一起时时间戳对不上。
int formatLogPrefix(char* out, size_t outSize, const char* level, const char* logger) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return std::snprintf(out, outSize, "[%02u%02u月%04u %02u:%02u:%02u.%03u] [thread-%lu/%s] [%s/]: ",
                         static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wMonth),
                         static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wHour),
                         static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                         static_cast<unsigned>(st.wMilliseconds),
                         static_cast<unsigned long>(GetCurrentThreadId()), level, logger);
}

// 单行出口：前缀 + 正文 + 换行，同时写 stderr 与（可选的）镜像文件。
static void emitLogLine(const char* level, const char* line) {
    char full[2304];
    const int n = formatLogPrefix(full, sizeof(full), level, "dx12");
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(full)) return;  // 前缀都放不下：丢行，不写半行
    std::snprintf(full + n, sizeof(full) - static_cast<size_t>(n), "%s", line);
    std::fprintf(stderr, "%s\n", full);
    fflush(stderr);
    mirrorLogToFile(full);
}

void dbgLog(const char* fmt, ...) {
    if (gLogLevel < 1) return; // WARN=1
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("WARN", line);
}
void dbgLogError(const char* fmt, ...) {
    if (gLogLevel < 0) return; // ERR=0：始终输出（gLogLevel 被钳制在 0..3，故永不返回）
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("ERROR", line);
}
void dbgLogWarn(const char* fmt, ...) {
    if (gLogLevel < 1) return; // WARN=1，与 dbgLog 同门槛（仅用于区分语义）
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("WARN", line);
}
void dbgLogInfo(const char* fmt, ...) {
    if (gLogLevel < 2) return; // INFO=2
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("INFO", line);
}
// P184 补记：进度标记（关停/初始化这类）。按 **INFO** 标注 —— 以前借用 dbgLogError，于是每次
// 正常退出都会刷一串 `[ERROR]`，既误导用户，也让真正要找 ERROR 的人被噪音淹掉。
// 输出策略由配置 `[logging] shutdownTrace` 决定（`mcdrSetShutdownTraceAlways`）：
//   true （默认）：无视 gLogLevel 恒定输出 —— 退出路径卡死时这是唯一线索；
//   false        ：等同普通 INFO，被 logLevel 过滤掉。
void dbgLogProgress(const char* fmt, ...) {
    if (!gShutdownTraceAlways && gLogLevel < 2) return; // INFO=2
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("INFO", line);
}
void dbgLogDebug(const char* fmt, ...) {
    if (gLogLevel < 3) return; // DEBUG=3
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emitLogLine("DEBUG", line);
}
void setLogLevel(int level) { gLogLevel = (level < 0) ? 0 : (level > 3) ? 3 : level; }
// P184 补记：配置项 [logging] shutdownTrace → 进度标记是否恒定输出（见 dbgLogProgress）。
void setShutdownTraceAlways(bool always) { gShutdownTraceAlways = always; }

static DWORD gInfoQueueCookie = 0;
static void CALLBACK dx12DebugMessageCallback(D3D12_MESSAGE_CATEGORY /*category*/,
                                              D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                                              LPCSTR description, void* /*context*/) {
    static const char* kSev[] = {"CORRUPTION", "ERROR", "WARNING", "INFO", "MESSAGE"};
    const int si = static_cast<int>(severity);
    const char* sevName = (si >= 0 && si <= 4) ? kSev[si] : "?";
    const char* text = description ? description : "(null)";
    // 分级按 severity：0=CORRUPTION / 1=ERROR / 2=WARNING 默认可见；
    // 3=INFO / 4=MESSAGE 只在 INFO 级别以上输出（原先这两级被无条件丢弃）。
    if (si <= static_cast<int>(D3D12_MESSAGE_SEVERITY_ERROR)) {
        dbgLogError("[D3D12 %s #%d] %s", sevName, static_cast<int>(id), text);
    } else if (si == static_cast<int>(D3D12_MESSAGE_SEVERITY_WARNING)) {
        dbgLogWarn("[D3D12 %s #%d] %s", sevName, static_cast<int>(id), text);
    } else {
        dbgLogInfo("[D3D12 %s #%d] %s", sevName, static_cast<int>(id), text);
    }
}

static LONG WINAPI dx12CrashFilter(EXCEPTION_POINTERS* ep) {
    if (ep != nullptr && ep->ExceptionRecord != nullptr) {
        EXCEPTION_RECORD* rec = ep->ExceptionRecord;
        char modPath[MAX_PATH] = "<unknown>";
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rec->ExceptionAddress), &mod)) {
            GetModuleFileNameA(mod, modPath, MAX_PATH);
        }
        std::fprintf(stderr, "[dx12] FATAL native exception code=0x%08X addr=%p tid=%lu module=%s\n",
                     static_cast<unsigned>(rec->ExceptionCode), rec->ExceptionAddress,
                     static_cast<unsigned long>(GetCurrentThreadId()), modPath);
        std::fprintf(stderr,
                     "[dx12] FATAL context: pendingDeletes=%lld openList=%d flushInProgress=%ld "
                     "deletingObj=%p deletingIndex=%ld deletingTotal=%ld\n",
                     static_cast<long long>(gPendingDeletes.size()), gOpenListCount, gPdInProgress,
                     reinterpret_cast<void*>(gPdDeletingObj), gPdDeletingIndex, gPdDeletingTotal);
        Dx12Object* dObj = gPdDeletingObj;
        if (dObj) {
            std::fprintf(stderr,
                         "[dx12] FATAL deletingObj: kind=%d descSlot=%d resource=%p "
                         "sourceTexture=%p mappedPtr=%p size=%lld\n",
                         static_cast<int>(dObj->kind), dObj->descSlot,
                         reinterpret_cast<void*>(dObj->resource.Get()),
                         reinterpret_cast<void*>(dObj->sourceTexture), dObj->mappedPtr, dObj->size);
        }
        if (gCtx.device) {
            std::fprintf(stderr, "[dx12] FATAL deviceRemovedReason=0x%08X\n",
                         static_cast<unsigned>(gCtx.device->GetDeviceRemovedReason()));
        }
        void* frames[48];
        USHORT nframes = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        std::fprintf(stderr, "[dx12] FATAL backtrace: %u frames\n", static_cast<unsigned>(nframes));
        for (USHORT i = 0; i < nframes; ++i) {
            HMODULE fm = nullptr;
            char fpath[MAX_PATH] = "<unknown>";
            unsigned long long rva = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(frames[i]), &fm)) {
                GetModuleFileNameA(fm, fpath, MAX_PATH);
                rva = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i])
                                                      - reinterpret_cast<uintptr_t>(fm));
            }
            const char* fb = fpath;
            for (const char* p = fpath; *p; ++p) {
                if (*p == '\\' || *p == '/') fb = p + 1;
            }
            std::fprintf(stderr, "  [%02u] %s+0x%llx\n", static_cast<unsigned>(i), fb, rva);
        }
        std::fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static void installCrashHandlerOnce() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    SetUnhandledExceptionFilter(&dx12CrashFilter);
}

// ---------------------------------------------------------------------------
// P66：设备功能级 12_2 基线 —— 适配器选择与能力查询
//
// 旧实现是 D3D12CreateDevice(nullptr, level, ...)：nullptr 取的是"默认适配器"，
// 在混合显卡机器上通常就是驱动主输出的核显（本机实测 Intel UHD 只能到 12_1、
// MeshShaderTier=0），而窗口（GLFW/OpenGL）和实际渲染其实在独显上。设备与窗口
// 不在同一块 GPU 上时，CreateSwapChainForHwnd 会对所有 tier 返回
// E_ACCESSDENIED(0x80070005)，于是只能回退 CreateSwapChainForComposition——
// 这正是 RTSS 之类挂在 HWND Present 上的 overlay 无法显示的原因之一。
//
// 现在改成：按 IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE) 的顺序
// 枚举硬件适配器，取第一块能建出 12_2 的设备；一块都没有就明确报错，不再像以前
// 那样静默退到 11_0/12_1——那些级别下 mesh shader、DXR 1.1 之类目标特性根本不存在。
// ---------------------------------------------------------------------------

namespace {

// 设备功能级下限：12_2 == D3D12 Ultimate。
constexpr D3D_FEATURE_LEVEL kRequiredFeatureLevel = D3D_FEATURE_LEVEL_12_2;

constexpr D3D_FEATURE_LEVEL kProbeLevels[] = {
    D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
    D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
};

// 某块适配器能建出的最高功能级；(D3D_FEATURE_LEVEL)0 表示完全不支持 D3D12。
// P112：Streamline 的 manual hooking 要求**设备本身也由 interposer 代理创建**
// （ProgrammingGuideManualHooking.md:143-146：`proxyDevice = D3D12CreateDevice();`，再用
// slGetNativeInterface/slUpgradeInterface 取原生设备）。否则 SL 不会把设备登记进它的管线，
// DLSS 插件虽然加载（sl.log 有 "Loaded plugin 'sl.dlss'"）但特性永远 not ready ——
// P112 实测 slIsFeatureLoaded / slGetFeatureFunction 都返回 0x1F（eErrorFeatureNotSupported）。
// 这里运行时解析 interposer 的 D3D12CreateDevice；未请求 streamline 时退回系统 d3d12.dll，
// 默认路径与改动前完全一致。
typedef HRESULT(WINAPI* PFN_SL_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

PFN_SL_D3D12CreateDevice interposerCreateDeviceFn() {
    static const PFN_SL_D3D12CreateDevice fn = []() -> PFN_SL_D3D12CreateDevice {
        // P118：统一决策（默认 Streamline，缺运行库自动回退）。
        const bool want = mcdr_dlss_streamline_requested() != 0;
        if (!want) return nullptr;
        HMODULE self = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(&interposerCreateDeviceFn), &self)) {
            return nullptr;
        }
        wchar_t path[MAX_PATH] = {};
        if (!::GetModuleFileNameW(self, path, MAX_PATH)) return nullptr;
        std::wstring modulePath(path);
        const auto slash = modulePath.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return nullptr;
        const std::wstring dir = modulePath.substr(0, slash + 1);
        HMODULE mod = ::LoadLibraryW((dir + L"sl.interposer.dll").c_str());
        if (mod == nullptr) return nullptr;
        return reinterpret_cast<PFN_SL_D3D12CreateDevice>(::GetProcAddress(mod, "D3D12CreateDevice"));
    }();
    return fn;
}

// 设备创建入口：streamline 模式下走 interposer 代理，否则原生。
HRESULT createD3D12Device(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** out) {
    const PFN_SL_D3D12CreateDevice proxy = interposerCreateDeviceFn();
    if (proxy != nullptr) {
        return proxy(adapter, level, riid, out);
    }
    return ::D3D12CreateDevice(adapter, level, riid, out);
}

D3D_FEATURE_LEVEL probeHighestFeatureLevel(IDXGIAdapter* adapter) {
    for (auto level : kProbeLevels) {
        ComPtr<ID3D12Device> probe;
        if (SUCCEEDED(D3D12CreateDevice(adapter, level, IID_PPV_ARGS(&probe)))) {
            return level;
        }
    }
    return static_cast<D3D_FEATURE_LEVEL>(0);
}

std::string adapterDescName(const DXGI_ADAPTER_DESC1& desc) {
    char name[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    return name;
}

// 枚举硬件适配器（跳过 WARP / Basic Render Driver 这类软件适配器），按 GPU 偏好
// 顺序返回第一块支持 12_2 的。失败时 Error 的 message 带上每块候选的最高功能级，方便
// 用户在日志里直接看到是哪块卡不达标。
mcdr::Result<ComPtr<IDXGIAdapter1>> selectDeviceAdapter() {
    constexpr UINT kMaxAdapters = 32;
    std::string report;

    // IDXGIFactory6（Win10 1803+）拿不到就退回默认枚举顺序。
    ComPtr<IDXGIFactory6> factory6;
    ComPtr<IDXGIFactory4> factory4;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory6)))) {
        factory6 = nullptr;
    }
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4)))) {
        return mcdr::err("selectDeviceAdapter", "CreateDXGIFactory failed");
    }

    for (UINT i = 0; i < kMaxAdapters; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory6) {
            if (FAILED(factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                            IID_PPV_ARGS(&adapter)))) {
                break;
            }
        } else if (FAILED(factory4->EnumAdapters1(i, &adapter))) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        const std::string name = adapterDescName(desc);
        if ((desc.Flags & static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE)) != 0u) {
            report += "  [software] " + name + "\n";
            continue;
        }
        const D3D_FEATURE_LEVEL best = probeHighestFeatureLevel(adapter.Get());
        report += "  " + name + " -> " + featureLevelName(best) + "\n";
        if (best >= kRequiredFeatureLevel) {
            dbgLogInfo("P66 adapter select: %s (highest FL=%s)", name.c_str(), featureLevelName(best));
            return adapter;
        }
    }

    const std::string noAdapter = "no adapter supports D3D_FEATURE_LEVEL_12_2; candidates:\n" + report;
    dbgLogError("%s", noAdapter.c_str());
    return mcdr::err("selectDeviceAdapter", noAdapter);
}

}  // namespace

const char* featureLevelName(D3D_FEATURE_LEVEL level) {
    switch (level) {
        case D3D_FEATURE_LEVEL_12_2: return "12_2";
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        case D3D_FEATURE_LEVEL_10_1: return "10_1";
        case D3D_FEATURE_LEVEL_10_0: return "10_0";
        case D3D_FEATURE_LEVEL_9_3: return "9_3";
        case D3D_FEATURE_LEVEL_9_2: return "9_2";
        case D3D_FEATURE_LEVEL_9_1: return "9_1";
        default: return "unknown";
    }
}

const char* shaderModelName(D3D_SHADER_MODEL model) {
    switch (model) {
        case D3D_SHADER_MODEL_5_1: return "5.1";
        case D3D_SHADER_MODEL_6_0: return "6.0";
        case D3D_SHADER_MODEL_6_1: return "6.1";
        case D3D_SHADER_MODEL_6_2: return "6.2";
        case D3D_SHADER_MODEL_6_3: return "6.3";
        case D3D_SHADER_MODEL_6_4: return "6.4";
        case D3D_SHADER_MODEL_6_5: return "6.5";
        case D3D_SHADER_MODEL_6_6: return "6.6";
        case D3D_SHADER_MODEL_6_7: return "6.7";
        case D3D_SHADER_MODEL_6_8: return "6.8";
        case D3D_SHADER_MODEL_6_9: return "6.9";
        default: break;
    }
    // 6.10 (0x6A) 尚未出现在 10.0.26100 SDK 的 D3D_SHADER_MODEL 枚举里（最高只到
    // D3D_SHADER_MODEL_6_9 = 0x69），但运行时能报出来、kShaderModelCeiling 也钉在它上面，
    // 所以单独判一次。放在 switch 之后是因为 C4063：case 标签取值若不在枚举值域内，
    // /W4 会直接报"case 不是有效值"，而本项目 TreatWarningAsError=true。
    if (model == kShaderModelCeiling) return "6.10";
    return "unknown";
}

const char* shaderModelProfileSuffix(D3D_SHADER_MODEL model) {
    switch (model) {
        case D3D_SHADER_MODEL_6_0: return "6_0";
        case D3D_SHADER_MODEL_6_1: return "6_1";
        case D3D_SHADER_MODEL_6_2: return "6_2";
        case D3D_SHADER_MODEL_6_3: return "6_3";
        case D3D_SHADER_MODEL_6_4: return "6_4";
        case D3D_SHADER_MODEL_6_5: return "6_5";
        case D3D_SHADER_MODEL_6_6: return "6_6";
        case D3D_SHADER_MODEL_6_7: return "6_7";
        case D3D_SHADER_MODEL_6_8: return "6_8";
        case D3D_SHADER_MODEL_6_9: return "6_9";
        default: break;
    }
    // 同 shaderModelName：0x6A 不在 10.0.26100 SDK 的枚举值域内，只能出 switch 单独判。
    if (model == kShaderModelCeiling) return "6_10";
    return "";
}

D3D_SHADER_MODEL effectiveShaderModel(const DeviceContext::Capabilities& caps) {
    // 注意：未查询成功时 caps.shaderModel 是 5_1，这里会老实返回 5_1，调用方据此
    // 退到旧路径，而不是误以为能用 6.x。
    return caps.shaderModel < kShaderModelCeiling ? caps.shaderModel : kShaderModelCeiling;
}

// ---------------------------------------------------------------------------
// PIX GPU 捕获层（mcdrSetPixCapture）
// ---------------------------------------------------------------------------
// PIX 的捕获层靠挂钩 D3D12/DXGI 工作，必须在**第一个 D3D12 设备创建之前**载入：晚一步，
// 先建好的对象就不在被捕获的范围内。所以 JVM 侧一读到配置就推过来（Illusion.kt →
// MCDR_ABI.dx12SetPixCapture），ensureDevice()/enumerateAdaptersJson() 里再各兜底一次。
// 本段自带路径工具：下面 DXC 段里的 fileExists/moduleDirectory/toUtf8 被
// #if MCDR_HAVE_DXCAPI 包着，没有 DXC 的构建里不存在，不能依赖。
namespace {

HMODULE gPixCapturer = nullptr;
int gPixStatus = 0;  // 1=已载入 0=未请求 -1=请求了但没载入（见 setPixCapture）
bool gPixRequested = false;

// 配置项 [debug] pixDirectory 的值（mcdrSetPixDirectory 推过来）。空 = 自动检测。
// 刻意不用环境变量：配置项才是入口，环境变量只是某个人的临时逃生口，进不了文档也就进不了
// 用户的脑子（见 AGENTS.md「开关一律走 NeoForge 配置」）。
std::wstring gPixDirectory;

bool pixIsFile(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool pixIsDirectory(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// MCDR.dll 自己所在的目录（按本 TU 里的数据地址反查模块，而不是取进程 exe）。
std::wstring pixSelfDirectory() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&gPixCapturer), &self)
        || self == nullptr) {
        return std::wstring();
    }
    wchar_t buffer[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, buffer, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return std::wstring();
    }
    const std::wstring path(buffer, n);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::string pixToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return std::string();
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

// %ProgramFiles%\Microsoft PIX\<版本>\ —— PIX 按版本号装子目录，取版本号最大的一个。
std::wstring pixInstallDirectory() {
    const wchar_t* programFiles = _wgetenv(L"ProgramFiles");
    const std::wstring root =
        std::wstring(programFiles && *programFiles ? programFiles : L"C:\\Program Files")
        + L"\\Microsoft PIX\\";
    std::wstring best;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return std::wstring();
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        const std::wstring ver(fd.cFileName);
        if (ver == L"." || ver == L".." || (!best.empty() && ver <= best)) {
            continue;
        }
        if (pixIsFile(root + ver + L"\\WinPixGpuCapturer.dll")) {
            best = ver;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best.empty() ? std::wstring() : root + best;
}

// 搜索顺序：[debug] pixDirectory（dll 本身或所在目录）→ MCDR.dll 同目录 → PIX 安装目录最新版。
std::wstring findPixCapturerDll() {
    if (!gPixDirectory.empty()) {
        if (pixIsFile(gPixDirectory)) {
            return gPixDirectory;
        }
        if (pixIsDirectory(gPixDirectory)) {
            const std::wstring inside = gPixDirectory + L"\\WinPixGpuCapturer.dll";
            if (pixIsFile(inside)) {
                return inside;
            }
        }
    }
    const std::wstring selfDir = pixSelfDirectory();
    if (!selfDir.empty()) {
        const std::wstring next = selfDir + L"\\WinPixGpuCapturer.dll";
        if (pixIsFile(next)) {
            return next;
        }
    }
    const std::wstring pixDir = pixInstallDirectory();
    if (!pixDir.empty()) {
        return pixDir + L"\\WinPixGpuCapturer.dll";
    }
    return std::wstring();
}

// 幂等：只尝试一次（成功或失败都记住）。1=已载入 0=未请求 -1=请求了但没载入。
int ensurePixCapturerLoaded() {
    static bool attempted = false;
    if (!gPixRequested) {
        // 还没人要求（配置是稍后才读到的）：保持可重试，别把这次“没请求”记成结论。
        return 0;
    }
    if (attempted) {
        return gPixStatus;
    }
    attempted = true;
    const std::wstring path = findPixCapturerDll();
    if (!path.empty()) {
        // LOAD_WITH_ALTERED_SEARCH_PATH：让捕获层从它自己所在目录解析依赖 —— 按绝对路径
        // 直接 LoadLibraryW 的话，PIX 目录并不在进程的搜索路径里。
        gPixCapturer = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (gPixCapturer) {
            dbgLogInfo("PIX GPU capture: capture layer loaded from %s", pixToUtf8(path).c_str());
            dbgLogInfo("PIX GPU capture: PIX has to be running to save a capture; expect a small "
                       "per-frame overhead while the layer is loaded");
            gPixStatus = 1;
            return 1;
        }
        dbgLogWarn("PIX GPU capture: LoadLibraryExW(%s) failed err=%lu", pixToUtf8(path).c_str(),
                   static_cast<unsigned long>(GetLastError()));
    }
    // 兜底：dll 已经在进程搜索路径里（用户自己部署，或 PIX 装过）。
    gPixCapturer = LoadLibraryW(L"WinPixGpuCapturer.dll");
    if (gPixCapturer) {
        dbgLogInfo("PIX GPU capture: capture layer loaded from the DLL search path");
        gPixStatus = 1;
        return 1;
    }
    const std::wstring pixDir = pixInstallDirectory();
    dbgLogError("PIX GPU capture: enabled but WinPixGpuCapturer.dll was not found — searched "
                "[debug] pixDirectory (%s), %s and %s; this run captures nothing",
                gPixDirectory.empty() ? "<empty>" : pixToUtf8(gPixDirectory).c_str(),
                pixToUtf8(pixSelfDirectory()).c_str(),
                pixDir.empty() ? "<no Microsoft PIX install under %ProgramFiles%>"
                               : pixToUtf8(pixDir).c_str());
    gPixStatus = -1;
    return -1;
}

}  // namespace

namespace {

// 定义在同文件后面的 timing 捕获段里（见「PIX 程序化 timing 捕获」）。
bool ensurePixTimingRuntime();

}  // namespace

// JVM 侧的配置入口（mcdrSetPixCapture）。
int setPixCapture(bool enabled) {
    gPixRequested = enabled;
    if (!enabled) {
        dbgLogInfo("PIX GPU capture: disabled by configuration");
        return 0;
    }
    const int status = ensurePixCapturerLoaded();
    // timing 捕获（startPixTimingCapture）要的两个库也在这里载入：PIX 的捕获层挂钩 D3D12/ETW
    // 是在 LoadLibrary 那一刻做的，设备与呈现链都建好之后再载入只能抓到一半的数据。所以
    // [debug] pixCapture / pixTimingCapture 任一为真时 JVM 侧都会推一次 true 到这里。
    ensurePixTimingRuntime();
    return status;
}

// ---------------------------------------------------------------------------
// PIX 程序化 timing 捕获（mcdrStartPixTimingCapture / mcdrStopPixTimingCapture）
// ---------------------------------------------------------------------------
// 为什么需要它：PIX 界面上的持续捕获（Counters recording）把数据留在 PIX 会话里，用户不点
// Save 就一个文件也拿不到；而 attach 到进程后抓的那一帧是 PIX 自己落在
// %TEMP%\<时间戳>.wpix 的。想让捕获自动写到 run/ 目录下，只能由本进程发起：
//     PIXBeginCapture(PIX_CAPTURE_TIMING, { file_name }) … PIXEndCapture(0)
// 这条路径照搬 wgpu-mc-neo 的实现（rust\wgpu-mc-jni\src\pix.rs），三个细节必须保留：
//   * 两个库：导出 PIXBeginCapture/PIXEndCapture 的是 WinPixEventRuntime*.dll，真正干活的是
//     WinPixTimingCapturer.dll。本机的 PIX 2603.25 只带 OneCore 版运行时（NuGet 包里那个
//     WinPixEventRuntime.dll 不存在），而 OneCore 版只导出 PIXBeginCapture2 —— 与
//     PIXBeginCapture 文档等价，两个符号都试。
//   * 线程：PIX 运行库载入时会初始化 COM，渲染线程的 apartment 早已定好，从那里调用恒返回
//     RPC_E_CHANGED_MODE (0x80010106)。所以 begin 在 CreateThread 出来的线程上做（主线程等
//     ≤10 s 取结果），end 同样另起线程但**不等** —— 它要把整个捕获写盘（开内存事件时从几百
//     MB 到几 GB）。
//   * ETW 权限：timing 捕获走 ETW 会话，创建会话的权限默认只给管理员。2026-10-21 本机实测：
//     当前用户**已经**在 `Performance Log Users`（S-1-5-32-559）里（`whoami /groups` 显示
//     Enabled group），PIX 仍返回 E_ACCESSDENIED；同一条探针在提权进程里是 S_OK 并真的写出了
//     .wpix。所以 PIX 文档那句「加进组即可」在本机不成立，**必须有提权 token**；而 `gradlew`
//     会复用没提权的 daemon，从管理员终端启动也不够，得先 `gradlew --stop`。
//     这里不做提权检查，失败就把 HRESULT 与这条提示一起打出来。
namespace {

// PIXCaptureParameters 的前 14 个字段，顺序与类型取自 WinPixEventRuntime 1.0.240308001 的
// Include/WinPixEventRuntime/pix3.h（该头文件不随任何 SDK 发布，所以这里手写）。尾部
// reserved[16] 不在那个头文件里：PIX 运行时与本结构体是分别版本化的，而 API 只收指针、
// 没法告诉它结构体多长，新版 PIX 读到我们不认识的字段时会读到栈上的垃圾；留 16 个 u64 的
// 零，让它读到的都是「关」。
struct PixCaptureParameters {
    const wchar_t* file_name;
    uint32_t maximum_tooling_memory_size_mb;
    uint32_t capture_storage;
    int32_t capture_gpu_timing;
    int32_t capture_callstacks;
    int32_t capture_cpu_samples;
    uint32_t cpu_samples_per_second;
    int32_t capture_file_io;
    int32_t capture_virtual_alloc_events;
    int32_t capture_heap_alloc_events;
    int32_t capture_xmem_events;
    int32_t capture_pix_mem_events;
    int32_t capture_page_fault_events;
    int32_t capture_video_frames;
    uint64_t reserved[16];
};

using PixBeginCaptureFn = HRESULT(__stdcall*)(unsigned long, const void*);
using PixEndCaptureFn = HRESULT(__stdcall*)(unsigned long);

constexpr unsigned long kPixCaptureTiming = 1u;       // PIX_CAPTURE_TIMING
constexpr uint32_t kPixCaptureStorageMemory = 0u;     // PIX_CAPTURE_STORAGE_MEMORY
constexpr uint32_t kPixCpuSamplesPerSecond = 4000u;   // 微软自家示例用的采样率
constexpr uint32_t kPixMaximumToolingMemoryMb = 4096u;
constexpr DWORD kPixBeginWaitMs = 10000;

HMODULE gPixTimingCapturer = nullptr;
HMODULE gPixTimingRuntime = nullptr;
PixBeginCaptureFn gPixTimingBegin = nullptr;
PixEndCaptureFn gPixTimingEnd = nullptr;
bool gPixTimingLoadTried = false;
bool gPixTimingCapturing = false;
// begin 的线程拿的是这个字符串的 c_str()，所以它必须活到线程结束之后 —— 放在文件级 static
// 里，一次运行只写一遍，不参与任何生命周期管理。
std::wstring gPixTimingFilePath;

std::wstring pixFromUtf8(const char* utf8) {
    if (utf8 == nullptr || utf8[0] == '\0') {
        return std::wstring();
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (need <= 1) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(need), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), need);
    if (written <= 1) {
        return std::wstring();
    }
    out.resize(static_cast<size_t>(written - 1));
    return out;
}

// 与 GPU 捕获层同一套搜索顺序：[debug] pixDirectory（dll 本身或所在目录）→ MCDR.dll 同目录
// → %ProgramFiles%\Microsoft PIX\ 下版本号最大的安装目录。
std::wstring findPixRuntimeFile(const wchar_t* fileName) {
    if (!gPixDirectory.empty()) {
        if (pixIsFile(gPixDirectory)) {
            // 配置项直接指到某个 dll 时，兄弟 dll 就在它旁边。
            const size_t slash = gPixDirectory.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                const std::wstring sibling = gPixDirectory.substr(0, slash + 1) + fileName;
                if (pixIsFile(sibling)) {
                    return sibling;
                }
            }
        } else if (pixIsDirectory(gPixDirectory)) {
            const std::wstring inside = gPixDirectory + L"\\" + fileName;
            if (pixIsFile(inside)) {
                return inside;
            }
        }
    }
    const std::wstring selfDir = pixSelfDirectory();
    if (!selfDir.empty()) {
        const std::wstring next = selfDir + L"\\" + fileName;
        if (pixIsFile(next)) {
            return next;
        }
    }
    const std::wstring pixDir = pixInstallDirectory();
    if (!pixDir.empty()) {
        const std::wstring candidate = pixDir + L"\\" + fileName;
        if (pixIsFile(candidate)) {
            return candidate;
        }
    }
    return std::wstring();
}

const char* pixHresultNote(HRESULT hr) {
    switch (static_cast<unsigned long>(hr)) {
        case 0x80070005ul: return ", E_ACCESSDENIED";
        case 0x8000000Aul: return ", E_PENDING";
        case 0x8007000Aul: return ", E_PENDING";
        case 0x80070057ul: return ", E_INVALIDARG";
        case 0x80004005ul: return ", E_FAIL";
        case 0x8000FFFFul: return ", E_UNEXPECTED";
        case 0x800401F0ul: return ", CO_E_NOTINITIALIZED";
        case 0x80010106ul: return ", RPC_E_CHANGED_MODE";
        default: return "";
    }
}

struct PixBeginJob {
    const wchar_t* file = nullptr;
    // E_PENDING 的初值让「等超时了」与「PIX 回了 S_OK」区分得开（S_OK == 0）。
    HRESULT result = static_cast<HRESULT>(0x8000000Au);
};

DWORD WINAPI pixBeginThread(void* raw) {
    auto* job = static_cast<PixBeginJob*>(raw);
    PixCaptureParameters params{};
    params.file_name = job->file;
    // 这两个字段在 Windows 上被 PIX 忽略，留着是因为结构体与 Xbox 版共用。
    params.maximum_tooling_memory_size_mb = kPixMaximumToolingMemoryMb;
    params.capture_storage = kPixCaptureStorageMemory;
    params.capture_gpu_timing = 1;
    // CPU 采样 + 调用栈是 timing 捕获的另一半：只有 GPU 时间只能说这一帧 5 ms，有了它们才看得
    // 出这一帧 CPU 上跑了什么、哪一次调用在等。
    params.capture_callstacks = 1;
    params.capture_cpu_samples = 1;
    params.cpu_samples_per_second = kPixCpuSamplesPerSecond;
    // 内存那一半：文件 IO、VirtualAlloc、HeapAlloc、PIX 自己的分配器、缺页事件，缺一个对应
    // 的表就是空的。
    params.capture_file_io = 1;
    params.capture_virtual_alloc_events = 1;
    params.capture_heap_alloc_events = 1;
    params.capture_xmem_events = 0;   // Xbox only
    params.capture_pix_mem_events = 1;
    params.capture_page_fault_events = 1;
    params.capture_video_frames = 0;  // Xbox only
    job->result = gPixTimingBegin(kPixCaptureTiming, &params);
    return 0;
}

DWORD WINAPI pixEndThread(void* raw) {
    (void)raw;
    // 不在这条线程上打日志：它与渲染线程并发，两边同时写同一个日志 sink 会把行撕开。
    (void)gPixTimingEnd(0u);
    return 0;
}

bool ensurePixTimingRuntime() {
    if (gPixTimingLoadTried) {
        return gPixTimingBegin != nullptr && gPixTimingEnd != nullptr;
    }
    gPixTimingLoadTried = true;
    const std::wstring capturerPath = findPixRuntimeFile(L"WinPixTimingCapturer.dll");
    if (!capturerPath.empty()) {
        gPixTimingCapturer =
            LoadLibraryExW(capturerPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (gPixTimingCapturer != nullptr) {
            dbgLogInfo("PIX timing capture: WinPixTimingCapturer.dll loaded from %s",
                       pixToUtf8(capturerPath).c_str());
        } else {
            dbgLogWarn("PIX timing capture: LoadLibraryExW(%s) failed err=%lu",
                       pixToUtf8(capturerPath).c_str(),
                       static_cast<unsigned long>(GetLastError()));
        }
    }
    // 运行时：NuGet 包里的 WinPixEventRuntime.dll 优先，其次 PIX 自带的 OneCore 版。
    const wchar_t* const runtimeNames[] = {L"WinPixEventRuntime.dll",
                                           L"WinPixEventRuntime_OneCore.dll"};
    for (const wchar_t* name : runtimeNames) {
        const std::wstring path = findPixRuntimeFile(name);
        if (path.empty()) {
            continue;
        }
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr) {
            continue;
        }
        gPixTimingBegin =
            reinterpret_cast<PixBeginCaptureFn>(GetProcAddress(module, "PIXBeginCapture"));
        if (gPixTimingBegin == nullptr) {
            gPixTimingBegin =
                reinterpret_cast<PixBeginCaptureFn>(GetProcAddress(module, "PIXBeginCapture2"));
        }
        gPixTimingEnd = reinterpret_cast<PixEndCaptureFn>(GetProcAddress(module, "PIXEndCapture"));
        if (gPixTimingBegin != nullptr && gPixTimingEnd != nullptr) {
            gPixTimingRuntime = module;
            dbgLogInfo("PIX timing capture: PIXBeginCapture/PIXEndCapture from %s",
                       pixToUtf8(path).c_str());
            break;
        }
        FreeLibrary(module);
        gPixTimingBegin = nullptr;
        gPixTimingEnd = nullptr;
    }
    if (gPixTimingBegin == nullptr || gPixTimingEnd == nullptr) {
        dbgLogError("PIX timing capture: no PIXBeginCapture/PIXEndCapture in this process — looked "
                    "for WinPixEventRuntime.dll and WinPixEventRuntime_OneCore.dll in "
                    "[debug] pixDirectory (%s), %s and %s",
                    gPixDirectory.empty() ? "<empty>" : pixToUtf8(gPixDirectory).c_str(),
                    pixToUtf8(pixSelfDirectory()).c_str(), pixToUtf8(pixInstallDirectory()).c_str());
        return false;
    }
    return true;
}

}  // namespace

// 配置项 [debug] pixDirectory 的入口（mcdrSetPixDirectory）：PIX 的 dll 本身，或它们所在的目录。
// 必须在 setPixCapture() 之前调用 —— 捕获层要在第一个 D3D12 设备创建之前载入，路径那时就得定。
// 1=已设置 0=已清空（回到自动检测）-1=路径无效（非 UTF-8）。
int setPixDirectory(const char* utf8Path) {
    if (utf8Path == nullptr || utf8Path[0] == '\0') {
        gPixDirectory.clear();
        dbgLogInfo("PIX: [debug] pixDirectory is empty; searching the default locations");
        return 0;
    }
    std::wstring wide = pixFromUtf8(utf8Path);
    if (wide.empty()) {
        dbgLogWarn("PIX: [debug] pixDirectory(%s) is not valid UTF-8; ignoring it", utf8Path);
        return -1;
    }
    gPixDirectory = std::move(wide);
    // 目录写错是最常见的失败：这里就把结论说清楚，别等到载入时才报「找不到 dll」。
    if (pixIsFile(gPixDirectory)) {
        dbgLogInfo("PIX: [debug] pixDirectory -> file %s", pixToUtf8(gPixDirectory).c_str());
    } else if (pixIsDirectory(gPixDirectory)) {
        dbgLogInfo("PIX: [debug] pixDirectory -> directory %s", pixToUtf8(gPixDirectory).c_str());
    } else {
        dbgLogWarn("PIX: [debug] pixDirectory(%s) does not exist; falling back to the default "
                   "locations", pixToUtf8(gPixDirectory).c_str());
        gPixDirectory.clear();
        return -1;
    }
    return 1;
}

// 开始一次 timing 捕获，写到 utf8Path 指定的 .wpix。1=已开始 0=已经在录
// 负值=失败的 HRESULT（原因见日志）。调 JVM 那边直接把它当 HRESULT 显示。
int startPixTimingCapture(const char* utf8Path) {
    if (gPixTimingCapturing) {
        dbgLogWarn("PIX timing capture: already recording; mcdrStartPixTimingCapture ignored");
        return 0;
    }
    if (!ensurePixTimingRuntime()) {
        return static_cast<int>(E_FAIL);
    }
    gPixTimingFilePath = pixFromUtf8(utf8Path);
    if (gPixTimingFilePath.empty()) {
        dbgLogError("PIX timing capture: empty or invalid output path");
        return static_cast<int>(E_INVALIDARG);
    }
    // 这个 job 故意不回收：等待超时后线程还在往里写，delete 掉就是 use-after-free。
    auto* const job = new PixBeginJob();
    job->file = gPixTimingFilePath.c_str();
    HANDLE thread = CreateThread(nullptr, 0, &pixBeginThread, job, 0, nullptr);
    if (thread == nullptr) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        dbgLogError("PIX timing capture: CreateThread failed (HRESULT 0x%08lX)",
                    static_cast<unsigned long>(hr));
        return static_cast<int>(hr);
    }
    const DWORD waited = WaitForSingleObject(thread, kPixBeginWaitMs);
    CloseHandle(thread);
    if (waited != WAIT_OBJECT_0) {
        dbgLogError("PIX timing capture: PIXBeginCapture did not answer within %lu ms (waited=%lu)",
                    static_cast<unsigned long>(kPixBeginWaitMs),
                    static_cast<unsigned long>(waited));
        return static_cast<int>(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    const HRESULT hr = job->result;
    if (FAILED(hr)) {
        if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
            // 上一轮被强杀时 PIX 的 ETW 会话还在（PixSysMonSession.* /
            // LongRunningTimingCaptureSession.*），PIX 就会以「已存在」拒绝新的一次捕获。
            dbgLogError("PIX timing capture: refused with ERROR_ALREADY_EXISTS for %s — a capture "
                        "session from an earlier run is still alive (that run was probably killed "
                        "mid-capture). List them with `logman query -ets` and stop the two PIX "
                        "sessions with `logman stop <name> -ets`, then press the key again",
                        pixToUtf8(gPixTimingFilePath).c_str());
            return static_cast<int>(hr);
        }
        dbgLogError("PIX timing capture: PIXBeginCapture refused (HRESULT 0x%08lX%s) for %s — a "
                    "timing capture records through ETW sessions, and creating them needs an "
                    "elevated (administrator) token on this machine: the current user is already in "
                    "`Performance Log Users` (S-1-5-32-559) and it was still refused (measured "
                    "2026-10-21, while the same call in an elevated process returned S_OK and wrote "
                    "the file). Note `gradlew` reuses a daemon started without elevation, so "
                    "launching gradlew from an elevated terminal is not enough on its own — run "
                    "`gradlew --stop` first",
                    static_cast<unsigned long>(hr), pixHresultNote(hr),
                    pixToUtf8(gPixTimingFilePath).c_str());
        return static_cast<int>(hr);
    }
    gPixTimingCapturing = true;
    dbgLogInfo("PIX timing capture: started (HRESULT 0x%08lX), writing %s",
               static_cast<unsigned long>(hr), pixToUtf8(gPixTimingFilePath).c_str());
    return 1;
}

// 停止当前捕获，PIX 在后台线程里把文件写完。1=已请求 0=没有在录
// 负值=失败的 HRESULT（原因见日志）。
int stopPixTimingCapture() {
    if (!gPixTimingCapturing) {
        return 0;
    }
    gPixTimingCapturing = false;
    if (gPixTimingEnd == nullptr) {
        return 0;
    }
    HANDLE thread = CreateThread(nullptr, 0, &pixEndThread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        dbgLogError("PIX timing capture: CreateThread failed (HRESULT 0x%08lX)",
                    static_cast<unsigned long>(hr));
        return static_cast<int>(hr);
    }
    // 不等：PIXEndCapture 会把整个捕获写盘（几百 MB 起），等它等于把这一帧卡死。
    CloseHandle(thread);
    dbgLogInfo("PIX timing capture: stopping, PIX is writing %s",
               pixToUtf8(gPixTimingFilePath).c_str());
    return 1;
}

// ---------------------------------------------------------------------------
// P67：DXC 探测（mesh shader 基础）
// ---------------------------------------------------------------------------
// 目的：把"本机能不能编译 mesh shader"变成一条确定的能力事实（日志 + JSON），
// 并给出 dxcompiler.dll 的实际来源，便于后续把 AS/MS 管线接上去时直接复用。
// 真正的 mesh 管线还需要 D3D12_PIPELINE_STATE_STREAM_DESC（AS/MS PSO）与 meshlet
// 数据组织，本步不做。
#if MCDR_HAVE_DXCAPI
namespace {

using PFN_DxcCreateInstance = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID);

// 最小可编译 mesh shader：只用于验证 DXC 能产出 ms_6_x 的 DXIL，不参与渲染。
constexpr char kMeshSmokeHlsl[] = R"hlsl(
struct MeshVertex { float4 position : SV_Position; };

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(out vertices MeshVertex verts[3], out indices uint3 tris[1]) {
    SetMeshOutputCounts(3, 1);
    verts[0].position = float4(0.0, 0.0, 0.0, 1.0);
    verts[1].position = float4(1.0, 0.0, 0.0, 1.0);
    verts[2].position = float4(0.0, 1.0, 0.0, 1.0);
    tris[0] = uint3(0, 1, 2);
}
)hlsl";

std::string hexHr(HRESULT hr) {
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

// 多行 DXC 诊断只留第一行（日志/JSON 里不宜铺开）。
std::string firstLine(const std::string& text) {
    const size_t nl = text.find_first_of("\r\n");
    std::string line = nl == std::string::npos ? text : text.substr(0, nl);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    if (line.size() > 220) {
        line.resize(220);
        line += "...";
    }
    return line;
}

// 2026-10-14 现代化改造：从被调方的 mcdr::Error 里取出纯文本 message。
// 调用方一律自己补 "函数名: " 前缀（与改造前 err 里直接写整句的习惯一致），
// 因此这里刻意不用 mcdr::describe()——那会带出被调方的 where 前缀。
std::string errorMessage(const mcdr::Error& e) { return e.message; }

bool fileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// MCDR.dll 自身所在目录（将来随模组分发 dxcompiler.dll/dxil.dll 时放这里）。
std::wstring moduleDirectory() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleDirectory), &self)) {
        return std::wstring();
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return std::wstring();
    }
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash);
}

// Windows SDK 里版本号最大的 bin\<ver>\x64（含 dxcompiler.dll/dxil.dll 的那一份）。
std::wstring sdkDxcDirectory() {
    const std::wstring root = L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\";
    std::wstring best;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return std::wstring();
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        const std::wstring ver(fd.cFileName);
        if (ver == L"." || ver == L".." || (!best.empty() && ver <= best)) {
            continue;
        }
        if (fileExists(root + ver + L"\\x64\\dxcompiler.dll")) {
            best = ver;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best.empty() ? std::wstring() : root + best + L"\\x64";
}

// 搜索顺序：MCDR_DXC_DIR 环境变量 → MCDR.dll 同目录 → SDK 最新版本。
std::wstring findDxcDirectory() {
    // 实机验收开关：`MCDR_DXC_DISABLE=1` 时**三站全不查**，直接返回空目录。
    //
    // 为什么需要它：`MCDR_DXC_DIR` 只是**追加一个候选**，不是权威覆盖 —— 即便把它指向空目录，
    // 模块目录与 Windows SDK 那两站照样能把 dxcompiler.dll 供上（实测：把 `run\dxrmc\` 里两个
    // DLL 改名后，客户端仍从 `C:\Program Files (x86)\Windows Kits\10\bin\…\dxcompiler.dll`
    // 拿到，`dxc=yes` 依旧）。系统目录不能改，所以"确实找不到 DXC"这种现场只能靠一个显式
    // 开关来验证 —— 2026-10-25 起找不到 DXC 不再回退 D3DCompile(5.1)，而是图形管线直接报错，
    // 这条开关的用途就变成"验证失败路径本身"。
    //
    // 只认完全等于 "1" 的值；未设置、空串、其它值一律照旧 —— 验收专用，不是产品行为开关。
    if (const wchar_t* off = _wgetenv(L"MCDR_DXC_DISABLE");
        off != nullptr && off[0] == L'1' && off[1] == L'\0') {
        return std::wstring();
    }
    std::vector<std::wstring> candidates;
    if (const wchar_t* env = _wgetenv(L"MCDR_DXC_DIR")) {
        if (env[0] != L'\0') {
            candidates.emplace_back(env);
        }
    }
    const std::wstring selfDir = moduleDirectory();
    if (!selfDir.empty()) {
        candidates.push_back(selfDir);
    }
    const std::wstring sdk = sdkDxcDirectory();
    if (!sdk.empty()) {
        candidates.push_back(sdk);
    }
    for (const std::wstring& dir : candidates) {
        if (fileExists(dir + L"\\dxcompiler.dll")) {
            return dir;
        }
    }
    return std::wstring();
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}



// 用 DXC 把 HLSL 编到 DXIL；成功时 outDxil 收到字节码，失败时返回 mcdr::Error（诊断首行）。
// P72：入口点与源码长度都变成显式参数（原实现写死 entry="main" 且要求 NUL 结尾），
// 这样 as_6_x / ms_6_x / ps_6_x 都可以走同一条编译路径。
mcdr::Result<void> compileWithDxc(IDxcUtils* utils, IDxcCompiler3* compiler, const char* source,
                                  size_t sourceLen, const std::string& entry, const std::string& target,
                                  std::vector<uint8_t>& outDxil) {
    ComPtr<IDxcBlobEncoding> srcBlob;
    if (FAILED(utils->CreateBlobFromPinned(source, static_cast<UINT32>(sourceLen), DXC_CP_UTF8,
                                           &srcBlob))) {
        return mcdr::err("compileWithDxc", "CreateBlobFromPinned failed");
    }
    DxcBuffer buffer{};
    buffer.Ptr = srcBlob->GetBufferPointer();
    buffer.Size = srcBlob->GetBufferSize();
    buffer.Encoding = DXC_CP_UTF8;

    const std::wstring targetW = toWide(target);
    const std::wstring entryW = toWide(entry);
    const wchar_t* args[] = {L"-T",  targetW.c_str(), L"-E", entryW.c_str(),
                             L"-HV", L"2018",         L"-O3"};
    ComPtr<IDxcResult> result;
    const HRESULT hr =
        compiler->Compile(&buffer, args, static_cast<UINT32>(std::size(args)), nullptr,
                          IID_PPV_ARGS(&result));
    if (FAILED(hr) || !result) {
        return mcdr::err("compileWithDxc", "IDxcCompiler3::Compile entry failed hr=" + hexHr(hr));
    }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (FAILED(status)) {
        ComPtr<IDxcBlobEncoding> errBlob;
        std::string dxcError;
        if (SUCCEEDED(result->GetErrorBuffer(&errBlob)) && errBlob) {
            dxcError.assign(static_cast<const char*>(errBlob->GetBufferPointer()),
                            static_cast<size_t>(errBlob->GetBufferSize()));
        } else {
            dxcError = "DXC compile failed hr=" + hexHr(status);
        }
        return mcdr::err("compileWithDxc", std::move(dxcError));
    }
    ComPtr<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object) {
        return mcdr::err("compileWithDxc", "DXC produced no DXIL object");
    }
    const auto* bytes = static_cast<const uint8_t*>(object->GetBufferPointer());
    outDxil.assign(bytes, bytes + object->GetBufferSize());
    return {};
}

// ---------------------------------------------------------------------------
// P99（DXIL 磁盘缓存）：把 DXC 的编译结果缓存到磁盘。
// 动机（用户报告「启动加载界面冻结 / 加载世界弹出瞬间帧时间骤增」）：DXC 每次编译
// 实测 0.4-3 s 且全部落在主线程；一个会话要编译几十个着色器（主图形管线 + blit +
// 速度/后处理），启动阶段累计停顿十几秒（P98 日志：519/697/553/423/726/496/2020/
// 3079/2938 ms 等 gap）。同一份 HLSL + 同一 profile + 同一 dxcompiler.dll 产出的 DXIL
// 是确定的，因此第二次启动可直接读回缓存、跳过 DXC。
// 键 = FNV-1a 64(dxc 版本标签 + profile + entry + HLSL 源码)；dxcompiler.dll 被替换
// （大小/时间戳变化）时键自动变化，旧缓存自然失效。
// 开关：MCDR_DXIL_CACHE=0 关闭；MCDR_DXIL_CACHE_DIR=<dir> 指定目录（默认 MCDR.dll
// 同目录下的 MCDR_dxil_cache）。
std::wstring dxilCacheDirectory() {
    if (const char* env = std::getenv("MCDR_DXIL_CACHE_DIR")) {
        if (env[0] != '\0') return toWide(env);
    }
    const std::wstring selfDir = moduleDirectory();
    if (selfDir.empty()) return {};
    return selfDir + L"\\MCDR_dxil_cache";
}

uint64_t fnv1a64(const void* data, size_t len, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

std::wstring dxilCachePath(const std::wstring& dir, uint64_t key) {
    return dir + L"\\" + std::to_wstring(key) + L".dxil";
}

bool dxilCacheLoad(const std::wstring& dir, uint64_t key, std::vector<uint8_t>& out) {
    if (dir.empty()) return false;
    const HANDLE f = CreateFileW(dxilCachePath(dir, key).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > (64 << 20)) {
        CloseHandle(f);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(f);
    if (!ok || static_cast<size_t>(read) != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

void dxilCacheStore(const std::wstring& dir, uint64_t key, const std::vector<uint8_t>& dxil) {
    if (dir.empty() || dxil.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);  // 已存在时返回 ERROR_ALREADY_EXISTS，忽略
    const HANDLE f = CreateFileW(dxilCachePath(dir, key).c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, dxil.data(), static_cast<DWORD>(dxil.size()), &written, nullptr);
    CloseHandle(f);
}

}  // namespace
#endif  // MCDR_HAVE_DXCAPI

// ---------------------------------------------------------------------------
// P108：DLSS 接线的辅助函数。
//
// 定义在这里（而不是与 applyDlssToTexture 放在一起）的唯一原因：notifyDlssDeviceReady()
// 要在 ensureDevice()（本文件约 1520 行）里调用，而那一处远早于 P104 段。
// 它们放在一个匿名 namespace 里，因此不进入 d3d12_mc 的导出面。
// ---------------------------------------------------------------------------
namespace {

// 「可用性一次性行」的闸门。
bool gDlssAvailabilityLogged = false;
// 日志注入是不是已经做过（设备创建时做一次）。
bool gDlssWired = false;

// B4：DLSS 私有纹理的创建/销毁入口（参数见 mcdr_dlss_impl.h 的 DlssTextureDesc）。定义在
// 本文件靠后的 DLSS 接线区（那里紧挨着读回路径），这里先声明是因为 wireDlssImpl() 现在就要
// 把它们的地址注入实现单元。
int32_t createDlssTexture(CommandContext* ctx, const DlssTextureDesc* desc, void** outObject,
                          void** outResource);
void destroyDlssTexture(CommandContext* ctx, void* object);

void wireDlssImpl() {
    if (gDlssWired) {
        return;
    }
    gDlssWired = true;
    // 只能在模块实现单元里取这三个函数的地址：普通 TU 里的同名声明是另一个修饰名，
    // 直接跨界调用会链不上，所以用函数指针表注入。
    mcdr_dlss_install_log(reinterpret_cast<void*>(&d3d12_mc::dbgLogInfo),
                          reinterpret_cast<void*>(&d3d12_mc::dbgLogWarn),
                          reinterpret_cast<void*>(&d3d12_mc::dbgLogError));
    // B4：私有纹理的创建/销毁也走注入 —— 实现单元不再自己 `CreateCommittedResource`，
    // 于是它的资源从出生起就在本模块的跟踪表里（详见 mcdr_dlss_impl.h 那一段）。
    mcdr_dlss_install_texture_ops(reinterpret_cast<void*>(&d3d12_mc::createDlssTexture),
                                  reinterpret_cast<void*>(&d3d12_mc::destroyDlssTexture));
}

// 设备就绪后调用一次：注入日志、把厂商运行库目录登记进 DLL 搜索路径（见下）、把探测到的
// nvngx_dlss.dll 路径告诉 DLSS 实现（Init 失败时那是最有用的一条诊断），并打一行可用性结论。
void notifyDlssDeviceReady() {
    wireDlssImpl();
    // P108 阻塞 B（防御性的一半）：MCDR.dll 被 JVM 解到 <user.dir>\dxrmc\，而厂商运行库就在
    // 它旁边，所以「本模块所在目录」是最可能的厂商目录 —— **但必须先确认那里真的有
    // nvngx_dlss.dll**，而且必须问对模块：GetModuleFileNameA(nullptr) 返回的是**宿主进程的
    // exe**（开发跑时是 JDK 的 java.exe，于是第一版把 C:\Program Files\GraalVM\JDKs\25.1\bin
    // 当成了厂商目录）。用 GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 取本模块自己的路径。
    // JVM 侧也可以显式调 mcdr_dx12SetDlssVendorDir（同一条路，幂等）。
    {
        HMODULE self = nullptr;
        wchar_t ownPath[MAX_PATH] = {};
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&notifyDlssDeviceReady), &self) != FALSE &&
            self != nullptr &&
            ::GetModuleFileNameW(self, ownPath, static_cast<DWORD>(std::size(ownPath))) > 0) {
            std::wstring dir(ownPath);
            const std::wstring::size_type slash = dir.find_last_of(L"\\/");
            dir.resize(slash == std::wstring::npos ? 0 : slash);
            if (!dir.empty()) {
                wchar_t probe[MAX_PATH] = {};
                std::swprintf(probe, std::size(probe), L"%s\\nvngx_dlss.dll", dir.c_str());
                if (::GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) {
                    // Convert to UTF-8 for the ABI (the native side keeps everything UTF-8).
                    char utf8[MAX_PATH * 2] = {};
                    const int need = ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(),
                                                          static_cast<int>(dir.size()), utf8,
                                                          static_cast<int>(sizeof(utf8)) - 1, nullptr,
                                                          nullptr);
                    if (need > 0) {
                        utf8[need] = '\0';
                        mcdr_dlss_register_vendor_dir(utf8, need + 1);
                    }
                } else {
                    dbgLogInfo("P108: this module's directory has no nvngx_dlss.dll; not registering it "
                               "as the DLSS vendor directory (the JVM can pass the real one via "
                               "mcdr_dx12SetDlssVendorDir)");
                }
            }
        }
    }
    if (char note[512] = {}; true) {
        // P108：NGX 的 Init 结果现在就打出来。设备就绪这一刻命令列表还没开，所以
        // CreateFeature 还做不了；但 Init 是纯设备侧调用，早点暴露它的返回码能省掉一整轮
        // 实机排查（第一版只在 applyDlssToTexture 里懒初始化，失败原因要到第一帧才可见）。
        const int32_t canRun = mcdr_dlss_can_run(reinterpret_cast<void*>(gCtx.device.Get()));
        char runtimePath[512] = {};
        dbgLogInfo("P108: DLSS NGX init on device-ready: canRun=%d runtime=\"%s\"", canRun,
                   mcdr_dlss_runtime_path());
        mcdr_dlss_query(note, sizeof(note), nullptr, 0, runtimePath, sizeof(runtimePath), nullptr);
        dbgLogInfo("P108: DLSS status after init: %s", note);
    }
    if (gCtx.adapter && gCtx.rtx.ngxRuntime && !gCtx.rtx.ngxRuntimePath.empty()) {
        mcdr_dlss_register_runtime_path(gCtx.rtx.ngxRuntimePath.c_str(),
                                        static_cast<int32_t>(gCtx.rtx.ngxRuntimePath.size() + 1));
    }
    if (gDlssAvailabilityLogged) {
        return;
    }
    gDlssAvailabilityLogged = true;
    const bool adapterOk = gCtx.rtx.dlssSuperResolution;
    dbgLogInfo("P108: DLSS availability: ngxRuntime=%s%s%s%s dlssSupported=%s (why: %s) runtime=%s",
               gCtx.rtx.ngxRuntime ? "yes" : "no",
               (gCtx.rtx.ngxRuntime && !gCtx.rtx.ngxRuntimePath.empty()) ? " \"" : "",
               (gCtx.rtx.ngxRuntime && !gCtx.rtx.ngxRuntimePath.empty()) ? gCtx.rtx.ngxRuntimePath.c_str()
                                                                         : "",
               (gCtx.rtx.ngxRuntime && !gCtx.rtx.ngxRuntimePath.empty()) ? "\"" : "",
               adapterOk ? "yes" : "no",
               gCtx.rtx.dlssQueried ? (adapterOk ? "-" : gCtx.rtx.dlssSuperResolutionWhy.c_str())
                                    : gCtx.rtx.dlssQueryNote.c_str(),
               gCtx.rtx.ngxNote.empty() ? "-" : gCtx.rtx.ngxNote.c_str());
}

}  // namespace

void probeDxc(DeviceContext::Capabilities& caps) {
#if !MCDR_HAVE_DXCAPI
    caps.dxcNote = "built without <dxcapi.h>";
    (void)caps;
#else
    const std::wstring dir = findDxcDirectory();
    if (dir.empty()) {
        // 保留既有文案（其它调用方/工具会 grep 它），只在验收开关生效时额外说明**原因**：
        // 正常机器上"找不到 dxcompiler.dll"与"我们主动短路了搜索链"是两件事，日志必须能区分，
        // 否则这条回退路径的验收结论就成了空话。
        caps.dxcNote = "dxcompiler.dll not found (MCDR_DXC_DIR / MCDR.dll dir / Windows SDK)";
        if (const wchar_t* off = _wgetenv(L"MCDR_DXC_DISABLE");
            off != nullptr && off[0] == L'1' && off[1] == L'\0') {
            dbgLogInfo("P66: DXC discovery was short-circuited by MCDR_DXC_DISABLE=1 — this is the "
                       "acceptance switch for the \"no DXC\" path (graphics pipelines now fail "
                       "instead of falling back to D3DCompile SM5.1), not a missing-SDK condition");
        }
        return;
    }
    const std::wstring dllPath = dir + L"\\dxcompiler.dll";
    caps.dxcPath = toUtf8(dllPath);

    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (mod == nullptr) {
        caps.dxcNote = "LoadLibraryW(dxcompiler.dll) failed err=" + std::to_string(GetLastError());
        return;
    }
    const auto createInstance =
        reinterpret_cast<PFN_DxcCreateInstance>(GetProcAddress(mod, "DxcCreateInstance"));
    if (createInstance == nullptr) {
        caps.dxcNote = "DxcCreateInstance export missing";
        return;
    }
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) || !utils
        || FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || !compiler) {
        caps.dxcNote = "DxcCreateInstance failed (dxil.dll missing next to dxcompiler.dll?)";
        return;
    }
    caps.dxcAvailable = true;

    // 冒烟编译目标：**固定 ms_6_9**（2026-10-25 起图形管线硬要求 SM 6.9，没有 5.1 兜底）。
    // 这里只回答"DXC 能不能产出 6_9 的 DXIL"，与设备能力无关 —— 设备那一半由
    // graphicsShaderModelProfile() 查 caps.shaderModel 单独把关。
    // 实测 DXC 1.9.2602.17 接受 vs/ps/as_6_9；1.8 及更早对 6_9 报 invalid profile
    // （6_10 至今没有 DXC 认，所以 6_9 就是天花板）。
    const std::string profile = "ms_6_9";
    caps.dxcProfile = profile;

    std::vector<uint8_t> dxil;
    auto smoke = compileWithDxc(utils.Get(), compiler.Get(), kMeshSmokeHlsl, std::strlen(kMeshSmokeHlsl),
                                "main", profile, dxil);
    if (smoke) {
        caps.meshShaderCompileOk = true;
        caps.dxcNote = "mesh smoke compile OK (" + profile + ", "
                       + std::to_string(dxil.size()) + " bytes DXIL)";
    } else {
        caps.dxcNote = "mesh smoke compile failed: " + firstLine(smoke.error().message);
    }
#endif
}

// ---------------------------------------------------------------------------
// P72：可复用的 DXC 编译入口（mesh shader 基础）
// ---------------------------------------------------------------------------
// probeDxc 只回答"这台机器能不能编译 mesh shader"，随后就把 IDxcUtils/IDxcCompiler3
// 释放掉了；mesh 管线真正需要的是一个运行期可反复调用的编译入口：Java 侧给 HLSL 源码
// + 入口点 + profile（as_6_6 / ms_6_6 / ps_6_0 ...），拿回 DXIL 字节码交给
// createMeshPipeline。这里把 dxcompiler.dll 与两个接口缓存成进程级单例，只加载一次。
mcdr::Result<void> compileHlslToDxil(const std::string& source, const std::string& entry,
                                    const std::string& profile, std::vector<uint8_t>& outDxil) {
#if !MCDR_HAVE_DXCAPI
    (void)source;
    (void)entry;
    (void)profile;
    (void)outDxil;
    return mcdr::err("compileHlslToDxil", "compileHlslToDxil: built without <dxcapi.h>");
#else
    struct CachedDxc {
        HMODULE module = nullptr;
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        bool tried = false;
        std::string note;
        // P99：dxcompiler.dll 的版本标签（大小 + 最后写入时间），参与 DXIL 缓存键。
        std::string versionTag;
    };
    // 进程级单例；C++11 起函数内 static 初始化线程安全。
    // 故意 new 出来且永不 delete（等价于"进程级泄漏"）：**绝不能让这个单例在进程退出时被析构**。
    // 析构里对 DXC 的 COM 对象做 Release，会在 CRT 退出阶段触发 ucrtbase 的 fail-fast
    // （事件日志：java.exe / 出错模块 ucrtbase.dll / 异常代码 0xc0000409 / BEX64），
    // 于是"渲染完全正常、destroyDevice: done"之后整个进程仍以 -1073740791 退出，
    // Gradle 报 BUILD FAILED。与 dxcore.dll 故意不 FreeLibrary（见下方 queryDxCoreInfo）同理。
    // 取证：MCDR\src\core\d3d12_device.cpp 本函数 + _port\gsm68_*_info.err.log + 事件日志 Id=1000。
    static CachedDxc& cached = *new CachedDxc();
    if (!cached.tried) {
        cached.tried = true;
        const std::wstring dir = findDxcDirectory();
        if (dir.empty()) {
            cached.note = "dxcompiler.dll not found (MCDR_DXC_DIR / MCDR.dll dir / Windows SDK)";
            // 同 capa 探测那一处：验收开关生效时说明**原因**，否则"主动短路"与"真缺 SDK"
            // 在日志里长得一样，回退路径就无法取证。
            if (const wchar_t* off = _wgetenv(L"MCDR_DXC_DISABLE");
                off != nullptr && off[0] == L'1' && off[1] == L'\0') {
                dbgLogInfo("P66: DXC discovery was short-circuited by MCDR_DXC_DISABLE=1 (acceptance "
                           "switch for the no-DXC path; graphics pipelines now fail instead of "
                           "falling back to D3DCompile SM5.1)");
            }
        } else {
            const std::wstring dllPath = dir + L"\\dxcompiler.dll";
            // P99：记录 dxcompiler.dll 的大小+写入时间，作为 DXIL 缓存键的一部分。
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(dllPath.c_str(), GetFileExInfoStandard, &fad)) {
                cached.versionTag = std::to_string(fad.nFileSizeHigh) + ":" +
                                    std::to_string(fad.nFileSizeLow) + ":" +
                                    std::to_string(fad.ftLastWriteTime.dwHighDateTime) + ":" +
                                    std::to_string(fad.ftLastWriteTime.dwLowDateTime);
            }
            cached.module = LoadLibraryW(dllPath.c_str());
            if (cached.module == nullptr) {
                cached.note =
                    "LoadLibraryW(dxcompiler.dll) failed err=" + std::to_string(GetLastError());
            } else {
                const auto createInstance = reinterpret_cast<PFN_DxcCreateInstance>(
                    GetProcAddress(cached.module, "DxcCreateInstance"));
                if (createInstance == nullptr) {
                    cached.note = "DxcCreateInstance export missing";
                } else if (FAILED(createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&cached.utils)))
                           || !cached.utils
                           || FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&cached.compiler)))
                           || !cached.compiler) {
                    cached.note = "DxcCreateInstance failed (dxil.dll missing next to dxcompiler.dll?)";
                } else {
                    cached.note = "dxcompiler.dll loaded from " + toUtf8(dllPath);
                }
            }
        }
        dbgLogInfo("compileHlslToDxil: DXC init — %s", cached.note.c_str());
    }
    if (!cached.utils || !cached.compiler) {
        return mcdr::err("compileHlslToDxil", "compileHlslToDxil: " + cached.note);
    }
    if (source.empty()) {
        return mcdr::err("compileHlslToDxil", "compileHlslToDxil: empty HLSL source");
    }
    if (profile.empty()) {
        return mcdr::err("compileHlslToDxil", "compileHlslToDxil: empty profile");
    }
    const std::string entryName = entry.empty() ? std::string("main") : entry;
    // P99：先查 DXIL 磁盘缓存（键含 dxcompiler.dll 版本 + profile + entry + 源码）。
    const char* cacheEnv = std::getenv("MCDR_DXIL_CACHE");
    const bool cacheEnabled = !(cacheEnv != nullptr && cacheEnv[0] == '0');
    const std::wstring cacheDir = cacheEnabled ? dxilCacheDirectory() : std::wstring();
    uint64_t cacheKey = 0;
    if (cacheEnabled) {
        cacheKey = fnv1a64(cached.versionTag.data(), cached.versionTag.size());
        cacheKey = fnv1a64(profile.data(), profile.size(), cacheKey);
        cacheKey = fnv1a64(entryName.data(), entryName.size(), cacheKey);
        cacheKey = fnv1a64(source.data(), source.size(), cacheKey);
        if (dxilCacheLoad(cacheDir, cacheKey, outDxil)) {
            dbgLogInfo("compileHlslToDxil: %s entry=%s -> %zu bytes DXIL (disk cache hit)",
                       profile.c_str(), entryName.c_str(), outDxil.size());
            return {};
        }
    }
    const double p98CompileT0 = nowMs();
    {
        auto compiled = compileWithDxc(cached.utils.Get(), cached.compiler.Get(), source.data(),
                                       source.size(), entryName, profile, outDxil);
        if (!compiled) return std::unexpected<mcdr::Error>(std::move(compiled.error()));
    }
    // P98：把编译耗时打出来——这是启动/入场冻结的主要来源之一（缓存未命中时 0.4-3 s）。
    dbgLogInfo("compileHlslToDxil: %s entry=%s -> %zu bytes DXIL (took %.0f ms)", profile.c_str(),
               entryName.c_str(), outDxil.size(), nowMs() - p98CompileT0);
    if (cacheEnabled) {
        dxilCacheStore(cacheDir, cacheKey, outDxil);
    }
    return {};
#endif
}

void queryDeviceCapabilities(ID3D12Device* device, DeviceContext::Capabilities& caps) {
    caps = DeviceContext::Capabilities{};
    if (!device) {
        return;
    }
    // 设备已经建出来了就算"查过"；单项 CheckFeatureSupport 失败就保持默认值（0 / false），
    // 调用方据此做保守门控。
    caps.queried = true;
    caps.featureLevel = gCtx.featureLevel;

    // 这个结构是 in/out：HighestShaderModel 必须先由调用方填一个"上限"，驱动才会回填
    // 它支持的最高级别。但直接填 6.10(0x6a) 并不安全——较老的 d3d12 runtime 不认识
    // 这个枚举值，CheckFeatureSupport 会直接失败（E_INVALIDARG），能力值便停在默认的
    // 5.1（实测：12_2 设备曾被误报成 SM=5.1(0x51)）。因此从高到低逐档探测：第一个
    // 成功的档位返回的就是该设备真实支持的最高级别。
    static constexpr D3D_SHADER_MODEL kShaderModelProbes[] = {
        static_cast<D3D_SHADER_MODEL>(0x6a),  // 6.10：SDK 头文件还未定义，驱动可能先支持
        D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
        D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
        D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1,
    };
    for (D3D_SHADER_MODEL probe : kShaderModelProbes) {
        D3D12_FEATURE_DATA_SHADER_MODEL query{};
        query.HighestShaderModel = probe;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &query, sizeof(query)))) {
            caps.shaderModel = query.HighestShaderModel;
            break;
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7,
                                              sizeof(options7)))) {
        caps.meshShaderTier = options7.MeshShaderTier;
        caps.samplerFeedbackTier = options7.SamplerFeedbackTier;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                              sizeof(options5)))) {
        caps.raytracingTier = options5.RaytracingTier;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6,
                                              sizeof(options6)))) {
        caps.shadingRateTier = options6.VariableShadingRateTier;
        // P97：VRS 的另两个事实一起取出来 —— tier 2 才有着色率图像（屏幕空间 VRS），tile
        // 大小决定那张图像的分辨率，additional 表示 1x4/4x1 这类附加速率可用。
        caps.shadingRateImageTileSize = options6.ShadingRateImageTileSize;
        caps.additionalShadingRates = options6.AdditionalShadingRatesSupported != FALSE;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                              sizeof(options)))) {
        caps.resourceBindingTier = options.ResourceBindingTier;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12,
                                              sizeof(options12)))) {
        caps.enhancedBarriers = options12.EnhancedBarriersSupported != FALSE;
    }

    // P67：DXC/mesh shader 冒烟自检（不依赖 ID3D12Device，但如果这一层不成立，
    // 后面接 AS/MS 管线就无从谈起，所以放在同一处一次性报出来）。
    probeDxc(caps);
}

// SM 值写成十进制（D3D_SHADER_MODEL_6_8 == 0x68 == 104），tier 枚举按 d3d12.h 原值。
std::string capabilitiesText(const DeviceContext::Capabilities& caps) {
    char buf[512] = {};
    std::snprintf(buf, sizeof(buf),
                  "FL=%s SM=%s(0x%02X)/eff=%s mesh=%d samplerFeedback=%d rt=%d rtMode=%d"
                  "vrs=%d(tile=%u,extra=%d) binding=%d enhancedBarriers=%d",
                  featureLevelName(caps.featureLevel), shaderModelName(caps.shaderModel),
                  static_cast<unsigned>(caps.shaderModel),
                  shaderModelProfileSuffix(effectiveShaderModel(caps)),
                  static_cast<int>(caps.meshShaderTier), static_cast<int>(caps.samplerFeedbackTier),
                  static_cast<int>(caps.raytracingTier), mcdr_rt_state_mode(),
                  static_cast<int>(caps.shadingRateTier),
                  caps.shadingRateImageTileSize, caps.additionalShadingRates ? 1 : 0,
                  static_cast<int>(caps.resourceBindingTier), caps.enhancedBarriers ? 1 : 0);
    std::string text(buf);
    // P67：DXC（mesh shader 编译器）状态一并带出；完整路径与失败原因看 JSON。
    text += " dxc=";
    text += caps.dxcAvailable ? "yes" : "no";
    if (!caps.dxcProfile.empty()) {
        text += "(" + caps.dxcProfile + ")";
    }
    text += " meshSmoke=";
    text += caps.meshShaderCompileOk ? "ok" : "fail";
    // 可用性探测（DLSS/NGX + NRD）：与 caps 分开统计，失败原因看 JSON。
    text += gCtx.rtx.summary();
    return text;
}

// 把 nlohmann 对象序列化成「字段串」：`{"a":1,"b":2}` → `"a":1,"b":2`。
// 少数导出（能力字段、RTX 探测字段）要把自己的键并进调用方的大对象里，而不是再多一层
// 嵌套对象 —— JVM 侧的平坦扫描器读不了嵌套。空对象返回空串，调用方据此跳过分隔逗号。
static std::string jsonFieldsOf(const nlohmann::json& obj) {
    std::string text = obj.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (text.size() < 2 || text.front() != '{' || text.back() != '}') {
        return std::string();
    }
    text.erase(text.size() - 1);
    text.erase(0, 1);
    return text;
}

// 把一段字段串并到已经 dump 好的对象体末尾（`{...}` 的 `}` 之前）。字段串为空时什么都不做，
// 避免拼出 `{,}` 这种非法 JSON。
static void appendJsonFields(std::string& body, const std::string& fields) {
    if (fields.empty() || body.size() < 2 || body.back() != '}') {
        return;
    }
    body.insert(body.size() - 1, "," + fields);
}

// 注意：不含 feature_level / feature_level_value —— 适配器信息 JSON 里已经有这两个键了
// （见 adapterInfoJson），同名键会让解析方拿到后者，所以这里只用 caps_* 语义的新键。
std::string capabilitiesJson(const DeviceContext::Capabilities& caps) {
    nlohmann::json j;
    j["shader_model"] = shaderModelName(caps.shaderModel);
    j["shader_model_value"] = static_cast<int>(caps.shaderModel);
    j["shader_model_effective"] = shaderModelProfileSuffix(effectiveShaderModel(caps));
    j["mesh_shader_tier"] = static_cast<int>(caps.meshShaderTier);
    j["sampler_feedback_tier"] = static_cast<int>(caps.samplerFeedbackTier);
    j["raytracing_tier"] = static_cast<int>(caps.raytracingTier);
    j["shading_rate_tier"] = static_cast<int>(caps.shadingRateTier);
    j["shading_rate_image_tile_size"] = caps.shadingRateImageTileSize;
    j["additional_shading_rates"] = caps.additionalShadingRates;
    j["resource_binding_tier"] = static_cast<int>(caps.resourceBindingTier);
    j["enhanced_barriers"] = caps.enhancedBarriers;
    j["caps_queried"] = caps.queried;
    // P67：DXC/mesh shader 自检结果（dxc_note 里带失败原因或冒烟编译产物大小）。
    j["dxc_available"] = caps.dxcAvailable;
    j["dxc_mesh_shader_compile_ok"] = caps.meshShaderCompileOk;
    j["dxc_profile"] = caps.dxcProfile;
    j["dxc_path"] = caps.dxcPath;
    j["dxc_note"] = caps.dxcNote;
    // 可用性探测（DLSS/NGX + NRD + XeSS + FFX）。与 caps 分开：caps 是 D3D12 基线，
    // rtx 是「第三方 SDK 在这台机器上能不能用」。它同样是一段字段串，接在后面即可。
    std::string fields = jsonFieldsOf(j);
    const std::string rtx = gCtx.rtx.jsonFragment();
    if (!rtx.empty()) {
        fields += ",";
        fields += rtx;
    }
    return fields;
}

// ---------------------------------------------------------------------------
// P110：Agility SDK（D3D12Core.dll 1.619）opt-in —— 2026-10-25 起**默认开启**
// ---------------------------------------------------------------------------
// 机制（2026-10 核实，出处见 known_bugs.md §D.12）：
//   * 官方推荐的两符号（D3D12SDKVersion / D3D12SDKPath）只对**进程主 exe** 生效：
//     d3d12.dll 用 GetModuleHandle(NULL) 取 exe 模块再 GetProcAddress，被注入的
//     MCDR.dll 导出了也不会被看到（实测：注入 DLL 里导出 619，设备仍是 616）。
//   * 改不了 javaw.exe 的替代路径：D3D12GetInterface(CLSID_D3D12SDKConfiguration)
//     → ID3D12SDKConfiguration1::CreateDeviceFactory(版本, 绝对路径) 就地加载
//     side-by-side D3D12Core.dll（不需要开发者模式、路径不必相对 exe）→
//     ID3D12DeviceFactory::ApplyToGlobalState() 把全局 core 换成它，之后所有
//     d3d12.dll 导出（D3D12CreateDevice 等）都走新 core。
//   * 生效判据不是函数返回 S_OK，而是设备上 QI ID3D12DeviceConfiguration 的
//     GetDesc().SDKVersion：616 = 系统 inbox，619 = 我们带的那份。
// 约束：D3D12Core.dll 必须与 MCDR.dll 同目录（Illusion\run\dxrmc\）；1.619 的
// D3D12SDKLayers.dll 不在 vcpkg 包内，所以 agility 生效时调试层必须关掉（版本不匹配
// 会让 D3D12CreateDevice 直接失败），见 ensureDevice 里的 debugEnabled 分支。
constexpr UINT kAgilitySdkVersion = 619;

struct AgilityAttempt {
    bool requested = false;
    bool applied = false;
    HRESULT factoryHr = E_FAIL;
    HRESULT applyHr = E_FAIL;
    std::wstring dir;  // D3D12Core.dll 所在目录（带结尾反斜杠）
};

AgilityAttempt gAgility;

std::wstring ownModuleDir() {    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ownModuleDir), &mod)
        || !mod)
        return {};
    wchar_t path[1024] = {};
    const DWORD n = GetModuleFileNameW(mod, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (n == 0 || n >= sizeof(path) / sizeof(path[0])) return {};
    const std::wstring full(path, n);
    const size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash + 1);
}

// 必须在任何 D3D12 设备创建之前调用（ApplyToGlobalState 会改进程全局 core）。
// ensureDevice() 在设备就绪后仍会被逐帧调用，所以这里必须幂等：只在第一次进入时
// 真正执行（否则每次调用都会重新 CreateDeviceFactory + ApplyToGlobalState，
// 实测刷出 32k 行日志，也让全局 core 被反复"改配置"）。
// 2026-10-25：**不再由 MCDR_AGILITY 环境变量门控**。图形管线硬要求 SM 6.9，而 6.9
// 只有 Agility 619 的 core 才会报（inbox D3D12Core 最高 6.8），所以"要跑 D3D12 后端"
// 与"启用 agility"现在是同一件事。D3D12Core.dll 不在 MCDR.dll 同目录时下面会 WARN
// 并跳过，随后 6.9 前置检查（graphicsShaderModelProfile / sm69RequirementText）报错
// —— 不会静默降级。
void applyAgilitySdk() {
    if (gAgility.requested) return;
    gAgility.requested = true;
    const std::wstring dir = ownModuleDir();
    if (dir.empty()) {
        dbgLogWarn("P110 agility: cannot resolve MCDR.dll directory - skipped");
        return;
    }
    const std::wstring core = dir + L"D3D12Core.dll";
    if (GetFileAttributesW(core.c_str()) == INVALID_FILE_ATTRIBUTES) {
        dbgLogWarn("P110 agility: %s not found next to MCDR.dll - skipped",
                   toUtf8(core).c_str());
        return;
    }
    const std::string utf8Dir = toUtf8(dir);
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12) {
        dbgLogWarn("P110 agility: d3d12.dll unavailable (err=%lu) - skipped", GetLastError());
        return;
    }
    const auto getInterface =
        reinterpret_cast<PFN_D3D12_GET_INTERFACE>(GetProcAddress(d3d12, "D3D12GetInterface"));
    if (!getInterface) {
        dbgLogWarn("P110 agility: D3D12GetInterface missing from d3d12.dll - skipped");
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12SDKConfiguration> config;
    HRESULT hr = getInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&config));
    if (FAILED(hr) || !config) {
        dbgLogWarn("P110 agility: D3D12GetInterface(CLSID_D3D12SDKConfiguration) hr=0x%08lX - skipped",
                   static_cast<unsigned long>(hr));
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12SDKConfiguration1> config1;
    hr = config.As(&config1);
    if (FAILED(hr) || !config1) {
        dbgLogWarn("P110 agility: QI ID3D12SDKConfiguration1 hr=0x%08lX - skipped",
                   static_cast<unsigned long>(hr));
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12DeviceFactory> factory;
    gAgility.factoryHr = config1->CreateDeviceFactory(kAgilitySdkVersion, utf8Dir.c_str(),
                                                      IID_PPV_ARGS(&factory));
    if (FAILED(gAgility.factoryHr) || !factory) {
        dbgLogWarn("P110 agility: CreateDeviceFactory(sdk=%u, \"%s\") hr=0x%08lX - skipped",
                   kAgilitySdkVersion, utf8Dir.c_str(),
                   static_cast<unsigned long>(gAgility.factoryHr));
        return;
    }
    gAgility.applyHr = factory->ApplyToGlobalState();
    if (FAILED(gAgility.applyHr)) {
        dbgLogWarn("P110 agility: ApplyToGlobalState hr=0x%08lX - skipped",
                   static_cast<unsigned long>(gAgility.applyHr));
        return;
    }
    gAgility.dir = dir;
    gAgility.applied = true;
    dbgLogInfo("P110 agility: opt-in ok sdk=%u core=%s factory=ok apply=ok",
               kAgilitySdkVersion, utf8Dir.c_str());
}

// 设备建好之后才知道真正生效的是哪份 core：SDKVersion 是唯一权威判据。
void logAgilityRuntime(ID3D12Device* device) {
    UINT sdkVersion = 0;
    bool haveConfig = false;
    Microsoft::WRL::ComPtr<ID3D12DeviceConfiguration> config;
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&config))) && config) {
        sdkVersion = config->GetDesc().SDKVersion;
        haveConfig = true;
    }
    const bool agility = haveConfig && sdkVersion >= kAgilitySdkVersion;
    // 注意：这里不要用 GetModuleHandleW(L"D3D12Core.dll") 的路径当"生效 core"——d3d12.dll
    // 在返回 SDKConfiguration 之前就已经把 inbox 那份载入了，同名的 app-local 模块是
    // 第二个模块，按基名查只会拿到先加载的 System32 那份。真正判据是上面的 SDKVersion。
    dbgLogInfo("P110 agility: agility=%s sdk=%u%s requested=%d requestedCoreDir=%s "
               "factory=0x%08lX apply=0x%08lX",
               agility ? "on" : "off", sdkVersion, haveConfig ? "" : "(n/a)",
               gAgility.requested ? 1 : 0,
               gAgility.dir.empty() ? "<none>" : toUtf8(gAgility.dir).c_str(),
               static_cast<unsigned long>(gAgility.factoryHr),
               static_cast<unsigned long>(gAgility.applyHr));
}

#if MCDR_HAVE_STREAMLINE
// P111/P112：Streamline 的「初始化」与「挂接设备」必须分两步，顺序按官方文档：
//   ProgrammingGuideDLSS.md:14  "Call slInit as early as possible (before any dxgi/d3d11/d3d12 APIs are invoked)"
//   ProgrammingGuideDLSS.md:53  设备建好之后再 slSetD3DDevice。
// 之前把 slInit 放在设备创建之后（Phase 0/1），SL 无法完成 DXGI/D3D12 挂钩，插件虽然加载但
// 特性始终 not loaded（slIsFeatureLoaded / slGetFeatureFunction 都返回 0x1F）。这里改成
// ensureDevice() 的第一件事就 slInit（此时还没有任何 DXGI/D3D12 调用）。
// 仍然不静态链接 sl.interposer.lib（LoadLibrary + GetProcAddress，理由见 P111 的教训）。
namespace {
HMODULE gSlModule = nullptr;
std::wstring gSlDir;
bool gSlInitAttempted = false;
bool gSlRequested = false;
sl::Result gSlInitResult = sl::Result::eErrorInitNotCalled;
PFun_slSetD3DDevice* gSlSetDevice = nullptr;
PFun_slIsFeatureLoaded* gSlIsLoaded = nullptr;
}  // namespace

// P112：SL 的 NGX 后端要求一个 **数字** applicationId（sl_appidentity.h / Preferences 注释：
// "Id provided by NVIDIA, if not specified then engine type and version are required"；但实测
// 只给 engine/engineVersion 时 SL 直接拒绝：sl.log commonEntry.cpp:1658
// "Please provide correct application id when calling slInit - NGX based features will be disabled"，
// 随后 dlssEntry.cpp:923 "Missing NGX context" ⇒ kFeatureDLSS 永远 not loaded（0x1F））。
// 用户要求用 Illusion 的 modid 作为身份，这里取 modid 的 FNV-1a 32 位哈希（稳定、可复现）；
// 若将来 NVIDIA 下发正式 Application ID，替换成那个数字即可。
constexpr uint32_t fnv1a32(const char* s, uint32_t h = 2166136261u) {
    return (*s == '\0')
               ? h
               : fnv1a32(s + 1, (h ^ static_cast<uint32_t>(static_cast<unsigned char>(*s))) * 16777619u);
}

bool streamlineRequested() {
    // P118：默认 Streamline（缺运行库自动回退 NGX），决策由 DLSS 实现单元统一给出。
    return mcdr_dlss_streamline_requested() != 0;
}

// 必须在任何 DXGI/D3D12 调用之前跑（ensureDevice 的第一步）。只做 slInit，不碰设备。
void streamlineInitEarly() {
    if (gSlInitAttempted) return;
    gSlInitAttempted = true;
    gSlRequested = streamlineRequested();
    gSlDir = ownModuleDir();
    FILE* probe = gSlDir.empty() ? nullptr : _wfopen((gSlDir + L"sl-probe.txt").c_str(), L"w, ccs=UTF-8");
    if (probe) fwprintf(probe, L"env=%d dir=%s\n", gSlRequested ? 1 : 0, gSlDir.c_str());
    if (!gSlRequested) { if (probe) fclose(probe); return; }
    if (gSlDir.empty()) { if (probe) fclose(probe); return; }
    const std::wstring dllPath = gSlDir + L"sl.interposer.dll";
    gSlModule = ::LoadLibraryW(dllPath.c_str());
    if (gSlModule == nullptr) {
        if (probe) { fwprintf(probe, L"load=fail err=%lu\n", ::GetLastError()); fclose(probe); }
        dbgLogWarn("P111 streamline: %s not found (err=%lu) - skipped", toUtf8(dllPath).c_str(),
                   ::GetLastError());
        return;
    }
    auto pInit = reinterpret_cast<PFun_slInit*>(::GetProcAddress(gSlModule, "slInit"));
    gSlSetDevice = reinterpret_cast<PFun_slSetD3DDevice*>(::GetProcAddress(gSlModule, "slSetD3DDevice"));
    gSlIsLoaded = reinterpret_cast<PFun_slIsFeatureLoaded*>(::GetProcAddress(gSlModule, "slIsFeatureLoaded"));
    if (probe) fwprintf(probe, L"load=ok exports=%d\n", (pInit && gSlSetDevice && gSlIsLoaded) ? 1 : 0);
    if (pInit == nullptr || gSlSetDevice == nullptr || gSlIsLoaded == nullptr) {
        if (probe) fclose(probe);
        dbgLogWarn("P111 streamline: %s is missing expected exports - skipped", toUtf8(dllPath).c_str());
        return;
    }
    const wchar_t* paths[1] = { gSlDir.c_str() };
    // P113：同时请求 Reflex（低延迟），插件才会加载。
    // P113：DLSS + Reflex（低延迟）+ PCL（延迟统计的标记通道，文档 ProgrammingGuidePCL.md:115）。
    // P213：加上 DLSS-G（帧生成）。featuresToLoad 里的插件缺一个，SL 只会记一条日志，
    // 不会让 slInit 整体失败——所以 P213 的探针要单独核对 DLSS_G 的 supported/loaded。
    sl::Feature features[4] = { sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL,
                               sl::kFeatureDLSS_G };
    sl::Preferences pref{};
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0.0";
    pref.applicationId = fnv1a32("dxr_mc");  // = modid 的 FNV-1a 哈希（见上方注释）
    pref.projectId = "";
    pref.renderAPI = sl::RenderAPI::eD3D12;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    // 必须预声明 featuresToLoad（ProgrammingGuide.md:231）：为空时 SL 不加载任何插件。
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 4;
    pref.pathToLogsAndData = gSlDir.c_str();
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                sl::PreferenceFlags::eDisableDebugText |
                // 文档 ProgrammingGuide.md:853：使用 slSetTagForFrame 必须置这一位，
                // 否则 tag 调用返回 eErrorMissingInputParameter(0x13)（P112 实测）。
                sl::PreferenceFlags::eUseFrameBasedResourceTagging |
                // P248：让 SL 走「DXGI 工厂代理」而不是改写 base interface 的 v-table。
                // sl_core_types.h:539-542 原文：'If specified SL will create DXGI factory proxy
                // rather than modifying the v-table for the base interface. This can help with 3rd
                // party overlays which are NOT integrated with the host application but rather
                // operate via injection.' —— 正是 RTSS/Afterburner 的 OSD 场景：改写 v-table 的方式
                // 会把 Present 槽指到 sl.interposer.dll，注入式 overlay 拿不到钩子，OSD 消失。
                sl::PreferenceFlags::eUseDXGIFactoryProxy;
    gSlInitResult = (*pInit)(pref, sl::kSDKVersion);
    if (probe) { fwprintf(probe, L"init=0x%X\n", static_cast<unsigned>(gSlInitResult)); fclose(probe); }
    dbgLogWarn("P111 streamline: slInit=0x%X (early, before any DXGI/D3D12 call)", 
               static_cast<unsigned>(gSlInitResult));
}

// 设备就绪后挂接：slSetD3DDevice + 查询 DLSS 特性是否已加载 + 一次 slDLSSSetOptions 探针。
void applyStreamline(ID3D12Device* device) {
    if (!gSlInitAttempted) streamlineInitEarly();
    if (!gSlRequested || gSlModule == nullptr || gSlInitResult != sl::Result::eOk) return;
    // P112 阶段 4：用 SL 自己的能力查询替代 NGX 的 caps 探测（让"双 NGX"只存在于代码里、
    // 运行期只有 SL 这一套）。slIsFeatureSupported 需要适配器 LUID（ProgrammingGuideDLSS.md:77）。
    int slSupport = -1;
    {
        using PFn_IsFeatureSupported = sl::Result (*)(sl::Feature, const sl::AdapterInfo&);
        auto isSupported = reinterpret_cast<PFn_IsFeatureSupported>(
            ::GetProcAddress(gSlModule, "slIsFeatureSupported"));
        if (isSupported != nullptr && device != nullptr) {
            const LUID luid = device->GetAdapterLuid();
            sl::AdapterInfo info{};
            info.deviceLUID = reinterpret_cast<uint8_t*>(const_cast<LUID*>(&luid));
            info.deviceLUIDSizeInBytes = sizeof(LUID);
            slSupport = static_cast<int>(isSupported(sl::kFeatureDLSS, info));
        }
    }
    const sl::Result deviceResult = (*gSlSetDevice)(device);
    bool loaded = false;
    const sl::Result loadResult = (*gSlIsLoaded)(sl::kFeatureDLSS, loaded);
    FILE* probe = gSlDir.empty() ? nullptr : _wfopen((gSlDir + L"sl-probe.txt").c_str(), L"a, ccs=UTF-8");
    if (probe) {
        fwprintf(probe, L"setDevice=0x%X load=0x%X dlssLoaded=%s\n",
                 static_cast<unsigned>(deviceResult), static_cast<unsigned>(loadResult),
                 loaded ? L"yes" : L"no");
    }
    mcdr_dlss_note_sl_loaded(loaded ? 1 : 0);
    if (!loaded) {
        // P208：插件没加载 ⇒ 后面每帧 evaluate 都会被 native 跳过。历史上这条只打 P111 的
        // `dlssLoaded=no`，容易被当成噪音忽略；这里给一句能照着办的说明。
        dbgLogError("[P208] Streamline is up but the DLSS plugin did NOT initialize. Look for "
                    "'NGXInitValidateSnippets' in sl.log next to MCDR.dll: the installed NGX "
                    "runtime is older than the bundled nvngx_dlss.dll. DLSS stays off and "
                    "sub-pixel jitter is suppressed, so the image is stable instead of vibrating.");
    }
    dbgLogWarn("P111 streamline: slIsFeatureSupported(DLSS)=%d setDevice=0x%X load=0x%X dlssLoaded=%s", slSupport,
               static_cast<unsigned>(deviceResult), static_cast<unsigned>(loadResult),
               loaded ? "yes" : "no");
    using PFn_GetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);
    auto getFeatureFunction = reinterpret_cast<PFn_GetFeatureFunction>(
        ::GetProcAddress(gSlModule, "slGetFeatureFunction"));
    unsigned getFnResult = 0xFFFFFFFFu;
    int optResult = -1;
    if (getFeatureFunction != nullptr) {
        void* raw = nullptr;
        const sl::Result r = getFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", raw);
        getFnResult = static_cast<unsigned>(r);
        if (r == sl::Result::eOk && raw != nullptr) {
            using PFn_SetOptions = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSOptions&);
            auto setOptions = reinterpret_cast<PFn_SetOptions>(raw);
            sl::DLSSOptions opt{};
            opt.mode = sl::DLSSMode::eMaxQuality;
            opt.outputWidth = 1920;
            opt.outputHeight = 1080;
            opt.preExposure = 1.0f;
            opt.exposureScale = 1.0f;
            opt.useAutoExposure = sl::Boolean::eFalse;
            const sl::ViewportHandle viewport(0);
            optResult = static_cast<int>(setOptions(viewport, opt));
        }
    }
    if (probe) {
        fwprintf(probe, L"dlssOptions: getFn=0x%X result=0x%X\n", getFnResult,
                 static_cast<unsigned>(optResult));
        fclose(probe);
    }
    dbgLogWarn("P111 streamline: slDLSSSetOptions probe getFn=0x%X result=0x%X", getFnResult,
               static_cast<unsigned>(optResult));

    }
#endif

// P175：描述符游标与空闲表是文件级全局，不在 DeviceContext 里，所以 `gCtx = DeviceContext{}`
// 清不到它们。重建/销毁设备时必须一起归零，否则新 heap 会从旧游标继续分配（0..旧游标-1 永久
// 空着），旧堆留下的槽号还会从 free list 里被递出来、写进新 heap（槽号在范围内，所以不报错），
// 最终 allocSrvSlot 提前报 "srv heap exhausted"，且新旧堆的占用完全对不上、无从排查。
void resetDescriptorAllocationState() {
    gNextSrv = 0;
    gNextRtv = 0;
    gNextFrameRtv = 0;
    gNextDsv = 0;
    std::lock_guard<std::mutex> lk(gFreeSlotMutex);
    gFreeSrvSlots.clear();
    gFreeSamplerSlots.clear();
}

// P175：完成判据必须和慢路径真正建出来的东西一致。原来只查 device + 7 个 heap，而 queue /
// queueFence / 两个间接绘制签名都是之后才建的：签名创建失败时函数只返回一次错误，之后每次
// 调用却都命中快速路径返回成功，设备永远缺签名（间接绘制每帧报错）。
bool deviceReady() {
    return gCtx.device && gCtx.queue && gCtx.queueFence && gCtx.srvHeap && gCtx.srvCpuHeap && gCtx.rtvHeap
        && gCtx.frameRtvHeap && gCtx.dsvHeap && gCtx.samplerHeap && gCtx.drawHeap && gCtx.cmdSigIndexed
        && gCtx.cmdSigNonIndexed;
}

// P175：慢路径的重入标记（线程局部）。初始化过程中若有代码再建资源，会再次进 ensureDevice；
// 那时 device/heap 还没凑齐、快速路径不成立，若继续走慢路径就会在 gDeviceInitMutex 上自死锁
// 或无限递归 —— 直接返回错误，让调用方看到一条明确的失败。
thread_local bool t_deviceInitInProgress = false;

mcdr::Result<void> ensureDevice() {
    // P112：SL 必须在任何 DXGI/D3D12 调用之前初始化（ProgrammingGuideDLSS.md:14）。
    streamlineInitEarly();
    // P110：在任何设备创建之前把全局 core 换成 619（SM 6.9 的唯一来源，2026-10-25 起无条件尝试）。
    applyAgilitySdk();
    // PIX 捕获层必须在任何 D3D12CreateDevice（含适配器探测）之前载入；未请求时空操作。
    ensurePixCapturerLoaded();
    installCrashHandlerOnce();
    if (deviceReady())
        return {};
    // P175：慢路径串行化（双检）。先查重入，再取锁，再查一次判据 —— 抢到锁的第二个人不会
    // 重建一套 device/heap，而是直接复用第一个人建好的。
    if (t_deviceInitInProgress) {
        return mcdr::err("ensureDevice", "recursive ensureDevice() during device initialization");
    }
    std::lock_guard<std::mutex> initLock(gDeviceInitMutex);
    if (deviceReady())
        return {};
    t_deviceInitInProgress = true;
    struct InitInProgressReset {
        ~InitInProgressReset() { t_deviceInitInProgress = false; }
    } initInProgressReset;

    bool debugEnabled = false;
    const char* dbgLayerEnv = std::getenv("DX12_DEBUG_LAYER");
    if (dbgLayerEnv && dbgLayerEnv[0] == '1') {
        if (gAgility.applied) {
            // 1.619 的 D3D12SDKLayers.dll 不在 vcpkg 包里（只有 D3D12Core.dll）。拿 inbox 的
            // layers 配 agility 的 core 会让 D3D12CreateDevice 直接失败（层与 core 版本不匹配），
            // 所以在 agility 生效时宁可不启用调试层，也不冒这个风险。
            dbgLogWarn("P110 agility: DX12_DEBUG_LAYER ignored - inbox D3D12SDKLayers would not "
                       "match the 1.619 core");
        } else {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                debugEnabled = true;
            }
        }
    }

    // P66：先按 GPU 偏好顺序挑一块支持 12_2 的适配器，再在它上面建设备。
    ComPtr<ID3D12Device> device;
    {
        auto selectRes = selectDeviceAdapter();
        bool ok = static_cast<bool>(selectRes);
        std::string selectError = selectRes ? std::string() : selectRes.error().message;
        // P241：chosen 必须**先**拿到选中的适配器再建设备。原来这里直接把默认构造的
        // `chosen` 交给 createD3D12Device —— 那是一枚空 ComPtr，而 D3D12CreateDevice(NULL)
        // 的含义是「用系统默认适配器」：好不容易探测到的 12_2 卡被丢掉，实际落在进程默认
        // 的那块卡上。Optimus 笔电上进程默认卡常是核显（只有 12_1），于是创建必然失败。
        // 开发机默认卡恰好是 dGPU(RTX 5070)，所以长期未暴露；独立启动器里 FML 早期窗口先在
        // Intel 核显上建 GL 上下文（日志 "GL info: Intel(R) RaptorLake-S Mobile Graphics
        // Controller"），进程默认适配器随之被钉在 iGPU 上 ⇒ D3D12CreateDevice(NULL, 12_2)
        // 失败 ⇒ 自检拿不到纹理 ⇒ P73 致命崩溃。
        if (ok) {
            ComPtr<IDXGIAdapter1> chosen = *selectRes;
            const HRESULT hr = createD3D12Device(chosen.Get(), kRequiredFeatureLevel,
                                                IID_PPV_ARGS(&device));
            if (FAILED(hr)) {
                DXGI_ADAPTER_DESC1 desc{};
                const std::string name = SUCCEEDED(chosen->GetDesc1(&desc))
                    ? adapterDescName(desc) : std::string("<unknown>");
                char hrText[16] = {};
                std::snprintf(hrText, sizeof(hrText), "0x%08X", static_cast<unsigned>(hr));
                selectError = "D3D12CreateDevice(12_2) failed on selected adapter '" + name +
                              "' (hr=" + hrText + ")";
                dbgLogError("P66 %s", selectError.c_str());
                ok = false;
            }
        }
        if (!ok && debugEnabled) {
            // 调试层偶尔会让设备创建失败：关掉调试层重选一次（与旧行为一致）。
            debugEnabled = false;
            selectError.clear();
            auto selectRes2 = selectDeviceAdapter();
            if (selectRes2) {
                const ComPtr<IDXGIAdapter1> chosen2 = *selectRes2;
                ok = SUCCEEDED(createD3D12Device(chosen2.Get(), kRequiredFeatureLevel,
                                                 IID_PPV_ARGS(&device)));
            } else {
                ok = false;
                selectError = selectRes2.error().message;
            }
        }
        if (!ok) {
            // P241 兜底：选中的适配器建不出设备时，退回 P66 之前的旧行为（系统默认适配器），
            // 免得一台本来能跑的机器因为枚举/选择环节出岔子而直接崩掉。
            if (SUCCEEDED(createD3D12Device(nullptr, kRequiredFeatureLevel, IID_PPV_ARGS(&device)))) {
                dbgLogWarn("P241 using the system default adapter after: %s",
                           selectError.c_str());
                ok = true;
            }
        }
        if (!ok) {
            return mcdr::err("ensureDevice",
                             selectError.empty() ? std::string("no D3D12 12_2 adapter") : selectError);
        }
        gCtx.featureLevel = kRequiredFeatureLevel;
    }
    gCtx.device = device;
    gCtx.adapterName = queryAdapterName(device.Get());
    queryDeviceCapabilities(device.Get(), gCtx.caps);
    dbgLogInfo("device ready: %s FL=%s %s", gCtx.adapterName.c_str(),
               featureLevelName(gCtx.featureLevel), capabilitiesText(gCtx.caps).c_str());
    logAgilityRuntime(device.Get());
    // P111：MCDR_STREAMLINE=1 时做一次 Streamline interposer 探针（默认空操作）。
    applyStreamline(device.Get());
    {
        LUID luid = device->GetAdapterLuid();
        ComPtr<IDXGIFactory4> tmpFactory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&tmpFactory)))) {
            tmpFactory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&gCtx.adapter));
        }
        // P108：日志出口必须在**探测之前**接好。探测内部会调 mcdr_dlss_load_runtime()，
        // 而那个函数用 gLogInfo/gLogWarn 报告每一次 LoadLibrary 尝试 —— 之前在这里（探测
        // 之后）才 wire，于是三条尝试日志全被静默丢掉，实机只剩一句
        // "nvngx_dlss.dll not found (GetLastError=0x7E)"，根本看不出它找的是哪条路径。
        // wireDlssImpl() 幂等，所以这里提前调一次是安全的（notifyDlssDeviceReady 里还会再调）。
        wireDlssImpl();
        // RTX（DLSS/NRD）探测。必须放在适配器就绪之后：NGX 的
        // GetFeatureRequirements 要真实 IDXGIAdapter 才能回答「这台机器支不支持」，
        // 拿不到适配器时它只会返回 InvalidParameter，那样的结果比不查更糟。
        // NRD 侧不需要设备（nrd::GetLibraryDesc 是纯查询），会一并填好。
        gCtx.rtx = mcdr::probeRtxSupport(gCtx.adapter.Get());
    }
    // P108：DLSS 那一侧的接线（日志注入 + 运行期路径登记 + 一行可用性结论）。只做这些
    // —— NGX 的 Init / CreateFeature 都要等命令列表打开，见 applyDlssToTexture。
    notifyDlssDeviceReady();
    if (debugEnabled) {
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&gCtx.infoQueue)))) {
            gCtx.infoQueue = nullptr;
        }
        if (gCtx.infoQueue) {
            ComPtr<ID3D12InfoQueue1> iq1;
            if (SUCCEEDED(gCtx.infoQueue.As(&iq1)) && iq1) {
                if (FAILED(iq1->RegisterMessageCallback(&dx12DebugMessageCallback,
                                                        D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr,
                                                        &gInfoQueueCookie))) {
                    dbgLogError("infoQueue: RegisterMessageCallback failed");
                } else {
                    dbgLogInfo("infoQueue: message callback registered cookie=%lu",
                               static_cast<unsigned long>(gInfoQueueCookie));
                }
            }
        }
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&gCtx.queue)))) {
        return mcdr::err("ensureDevice", "CreateCommandQueue failed");
    }
    gCtx.queue->GetTimestampFrequency(&gCtx.timestampFrequency);

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gCtx.queueFence)))) {
        return mcdr::err("ensureDevice", "CreateFence(queue) failed");
    }

    if (!createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, gCtx.srvHeap)
        ||
        !createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize,
                    D3D12_DESCRIPTOR_HEAP_FLAG_NONE, gCtx.srvCpuHeap)
        || !createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapSize,
                       D3D12_DESCRIPTOR_HEAP_FLAG_NONE, gCtx.rtvHeap)
        ||
        !createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kFrameRtvHeapSize,
                    D3D12_DESCRIPTOR_HEAP_FLAG_NONE, gCtx.frameRtvHeap)
        || !createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapSize,
                       D3D12_DESCRIPTOR_HEAP_FLAG_NONE, gCtx.dsvHeap)
        || !createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize,
                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, gCtx.samplerHeap)
        ||
        !createHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDrawHeapSize,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, gCtx.drawHeap)) {
        return mcdr::err("ensureDevice", "CreateDescriptorHeap failed");
    }
    gCtx.srvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gCtx.rtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    gCtx.dsvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    gCtx.samplerInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    gCtx.drawInc = gCtx.srvInc;
    // P175：新 heap 是空的，游标/空闲表必须同步归零（部分失败的路径会走到这里重建 heap）。
    resetDescriptorAllocationState();

    {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC cs{};
        cs.NumArgumentDescs = 1;
        cs.pArgumentDescs = &arg;
        cs.ByteStride =
            20;
        if (FAILED(device->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&gCtx.cmdSigIndexed)))) {
            return mcdr::err("ensureDevice", "CreateCommandSignature(indexed) failed");
        }
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        cs.ByteStride = 16;
        if (FAILED(device->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&gCtx.cmdSigNonIndexed)))) {
            return mcdr::err("ensureDevice", "CreateCommandSignature(non-indexed) failed");
        }
    }
    return {};
}

void destroyDevice() {
    dbgLogProgress("[P184] destroyDevice stage: enter");
    if (gCtx.queueFenceEvent) {
        CloseHandle(gCtx.queueFenceEvent);
        gCtx.queueFenceEvent = nullptr;
    }
    if (gCtx.queue && gCtx.queueFence) {
        UINT64 target = gCtx.queueFenceValue;
        if (gCtx.queueFence->GetCompletedValue() < target) {
            HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (evt) {
                if (SUCCEEDED(gCtx.queueFence->SetEventOnCompletion(target, evt))) {
                    WaitForSingleObject(evt, 1000);
                }
                CloseHandle(evt);
            }
        }
    }
    dbgLogProgress("[P184] destroyDevice stage: queue fence wait done");
    flushPendingDeletes(/*force=*/true);
    dbgLogProgress("[P184] destroyDevice stage: pending deletes flushed");
    // P108：DLSS 的特征句柄与 scratch/中间纹理必须在**设备还活着**的时候释放，并且 NGX
    // 只能在设备销毁前 Shutdown。放在 gCtx 清零之前。
    mcdr_dlss_shutdown();
    dbgLogProgress("[P184] destroyDevice stage: dlss shutdown done");
    gCtx = DeviceContext{};
    // P175：设备级描述符状态不跟着 gCtx 走（见 resetDescriptorAllocationState 的注释）。
    resetDescriptorAllocationState();
    dbgLogProgress("[P184] destroyDevice stage: done");
}

DeviceContext& deviceContext() { return gCtx; }

uintptr_t getDeviceHandle() { return reinterpret_cast<uintptr_t>(gCtx.device.Get()); }

uintptr_t getQueueHandle() { return reinterpret_cast<uintptr_t>(gCtx.queue.Get()); }

uintptr_t createHiddenWindow(int width, int height) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Dx12HiddenTest";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc)) return 0;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"Dx12HiddenTest", L"DX12 Render Test", WS_POPUP, 0, 0,
                                width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        UnregisterClassW(L"Dx12HiddenTest", wc.hInstance);
        return 0;
    }
    ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);
    return reinterpret_cast<uintptr_t>(hwnd);
}

void destroyHiddenWindow(uintptr_t hwnd) {
    if (hwnd) {
        DestroyWindow(reinterpret_cast<HWND>(hwnd));
        UnregisterClassW(L"Dx12HiddenTest", GetModuleHandleW(nullptr));
    }
}

mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> allocRtvHandle() {
    if (gNextRtv >= kRtvHeapSize) {
        return mcdr::err("allocRtvHandle", "RTV descriptor heap exhausted");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h = gCtx.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(gNextRtv) * gCtx.rtvInc;
    ++gNextRtv;
    return h;
}

// Format casting
DXGI_FORMAT toDxgiFormat(int gpuFormat) {
    switch (gpuFormat) {
        case 0: return DXGI_FORMAT_R8_UNORM;  // R8_UNORM
        case 1: return DXGI_FORMAT_R8_SNORM;  // R8_SNORM
        case 2: return DXGI_FORMAT_R8G8_UNORM;  // RG8_UNORM
        case 3: return DXGI_FORMAT_R8G8_SNORM;  // RG8_SNORM
        case 4: return DXGI_FORMAT_R8G8B8A8_UNORM;  // RGB8_UNORM (approximately)
        case 5: return DXGI_FORMAT_R8G8B8A8_SNORM;  // RGB8_SNORM (approximately)
        case 6: return DXGI_FORMAT_R8G8B8A8_UNORM;  // RGBA8_UNORM
        case 7: return DXGI_FORMAT_R8G8B8A8_SNORM;  // RGBA8_SNORM
        case 8: return DXGI_FORMAT_R16_UNORM;  // R16_UNORM
        case 9: return DXGI_FORMAT_R16_SNORM;  // R16_SNORM
        case 10: return DXGI_FORMAT_R16G16_UNORM;  // RG16_UNORM
        case 11: return DXGI_FORMAT_R16G16_SNORM;  // RG16_SNORM
        case 12: return DXGI_FORMAT_R16G16B16A16_UNORM;  // RGB16_UNORM (approximately)
        case 13: return DXGI_FORMAT_R16G16B16A16_SNORM;  // RGB16_SNORM (approximately)
        case 14: return DXGI_FORMAT_R16G16B16A16_UNORM;  // RGBA16_UNORM
        case 15: return DXGI_FORMAT_R16G16B16A16_SNORM;  // RGBA16_SNORM
        case 16: return DXGI_FORMAT_R8_UINT;  // R8_UINT
        case 17: return DXGI_FORMAT_R8_SINT;  // R8_SINT
        case 18: return DXGI_FORMAT_R8G8_UINT;  // RG8_UINT
        case 19: return DXGI_FORMAT_R8G8_SINT;  // RG8_SINT
        case 20: return DXGI_FORMAT_R8G8B8A8_UINT;  // RGB8_UINT (近似)
        case 21: return DXGI_FORMAT_R8G8B8A8_SINT;  // RGB8_SINT (近似)
        case 22: return DXGI_FORMAT_R8G8B8A8_UINT;  // RGBA8_UINT
        case 23: return DXGI_FORMAT_R8G8B8A8_SINT;  // RGBA8_SINT
        case 24: return DXGI_FORMAT_R16_UINT;  // R16_UINT
        case 25: return DXGI_FORMAT_R16_SINT;  // R16_SINT
        case 26: return DXGI_FORMAT_R16G16_UINT;  // RG16_UINT
        case 27: return DXGI_FORMAT_R16G16_SINT;  // RG16_SINT
        case 28: return DXGI_FORMAT_R16G16B16A16_UINT;  // RGB16_UINT (approximately)
        case 29: return DXGI_FORMAT_R16G16B16A16_SINT;  // RGB16_SINT (approximately)
        case 30: return DXGI_FORMAT_R16G16B16A16_UINT;  // RGBA16_UINT
        case 31: return DXGI_FORMAT_R16G16B16A16_SINT;  // RGBA16_SINT
        case 32: return DXGI_FORMAT_R32_UINT;  // R32_UINT
        case 33: return DXGI_FORMAT_R32_SINT;  // R32_SINT
        case 34: return DXGI_FORMAT_R32G32_UINT;  // RG32_UINT
        case 35: return DXGI_FORMAT_R32G32_SINT;  // RG32_SINT
        case 36: return DXGI_FORMAT_R32G32B32A32_UINT;  // RGB32_UINT (approximately)
        case 37: return DXGI_FORMAT_R32G32B32A32_SINT;  // RGB32_SINT (approximately)
        case 38: return DXGI_FORMAT_R32G32B32A32_UINT;  // RGBA32_UINT
        case 39: return DXGI_FORMAT_R32G32B32A32_SINT;  // RGBA32_SINT
        case 40: return DXGI_FORMAT_R16_FLOAT;  // R16_FLOAT
        case 41: return DXGI_FORMAT_R16G16_FLOAT;  // RG16_FLOAT
        case 42: return DXGI_FORMAT_R16G16B16A16_FLOAT;  // RGB16_FLOAT (approximately)
        case 43: return DXGI_FORMAT_R16G16B16A16_FLOAT;  // RGBA16_FLOAT
        case 44: return DXGI_FORMAT_R32_FLOAT;  // R32_FLOAT
        case 45: return DXGI_FORMAT_R32G32_FLOAT;  // RG32_FLOAT
        case 46: return DXGI_FORMAT_R32G32B32_FLOAT;  // RGB32_FLOAT (12B)
        case 47: return DXGI_FORMAT_R32G32B32A32_FLOAT;  // RGBA32_FLOAT (16B)
        case 48: return DXGI_FORMAT_R10G10B10A2_UNORM;  // RGB10A2_UNORM
        case 49: return DXGI_FORMAT_R10G10B10A2_UINT;  // RGB10A2_UINT
        case 50: return DXGI_FORMAT_R11G11B10_FLOAT;  // RG11B10_FLOAT
        case 51: return DXGI_FORMAT_D32_FLOAT;  // D32_FLOAT
        case 52: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;  // D32_FLOAT_S8_UINT
        case 53: return DXGI_FORMAT_D24_UNORM_S8_UINT;  // D24_UNORM_S8_UINT
        case 54: return DXGI_FORMAT_D16_UNORM;  // D16_UNORM
        case 55: return DXGI_FORMAT_UNKNOWN;  // S8_UINT (there's no actual format target)
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

// 顶点输入布局格式：RGB32_* 使用精确三分量格式。
// R32G32B32_FLOAT/UINT/SINT 在 D3D11/D3D12 中都是合法的顶点输入格式
// （仅不能用于纹理/RT 采样）。P25 fix：此前把 RGB32_FLOAT 加宽成 RGBA32_FLOAT，
// 导致 POSITION 元素吃掉 16 字节（offset0-15），与 offset=12 的 TEXCOORD 重叠，
// position.w 读到 TEXCOORD/未初始化内存 → NaN → 齐次裁剪丢弃全部三角形 → 黑屏。
// 用精确三分量后 D3D12 自动为 HLSL float4 语义补 w=1.0。
DXGI_FORMAT toDxgiVertexFormat(int gpuFormat) {
    switch (gpuFormat) {
        case 36: return DXGI_FORMAT_R32G32B32_UINT;  // RGB32_UINT (12B)
        case 37: return DXGI_FORMAT_R32G32B32_SINT;  // RGB32_SINT (12B)
        case 45: return DXGI_FORMAT_R32G32_FLOAT;  // RG32_FLOAT (8B)
        case 46: return DXGI_FORMAT_R32G32B32_FLOAT;  // RGB32_FLOAT (12B)
        default: return toDxgiFormat(gpuFormat);
    }
}

namespace {

// 是否有 depth 面（官方 GpuFormat.hasDepthAspect）
bool hasDepthAspect(int fmt) { return fmt == 51 || fmt == 52 || fmt == 53 || fmt == 54; }

D3D12_HEAP_TYPE pickBufferHeapType(int usage) {
    if (usage & 2) return D3D12_HEAP_TYPE_UPLOAD;  // MAP_WRITE → CPU 直接写入
    if (usage & 1) return D3D12_HEAP_TYPE_READBACK;  // MAP_READ（无 MAP_WRITE）→ CPU 直接读回
    return D3D12_HEAP_TYPE_DEFAULT;  // uniform/vertex → GPU 专属，copy 上传
}

D3D12_RESOURCE_STATES initialStateFor(D3D12_HEAP_TYPE heap) {
    switch (heap) {
        case D3D12_HEAP_TYPE_UPLOAD: return D3D12_RESOURCE_STATE_GENERIC_READ;
        case D3D12_HEAP_TYPE_READBACK: return D3D12_RESOURCE_STATE_COPY_DEST;
        default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

// P76f（用户 m08609「拖影产生了就不会消失」的根因）：深度纹理必须建在**无类型（TYPELESS）**
// 格式上，否则它根本不能被采样。
//
// D3D12 规定：给一个**有类型**（typed）资源建另一种格式的 SRV 是非法的 ——
// `CreateShaderResourceView(D32_FLOAT 资源, R32_FLOAT 描述)` 会被拒绝（无调试层时静默），
// 描述符里留着垃圾，于是着色器 `SampleLevel`/`Load` **一律返回 0**。速度 pass 的开头是
// `if (depth <= 0.0) return 0`，所以运动矢量整张 0 —— 而全 0 的 mv 会让 DLSS 认为画面
// 从未移动过，拖影一旦产生就永不衰减（这正是用户看到的现象）。深度快照的**拷贝**一直
// 正常（拷贝不要求可采样），所以 `input-depth-after-evaluate` 一直有真实数据，把这个
// 问题掩盖了很久。
//
// 建在 R32_TYPELESS 上之后：DSV 仍用 `dxgiFormat`（D32_FLOAT，见 makeDsvDesc），
// SRV 用 R32_FLOAT（见 createTextureView），两边都合法。
DXGI_FORMAT typelessDepthFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        default: return format;
    }
}

D3D12_RESOURCE_FLAGS textureFlags(int usage, int format) {
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (usage & 8) {  // RENDER_ATTACHMENT
        flags |= hasDepthAspect(format) ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                        : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    // P108（用户 m11147 之后的设计收口，(B) 方案的第一步）：storage / UAV 位。
    // 为什么需要：NGX 只肯写进带 ALLOW_UNORDERED_ACCESS 的资源，否则直接
    // `FAIL_RWFlagMissing`（0xBAD00009）。此前我们自己用
    // CreateCommittedResource 建的私有 UAV 中间纹理**连一次普通拷贝都存不住**
    // （写侧二分：拷贝引擎填进去，立刻读与下一帧读都是全 0；见 d3d12_dlss.cpp
    // 里 createDlssOutput 的注释），而由模块这条 createTexture 路径建出来的纹理
    // 一直读写正常。所以改由 JVM 侧建中间纹理、走本路径创建。
    // 位值 32 = USAGE_STORAGE（vulkan 语义的 storage image），与 JVM 侧约定；
    // 纯追加：没有这张位时行为与以前完全一致。
    if (usage & 32) {  // STORAGE / UAV
        flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    return flags;
}

}  // namespace

// ---------------------------------------------------------------------------
// 资源创建
// ---------------------------------------------------------------------------

mcdr::Result<void*> createTexture(int usage, int format, int width, int height, int depthOrLayers,
                                  int mipLevels) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createTexture", errorMessage(d.error()));

    DXGI_FORMAT dxgi = toDxgiFormat(format);
    if (dxgi == DXGI_FORMAT_UNKNOWN) {
        return mcdr::err("createTexture", "createTexture: unsupported format ordinal " + std::to_string(format));
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = std::max(1u, static_cast<UINT>(width));
    desc.Height = std::max(1u, static_cast<UINT>(height));
    // DepthOrArraySize / MipLevels 是 UINT16：std::max 内部推导出 unsigned int，
    // 赋值时的窄化在 /W4 下报 C4244（配合 TreatWarningAsError 即 C2220 构建失败），
    // 故显式 static_cast<UINT16>。
    desc.DepthOrArraySize = static_cast<UINT16>(std::max(1u, static_cast<UINT>(depthOrLayers)));
    desc.MipLevels = static_cast<UINT16>(std::max(1u, static_cast<UINT>(mipLevels)));
    desc.Format = typelessDepthFormat(dxgi);
    desc.SampleDesc.Count = 1;
    desc.Flags = textureFlags(usage, format);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::Texture;
    ++gLiveTex;
    obj->usage = usage;
    obj->size = static_cast<long long>(desc.Width) * desc.Height;
    obj->dxgiFormat = dxgi;
    ++gStatTexCreate;
    HRESULT hr = gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&obj->resource));
    if (FAILED(hr)) {
        std::string text = "CreateCommittedResource(texture): " + hrText(hr);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
            || hr == DXGI_ERROR_DEVICE_HUNG) {
            text += " — " + deviceStatusText();
        }
        return mcdr::err("createTexture", text);
    }
    // P6 诊断：只打印 RENDER_ATTACHMENT（usage & 8）纹理——GUI 中间渲染目标/
    // 主场景 RT 都带此标志，blit 源纹理也在其中（定位 54EE180 之类 handle）。
    if (usage & 8) {
        dbgLogInfo("createTexture: RTA handle=%p w=%d h=%d layers=%d mips=%d fmt=%d usage=0x%x",
                   reinterpret_cast<void*>(obj.get()), width, height, depthOrLayers, mipLevels, format,
                   usage);
    }
    if (usage & 16) {  // CUBEMAP_COMPATIBLE
        dbgLogInfo("createTexture: CUBE handle=%p w=%d h=%d layers=%d mips=%d fmt=%d usage=0x%x",
               reinterpret_cast<void*>(obj.get()), width, height, depthOrLayers, mipLevels, format, usage);
    }
    return static_cast<void*>(obj.release());
}

mcdr::Result<void*> createBuffer(int usage, long long size) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createBuffer", errorMessage(d.error()));
    if (size <= 0) {
        return mcdr::err("createBuffer", "createBuffer: buffer size must be positive");
    }

    D3D12_HEAP_TYPE heapType = pickBufferHeapType(usage);

    // P141：usage 位 1024（本工程内部约定）表示这块缓冲要当 UAV 用（RWByteAddressBuffer），
    // 必须带 ALLOW_UNORDERED_ACCESS。这类缓冲不进池 —— 池键只有 (heapType, 容量)，复用到
    // 一块没这个标志的旧缓冲会让 CreateUnorderedAccessView 失败。
    //
    // 26.3 注意：这里原本用位 512，而 26.3 的 `GpuBuffer.USAGE_INDIRECT_PARAMETERS` 正好是
    // 512。撞位的后果是 vanilla 的 MappableRingBuffer（MAP_WRITE + INDIRECT_PARAMETERS，
    // UPLOAD 堆）被我们当成 UAV 缓冲、给 UPLOAD 堆资源加了
    // D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS —— 该组合非法，
    // CreateCommittedResource 直接返回 E_INVALIDARG(0x80070057)，世界渲染第一帧就崩。
    const bool uavBuffer = (usage & 1024) != 0;

    // P60：优先复用池中 (heapType, 容量) 完全匹配的空闲 buffer，跳过
    // CreateCommittedResource（D3D12 最小分配粒度 64KB + DEFAULT 堆零初始化）。
    // 池内对象均为 fence 已覆盖（GPU 不再引用）者，复用安全。
    const UINT64 alignedSize = (static_cast<UINT64>(size) + 255ULL) & ~255ULL;
    if (!uavBuffer) {
        if (Dx12Object* reused = takePooledBuffer(heapType, alignedSize)) {
            reused->usage = usage;
            reused->size = size;
            reused->mappedPtr = nullptr;
            DBG_LOG_DEBUG("createBuffer: reused pooled buffer size=%lld usage=0x%x", (long long)size, usage);
            if (heapType == D3D12_HEAP_TYPE_READBACK) {
                static std::atomic<int> gRbReuseLog{0};
                const int n = gRbReuseLog.fetch_add(1);
            }
            return reused;
        }
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    // P6：统一 256 对齐分配（D3D12 资源分配粒度本就 64KB，无额外浪费），
    // 保证任意 uniform 切片的 CBV（SizeInBytes 向上取整 256）不越界。
    desc.Width = alignedSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::Buffer;
    ++gLiveBuf;
    accountLiveBuffer(heapType, static_cast<long long>(alignedSize));
    if (heapType == D3D12_HEAP_TYPE_READBACK) {
        static std::atomic<int> gRbLog{0};
        const int rbn = gRbLog.fetch_add(1);
    }
    obj->usage = usage;
    obj->size = size;
    obj->heapType = heapType;
    // P141：UAV 缓冲（meshlet 包围球）必须显式带 ALLOW_UNORDERED_ACCESS，
    // 否则 CreateUnorderedAccessView 直接失败（缓冲默认 Flags = NONE）。
    if (uavBuffer) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ++gStatBufCreate;
    HRESULT hr = gCtx.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, initialStateFor(heapType), nullptr, IID_PPV_ARGS(&obj->resource));
    if (FAILED(hr)) {
        std::string text = "CreateCommittedResource(buffer): " + hrText(hr);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
            || hr == DXGI_ERROR_DEVICE_HUNG) {
            text += " — " + deviceStatusText();
        }
        return mcdr::err("createBuffer", text);
    }
    return static_cast<void*>(obj.release());
}

mcdr::Result<void*> createSampler(int addressU, int addressV, int minFilter, int magFilter,
                                  int maxAnisotropy, float maxLod) {
    return createSamplerWithMipBias(addressU, addressV, minFilter, magFilter, maxAnisotropy, maxLod, 0.0f);
}

mcdr::Result<void*> createSamplerWithMipBias(int addressU, int addressV, int minFilter, int magFilter,
                                             int maxAnisotropy, float maxLod, float mipLodBias) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createSampler", errorMessage(d.error()));

    D3D12_SAMPLER_DESC desc{};
    // address: 0=REPEAT(WRAP), 1=CLAMP_TO_EDGE(CLAMP)
    desc.AddressU = (addressU == 0) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.AddressV = (addressV == 0) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    // P220：超分专用 mip bias（只在 D3D12TerrainSampler 里按 DLSS/DLAA 状态计算）。
    // 非超分时调用方传 0；NaN/越界一律回 0，避免一个坏值把采样器打成全糊。
    float bias = mipLodBias;
    if (!(bias >= -16.0f && bias <= 15.99f)) bias = 0.0f;
    desc.MipLODBias = bias;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = maxLod > 0.0f ? maxLod : 16.0f;
    if (maxAnisotropy > 1) {
        desc.Filter = D3D12_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = static_cast<UINT>(maxAnisotropy);
    } else {
        bool linear = (minFilter == 1) || (magFilter == 1);
        desc.Filter = linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
    }

    auto slotRes = allocSamplerSlot();
    if (!slotRes) return mcdr::err("createSampler", errorMessage(slotRes.error()));
    int slot = *slotRes;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = gCtx.samplerHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * gCtx.samplerInc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = gCtx.samplerHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<SIZE_T>(slot) * gCtx.samplerInc;

    gCtx.device->CreateSampler(&desc, cpu);

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::Sampler;
    obj->cpuHandle = cpu;
    obj->gpuHandle = gpu;
    obj->descSlot = slot;  // 销毁时归还 sampler 槽位
    return static_cast<void*>(obj.release());
}

mcdr::Result<void*> createTextureView(Dx12Object* texture, int baseMipLevel, int mipLevels) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createTextureView", errorMessage(d.error()));
    if (!texture || texture->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("createTextureView", "createTextureView: invalid texture handle");
    }
    // P29：放宽 TEXTURE_BINDING 硬校验。深度/自绘纹理可能只有 RENDER_ATTACHMENT
    // 而无 TEXTURE_BINDING（如 GuiItemAtlas 的 depth 图集 usage=9，官方 Vulkan 后端
    // 创建 view 时不校验 usage），D3D12 对未设 DENY_SHADER_RESOURCE 的资源均可创建
    // SRV，且采样前的状态切换（transitionTo SHADER_RESOURCE）不依赖此位——故降级为
    // 警告而非硬错误，避免 GuiItemAtlas.<init> 渲染崩溃。
    if (!(texture->usage & 4)) {
        dbgLog("createTextureView: texture usage=0x%x lacks TEXTURE_BINDING (fmt=%d) — creating SRV anyway",
               texture->usage, static_cast<int>(texture->dxgiFormat));
    }

    auto slotRes = allocSrvSlot();
    if (!slotRes) return mcdr::err("createTextureView", errorMessage(slotRes.error()));
    int slot = *slotRes;
    // P6：CPU-only 镜像堆创建描述符（CopyDescriptorsSimple 复制源必须非
    // SHADER_VISIBLE）；同槽位在 SHADER_VISIBLE 堆再创建一份供 GPU 直接引用。
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = gCtx.srvCpuHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * gCtx.srvInc;
    D3D12_CPU_DESCRIPTOR_HANDLE gpuCpu = gCtx.srvHeap->GetCPUDescriptorHandleForHeapStart();
    gpuCpu.ptr += static_cast<SIZE_T>(slot) * gCtx.srvInc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = gCtx.srvHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<SIZE_T>(slot) * gCtx.srvInc;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    // 深度格式（D32_FLOAT/D16_UNORM/D32_FLOAT_S8X24_UINT/D24_UNORM_S8_UINT）
    // 不能直接作为 SRV 格式（调试层：'The format cannot be used with a
    // ShaderResource view'）——需换成同格式族内的只读视图格式（读深度为
    // float/uint），纹理本体仍保持深度格式用于 DSV。
    // P76f：格式取 `dxgiFormat`（具体的有类型格式），不取资源描述 —— 深度纹理现在建在
    // TYPELESS 上（见 typelessDepthFormat），按资源格式走会得到 TYPELESS，那种格式不能
    // 直接做 SRV 格式，必须映射到同族的只读格式。
    DXGI_FORMAT viewFormat = texture->dxgiFormat;
    switch (viewFormat) {
        case DXGI_FORMAT_D32_FLOAT: viewFormat = DXGI_FORMAT_R32_FLOAT; break;
        case DXGI_FORMAT_D16_UNORM: viewFormat = DXGI_FORMAT_R16_UNORM; break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: viewFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: viewFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        default: break;
    }
    srv.Format = viewFormat;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (texture->usage & 16) {  // CUBEMAP_COMPATIBLE
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MipLevels = static_cast<UINT>(std::max(1, mipLevels));
        srv.TextureCube.MostDetailedMip = static_cast<UINT>(std::max(0, baseMipLevel));
        dbgLogInfo("createTextureView: CUBE srv slot=%d fmt=%d mip=%d most=%d layers=%d", slot,
               static_cast<int>(texture->dxgiFormat), static_cast<int>(srv.TextureCube.MipLevels),
               static_cast<int>(srv.TextureCube.MostDetailedMip),
               static_cast<int>(texture->resource->GetDesc().DepthOrArraySize));
    } else {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = static_cast<UINT>(std::max(1, mipLevels));
        srv.Texture2D.MostDetailedMip = static_cast<UINT>(std::max(0, baseMipLevel));
    }
    gCtx.device->CreateShaderResourceView(texture->resource.Get(), &srv, cpu);
    // 同槽位在 SHADER_VISIBLE 堆再创建一份（gpuHandle 供 GPU 直接引用）。
    gCtx.device->CreateShaderResourceView(texture->resource.Get(), &srv, gpuCpu);

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::TextureView;
    obj->cpuHandle = cpu;
    obj->gpuHandle = gpu;
    obj->descSlot = slot;  // 销毁时归还 SRV 槽位
    obj->sourceTexture = texture;  // 采样前据此 transition 底层纹理状态
    return static_cast<void*>(obj.release());
}

void destroyObject(Dx12Object* obj) {
    if (!obj) return;
    if (obj->mappedPtr) {
        if (obj->resource) obj->resource->Unmap(0, nullptr);
        obj->mappedPtr = nullptr;
    }
    // P12 诊断：关闭阶段定位用（destroyCommandEncoder 之后的销毁路径原本
    // 全静默，卡死时无法判断 Java 侧 transientMemory/管线缓存关闭进行到哪）。
    DBG_LOG_DEBUG("destroyObject: kind=%d size=%lld", (int)obj->kind, (long long)obj->size);
    if (obj->kind == Dx12Object::Kind::Buffer && obj->heapType == D3D12_HEAP_TYPE_READBACK) {
        static std::atomic<int> gRbDestroyLog{0};
        const int n = gRbDestroyLog.fetch_add(1);
    }
    // P6：延迟销毁（官方 VulkanGpuBuffer.close() 的 queueForDestroy 语义）。
    // 若资源正被打开的命令列表引用，立即 delete 会在 Close 时报
    // "deleted prior to closing the command list"（E_INVALIDARG）。登记到
    // pending，等所有打开的命令列表提交完成（submitCommandList 同步等待后
    // 调用 flushPendingDeletes）再统一释放。
    // P45 fix：与渲染线程 flushPendingDeletes() 的 swap 互斥（见 gPendingMutex
    // 注释）。此前无锁的 push_back 会在渲染线程遍历期间触发扩容 → 迭代器失效。
    //
    // P46 fix：登记"必须已完成"的 queue fence 值。submitCommandList 每次提交
    // 都会 `++gCtx.queueFenceValue` 并 Signal gCtx.queueFence，所以
    // 「下一次提交的 fence 值」就覆盖了任何可能引用本对象的在飞命令列表。
    //
    // P47 fix（#921 根因）：分两种情况——
    //   - 仍有打开的命令列表：该对象可能被这条未提交的列表引用，而它的提交时刻
    //     未知（可能很多帧之后、fence 值远高于当前）。登记阻塞哨兵值，待所有
    //     打开列表都提交完毕后再回填门槛（见 flushPendingDeletes）。
    //   - 无打开列表：新列表无法再引用它（Java 句柄已销毁），只需等"当前已提交/
    //     正在提交"的所有 GPU 工作完成，即 queueFenceValue+1（+1 覆盖并发中
    //     尚未 Signal 的那次提交，消除 endCommandList 与 ++queueFenceValue 之间的
    //     竞态窗口）。
    UINT64 required = (gOpenListCount > 0) ? kFenceBlockedByOpenList : (gCtx.queueFenceValue + 1);
    {
        std::lock_guard<std::mutex> lk(gPendingMutex);
        gPendingDeletes.push_back(PendingDelete{obj, required});
    }
}

// ---------------------------------------------------------------------------
// Buffer 映射（对应官方 GpuBuffer.map：read 需 MAP_READ，write 需 MAP_WRITE）
// ---------------------------------------------------------------------------

mcdr::Result<void*> mapBuffer(Dx12Object* buffer, long long offset, long long length, bool read,
                              bool write) {
    if (!buffer || buffer->kind != Dx12Object::Kind::Buffer) {
        return mcdr::err("mapBuffer", "mapBuffer: invalid buffer handle");
    }
    if (read && !(buffer->usage & 1)) {
        return mcdr::err("mapBuffer", "mapBuffer: buffer lacks MAP_READ");
    }
    if (write && !(buffer->usage & 2)) {
        return mcdr::err("mapBuffer", "mapBuffer: buffer lacks MAP_WRITE");
    }
    if (offset < 0 || length < 0 || offset + length > buffer->size) {
        return mcdr::err("mapBuffer", "mapBuffer: range out of bounds");
    }
    if (buffer->heapType != D3D12_HEAP_TYPE_UPLOAD && buffer->heapType != D3D12_HEAP_TYPE_READBACK) {
        return mcdr::err("mapBuffer",
                         "mapBuffer: buffer is not host-visible (use MAP_WRITE/MAP_READ usage)");
    }

    if (!buffer->mappedPtr) {
        void* base = nullptr;
        HRESULT hr = buffer->resource->Map(0, nullptr, &base);
        if (FAILED(hr)) {
            return mcdr::err("mapBuffer", "Map: " + hrText(hr));
        }
        buffer->mappedPtr = base;
    }
    return static_cast<char*>(buffer->mappedPtr) + offset;
}

void unmapBuffer(Dx12Object* buffer) {
    if (!buffer || buffer->kind != Dx12Object::Kind::Buffer) return;
    if (buffer->mappedPtr) {
        buffer->resource->Unmap(0, nullptr);
        buffer->mappedPtr = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 资源层自检：创建 texture/buffer/sampler/view，map 写读验证，再销毁
// ---------------------------------------------------------------------------

std::string runResourceSelfTest() {
    if (auto d = ensureDevice(); !d) return "SELF-TEST FAILED: " + errorMessage(d.error());

    // 1) texture: RGBA8_UNORM(6), 64x64, TEXTURE_BINDING|RENDER_ATTACHMENT
    auto texRes = createTexture(4 | 8, 6, 64, 64, 1, 1);
    if (!texRes) return "SELF-TEST FAILED: texture - " + errorMessage(texRes.error());
    Dx12Object* tex = static_cast<Dx12Object*>(*texRes);

    // 2) buffer: VERTEX|COPY_DST|MAP_WRITE, 1024
    auto bufRes = createBuffer(32 | 8 | 2, 1024);
    if (!bufRes) {
        destroyObject(tex);
        return "SELF-TEST FAILED: buffer - " + errorMessage(bufRes.error());
    }
    Dx12Object* buf = static_cast<Dx12Object*>(*bufRes);

    // 3) map 写入并读回验证
    auto mapRes = mapBuffer(buf, 0, 1024, false, true);
    if (!mapRes) {
        destroyObject(buf);
        destroyObject(tex);
        return "SELF-TEST FAILED: map - " + errorMessage(mapRes.error());
    }
    void* p = *mapRes;
    memset(p, 0x5A, 1024);
    bool dataOk =
        (static_cast<unsigned char*>(p)[0] == 0x5A) && (static_cast<unsigned char*>(p)[1023] == 0x5A);
    unmapBuffer(buf);

    // 4) sampler: REPEAT/REPEAT, NEAREST/NEAREST, aniso=1, maxLod=16
    auto samplerRes = createSampler(0, 0, 0, 0, 1, 16.0f);
    if (!samplerRes) {
        destroyObject(buf);
        destroyObject(tex);
        return "SELF-TEST FAILED: sampler - " + errorMessage(samplerRes.error());
    }
    Dx12Object* sampler = static_cast<Dx12Object*>(*samplerRes);

    // 5) texture view
    auto viewRes = createTextureView(tex, 0, 1);
    if (!viewRes) {
        destroyObject(sampler);
        destroyObject(buf);
        destroyObject(tex);
        return "SELF-TEST FAILED: texture view - " + errorMessage(viewRes.error());
    }
    Dx12Object* view = static_cast<Dx12Object*>(*viewRes);

    std::string result =
        "SELF-TEST OK (map write/read " + std::string(dataOk ? "verified" : "MISMATCH") + ")";

    destroyObject(view);
    destroyObject(sampler);
    destroyObject(buf);
    destroyObject(tex);
    return result;
}

// ---------------------------------------------------------------------------
// 命令层（P3）
// ---------------------------------------------------------------------------

namespace {

// initialStateFor 已在 P2 资源层匿名 namespace 中定义（同一翻译单元内不得重复）。
// 注意：UPLOAD heap 的 staging buffer 初始态为 GENERIC_READ，但 CopyBuffer/CopyTexture
// 操作要求源资源必须显式处于 COPY_SOURCE。不能将 GENERIC_READ 视为 COPY_SOURCE 的等价
// 状态而跳过 barrier——D3D12 Debug Layer 会报 "Before state must be COPY_SOURCE" 错误。
// from==to 的冗余跳过由 transitionTo/resourceBarrier 内部保护。
bool needTransition(D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return false;
    return true;
}


// 将 D3D12_RESOURCE_STATES 转为可读名称，用于诊断日志。
// 注意：D3D12_RESOURCE_STATE_PRESENT 和 D3D12_RESOURCE_STATE_COMMON 在此 SDK
// 版本中均为 0（flip model 下等价），不可同时作为独立 case。
static const char* stateNameFor(D3D12_RESOURCE_STATES s) {
    switch (s) {
        case D3D12_RESOURCE_STATE_COMMON: return "COMMON";
        case D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER: return "VB_CB";
        case D3D12_RESOURCE_STATE_INDEX_BUFFER: return "IB";
        case D3D12_RESOURCE_STATE_RENDER_TARGET: return "RT";
        case D3D12_RESOURCE_STATE_UNORDERED_ACCESS: return "UA";
        case D3D12_RESOURCE_STATE_DEPTH_WRITE: return "DEPTH_W";
        case D3D12_RESOURCE_STATE_DEPTH_READ: return "DEPTH_R";
        case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE: return "NPSR";
        case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE: return "PSR";
        case D3D12_RESOURCE_STATE_STREAM_OUT: return "SO";
        case D3D12_RESOURCE_STATE_COPY_DEST: return "COPY_D";
        case D3D12_RESOURCE_STATE_COPY_SOURCE: return "COPY_S";
        case D3D12_RESOURCE_STATE_RESOLVE_DEST: return "RESOLVE_D";
        case D3D12_RESOURCE_STATE_RESOLVE_SOURCE: return "RESOLVE_S";
        case D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT: return "INDIRECT";
        default: return "???";
    }
}

void resourceBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
                     D3D12_RESOURCE_STATES to) {
    // P277：res 为空时 ResourceBarrier 会记一条非法调用，随后 Close() 直接 E_INVALIDARG，
    // 而日志里只剩 "Close HRESULT 0x80070057" 这句——查不到是谁把空指针塞进了状态表。
    // 这里挡掉并报错，让症状落在真正的调用点上。
    if (res == nullptr) {
        dbgLogError("resourceBarrier: null resource (%s -> %s) ignored", stateNameFor(from),
                    stateNameFor(to));
        return;
    }
    // D3D12 要求 StateBefore != StateAfter；相同状态发出 barrier 会触发
    // ID3D12CommandList::ResourceBarrier 验证 ERROR（0x80004005）。
    // 此处做保护性跳过，并记录诊断日志。
    if (from == to) {
        DBG_LOG_DEBUG("resourceBarrier: SKIP (from==to=%s) res=%p", stateNameFor(from), (void*)res);
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    // P27：resourceBarrier 在加载期可被调用数万次（上传纹理回切 COMMON），
    // 逐条 dbgLog 写文件曾拖死主线程（加载条冻结 4.7s+、窗口未响应）。
    // 降级为 DBG_LOG_DEBUG（DX12_LOG_VERBOSE=1 才输出）。
    DBG_LOG_DEBUG("resourceBarrier: %p %s -> %s", (void*)res, stateNameFor(from), stateNameFor(to));
    list->ResourceBarrier(1, &b);
}

// DXGI_FORMAT -> 每 texel 字节数（仅非压缩格式；压缩格式返回 16 = 4x4 块）。
// 用于 CopyTextureRegion 的 footprint 行距计算。
UINT blockSizeFor(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT: return 12;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_D32_FLOAT: return 8;
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return 4;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT: return 4;
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT: return 2;
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM: return 1;
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM: return 8;  // 4x4 块
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB: return 16;  // 4x4 块
        default: return 4;
    }
}

// 在指定队列上执行瞬时提交并等待完成（query 读回 / getTimestampNow 用）。
// 等待有界（5s）：GPU 队列卡住时返回错误而不是无限挂起（黑屏无响应根因嫌疑）。
mcdr::Result<void> flushAndWait(ID3D12CommandList* list, const char* where) {
    dbgLogInfo("flushAndWait: enter");
    ID3D12CommandList* lists[] = {list};
    gCtx.queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    if (FAILED(gCtx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        return mcdr::err(where, "CreateFence failed");
    }
    UINT64 fv = 1;
    if (FAILED(gCtx.queue->Signal(fence.Get(), fv))) {
        return mcdr::err(where, "Signal failed");
    }
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt) {
        return mcdr::err(where, "CreateEvent failed");
    }
    while (fence->GetCompletedValue() < fv) {
        fence->SetEventOnCompletion(fv, evt);
        if (WaitForSingleObject(evt, 5000) != WAIT_OBJECT_0) {
            CloseHandle(evt);
            dbgLogError("flushAndWait: TIMEOUT (queue stalled)");
            return mcdr::err(where, "flushAndWait: timed out waiting for queue completion");
        }
    }
    CloseHandle(evt);
    dbgLogInfo("flushAndWait: done");
    return {};
}

}  // namespace

// 按本 command list 已跟踪状态把资源过渡到 to。fromInitial 为资源创建时的
// 初始状态（texture=COMMON，buffer=initialStateFor(heapType)）；若本 list 内
// 已跟踪过该资源则以跟踪状态为准。绝不在 list 内回退到 COMMON（D3D12 禁止
// 从已提升状态显式回 COMMON；decay 由命令列表执行完成时隐式处理）。
// beginCommandList 无条件清空 tracking（submit 阻塞等待 GPU 完成，资源已
// 隐式 decay 回 COMMON；此处按初始态开始录制是正确且保守的）。
// 定义在匿名 namespace 之外：transitionTextureTo 供 dx12_surface.cpp 使用。
void transitionTo(CommandContext* ctx, ID3D12Resource* res, D3D12_RESOURCE_STATES fromInitial,
                  D3D12_RESOURCE_STATES to) {
    if (!ctx || !res) return;
    auto& m = ctx->resourceState;
    D3D12_RESOURCE_STATES from;
    auto it = m.find(res);
    if (it != m.end()) {
        from = it->second;
    } else {
        from = fromInitial;
        if (!needTransition(from, to)) return;
    }
    if (from == to) return;
    resourceBarrier(ctx->commandList, res, from, to);
    m[res] = to;
}

// buffer 按跟踪状态过渡（不回落初始状态）。UPLOAD heap staging buffer 被
// transition 到 COPY_SOURCE 时，同时记录到 stagingCopySourceResources，
// 以便 endCommandList 将其回切 GENERIC_READ（而非 COMMON）。
void transitionBufferTo(CommandContext* ctx, Dx12Object* buf, D3D12_RESOURCE_STATES to) {
    if (!buf || !buf->resource) return;
    transitionTo(ctx, buf->resource.Get(), initialStateFor(buf->heapType), to);
    // 记录 staging buffer 的 COPY_SOURCE 状态，供 endCommandList 回切到 GENERIC_READ。
    if (buf->heapType == D3D12_HEAP_TYPE_UPLOAD && to == D3D12_RESOURCE_STATE_COPY_SOURCE) {
        auto& m = ctx->resourceState;
        auto it = m.find(buf->resource.Get());
        if (it != m.end() && it->second == D3D12_RESOURCE_STATE_COPY_SOURCE) {
            ctx->stagingCopySourceResources.insert(buf->resource.Get());
        }
    }
}

// texture 按跟踪状态过渡（初始锚点 COMMON；不回落 COMMON）。
void transitionTextureTo(CommandContext* ctx, Dx12Object* tex, D3D12_RESOURCE_STATES to) {
    if (!tex || !tex->resource) return;
    transitionTo(ctx, tex->resource.Get(), D3D12_RESOURCE_STATE_COMMON, to);
}

// 阻塞等待 GPU 队列空闲：Signal 新 fence 并等待其完成。
// 用于销毁 swapchain/设备前，确保 backbuffer 等资源不再被 GPU 使用。
// 等待有界（5s）：队列卡住时返回错误，避免销毁路径无限挂起。
mcdr::Result<void> deviceWaitIdle() {
    if (!gCtx.device || !gCtx.queue) {
        return mcdr::err("deviceWaitIdle", "deviceWaitIdle: device not initialized");
    }
    dbgLogDebug("deviceWaitIdle: enter");
    // 关键：先读当前已完成值，再信号一个绝对高于它的值。
    // 原逻辑 fv = ++gCtx.queueFenceValue 存在竞态——若 GPU 已处理到更高的 fence
    // 值（Java 线程在 awaitCompletion 后提交更多帧），fv 会低于已完成值，
    // GetCompletedValue() >= fv 导致 while 循环立即退出，后续 ResizeBuffers
    // 看到 DWM 仍持有 backbuffer 引用而失败（DXGI_ERROR_INVALID_CALL）。
    UINT64 cv = gCtx.queueFence->GetCompletedValue();
    UINT64 fv = cv + 1;
    // 防御：若 fv 已等于或超过 queueFenceValue（说明有并发提交更新了计数器），
    // 继续递增直到 fv > queueFenceValue，保证我们等待的值是尚未被 GPU 处理过的。
    while (fv <= gCtx.queueFenceValue) {
        ++fv;
    }
    if (FAILED(gCtx.queue->Signal(gCtx.queueFence.Get(), fv))) {
        return mcdr::err("deviceWaitIdle", "deviceWaitIdle: Signal failed");
    }
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt) {
        return mcdr::err("deviceWaitIdle", "deviceWaitIdle: CreateEvent failed");
    }
    while (gCtx.queueFence->GetCompletedValue() < fv) {
        gCtx.queueFence->SetEventOnCompletion(fv, evt);
        if (WaitForSingleObject(evt, 5000) != WAIT_OBJECT_0) {
            CloseHandle(evt);
            dbgLogError("deviceWaitIdle: TIMEOUT (queue stalled)");
            return mcdr::err("deviceWaitIdle", "deviceWaitIdle: timed out waiting for queue completion");
        }
    }
    CloseHandle(evt);
    dbgLogDebug("deviceWaitIdle: done (waited for fv=%llu)", static_cast<unsigned long long>(fv));
    return {};
}

mcdr::Result<void*> createCommandEncoder() {
    if (auto d = ensureDevice(); !d) return mcdr::err("createCommandEncoder", errorMessage(d.error()));
    DBG_LOG_DEBUG("createCommandEncoder: enter");
    auto ctx = std::make_unique<CommandContext>();
    for (int i = 0; i < 3; ++i) {
        ++gStatEncoderCreate;
        if (FAILED(gCtx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&ctx->allocators[i])))) {
            return mcdr::err("createCommandEncoder", "createCommandEncoder: CreateCommandAllocator failed");
        }
    }
    // P55：创建 3 条 command list，与 3 个 allocator 一一对应（多帧飞行）。
    for (int i = 0; i < 3; ++i) {
        if (FAILED(gCtx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ctx->allocators[i].Get(),
                                                  nullptr, IID_PPV_ARGS(&ctx->commandLists[i])))) {
            return mcdr::err("createCommandEncoder", "createCommandEncoder: CreateCommandList failed");
        }
        // 初始 closed 状态；beginCommandList 时 Reset。
        ctx->commandLists[i]->Close();
    }
    ctx->currentListSlot = 0;
    ctx->commandList = ctx->commandLists[0].Get();
    if (FAILED(gCtx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx->fence)))) {
        return mcdr::err("createCommandEncoder", "createCommandEncoder: CreateFence failed");
    }
    ctx->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ctx->fenceEvent) {
        return mcdr::err("createCommandEncoder", "createCommandEncoder: CreateEvent failed");
    }
    dbgLogInfo("createCommandEncoder: done ctx=%p", reinterpret_cast<void*>(ctx.get()));
    return static_cast<void*>(ctx.release());
}

void destroyCommandEncoder(CommandContext* ctx) {
    if (!ctx) return;
    DBG_LOG_DEBUG("destroyCommandEncoder: enter fenceValue=%llu", (unsigned long long)ctx->fenceValue);
    // 该 ctx 可能仍有未执行的已提交命令（如 createBuffer(data) 的 submit 后
    // 立即 close）；先等它全部完成再销毁 allocator，否则 GPU 在使用已释放的
    // allocator 会导致 DXGI_ERROR_DEVICE_REMOVED。
    if (ctx->fenceValue > 0) {
        auto w = waitForFenceValue(ctx, ctx->fenceValue, 5000000000ULL);
        if (!w) {
            dbgLogError("destroyCommandEncoder: wait FAILED: %s", errorMessage(w.error()).c_str());
        }
    }
    // 若该 ctx 仍处于打开（begin 后从未 submit）状态：命令列表即将销毁，
    // 从未进入 GPU 执行队列，引用它的资源随之不再被任何命令列表引用，
    // 因此计入的打开计数一并释放；计数归零时顺带 flush 延迟删除对象。
    if (ctx->listOpen) {
        ctx->listOpen = 0;
        if (gOpenListCount > 0) --gOpenListCount;
        dbgLog("destroyCommandEncoder: openList now=%d", gOpenListCount);
        if (gOpenListCount == 0) flushPendingDeletes();
    }
    if (ctx->fenceEvent) CloseHandle(ctx->fenceEvent);
    delete ctx;
    dbgLogInfo("destroyCommandEncoder: done ctx=%p", reinterpret_cast<void*>(ctx));
}

mcdr::Result<void> beginCommandList(CommandContext* ctx) {
    if (!ctx) {
        return mcdr::err("beginCommandList", "beginCommandList: null ctx");
    }
    // P33 fix：若列表已打开（构造函数或先前调用已打开），幂等返回——不再 Reset
    // 同一 allocator，避免 E_FAIL（allocator 仍在 GPU 使用中）。
    if (ctx->listOpen) return {};
    // P55（command list 三路 ring）：选一个 ring 槽位 = fenceValue % 3。
    // 此前只有单条 commandList，主线程必须等**上一帧**（fenceValue）GPU 执行完
    // 才能 Reset 同一条 list，导致 CPU 录制与 GPU 执行完全串行（实测 GPU 33%/
    // CPU 29% 却只跑到 ~680 FPS）。现在 list 与 allocator 一一对应同一槽位，
    // 录制帧 N 时用的是帧 N-3 的槽位，只需等该槽位**上一次**提交的 per-list
    // fence（slotFence[slot]）完成，即可与最近两帧的 GPU 执行重叠。
    UINT slot = static_cast<UINT>(ctx->fenceValue % 3u);
    ctx->currentListSlot = slot;
    ctx->commandList = ctx->commandLists[slot].Get();
    // P55：DX12_CMD_RING=0 回退为旧的串行行为（等最近一次提交的 per-list fence，
    // 即帧 N-1），便于 AB 对比/出问题快速回退。默认开启三路 ring。
    static const bool sCmdRing = [] {
        const char* v = getenv("DX12_CMD_RING");
        return !(v && v[0] == '0');
    }();
    UINT64 waitVal = sCmdRing ? ctx->slotFence[slot] : ctx->fenceValue;
    return beginCommandListWithWait(ctx, waitVal);
}

namespace {

// ---- P97：VRS 着色粒度 ------------------------------------------------------
// 着色速率只有这 7 个取值（d3d12.h 的 D3D12_SHADING_RATE）。注意 2X1 不是 0x2 而是
// 0x4：这个枚举按 x/y 两轴各自的 log2 指数打包，不是递增序号。
const char* shadingRateName(int rate) {
    switch (rate) {
        case static_cast<int>(D3D12_SHADING_RATE_1X1): return "1X1";
        case static_cast<int>(D3D12_SHADING_RATE_1X2): return "1X2";
        case static_cast<int>(D3D12_SHADING_RATE_2X1): return "2X1";
        case static_cast<int>(D3D12_SHADING_RATE_2X2): return "2X2";
        case static_cast<int>(D3D12_SHADING_RATE_2X4): return "2X4";
        case static_cast<int>(D3D12_SHADING_RATE_4X2): return "4X2";
        case static_cast<int>(D3D12_SHADING_RATE_4X4): return "4X4";
        default: return "unknown";
    }
}

bool isKnownShadingRate(int rate) {
    switch (rate) {
        case static_cast<int>(D3D12_SHADING_RATE_1X1):
        case static_cast<int>(D3D12_SHADING_RATE_1X2):
        case static_cast<int>(D3D12_SHADING_RATE_2X1):
        case static_cast<int>(D3D12_SHADING_RATE_2X2):
        case static_cast<int>(D3D12_SHADING_RATE_2X4):
        case static_cast<int>(D3D12_SHADING_RATE_4X2):
        case static_cast<int>(D3D12_SHADING_RATE_4X4):
            return true;
        default:
            return false;
    }
}

// 档位能否落地：nullptr = 可以，否则返回原因（静态字符串）。只看设备能力表，因此既能在设备
// 已存在时立刻作答，也能在每条命令列表下发前兜底。
const char* shadingRateUnsupportedReason(int rate) {
    if (!gCtx.caps.shadingRateSupported()) {
        return "the device reports VRS tier 0";
    }
    if (!gCtx.caps.additionalShadingRates &&
        (rate == static_cast<int>(D3D12_SHADING_RATE_2X4) ||
         rate == static_cast<int>(D3D12_SHADING_RATE_4X2) ||
         rate == static_cast<int>(D3D12_SHADING_RATE_4X4))) {
        // tier 1 只保证 1x1/1x2/2x1/2x2；下发设备不支持的速率会让整条命令列表被 runtime 丢弃。
        return "the device reports AdditionalShadingRatesSupported = false";
    }
    return nullptr;
}

// P231：组合器名（日志用）。
const char* shadingRateCombinerName(int combiner) {
    switch (combiner) {
        case static_cast<int>(D3D12_SHADING_RATE_COMBINER_PASSTHROUGH): return "PASSTHROUGH";
        case static_cast<int>(D3D12_SHADING_RATE_COMBINER_OVERRIDE): return "OVERRIDE";
        case static_cast<int>(D3D12_SHADING_RATE_COMBINER_MIN): return "MIN";
        case static_cast<int>(D3D12_SHADING_RATE_COMBINER_MAX): return "MAX";
        case static_cast<int>(D3D12_SHADING_RATE_COMBINER_SUM): return "SUM";
        default: return "unknown";
    }
}

// 着色粒度控制的**唯一落点**：把 rate 立刻下发到 list（RSSetShadingRate + 当前组合器）。
// P231 起只会从 setShadingRateNow()（Java 侧的 scope）进来，不再在每条列表 Reset 之后全局
// 重放 —— 那正是 P97 让天空/粒子/GUI 一起变粗的原因。返回值只用于日志，调用方拿 bool。
bool issueShadingRate(ID3D12GraphicsCommandList* list, int rate) {
    if (!list || rate < 0) {
        return false;
    }
    // 能力兜底：配置一般在设备创建之前就读到了，那时问不到 tier/additional，所以真正下发前
    // 再判一次（只报一次 WARN，避免刷屏）。
    if (const char* why = shadingRateUnsupportedReason(rate)) {
        if (!gCtx.shadingRateUnsupportedLogged) {
            gCtx.shadingRateUnsupportedLogged = true;
            dbgLogWarn("VRS: shading rate %s not applied: %s; staying at 1X1", shadingRateName(rate), why);
        }
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList5> list5;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list5))) || !list5) {
        // 老 runtime 上拿不到 list5：不能静默 —— 那会表现为「配置了却不生效」。
        if (!gCtx.shadingRateList5Logged) {
            gCtx.shadingRateList5Logged = true;
            dbgLogWarn("VRS: ID3D12GraphicsCommandList5 unavailable; shading rate %s not applied",
                       shadingRateName(rate));
        }
        return false;
    }
    // 组合器来自设备上下文（P231 起可设）。默认都 PASSTHROUGH：既没人写 SV_ShadingRate，
    // 也还没有着色率图像，于是最终速率就等于这次设的每 draw 速率。
    const D3D12_SHADING_RATE_COMBINER combiners[2] = {
        static_cast<D3D12_SHADING_RATE_COMBINER>(gCtx.shadingRateCombiner0),
        static_cast<D3D12_SHADING_RATE_COMBINER>(gCtx.shadingRateCombiner1)};
    list5->RSSetShadingRate(static_cast<D3D12_SHADING_RATE>(rate), combiners);
    ++gCtx.shadingRateAppliedLists;
    if (gCtx.shadingRateAppliedLists == 1) {
        dbgLogInfo("VRS: shading rate %s applied via RSSetShadingRate (combiners %s/%s)", shadingRateName(rate),
                   shadingRateCombinerName(gCtx.shadingRateCombiner0),
                   shadingRateCombinerName(gCtx.shadingRateCombiner1));
    }
    return true;
}

}  // namespace

int setShadingRate(int rate) {
    if (rate >= 0 && !isKnownShadingRate(rate)) {
        dbgLogError("VRS: unknown shading rate 0x%X (expected 0x0/0x1/0x4/0x5/0x6/0x9/0xA, or -1 off)",
                    rate);
        return -1;
    }
    if (rate >= 0 && !gCtx.caps.queried) {
        // 配置是在设备创建之前读到的（NeoForge 的 ModConfigEvent 早于渲染器启动），那一刻还问
        // 不到 tier/additional ⇒ 先收下，等第一次 scope（setShadingRateNow）下发时再判。
        gCtx.requestedShadingRate = rate;
        dbgLogInfo("VRS: shading rate %s requested before the device exists; it will be gated on the first "
                   "scoped draw batch",
                   shadingRateName(rate));
        return 1;
    }
    if (rate >= 0) {
        if (const char* why = shadingRateUnsupportedReason(rate)) {
            // 设备已存在（典型：游戏内改配置触发的 Reloading 事件）⇒ 现在就能给结论，不留半开状态。
            dbgLogWarn("VRS: shading rate %s refused: %s; keeping the default 1X1", shadingRateName(rate), why);
            return 0;
        }
    }
    gCtx.requestedShadingRate = (rate < 0) ? -1 : rate;
    gCtx.shadingRateUnsupportedLogged = false;
    if (gCtx.requestedShadingRate < 0) {
        dbgLogInfo("VRS: shading rate control off; device keeps its default 1X1");
        return 1;
    }
    dbgLogInfo("VRS: shading rate %s set (tier=%d imageTileSize=%u additionalRates=%d) — it is the coarse "
               "rate ceiling for the whitelisted draw batches (P231: no longer replayed per list)",
               shadingRateName(rate), static_cast<int>(gCtx.caps.shadingRateTier),
               gCtx.caps.shadingRateImageTileSize, gCtx.caps.additionalShadingRates ? 1 : 0);
    return 1;
}

int setShadingRateNow(CommandContext* ctx, int rate) {
    // P231：scope 型下发。rate 只有 0x0..0xA 合法 —— 这里不接受 -1，因为"关闭"是 Java 侧
    // 不调用它（不调用就等于列表保持默认 1X1），把 -1 混进来会让调用点多一个分支。
    if (!isKnownShadingRate(rate)) {
        dbgLogError("VRS: setShadingRateNow got unknown rate 0x%X (expected 0x0/0x1/0x4/0x5/0x6/0x9/0xA)",
                    rate);
        return -1;
    }
    if (!ctx || !ctx->commandList || !ctx->listOpen) {
        // 调用点只可能是"主列表已开始录制"的地方；没有打开的列表说明 JVM 侧的 scope 包错了位置。
        dbgLogWarn("VRS: setShadingRateNow(%s) without an open command list; ignored", shadingRateName(rate));
        return -2;
    }
    return issueShadingRate(ctx->commandList, rate) ? 1 : 0;
}

int setShadingRateCombiners(int first, int second) {
    const auto valid = [](int combiner) {
        return combiner >= static_cast<int>(D3D12_SHADING_RATE_COMBINER_PASSTHROUGH) &&
               combiner <= static_cast<int>(D3D12_SHADING_RATE_COMBINER_SUM);
    };
    if (!valid(first) || !valid(second)) {
        dbgLogError("VRS: bad shading-rate combiners %d/%d (expected 0..4, PASSTHROUGH=0 OVERRIDE=1 MIN=2 "
                    "MAX=3 SUM=4)",
                    first, second);
        return -1;
    }
    gCtx.shadingRateCombiner0 = first;
    gCtx.shadingRateCombiner1 = second;
    dbgLogInfo("VRS: combiners set to %s / %s", shadingRateCombinerName(first), shadingRateCombinerName(second));
    return 1;
}

mcdr::Result<void> beginCommandListWithWait(CommandContext* ctx, UINT64 waitForValue) {
    if (!ctx) {
        return mcdr::err("beginCommandListWithWait", "beginCommandListWithWait: null ctx");
    }
    // P6 诊断：转储上一帧累积的验证错误（Close 成功后不打印不代表无错）。
    if (gLogLevel >= 3) dumpInfoQueueMessages();
    DBG_LOG_DEBUG("beginCommandListWithWait: fenceValue=%llu waitValue=%llu",
                  (unsigned long long)ctx->fenceValue, (unsigned long long)waitForValue);
    // P33 async：若 waitForValue > 0，先等 GPU 完成对应提交后再 Reset allocator。
    // 这是对齐官方 Vulkan 双缓冲模式的关键——Vulkan 在 signalSemaphore(idx) 后
    // awaitSubmitCompletion(idx-2) 再 reset command pool，保证 GPU 不再引用。
    // D3D12 等效：Signal(fence, N) → beginCommandList 等 fence=N 完成。
    //
    // P40 fix：改用 per-command-list fence（ctx->fence）而非 queue fence。
    // queue fence 仅表示命令已入队执行，GPU 可能仍在处理；
    // per-command-list fence 在 GPU 真正执行完该命令列表后才 signal，
    // 保证 allocator 不再被 GPU 引用，Reset 不会 hang。
    if (waitForValue > 0) {
        UINT64 cv = 0ULL;
        if (gCtx.queueFence) cv = gCtx.queueFence->GetCompletedValue();
        dbgLogDebug("beginCommandListWithWait: checking qf=%llu cv=%llu qfVal=%llu",
                    static_cast<unsigned long long>(waitForValue), cv,
                    static_cast<unsigned long long>(gCtx.queueFenceValue));
        auto wq = waitForQueueFenceValue(waitForValue, 5'000'000'000ULL);
        if (!wq) {
            dbgLogError("beginCommandListWithWait: wait FAILED: %s", errorMessage(wq.error()).c_str());
            return mcdr::err("beginCommandListWithWait", errorMessage(wq.error()));
        }
        dbgLogDebug("beginCommandListWithWait: wait done");
    }
    // P40：等待 per-command-list fence 完成，确保 GPU 已实际执行完引用该
    // allocator 的所有命令，再 Reset——避免 allocator->Reset() 内部 hang。
    if (waitForValue > 0) {
        auto wl = waitForFenceValue(ctx, waitForValue, 5'000'000'000ULL);
        if (!wl) {
            dbgLogError("beginCommandListWithWait: per-list fence wait FAILED: %s",
                        errorMessage(wl.error()).c_str());
            return mcdr::err("beginCommandListWithWait", errorMessage(wl.error()));
        }
        dbgLogDebug("beginCommandListWithWait: per-list fence wait done val=%llu",
                    static_cast<unsigned long long>(waitForValue));
    }
    dbgLogDebug("beginCommandListWithWait: begin allocator reset");
    HRESULT hr = ctx->currentAllocator()->Reset();
    if (FAILED(hr)) {
        return mcdr::err("beginCommandListWithWait", "beginCommandListWithWait: allocator Reset " + hrText(hr));
    }
    dbgLogDebug("beginCommandListWithWait: allocator reset ok");
    dbgLogDebug("beginCommandListWithWait: begin list reset");
    hr = ctx->commandList->Reset(ctx->currentAllocator().Get(), nullptr);
    if (FAILED(hr)) {
        return mcdr::err("beginCommandListWithWait", "beginCommandListWithWait: list Reset " + hrText(hr));
    }
    dbgLogDebug("beginCommandListWithWait: list reset ok");
    // P231：这里**不再**重放玩家档位。Reset 之后列表就是 D3D12 默认的 1X1，只有 Java 侧的
    // scope（setShadingRateNow）才会把某一批绘制改粗 —— 否则天空、粒子、GUI 会跟着一起变粗
    // （P97 的行为，P232 修掉）。
    dbgLogDebug("beginCommandListWithWait: pre-increment ctx=%p", reinterpret_cast<void*>(ctx));
    ctx->listOpen = 1;
    ctx->inRenderPass = 0;
    ctx->colorTargetsWritten = false;  // 新 command list 从零开始追踪绘制状态
    ++gOpenListCount;  // 延迟销毁：登记打开计数，submit 完成前不释放资源
    dbgLogDebug("beginCommandListWithWait: post-increment openList=%d ctx=%p", gOpenListCount,
                reinterpret_cast<void*>(ctx));
    // submit 阻塞等待 GPU 完成（见 submitCommandList），此处清空 resourceState
    // 后一切资源视为初始态是正确且保守的（D3D12 驱动会按实际 GPU 状态纠正）。
    ctx->resourceState.clear();
    // P20 fix：使用 4 个半区（ring x4）确保三帧飞行时安全交替。
    // fenceValue % 4 保证帧 N+2 写入的半区 ≠ 帧 N/N+1 GPU 正在读的半区。
    ctx->drawHeapSlotBase = static_cast<UINT>(ctx->fenceValue % kDrawHeapSections) * kDrawHeapPerFrame;
    ctx->nextDrawSlot = 0;
    // P243：新列表 ⇒ 段内槽位从头分配，上一列表的 SRV 复用缓存作废。
    ctx->lastSrvSlotOffset = 0;
    ctx->lastSrvCount = 0;
    ctx->lastSrvViews.clear();
    // RTV/DSV 是 CPU-only 描述符堆：命令列表提交时驱动已捕获描述符内容，
    // 双 allocator + value-2 完成等待保证旧命令已结束，可每帧从 0 复用。
    // P24：仅帧级瞬态堆（frameRtvHeap/dsvHeap）每帧清零；rtvHeap 上的
    // backbuffer RTV 由 allocRtvHandle 持久分配，绝不能覆盖（曾致 blit 把
    // 颜色纹理当 RT 绑定 → 黑屏）。
    gNextFrameRtv = 0;
    gNextDsv = 0;
    // 游标倒回 ⇒ 所有 ctx 的 (资源,mip) -> 句柄缓存失效（见 gFrameViewGeneration 的说明）。
    ++gFrameViewGeneration;
    // P64：帧级 RTV/DSV 句柄随堆槽位清零而失效；setPipeline 快速路径同理（新 list 无
    // 已绑定的 PSO/根签名）。必须与本 list 的生命周期严格一致。
    ctx->rtvCache.clear();
    ctx->dsvCache.clear();
    ctx->cachedPso = nullptr;
    ID3D12DescriptorHeap* drawHeaps[] = {gCtx.drawHeap.Get()};
    ctx->commandList->SetDescriptorHeaps(1, drawHeaps);
    return {};
}

void restoreDrawHeap(CommandContext* ctx) {
    if (ctx == nullptr || ctx->commandList == nullptr || gCtx.drawHeap == nullptr) {
        return;
    }
    ID3D12DescriptorHeap* drawHeaps[] = {gCtx.drawHeap.Get()};
    ctx->commandList->SetDescriptorHeaps(1, drawHeaps);
}

mcdr::Result<void> endCommandList(CommandContext* ctx) {
    if (!ctx) {
        return mcdr::err("endCommandList", "endCommandList: null ctx");
    }
    if (!ctx->listOpen) return {};  // 幂等
    // 采样纹理在本 list 内被 transition 到 SHADER_RESOURCE 后，命令列表结束时
    // 必须回切 COMMON，保持"列表结束一切资源回 COMMON"的约定。否则下一
    // command list 在 beginCommandList 清空 resourceState 后按 COMMON 写 Before
    // 状态，与 GPU 上遗留的 SHADER_RESOURCE 错配 → 每帧验证 ERROR
    // （"Before state ... does not match ... preceding ResourceBarrier"）。
    for (auto it = ctx->resourceState.begin(); it != ctx->resourceState.end();) {
        if (it->second
            & (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
            if (it->second != D3D12_RESOURCE_STATE_COMMON)
                resourceBarrier(ctx->commandList, it->first, it->second, D3D12_RESOURCE_STATE_COMMON);
            it = ctx->resourceState.erase(it);
        } else {
            ++it;
        }
    }
    // copy 操作（copyBufferToBuffer 等）会把源/目的缓冲区留在 COPY_SOURCE /
    // COPY_DEST；若不清理，下一帧 beginCommandList 清空 resourceState 后
    // 按 INITIAL 写 Before，GPU 实际仍在 COPY_SOURCE/COPY_DEST → 验证 ERROR。
    //
    // UPLOAD heap staging buffer（COPY_SOURCE）必须回切 GENERIC_READ（初始态），
    // 因为 beginCommandList 以 GENERIC_READ 为假设值。其他 COPY_SOURCE/COPY_DEST
    // 资源回切 COMMON。
    for (auto it = ctx->resourceState.begin(); it != ctx->resourceState.end();) {
        D3D12_RESOURCE_STATES target = D3D12_RESOURCE_STATE_COMMON;
        if (it->second == D3D12_RESOURCE_STATE_COPY_SOURCE
            && ctx->stagingCopySourceResources.count(it->first)) {
            target = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
        if (it->second == target) {
            it = ctx->resourceState.erase(it);
        } else {
            resourceBarrier(ctx->commandList, it->first, it->second, target);
            it = ctx->resourceState.erase(it);
        }
    }
    ctx->stagingCopySourceResources.clear();
    HRESULT hr = ctx->commandList->Close();
    if (FAILED(hr)) {
        // P6 诊断：Close 返回 E_INVALIDARG 通常是 debug layer 的验证错误
        // （UPLOAD heap 非法 transition、未绑定/越界描述符、root 参数未设置、
        // PSO 与 root signature 不匹配等）。附加 InfoQueue 最近消息定位具体
        // 非法调用——这是 Close 失败时唯一的确定性证据。
        // P277：原来只在 deviceStatusText() 里带一行设备状态，而 0x80070057 这种
        // **非设备移除**的失败根本不会出现在那里（GetDeviceRemovedReason=0）。调试层
        // 开着时必须把 InfoQueue 的原文打出来，否则这条日志没有信息量。
        dumpInfoQueueMessages();
        return mcdr::err("endCommandList",
                         "endCommandList: Close " + hrText(hr) + " — " + deviceStatusText());
    }
    ctx->listOpen = 0;
    // P44 fix：gOpenListCount 必须与 listOpen 严格一一对应。
    // 此前 endCommandList 只置 listOpen=0 而不递减计数，导致一次性
    // （device==null）encoder 的 close() 路径（先 end 后 destroy）永远
    // 泄漏计数：destroy 时 listOpen 已为 0，不再递减 → 计数单调上涨 →
    // flushPendingDeletes 饥饿 → 加载末期一次性释放大批对象时崩溃。
    // 现在统一由“listOpen 1→0”的唯一收口点递减；submit / destroy / 渲染
    // 线程 abort 路径不再重复递减。注意此处**不**触发 flush——延迟删除
    // 必须等到渲染线程 present fence 之后（见 renderThread post-present）。
    if (gOpenListCount > 0) --gOpenListCount;
    dbgLogDebug("endCommandList: openList now=%d ctx=%p", gOpenListCount, reinterpret_cast<void*>(ctx));
    return {};
}

// P199c：内存分类采样。P199b 的对象计数全平但进程内存仍持续上涨，所以必须把
// “进程私有内存”和 WDDM 的 Local（显存）/NonLocal（共享内存、上传堆）分开看：
// Local 涨 → 真显存资源泄漏；NonLocal 涨 → CPU 可见的上传/读回堆或驱动 staging。
static void logMemoryClass(uint64_t frame) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return;
    }
    const unsigned long long privMB = static_cast<unsigned long long>(pmc.PrivateUsage) >> 20;
    const unsigned long long wsMB = static_cast<unsigned long long>(pmc.WorkingSetSize) >> 20;
    DXGI_QUERY_VIDEO_MEMORY_INFO localInfo{};
    DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalInfo{};
    bool haveVid = false;
    if (gCtx.adapter) {
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(gCtx.adapter.As(&adapter3)) && adapter3) {
            haveVid =
                SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo)) &&
                SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalInfo));
        }
    }
    if (haveVid) {
        dbgLogInfo("P199c frame=%llu private=%lluMB ws=%lluMB local=%lluMB/%lluMB nonLocal=%lluMB/%lluMB liveTex=%lld liveBuf=%lld bufTotal=%llu upMB=%lld defMB=%lld rbMB=%lld pd=%lld",
                   static_cast<unsigned long long>(frame), privMB, wsMB,
                   static_cast<unsigned long long>(localInfo.CurrentUsage >> 20),
                   static_cast<unsigned long long>(localInfo.Budget >> 20),
                   static_cast<unsigned long long>(nonLocalInfo.CurrentUsage >> 20),
                   static_cast<unsigned long long>(nonLocalInfo.Budget >> 20),
                   static_cast<long long>(gLiveTex.load()), static_cast<long long>(gLiveBuf.load()),
                   static_cast<unsigned long long>(gStatBufCreate.load()),
                   static_cast<long long>(gLiveUploadBytes.load() >> 20),
                   static_cast<long long>(gLiveDefaultBytes.load() >> 20),
                   static_cast<long long>(gLiveReadbackBytes.load() >> 20),
                   static_cast<long long>(pendingDeletesCount()));
    } else {
        dbgLogInfo("P199c frame=%llu private=%lluMB ws=%lluMB (video memory query unavailable)",
                   static_cast<unsigned long long>(frame), privMB, wsMB);
    }
}
mcdr::Result<UINT64> submitCommandList(CommandContext* ctx) {
    if (!ctx) {
        return mcdr::err("submitCommandList", "submitCommandList: null ctx");
    }
    // P33 fix：命令列表未打开说明本帧没有任何录制内容。此时若仍执行
    // ExecuteCommandLists，会把**上一条已提交的命令列表**再执行一遍
    // （D3D12 中重复执行同一条 list 是合法的，但会重复写入同一批资源）。
    // 这里按“空帧”处理：不执行，但仍返回本 ctx 已经 Signal 过的 fence 值，让调用方继续。
    if (!ctx->listOpen) {
        dbgLogInfo("submitCommandList: no open list — skip execute (empty frame) ctx=%p",
               reinterpret_cast<void*>(ctx));
        // BUG-03：返回本 ctx 已 Signal 的最新值，且**不递增**——没有内容要提交就没有新的
        // per-ctx 序号，之前 createFence() 生成的 token（= 该值 + 1）仍然挂着，要等这个
        // ctx 的下一次真正提交才完成（与官方「共享 encoder 的下一个 submit index」一致）。
        return ctx->fenceValue;
    }
    if (auto e = endCommandList(ctx); !e) {
        return mcdr::err("submitCommandList", errorMessage(e.error()));
    }
    UINT64 value = ctx->fenceValue + 1;
    // P15 诊断：每 30 帧打印 submit 摘要（含 fence 值 + queueFence）
    if ((value % 30ULL) == 1) {
        dbgLogInfo("submitCommandList: frame=%llu ctx=%p queueFence=%llu | P199b pending=%lld "
                   "srvSlot=%d samplerSlot=%d openLists=%d tex=%llu buf=%llu pso=%llu enc=%llu",
                   static_cast<unsigned long long>(value), reinterpret_cast<void*>(ctx),
                   static_cast<unsigned long long>(gCtx.queueFenceValue),
                   static_cast<long long>(pendingDeletesCount()), static_cast<int>(gNextSrv),
                   static_cast<int>(gNextSampler), static_cast<int>(gOpenListCount),
                   static_cast<unsigned long long>(gStatTexCreate.load()),
                   static_cast<unsigned long long>(gStatBufCreate.load()),
                   static_cast<unsigned long long>(gStatPsoCreate.load()),
                   static_cast<unsigned long long>(gStatEncoderCreate.load()));
    }
    if ((value % 300ULL) == 1) {
        logMemoryClass(value);
    }
    DBG_LOG_DEBUG("submit: ExecuteCommandLists enter (v->%llu)", (unsigned long long)value);
    ID3D12CommandList* lists[] = {ctx->commandList};
    gCtx.queue->ExecuteCommandLists(1, lists);
    value = ++ctx->fenceValue;
    // P55：记录本槽位（currentListSlot）最后一次提交的 per-list fence，供 3 帧后
    // 的 beginCommandList 等待该槽位 GPU 执行完成后再 Reset 其 list/allocator。
    ctx->slotFence[ctx->currentListSlot] = value;
    DBG_LOG_DEBUG("submit: executed, Signal v=%llu", (unsigned long long)value);
    if (FAILED(gCtx.queue->Signal(ctx->fence.Get(), value))) {
        return mcdr::err("submitCommandList", "submitCommandList: Signal failed");
    }
    // 全局队列 fence 同步推进：只服务于 surface / present 记账（surfaceFences、
    // lastSubmitQueueFence、异步线程的 present fence）。它**不再**是 GpuFence token
    // 的判据——token 走本 ctx 的 per-command-list fence（见函数末尾的 return value），
    // 否则一次临时上传（createBuffer(data) 的一次性 encoder）就能提前满足帧 fence。
    {
        UINT64 qv = ++gCtx.queueFenceValue;
        if (FAILED(gCtx.queue->Signal(gCtx.queueFence.Get(), qv))) {
            return mcdr::err("submitCommandList", "submitCommandList: Signal(queue) failed");
        }
        // P33 async：记录本次 submit 的 queue fence 值，供 beginCommandListWithWait
        // 等待。下一帧 begin 时传入此值，确保 GPU 完成当前帧后才 Reset allocator。
        ctx->lastSubmitQueueFence = qv;
        // P40：记录 per-command-list fence 值，确保 GPU 实际执行完引用该
        // allocator 的所有命令后再 Reset——这是避免 hang 的关键同步点。
        ctx->prevFenceValue = value;
        dbgLogDebug("renderThread: submitCommandList done ctx=%p value=%llu qf=%llu lastQF=%llu",
                    reinterpret_cast<void*>(ctx), static_cast<unsigned long long>(value),
                    static_cast<unsigned long long>(gCtx.queueFenceValue),
                    static_cast<unsigned long long>(ctx->lastSubmitQueueFence));
    }
    // P18：记录 per-backbuffer fence 值，供 acquireSurface 按需同步（非阻塞）。
    // submit 本身不等待 GPU，改为在 acquireSurface 中检查重用的 back buffer
    // 是否仍被 GPU 使用（上次 blit 未完成时等待），避免渲染线程阻塞导致鼠标卡顿。
    Dx12Surface* s = getActiveSurface();
    if (s) {
        int idx = s->currentImageIndex;
        if (idx >= 0 && idx < static_cast<int>(kSurfaceBufferCount)) {
            if (s->surfaceFences.size() < static_cast<size_t>(kSurfaceBufferCount))
                s->surfaceFences.resize(kSurfaceBufferCount, 0);
            s->surfaceFences[static_cast<size_t>(idx)] = gCtx.queueFenceValue;
        }
    }
    DBG_LOG_DEBUG("submit: done v=%llu qf=%llu", (unsigned long long)value,
                  (unsigned long long)gCtx.queueFenceValue);
    // 提交并同步等待完成：本命令列表已执行完，其引用的资源可安全释放。
    // 注意：flushPendingDeletes() 不在 submit 时调用，而是延迟到渲染线程
    // presentSurface + present fence signal 之后执行，确保 GPU 工作（含 display
    // controller flip）完成后才释放延迟删除对象。
    // P44 fix：openList 的递减已统一由 endCommandList（listOpen 1→0 的唯一
    // 收口点）完成，此处不再重复递减（否则会双重递减导致计数提前归零）。
    // BUG-03：返回本 ctx 的 per-command-list 序号（不是全局 queueFenceValue）。
    // mcdr_dx12Submit 的返回值、createFence() 的 token、mcdr_dx12WaitForFence 的
    // 目标值属于同一条 per-ctx 时间线，因此一次性 encoder 的提交不会推进帧 encoder 的 token。
    return value;
}

mcdr::Result<void> waitForFenceValue(CommandContext* ctx, UINT64 value, UINT64 timeoutNs) {
    if (!ctx) {
        return mcdr::err("waitForFenceValue", "waitForFenceValue: null ctx");
    }
    UINT64 cv = ctx->fence->GetCompletedValue();
    DBG_LOG_DEBUG("waitFence: value=%llu completed=%llu", (unsigned long long)value, (unsigned long long)cv);
    if (cv >= value) return {};
    HRESULT hr = ctx->fence->SetEventOnCompletion(value, ctx->fenceEvent);
    if (FAILED(hr)) {
        return mcdr::err("waitForFenceValue", "waitForFenceValue: SetEventOnCompletion " + hrText(hr));
    }
    DWORD ms = static_cast<DWORD>((timeoutNs + 999999ULL) / 1000000ULL);
    if (WaitForSingleObject(ctx->fenceEvent, ms) != WAIT_OBJECT_0) {
        dbgLogError("waitFence: TIMEOUT value=%llu completed=%llu", static_cast<unsigned long long>(value),
               static_cast<unsigned long long>(ctx->fence->GetCompletedValue()));
        return mcdr::err("waitForFenceValue",
                         "waitForFenceValue: timed out after " + std::to_string(timeoutNs) + "ns");
    }
    dbgLogDebug("waitFence: OK value=%llu", static_cast<unsigned long long>(value));
    return {};
}

UINT64 currentFenceValue(CommandContext* ctx) { return ctx ? ctx->fenceValue : 0; }

// 全局队列 fence 等待：等待对象是设备级 queueFence，仅供 native 内部记账
// （渲染线程 present 之后、自测路径）。Kotlin 的 GpuFence token 现在走
// waitForContextFenceValue（BUG-03），不再使用这条全局序列。
// P60：改为每线程复用一个 auto-reset 事件（原实现每次调用 CreateEventW +
// CloseHandle；该路径由 StagedVertexBuffer 每帧大量轮询触发，是 dx12WaitForFence
// 热度来源）。因同一事件上可能残留更早（更低 fence 值）的 SetEventOnCompletion
// 注册，等待返回后必须回读 GetCompletedValue 复核，避免把"更早的值到达"误判为
// 本次目标已完成（否则会提前回收在飞 buffer）。auto-reset 事件在每次
// WaitForSingleObject 消费后自动复位，故复核失败可安全重试。
mcdr::Result<void> waitForQueueFenceValue(UINT64 value, UINT64 timeoutNs) {
    if (!gCtx.queueFence) {
        return mcdr::err("waitForQueueFenceValue", "waitForQueueFenceValue: queue fence not initialized");
    }
    UINT64 cv = gCtx.queueFence->GetCompletedValue();
    dbgLogDebug("waitQFence: value=%llu completed=%llu", static_cast<unsigned long long>(value),
                static_cast<unsigned long long>(cv));
    if (cv >= value) return {};
    // P60 快路径：零超时 = 纯轮询（官方 GpuFence.awaitCompletion(0) 语义，官方
    // StagedVertexBuffer 回收 buffer 依赖它）。已确认未完成，直接返回 false，
    // 不创建事件对象——原路径每次都会 CreateEventW/SetEventOnCompletion/
    // WaitForSingleObject(0)/CloseHandle。
    if (timeoutNs == 0) {
        return mcdr::err("waitForQueueFenceValue", "waitForQueueFenceValue: timed out after 0ns");
    }
    static thread_local HANDLE t_queueFenceEvent = nullptr;
    if (!t_queueFenceEvent) {
        t_queueFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!t_queueFenceEvent) {
            return mcdr::err("waitForQueueFenceValue", "waitForQueueFenceValue: CreateEvent failed");
        }
    }
    // 上限钳制：Java awaitCompletion(Long.MAX_VALUE) 换算后的纳秒数溢出 DWORD，
    // 直接 cast 会得到很小的毫秒数（把"永久等待"变成瞬时轮询）。钳到 ~49.7 天。
    UINT64 ms64 = (timeoutNs + 999999ULL) / 1000000ULL;
    DWORD ms = ms64 > 0xFFFFFFF0ULL ? 0xFFFFFFF0ULL : static_cast<DWORD>(ms64);
    for (;;) {
        HRESULT hr = gCtx.queueFence->SetEventOnCompletion(value, t_queueFenceEvent);
        if (FAILED(hr)) {
            return mcdr::err("waitForQueueFenceValue",
                             "waitForQueueFenceValue: SetEventOnCompletion " + hrText(hr));
        }
        DWORD wr = WaitForSingleObject(t_queueFenceEvent, ms);
        // 回读复核：事件可能被同一事件上更早的注册误触发。
        if (gCtx.queueFence->GetCompletedValue() >= value) {
            DBG_LOG_DEBUG("waitQFence: OK value=%llu", (unsigned long long)value);
            return {};
        }
        if (wr != WAIT_OBJECT_0) {
            dbgLogError("waitQFence: TIMEOUT value=%llu completed=%llu", static_cast<unsigned long long>(value),
                   static_cast<unsigned long long>(gCtx.queueFence->GetCompletedValue()));
            return mcdr::err("waitForQueueFenceValue",
                             "waitForQueueFenceValue: timed out after " + std::to_string(timeoutNs) + "ns");
        }
        // 被更早注册误触发：继续等待本次目标值。
    }
}

// per-ctx fence 等待（Kotlin GpuFence.awaitCompletion 的落点，BUG-03）：
// 等待对象是**这个 ctx 自己的** per-command-list fence，目标值来自
// currentFenceValue() / submitCommandList()，只由该 ctx 自己的提交推进。
// 这样帧 encoder 的 token 不会被一次性 encoder（createBuffer(data) 的上传）
// 提前满足，StagedVertexBuffer / MappableRingBuffer 的回收时机才正确。
// 与 waitForQueueFenceValue 一样用每线程 auto-reset 事件 + 回读复核：同一事件上
// 可能残留更低值的注册，事件触发后必须回读 GetCompletedValue 再判定。
mcdr::Result<void> waitForContextFenceValue(CommandContext* ctx, UINT64 value, UINT64 timeoutNs) {
    if (!ctx || !ctx->fence) {
        return mcdr::err("waitForContextFenceValue", "waitForContextFenceValue: null ctx or fence");
    }
    UINT64 cv = ctx->fence->GetCompletedValue();
    dbgLogDebug("waitCtxFence: value=%llu completed=%llu ctx=%p", static_cast<unsigned long long>(value),
                static_cast<unsigned long long>(cv), reinterpret_cast<void*>(ctx));
    if (cv >= value) return {};
    if (timeoutNs == 0) {
        return mcdr::err("waitForContextFenceValue", "waitForContextFenceValue: timed out after 0ns");
    }
    static thread_local HANDLE t_ctxFenceEvent = nullptr;
    if (!t_ctxFenceEvent) {
        t_ctxFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!t_ctxFenceEvent) {
            return mcdr::err("waitForContextFenceValue", "waitForContextFenceValue: CreateEvent failed");
        }
    }
    UINT64 ms64 = (timeoutNs + 999999ULL) / 1000000ULL;
    DWORD ms = ms64 > 0xFFFFFFF0ULL ? 0xFFFFFFF0ULL : static_cast<DWORD>(ms64);
    for (;;) {
        HRESULT hr = ctx->fence->SetEventOnCompletion(value, t_ctxFenceEvent);
        if (FAILED(hr)) {
            return mcdr::err("waitForContextFenceValue",
                             "waitForContextFenceValue: SetEventOnCompletion " + hrText(hr));
        }
        DWORD wr = WaitForSingleObject(t_ctxFenceEvent, ms);
        if (ctx->fence->GetCompletedValue() >= value) {
            DBG_LOG_DEBUG("waitCtxFence: OK value=%llu", (unsigned long long)value);
            return {};
        }
        if (wr != WAIT_OBJECT_0) {
            dbgLogError("waitCtxFence: TIMEOUT value=%llu completed=%llu ctx=%p",
                        static_cast<unsigned long long>(value),
                        static_cast<unsigned long long>(ctx->fence->GetCompletedValue()),
                        reinterpret_cast<void*>(ctx));
            return mcdr::err("waitForContextFenceValue",
                             "waitForContextFenceValue: timed out after " + std::to_string(timeoutNs) + "ns");
        }
    }
}

UINT64 currentQueueFenceValue() { return gCtx.queueFenceValue; }

// 读回单个 timestamp 的通用实现：录制 EndQuery + ResolveQueryData 到 READBACK
// buffer，提交并等待，然后 map 读。list 在调用前必须已 Close。
static mcdr::Result<void> resolveQuery(QueryPool* pool, int start, int count, long long* out) {
    if (!pool) {
        return mcdr::err("resolveQuery", "resolveQuery: null pool");
    }
    if (start < 0 || count <= 0 || start + count > pool->size) {
        return mcdr::err("resolveQuery", "resolveQuery: invalid range");
    }
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(gCtx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
        || FAILED(gCtx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                 IID_PPV_ARGS(&list)))) {
        return mcdr::err("resolveQuery", "resolveQuery: create command list failed");
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(count) * sizeof(long long);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readback)))) {
        return mcdr::err("resolveQuery", "resolveQuery: readback alloc failed");
    }
    list->ResolveQueryData(pool->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(start),
                           static_cast<UINT>(count), readback.Get(), 0);
    list->Close();
    if (auto f = flushAndWait(list.Get(), "resolveQuery"); !f) {
        return mcdr::err("resolveQuery", errorMessage(f.error()));
    }
    void* p = nullptr;
    readback->Map(0, nullptr, &p);
    memcpy(out, p, static_cast<size_t>(count) * sizeof(long long));
    readback->Unmap(0, nullptr);
    return {};
}

mcdr::Result<long long> getTimestampNow(CommandContext* ctx) {
    (void)ctx;
    if (auto d = ensureDevice(); !d) return mcdr::err("getTimestampNow", errorMessage(d.error()));
    auto poolRes = createQueryPool(1);
    if (!poolRes) return mcdr::err("getTimestampNow", errorMessage(poolRes.error()));
    QueryPool* pool = static_cast<QueryPool*>(*poolRes);
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(gCtx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
        || FAILED(gCtx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                 IID_PPV_ARGS(&list)))) {
        destroyQueryPool(pool);
        return mcdr::err("getTimestampNow", "getTimestampNow: create command list failed");
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(long long);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readback)))) {
        destroyQueryPool(pool);
        return mcdr::err("getTimestampNow", "getTimestampNow: readback alloc failed");
    }
    list->EndQuery(pool->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    list->ResolveQueryData(pool->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, readback.Get(), 0);
    list->Close();
    if (auto f = flushAndWait(list.Get(), "getTimestampNow"); !f) {
        destroyQueryPool(pool);
        return mcdr::err("getTimestampNow", errorMessage(f.error()));
    }
    long long ts = 0;
    void* p = nullptr;
    readback->Map(0, nullptr, &p);
    memcpy(&ts, p, sizeof(ts));
    readback->Unmap(0, nullptr);
    destroyQueryPool(pool);
    return ts;
}

// ---------------------------------------------------------------------------
// 命令录制（P3）：copy / clear / render pass / timestamp
// ---------------------------------------------------------------------------

mcdr::Result<void> copyBufferToBuffer(CommandContext* ctx, Dx12Object* src, long long srcOffset,
                                      Dx12Object* dst, long long dstOffset, long long size) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("copyBufferToBuffer", "copyBufferToBuffer: no open command list");
    }
    if (!src || !dst || src->kind != Dx12Object::Kind::Buffer || dst->kind != Dx12Object::Kind::Buffer) {
        return mcdr::err("copyBufferToBuffer", "copyBufferToBuffer: invalid buffer handle");
    }
    if (srcOffset < 0 || dstOffset < 0 || size < 0 || srcOffset + size > src->size
        || dstOffset + size > dst->size) {
        return mcdr::err("copyBufferToBuffer", "copyBufferToBuffer: range out of bounds");
    }
    // P56：移除 P6/P23 遗留的两段 UPLOAD 源诊断（每次拷贝都 Map + 扫描/格式化 + Unmap）。
    // 它们不受日志级别保护、在 Release 中依然无条件执行，而顶点/UBO 上传正是每帧高频
    // 路径（StagedVertexBuffer / GlobalSettingsUniform / UberGpuBuffer），曾把 dx12CopyBuffer
    // 抬到 P53 热点约 12%。语义不变：CopyBufferRegion 本就不改写源数据。
    transitionBufferTo(ctx, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionBufferTo(ctx, dst, D3D12_RESOURCE_STATE_COPY_DEST);
    ctx->commandList->CopyBufferRegion(dst->resource.Get(), static_cast<UINT64>(dstOffset),
                                       src->resource.Get(), static_cast<UINT64>(srcOffset),
                                       static_cast<UINT64>(size));
    return {};
}

// 构建绑定到指定 mip slice 的 RTV 描述符。原实现用 nullptr desc → 恒绑 mip0，
// 官方 TextureAtlas 图集上传对每个 mipViews[level] 发起 render pass（逐级写入
// mip1..N），mip0 以外的层级从未被写入 → 远处/大 LOD 采样到未初始化内容
// （黑色远区块 / 个别方块呈 4px 粗颗粒模糊）。2D array / cubemap 保持整 array
// 语义（与 nullptr desc 一致），仅修正 mip slice。
static D3D12_RENDER_TARGET_VIEW_DESC makeRtvDesc(Dx12Object* tex, int mip) {
    D3D12_RENDER_TARGET_VIEW_DESC d{};
    d.Format = tex->dxgiFormat;
    auto rd = tex->resource->GetDesc();
    int mips = static_cast<int>(rd.MipLevels);
    UINT mipU = static_cast<UINT>(std::max(0, std::min(mip, mips - 1)));
    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.DepthOrArraySize > 1) {
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.MipSlice = mipU;
        d.Texture2DArray.FirstArraySlice = 0;
        d.Texture2DArray.ArraySize = rd.DepthOrArraySize;
        d.Texture2DArray.PlaneSlice = 0;
    } else {
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = mipU;
        d.Texture2D.PlaneSlice = 0;
    }
    return d;
}

// 构建绑定到指定 mip slice 的 DSV 描述符（用途同 makeRtvDesc）。
static D3D12_DEPTH_STENCIL_VIEW_DESC makeDsvDesc(Dx12Object* tex, int mip) {
    D3D12_DEPTH_STENCIL_VIEW_DESC d{};
    d.Format = tex->dxgiFormat;
    auto rd = tex->resource->GetDesc();
    int mips = static_cast<int>(rd.MipLevels);
    UINT mipU = static_cast<UINT>(std::max(0, std::min(mip, mips - 1)));
    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.DepthOrArraySize > 1) {
        d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.MipSlice = mipU;
        d.Texture2DArray.FirstArraySlice = 0;
        d.Texture2DArray.ArraySize = rd.DepthOrArraySize;
    } else {
        d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = mipU;
    }
    return d;
}

// P64：取（或创建）(资源, mip) 对应的帧级瞬态 RTV。同一命令列表内复用同一 CPU 句柄，
// 免去重复的 CreateRenderTargetView 驱动调用。句柄有效性由 beginCommandList 中
// gNextFrameRtv 清零 + rtvCache.clear() 同步保证（两者必须同点发生）。
static mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> frameRtvFor(CommandContext* ctx, Dx12Object* tex,
                                                             int mip) {
    // P109（#538 根因修复）：描述符堆游标被倒回过（任何 ctx 的 beginCommandList 都会），
    // 本 ctx 缓存里的句柄可能已被覆盖成另一张资源的视图 —— 按代整体失效，绝不跨代复用。
    if (ctx->frameViewGeneration != gFrameViewGeneration) {
        ctx->rtvCache.clear();
        ctx->dsvCache.clear();
        ctx->frameViewGeneration = gFrameViewGeneration;
    }
    RtvDsvKey key{tex->resource.Get(), static_cast<UINT>(mip)};
    auto it = ctx->rtvCache.find(key);
    if (it != ctx->rtvCache.end()) {
        return it->second;
    }
    if (gNextFrameRtv >= kFrameRtvHeapSize) {
        return mcdr::err("frameRtvFor", "frameRtvFor: frame rtv heap exhausted");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = gCtx.frameRtvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(gNextFrameRtv) * gCtx.rtvInc;
    ++gNextFrameRtv;
    D3D12_RENDER_TARGET_VIEW_DESC desc = makeRtvDesc(tex, mip);
    gCtx.device->CreateRenderTargetView(tex->resource.Get(), &desc, cpu);
    ctx->rtvCache.emplace(key, cpu);
    return cpu;
}

// P106：frameRtvFor 是文件内 static，纹理 → 纹理 blit（在 d3d12_surface.cpp 里）拿不到，
// 这里开一个薄包装出去。
mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> acquireTextureRtv(CommandContext* ctx, Dx12Object* tex) {
    if (!ctx || !tex || !tex->resource) {
        return mcdr::err("acquireTextureRtv", "acquireTextureRtv: null ctx/texture/resource");
    }
    return frameRtvFor(ctx, tex, 0);
}

// P64：DSV 版（语义同上，句柄来自 dsvHeap）。
static mcdr::Result<D3D12_CPU_DESCRIPTOR_HANDLE> frameDsvFor(CommandContext* ctx, Dx12Object* tex,
                                                             int mip) {
    // P109（#538 根因修复）：描述符堆游标被倒回过（任何 ctx 的 beginCommandList 都会），
    // 本 ctx 缓存里的句柄可能已被覆盖成另一张资源的视图 —— 按代整体失效，绝不跨代复用。
    if (ctx->frameViewGeneration != gFrameViewGeneration) {
        ctx->rtvCache.clear();
        ctx->dsvCache.clear();
        ctx->frameViewGeneration = gFrameViewGeneration;
    }
    RtvDsvKey key{tex->resource.Get(), static_cast<UINT>(mip)};
    auto it = ctx->dsvCache.find(key);
    if (it != ctx->dsvCache.end()) {
        return it->second;
    }
    if (gNextDsv >= kDsvHeapSize) {
        return mcdr::err("frameDsvFor", "frameDsvFor: dsv heap exhausted");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = gCtx.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(gNextDsv) * gCtx.dsvInc;
    ++gNextDsv;
    D3D12_DEPTH_STENCIL_VIEW_DESC desc = makeDsvDesc(tex, mip);
    gCtx.device->CreateDepthStencilView(tex->resource.Get(), &desc, cpu);
    ctx->dsvCache.emplace(key, cpu);
    return cpu;
}

mcdr::Result<void> clearColorTexture(CommandContext* ctx, Dx12Object* tex, float r, float g, float b,
                                     float a) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("clearColorTexture", "clearColorTexture: no open command list");
    }
    if (!tex || tex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("clearColorTexture", "clearColorTexture: invalid texture");
    }
    if (!(tex->usage & 8)) {  // RENDER_ATTACHMENT
        return mcdr::err("clearColorTexture", "clearColorTexture: texture lacks RENDER_ATTACHMENT");
    }
    // P24：帧级瞬态 RTV（frameRtvHeap），每帧从 0 复用，不与 backbuffer RTV 冲突。
    // P64：同一命令列表内 (资源, mip=0) 的 RTV 走缓存，只创建一次。
    auto rtvRes = frameRtvFor(ctx, tex, 0);
    if (!rtvRes) return mcdr::err("clearColorTexture", errorMessage(rtvRes.error()));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = *rtvRes;
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float color[4] = {r, g, b, a};
    ctx->commandList->ClearRenderTargetView(cpu, color, 0, nullptr);
    // P64：不再回切 COMMON。若随后 beginRenderPass 以同一纹理为附件，可省去
    // 一次冗余过渡。
    return {};
}

mcdr::Result<void> clearDepthTexture(CommandContext* ctx, Dx12Object* tex, double depth) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("clearDepthTexture", "clearDepthTexture: no open command list");
    }
    if (!tex || tex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("clearDepthTexture", "clearDepthTexture: invalid texture");
    }
    auto dsvRes = frameDsvFor(ctx, tex, 0);
    if (!dsvRes) return mcdr::err("clearDepthTexture", errorMessage(dsvRes.error()));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = *dsvRes;
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ctx->commandList->ClearDepthStencilView(cpu, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                            static_cast<FLOAT>(depth), 0, 0, nullptr);
    // P64：不再回切 COMMON（同 clearColorTexture：后续 beginRenderPass 复用 DEPTH_WRITE，
    // 彻底未用的由 endCommandList 收尾回切）。
    return {};
}

mcdr::Result<void> clearStencilTexture(CommandContext* ctx, Dx12Object* tex, int value) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("clearStencilTexture", "clearStencilTexture: no open command list");
    }
    if (!tex || tex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("clearStencilTexture", "clearStencilTexture: invalid texture");
    }
    auto dsvRes = frameDsvFor(ctx, tex, 0);
    if (!dsvRes) return mcdr::err("clearStencilTexture", errorMessage(dsvRes.error()));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = *dsvRes;
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    // 只带 STENCIL flag：depth 参数被 D3D12 忽略，传 1.0f 表明"深度位未参与"。
    ctx->commandList->ClearDepthStencilView(cpu, D3D12_CLEAR_FLAG_STENCIL, 1.0f, static_cast<UINT8>(value), 0,
                                            nullptr);
    // 与 clearDepthTexture 一致：不回切 COMMON，交由 endCommandList 收尾。
    return {};
}

mcdr::Result<void> clearColorTextureRegion(CommandContext* ctx, Dx12Object* tex, float r, float g, float b,
                                           float a, int x, int y, int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("clearColorTextureRegion", "clearColorTextureRegion: no open command list");
    }
    if (!tex || tex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("clearColorTextureRegion", "clearColorTextureRegion: invalid texture");
    }
    if (!(tex->usage & 8)) {  // RENDER_ATTACHMENT
        return mcdr::err("clearColorTextureRegion",
                         "clearColorTextureRegion: texture lacks RENDER_ATTACHMENT");
    }
    if (w <= 0 || h <= 0) return {};  // 空区域：无事可做
    auto rtvRes = frameRtvFor(ctx, tex, 0);
    if (!rtvRes) return mcdr::err("clearColorTextureRegion", errorMessage(rtvRes.error()));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = *rtvRes;
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float color[4] = {r, g, b, a};
    D3D12_RECT rc{x, y, x + w, y + h};
    ctx->commandList->ClearRenderTargetView(cpu, color, 1, &rc);
    // P64：不回切 COMMON（与 clearColorTexture 一致，交由 endCommandList 收尾）。
    return {};
}

mcdr::Result<void> clearDepthTextureRegion(CommandContext* ctx, Dx12Object* tex, double depth, int x, int y,
                                           int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("clearDepthTextureRegion", "clearDepthTextureRegion: no open command list");
    }
    if (!tex || tex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("clearDepthTextureRegion", "clearDepthTextureRegion: invalid texture");
    }
    if (w <= 0 || h <= 0) return {};  // 空区域：无事可做
    auto dsvRes = frameDsvFor(ctx, tex, 0);
    if (!dsvRes) return mcdr::err("clearDepthTextureRegion", errorMessage(dsvRes.error()));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = *dsvRes;
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_RECT rc{x, y, x + w, y + h};
    ctx->commandList->ClearDepthStencilView(cpu, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                            static_cast<FLOAT>(depth), 0, 1, &rc);
    // P64：不回切 COMMON（与 clearDepthTexture 一致，交由 endCommandList 收尾）。
    return {};
}

mcdr::Result<void> copyBufferToTexture(CommandContext* ctx, Dx12Object* srcBuf, long long srcOffset,
                                       int srcWidth, int srcHeight, Dx12Object* dstTex, int mip, int layer,
                                       int dstX, int dstY, int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("copyBufferToTexture", "copyBufferToTexture: no open command list");
    }
    if (!srcBuf || srcBuf->kind != Dx12Object::Kind::Buffer || !dstTex
        || dstTex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("copyBufferToTexture", "copyBufferToTexture: invalid handle");
    }
    if (w <= 0 || h <= 0 || srcWidth <= 0 || srcHeight <= 0) {
        return mcdr::err("copyBufferToTexture", "copyBufferToTexture: non-positive size");
    }
    UINT block = blockSizeFor(dstTex->dxgiFormat);
    UINT rowBytes = static_cast<UINT>(w) * block;
    UINT srcRowBytes = static_cast<UINT>(srcWidth) * block;  // 源 buffer 每行实际间距（texel*block）

    // D3D12 要求 footprint RowPitch 为 256 的倍数；Minecraft 的 CPU 侧数据是
    // 紧凑打包的，因此整块 RowPitch 未对齐时逐行拷贝以保证正确。
    constexpr UINT kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    bool aligned = (srcRowBytes % kPitchAlign) == 0 && srcWidth == w;

    transitionBufferTo(ctx, srcBuf, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionTextureTo(ctx, dstTex, D3D12_RESOURCE_STATE_COPY_DEST);

    UINT subresource = static_cast<UINT>(mip + layer * dstTex->resource->GetDesc().MipLevels);
    if (dstTex->usage & 16) {  // CUBEMAP_COMPATIBLE：确认 6 个面是否逐一上传
        DBG_LOG_DEBUG("copyBufferToTexture: CUBE dst=%p mip=%d layer=%d subres=%u srcOff=%lld srcW=%d "
                      "srcH=%d w=%d h=%d rowBytes=%u aligned=%d",
                      (void*)dstTex, mip, layer, subresource, (long long)srcOffset, srcWidth, srcHeight, w, h,
                      srcRowBytes, (int)aligned);
    }
    if (aligned) {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = srcBuf->resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = static_cast<UINT64>(srcOffset);
        src.PlacedFootprint.Footprint.Format = dstTex->dxgiFormat;
        src.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
        src.PlacedFootprint.Footprint.Height = static_cast<UINT>(h);
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = srcRowBytes;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = dstTex->resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = subresource;
        ctx->commandList->CopyTextureRegion(&dst, dstX, dstY, 0, &src, nullptr);
    } else {
        // 逐行拷贝：每行 1 像素高，footprint 行距 = 源行距（可能未 256 对齐，
        // 部分驱动允许；这是紧凑数据上传的常见做法）。
        for (int row = 0; row < h; ++row) {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = srcBuf->resource.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset =
                static_cast<UINT64>(srcOffset) + static_cast<UINT64>(row) * srcRowBytes;
            src.PlacedFootprint.Footprint.Format = dstTex->dxgiFormat;
            src.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
            src.PlacedFootprint.Footprint.Height = 1;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = rowBytes;
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = dstTex->resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = subresource;
            ctx->commandList->CopyTextureRegion(&dst, dstX, dstY + row, 0, &src, nullptr);
        }
    }
    // P11：目标纹理显式回切 COMMON（显式进入的 COPY_DEST 不会随命令列表完成
    // decay；回切后下一 command list 的 Before=COMMON 假设才成立）。
    transitionTextureTo(ctx, dstTex, D3D12_RESOURCE_STATE_COMMON);
    return {};
}

mcdr::Result<void> copyTextureToBuffer(CommandContext* ctx, Dx12Object* srcTex, int mip, int layer,
                                       int srcX, int srcY, int w, int h, Dx12Object* dstBuf,
                                       long long dstOffset) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("copyTextureToBuffer", "copyTextureToBuffer: no open command list");
    }
    if (!srcTex || srcTex->kind != Dx12Object::Kind::Texture || !dstBuf
        || dstBuf->kind != Dx12Object::Kind::Buffer) {
        return mcdr::err("copyTextureToBuffer", "copyTextureToBuffer: invalid handle");
    }
    if (w <= 0 || h <= 0) {
        return mcdr::err("copyTextureToBuffer", "copyTextureToBuffer: non-positive size");
    }
    UINT block = blockSizeFor(srcTex->dxgiFormat);
    UINT rowBytes = static_cast<UINT>(w) * block;

    transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionBufferTo(ctx, dstBuf, D3D12_RESOURCE_STATE_COPY_DEST);

    UINT subresource = static_cast<UINT>(mip + layer * srcTex->resource->GetDesc().MipLevels);
    // 客户端（Minecraft）按紧凑行距访问读回数据，且读回 buffer 只分配了
    // 紧凑大小（rowBytes*h）。D3D12 要求 footprint RowPitch 为 256 的倍数，
    // 因此行距未对齐时逐行紧凑拷贝（每行 Height=1）。
    constexpr UINT kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    bool aligned = (rowBytes % kPitchAlign) == 0;
    if (dstOffset + static_cast<UINT64>(rowBytes) * h > static_cast<UINT64>(dstBuf->size)) {
        return mcdr::err("copyTextureToBuffer", "copyTextureToBuffer: destination buffer too small");
    }

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = srcTex->resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = subresource;
    if (aligned) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = dstBuf->resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = static_cast<UINT64>(dstOffset);
        dst.PlacedFootprint.Footprint.Format = srcTex->dxgiFormat;
        dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
        dst.PlacedFootprint.Footprint.Height = static_cast<UINT>(h);
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = rowBytes;
        D3D12_BOX srcBox{static_cast<UINT>(srcX),     static_cast<UINT>(srcY),     0,
                         static_cast<UINT>(srcX + w), static_cast<UINT>(srcY + h), 1};
        ctx->commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &srcBox);
    } else {
        // 逐行拷贝：目标 buffer 紧凑布局，RowPitch=rowBytes（每行 1 像素高）。
        for (int row = 0; row < h; ++row) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = dstBuf->resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = static_cast<UINT64>(dstOffset) + static_cast<UINT64>(row) * rowBytes;
            dst.PlacedFootprint.Footprint.Format = srcTex->dxgiFormat;
            dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
            dst.PlacedFootprint.Footprint.Height = 1;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = rowBytes;
            D3D12_BOX srcBox{static_cast<UINT>(srcX),     static_cast<UINT>(srcY + row),     0,
                             static_cast<UINT>(srcX + w), static_cast<UINT>(srcY + row + 1), 1};
            ctx->commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &srcBox);
        }
    }
    // P11：源纹理显式回切 COMMON（显式进入的 COPY_SOURCE 不会随命令列表完成
    // decay；回切后下一 command list 的 Before=COMMON 假设才成立）。
    transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COMMON);
    return {};
}

mcdr::Result<void> copyTextureToTexture(CommandContext* ctx, Dx12Object* srcTex, Dx12Object* dstTex, int mip,
                                        int layer, int srcX, int srcY, int dstX, int dstY, int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("copyTextureToTexture", "copyTextureToTexture: no open command list");
    }
    if (!srcTex || srcTex->kind != Dx12Object::Kind::Texture || !dstTex
        || dstTex->kind != Dx12Object::Kind::Texture) {
        return mcdr::err("copyTextureToTexture", "copyTextureToTexture: invalid handle");
    }
    if (w <= 0 || h <= 0) {
        return mcdr::err("copyTextureToTexture", "copyTextureToTexture: non-positive size");
    }
    if (srcTex->dxgiFormat != dstTex->dxgiFormat) {
        return mcdr::err("copyTextureToTexture", "copyTextureToTexture: format mismatch");
    }
    transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionTextureTo(ctx, dstTex, D3D12_RESOURCE_STATE_COPY_DEST);

    UINT srcSub = static_cast<UINT>(mip + layer * srcTex->resource->GetDesc().MipLevels);
    UINT dstSub = static_cast<UINT>(mip + layer * dstTex->resource->GetDesc().MipLevels);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = srcTex->resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = srcSub;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = dstTex->resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = dstSub;
    D3D12_BOX srcBox{static_cast<UINT>(srcX),     static_cast<UINT>(srcY),     0,
                     static_cast<UINT>(srcX + w), static_cast<UINT>(srcY + h), 1};
    ctx->commandList->CopyTextureRegion(&dst, dstX, dstY, 0, &src, &srcBox);
    // P11：src/dst 显式回切 COMMON（显式进入的 COPY_SOURCE/COPY_DEST 不会随
    // 命令列表完成 decay；回切后下一 command list 的 Before=COMMON 假设才成立）。
    transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COMMON);
    transitionTextureTo(ctx, dstTex, D3D12_RESOURCE_STATE_COMMON);
    return {};
}

mcdr::Result<void> beginRenderPass(CommandContext* ctx, Dx12Object* const* colorViews, int colorCount,
                                   const int* colorMips, const int* colorClearFlags,
                                   const float* clearColors, Dx12Object* depthView, int depthMip,
                                   int depthClearFlag, double depthClearValue, int x, int y, int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("beginRenderPass", "beginRenderPass: no open command list");
    }
    if (ctx->inRenderPass) {
        return mcdr::err("beginRenderPass", "beginRenderPass: render pass already open");
    }
    if (colorCount < 0) {
        return mcdr::err("beginRenderPass", "beginRenderPass: negative color count");
    }

    // P11：本 pass 的附件在 endRenderPass 时需显式回切 COMMON（见 endRenderPass），
    // 先清空上次记录（防御：上次 pass 未正常 end）。
    ctx->activeColorTargets.clear();
    ctx->activeColorTargetsTouched.clear();
    ctx->activeDepthTarget = nullptr;

    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
    rtvs.reserve(static_cast<size_t>(colorCount));
    for (int i = 0; i < colorCount; ++i) {
        Dx12Object* tex = colorViews[i];
        if (!tex) continue;  // withUnusedColorAttachment 占位
        if (tex->kind != Dx12Object::Kind::Texture || !(tex->usage & 8)) {
            return mcdr::err("beginRenderPass",
                             "beginRenderPass: invalid color attachment (needs RENDER_ATTACHMENT)");
        }
        // P24：帧级瞬态 RTV（frameRtvHeap），每帧从 0 复用，不与 backbuffer RTV
        // 冲突。此前共用 rtvHeap + gNextRtv 导致每帧覆盖 surface 的 backbuffer RTV，
        // blit 的 OMSetRenderTargets 绑到颜色纹理 → 资源状态 0xC0 错误 → 黑屏。
        // P3b fix：RTV 绑定 view 的真实 mip slice（官方对每个 mipViews[level]
        // 发起 render pass 逐级写入图集 mip1..N）。P64：(资源, mip) 走缓存。
        int mip = colorMips ? colorMips[i] : 0;
        auto rtvRes = frameRtvFor(ctx, tex, mip);
        if (!rtvRes) return std::unexpected<mcdr::Error>(std::move(rtvRes.error()));
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = *rtvRes;
        rtvs.push_back(cpu);
        ctx->activeColorTargets.push_back(tex);
        // P27：beginRenderPass 加载期可调用数千次，逐条 dbgLog 写文件拖死主线程，
        // 降级为 DBG_LOG_DEBUG（DX12_LOG_VERBOSE=1 才输出）。
        DBG_LOG_DEBUG("beginRenderPass color[%d] tex=%p rtv=%p mip=%d dims=%ux%u fmt=%d", i, (void*)tex,
                      (void*)cpu.ptr, mip, (UINT)tex->resource->GetDesc().Width,
                      (UINT)tex->resource->GetDesc().Height, (int)tex->dxgiFormat);
        transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (colorClearFlags && colorClearFlags[i] && clearColors) {
            const float* c = clearColors + i * 4;
            ctx->commandList->ClearRenderTargetView(cpu, c, 0, nullptr);
            // P6 诊断：每 60 帧打印一次 clear 颜色（确认画面底色=clear 值）。
            static int rpDbg = 0;
            if ((++rpDbg % 60) == 1) {
                dbgLogDebug("beginRenderPass clear[%d] = (%.3f, %.3f, %.3f, %.3f)", i, static_cast<double>(c[0]),
                       static_cast<double>(c[1]), static_cast<double>(c[2]), static_cast<double>(c[3]));
            }
        }
    }

    // BUG-02 fix：touched 标记与 activeColorTargets 一一对应（按实际入队的附件数分配，
    // 不能用 colorCount —— nullptr 占位槽会跳过，两者位置会错开）。此前从未分配过，
    // size() 恒为 0，markMeshColorTargets / drawIndexedInstanced 里的 `i < size()`
    // 守卫全部命中不了，这套「按附件记录是否写入」的机制等于不存在。
    ctx->activeColorTargetsTouched.assign(ctx->activeColorTargets.size(), false);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    bool hasDsv = false;
    if (depthView) {
        if (depthView->kind != Dx12Object::Kind::Texture) {
            return mcdr::err("beginRenderPass", "beginRenderPass: invalid depth attachment");
        }
        // P3b fix：DSV 绑定真实 mip slice（默认 0，语义不变）。P64：(资源, mip) 走缓存。
        auto dsvRes = frameDsvFor(ctx, depthView, depthMip);
        if (!dsvRes) return std::unexpected<mcdr::Error>(std::move(dsvRes.error()));
        dsv = *dsvRes;
        hasDsv = true;
        ctx->activeDepthTarget = depthView;
        transitionTextureTo(ctx, depthView, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        // reverse-Z 修复：Minecraft 26.x 使用反向深度（depthFunc=GREATER_EQUAL），
        // 深度 clear 值为 0.0（远平面）。vanilla 在创建主渲染 pass 前已通过
        // clearDepthTexture(..., 0.0) pre-clear（同一 command encoder，顺序有保证），
        // 主 pass 的 depth attachment clearValue 为 empty → depthClearFlag=0（LOAD）。
        // 原 P21 在 LOAD 时无条件回退 clear=1.0，覆盖了 pre-clear 的 0.0，导致
        // GREATER_EQUAL 深度测试丢弃几乎所有片元 → 全黑屏。
        // 正确行为：LOAD 不 clear（保持 pre-clear 值），仅显式 CLEAR 时 clear。
        if (depthClearFlag) {
            ctx->commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                    static_cast<FLOAT>(depthClearValue), 0, 0, nullptr);
            // P27：降级（数千次 pass 调用 × 高频写文件）。
            DBG_LOG_DEBUG("beginRenderPass depth clear=explicit val=%.3f", (FLOAT)depthClearValue);
        } else {
            DBG_LOG_DEBUG("beginRenderPass depth load (no clear)");
        }
    }

    ctx->commandList->OMSetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.empty() ? nullptr : rtvs.data(),
                                         false, hasDsv ? &dsv : nullptr);
    D3D12_VIEWPORT vp{static_cast<FLOAT>(x),
                      static_cast<FLOAT>(y),
                      static_cast<FLOAT>(w),
                      static_cast<FLOAT>(h),
                      0.0f,
                      1.0f};
    ctx->commandList->RSSetViewports(1, &vp);
    D3D12_RECT scissor{x, y, x + w, y + h};
    ctx->commandList->RSSetScissorRects(1, &scissor);
    ctx->inRenderPass = 1;
    // P6 诊断：确认渲染 pass 绑定的附件（blit 源纹理应出现在 color[0]）。
    // P27：降级为 DBG_LOG_DEBUG（数千次 pass 调用 × 高频写文件拖死主线程）。
    DBG_LOG_DEBUG("beginRenderPass: ctx=%p colorCount=%d area=%d,%d %dx%d depth=%s", (void*)ctx, colorCount,
                  x, y, w, h, depthView ? "yes" : "no");
    for (int i = 0; i < colorCount; ++i) {
        if (colorViews[i]) {
#ifdef DX12_DEBUG_VERBOSE
            // 整段诊断被括在 DX12_DEBUG_VERBOSE 里：默认构建下 DBG_LOG_DEBUG 展开为
            // ((void)0)，tex/desc/cc 全成了未引用局部变量，/W4 会报 C4189。
            auto* tex = colorViews[i];
            auto desc = tex->resource->GetDesc();
            const float* cc = clearColors ? (clearColors + i * 4) : nullptr;
            DBG_LOG_DEBUG("beginRenderPass color[%d] tex=%p dims=%ux%u fmt=%d clear=(%.3f,%.3f,%.3f,%.3f)", i,
                          (void*)tex, (UINT)desc.Width, (UINT)desc.Height, (int)tex->dxgiFormat,
                          cc ? (double)cc[0] : 0.0, cc ? (double)cc[1] : 0.0, cc ? (double)cc[2] : 0.0,
                          cc ? (double)cc[3] : 0.0);
#endif
            // BUG-01 fix：diag loop 只打印日志，不再重复 push_back（主循环 L1701 已添加）。
        }
    }
    return {};
}

mcdr::Result<void> endRenderPass(CommandContext* ctx) {
    if (!ctx) {
        return mcdr::err("endRenderPass", "endRenderPass: null ctx");
    }
    if (!ctx->inRenderPass) return {};  // 幂等
    DBG_LOG_DEBUG("endRenderPass: ctx=%p", (void*)ctx);
    // P11 修复：附件必须显式回切 COMMON。D3D12 的 RENDER_TARGET/DEPTH_WRITE 属
    // "非可提升状态"，命令列表执行完成时【不会】隐式 decay 回 COMMON（只有
    // COPY_SOURCE/COPY_DEST/UAV 等可提升状态才会）。若此处不显式回切，下一
    // command list 的 beginRenderPass/blit 会按 COMMON 写 barrier 的 Before 状态，
    // 与资源实际状态错配 → 每帧验证 ERROR（"Before state ... does not match"）→
    // GPU 状态错乱 → TDR 冻结（游戏挂死，启动器同 GPU 渲染也卡死）。
    // transitionTextureTo 按本 list 已跟踪状态回切（进入时为 RENDER_TARGET/
    // DEPTH_WRITE），from==to 时自动跳过，幂等。
    for (Dx12Object* tex : ctx->activeColorTargets) {
        if (tex) transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_COMMON);
    }
    if (ctx->activeDepthTarget) {
        transitionTextureTo(ctx, ctx->activeDepthTarget, D3D12_RESOURCE_STATE_COMMON);
    }
    // P6 诊断：记录 endRenderPass 前绑定的颜色附件，便于确认渲染目标正确性。
    // P27：降级为 DBG_LOG_DEBUG（数千次 pass 调用 × 高频写文件拖死主线程）。
    if (ctx->activeColorTargets.size() > 0) {
        for (Dx12Object* tex : ctx->activeColorTargets) {
            if (tex && tex->resource) {
                auto desc = tex->resource->GetDesc();
                DBG_LOG_DEBUG("endRenderPass colorAttach tex=%p dims=%ux%u fmt=%d touched=%d", (void*)tex,
                              (UINT)desc.Width, (UINT)desc.Height, (int)tex->dxgiFormat,
                              (int)std::distance(ctx->activeColorTargets.begin(),
                                                 std::find(ctx->activeColorTargets.begin(),
                                                           ctx->activeColorTargets.end(), tex)));
            }
        }
    }
    ctx->activeColorTargets.clear();
    ctx->activeColorTargetsTouched.clear();
    ctx->activeDepthTarget = nullptr;
    ctx->inRenderPass = 0;
    return {};
}

// ---------------------------------------------------------------------------
// Timestamp query pool（P3）
// ---------------------------------------------------------------------------

mcdr::Result<void*> createQueryPool(int size) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createQueryPool", errorMessage(d.error()));
    if (size <= 0) {
        return mcdr::err("createQueryPool", "createQueryPool: size must be positive");
    }
    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    desc.Count = static_cast<UINT>(size);
    auto pool = std::make_unique<QueryPool>();
    if (FAILED(gCtx.device->CreateQueryHeap(&desc, IID_PPV_ARGS(&pool->heap)))) {
        return mcdr::err("createQueryPool", "createQueryPool: CreateQueryHeap failed");
    }
    pool->size = size;
    return static_cast<void*>(pool.release());
}

void destroyQueryPool(QueryPool* pool) { delete pool; }

mcdr::Result<void> writeTimestampToPool(CommandContext* ctx, QueryPool* pool, int index) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("writeTimestampToPool", "writeTimestampToPool: no open command list");
    }
    if (!pool || index < 0 || index >= pool->size) {
        return mcdr::err("writeTimestampToPool", "writeTimestampToPool: invalid index");
    }
    ctx->commandList->EndQuery(pool->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(index));
    return {};
}

mcdr::Result<void> readQueryValue(QueryPool* pool, int index, long long& out) {
    return resolveQuery(pool, index, 1, &out);
}

mcdr::Result<void> readQueryValues(QueryPool* pool, int start, int count, long long* out) {
    return resolveQuery(pool, start, count, out);
}

unsigned long long getTimestampFrequency() { return gCtx.timestampFrequency; }

// ---------------------------------------------------------------------------
// P4: 图形管线（D3DCompile + root signature + 双 PSO）
// ---------------------------------------------------------------------------

namespace {

// MC BlendFactor ordinal -> D3D12_BLEND（枚举声明顺序）
D3D12_BLEND toD3d12BlendFactor(uint8_t f) {
    switch (f) {
        case 0: return D3D12_BLEND_BLEND_FACTOR;  // CONSTANT_ALPHA
        case 1: return D3D12_BLEND_BLEND_FACTOR;  // CONSTANT_COLOR
        case 2: return D3D12_BLEND_DEST_ALPHA;  // DST_ALPHA
        case 3: return D3D12_BLEND_DEST_COLOR;  // DST_COLOR
        case 4: return D3D12_BLEND_ONE;  // ONE
        case 5: return D3D12_BLEND_INV_BLEND_FACTOR;  // ONE_MINUS_CONSTANT_ALPHA
        case 6: return D3D12_BLEND_INV_BLEND_FACTOR;  // ONE_MINUS_CONSTANT_COLOR
        case 7: return D3D12_BLEND_INV_DEST_ALPHA;  // ONE_MINUS_DST_ALPHA
        case 8: return D3D12_BLEND_INV_DEST_COLOR;  // ONE_MINUS_DST_COLOR
        case 9: return D3D12_BLEND_INV_SRC_ALPHA;  // ONE_MINUS_SRC_ALPHA
        case 10: return D3D12_BLEND_INV_SRC_COLOR;  // ONE_MINUS_SRC_COLOR
        case 11: return D3D12_BLEND_SRC_ALPHA;  // SRC_ALPHA
        case 12: return D3D12_BLEND_SRC_ALPHA_SAT;  // SRC_ALPHA_SATURATE
        case 13: return D3D12_BLEND_SRC_COLOR;  // SRC_COLOR
        case 14: return D3D12_BLEND_ZERO;  // ZERO
        default: return D3D12_BLEND_ONE;
    }
}

// MC BlendOp ordinal -> D3D12_BLEND_OP
D3D12_BLEND_OP toD3d12BlendOp(uint8_t op) {
    switch (op) {
        case 0: return D3D12_BLEND_OP_ADD;  // ADD
        case 1: return D3D12_BLEND_OP_SUBTRACT;  // SUBTRACT
        case 2: return D3D12_BLEND_OP_REV_SUBTRACT;  // REVERSE_SUBTRACT
        case 3: return D3D12_BLEND_OP_MIN;  // MIN
        case 4: return D3D12_BLEND_OP_MAX;  // MAX
        default: return D3D12_BLEND_OP_ADD;
    }
}

// MC CompareOp ordinal -> D3D12_COMPARISON_FUNC
D3D12_COMPARISON_FUNC toD3d12Compare(uint8_t c) {
    switch (c) {
        case 0: return D3D12_COMPARISON_FUNC_ALWAYS;  // ALWAYS_PASS
        case 1: return D3D12_COMPARISON_FUNC_LESS;  // LESS_THAN
        case 2: return D3D12_COMPARISON_FUNC_LESS_EQUAL;  // LESS_THAN_OR_EQUAL
        case 3: return D3D12_COMPARISON_FUNC_EQUAL;  // EQUAL
        case 4: return D3D12_COMPARISON_FUNC_NOT_EQUAL;  // NOT_EQUAL
        case 5: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // GREATER_THAN_OR_EQUAL
        case 6: return D3D12_COMPARISON_FUNC_GREATER;  // GREATER_THAN
        case 7: return D3D12_COMPARISON_FUNC_NEVER;  // NEVER_PASS
        default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

// MC PrimitiveTopology ordinal -> D3D12_PRIMITIVE_TOPOLOGY_TYPE
D3D12_PRIMITIVE_TOPOLOGY_TYPE toTopologyType(int t) {
    switch (t) {
        case 0:
        case 1:
        case 2: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;  // LINES/DEBUG_LINES/DEBUG_LINE_STRIP
        case 3: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;  // POINTS
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;  // TRIANGLES/STRIP/FAN/QUADS
    }
}

// MC PrimitiveTopology ordinal -> 命令列表级 D3D_PRIMITIVE_TOPOLOGY。
// D3D12 命令列表初始 topology 是 UNDEFINED，任何 draw 前必须 IASetPrimitiveTopology，
// 否则 GPU 丢弃全部图元（纯 clear 色黑屏根因）。QUADS 无原生支持，回退 TRIANGLELIST。
D3D12_PRIMITIVE_TOPOLOGY toPrimitiveTopology(int t) {
    switch (t) {
        case 0:
        case 1: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;  // LINES/DEBUG_LINES
        case 2: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;  // DEBUG_LINE_STRIP
        case 3: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;  // POINTS
        case 4: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;  // TRIANGLES
        case 5: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;  // TRIANGLE_STRIP
        case 6: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLEFAN;  // TRIANGLE_FAN
        case 7: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;  // QUADS（不支持，回退）
        default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;  // 未知 ordinal（含 > 7），按三角形回退
    }
}

}  // namespace

// P84 / 2026-10-25：图形管线的着色器模型现在是**硬要求 SM 6.9**。
// 用户明确要求删掉 SM 5.1(DXBC) 兜底、不留回退路径，所以这里只有两种结果：
//   "6_9" —— 可以继续建 PSO；
//   ""    —— 不满足，调用方必须报错（sm69RequirementText 说清缺哪一条）。
// 两个前置条件，缺一不可：
//   1) 这台 DXC 能产出 6_9 的 DXIL（caps.dxcSupportsSm69()，probeDxc 的 ms_6_9 冒烟）；
//   2) 设备自己报 HighestShaderModel >= 6.9 —— 只有 Agility SDK 1.619 的 core 才有，
//      inbox D3D12Core 最高 6.8（P110 / known_bugs §D.18）。这种情况下 D3D12 后端
//      直接报错，而不是偷偷降级。
static std::string graphicsShaderModelProfile() {
    if (!gCtx.caps.dxcAvailable || !gCtx.caps.dxcSupportsSm69()) return {};
    if (effectiveShaderModel(gCtx.caps) < D3D_SHADER_MODEL_6_9) return {};
    return "6_9";
}

// 6.9 硬要求满足不了时的统一说明：把"缺 DXC / DXC 不认 6.9 / 设备只到 6.x"分开说清楚，
// 否则现场只剩一句 CreatePipelineState 失败，排查要重新走一遍 P84 那套。
static std::string sm69RequirementText() {
    if (!gCtx.caps.dxcAvailable) {
        return "shader model 6.9 is required and there is no SM 5.1 fallback any more, but "
               "dxcompiler.dll was not found (MCDR_DXC_DIR / MCDR.dll dir / Windows SDK)";
    }
    if (!gCtx.caps.dxcSupportsSm69()) {
        return "shader model 6.9 is required and there is no SM 5.1 fallback any more, but this "
               "dxcompiler.dll cannot compile 6_9 (profile probe: " + gCtx.caps.dxcProfile +
               "; needs DXC 1.9.2502 or newer)";
    }
    return "shader model 6.9 is required and there is no SM 5.1 fallback any more, but the device "
           "reports SM " + std::string(shaderModelName(gCtx.caps.shaderModel)) +
           " (Agility SDK 619's D3D12Core.dll must sit next to MCDR.dll and be opted in)";
}

// ---------------------------------------------------------------------------
// P234：compute 最小通路
//
// 根签名固定为「root UAV u0 + 8 个根常量 b0」：既避开描述符堆，也避开把根签名布局
// 跨模块传出去。compute 本身跑在普通的 DIRECT 列表上（本模块没有 COMPUTE 列表类型），
// 所以它和图形命令可以同列表共存 —— SRI 生成、NRC 的 update/query 都要这个性质。
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// P234/P235 共用：compute 的描述符堆
//
// typed UAV（`RWTexture2D`、以及 P235 的 R8_UINT 着色率图像）与 typed SRV 都必须
// 经描述符表绑定 —— root descriptor 只支持 raw / structured buffer。堆懒建、64 槽
// ring：一次 dispatch 占 1～3 格；同一帧内多次 dispatch 各占各的格，避免后一次
// 覆盖前一次"已录制、未执行"的描述符。
// ---------------------------------------------------------------------------
bool ensureComputeHeap() {
    if (gCtx.computeUavHeap) return true;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 64;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT hr =
        gCtx.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gCtx.computeUavHeap));
    if (FAILED(hr)) {
        dbgLogError("compute: CreateDescriptorHeap hr=0x%s", hrText(hr).c_str());
        return false;
    }
    gCtx.computeUavStride =
        gCtx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gCtx.computeUavNext = 0;
    dbgLogInfo("compute: descriptor heap ready (64 slots, stride=%u)", gCtx.computeUavStride);
    return true;
}

UINT computeHeapSlot() { return gCtx.computeUavNext++ % 64; }

D3D12_CPU_DESCRIPTOR_HANDLE computeHeapCpu(UINT slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = gCtx.computeUavHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * gCtx.computeUavStride;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE computeHeapGpu(UINT slot) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = gCtx.computeUavHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(slot) * gCtx.computeUavStride;
    return h;
}

mcdr::Result<void*> createComputePipeline(const void* dxil, int32_t dxilSize) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createComputePipeline", errorMessage(d.error()));
    if (dxil == nullptr || dxilSize <= 0) {
        return mcdr::err("createComputePipeline", "createComputePipeline: empty DXIL bytecode");
    }

    // u0 必须走描述符表：root descriptor 的 UAV 只支持 raw / structured buffer，
    // typed UAV（RWTexture2D，P235 的着色率图像也是这种）挂在 root UAV 上会让
    // CreateComputePipelineState 直接返回 E_INVALIDARG（2026-10-25 实测）。
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;  // u0
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &uavRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;  // b0
    params[1].Constants.RegisterSpace = 0;
    params[1].Constants.Num32BitValues = 8;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 0;
    rsDesc.pStaticSamplers = nullptr;
    // 不带 ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT：compute 没有 IA 阶段。
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        return mcdr::err("createComputePipeline",
                         "createComputePipeline: D3D12SerializeRootSignature hr=0x" + hrText(hr));
    }

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::ComputePipeline;
    obj->size = dxilSize;
    hr = gCtx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&obj->computeRootSignature));
    if (FAILED(hr)) {
        return mcdr::err("createComputePipeline",
                         "createComputePipeline: CreateRootSignature hr=0x" + hrText(hr));
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = obj->computeRootSignature.Get();
    psoDesc.CS.pShaderBytecode = dxil;
    psoDesc.CS.BytecodeLength = static_cast<SIZE_T>(dxilSize);
    psoDesc.NodeMask = 0;
    psoDesc.CachedPSO.pCachedBlob = nullptr;
    psoDesc.CachedPSO.CachedBlobSizeInBytes = 0;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    hr = gCtx.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&obj->computePipelineState));
    if (FAILED(hr)) {
        std::string text = "createComputePipeline: CreateComputePipelineState hr=" + hrText(hr);
        // P234：E_INVALIDARG 在 compute PSO 上只有几种可能（DXIL 不是 compute 对象 / 根签名指针
        // 无效 / desc 被写坏），把能查的都打出来，并把 debug layer 的最近消息一并倒出，
        // 免得靠猜。dxil 的头 4 字节应当是 'DXBC'（DXIL 容器沿用这个魔数）。
        char magic[5] = {0, 0, 0, 0, 0};
        if (dxilSize >= 4) {
            std::memcpy(magic, dxil, 4);
        }
        char headText[128];
        std::snprintf(headText, sizeof(headText),
                      " dxil=%d magic=[%c%c%c%c] rootSig=%p", dxilSize, magic[0], magic[1], magic[2],
                      magic[3], static_cast<void*>(psoDesc.pRootSignature));
        text += headText;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
            || hr == DXGI_ERROR_DEVICE_HUNG) {
            text += " — " + deviceStatusText();
        }
        dumpInfoQueueMessages();
        return mcdr::err("createComputePipeline", text);
    }

    dbgLogInfo("createComputePipeline: PSO handle=%p dxil=%d bytes (descriptor table u0 + 8 root constants b0)",
               reinterpret_cast<void*>(obj.get()), dxilSize);
    return static_cast<void*>(obj.release());
}

int dispatchCompute(CommandContext* ctx, Dx12Object* pipeline, Dx12Object* uavTexture,
                    const int32_t* constants, int32_t constantCount, int32_t x, int32_t y, int32_t z) {
    if (ctx == nullptr || !ctx->commandList || !ctx->listOpen) {
        dbgLogError("dispatchCompute: called without an open command list");
        return -2;
    }
    if (pipeline == nullptr || pipeline->kind != Dx12Object::Kind::ComputePipeline
        || !pipeline->computePipelineState || !pipeline->computeRootSignature) {
        dbgLogError("dispatchCompute: handle is not a compute pipeline");
        return 0;
    }
    if (uavTexture == nullptr || uavTexture->kind != Dx12Object::Kind::Texture
        || !uavTexture->resource) {
        dbgLogError("dispatchCompute: UAV target is not a texture");
        return 0;
    }
    // usage 位 32 = ALLOW_UNORDERED_ACCESS（见 textureFlags）。少了它 CreateUnorderedAccessView
    // 与 root UAV 都会失败，而失败点落在 GPU 侧、日志里什么也没有 —— 这里提前拦住。
    if ((uavTexture->usage & 32) == 0) {
        dbgLogError("dispatchCompute: texture lacks usage bit 32 (ALLOW_UNORDERED_ACCESS), usage=0x%x",
                    uavTexture->usage);
        return 0;
    }
    if (x <= 0 || y <= 0 || z <= 0) {
        dbgLogError("dispatchCompute: invalid thread-group counts %d,%d,%d", x, y, z);
        return 0;
    }
    constexpr int32_t kMaxConstants = 8;
    if (constantCount < 0 || constantCount > kMaxConstants
        || (constantCount > 0 && constants == nullptr)) {
        dbgLogError("dispatchCompute: constantCount %d out of range (0..%d)", constantCount, kMaxConstants);
        return 0;
    }

    ID3D12GraphicsCommandList* list = ctx->commandList;
    // P234：typed UAV 必须经描述符表绑定（见根签名里 uavRange 的注释）。堆懒建，
    // 64 槽 ring —— 一次 dispatch 占一格；同一帧内多次 dispatch 各自占格，避免
    // 覆盖前一次已录制但未执行的描述符。
    if (!ensureComputeHeap()) {
        return 0;
    }
    const UINT uavSlot = computeHeapSlot();
    const D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = computeHeapCpu(uavSlot);
    const D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = computeHeapGpu(uavSlot);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = uavTexture->dxgiFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    uavDesc.Texture2D.PlaneSlice = 0;
    gCtx.device->CreateUnorderedAccessView(uavTexture->resource.Get(), nullptr, &uavDesc, uavCpu);

    // 创建时恒为 COMMON（createTexture 的初始状态），所以这里的 from 状态是确定的；
    // 转回 COMMON 则让后续跨列表的拷贝仍按"未保持状态"处理（见 dbgReadbackTexturePixels）。
    transitionTextureTo(ctx, uavTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(pipeline->computePipelineState.Get());
    list->SetComputeRootSignature(pipeline->computeRootSignature.Get());
    ID3D12DescriptorHeap* heaps[1] = {gCtx.computeUavHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootDescriptorTable(0, uavGpu);
    if (constantCount > 0) {
        list->SetComputeRoot32BitConstants(1, static_cast<UINT>(constantCount), constants, 0);
    }
    list->Dispatch(static_cast<UINT>(x), static_cast<UINT>(y), static_cast<UINT>(z));
    transitionTextureTo(ctx, uavTexture, D3D12_RESOURCE_STATE_COMMON);
    // P273：上面把堆切成了 computeUavHeap。命令列表级状态不回退，同一列表里后续
    // 走瞬时 SRV 的绘制会因此撞 P24 那条 "descriptor heap ... different" 报错。
    // 这里立刻把主堆绑回来（Dispatch 已经按顺序录制完，执行期仍用 computeUavHeap）。
    restoreDrawHeap(ctx);
    return 1;
}

// ---------------------------------------------------------------------------
// P235：着色率图像（tier 2 SRI）
//
// 布局：[0] 描述符表（t0/t1 两张 SRV：上一帧深度 + 上一帧速度）
//       [1] 描述符表（u0 一张 UAV：R8_UINT 的着色率图像）
//       [2] 根常量 b0（8 个 int32：宽/高/tile/阈值…）
// 与 P234 的单 UAV 版并存：那条给"只写一张图"的简单 compute 用，这条给需要
// 读两张输入的 SRI 用。图像值就是 D3D12_SHADING_RATE_* 的枚举值（0=1X1 … 0xA=4X4）。
// ---------------------------------------------------------------------------
mcdr::Result<void*> createComputePipelineShadingRate(const void* dxil, int32_t dxilSize) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createComputePipelineShadingRate", errorMessage(d.error()));
    if (dxil == nullptr || dxilSize <= 0) {
        return mcdr::err("createComputePipelineShadingRate",
                         "createComputePipelineShadingRate: empty DXIL bytecode");
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 3;
    srvRange.BaseShaderRegister = 0;  // t0 深度, t1 速度, t2 上一帧图像（P236 时间钳制）
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;  // u0
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &uavRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;  // b0
    params[2].Constants.RegisterSpace = 0;
    params[2].Constants.Num32BitValues = 8;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 0;
    rsDesc.pStaticSamplers = nullptr;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        return mcdr::err("createComputePipelineShadingRate",
                         "createComputePipelineShadingRate: D3D12SerializeRootSignature hr=0x" + hrText(hr));
    }

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::ComputePipeline;
    obj->size = dxilSize;
    hr = gCtx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&obj->computeRootSignature));
    if (FAILED(hr)) {
        return mcdr::err("createComputePipelineShadingRate",
                         "createComputePipelineShadingRate: CreateRootSignature hr=0x" + hrText(hr));
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = obj->computeRootSignature.Get();
    psoDesc.CS.pShaderBytecode = dxil;
    psoDesc.CS.BytecodeLength = static_cast<SIZE_T>(dxilSize);
    psoDesc.NodeMask = 0;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    hr = gCtx.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&obj->computePipelineState));
    if (FAILED(hr)) {
        std::string text =
            "createComputePipelineShadingRate: CreateComputePipelineState hr=0x" + hrText(hr);
        char head[96];
        std::snprintf(head, sizeof(head), " dxil=%d", dxilSize);
        text += head;
        dumpInfoQueueMessages();
        return mcdr::err("createComputePipelineShadingRate", text);
    }

    dbgLogInfo("createComputePipelineShadingRate: PSO handle=%p dxil=%d bytes (srv t0/t1/t2 + uav u0 + b0)",
               static_cast<void*>(obj.get()), dxilSize);
    return static_cast<void*>(obj.release());
}

int dispatchComputeShadingRate(CommandContext* ctx, Dx12Object* pipeline, Dx12Object* srvDepth,
                              Dx12Object* srvMotion, Dx12Object* srvPrevious, Dx12Object* image,
                              const int32_t* constants, int32_t constantCount, int32_t x, int32_t y,
                              int32_t z) {
    if (ctx == nullptr || !ctx->commandList || !ctx->listOpen) {
        dbgLogError("dispatchComputeShadingRate: called without an open command list");
        return -2;
    }
    if (pipeline == nullptr || pipeline->kind != Dx12Object::Kind::ComputePipeline
        || !pipeline->computePipelineState || !pipeline->computeRootSignature) {
        dbgLogError("dispatchComputeShadingRate: handle is not a compute pipeline");
        return 0;
    }
    if (srvDepth == nullptr || srvMotion == nullptr || srvPrevious == nullptr || image == nullptr
        || !image->resource || image->kind != Dx12Object::Kind::Texture) {
        dbgLogError("dispatchComputeShadingRate: depth/motion/previous view or image missing");
        return 0;
    }
    if ((image->usage & 32) == 0) {
        dbgLogError("dispatchComputeShadingRate: image lacks usage bit 32, usage=0x%x", image->usage);
        return 0;
    }
    constexpr int32_t kMaxConstants = 8;
    if (constantCount < 0 || constantCount > kMaxConstants
        || (constantCount > 0 && constants == nullptr)) {
        dbgLogError("dispatchComputeShadingRate: constantCount %d out of range (0..%d)", constantCount,
                    kMaxConstants);
        return 0;
    }
    if (x <= 0 || y <= 0 || z <= 0) {
        dbgLogError("dispatchComputeShadingRate: invalid thread-group counts %d,%d,%d", x, y, z);
        return 0;
    }

    if (!ensureComputeHeap()) return 0;
    ID3D12GraphicsCommandList* list = ctx->commandList;

    // 三张 SRV 必须落在同一条连续区间（根参数 [0] 是 3 个描述符的表）。
    const UINT srvSlot = computeHeapSlot();
    const UINT uavSlot = srvSlot + 3;
    gCtx.computeUavNext = srvSlot + 4;
    const D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = computeHeapCpu(srvSlot);
    const D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = computeHeapCpu(uavSlot);
    if (srvDepth->cpuHandle.ptr != 0) {
        gCtx.device->CopyDescriptorsSimple(1, srvCpu, srvDepth->cpuHandle,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (srvMotion->cpuHandle.ptr != 0) {
        D3D12_CPU_DESCRIPTOR_HANDLE second{};
        second.ptr = srvCpu.ptr + gCtx.computeUavStride;
        gCtx.device->CopyDescriptorsSimple(1, second, srvMotion->cpuHandle,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    // t2 = 上一帧的着色率图像（P236 时间钳制）。没有历史时也要放一张有效描述符：
    // 着色器里由 constants 的 bit1 决定要不要读它。
    if (srvPrevious->cpuHandle.ptr != 0) {
        D3D12_CPU_DESCRIPTOR_HANDLE third{};
        third.ptr = srvCpu.ptr + 2 * gCtx.computeUavStride;
        gCtx.device->CopyDescriptorsSimple(1, third, srvPrevious->cpuHandle,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    } else {
        dbgLogError("dispatchComputeShadingRate: previous image view has no CPU handle");
        return 0;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = image->dxgiFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    uavDesc.Texture2D.PlaneSlice = 0;
    gCtx.device->CreateUnorderedAccessView(image->resource.Get(), nullptr, &uavDesc, uavCpu);

    // 三张输入必须可读；状态跟踪走 transitionTo（同一命令上下文内自洽）。
    if (srvDepth->sourceTexture && srvDepth->sourceTexture->resource) {
        transitionTo(ctx, srvDepth->sourceTexture->resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (srvMotion->sourceTexture && srvMotion->sourceTexture->resource) {
        transitionTo(ctx, srvMotion->sourceTexture->resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (srvPrevious->sourceTexture && srvPrevious->sourceTexture->resource) {
        transitionTo(ctx, srvPrevious->sourceTexture->resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    transitionTextureTo(ctx, image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    list->SetPipelineState(pipeline->computePipelineState.Get());
    list->SetComputeRootSignature(pipeline->computeRootSignature.Get());
    ID3D12DescriptorHeap* heaps[1] = {gCtx.computeUavHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootDescriptorTable(0, computeHeapGpu(srvSlot));
    list->SetComputeRootDescriptorTable(1, computeHeapGpu(uavSlot));
    if (constantCount > 0) {
        list->SetComputeRoot32BitConstants(2, static_cast<UINT>(constantCount), constants, 0);
    }
    list->Dispatch(static_cast<UINT>(x), static_cast<UINT>(y), static_cast<UINT>(z));
    // 图像留在 COMMON：绑定前由 setShadingRateImage 单独转到 SHADING_RATE_SOURCE。
    transitionTextureTo(ctx, image, D3D12_RESOURCE_STATE_COMMON);
    return 1;
}

// 建一张 R8_UINT 的着色率图像。尺寸是 **tile 数**（宽高各自 ceil(pixel / tile)），
// 因为 SRI 的每个 texel 管一整块 tile；tile 边长见 caps.shadingRateImageTileSize
// （NVIDIA 16、AMD 8）。usage 38 = 32(UAV，写) | 4(TEXTURE_BINDING，上一帧当 t2 读) |
// 2(COPY_SRC，供自检读回)。
mcdr::Result<void*> createShadingRateImage(int32_t width, int32_t height) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createShadingRateImage", errorMessage(d.error()));
    if (!gCtx.caps.shadingRateImageSupported()) {
        return mcdr::err("createShadingRateImage", "createShadingRateImage: device is below VRS tier 2");
    }
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return mcdr::err("createShadingRateImage", "createShadingRateImage: bad tile dimensions");
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = static_cast<UINT64>(width);
    desc.Height = static_cast<UINT>(height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::Texture;
    obj->usage = 38;
    obj->dxgiFormat = DXGI_FORMAT_R8_UINT;
    obj->size = static_cast<long long>(width) * height;
    HRESULT hr = gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&obj->resource));
    if (FAILED(hr)) {
        return mcdr::err("createShadingRateImage",
                         "createShadingRateImage: CreateCommittedResource hr=0x" + hrText(hr));
    }
    dbgLogInfo("createShadingRateImage: handle=%p %dx%d tiles (tile=%u)", static_cast<void*>(obj.get()),
               width, height, gCtx.caps.shadingRateImageTileSize);
    return static_cast<void*>(obj.release());
}

// 把图像挂到命令列表的 SRI 槽位（image == nullptr 表示摘掉）。
// SHADING_RATE_SOURCE 是 D3D12_RESOURCE_STATES 里一个普通位（0x1000000），
// transitionTo 直接能用 —— 本模块没有别的 barrier ABI。
int setShadingRateImage(CommandContext* ctx, Dx12Object* image) {
    if (ctx == nullptr || !ctx->commandList || !ctx->listOpen) {
        dbgLogError("setShadingRateImage: called without an open command list");
        return -2;
    }
    ComPtr<ID3D12GraphicsCommandList5> list5;
    if (FAILED(ctx->commandList->QueryInterface(IID_PPV_ARGS(&list5))) || !list5) {
        dbgLogWarn("setShadingRateImage: command list is not ID3D12GraphicsCommandList5");
        return 0;
    }
    if (image == nullptr) {
        list5->RSSetShadingRateImage(nullptr);
        // P235：解绑必须把图像转回 COMMON —— SHADING_RATE_SOURCE 只在"绑定期间"
        // 合法，其余代码路径一律按 COMMON 做状态转换（读回就是其中之一）。
        if (gCtx.shadingRateImageBound != nullptr && gCtx.shadingRateImageBound->resource) {
            transitionTo(ctx, gCtx.shadingRateImageBound->resource.Get(),
                         D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            gCtx.shadingRateImageBound = nullptr;
        }
        return 1;
    }
    if (image->kind != Dx12Object::Kind::Texture || !image->resource) {
        dbgLogError("setShadingRateImage: handle is not a texture");
        return -1;
    }
    // 换绑：先把上一张转回 COMMON，再推新的这张。
    if (gCtx.shadingRateImageBound != nullptr && gCtx.shadingRateImageBound != image
        && gCtx.shadingRateImageBound->resource) {
        transitionTo(ctx, gCtx.shadingRateImageBound->resource.Get(),
                     D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        gCtx.shadingRateImageBound = nullptr;
    }
    transitionTo(ctx, image->resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                 D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
    gCtx.shadingRateImageBound = image;
    list5->RSSetShadingRateImage(image->resource.Get());
    static bool logged = false;
    if (!logged) {
        logged = true;
        dbgLogInfo("setShadingRateImage: RSSetShadingRateImage bound");
    }
    return 1;
}

mcdr::Result<void*> createGraphicsPipeline(const PipelineDesc& desc) {
    if (auto d = ensureDevice(); !d) return mcdr::err("createGraphicsPipeline", errorMessage(d.error()));

    std::vector<uint8_t> vsBytes = desc.vsBytes;
    std::vector<uint8_t> psBytes = desc.psBytes;
    // P7 诊断：打印前 2 个真实管线 HLSL（仅 DEBUG 级别，帮助排查着色器数据流问题）。
    // 注意：dbgLogDebug 的格式化缓冲只有 2048 字节（完整行缓冲 2304），vs 与 ps 必须
    // 拆成两次调用，合在一次里 HLSL 全文会被静默截断。
    {
        static int hlslDump = 0;
        if (hlslDump < 2) {
            ++hlslDump;
            std::string vsStr(reinterpret_cast<const char*>(vsBytes.data()), vsBytes.size());
            std::string psStr(reinterpret_cast<const char*>(psBytes.data()), psBytes.size());
            dbgLogDebug("=== HLSL #%d vs (%zuB) ===\n%s", hlslDump, vsBytes.size(), vsStr.c_str());
            dbgLogDebug("=== HLSL #%d ps (%zuB) ===\n%s", hlslDump, psBytes.size(), psStr.c_str());
        }
    }
    // 1) HLSL -> DXIL。2026-10-25 起图形管线硬要求 SM 6.9，**没有 D3DCompile/SM 5.1 兜底**：
    //    前置条件不满足就直接报错（不再降级成 DXBC）。DXIL 与旧 DXBC 在 PSO 里同构，
    //    所以下面仍统一成裸指针 + 长度。
    const std::string smProfile = graphicsShaderModelProfile();
    if (smProfile.empty()) {
        return mcdr::err("createGraphicsPipeline", "createGraphicsPipeline: " + sm69RequirementText());
    }
    std::vector<uint8_t> vsDxil, psDxil;
    const std::string vsSrc(reinterpret_cast<const char*>(vsBytes.data()), vsBytes.size());
    const std::string psSrc(reinterpret_cast<const char*>(psBytes.data()), psBytes.size());
    auto vsDxc = compileHlslToDxil(vsSrc, "main", "vs_" + smProfile, vsDxil);
    if (!vsDxc) {
        return mcdr::err("createGraphicsPipeline",
                         "vs_" + smProfile + " (DXC): " + firstLine(vsDxc.error().message));
    }
    auto psDxc = compileHlslToDxil(psSrc, "main", "ps_" + smProfile, psDxil);
    if (!psDxc) {
        return mcdr::err("createGraphicsPipeline",
                         "ps_" + smProfile + " (DXC): " + firstLine(psDxc.error().message));
    }
    const void* vsCode = vsDxil.data();
    SIZE_T vsCodeSize = vsDxil.size();
    const void* psCode = psDxil.data();
    SIZE_T psCodeSize = psDxil.size();
    // 只在第一条管线时记一行，避免每个 PSO 都刷屏（管线创建可达上千次）。
    static bool loggedDxcGraphics = false;
    if (!loggedDxcGraphics) {
        loggedDxcGraphics = true;
        dbgLogInfo("createGraphicsPipeline: main graphics pipeline compiled with DXC SM %s (DXIL vs=%zuB ps=%zuB)",
                   smProfile.c_str(), vsDxil.size(), psDxil.size());
    }

    // 2) root signature：CBV 走 root descriptor（每 draw 直接
    //    SetGraphicsRootConstantBufferView，地址烘焙进命令列表，零描述符堆写入）；
    //    SRV 汇入单个 descriptor table（每批只写一次 + 一次
    //    SetGraphicsRootDescriptorTable）+ static sampler（仅 SAMPLED_IMAGE 条目）。
    //    root 参数索引：第 k 个 CBV -> k（按 binding 顺序稠密编号），SRV 表 -> cbvCount。
    //    shader register 仍取 binding 的 reg（= 条目序号），与 HLSL 声明一一对应。
    std::vector<uint8_t> cbvRegs;
    std::vector<D3D12_DESCRIPTOR_RANGE> srvRanges;
    std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
    cbvRegs.reserve(desc.bindings.size());
    srvRanges.reserve(desc.bindings.size());
    for (const PipelineDesc::Binding& b : desc.bindings) {
        if (b.type == 0) {
            cbvRegs.push_back(b.reg);
            continue;
        }
        D3D12_DESCRIPTOR_RANGE r{};
        r.NumDescriptors = 1;
        r.RegisterSpace = 0;
        r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        r.BaseShaderRegister = b.reg;
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges.push_back(r);
        if (b.type == 1) {
            D3D12_STATIC_SAMPLER_DESC s{};
            s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            // 地址模式用 WRAP 对齐 MC 的默认采样器（AbstractTexture 为 REPEAT/REPEAT）。
            // 此前用 CLAMP 会让任何 UV 超出 [0,1] 的纹理采样退化到边缘像素：天气粒子
            // (rain/snow) 的 v = y*0.25 + offset 可达 16~40，被 CLAMP 后整根雨柱都取
            // 边缘行 → 渲染成贯穿屏幕的实心竖条（BUG：雨一条一条）。改为 WRAP 后 v 正常
            // 平铺，恢复短虚线雨滴。对 UV 已在 [0,1] 内的纹理（图集/字体/lightmap）
            // WRAP 与 CLAMP 等价，无副作用。
            s.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            s.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            s.MipLODBias = 0.0f;
            s.MaxAnisotropy = 1;
            s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
            s.MinLOD = 0.0f;
            s.MaxLOD = D3D12_FLOAT32_MAX;
            s.ShaderRegister = b.reg;
            s.RegisterSpace = 0;
            s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            staticSamplers.push_back(s);
        }
    }
    // srvRanges 必须先填满再取 data()（否则 params 内的指针会因扩容而悬空）。
    std::vector<D3D12_ROOT_PARAMETER> params;
    params.reserve(cbvRegs.size() + 1);
    for (uint8_t reg : cbvRegs) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = reg;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    if (!srvRanges.empty()) {
        D3D12_ROOT_DESCRIPTOR_TABLE table{};
        table.NumDescriptorRanges = static_cast<UINT>(srvRanges.size());
        table.pDescriptorRanges = srvRanges.data();
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable = table;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(params.size());
    rsDesc.pParameters = params.empty() ? nullptr : params.data();
    rsDesc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
    rsDesc.pStaticSamplers = staticSamplers.empty() ? nullptr : staticSamplers.data();
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        return mcdr::err("createGraphicsPipeline",
                         "createGraphicsPipeline: D3D12SerializeRootSignature hr=0x" + hrText(hr));
    }
    ComPtr<ID3D12RootSignature> rootSig;
    hr = gCtx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        return mcdr::err("createGraphicsPipeline",
                         "createGraphicsPipeline: CreateRootSignature hr=0x" + hrText(hr));
    }

    // 3) 输入布局：语义名称来自 Java 侧 HLSL 解析；空串时回退到 TEXCOORD<location>
    //    D3D12 要求 SemanticName 不能以数字结尾，数字必须放在 SemanticIndex 字段。
    //    注意：SemanticName 是 const char*，必须指向持久化内存，不能指向局部 string 的 c_str()。
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
    inputLayout.reserve(desc.inputElements.size());
    std::vector<std::string> semanticStore;  // 持久化存储，防止 c_str() 悬空
    semanticStore.reserve(desc.inputElements.size());
    std::string inputDescStr;
    for (const PipelineDesc::InputElement& el : desc.inputElements) {
        D3D12_INPUT_ELEMENT_DESC ie{};
        // 解析 semantic：如 "TEXCOORD0" → name="TEXCOORD", index=0；"COLOR" → name="COLOR", index=0
        std::string baseName;
        UINT semanticIndex = 0;
        if (!el.semanticName.empty()) {
            const std::string& sn = el.semanticName;
            size_t lastNonDigit = sn.find_last_not_of("0123456789");
            if (lastNonDigit == std::string::npos) {
                // 全是数字（如 "0"），视为 index=0，name=""
                baseName = "";
            } else {
                baseName = sn.substr(0, lastNonDigit + 1);
                if (lastNonDigit + 1 < sn.size()) {
                    semanticIndex = static_cast<UINT>(std::stoi(sn.substr(lastNonDigit + 1)));
                }
            }
        } else {
            baseName = "TEXCOORD";
            semanticIndex = static_cast<UINT>(el.location);
        }
        semanticStore.push_back(baseName);
        ie.SemanticName = semanticStore.back().c_str();
        ie.SemanticIndex = semanticIndex;
        ie.Format = toDxgiVertexFormat(el.format);
        // P26 debug: 打印 format ordinal -> DXGI enum + 字符串名
        auto dxgiFmtName = [](DXGI_FORMAT f) -> const char* {
            switch (f) {
                case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
                case DXGI_FORMAT_R8_SNORM: return "R8_SNORM";
                case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
                case DXGI_FORMAT_R8G8_SNORM: return "R8G8_SNORM";
                case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
                case DXGI_FORMAT_R8G8B8A8_SNORM: return "R8G8B8A8_SNORM";
                case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
                case DXGI_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
                case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
                case DXGI_FORMAT_R16G16B16A16_UINT: return "R16G16B16A16_UINT";
                case DXGI_FORMAT_R32_UINT: return "R32_UINT";
                case DXGI_FORMAT_R32G32_UINT: return "R32G32_UINT";
                case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
                case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
                case DXGI_FORMAT_R32G32B32_FLOAT: return "R32G32B32_FLOAT";
                case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
                case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
                case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
                case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
                case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
                case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
                case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
                case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
                case DXGI_FORMAT_R8G8B8A8_UINT: return "R8G8B8A8_UINT";
                case DXGI_FORMAT_R8G8B8A8_SINT: return "R8G8B8A8_SINT";
                default: return "UNKNOWN";
            }
        };
        dbgLogDebug("P26 DIAG: ord=%d -> dxgi_fmt=%u (%s) semantic=%s", el.format,
                     static_cast<unsigned>(ie.Format), dxgiFmtName(ie.Format), ie.SemanticName);
        ie.InputSlot = static_cast<UINT>(el.binding);
        ie.AlignedByteOffset = static_cast<UINT>(el.offset);
        if (el.stepRate > 0) {
            ie.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
            ie.InstanceDataStepRate = static_cast<UINT>(el.stepRate);
        } else {
            ie.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            ie.InstanceDataStepRate = 0;
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "  loc=%d bind=%d fmt=%d off=%d stride=%d step=%d semantic=%s idx=%u\n",
                 el.location, el.binding, el.format, el.offset, el.stride, el.stepRate,
                 el.semanticName.empty() ? "(fallback TEXCOORD)" : el.semanticName.c_str(), semanticIndex);
        inputDescStr += buf;
        inputLayout.push_back(ie);
    }
    // 修正 stride：Java 侧 getVertexSize() 可能不准确，按 DXGI_FORMAT 实际字节数计算 end-offset。
    // 修正 stride：按 gpuFormat ordinal 直接算字节数，不依赖 DXGI 枚举值（某些 SDK 枚举值非标准）。
    auto ordinalByteSize = [](int ord) -> UINT {
        switch (ord) {
            case 36: return 12u;  // RGB32_UINT
            case 37: return 12u;  // RGB32_SINT
            case 45: return 8u;  // RG32_FLOAT
            case 46: return 12u;  // RGB32_FLOAT
            case 47: return 16u;  // RGBA32_FLOAT
            case 44: return 4u;  // R32_FLOAT
            case 40: return 2u;  // R16_FLOAT
            case 41: return 4u;  // RG16_FLOAT
            case 42: return 8u;  // RGB16_FLOAT
            case 43: return 8u;  // RGBA16_FLOAT
            case 48: return 4u;  // RGB10A2_UNORM
            case 49: return 4u;  // RGB10A2_UINT
            case 50: return 4u;  // RG11B10_FLOAT
            case 0: return 1u;  // R8_UNORM
            case 1: return 1u;  // R8_SNORM
            case 2: return 2u;  // RG8_UNORM
            case 3: return 2u;  // RG8_SNORM
            case 4: return 4u;  // RGB8_UNORM
            case 5: return 4u;  // RGB8_SNORM
            case 6: return 4u;  // RGBA8_UNORM
            case 7: return 4u;  // RGBA8_SNORM
            case 8: return 2u;  // R16_UNORM
            case 9: return 2u;  // R16_SNORM
            case 10: return 4u;  // RG16_UNORM
            case 11: return 4u;  // RG16_SNORM
            case 12: return 8u;  // RGB16_UNORM
            case 13: return 8u;  // RGB16_SNORM
            case 14: return 8u;  // RGBA16_UNORM
            case 15: return 8u;  // RGBA16_SNORM
            case 16: return 1u;  // R8_UINT
            case 17: return 1u;  // R8_SINT
            case 18: return 2u;  // RG8_UINT
            case 19: return 2u;  // RG8_SINT
            case 20: return 4u;  // RGB8_UINT
            case 21: return 4u;  // RGB8_SINT
            case 22: return 4u;  // RGBA8_UINT
            case 23: return 4u;  // RGBA8_SINT
            case 24: return 2u;  // R16_UINT
            case 25: return 2u;  // R16_SINT
            case 26: return 4u;  // RG16_UINT
            case 27: return 4u;  // RG16_SINT
            case 28: return 8u;  // RGB16_UINT
            case 29: return 8u;  // RGB16_SINT
            case 30: return 8u;  // RGBA16_UINT
            case 31: return 8u;  // RGBA16_SINT
            case 32: return 4u;  // R32_UINT
            case 33: return 4u;  // R32_SINT
            case 34: return 8u;  // RG32_UINT
            case 35: return 8u;  // RG32_SINT
            case 38: return 16u;  // RGBA32_UINT
            case 39: return 16u;  // RGBA32_SINT
            case 51: return 4u;  // D32_FLOAT
            case 52: return 4u;  // D32_FLOAT_S8
            case 53: return 4u;  // D24_UNORM_S8
            case 54: return 2u;  // D16_UNORM
            default: return 4u;  // fallback
        }
    };
    // P192：逐 slot 的步长必须分开算。26.3 的地形管线把 ChunkPosition/ChunkVisibility
    // 挪成了逐实例顶点属性（slot 1，stride 16），而同一条管线的 slot 0（Position/Color/
    // UV0/UV2）是 stride 28。原先这里把所有元素 offset+size 的最大值当成**每个** slot 的
    // 步长，slot 1 于是被按 28 字节解释 → 每个区块读到的 ChunkPosition 都是错位的字节，
    // 表现为「几何本身正常，但区块整体位置乱跳」。Java 侧每个元素都带了所属 slot 的
    // stride，优先用它；没给时退回该 slot 内元素的 offset+size 上界。
    UINT slotStride[16] = {};
    bool anyInputSlot = false;
    for (const PipelineDesc::InputElement& el : desc.inputElements) {
        if (el.binding < 0 || el.binding >= 16) continue;
        const UINT end = static_cast<UINT>(el.offset) + ordinalByteSize(el.format);
        const UINT candidate = el.stride > 0 ? static_cast<UINT>(el.stride) : end;
        if (candidate > slotStride[el.binding]) slotStride[el.binding] = candidate;
        anyInputSlot = true;
    }
    if (anyInputSlot) {
        // 重新构建 inputLayout，语义名称从 semanticStore 恢复（避免悬空指针）。
        // 先清空 semanticStore 以便重新填充。
        semanticStore.clear();
        semanticStore.reserve(desc.inputElements.size());
        inputDescStr.clear();
        inputLayout.clear();
        for (size_t k = 0; k < desc.inputElements.size(); ++k) {
            const PipelineDesc::InputElement& el = desc.inputElements[k];
            D3D12_INPUT_ELEMENT_DESC ie{};
            std::string baseName;
            UINT semanticIndex = 0;
            if (!el.semanticName.empty()) {
                const std::string& sn = el.semanticName;
                size_t lastNonDigit = sn.find_last_not_of("0123456789");
                if (lastNonDigit == std::string::npos) {
                    baseName = "";
                } else {
                    baseName = sn.substr(0, lastNonDigit + 1);
                    if (lastNonDigit + 1 < sn.size()) {
                        semanticIndex = static_cast<UINT>(std::stoi(sn.substr(lastNonDigit + 1)));
                    }
                }
            } else {
                baseName = "TEXCOORD";
                semanticIndex = static_cast<UINT>(el.location);
            }
            semanticStore.push_back(baseName);
            ie.SemanticName = semanticStore.back().c_str();
            ie.SemanticIndex = semanticIndex;
            ie.Format = toDxgiVertexFormat(el.format);
            // P26 debug (second loop)
            dbgLogDebug("P26 DIAG2: ord=%d -> dxgi_fmt=%u semantic=%s", el.format,
                         static_cast<unsigned>(ie.Format), ie.SemanticName);
            ie.InputSlot = static_cast<UINT>(el.binding);
            ie.AlignedByteOffset = static_cast<UINT>(el.offset);
            if (el.stepRate > 0) {
                ie.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
                ie.InstanceDataStepRate = static_cast<UINT>(el.stepRate);
            } else {
                ie.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                ie.InstanceDataStepRate = 0;
            }
            char buf[128];
            snprintf(
                buf, sizeof(buf), "  loc=%d bind=%d fmt=%d off=%d stride=%d step=%d semantic=%s idx=%u\n",
                el.location, el.binding, el.format, el.offset,
                (el.binding >= 0 && el.binding < 16) ? slotStride[el.binding] : 0u, el.stepRate,
                el.semanticName.empty() ? "(fallback TEXCOORD)" : el.semanticName.c_str(), semanticIndex);
            inputDescStr += buf;
            inputLayout.push_back(ie);
        }
    }
    dbgLogInfo("createGraphicsPipeline: inputElements=%zu\n%s", desc.inputElements.size(),
               inputDescStr.c_str());

    // 4) PSO 共享状态：混合 + 光栅化
    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = TRUE;
    int numRT = 0;
    for (int i = 0; i < desc.colorCount; ++i) {
        const PipelineDesc::ColorTarget& ct = desc.colorTargets[i];
        if (ct.format < 0) continue;  // 未使用槽位
        numRT = i + 1;
        D3D12_RENDER_TARGET_BLEND_DESC& rt = blend.RenderTarget[i];
        rt.BlendEnable = ct.blendEnabled;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = toD3d12BlendFactor(ct.srcColor);
        rt.DestBlend = toD3d12BlendFactor(ct.dstColor);
        rt.BlendOp = toD3d12BlendOp(ct.colorOp);
        rt.SrcBlendAlpha = toD3d12BlendFactor(ct.srcAlpha);
        rt.DestBlendAlpha = toD3d12BlendFactor(ct.dstAlpha);
        rt.BlendOpAlpha = toD3d12BlendOp(ct.alphaOp);
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = ct.writeMask;  // MC 掩码位序与 D3D12 一致（R=1,G=2,B=4,A=8）
    }

    D3D12_RASTERIZER_DESC rs{};
    rs.FillMode = (desc.polygonMode == 1) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    rs.CullMode = desc.cullEnabled ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    // MC 前端为逆时针（官方 Vulkan frontFace=CCW），D3D12 默认按顺时针判定，需翻转。
    rs.FrontCounterClockwise = TRUE;
    rs.DepthBias = 0;  // P6 细化 depthBiasConstant/ScaleFactor
    rs.DepthBiasClamp = 0.0f;
    rs.SlopeScaledDepthBias = 0.0f;
    rs.DepthClipEnable = TRUE;
    rs.MultisampleEnable = FALSE;
    rs.AntialiasedLineEnable = FALSE;
    rs.ForcedSampleCount = 0;
    rs.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // 5) 双 PSO：withDepth 总是创建（DSV=D32_FLOAT，镜像官方 depthAttachmentFormat=126）；
    //    仅当管线无深度状态时再创建 withoutDepth（DSV=UNKNOWN，无深度附件）。
    auto pipeline = std::make_unique<Dx12Pipeline>();
    pipeline->rootSignature = rootSig;
    pipeline->topology = desc.topology;

    // P192：每个 slot 用自己的步长（详见上面 slotStride 的注释）。
    for (UINT slot = 0; slot < 16; ++slot) {
        if (slotStride[slot] != 0) pipeline->vertexStrides[slot] = slotStride[slot];
    }

    auto buildPso = [&](DXGI_FORMAT dsvFormat, bool depthEnable, D3D12_DEPTH_WRITE_MASK depthWrite,
                        D3D12_COMPARISON_FUNC depthFunc,
                        ComPtr<ID3D12PipelineState>& out) -> mcdr::Result<void> {
        D3D12_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = depthEnable;
        ds.DepthWriteMask = depthWrite;
        ds.DepthFunc = depthFunc;
        ds.StencilEnable = FALSE;
        ds.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        ds.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = rootSig.Get();
        pso.VS.pShaderBytecode = vsCode;
        pso.VS.BytecodeLength = vsCodeSize;
        pso.PS.pShaderBytecode = psCode;
        pso.PS.BytecodeLength = psCodeSize;
        pso.InputLayout.NumElements = static_cast<UINT>(inputLayout.size());
        pso.InputLayout.pInputElementDescs = inputLayout.empty() ? nullptr : inputLayout.data();
        pso.BlendState = blend;
        pso.RasterizerState = rs;
        pso.DepthStencilState = ds;
        pso.PrimitiveTopologyType = toTopologyType(desc.topology);
        pso.NumRenderTargets = static_cast<UINT>(numRT);
        for (int i = 0; i < desc.colorCount; ++i) {
            const PipelineDesc::ColorTarget& ct = desc.colorTargets[i];
            pso.RTVFormats[i] = ct.format < 0 ? DXGI_FORMAT_UNKNOWN : toDxgiFormat(ct.format);
        }
        pso.DSVFormat = dsvFormat;
        pso.SampleDesc.Count = 1;
        pso.SampleDesc.Quality = 0;
        pso.SampleMask = UINT_MAX;
        // 诊断：打印 PSO 关键字段
        {
            std::string descInfo =
                "inputElements=" + std::to_string(inputLayout.size()) + " numRT=" + std::to_string(numRT)
                + " colorCount=" + std::to_string(desc.colorCount)
                + " topology=" + std::to_string(desc.topology) + " hasDepth=" + std::to_string(desc.hasDepth)
                + " dsv=" + std::to_string(static_cast<int>(dsvFormat));
            for (int i = 0; i < desc.colorCount; ++i) {
                descInfo +=
                    " rt" + std::to_string(i) + "=" + std::to_string(static_cast<int>(pso.RTVFormats[i]));
            }
            dbgLogDebug("createGraphicsPipeline: PSO=[%s]\n", descInfo.c_str());
        }
        // 诊断：打印 HLSL 前 400 字符，便于检查语义声明格式
        {
            std::string vsStr(reinterpret_cast<const char*>(vsBytes.data()), vsBytes.size());
            std::string psStr(reinterpret_cast<const char*>(psBytes.data()), psBytes.size());
            int vsLen = std::min(static_cast<int>(vsStr.size()), 400);
            int psLen = std::min(static_cast<int>(psStr.size()), 400);
            dbgLogDebug("createGraphicsPipeline: vs(%zuB)=[%.*s...]\nps(%zuB)=[%.*s...]", vsBytes.size(), vsLen,
                   vsStr.c_str(), psBytes.size(), psLen, psStr.c_str());
        }
        // 详细 PSO 字段诊断（定位 E_INVALIDARG 根因）
        {
            char buf[512];
            snprintf(buf, sizeof(buf),
                     "  pso_fields: rootSig=%p vsPtr=%p vsSize=%u psPtr=%p psSize=%u "
                     "inputElts=%u rtCount=%u dsvFmt=%d topType=%d "
                     "blendIndep=%d depthEnable=%d depthWrite=%d depthFunc=%d",
                     reinterpret_cast<void*>(pso.pRootSignature), const_cast<void*>(pso.VS.pShaderBytecode),
                     static_cast<UINT>(pso.VS.BytecodeLength), const_cast<void*>(pso.PS.pShaderBytecode),
                     static_cast<UINT>(pso.PS.BytecodeLength), pso.InputLayout.NumElements,
                     pso.NumRenderTargets, static_cast<int>(pso.DSVFormat),
                     static_cast<int>(pso.PrimitiveTopologyType),
                     static_cast<int>(pso.BlendState.IndependentBlendEnable),
                     static_cast<int>(pso.DepthStencilState.DepthEnable),
                     static_cast<int>(pso.DepthStencilState.DepthWriteMask),
                     static_cast<int>(pso.DepthStencilState.DepthFunc));
            dbgLogDebug(buf);
            // 打印每个输入元素的完整描述
            for (UINT i = 0; i < pso.InputLayout.NumElements; ++i) {
                const D3D12_INPUT_ELEMENT_DESC& ie = inputLayout[i];
                snprintf(buf, sizeof(buf),
                         "  inputEl[%u]: fmt=%d slot=%u offset=%u class=%u step=%u semantic=%s", i,
                         static_cast<int>(ie.Format), ie.InputSlot, ie.AlignedByteOffset, ie.InputSlotClass,
                         ie.InstanceDataStepRate, ie.SemanticName);
                dbgLogDebug(buf);
            }
        }
        ++gStatPsoCreate;
        HRESULT h = gCtx.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out));
        if (FAILED(h)) {
            dbgLogDebug("  === D3D12 validation messages ===");
            if (gCtx.infoQueue) {
                UINT64 count = gCtx.infoQueue->GetNumStoredMessages();
                dbgLogDebug("  InfoQueue stored=%llu", static_cast<unsigned long long>(count));
                UINT64 start = count > 64 ? count - 64 : 0;
                for (UINT64 i = start; i < count; ++i) {
                    SIZE_T len = 0;
                    if (FAILED(gCtx.infoQueue->GetMessage(static_cast<UINT>(i), nullptr, &len))) continue;
                    std::vector<char> buf(len > 0 ? len : 1);
                    D3D12_MESSAGE* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                    if (SUCCEEDED(gCtx.infoQueue->GetMessage(static_cast<UINT>(i), msg, &len))) {
                        const char* sev = "?";
                        switch (msg->Severity) {
                            case D3D12_MESSAGE_SEVERITY_CORRUPTION: sev = "CORRUPTION"; break;
                            case D3D12_MESSAGE_SEVERITY_ERROR: sev = "ERROR"; break;
                            case D3D12_MESSAGE_SEVERITY_WARNING: sev = "WARNING"; break;
                            default: break;
                        }
                        dbgLogDebug("  InfoQueue[%s] %s", sev, msg->pDescription ? msg->pDescription : "");
                    }
                }
            }
            return mcdr::err("createGraphicsPipeline",
                             "createGraphicsPipeline: CreateGraphicsPipelineState hr=" + hrText(h));
        }
        return {};
    };

    D3D12_DEPTH_WRITE_MASK depthWrite =
        (desc.hasDepth && desc.depthWrite) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    // 注意：这里只需要 wDepthFunc —— 原先还并存一个未使用的 depthFunc（同表达式），
    // 它在两条 buildPso 调用里都没被引用，/W4 报 C4189，已删除。

    // 始终同时创建两个 PSO：withDepth（DSV=D32_FLOAT）和
    // withoutDepth（DSV=UNKNOWN，depthEnable=false），以支持运行时 hasDepth 与
    // 编译时 desc.hasDepth 不一致的场景（如 pipeline/gui 编译时无深度，但渲染
    // pass 请求 hasDepth=true）。
    // 关键：withDepth 的 depthEnable/depthFunc 必须跟随 desc.hasDepth，
    // 否则无深度管线（如 GUI）会被强制开启深度测试并拿到无效 depthFunc（如 8）→ 全黑。
    bool wDepthEnable = desc.hasDepth;
    D3D12_COMPARISON_FUNC wDepthFunc = desc.hasDepth
                                           ? toD3d12Compare(static_cast<uint8_t>(desc.depthCompareOp))
                                           : D3D12_COMPARISON_FUNC_ALWAYS;
    auto wDepthPso = buildPso(DXGI_FORMAT_D32_FLOAT, wDepthEnable, depthWrite, wDepthFunc,
                              pipeline->withDepth);
    if (!wDepthPso) return std::unexpected<mcdr::Error>(std::move(wDepthPso.error()));
    auto noDepthPso = buildPso(DXGI_FORMAT_UNKNOWN, false, D3D12_DEPTH_WRITE_MASK_ZERO,
                               D3D12_COMPARISON_FUNC_ALWAYS, pipeline->withoutDepth);
    if (!noDepthPso) return std::unexpected<mcdr::Error>(std::move(noDepthPso.error()));
    return static_cast<void*>(pipeline.release());
}

void destroyPipeline(Dx12Pipeline* pipeline) {
    DBG_LOG_DEBUG("destroyPipeline: pso=%p", (void*)pipeline);
    delete pipeline;
    DBG_LOG_DEBUG("destroyPipeline: done");
}

// ---------------------------------------------------------------------------
// P6: draw 命令录制（渲染 pass 内）
// ---------------------------------------------------------------------------

mcdr::Result<void> setPipeline(CommandContext* ctx, Dx12Pipeline* pipeline, bool hasDepth) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("setPipeline", "setPipeline: no open command list");
    }
    if (!pipeline) {
        return mcdr::err("setPipeline", "setPipeline: null pipeline");
    }
    ID3D12PipelineState* pso = hasDepth ? pipeline->withDepth.Get() : pipeline->withoutDepth.Get();
    if (!pso) pso = pipeline->withDepth.Get();  // 无深度渲染但管线未建 withoutDepth 时回退
    if (!pipeline->rootSignature) {
        return mcdr::err("setPipeline", "setPipeline: null root signature");
    }
    // P64：快速路径——同一命令列表内重复设置同一 PSO 时，root signature / PSO /
    // topology 三者均未变，跳过 3 次驱动调用（GUI/串行路径同一管线连续 draw 常见）。
    // 比较 PSO 裸指针：管线被销毁重建会得到新的 ID3D12PipelineState，不会误命中。
    if (pso == ctx->cachedPso) {
        ctx->currentPipeline = pipeline;  // 保持 setVertexBuffer 的 stride 修正
        return {};
    }
    // P6 崩溃修复（第 5 轮）：D3D12 规定使用任何 root 参数（SetGraphicsRootDescriptorTable
    // 等）前必须先 SetGraphicsRootSignature。此前缺失 → UMD 首次真实 draw 时对 NULL root
    // signature 解引用崩溃（hs_err：NVIDIA UMD 内读 NULL+0x2b88，AV，PC 0x7ffcd70e8fa3）。
    ctx->commandList->SetGraphicsRootSignature(pipeline->rootSignature.Get());
    ctx->commandList->SetPipelineState(pso);
    ctx->currentPipeline = pipeline;
    ctx->cachedPso = pso;
    // P6 纯色黑屏修复：D3D12 命令列表初始 topology 是 UNDEFINED，必须显式
    // IASetPrimitiveTopology，否则 GPU 丢弃全部图元（只有 clear 色可见）。
    // P72：mesh 管线（AS/MS）没有 IA 阶段，PSO 里也没有输入布局，跳过 IA 状态设置。
    D3D12_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    if (!pipeline->isMesh) {
        topo = toPrimitiveTopology(pipeline->topology);
        ctx->commandList->IASetPrimitiveTopology(topo);
    }
    // P27：setPipeline 每 draw 调用一次（可上万次/帧），降级为 DEBUG 日志。
    DBG_LOG_DEBUG("setPipeline rootSig=%p pso=%p hasDepth=%d topoOrdinal=%d topo=%d",
                  (void*)pipeline->rootSignature.Get(), (void*)pso, (int)hasDepth, (int)pipeline->topology,
                  (int)topo);
    return {};
}

mcdr::Result<void> setScissor(CommandContext* ctx, int x, int y, int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("setScissor", "setScissor: no open command list");
    }
    D3D12_RECT rect{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x) + w,
                    static_cast<LONG>(y) + h};
    ctx->commandList->RSSetScissorRects(1, &rect);
    return {};
}

mcdr::Result<void> setViewport(CommandContext* ctx, int x, int y, int w, int h) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("setViewport", "setViewport: no open command list");
    }
    if (w <= 0 || h <= 0) {
        return mcdr::err("setViewport", "setViewport: empty viewport");
    }
    // 与 beginRenderPass 内的视口设置保持同一形状（TopLeftY=0，深度范围 0..1）。
    D3D12_VIEWPORT vp{static_cast<FLOAT>(x),
                      static_cast<FLOAT>(y),
                      static_cast<FLOAT>(w),
                      static_cast<FLOAT>(h),
                      0.0f,
                      1.0f};
    ctx->commandList->RSSetViewports(1, &vp);
    return {};
}

mcdr::Result<void> setVertexBuffer(CommandContext* ctx, int slot, Dx12Object* buffer, long long offset,
                                   int stride) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("setVertexBuffer", "setVertexBuffer: no open command list");
    }
    if (slot < 0 || slot >= 16) {
        return mcdr::err("setVertexBuffer", "setVertexBuffer: slot out of range");
    }
    if (!buffer || buffer->kind != Dx12Object::Kind::Buffer) {
        return mcdr::err("setVertexBuffer", "setVertexBuffer: invalid buffer handle");
    }
    if (offset < 0 || offset >= buffer->size) {
        return mcdr::err("setVertexBuffer", "setVertexBuffer: offset out of bounds");
    }
    transitionBufferTo(ctx, buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    // P6：使用管线中记录的修正 stride（覆盖 Java 传入的错误值）
    UINT effectiveStride = static_cast<UINT>(stride);
    if (ctx->currentPipeline && ctx->currentPipeline->vertexStrides.count(slot))
        effectiveStride = ctx->currentPipeline->vertexStrides[slot];
    D3D12_VERTEX_BUFFER_VIEW vb;
    vb.BufferLocation = buffer->resource->GetGPUVirtualAddress() + static_cast<UINT64>(offset);
    vb.SizeInBytes = static_cast<UINT>(buffer->size - offset);
    vb.StrideInBytes = effectiveStride;
    ctx->commandList->IASetVertexBuffers(static_cast<UINT>(slot), 1, &vb);
    DBG_LOG_DEBUG("setVertexBuffer: slot=%d buf=%p size=%lld off=%lld javaStride=%d effStride=%d heap=%d",
                  slot, (void*)buffer, (long long)buffer->size, (long long)offset, (int)stride,
                  (int)effectiveStride, (int)buffer->heapType);
    return {};
}

mcdr::Result<void> setIndexBuffer(CommandContext* ctx, Dx12Object* buffer, int indexType) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("setIndexBuffer", "setIndexBuffer: no open command list");
    }
    if (!buffer || buffer->kind != Dx12Object::Kind::Buffer) {
        return mcdr::err("setIndexBuffer", "setIndexBuffer: invalid buffer handle");
    }
    transitionBufferTo(ctx, buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    D3D12_INDEX_BUFFER_VIEW ib{};
    ib.BufferLocation = buffer->resource->GetGPUVirtualAddress();
    ib.SizeInBytes = static_cast<UINT>(buffer->size);
    ib.Format = indexType == 1 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    ctx->commandList->IASetIndexBuffer(&ib);
    // P6 诊断：确认每帧 draw 前确实绑定了索引缓冲（内容尺寸/索引宽度）。
    DBG_LOG_DEBUG("setIndexBuffer: buf=%p size=%lld idxWidth=%d heap=%d", (void*)buffer,
                  (long long)buffer->size, indexType == 1 ? 4 : 2, (int)buffer->heapType);
    return {};
}

mcdr::Result<void> pushDescriptors(CommandContext* ctx, const std::vector<DrawBinding>& bindings) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("pushDescriptors", "pushDescriptors: no open command list");
    }
    UINT count = static_cast<UINT>(bindings.size());
    if (count == 0) return {};
    // B：CBV 走 root descriptor（不占堆槽位），只有 SRV 需要写瞬时描述符堆。
    UINT cbvCount = 0;
    UINT srvCount = 0;
    for (const DrawBinding& b : bindings) {
        if (b.type == 0) {
            ++cbvCount;
        } else {
            ++srvCount;
        }
    }
    // P243：SRV 集合与同一命令列表上一次推送完全一致（且全是 texture view）⇒ 复用旧槽位。
    // 加载屏（ELSDrawCollector）与图集动画上传会在**一次不提交**的命令列表里推上万个相同的
    // SRV 表（整张图集只有一张 view），旧逻辑每次新分配槽位，最终把每帧段用尽并抛异常。
    // 槽位在列表内只增不减，旧内容不会被覆写，所以复用是安全的；CBV 仍逐次重写 root 描述符。
    bool reuseSrvTable = false;
    if (srvCount > 0 && ctx->lastSrvCount == srvCount) {
        reuseSrvTable = true;
        size_t viewIndex = 0;
        for (const DrawBinding& b : bindings) {
            if (b.type == 0) {
                continue;
            }
            if (b.type != 1 || viewIndex >= ctx->lastSrvViews.size()
                || ctx->lastSrvViews[viewIndex].first != b.view
                || !b.view || ctx->lastSrvViews[viewIndex].second != b.view->cpuHandle.ptr) {
                reuseSrvTable = false;
                break;
            }
            ++viewIndex;
        }
    }
    if (!reuseSrvTable && ctx->nextDrawSlot + srvCount > syncRingCapacity()) {
        // 兜底：真到上限也不要把渲染线程打死。复用上一张同样大的表并告警——这一批 draw 的
        // 贴图可能不对（加载屏阶段可接受），但不会像旧逻辑那样抛异常让客户端崩掉。
        static int p243ExhaustWarned = 0;
        if (p243ExhaustWarned < 8) {
            ++p243ExhaustWarned;
            dbgLogWarn("P243 descriptor ring exhausted (next=%u need=%u cap=%u fence=%llu) - "
                       "reusing previous SRV table",
                       static_cast<unsigned>(ctx->nextDrawSlot), static_cast<unsigned>(srvCount),
                       static_cast<unsigned>(syncRingCapacity()),
                       static_cast<unsigned long long>(ctx->fenceValue));
        }
        if (ctx->lastSrvCount == srvCount && srvCount > 0) {
            reuseSrvTable = true;
        } else {
            // 诊断：带上槽位画像，便于定位是哪条路径的 burst 超限（图集上传等）。
            return mcdr::err("pushDescriptors",
                             "pushDescriptors: draw descriptor heap exhausted for this frame (next="
                                 + std::to_string(static_cast<long long>(ctx->nextDrawSlot))
                                 + " need=" + std::to_string(static_cast<long long>(srvCount))
                                 + " cap=" + std::to_string(static_cast<long long>(syncRingCapacity()))
                                 + " section=" + std::to_string(static_cast<long long>(kDrawHeapPerFrame))
                                 + " fence=" + std::to_string(static_cast<long long>(ctx->fenceValue))
                                 + ")");
        }
    }
    // P16 诊断：每帧首 pushDescriptors 打印 binding 数量（确认 uniform/texture 被推送）
    static int pdFrameCount = 0;
    if (static_cast<int>(ctx->fenceValue) != pdFrameCount) {
        pdFrameCount = static_cast<int>(ctx->fenceValue);
        dbgLogDebug("pushDescriptors[%llu]: count=%u firstBuf=%p firstView=%p",
                    static_cast<unsigned long long>(ctx->fenceValue), count,
                    (count > 0 ? reinterpret_cast<void*>(bindings[0].buffer) : nullptr),
                    (count > 0 ? reinterpret_cast<void*>(bindings[0].view) : nullptr));
        // P21 诊断：打印每个 binding 的 buffer/offset，定位 UBO shader 读取偏移
        for (UINT j = 0; j < count && j < 8; ++j) {
            const DrawBinding& bj = bindings[j];
            const char* tname = bj.type == 0 ? "CBV" : bj.type == 1 ? "SRV" : bj.type == 2 ? "BUF" : "?";
            dbgLogDebug("pushDesc BIND[%u]: type=%s buf=%p off=%lld len=%lld view=%p heap=%d",
                        static_cast<unsigned>(j), tname,
                        reinterpret_cast<void*>(bj.buffer ? bj.buffer : nullptr), bj.offset, bj.length,
                        reinterpret_cast<void*>(bj.view ? bj.view : nullptr),
                        (bj.buffer ? bj.buffer->heapType : -1));
        }
    }
    const UINT srvSlotOffset = reuseSrvTable ? ctx->lastSrvSlotOffset : ctx->nextDrawSlot;
    SIZE_T base = static_cast<SIZE_T>(ctx->drawHeapSlotBase + srvSlotOffset) * gCtx.drawInc;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = gCtx.drawHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += base;
    // 稠密编号：cbvIdx -> root 参数索引；srvIdx -> SRV 堆槽 / SRV 表内序号。
    UINT cbvIdx = 0;
    UINT srvIdx = 0;
    for (UINT i = 0; i < count; ++i) {
        const DrawBinding& b = bindings[i];
        DBG_LOG_DEBUG("pushDesc[%u] type=%d buf=%p view=%p off=%lld len=%lld texel=%d", (unsigned)i,
                      (int)b.type, (void*)b.buffer, (void*)b.view, (long long)b.offset, (long long)b.length,
                      b.texelFormat);
        switch (b.type) {
            case 0: {  // CBV -> root descriptor（地址烘焙进命令列表，不写描述符堆）
                if (!b.buffer || b.buffer->kind != Dx12Object::Kind::Buffer || !b.buffer->resource) {
                    dbgLogError("pushDesc[%u] INVALID CBV buffer", static_cast<unsigned>(i));
                    return mcdr::err("pushDescriptors",
                                     "pushDescriptors: invalid buffer for CBV entry " + std::to_string(i));
                }
                transitionBufferTo(ctx, b.buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                D3D12_GPU_VIRTUAL_ADDRESS va =
                    b.buffer->resource->GetGPUVirtualAddress() + static_cast<UINT64>(b.offset);
                // D3D12 要求 CBV 的 GPU 地址 256 字节对齐（root descriptor 走同一条约束）。
                // 缓冲基址本身永远是对齐的，所以问题只可能出在 b.offset 上：这里明确拦住。
                // 不放行的话调试层只会报 "pBufferLocation must be 256-byte aligned"，零售驱动
                // 下则是 shader 读到错位数据（表现成变换/贴图乱掉），排查成本远高于一次报错。
                constexpr UINT64 kCbvAlignment = 256;
                if (va % kCbvAlignment != 0) {
                    dbgLogError("pushDesc[%u] CBV not 256B aligned: off=%lld va=%llx", static_cast<unsigned>(i),
                                b.offset, static_cast<unsigned long long>(va));
                    return mcdr::err("pushDescriptors",
                                     "pushDescriptors: CBV entry " + std::to_string(i) + " is not 256-byte aligned"
                                     " (buffer offset=" + std::to_string(b.offset) +
                                         "; D3D12 requires the address to be a multiple of 256)");
                }
                // P20：诊断 CBV 地址（每帧首 pushDescriptors 打印）
                static UINT64 lastPdFrame = 0;
                if (ctx->fenceValue != lastPdFrame) {
                    lastPdFrame = ctx->fenceValue;
                    dbgLogDebug("pushDesc CBV[%u]: bufGVA=%llx off=%lld rootVA=%llx heap=%d",
                                static_cast<unsigned>(i),
                                static_cast<unsigned long long>(b.buffer->resource->GetGPUVirtualAddress()),
                                b.offset, static_cast<unsigned long long>(va),
                                static_cast<int>(b.buffer->heapType));
                    // P21：额外诊断 binding[1]（DynamicTransforms UBO）的 offset，确认 shader 读取位置
                    if (i == 0 && count > 1) {
                        const DrawBinding& b1 = bindings[1];
                        dbgLogDebug("pushDesc UBO_BIND[1]: bufGVA=%llx off=%lld len=%lld heap=%d",
                                    b1.buffer ? static_cast<unsigned long long>(
                                                    b1.buffer->resource->GetGPUVirtualAddress())
                                              : 0ULL,
                                    b1.offset, b1.length, (b1.buffer ? b1.buffer->heapType : -1));
                    }
                }
                ctx->commandList->SetGraphicsRootConstantBufferView(cbvIdx, va);
                ++cbvIdx;
                break;
            }
            case 1: {  // SRV：复制 texture view 的现有描述符
                D3D12_CPU_DESCRIPTOR_HANDLE dst{cpu.ptr + static_cast<SIZE_T>(srvIdx) * gCtx.drawInc};
                if (!b.view || b.view->cpuHandle.ptr == 0) {
                    dbgLogError("pushDesc[%u] INVALID view", static_cast<unsigned>(i));
                    return mcdr::err("pushDescriptors",
                                     "pushDescriptors: missing view for SRV entry " + std::to_string(i));
                }
                // 关键修复：普通纹理（字体/logo/panorama 等）上传后处于 COMMON，
                // 采样前必须显式 transition 到 SHADER_RESOURCE，否则 D3D12 在
                // COMMON 状态采样是未定义行为（NVIDIA 驱动静默返回黑）→ 全黑屏。
                if (b.view->sourceTexture) {
                    if (b.view->sourceTexture->usage & 16) {
                        dbgLogDebug("pushDesc SRV: CUBE view=%p srcTex=%p -> SHADER_RESOURCE",
                                    reinterpret_cast<void*>(b.view),
                                    reinterpret_cast<void*>(b.view->sourceTexture));
                    }
                    // P109（#538 取证）：若这条 SRV 的源纹理正是**当前 render pass 的附件**，
                    // 把它切到 SHADER_RESOURCE 就会让"仍绑定着的 RTV/DSV"处于非附件状态 ⇒
                    // 之后每一条 draw 都会被调试层判成 #538（把 0xC0 当 RENDER_TARGET /
                    // DEPTH_WRITE）。这在 D3D12 里是非法自采样（sampling a bound attachment）。
                    // 这里只记账不改行为：先拿到"是谁在采样自己的附件"，再决定是跳过过渡还是
                    // 拆 pass —— 盲目跳过会让采样读到未定义内容，比报错更难查。
                    {
                        ID3D12Resource* srvRes = b.view->sourceTexture->resource.Get();
                        bool p109IsAttachment = false;
                        for (Dx12Object* p109Att : ctx->activeColorTargets) {
                            if (p109Att && p109Att->resource.Get() == srvRes) {
                                p109IsAttachment = true;
                                break;
                            }
                        }
                        if (!p109IsAttachment && ctx->activeDepthTarget
                            && ctx->activeDepthTarget->resource.Get() == srvRes) {
                            p109IsAttachment = true;
                        }
                        if (p109IsAttachment) {
                            static int p109SelfSamples = 0;
                            if (p109SelfSamples < 5) {
                                ++p109SelfSamples;
                                dbgLogWarn("P109 SRV self-sample: pushDescriptors is about to move an ACTIVE "
                                           "pass attachment (res=%p) to SHADER_RESOURCE (ctx=%p inPass=%d) - "
                                           "this is the state divergence behind #538",
                                           reinterpret_cast<void*>(srvRes), reinterpret_cast<void*>(ctx),
                                           static_cast<int>(ctx->inRenderPass));
                            }
                        }
                    }
                    transitionTextureTo(ctx, b.view->sourceTexture,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                            | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
                if (!reuseSrvTable) {
                    gCtx.device->CopyDescriptorsSimple(1, dst, b.view->cpuHandle,
                                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                }
                ++srvIdx;
                break;
            }
            case 2: {  // SRV：texel buffer
                D3D12_CPU_DESCRIPTOR_HANDLE dst{cpu.ptr + static_cast<SIZE_T>(srvIdx) * gCtx.drawInc};
                if (!b.buffer || b.buffer->kind != Dx12Object::Kind::Buffer || !b.buffer->resource) {
                    dbgLogError("pushDesc[%u] INVALID texel buffer", static_cast<unsigned>(i));
                    return mcdr::err("pushDescriptors", "pushDescriptors: invalid texel buffer handle");
                }
                transitionBufferTo(ctx, b.buffer,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                       | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = toDxgiFormat(b.texelFormat);
                if (srv.Format == DXGI_FORMAT_UNKNOWN) {
                    return mcdr::err("pushDescriptors", "pushDescriptors: unsupported texel buffer format "
                                                            + std::to_string(b.texelFormat));
                }
                srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                UINT elementBytes = std::max<UINT>(1u, blockSizeFor(srv.Format));
                srv.Buffer.FirstElement = static_cast<UINT>(b.offset / elementBytes);
                srv.Buffer.NumElements = static_cast<UINT>(b.length / elementBytes);
                gCtx.device->CreateShaderResourceView(b.buffer->resource.Get(), &srv, dst);
                ++srvIdx;
                break;
            }
            default:
                return mcdr::err("pushDescriptors",
                                 "pushDescriptors: unknown binding type " + std::to_string(b.type));
        }
    }
    if (srvCount > 0 && !reuseSrvTable) {
        // P243：记下这次分配，供后续同集合的 push 复用同一张表。
        ctx->lastSrvSlotOffset = srvSlotOffset;
        ctx->lastSrvCount = srvCount;
        ctx->lastSrvViews.clear();
        ctx->lastSrvViews.reserve(srvCount);
        for (const DrawBinding& b : bindings) {
            if (b.type == 1) {
                ctx->lastSrvViews.emplace_back(b.view, b.view ? b.view->cpuHandle.ptr : 0);
            }
        }
        ctx->nextDrawSlot += srvCount;
    }
    // SRV 表位于 root 参数索引 cbvCount（在 CBV root descriptor 之后）。
    // gpuRoot 必须指向本帧实际写入位置（ring buffer 偏移 base），
    // 而不是 heap 起始处。否则多帧飞环时每帧的命令列表都把根表绑定到
    // 同一 GPU 地址（heap start），导致 GPU 读到前帧残留的描述符 → 黑屏。
    if (srvCount > 0) {
        D3D12_GPU_DESCRIPTOR_HANDLE gpuRoot = gCtx.drawHeap->GetGPUDescriptorHandleForHeapStart();
        gpuRoot.ptr += base;
        // P20：诊断 root descriptor table 绑定地址（指向本帧写入位置）
        static UINT64 lastGpuFrame = 0;
        if (ctx->fenceValue != lastGpuFrame) {
            lastGpuFrame = ctx->fenceValue;
            dbgLogDebug(
                "pushDesc SET_ROOT_TABLE[%u]: heapBase=%llx writeBase=%llx slotCount=%u", cbvCount,
                static_cast<unsigned long long>(gCtx.drawHeap->GetGPUDescriptorHandleForHeapStart().ptr),
                static_cast<unsigned long long>(base), static_cast<unsigned>(srvCount));
        }
        ctx->commandList->SetGraphicsRootDescriptorTable(cbvCount, gpuRoot);
    }
    return {};
}

// ---------------------------------------------------------------------------
// P33 async：把瞬时描述符写到 drawHeap 的绝对槽位（worker 分区区域内）。
// 不做 transition（bundle 内禁止 ResourceBarrier），调用方须先在主列表上过渡。
// ---------------------------------------------------------------------------
namespace {

// 绝对槽位 -> drawHeap 的 CPU 句柄。
D3D12_CPU_DESCRIPTOR_HANDLE drawHeapCpuSlot(UINT slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = gCtx.drawHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * gCtx.drawInc;
    return h;
}

mcdr::Result<void> drawHeapSlotInRange(UINT slot, const char* where) {
    if (!gCtx.drawHeap || !gCtx.device) {
        return mcdr::err(where, std::string(where) + ": drawHeap/device not ready");
    }
    if (slot >= gCtx.drawHeap->GetDesc().NumDescriptors) {
        return mcdr::err(where, std::string(where) + ": slot out of range");
    }
    return {};
}

}  // namespace

mcdr::Result<void> writeCBVToDrawHeap(const UINT& slot, const Dx12Object*& buf, const long long& offset,
                                      const long long& size) {
    if (auto r = drawHeapSlotInRange(slot, "writeCBVToDrawHeap"); !r) return r;
    if (!buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) {
        return mcdr::err("writeCBVToDrawHeap", "writeCBVToDrawHeap: invalid buffer");
    }
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv;
    cbv.BufferLocation = buf->resource->GetGPUVirtualAddress() + static_cast<UINT64>(offset);
    // 描述符堆里的 CBV 与 root CBV 同一条约束：BufferLocation 必须 256 字节对齐。
    if (cbv.BufferLocation % 256ULL != 0) {
        return mcdr::err("writeCBVToDrawHeap", std::string("writeCBVToDrawHeap: CBV is not 256-byte aligned"
                                                           " (buffer offset=") +
                                                    std::to_string(offset) + ")");
    }
    UINT64 cbvSize = static_cast<UINT64>(size);
    cbvSize = (cbvSize + 255) & ~255ULL;
    if (cbvSize == 0) cbvSize = 256;
    cbv.SizeInBytes = static_cast<UINT>(cbvSize);
    gCtx.device->CreateConstantBufferView(&cbv, drawHeapCpuSlot(slot));
    return {};
}

mcdr::Result<void> writeTextureSRVToDrawHeap(const UINT& slot, const Dx12Object*& view) {
    if (auto r = drawHeapSlotInRange(slot, "writeTextureSRVToDrawHeap"); !r) return r;
    if (!view || view->cpuHandle.ptr == 0) {
        return mcdr::err("writeTextureSRVToDrawHeap", "writeTextureSRVToDrawHeap: invalid texture view");
    }
    gCtx.device->CopyDescriptorsSimple(1, drawHeapCpuSlot(slot), view->cpuHandle,
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return {};
}

mcdr::Result<void> writeBufferSRVToDrawHeap(UINT slot, Dx12Object* buf, long long offset, long long length,
                                            int texelFormat) {
    if (auto r = drawHeapSlotInRange(slot, "writeBufferSRVToDrawHeap"); !r) return r;
    if (!buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) {
        return mcdr::err("writeBufferSRVToDrawHeap", "writeBufferSRVToDrawHeap: invalid buffer");
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = toDxgiFormat(texelFormat);
    if (srv.Format == DXGI_FORMAT_UNKNOWN) {
        return mcdr::err("writeBufferSRVToDrawHeap", "writeBufferSRVToDrawHeap: unsupported texel buffer format "
                                                         + std::to_string(texelFormat));
    }
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    UINT elementBytes = std::max<UINT>(1u, blockSizeFor(srv.Format));
    srv.Buffer.FirstElement = static_cast<UINT>(static_cast<UINT64>(offset) / elementBytes);
    srv.Buffer.NumElements = static_cast<UINT>(static_cast<UINT64>(length) / elementBytes);
    gCtx.device->CreateShaderResourceView(buf->resource.Get(), &srv, drawHeapCpuSlot(slot));
    return {};
}

// ---------------------------------------------------------------------------
// P217：RAW 缓冲 SRV（R32_TYPELESS + D3D12_BUFFER_SRV_FLAG_RAW）
// ---------------------------------------------------------------------------
// 生成的 mesh HLSL 用 ByteAddressBuffer 读顶点/段表/meshlet 表（t14/t15/t16）。而
// writeBufferSRVToDrawHeap 只能按 texelFormat 建**类型化** SRV，类型化视图喂给
// ByteAddressBuffer 是非法的（调试层报 view format mismatch）；RAW 视图忽略格式，
// 首元素与元素数一律按 4 字节计，正是 ByteAddressBuffer.Load 的寻址语义。
// 不导出：只有本 TU 的 drawMeshlets 用。
namespace {

void writeRawBufferSRV(const D3D12_CPU_DESCRIPTOR_HANDLE& dst, Dx12Object* buf, long long offset,
                       long long length) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = static_cast<UINT>(static_cast<UINT64>(offset) / 4ull);
    srv.Buffer.NumElements = static_cast<UINT>(static_cast<UINT64>(length) / 4ull);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    gCtx.device->CreateShaderResourceView(buf->resource.Get(), &srv, dst);
}

// 本 TU 里 SRV/段表缓冲的长度校验：offset 必须落在缓冲内，长度缺省取到末尾。
long long remainingToEnd(const Dx12Object* buf, long long offset) {
    if (!buf || buf->size <= offset) return 0;
    return buf->size - offset;
}

}  // namespace

// ---------------------------------------------------------------------------
// P217：meshlet 地形派发（一次 DispatchMesh 覆盖整批 meshlet）
// ---------------------------------------------------------------------------
// 与 drawIndexedInstanced 的分工：普通绑定（地形 CBV + 纹理表）已经由
// pushDescriptors 推好，这里只补 mesh 尾参 —— b8 小常量（root CBV）与 t14/t15/t16
// 的 3 连描述符表（根签名里就是 3 个 DESCRIPTOR_RANGE_OFFSET_APPEND 的范围，
// 所以三个 SRV 描述符必须连续写在同一个 draw 槽位窗口里）。
namespace {

// UPLOAD heap 的瞬时缓冲必须一直停在 GENERIC_READ：D3D12 禁止对 UPLOAD/READBACK 资源
// 发过渡（调试层直接报错），而 GENERIC_READ 本身就允许被读。
void prepareMeshBuffer(CommandContext* ctx, Dx12Object* obj, D3D12_RESOURCE_STATES state) {
    if (obj->heapType != D3D12_HEAP_TYPE_UPLOAD) transitionBufferTo(ctx, obj, state);
}

// 与 drawIndexedInstanced 一致：标记所有 color target 已写入（render pass 结束时的
// 存/清决策依赖它）。
void markMeshColorTargets(CommandContext* ctx) {
    for (size_t i = 0; i < ctx->activeColorTargets.size(); ++i) {
        if (i < ctx->activeColorTargetsTouched.size()) ctx->activeColorTargetsTouched[i] = true;
    }
    ctx->colorTargetsWritten = true;
}

}  // namespace

// P217：meshlet 版地形派发。
//   * 不绑索引缓冲（MS 算术展开 quad 索引）；
//   * 段表换成 32 B 的 SectionDesc 表、另加 32 B 的 MeshletDesc 表（t15/t16）；
//   * b8 放的是 256 B 对齐的小常量（meshlet 总数），不是段表基址。
// 三个 SRV 的顺序必须与 createMeshRootSignature 的 extras 一致：t14 顶点、t15 段、t16 meshlet。
mcdr::Result<void> drawMeshlets(CommandContext* ctx, Dx12Object* vertexBuffer, long long vertexOffset,
                                Dx12Object* sections, long long sectionsOffset, long long sectionsLength,
                                Dx12Object* meshlets, long long meshletsOffset, long long meshletsLength,
                                Dx12Object* globals, long long globalsOffset, int meshletCount) {
    constexpr const char* kApi = "drawMeshlets";
    if (!ctx || !ctx->listOpen) {
        return mcdr::err(kApi, "meshletDispatch: no open command list");
    }
    if (!ctx->inRenderPass) {
        return mcdr::err(kApi, "meshletDispatch: no open render pass");
    }
    if (!ctx->currentPipeline || !ctx->currentPipeline->isMesh || ctx->currentPipeline->geometryParam == 0) {
        return mcdr::err(kApi, "meshletDispatch: current pipeline is not a mesh PSO with mesh extras");
    }
    if (meshletCount <= 0) {
        return mcdr::err(kApi, "meshletDispatch: meshletCount must be positive");
    }
    for (const Dx12Object* buf : {vertexBuffer, sections, meshlets, globals}) {
        if (!buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) {
            return mcdr::err(kApi, "meshletDispatch: invalid buffer handle");
        }
    }
    const long long vbBytes = remainingToEnd(vertexBuffer, vertexOffset);
    const long long secBytes = sectionsLength > 0 ? sectionsLength : remainingToEnd(sections, sectionsOffset);
    const long long meshletBytes = meshletsLength > 0 ? meshletsLength : remainingToEnd(meshlets, meshletsOffset);
    if (vbBytes < 4 || secBytes < 32 || meshletBytes < 32) {
        return mcdr::err(kApi, "meshletDispatch: empty vertex/section/meshlet range");
    }
    if (sectionsLength > 0 && sectionsLength > remainingToEnd(sections, sectionsOffset)) {
        return mcdr::err(kApi, "meshletDispatch: sections range overflow");
    }
    if (meshletsLength > 0 && meshletsLength > remainingToEnd(meshlets, meshletsOffset)) {
        return mcdr::err(kApi, "meshletDispatch: meshlet range overflow");
    }

    const UINT slotCount = 3u;
    if (ctx->nextDrawSlot + slotCount > syncRingCapacity()) {
        return mcdr::err(kApi, "meshletDispatch: draw descriptor heap exhausted for this frame");
    }

    prepareMeshBuffer(ctx, vertexBuffer,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    prepareMeshBuffer(ctx, sections,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    prepareMeshBuffer(ctx, meshlets,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    prepareMeshBuffer(ctx, globals, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    const UINT slot = ctx->drawHeapSlotBase + ctx->nextDrawSlot;
    const SIZE_T base = static_cast<SIZE_T>(slot) * gCtx.drawInc;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuRoot = gCtx.drawHeap->GetCPUDescriptorHandleForHeapStart();
    cpuRoot.ptr += base;
    writeRawBufferSRV(cpuRoot, vertexBuffer, vertexOffset, vbBytes);
    cpuRoot.ptr += gCtx.drawInc;
    writeRawBufferSRV(cpuRoot, sections, sectionsOffset, secBytes);
    cpuRoot.ptr += gCtx.drawInc;
    writeRawBufferSRV(cpuRoot, meshlets, meshletsOffset, meshletBytes);
    ctx->nextDrawSlot += slotCount;

    D3D12_GPU_DESCRIPTOR_HANDLE gpuRoot = gCtx.drawHeap->GetGPUDescriptorHandleForHeapStart();
    gpuRoot.ptr += base;
    ctx->commandList->SetGraphicsRootDescriptorTable(ctx->currentPipeline->geometryParam, gpuRoot);

    const D3D12_GPU_VIRTUAL_ADDRESS globalsVA =
        globals->resource->GetGPUVirtualAddress() + static_cast<UINT64>(globalsOffset);
    if (globalsVA % 256ULL != 0) {
        return mcdr::err(kApi, std::string("meshletDispatch: globals root CBV is not 256-byte aligned (offset=") +
                                   std::to_string(globalsOffset) + ")");
    }
    ctx->commandList->SetGraphicsRootConstantBufferView(ctx->currentPipeline->segmentParam, globalsVA);

    ComPtr<ID3D12GraphicsCommandList6> list6;
    if (FAILED(ctx->commandList->QueryInterface(IID_PPV_ARGS(&list6))) || !list6) {
        return mcdr::err(kApi, "meshletDispatch: ID3D12GraphicsCommandList6 is not available on this command list");
    }
    list6->DispatchMesh(static_cast<UINT>(meshletCount), 1, 1);
    markMeshColorTargets(ctx);
    return {};
}


mcdr::Result<void> drawIndexedInstanced(CommandContext* ctx, UINT indexCount, UINT instanceCount,
                                        INT startIndexLocation, INT baseVertexLocation,
                                        UINT startInstanceLocation) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("drawIndexedInstanced", "drawIndexed: no open command list");
    }
    if (!ctx->inRenderPass) {
        return mcdr::err("drawIndexedInstanced", "drawIndexed: no open render pass");
    }
    // P17 诊断：每次 drawIndexed 都记录（验证 GUI pass 是否有实际 draw call）
    // 使用静态计数器区分同一 command list 内的多个 pass（如 512x256 + 854x480）
    static UINT64 lastFence = 0;
    static int passCount = 0;
    if (ctx->fenceValue != lastFence) {
        lastFence = ctx->fenceValue;
        passCount = 0;
    }
    passCount++;
    // P27：drawIndexed 每 draw 一次（重帧上万次），降级为 DEBUG 日志。
    if (indexCount == 0) {
        DBG_LOG_DEBUG("drawIndexed[%llu] PASS#%d: ZERO-COUNT (skip) topo=%d",
                      (unsigned long long)ctx->fenceValue, passCount,
                      (int)(ctx->currentPipeline ? ctx->currentPipeline->topology : -1));
    } else {
        DBG_LOG_DEBUG("drawIndexed[%llu] PASS#%d: count=%u inst=%u first=%d base=%d topo=%d",
                      (unsigned long long)ctx->fenceValue, passCount, indexCount, instanceCount,
                      startIndexLocation, baseVertexLocation,
                      (int)(ctx->currentPipeline ? ctx->currentPipeline->topology : -1));
    }
    // P17：标记当前 render pass 的所有 color target 为"已写入"
    for (size_t i = 0; i < ctx->activeColorTargets.size(); ++i) {
        if (i < ctx->activeColorTargetsTouched.size()) ctx->activeColorTargetsTouched[i] = true;
    }
    ctx->colorTargetsWritten = true;
    DBG_LOG_DEBUG("drawIndexed[%llu] colorTargetsWritten -> %d", (unsigned long long)ctx->fenceValue,
                  (int)ctx->colorTargetsWritten);
    ctx->commandList->DrawIndexedInstanced(indexCount, instanceCount, startIndexLocation, baseVertexLocation,
                                           startInstanceLocation);
    return {};
}

mcdr::Result<void> drawInstanced(CommandContext* ctx, UINT vertexCount, UINT instanceCount, UINT firstVertex,
                                 UINT startInstanceLocation) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("drawInstanced", "draw: no open command list");
    }
    if (!ctx->inRenderPass) {
        return mcdr::err("drawInstanced", "draw: no open render pass");
    }
    DBG_LOG_DEBUG("draw: vertexCount=%u instance=%u firstVertex=%u", vertexCount, instanceCount, firstVertex);
    ctx->commandList->DrawInstanced(vertexCount, instanceCount, firstVertex, startInstanceLocation);
    return {};
}

mcdr::Result<void> drawIndexedIndirect(CommandContext* ctx, Dx12Object* commands, long long offset,
                                       UINT drawCount) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("drawIndexedIndirect", "drawIndexedIndirect: no open command list");
    }
    if (!ctx->inRenderPass) {
        return mcdr::err("drawIndexedIndirect", "drawIndexedIndirect: no open render pass");
    }
    if (!commands || commands->kind != Dx12Object::Kind::Buffer) {
        return mcdr::err("drawIndexedIndirect", "drawIndexedIndirect: invalid buffer handle");
    }
    if (!gCtx.cmdSigIndexed) {
        return mcdr::err("drawIndexedIndirect", "drawIndexedIndirect: no indexed command signature");
    }
    ctx->commandList->ExecuteIndirect(gCtx.cmdSigIndexed.Get(), drawCount, commands->resource.Get(),
                                      static_cast<UINT64>(offset), nullptr, 0);
    return {};
}

mcdr::Result<void> drawIndirect(CommandContext* ctx, Dx12Object* commands, long long offset, UINT drawCount) {
    if (!ctx || !ctx->listOpen) {
        return mcdr::err("drawIndirect", "drawIndirect: no open command list");
    }
    if (!ctx->inRenderPass) {
        return mcdr::err("drawIndirect", "drawIndirect: no open render pass");
    }
    if (!commands || commands->kind != Dx12Object::Kind::Buffer) {
        return mcdr::err("drawIndirect", "drawIndirect: invalid buffer handle");
    }
    if (!gCtx.cmdSigNonIndexed) {
        return mcdr::err("drawIndirect", "drawIndirect: no non-indexed command signature");
    }
    ctx->commandList->ExecuteIndirect(gCtx.cmdSigNonIndexed.Get(), drawCount, commands->resource.Get(),
                                      static_cast<UINT64>(offset), nullptr, 0);
    return {};
}

// ---------------------------------------------------------------------------
// P6 诊断：读回纹理/buffer 内容（纯色黑屏定位用）。
// 内部先等 GPU 空闲（已提交命令全部完成、资源隐式 decay 回初始状态），再用
// 一次性命令列表拷贝到 readback staging。调用点每 ~60 帧触发一次，同步开销可忽略。
// ---------------------------------------------------------------------------

void dbgReadbackTexturePixels(Dx12Object* tex, const char* tag) {
    // P33: 禁止在 ASYNC 管道活跃时执行读回——static command list 会竞争
    //      gCtx.queue 与 render thread 的 submitCommandList，触发
    //      "Command lists must be closed before execution" → device removed。
    if (gAsyncRenderCtx != nullptr) return;
    if (!tex || tex->kind != Dx12Object::Kind::Texture || !tex->resource) return;
    if (auto idle = deviceWaitIdle(); !idle) return;
    ID3D12Resource* r = tex->resource.Get();
    D3D12_RESOURCE_DESC td = r->GetDesc();
    UINT w = static_cast<UINT>(td.Width), h = td.Height;
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return;

    // 根据纹理实际格式计算每像素字节数和读回足迹格式。
    // 硬编码 R8G8B8A8_UNORM（原代码）对 R16 类格式（fmt=28/30/31等）会导致
    // 字节偏移错位（*4 vs *8）和 staging buffer 容量不足（total 减半）。
    DXGI_FORMAT fmt = td.Format;
    UINT bpp = 4;
    DXGI_FORMAT fbFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            bpp = 4;
            fbFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            bpp = 8;
            fbFmt = DXGI_FORMAT_R16G16B16A16_UNORM;
            break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            bpp = 16;
            fbFmt = DXGI_FORMAT_R32G32_FLOAT;
            break;
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
            bpp = 1;
            fbFmt = DXGI_FORMAT_R8_UNORM;
            break;
        default:
            // 未知格式：按4字节处理（多数常见格式兼容）
            bpp = 4;
            fbFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
    }
    // P235: 拷贝足迹的格式必须与源纹理完全一致。R8_UINT 源配 R8_UNORM 足迹不属于
    // "兼容格式"，CopyTextureRegion 返回 E_INVALIDARG，整条命令列表被丢弃之后
    // staging 的 Map 也跟着失败（2026-10-25 实测：SRI 自检报 staging Map failed）。
    fbFmt = fmt;
    UINT64 rowBytes = static_cast<UINT64>(w) * bpp;
    UINT64 pitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                   & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    UINT64 total = pitch * h;

    static ComPtr<ID3D12Resource> staging;
    if (!staging || staging->GetDesc().Width < total) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = total;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        hp.CreationNodeMask = 0;
        hp.VisibleNodeMask = 0;
        if (FAILED(gCtx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&staging))))
            return;
    }
    static ComPtr<ID3D12CommandAllocator> alloc;
    static ComPtr<ID3D12GraphicsCommandList> cl;
    if (!alloc
        && FAILED(gCtx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))))
        return;
    if (!cl) {
        if (FAILED(gCtx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                  IID_PPV_ARGS(&cl))))
            return;
    } else {
        alloc->Reset();
        cl->Reset(alloc.Get(), nullptr);
    }
    // deviceWaitIdle 后未保持状态的纹理隐式 decay 回 COMMON。
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = r;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = staging.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = fbFmt;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cl->ResourceBarrier(1, &b);
    cl->Close();

    ID3D12CommandList* lists[] = {cl.Get()};
    gCtx.queue->ExecuteCommandLists(1, lists);
    UINT64 fv = ++gCtx.queueFenceValue;
    if (FAILED(gCtx.queue->Signal(gCtx.queueFence.Get(), fv))) return;
    if (auto waited = waitForQueueFenceValue(fv, 5'000'000'000ULL); !waited) return;

    void* ptr = nullptr;
    if (FAILED(staging->Map(0, nullptr, &ptr))) {
        // P235: 读回失败时把设备移除原因一起打出来 —— Map 失败往往意味着前面某条
        // 命令列表已经把设备弄丢了（GetCompletedValue 返回 UINT64_MAX 也是这个信号）。
        const HRESULT rr = gCtx.device ? gCtx.device->GetDeviceRemovedReason() : E_FAIL;
        dbgLogError("readback[%s]: staging Map failed (fmt=%d %ux%u pitch=%llu total=%llu removedReason=0x%08X)",
                    tag, static_cast<int>(fmt), w, h, static_cast<unsigned long long>(pitch),
                    static_cast<unsigned long long>(total), static_cast<unsigned>(rr));
        return;
    }
    const uint8_t* base = static_cast<const uint8_t*>(ptr);
    int xs[3] = {0, static_cast<int>(w) / 2, static_cast<int>(w) - 1};
    int ys[3] = {0, static_cast<int>(h) / 2, static_cast<int>(h) - 1};
    for (int yi = 0; yi < 3; ++yi) {
        for (int xi = 0; xi < 3; ++xi) {
            const uint8_t* p = base + static_cast<UINT64>(ys[yi]) * pitch + static_cast<UINT64>(xs[xi]) * bpp;
            dbgLogDebug("rbTex[%s][%ux%u] (%d,%d) = RGBA(%3d,%3d,%3d,%3d)", tag, w, h, xs[xi], ys[yi], p[0], p[1],
                   p[2], p[3]);
        }
    }
    if (gLogLevel >= 3) dbgDumpPixelsToFile(base, w, h, pitch, tag);
    staging->Unmap(0, nullptr);
}

// P6 诊断：把 RGBA8 像素完整导出（BMP 文件 + 日志 ASCII 缩略图）。
// BMP：32bpp BGRA，bottom-up 行序，写到当前工作目录 dx12_dump_<tag>.bmp。
// ASCII：缩略到 <=96 列，亮度映射 .:-=+*#%@ 字符，高饱和度用色相字母标注
// （R/G/B/Y/M/C），用于快速识别画面内容（全景/按钮/文字）。
void dbgDumpPixelsToFile(const uint8_t* rgba, UINT w, UINT h, UINT64 pitch, const char* tag) {
    if (!rgba || w == 0 || h == 0 || w > 16384 || h > 16384) return;

    // ---- 1) 写 BMP 文件 ----
    {
        char path[256];
        snprintf(path, sizeof(path), "dx12_dump_%s.bmp", tag);
        FILE* f = fopen(path, "wb");
        if (f) {
            UINT64 rowBytes = static_cast<UINT64>(w) * 4;
            UINT64 padded = (rowBytes + 3) & ~3ULL;  // BMP 行 4 字节对齐（RGBA 已对齐）
            UINT64 imgSize = padded * h;
            uint32_t bfSize = static_cast<uint32_t>(54 + imgSize);
            // BITMAPFILEHEADER (14)
            uint8_t hdr[54] = {0};
            hdr[0] = 'B';
            hdr[1] = 'M';
            hdr[2] = static_cast<uint8_t>(bfSize & 0xFF);
            hdr[3] = static_cast<uint8_t>((bfSize >> 8) & 0xFF);
            hdr[4] = static_cast<uint8_t>((bfSize >> 16) & 0xFF);
            hdr[5] = static_cast<uint8_t>((bfSize >> 24) & 0xFF);
            hdr[10] = 54;  // pixel data offset
            // BITMAPINFOHEADER (40)
            hdr[14] = 40;
            hdr[18] = static_cast<uint8_t>(w & 0xFF);
            hdr[19] = static_cast<uint8_t>((w >> 8) & 0xFF);
            hdr[20] = static_cast<uint8_t>((w >> 16) & 0xFF);
            hdr[21] = static_cast<uint8_t>((w >> 24) & 0xFF);
            uint32_t hh = h;
            hdr[22] = static_cast<uint8_t>(hh & 0xFF);
            hdr[23] = static_cast<uint8_t>((hh >> 8) & 0xFF);
            hdr[24] = static_cast<uint8_t>((hh >> 16) & 0xFF);
            hdr[25] = static_cast<uint8_t>((hh >> 24) & 0xFF);
            hdr[26] = 1;  // planes
            hdr[28] = 32;  // bpp
            hdr[34] = static_cast<uint8_t>(imgSize & 0xFF);
            hdr[35] = static_cast<uint8_t>((imgSize >> 8) & 0xFF);
            hdr[36] = static_cast<uint8_t>((imgSize >> 16) & 0xFF);
            hdr[37] = static_cast<uint8_t>((imgSize >> 24) & 0xFF);
            fwrite(hdr, 1, 54, f);
            // bottom-up：BMP 首行是图片最后一行
            for (UINT y = h; y > 0; --y) {
                const uint8_t* row = rgba + static_cast<UINT64>(y - 1) * pitch;
                for (UINT x = 0; x < w; ++x) {
                    uint8_t bgr[4];
                    bgr[0] = row[x * 4 + 2];  // B
                    bgr[1] = row[x * 4 + 1];  // G
                    bgr[2] = row[x * 4 + 0];  // R
                    bgr[3] = row[x * 4 + 3];  // A
                    fwrite(bgr, 1, 4, f);
                }
            }
            fclose(f);
            dbgLogDebug("dump[%s] BMP saved %ux%u -> %s (rgba=%p)", tag, w, h, path,
                   reinterpret_cast<const void*>(rgba));
        } else {
            dbgLogDebug("dump[%s] BMP open failed (%s)", tag, path);
        }
    }

    // ---- 2) 日志 ASCII 缩略图（亮度字符 + 色相字母）----
    const int cols = 96;
    const char* lumaChars = " .:-=+*#%@";
    int rows = static_cast<int>(static_cast<UINT64>(h) * cols / w);
    if (rows < 1) rows = 1;
    if (rows > 64) rows = 64;
    std::string art;
    art.reserve(static_cast<size_t>(cols) * rows + rows);
    for (int ry = 0; ry < rows; ++ry) {
        UINT y0 = static_cast<UINT>(static_cast<UINT64>(ry) * h / rows);
        UINT y1 = static_cast<UINT>(static_cast<UINT64>(ry + 1) * h / rows);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > h) y1 = h;
        for (int rx = 0; rx < cols; ++rx) {
            UINT x0 = static_cast<UINT>(static_cast<UINT64>(rx) * w / cols);
            UINT x1 = static_cast<UINT>(static_cast<UINT64>(rx + 1) * w / cols);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > w) x1 = w;
            unsigned r = 0, g = 0, b = 0, a = 0;
            UINT cnt = 0;
            for (UINT y = y0; y < y1; ++y) {
                const uint8_t* row = rgba + static_cast<UINT64>(y) * pitch;
                for (UINT x = x0; x < x1; ++x) {
                    r += row[x * 4 + 0];
                    g += row[x * 4 + 1];
                    b += row[x * 4 + 2];
                    a += row[x * 4 + 3];
                    ++cnt;
                }
            }
            if (cnt == 0) {
                art += ' ';
                continue;
            }
            r /= cnt;
            g /= cnt;
            b /= cnt;
            a /= cnt;
            // 亮度 + 饱和度（判断用亮度字符还是色相字母）
            int mx = static_cast<int>(r), mn = static_cast<int>(r);
            if (static_cast<int>(g) > mx) mx = g;
            if (static_cast<int>(b) > mx) mx = b;
            if (static_cast<int>(g) < mn) mn = g;
            if (static_cast<int>(b) < mn) mn = b;
            int luma = (r * 299 + g * 587 + b * 114) / 1000;
            int sat = (mx == 0) ? 0 : (mx - mn) * 255 / mx;
            if (sat > 90 && luma > 40) {
                // 高饱和度：用色相字母标出主色
                char hue = '?';
                if (r > g && r > b)
                    hue = 'R';
                else if (g > r && g > b)
                    hue = 'G';
                else if (b > r && b > g)
                    hue = 'B';
                else if (r > 120 && g > 120 && b < 90)
                    hue = 'Y';
                else if (r > 120 && b > 120 && g < 90)
                    hue = 'M';
                else if (g > 120 && b > 120 && r < 90)
                    hue = 'C';
                else if (r > 180 && g > 180 && b > 180)
                    hue = 'W';
                art += hue;
            } else {
                int idx = luma * 9 / 255;
                if (idx > 9) idx = 9;
                art += lumaChars[idx];
            }
        }
        art += '\n';
    }
    dbgLogDebug("dump[%s] ASCII %dx%d (grid %dx%d):\n%s", tag, w, h, cols, rows, art.c_str());
}

void dbgReadbackBufferBytes(Dx12Object* buf, long long offset, int len, const char* tag) {
    // P33: 禁止在 ASYNC 管道活跃时执行读回——static command list 会竞争
    //      gCtx.queue 与 render thread 的 submitCommandList。
    if (gAsyncRenderCtx != nullptr) return;
    if (!buf || buf->kind != Dx12Object::Kind::Buffer || !buf->resource) return;
    if (offset < 0 || offset >= buf->size) return;
    long long avail = buf->size - offset;
    if (len <= 0) len = static_cast<int>(std::min(avail, 128LL));
    len = static_cast<int>(std::min(static_cast<long long>(len), avail));
    if (len <= 0) return;

    void* ptr = nullptr;
    ID3D12Resource* unmapRes = nullptr;
    if (buf->heapType == D3D12_HEAP_TYPE_UPLOAD) {
        // UPLOAD：CPU 已写可见，直接 Map 读。P26 fix：必须带 offset 的 D3D12_RANGE，
        // 否则 Map 返回的指针恒指向缓冲区基址，offset>0 时读到错误位置
        // （日志 rbBuf[ubo] off=256 读到 base 内容 → 误判 UBO 为垃圾）。
        D3D12_RANGE range{static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + len)};
        if (FAILED(buf->resource->Map(0, &range, &ptr))) return;
        unmapRes = buf->resource.Get();
    } else {
        if (auto idle = deviceWaitIdle(); !idle) return;
        UINT64 total = (static_cast<UINT64>(len) + 255) & ~255ULL;
        static ComPtr<ID3D12Resource> staging;
        if (!staging || staging->GetDesc().Width < total) {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = total;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            hp.CreationNodeMask = 0;
            hp.VisibleNodeMask = 0;
            if (FAILED(gCtx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&staging))))
                return;
        }
        static ComPtr<ID3D12CommandAllocator> alloc;
        static ComPtr<ID3D12GraphicsCommandList> cl;
        if (!alloc
            && FAILED(
                gCtx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))))
            return;
        if (!cl) {
            if (FAILED(gCtx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                      IID_PPV_ARGS(&cl))))
                return;
        } else {
            alloc->Reset();
            cl->Reset(alloc.Get(), nullptr);
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = buf->resource.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        cl->CopyBufferRegion(staging.Get(), 0, buf->resource.Get(), static_cast<UINT64>(offset),
                             static_cast<UINT64>(len));
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        cl->ResourceBarrier(1, &b);
        cl->Close();

        ID3D12CommandList* lists[] = {cl.Get()};
        gCtx.queue->ExecuteCommandLists(1, lists);
        UINT64 fv = ++gCtx.queueFenceValue;
        if (FAILED(gCtx.queue->Signal(gCtx.queueFence.Get(), fv))) return;
        if (auto waited = waitForQueueFenceValue(fv, 5'000'000'000ULL); !waited) return;
        if (FAILED(staging->Map(0, nullptr, &ptr))) return;
        unmapRes = staging.Get();
    }

    const float* f = static_cast<const float*>(ptr);
    const uint8_t* b8 = static_cast<const uint8_t*>(ptr);
    std::string fs;
    int nf = std::min(len / 4, 12);
    for (int i = 0; i < nf; ++i) {
        char t[32];
        snprintf(t, sizeof(t), "%g ", static_cast<double>(f[i]));
        fs += t;
    }
    std::string hs;
    int nh = std::min(len, 16);
    for (int i = 0; i < nh; ++i) {
        char t[8];
        snprintf(t, sizeof(t), "%02X ", b8[i]);
        hs += t;
    }
    dbgLogDebug("rbBuf[%s] off=%lld len=%d heap=%d floats=[%s] hex=[%s]", tag, offset, len,
           static_cast<int>(buf->heapType), fs.c_str(), hs.c_str());
    unmapRes->Unmap(0, nullptr);
}

// wide -> UTF-8；空指针 / 空串 / 转换失败都返回空串。
static std::string wideToUtf8(const wchar_t* w) {
    if (w == nullptr || w[0] == L'\0') {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return std::string();
    }
    std::string s(static_cast<size_t>(n), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (written <= 1) {
        return std::string();
    }
    s.resize(static_cast<size_t>(written - 1));
    return s;
}

// 枚举所有 D3D12 支持的适配器，返回 JSON 数组字符串。
std::string enumerateAdaptersJson() {
    // 这里会探测性建一次设备，捕获层也得先于它载入（未请求时空操作）。
    ensurePixCapturerLoaded();
    nlohmann::json list = nlohmann::json::array();
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return "[{\"error\":\"CreateDXGIFactory1 failed\"}]";
        }
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter> adapter;
            if (FAILED(factory->EnumAdapters(i, &adapter))) break;
            DXGI_ADAPTER_DESC desc{};
            if (FAILED(adapter->GetDesc(&desc))) continue;
            // 只返回支持 D3D12 的适配器；顺带记下它能建出的最高功能级（探测设备随即释放），
            // 供 JVM 侧在设备创建之前打印「currently is using DirectX 12 FL …」。
            const D3D_FEATURE_LEVEL highest = probeHighestFeatureLevel(adapter.Get());
            if (highest < D3D_FEATURE_LEVEL_12_0) continue;
            // 十六进制串一律在 native 侧定好格式：JVM 侧只做展示，不该再关心补零。
            char luidBuf[32] = {};
            snprintf(luidBuf, sizeof(luidBuf), "%08X%08X", desc.AdapterLuid.LowPart,
                     desc.AdapterLuid.HighPart);
            char vidBuf[16] = {};
            snprintf(vidBuf, sizeof(vidBuf), "0x%08X", desc.VendorId);
            char didBuf[16] = {};
            snprintf(didBuf, sizeof(didBuf), "0x%08X", desc.DeviceId);
            list.push_back(nlohmann::json{
                {"name", wideToUtf8(desc.Description)},
                {"luid", luidBuf},
                {"vid", vidBuf},
                {"did", didBuf},
                {"feature_level", featureLevelName(highest)},
                {"feature_level_value", static_cast<int>(highest)},
                {"vram_gb", static_cast<long long>(desc.DedicatedVideoMemory / (1024LL * 1024 * 1024))},
            });
        }
    }
    return list.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// ---------------------------------------------------------------------------
// 活跃适配器完整描述（mcdr_dx12GetAdapterInfo 的后端实现）
//
// 与 enumerateAdaptersJson() 同源：CreateDXGIFactory1 + EnumAdapterByLuid 拿到
// 设备真正绑定的那块适配器。驱动版本 / 驱动描述 / 硬件·集显·可拆卸分类只有
// DXCore 提供，而 DXCore 在 Win10+ 才存在、又不能给 MCDR.dll 引入 dxcore.lib
// 硬依赖，因此只在 queryDxCoreInfo() 里用 LoadLibraryW("dxcore.dll") 动态解析。
// 任何一步失败都退回纯 DXGI 值，并把 "dxcore" 置为 false。
// ---------------------------------------------------------------------------

namespace {

// DXCore 侧查到的信息；ok == false 表示 DXCore 整体不可用，或没查到这个 LUID。
struct DxCoreInfo {
    bool ok = false;
    bool isHardware = false;
    bool isIntegrated = false;
    bool isDetachable = false;
    std::string driverVersion;
    std::string driverDescription;
};

}  // namespace

// 读一个 DXCore 布尔属性；属性不支持或读取失败时原值不动。
static void readDxCoreBool(IDXCoreAdapter* adapter, DXCoreAdapterProperty prop, bool& out) {
    if (!adapter->IsPropertySupported(prop)) {
        return;
    }
    bool value = false;
    if (SUCCEEDED(adapter->GetProperty(prop, sizeof(value), &value))) {
        out = value;
    }
}

// 运行期解析 DXCore，取指定 LUID 那块适配器的驱动与硬件分类。全部失败都静默。
static void queryDxCoreInfo(const LUID& luid, DxCoreInfo& out) {
    // 唯一的 DXCore 入口，必须动态解析：按名字调用 DXCoreCreateAdapterFactory
    // 会给 MCDR.dll 引入 dxcore.lib 硬依赖（何况 dxcore.h 里那个声明被
    // _WIN32_WINNT 守卫挡着）。模块句柄故意不 FreeLibrary —— 下面拿到的 COM
    // 接口 vtable 就在这个模块里，卸载之后任何一次 Release() 都会跳进已卸载的
    // 内存；一个常驻 HMODULE 的代价可以忽略。
    static HMODULE sDxCore = LoadLibraryW(L"dxcore.dll");
    if (sDxCore == nullptr) {
        return;
    }

    using PfnCreateAdapterFactory = HRESULT(WINAPI*)(REFIID, void**);
    static PfnCreateAdapterFactory sCreate =
        reinterpret_cast<PfnCreateAdapterFactory>(GetProcAddress(sDxCore, "DXCoreCreateAdapterFactory"));
    if (sCreate == nullptr) {
        return;
    }

    // 用 __uuidof 而不是 IID_IDXCoreAdapterFactory：后者是 DEFINE_GUID 声明，
    // 按名字引用就得链 uuid.lib/dxcore.lib，而 MIDL_INTERFACE 已经带了
    // __declspec(uuid(...))。GetAdapterByLuid 还顺带绕开了
    // DXCORE_ADAPTER_ATTRIBUTE_* 那些 GUID 符号。
    ComPtr<IDXCoreAdapterFactory> factory;
    if (FAILED(sCreate(__uuidof(IDXCoreAdapterFactory), reinterpret_cast<void**>(factory.GetAddressOf())))) {
        return;
    }

    ComPtr<IDXCoreAdapter> adapter;
    if (FAILED(factory->GetAdapterByLuid(luid, __uuidof(IDXCoreAdapter),
                                         reinterpret_cast<void**>(adapter.GetAddressOf())))) {
        return;
    }

    out.ok = true;

    if (adapter->IsPropertySupported(DXCoreAdapterProperty::DriverVersion)) {
        uint64_t raw = 0;
        if (SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::DriverVersion, sizeof(raw), &raw))) {
            // DXCore 的 DriverVersion 是 64 位：最高 16 位是主版本，依次往下。
            // NVIDIA 32.0.15.7602 == 0x0020'0000'000F'1DB2。
            const unsigned vA = static_cast<unsigned>((raw >> 48) & 0xFFFFu);
            const unsigned vB = static_cast<unsigned>((raw >> 32) & 0xFFFFu);
            const unsigned vC = static_cast<unsigned>((raw >> 16) & 0xFFFFu);
            const unsigned vD = static_cast<unsigned>(raw & 0xFFFFu);
            char buf[64] = {};
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u", vA, vB, vC, vD);
            out.driverVersion = buf;
        }
    }

    if (adapter->IsPropertySupported(DXCoreAdapterProperty::DriverDescription)) {
        size_t need = 0;
        if (SUCCEEDED(adapter->GetPropertySize(DXCoreAdapterProperty::DriverDescription, &need))
            && need > 0) {
            // 实测：DriverDescription 是**窄字符**串，不是 wchar_t（尽管不少资料
            // 按宽串写）。本机 NVIDIA 那块返回 35 字节 = 34 个 ASCII 字符 + 结尾
            // NUL，GetPropertySize 给的字节数已经含 NUL。多要一个字节再手工补
            // NUL，防止驱动不写终止符时越界读。
            std::vector<char> text(need + 1, '\0');
            if (SUCCEEDED(
                    adapter->GetProperty(DXCoreAdapterProperty::DriverDescription, need, text.data()))) {
                text.back() = '\0';
                out.driverDescription = text.data();
            }
        }
    }

    // IsHardware / IsIntegrated / IsDetachable 是 DXCoreAdapterProperty 枚举值，
    // 不是 IDXCoreAdapter 上的方法，只能按属性读。
    readDxCoreBool(adapter.Get(), DXCoreAdapterProperty::IsHardware, out.isHardware);
    readDxCoreBool(adapter.Get(), DXCoreAdapterProperty::IsIntegrated, out.isIntegrated);
    readDxCoreBool(adapter.Get(), DXCoreAdapterProperty::IsDetachable, out.isDetachable);
}

std::string adapterInfoJson() {
    // 没有活跃设备 ⇒ 空串，导出层据此返回 0。
    if (!gCtx.device) {
        return std::string();
    }

    const LUID luid = gCtx.device->GetAdapterLuid();

    DXGI_ADAPTER_DESC desc{};
    UINT descFlags = 0;
    bool haveDesc = false;
    bool haveFlags = false;
    {
        ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
                ComPtr<IDXGIAdapter1> adapter1;
                DXGI_ADAPTER_DESC1 desc1{};
                if (SUCCEEDED(adapter.As(&adapter1)) && SUCCEEDED(adapter1->GetDesc1(&desc1))) {
                    // DXGI_ADAPTER_DESC1 就是 DXGI_ADAPTER_DESC 之后追加 UINT Flags，
                    // 公共前缀直接拷过来。Flags 本身要单独留一份（基础结构里没有）。
                    std::memcpy(&desc, &desc1, sizeof(desc));
                    descFlags = desc1.Flags;
                    haveFlags = true;
                    haveDesc = true;
                } else if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    haveDesc = true;
                }
            }
        }
    }

    DxCoreInfo core;
    queryDxCoreInfo(luid, core);

    // name：DXGI 描述优先，取不到时回落到 deviceContext 里缓存的名字。
    std::string name;
    if (haveDesc) {
        name = wideToUtf8(desc.Description);
    }
    if (name.empty()) {
        name = gCtx.adapterName;
    }

    const UINT vendorId = haveDesc ? desc.VendorId : 0u;
    const UINT deviceId = haveDesc ? desc.DeviceId : 0u;
    const UINT subsysId = haveDesc ? desc.SubSysId : 0u;
    const UINT revision = haveDesc ? desc.Revision : 0u;

    // vendor：与 vanilla com/mojang/blaze3d/vulkan/VulkanPhysicalDevice.java:148-161
    // 完全一致的厂商表；表外一律 "0x%x"。
    std::string vendor;
    char vendorFallback[32] = {};
    snprintf(vendorFallback, sizeof(vendorFallback), "0x%x", vendorId);
    switch (vendorId) {
        case 4098:
        case 4130: vendor = "AMD"; break;
        case 4318:
        case 4818: vendor = "NVIDIA"; break;
        case 32902: vendor = "INTEL"; break;
        case 5772:
        case 6091:
        case 6505:
        case 20803: vendor = "QUALCOMM"; break;
        case 4112: vendor = "IMGTEC"; break;
        case 5348: vendor = "BROADCOM"; break;
        case 5045: vendor = "ARM"; break;
        case 5140: vendor = "MICROSOFT"; break;
        case 4203: vendor = "APPLE"; break;
        default: vendor = vendorFallback; break;
    }

    const char* featureLevel = featureLevelName(gCtx.featureLevel);

    // device_type：DXCore 可用就按它的三个布尔分类；否则只能退回 DXGI 的软件
    // 适配器标志（DXGI_ADAPTER_DESC 本身没有 Flags，只有 DESC1 有）。
    std::string deviceType;
    if (core.ok) {
        if (!core.isHardware) {
            deviceType = "cpu";
        } else if (core.isIntegrated) {
            deviceType = "integrated";
        } else if (core.isDetachable) {
            deviceType = "virtual";
        } else {
            deviceType = "discrete";
        }
    } else if (haveFlags && (descFlags & static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE)) != 0u) {
        deviceType = "cpu";
    } else {
        deviceType = "discrete";
    }

    // LUID 打印约定与 enumerateAdaptersJson() 保持一致：%08X 高低位拼一起。
    char luidBuf[32] = {};
    snprintf(luidBuf, sizeof(luidBuf), "%08X%08X", luid.LowPart, luid.HighPart);

    nlohmann::json j;
    j["name"] = name;
    j["vendor"] = vendor;
    j["vendor_id"] = vendorId;
    j["device_id"] = deviceId;
    j["subsys_id"] = subsysId;
    j["revision"] = revision;
    j["feature_level"] = featureLevel;
    j["feature_level_value"] = static_cast<int>(gCtx.featureLevel);
    j["driver_version"] = core.driverVersion;
    j["driver_description"] = core.driverDescription;
    j["device_type"] = deviceType;
    j["vram_bytes"] = static_cast<unsigned long long>(haveDesc ? desc.DedicatedVideoMemory : 0);
    j["shared_bytes"] = static_cast<unsigned long long>(haveDesc ? desc.SharedSystemMemory : 0);
    j["dedicated_system_bytes"] =
        static_cast<unsigned long long>(haveDesc ? desc.DedicatedSystemMemory : 0);
    j["luid"] = luidBuf;
    j["is_hardware"] = core.isHardware;
    j["is_detachable"] = core.isDetachable;
    j["dxcore"] = core.ok;
    std::string body = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    // P66：12_2 能力基线（mesh shader tier / SM / DXR / VRS / enhanced barriers）+ RTX 探测。
    // 它们故意是「字段串」而不是子对象：JVM 侧只认平坦键值对。
    appendJsonFields(body, capabilitiesJson(gCtx.caps));
    return body;
}

// ---------------------------------------------------------------------------
// P22: Blit 管线（全屏四边形，绕过 CopyTextureRegion 格式不兼容）
// ---------------------------------------------------------------------------
namespace {

// VS: 把 2D 位置直接作为 SV_Position 输出；texcoord 透传给 PS。
static const char kBlitVS[] = "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
                              "VS_OUT main(float2 pos : POSITION, float2 tc : TEXCOORD0) {\n"
                              "    VS_OUT o;\n"
                              "    o.pos = float4(pos, 0.0f, 1.0f);\n"
                              "    o.tc = tc;\n"
                              "    return o;\n"
                              "}\n";

// PS: 对源纹理线性采样，输出到 backbuffer；GPU 光栅器自动处理格式转换。
// 注意：'sampler' 是 HLSL 保留字，不能作变量名；PS 单独编译，须自带 VS_OUT 定义。
static const char kBlitPS[] = "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
                              "Texture2D<float4> srcTex : register(t0, space0);\n"
                              "SamplerState blitSampler : register(s0, space0);\n"
                              "float4 main(VS_OUT IN) : SV_Target {\n"
                              "    return srcTex.Sample(blitSampler, IN.tc);\n"
                              "}\n";

// P197（HDR）：同一份 blit 几何，像素着色器把 SDR 主渲染目标（RGBA8，内容按 sRGB 编码）
// 解码成线性、乘上「SDR 白 = 多少 scRGB 单位」，写进 R16G16B16A16_FLOAT 的交换链。
// scRGB 的 1.0 定义为 80 nits，所以 sdrWhiteScale = SDRWhiteLevel(nits) / 80。
// hdrEnabled 走 root constants：SDR 呈现时同一个 PSO 传 0 也能用（只是多一次分支）。
static const char kBlitPSHdr[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "Texture2D<float4> srcTex : register(t0, space0);\n"
    "SamplerState blitSampler : register(s0, space0);\n"
    "cbuffer BlitHdrParams : register(b0, space0) {\n"
    "    float sdrWhiteScale;  // SDR white (nits) / 80\n"
    "    float hdrEnabled;     // 0 = pass-through, 1 = sRGB->linear\n"
    "    float2 hdrPadding;\n"
    "};\n"
    "float3 srgbToLinear(float3 c) {\n"
    "    return (c <= 0.04045f) ? (c / 12.92f) : pow((c + 0.055f) / 1.055f, 2.4f);\n"
    "}\n"
    "float4 main(VS_OUT IN) : SV_Target {\n"
    "    float4 c = srcTex.Sample(blitSampler, IN.tc);\n"
    "    // hdrEnabled=0：float 链但没开 HDR（或色彩空间被拒）→ 原样直通，与 SDR 等价。\n"
    "    if (hdrEnabled < 0.5f) return c;\n"
    "    return float4(srgbToLinear(c.rgb) * sdrWhiteScale, c.a);\n"
    "}\n";

}  // namespace

mcdr::Result<void> initBlitPipeline() {
    if (auto d = ensureDevice(); !d) return mcdr::err("initBlitPipeline", errorMessage(d.error()));
    if (gCtx.blitPipeline) return {};  // 已初始化

    auto bp = std::make_unique<BlitPipeline>();

    ComPtr<ID3D12RootSignature> rootSig;
    {
        // 单个 descriptor table：SRV(t0) + static sampler(s0)
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE ranges[] = {srvRange};

        D3D12_ROOT_DESCRIPTOR_TABLE table{};
        table.NumDescriptorRanges = 1;
        table.pDescriptorRanges = ranges;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable = table;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // P197：blit 的 HDR 参数（sdrWhiteScale / hdrEnabled）。SDR PSO 不声明这个 cbuffer，
        // 但根签名里的参数必须一致，所以两个 PSO 共用同一份根签名，SDR 路径设成 (1,0) 即可。
        D3D12_ROOT_PARAMETER hdrParam{};
        hdrParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        hdrParam.Constants.ShaderRegister = 0;
        hdrParam.Constants.RegisterSpace = 0;
        hdrParam.Constants.Num32BitValues = 4;
        hdrParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_PARAMETER params[2] = {param, hdrParam};

        D3D12_STATIC_SAMPLER_DESC staticSam{};
        staticSam.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSam.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSam.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSam.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSam.MipLODBias = 0.0f;
        staticSam.MaxAnisotropy = 1;
        staticSam.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        staticSam.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        staticSam.MinLOD = 0.0f;
        staticSam.MaxLOD = D3D12_FLOAT32_MAX;
        staticSam.ShaderRegister = 0;
        staticSam.RegisterSpace = 0;
        staticSam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &staticSam;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
        if (FAILED(hr)) {
            return mcdr::err("initBlitPipeline", "initBlitPipeline: SerializeRootSignature hr=0x" + hrText(hr));
        }
        hr = gCtx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                              IID_PPV_ARGS(&rootSig));
        if (FAILED(hr)) {
            return mcdr::err("initBlitPipeline", "initBlitPipeline: CreateRootSignature hr=0x" + hrText(hr));
        }
    }

    // 编译 VS / PS。与主图形管线同一策略：硬要求 SM 6.9 + DXC，**没有 5.1(DXBC) 兜底**
    // （2026-10-25）—— 不满足就直接报错，blit 不通就等于 Present 不通，必须当场可见。
    const std::string smProfile = graphicsShaderModelProfile();
    if (smProfile.empty()) {
        return mcdr::err("initBlitPipeline", "initBlitPipeline: " + sm69RequirementText());
    }
    std::vector<uint8_t> vsDxil, psDxil;
    const std::string vsSrc(kBlitVS, sizeof(kBlitVS) - 1);
    const std::string psSrc(kBlitPS, sizeof(kBlitPS) - 1);
    auto vsDxc = compileHlslToDxil(vsSrc, "main", "vs_" + smProfile, vsDxil);
    if (!vsDxc) {
        return mcdr::err("initBlitPipeline", "blit_vs (DXC): " + firstLine(vsDxc.error().message));
    }
    auto psDxc = compileHlslToDxil(psSrc, "main", "ps_" + smProfile, psDxil);
    if (!psDxc) {
        return mcdr::err("initBlitPipeline", "blit_ps (DXC): " + firstLine(psDxc.error().message));
    }
    const void* vsCode = vsDxil.data();
    SIZE_T vsCodeSize = vsDxil.size();
    const void* psCode = psDxil.data();
    SIZE_T psCodeSize = psDxil.size();
    dbgLogInfo("initBlitPipeline: blit pipeline compiled with DXC SM %s (DXIL vs=%zuB ps=%zuB)", smProfile.c_str(),
               vsDxil.size(), psDxil.size());

    // P197：HDR 变体的像素着色器（几何与 VS 不变，只有 RTV 格式和 PS 不同）。
    std::vector<uint8_t> hdrPsDxil;
    const std::string hdrSrc(kBlitPSHdr, sizeof(kBlitPSHdr) - 1);
    auto hdrDxc = compileHlslToDxil(hdrSrc, "main", "ps_" + smProfile, hdrPsDxil);
    if (!hdrDxc) {
        return mcdr::err("initBlitPipeline", "blit HDR PS (DXC): " + firstLine(hdrDxc.error().message));
    }
    const void* hdrPsCode = hdrPsDxil.data();
    SIZE_T hdrPsCodeSize = hdrPsDxil.size();

    // 创建 PSO（P197：SDR 用 RGBA8、HDR 用 RGBA16F，共用同一份根签名与输入布局）
    auto createBlitPso = [&](const void* pixCode, SIZE_T pixSize, DXGI_FORMAT rtvFormat,
                             ComPtr<ID3D12PipelineState>& outPso) -> mcdr::Result<void> {
        D3D12_INPUT_ELEMENT_DESC ie[2] = {};
        ie[0].SemanticName = "POSITION";
        ie[0].SemanticIndex = 0;
        ie[0].Format = DXGI_FORMAT_R32G32_FLOAT;
        ie[0].InputSlot = 0;
        ie[0].AlignedByteOffset = 0;
        ie[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        ie[0].InstanceDataStepRate = 0;

        ie[1].SemanticName = "TEXCOORD";
        ie[1].SemanticIndex = 0;
        ie[1].Format = DXGI_FORMAT_R32G32_FLOAT;
        ie[1].InputSlot = 0;
        ie[1].AlignedByteOffset = 8;
        ie[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        ie[1].InstanceDataStepRate = 0;

        D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
        rtBlend.BlendEnable = FALSE;
        rtBlend.LogicOpEnable = FALSE;
        rtBlend.SrcBlend = D3D12_BLEND_ONE;
        rtBlend.DestBlend = D3D12_BLEND_ZERO;
        rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
        rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_BLEND_DESC blendDesc{};
        blendDesc.AlphaToCoverageEnable = FALSE;
        blendDesc.IndependentBlendEnable = FALSE;
        blendDesc.RenderTarget[0] = rtBlend;

        D3D12_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
        rasterDesc.CullMode = D3D12_CULL_MODE_NONE;  // 全屏 quad，无背面剔除
        rasterDesc.FrontCounterClockwise = FALSE;
        rasterDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        rasterDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        rasterDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        rasterDesc.DepthClipEnable = TRUE;
        rasterDesc.MultisampleEnable = FALSE;
        rasterDesc.AntialiasedLineEnable = FALSE;

        D3D12_DEPTH_STENCIL_DESC dsDesc{};
        dsDesc.DepthEnable = FALSE;
        dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        dsDesc.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = rootSig.Get();
        psoDesc.VS = {vsCode, vsCodeSize};
        psoDesc.PS = {pixCode, pixSize};
        psoDesc.InputLayout = {ie, 2};
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = rtvFormat;
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleMask = UINT_MAX;  // P25 fix：漏设 SampleMask → 0 → sample-mask 测试
        // 丢弃所有片元（dx12-native.log 632 行 WARNING）
        psoDesc.BlendState = blendDesc;
        psoDesc.RasterizerState = rasterDesc;
        psoDesc.DepthStencilState = dsDesc;
        psoDesc.Flags = D3D12_PIPELINE_STATE_FLAGS(0);

        ++gStatPsoCreate;
        const HRESULT hr = gCtx.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&outPso));
        if (FAILED(hr)) {
            return mcdr::err("initBlitPipeline",
                             "initBlitPipeline: CreateGraphicsPipelineState(rtv=" +
                                 std::to_string(static_cast<int>(rtvFormat)) + ") hr=0x" + hrText(hr));
        }
        return {};
    };

    ComPtr<ID3D12PipelineState> pso;
    if (auto cr = createBlitPso(psCode, psCodeSize, DXGI_FORMAT_R8G8B8A8_UNORM, pso); !cr) {
        return cr;
    }
    ComPtr<ID3D12PipelineState> psoHdr;
    if (auto cr = createBlitPso(hdrPsCode, hdrPsCodeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, psoHdr); !cr) {
        return cr;
    }

    // 顶点缓冲：4 顶点，R32G32(position) + R32G32(texcoord)，upload heap
    {
        struct Vertex {
            float x, y;
            float u, v;
        };
        Vertex verts[4] = {
            {-1.0f, -1.0f, 0.0f, 1.0f},  // bottom-left
            {1.0f, -1.0f, 1.0f, 1.0f},  // bottom-right
            {-1.0f, 1.0f, 0.0f, 0.0f},  // top-left
            {1.0f, 1.0f, 1.0f, 0.0f},  // top-right
        };
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(verts);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        HRESULT hr = gCtx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&bp->vertBuf));
        if (FAILED(hr)) {
            return mcdr::err("initBlitPipeline", "initBlitPipeline: vertBuf create hr=0x" + hrText(hr));
        }
        void* p = nullptr;
        hr = bp->vertBuf->Map(0, nullptr, &p);
        if (SUCCEEDED(hr) && p) {
            std::memcpy(p, verts, sizeof(verts));
            bp->vertBuf->Unmap(0, nullptr);
        }
        bp->vbView.BufferLocation = bp->vertBuf->GetGPUVirtualAddress();
        bp->vbView.SizeInBytes = sizeof(verts);
        bp->vbView.StrideInBytes = sizeof(Vertex);
    }

    // 索引缓冲：2 个三角形（顺时针，CullMode=NONE，顺序无关）
    {
        uint16_t idxs[6] = {0, 1, 2, 1, 3, 2};
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(idxs);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        HRESULT hr = gCtx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&bp->idxBuf));
        if (FAILED(hr)) {
            return mcdr::err("initBlitPipeline", "initBlitPipeline: idxBuf create hr=0x" + hrText(hr));
        }
        void* p = nullptr;
        hr = bp->idxBuf->Map(0, nullptr, &p);
        if (SUCCEEDED(hr) && p) {
            std::memcpy(p, idxs, sizeof(idxs));
            bp->idxBuf->Unmap(0, nullptr);
        }
        bp->ibView.BufferLocation = bp->idxBuf->GetGPUVirtualAddress();
        bp->ibView.SizeInBytes = sizeof(idxs);
        bp->ibView.Format = DXGI_FORMAT_R16_UINT;
    }

    bp->rootSig = rootSig;
    bp->pso = pso;
    bp->psoHdr = psoHdr;
    gCtx.blitPipeline = std::move(bp);
    dbgLogInfo("initBlitPipeline: done rootSig=%p pso=%p psoHdr=%p",
               reinterpret_cast<void*>(rootSig.Get()), reinterpret_cast<void*>(pso.Get()),
               reinterpret_cast<void*>(psoHdr.Get()));
    return {};
}

const BlitPipeline* getBlitPipeline() { return gCtx.blitPipeline.get(); }

// P121：锐化 PS —— 5-tap unsharp（中心 + 上下左右），参数走 32-bit 根常量 b1：
// {rcpSize.x, rcpSize.y, amount, pad}。alpha 恒 1（目标是 RGBA8 世界层/呈现目标）。
static const char kSharpenPS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "Texture2D<float4> srcTex : register(t0, space0);\n"
    "SamplerState sharpenSampler : register(s0, space0);\n"
    "cbuffer SharpenParams : register(b1, space0) {\n"
    "    float2 RcpSize;\n"
    "    float Amount;\n"
    "    float Pad0;\n"
    "};\n"
    "float4 main(VS_OUT IN) : SV_Target {\n"
    "    float3 c = srcTex.Sample(sharpenSampler, IN.tc).rgb;\n"
    "    float3 blur = (srcTex.Sample(sharpenSampler, IN.tc + float2( RcpSize.x, 0.0)).rgb\n"
    "                 + srcTex.Sample(sharpenSampler, IN.tc + float2(-RcpSize.x, 0.0)).rgb\n"
    "                 + srcTex.Sample(sharpenSampler, IN.tc + float2(0.0,  RcpSize.y)).rgb\n"
    "                 + srcTex.Sample(sharpenSampler, IN.tc + float2(0.0, -RcpSize.y)).rgb) * 0.25;\n"
    "    return float4(max(c + (c - blur) * Amount, 0.0), 1.0);\n"
    "}\n";

mcdr::Result<void> initSharpenPipeline() {
    if (auto d = ensureDevice(); !d) return mcdr::err("initSharpenPipeline", errorMessage(d.error()));
    if (gCtx.sharpenPipeline) return {};

    // 顶点/索引缓冲与 blit 共用：先确保 blit 管线建好，再把 view 抄过来。
    if (auto b = initBlitPipeline(); !b) {
        return mcdr::err("initSharpenPipeline", errorMessage(b.error()));
    }
    const BlitPipeline* bp = getBlitPipeline();
    if (bp == nullptr) {
        return mcdr::err("initSharpenPipeline", "initSharpenPipeline: blit pipeline unavailable");
    }

    auto sp = std::make_unique<SharpenPipeline>();
    ComPtr<ID3D12RootSignature> rootSig;
    {
        // 参数 0：SRV 描述符表（t0）；参数 1：4 个 32-bit 根常量（b1）。
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_DESCRIPTOR_TABLE table{};
        table.NumDescriptorRanges = 1;
        table.pDescriptorRanges = &srvRange;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = table;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 1;  // b1
        params[1].Constants.RegisterSpace = 0;
        params[1].Constants.Num32BitValues = 4;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC staticSam{};
        staticSam.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSam.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSam.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSam.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSam.MipLODBias = 0.0f;
        staticSam.MaxAnisotropy = 1;
        staticSam.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        staticSam.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        staticSam.MinLOD = 0.0f;
        staticSam.MaxLOD = D3D12_FLOAT32_MAX;
        staticSam.ShaderRegister = 0;
        staticSam.RegisterSpace = 0;
        staticSam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &staticSam;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
        if (FAILED(hr)) {
            return mcdr::err("initSharpenPipeline",
                             "initSharpenPipeline: SerializeRootSignature hr=0x" + hrText(hr));
        }
        hr = gCtx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                              IID_PPV_ARGS(&rootSig));
        if (FAILED(hr)) {
            return mcdr::err("initSharpenPipeline",
                             "initSharpenPipeline: CreateRootSignature hr=0x" + hrText(hr));
        }
    }

    // 编译 VS（复用 blit 的）/ PS：与主图形管线同一策略 —— 硬要求 SM 6.9 + DXC，无 5.1 兜底。
    const std::string smProfile = graphicsShaderModelProfile();
    if (smProfile.empty()) {
        return mcdr::err("initSharpenPipeline", "initSharpenPipeline: " + sm69RequirementText());
    }
    std::vector<uint8_t> vsDxil, psDxil;
    const std::string vsSrc(kBlitVS, sizeof(kBlitVS) - 1);
    const std::string psSrc(kSharpenPS, sizeof(kSharpenPS) - 1);
    auto vsDxc = compileHlslToDxil(vsSrc, "main", "vs_" + smProfile, vsDxil);
    if (!vsDxc) {
        return mcdr::err("initSharpenPipeline", "sharpen_vs (DXC): " + firstLine(vsDxc.error().message));
    }
    auto psDxc = compileHlslToDxil(psSrc, "main", "ps_" + smProfile, psDxil);
    if (!psDxc) {
        return mcdr::err("initSharpenPipeline", "sharpen_ps (DXC): " + firstLine(psDxc.error().message));
    }
    const void* vsCode = vsDxil.data();
    SIZE_T vsCodeSize = vsDxil.size();
    const void* psCode = psDxil.data();
    SIZE_T psCodeSize = psDxil.size();
    dbgLogInfo("initSharpenPipeline: compiled with DXC SM %s", smProfile.c_str());

    // PSO：与 blit 完全同状态（noDepth / noBlend / noCull），只是 PS 不同。
    ComPtr<ID3D12PipelineState> pso;
    {
        D3D12_INPUT_ELEMENT_DESC ie[2] = {};
        ie[0].SemanticName = "POSITION";
        ie[0].Format = DXGI_FORMAT_R32G32_FLOAT;
        ie[0].InputSlot = 0;
        ie[0].AlignedByteOffset = 0;
        ie[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        ie[1].SemanticName = "TEXCOORD";
        ie[1].Format = DXGI_FORMAT_R32G32_FLOAT;
        ie[1].InputSlot = 0;
        ie[1].AlignedByteOffset = 8;
        ie[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

        D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
        rtBlend.BlendEnable = FALSE;
        rtBlend.SrcBlend = D3D12_BLEND_ONE;
        rtBlend.DestBlend = D3D12_BLEND_ZERO;
        rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0] = rtBlend;

        D3D12_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
        rasterDesc.CullMode = D3D12_CULL_MODE_NONE;
        rasterDesc.DepthClipEnable = TRUE;

        D3D12_DEPTH_STENCIL_DESC dsDesc{};
        dsDesc.DepthEnable = FALSE;
        dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        dsDesc.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = rootSig.Get();
        psoDesc.VS = {vsCode, vsCodeSize};
        psoDesc.PS = {psCode, psCodeSize};
        psoDesc.InputLayout = {ie, 2};
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.BlendState = blendDesc;
        psoDesc.RasterizerState = rasterDesc;
        psoDesc.DepthStencilState = dsDesc;

        ++gStatPsoCreate;
        HRESULT hr = gCtx.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            return mcdr::err("initSharpenPipeline",
                             "initSharpenPipeline: CreateGraphicsPipelineState hr=0x" + hrText(hr));
        }
    }

    sp->rootSig = rootSig;
    sp->pso = pso;
    sp->vertBuf = bp->vertBuf;
    sp->idxBuf = bp->idxBuf;
    sp->vbView = bp->vbView;
    sp->ibView = bp->ibView;
    gCtx.sharpenPipeline = std::move(sp);
    dbgLogInfo("initSharpenPipeline: done rootSig=%p pso=%p", reinterpret_cast<void*>(rootSig.Get()),
               reinterpret_cast<void*>(pso.Get()));
    return {};
}

const SharpenPipeline* getSharpenPipeline() { return gCtx.sharpenPipeline.get(); }

mcdr::Result<void> blitBindSourceTexture(CommandContext* ctx, Dx12Object* srcTex,
                                         ID3D12GraphicsCommandList* cmd) {
    if (!srcTex || !srcTex->resource) {
        return mcdr::err("blitBindSourceTexture", "blitBindSourceTexture: null or invalid srcTex");
    }
    // 源纹理过渡到 PIXEL_SHADER_RESOURCE（采样必需）。
    transitionTextureTo(ctx, srcTex,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                            | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // P24 fix（黑屏根因之一）：SRV 必须写入命令列表当前设置的堆。
    // 此前在 srvHeap 分配 SRV 并直接 SetGraphicsRootDescriptorTable，但
    // beginCommandList 只 SetDescriptorHeaps({drawHeap}) → 每帧验证错误
    // "descriptor heap containing handle is different from currently set
    // descriptor heap"（dx12-native.log 658 行），根表绑定无效。
    // 改为复用 pushDescriptors 的机制：把 SRV 写到本帧 drawHeap 瞬态槽位，
    // root table 绑定该槽位的 GPU 句柄（与命令列表当前堆一致）。
    if (ctx->nextDrawSlot + 1 > syncRingCapacity()) {
        return mcdr::err("blitBindSourceTexture",
                         "blitBindSourceTexture: draw descriptor heap exhausted for this frame");
    }
    SIZE_T base = static_cast<SIZE_T>(ctx->drawHeapSlotBase + ctx->nextDrawSlot) * gCtx.drawInc;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = gCtx.drawHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += base;
    ctx->nextDrawSlot += 1;

    DXGI_FORMAT viewFmt = srcTex->resource->GetDesc().Format;
    switch (viewFmt) {
        case DXGI_FORMAT_D32_FLOAT: viewFmt = DXGI_FORMAT_R32_FLOAT; break;
        case DXGI_FORMAT_D16_UNORM: viewFmt = DXGI_FORMAT_R16_UNORM; break;
        default: break;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = viewFmt;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    gCtx.device->CreateShaderResourceView(srcTex->resource.Get(), &srvDesc, srvCpu);

    // 绑定根描述符表（表 0 = blit 的 SRV；drawHeap 已是命令列表当前设置的堆）。
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = gCtx.drawHeap->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += base;
    cmd->SetGraphicsRootDescriptorTable(0, srvGpu);
    // 逐帧诊断（每帧 1 次 stderr+文件 flush）→ DEBUG，需 DX12_LOG_VERBOSE=1 才输出。
    dbgLogDebug("blitBindSourceTexture: drawHeapSlotBase=%u nextDrawSlot=%u srvGpu=%llx",
                ctx->drawHeapSlotBase, ctx->nextDrawSlot, static_cast<unsigned long long>(srvGpu.ptr));
    return {};
}

// ---------------------------------------------------------------------------
// P104: SMAA（Subpixel Morphological Antialiasing）
// ---------------------------------------------------------------------------
// 插入点：blitSurface 把主颜色目标（源纹理）拷到 back buffer **之前**（见 d3d12_surface.cpp）。
// blit 本身就是"采样全屏四边形"（P22），所以这里天然能插后处理；SMAA 只看最终 LDR 颜色
// （亮度边检测），不需要深度/法线/运动矢量。
// 三个阶段，全部在**源分辨率**上跑，第三步顺带线性放大到窗口尺寸（⇒ 与分辨率解耦兼容）：
//   ① 边检测   source            → edges   (R8G8_UNORM，.r=与左邻有边 .g=与上邻有边)
//   ② 混合权重 edges + LUTs      → weights (R8G8_UNORM，.r=沿 x 混 .g=沿 y 混)
//   ③ 邻域混合 source + weights  → back buffer（写进 back buffer ⇒ 调用方跳过纯 blit）
// ② 用的是**官方算法**：把上游 SMAA.hlsl 的 SMAABlendingWeightCalculationPS（含斜向搜索、
// 拐角修正）整段搬进来，权重由 AreaTex(160x560 RG8) / SearchTex(64x16 R8) 两张 LUT 查表得出
// （数据见 include\mcdr_smaa_luts.h，字节级等于上游 Textures\AreaTexDX10.dds / SearchTex.dds）。
// LUT 初始化失败时②退回 kSmaaWeightPS 的解析式近似（WARN 一行，功能不降级为崩溃/黑屏）。
// TAA（模式 2）的实现见本文件末尾的 P152 分节（applyTaaToTexture，历史乒乓由 native 持有）；
// DLAA（3）仍由 DLSS 特性实现，setAntiAliasing 收到 3 时只记一条 WARN。
namespace {

// ① 边检测：亮度差超阈值即在 .r（与左邻）/ .g（与上邻）打标记。
const char kSmaaEdgePS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "Texture2D<float4> srcTex : register(t0, space0);\n"
    "SamplerState smaaSampler : register(s0, space0);\n"
    "static const float kThreshold = 0.10f;\n"
    "static const float3 kLumaW = float3(0.2126f, 0.7152f, 0.0722f);\n"
    "float SmaaLuma(float3 c) { return dot(c, kLumaW); }\n"
    "float4 main(VS_OUT IN) : SV_Target {\n"
    "    uint w, h; srcTex.GetDimensions(w, h);\n"
    "    float2 texel = float2(1.0f / (float)w, 1.0f / (float)h);\n"
    "    float lc = SmaaLuma(srcTex.SampleLevel(smaaSampler, IN.tc, 0).rgb);\n"
    "    float ll = SmaaLuma(srcTex.SampleLevel(smaaSampler, IN.tc - float2(texel.x, 0.0f), 0).rgb);\n"
    "    float lt = SmaaLuma(srcTex.SampleLevel(smaaSampler, IN.tc - float2(0.0f, texel.y), 0).rgb);\n"
    "    float2 e = step(kThreshold, float2(abs(lc - ll), abs(lc - lt)));\n"
    "    return float4(e, 0.0f, 0.0f);\n"
    "}\n";

// ② 混合权重：沿边往上下（垂直边）或左右（水平边）找最近的"台阶转折"，越近权重越大。
// 找不到转折（直边）⇒ 权重 0，直边不会被糊。
const char kSmaaWeightPS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "Texture2D<float2> edgesTex : register(t0, space0);\n"
    "SamplerState smaaSampler : register(s0, space0);\n"
    "static const int kMaxSearch = 8;\n"
    "float4 main(VS_OUT IN) : SV_Target {\n"
    "    uint w, h; edgesTex.GetDimensions(w, h);\n"
    "    float2 texel = float2(1.0f / (float)w, 1.0f / (float)h);\n"
    "    float2 e = edgesTex.SampleLevel(smaaSampler, IN.tc, 0).rg;\n"
    "    float wx = 0.0f, wy = 0.0f;\n"
    "    if (e.r > 0.5f) {\n"          // 本像素左侧有垂直边 → 沿 y 找台阶
    "        int up = 0, down = 0;\n"
    "        for (int k = 1; k <= kMaxSearch; ++k) {\n"
    "            float2 n = edgesTex.SampleLevel(smaaSampler, IN.tc - float2(0.0f, texel.y * k), 0).rg;\n"
    "            if (n.r < 0.5f) { up = (n.g > 0.5f) ? k : 0; break; }\n"
    "        }\n"
    "        for (int k = 1; k <= kMaxSearch; ++k) {\n"
    "            float2 n = edgesTex.SampleLevel(smaaSampler, IN.tc + float2(0.0f, texel.y * k), 0).rg;\n"
    "            if (n.r < 0.5f) { down = (n.g > 0.5f) ? k : 0; break; }\n"
    "        }\n"
    "        int near = (up == 0) ? down : ((down == 0) ? up : min(up, down));\n"
    "        if (near > 0) wx = saturate(0.5f / (float)near);\n"
    "    }\n"
    "    if (e.g > 0.5f) {\n"          // 本像素上方有水平边 → 沿 x 找台阶
    "        int left = 0, right = 0;\n"
    "        for (int k = 1; k <= kMaxSearch; ++k) {\n"
    "            float2 n = edgesTex.SampleLevel(smaaSampler, IN.tc - float2(texel.x * k, 0.0f), 0).rg;\n"
    "            if (n.g < 0.5f) { left = (n.r > 0.5f) ? k : 0; break; }\n"
    "        }\n"
    "        for (int k = 1; k <= kMaxSearch; ++k) {\n"
    "            float2 n = edgesTex.SampleLevel(smaaSampler, IN.tc + float2(texel.x * k, 0.0f), 0).rg;\n"
    "            if (n.g < 0.5f) { right = (n.r > 0.5f) ? k : 0; break; }\n"
    "        }\n"
    "        int near = (left == 0) ? right : ((right == 0) ? left : min(left, right));\n"
    "        if (near > 0) wy = saturate(0.5f / (float)near);\n"
    "    }\n"
    "    return float4(wx, wy, 0.0f, 1.0f);\n"
    "}\n";

// ②′ 混合权重（**官方 LUT 算法**）：与上游 SMAA.hlsl 的 SMAABlendingWeightCalculationPS 同一
// 算法与同一套常量（中等预设），只做了三处移植改动（都不改数学）：
//   - 上游 tex2D/tex2Dlod 宏 → 本工程写法：edges 用传进来的 smaaSampler，两张 LUT 用
//     smaaLutSampler（静态采样器 s1）；三张纹理都只写 SampleLevel(..., 0)。
//     上游的常量缓冲 smaa_rt_metrics / subsampleIndices / smaa_area_search_texel_size 换成
//     两个局部量 + 逐层传参：rt = float4(texelSize.xy, 1/texelSize)（对应上游 SMAA_RT_METRICS，
//     用 edges 的 texel size 在 main 里算出）、areaTexel/searchTexel = LUT 的 1/尺寸
//     （对应上游 SMAA_AREATEX_PIXEL_SIZE = 1/float2(160,560)、1/SMAA_SEARCHTEX_PACKED_SIZE
//     = 1/float2(64,16)）。之所以不照抄全局量：HLSL 的全局变量是隐式 const，不能像上游那样
//     在 ps 里赋值。SMAA 1x 的 subsampleIndices 恒 0。
//   - 上游的提前 return（SMAASearchXLeft 等函数体内的 while 循环）改成单出口：
//     循环体内 `if (!cond) break;` 等价于原 `while(cond)` 的退出条件，返回表达式逐字不变。
//     这样 SM 5.1 与 SM 6.x（DXC）都能编过，不依赖 fxc 的循环推断。
//   - 上游 r/g 与 b/a 的通道含义保持不变（r=沿 x 混、g=沿 y 混；b/a 是斜向权重，③ 本来就不用），
//     所以第 ③ 趟的读取约定一个字都不用改。
const char kSmaaWeightLutPS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "Texture2D<float2> edgesTex : register(t0, space0);\n"
    "Texture2D<float2> areaTex  : register(t1, space0);\n"
    "Texture2D<float>  searchTex: register(t2, space0);\n"
    "SamplerState smaaSampler    : register(s0, space0);\n"  // MIN_MAG_MIP_LINEAR + CLAMP（原样保留）
    "SamplerState smaaLutSampler : register(s1, space0);\n"  // MIN_MAG_LINEAR_MIP_POINT + CLAMP
    // ---- 上游 SMAA.hlsl 的非可配置常量 ----
    "#define SMAA_THRESHOLD 0.10\n"                      // = 本工程既有 kThreshold（中等预设）
    "#define SMAA_MAX_SEARCH_STEPS 16\n"                // 中等预设
    "#define SMAA_MAX_SEARCH_STEPS_DIAG 8\n"            // 中等预设
    "#define SMAA_CORNER_ROUNDING 25\n"                 // 中等预设
    "#define SMAA_CORNER_ROUNDING_NORM 0.25\n"          // 25/100，常量折叠后避免隐式转换告警
    "#define SMAA_AREATEX_MAX_DISTANCE 16\n"
    "#define SMAA_AREATEX_MAX_DISTANCE_DIAG 20\n"
    "#define SMAA_AREATEX_SUBTEX_SIZE (1.0 / 7.0)\n"
    "#define SMAA_SEARCHTEX_SIZE float2(66.0, 33.0)\n"
    "#define SMAA_SEARCHTEX_PACKED_SIZE float2(64.0, 16.0)\n"
    // 两张 LUT 的 1/尺寸：上游写成 float2(1.0/160.0, 1.0/560.0) 与 float2(1.0/64.0, 1.0/16.0)。
    // 它们在 main 里由 areaTex / searchTex 的 GetDimensions 算出后逐层传参，数值完全一致
    // （HLSL 的全局变量是隐式 const，不能像上游那样在 ps 里赋值给全局，所以改成参数）。
    // 上游的两个宏在 HLSL4/5 下就是 smaaLutSampler + SampleLevel(..., 0)。
    "#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(smaaLutSampler, coord, 0)\n"
    "#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(smaaLutSampler, coord, 0, offset)\n"
    // ---- 上游 Diagonal Search Functions（SMAA.hlsl :831-985）----
    "float2 SMAADecodeDiagBilinearAccess(float2 e) {\n"
    "    e.r = e.r * abs(5.0 * e.r - 5.0 * 0.75);\n"
    "    return round(e);\n"
    "}\n"
    "float4 SMAADecodeDiagBilinearAccess(float4 e) {\n"
    "    e.rb = e.rb * abs(5.0 * e.rb - 5.0 * 0.75);\n"
    "    return round(e);\n"
    "}\n"
    "void SMAAMovc(bool2 cond, inout float2 variable, float2 value) {\n"
    "    if (cond.x) variable.x = value.x;\n"
    "    if (cond.y) variable.y = value.y;\n"
    "}\n"
    "float2 SMAASearchDiag1(float2 rt, float2 texcoord, float2 dir, out float2 e) {\n"
    "    float4 coord = float4(texcoord, -1.0, 1.0);\n"
    "    float3 t = float3(rt.xy, 1.0);\n"
    "    for (;;) {\n"
    "        if (!(coord.z < (float)(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9)) break;\n"
    "        coord.xyz = mad(t, float3(dir, 1.0), coord.xyz);\n"
    "        e = SMAASampleLevelZero(edgesTex, coord.xy).rg;\n"
    "        coord.w = dot(e, float2(0.5, 0.5));\n"
    "    }\n"
    "    return coord.zw;\n"
    "}\n"
    "float2 SMAASearchDiag2(float2 rt, float2 texcoord, float2 dir, out float2 e) {\n"
    "    float4 coord = float4(texcoord, -1.0, 1.0);\n"
    "    coord.x += 0.25 * rt.x;\n"  // @SearchDiag2Optimization
    "    float3 t = float3(rt.xy, 1.0);\n"
    "    for (;;) {\n"
    "        if (!(coord.z < (float)(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9)) break;\n"
    "        coord.xyz = mad(t, float3(dir, 1.0), coord.xyz);\n"
    "        e = SMAASampleLevelZero(edgesTex, coord.xy).rg;\n"
    "        e = SMAADecodeDiagBilinearAccess(e);\n"
    "        coord.w = dot(e, float2(0.5, 0.5));\n"
    "    }\n"
    "    return coord.zw;\n"
    "}\n"
    "float2 SMAAAreaDiag(float2 areaTexel, float2 dist, float2 e, float offset) {\n"
    "    float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE_DIAG, SMAA_AREATEX_MAX_DISTANCE_DIAG), e, dist);\n"
    "    texcoord = mad(areaTexel, texcoord, 0.5 * areaTexel);\n"
    "    texcoord.x += 0.5;\n"  // 斜向区域在 AreaTex 的右半边
    "    texcoord.y += SMAA_AREATEX_SUBTEX_SIZE * offset;\n"
    "    return SMAASampleLevelZero(areaTex, texcoord).rg;\n"
    "}\n"
    "float2 SMAACalculateDiagWeights(float2 rt, float2 areaTexel, float2 texcoord, float2 e, float4 subsampleIndices) {\n"
    "    float2 weights = float2(0.0, 0.0);\n"
    "    float4 d;\n"
    "    float2 end;\n"
    "    if (e.r > 0.0) {\n"
    "        d.xz = SMAASearchDiag1(rt, texcoord, float2(-1.0, 1.0), end);\n"
    "        d.x += (float)(end.y > 0.9);\n"
    "    } else {\n"
    "        d.xz = float2(0.0, 0.0);\n"
    "    }\n"
    "    d.yw = SMAASearchDiag1(rt, texcoord, float2(1.0, -1.0), end);\n"
    "    [branch]\n"
    "    if (d.x + d.y > 2.0) {\n"
    "        float4 coords = mad(float4(-d.x + 0.25, d.x, d.y, -d.y - 0.25), rt.xyxy, texcoord.xyxy);\n"
    "        float4 c;\n"
    "        c.xy = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(-1, 0)).rg;\n"
    "        c.zw = SMAASampleLevelZeroOffset(edgesTex, coords.zw, int2(1, 0)).rg;\n"
    "        c.yxwz = SMAADecodeDiagBilinearAccess(c.xyzw);\n"
    "        float2 cc = mad(float2(2.0, 2.0), c.xz, c.yw);\n"
    "        SMAAMovc(bool2(step(0.9, d.zw)), cc, float2(0.0, 0.0));\n"
    "        weights += SMAAAreaDiag(areaTexel, d.xy, cc, subsampleIndices.z);\n"
    "    }\n"
    "    d.xz = SMAASearchDiag2(rt, texcoord, float2(-1.0, -1.0), end);\n"
    "    if (SMAASampleLevelZeroOffset(edgesTex, texcoord, int2(1, 0)).r > 0.0) {\n"
    "        d.yw = SMAASearchDiag2(rt, texcoord, float2(1.0, 1.0), end);\n"
    "        d.y += (float)(end.y > 0.9);\n"
    "    } else {\n"
    "        d.yw = float2(0.0, 0.0);\n"
    "    }\n"
    "    [branch]\n"
    "    if (d.x + d.y > 2.0) {\n"
    "        float4 coords = mad(float4(-d.x, -d.x, d.y, d.y), rt.xyxy, texcoord.xyxy);\n"
    "        float4 c;\n"
    "        c.x = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(-1, 0)).g;\n"
    "        c.y = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(0, -1)).r;\n"
    "        c.zw = SMAASampleLevelZeroOffset(edgesTex, coords.zw, int2(1, 0)).gr;\n"
    "        float2 cc = mad(float2(2.0, 2.0), c.xz, c.yw);\n"
    "        SMAAMovc(bool2(step(0.9, d.zw)), cc, float2(0.0, 0.0));\n"
    "        weights += SMAAAreaDiag(areaTexel, d.xy, cc, subsampleIndices.w).gr;\n"
    "    }\n"
    "    return weights;\n"
    "}\n"
    // ---- 上游 Horizontal/Vertical Search Functions（SMAA.hlsl :997-1103）----
    "float SMAASearchLength(float2 searchTexel, float2 e, float offset) {\n"
    "    float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5, -1.0);\n"
    "    float2 bias = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0);\n"
    "    scale += float2(-1.0, 1.0);\n"
    "    bias += float2(0.5, -0.5);\n"
    "    scale *= searchTexel;\n"  // 上游 *= 1.0/SMAA_SEARCHTEX_PACKED_SIZE
    "    bias *= searchTexel;\n"
    "    return SMAASampleLevelZero(searchTex, mad(scale, e, bias)).r;\n"
    "}\n"
    "float SMAASearchXLeft(float2 rt, float2 searchTexel, float2 texcoord, float end) {\n"
    "    float2 e = float2(0.0, 1.0);\n"
    "    for (;;) {\n"
    "        if (!(texcoord.x > end && e.g > 0.8281 && e.r == 0.0)) break;\n"
    "        e = SMAASampleLevelZero(edgesTex, texcoord).rg;\n"
    "        texcoord = mad(-float2(2.0, 0.0), rt.xy, texcoord);\n"
    "    }\n"
    "    float offset = mad(-(255.0 / 127.0), SMAASearchLength(searchTexel, e, 0.0), 3.25);\n"
    "    return mad(rt.x, offset, texcoord.x);\n"
    "}\n"
    "float SMAASearchXRight(float2 rt, float2 searchTexel, float2 texcoord, float end) {\n"
    "    float2 e = float2(0.0, 1.0);\n"
    "    for (;;) {\n"
    "        if (!(texcoord.x < end && e.g > 0.8281 && e.r == 0.0)) break;\n"
    "        e = SMAASampleLevelZero(edgesTex, texcoord).rg;\n"
    "        texcoord = mad(float2(2.0, 0.0), rt.xy, texcoord);\n"
    "    }\n"
    "    float offset = mad(-(255.0 / 127.0), SMAASearchLength(searchTexel, e, 0.5), 3.25);\n"
    "    return mad(-rt.x, offset, texcoord.x);\n"
    "}\n"
    "float SMAASearchYUp(float2 rt, float2 searchTexel, float2 texcoord, float end) {\n"
    "    float2 e = float2(1.0, 0.0);\n"
    "    for (;;) {\n"
    "        if (!(texcoord.y > end && e.r > 0.8281 && e.g == 0.0)) break;\n"
    "        e = SMAASampleLevelZero(edgesTex, texcoord).rg;\n"
    "        texcoord = mad(-float2(0.0, 2.0), rt.xy, texcoord);\n"
    "    }\n"
    "    float offset = mad(-(255.0 / 127.0), SMAASearchLength(searchTexel, e.gr, 0.0), 3.25);\n"
    "    return mad(rt.y, offset, texcoord.y);\n"
    "}\n"
    "float SMAASearchYDown(float2 rt, float2 searchTexel, float2 texcoord, float end) {\n"
    "    float2 e = float2(1.0, 0.0);\n"
    "    for (;;) {\n"
    "        if (!(texcoord.y < end && e.r > 0.8281 && e.g == 0.0)) break;\n"
    "        e = SMAASampleLevelZero(edgesTex, texcoord).rg;\n"
    "        texcoord = mad(float2(0.0, 2.0), rt.xy, texcoord);\n"
    "    }\n"
    "    float offset = mad(-(255.0 / 127.0), SMAASearchLength(searchTexel, e.gr, 0.5), 3.25);\n"
    "    return mad(-rt.y, offset, texcoord.y);\n"
    "}\n"
    "float2 SMAAArea(float2 areaTexel, float2 dist, float e1, float e2, float offset) {\n"
    "    float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE),\n"
    "                          round(4.0 * float2(e1, e2)), dist);\n"
    "    texcoord = mad(areaTexel, texcoord, 0.5 * areaTexel);\n"
    "    texcoord.y = mad(SMAA_AREATEX_SUBTEX_SIZE, offset, texcoord.y);\n"
    "    return SMAASampleLevelZero(areaTex, texcoord).rg;\n"
    "}\n"
    // ---- 上游 Corner Detection（SMAA.hlsl :1108-1140）----
    "void SMAADetectHorizontalCornerPattern(inout float2 weights, float4 texcoord, float2 d) {\n"
    "    float2 leftRight = step(d.xy, d.yx);\n"
    "    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;\n"
    "    rounding /= leftRight.x + leftRight.y;\n"
    "    float2 factor = float2(1.0, 1.0);\n"
    "    factor.x -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(0, 1)).r;\n"
    "    factor.x -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1, 1)).r;\n"
    "    factor.y -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(0, -2)).r;\n"
    "    factor.y -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1, -2)).r;\n"
    "    weights *= saturate(factor);\n"
    "}\n"
    "void SMAADetectVerticalCornerPattern(inout float2 weights, float4 texcoord, float2 d) {\n"
    "    float2 leftRight = step(d.xy, d.yx);\n"
    "    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;\n"
    "    rounding /= leftRight.x + leftRight.y;\n"
    "    float2 factor = float2(1.0, 1.0);\n"
    "    factor.x -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(1, 0)).g;\n"
    "    factor.x -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1, 1)).g;\n"
    "    factor.y -= rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(-2, 0)).g;\n"
    "    factor.y -= rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(-2, 1)).g;\n"
    "    weights *= saturate(factor);\n"
    "}\n"
    // ---- 上游 Blending Weight Calculation（SMAA.hlsl :1145-1247），子采样索引恒 0（SMAA 1x）----
    "float4 main(VS_OUT IN) : SV_Target {\n"
    "    uint w, h; edgesTex.GetDimensions(w, h);\n"
    "    float2 texelSize = float2(1.0 / (float)w, 1.0 / (float)h);\n"
    "    float4 rt = float4(texelSize.x, texelSize.y, 1.0 / texelSize.x, 1.0 / texelSize.y);\n"
    "    uint aw, ah; areaTex.GetDimensions(aw, ah);\n"
    "    float2 areaTexel = float2(1.0 / (float)aw, 1.0 / (float)ah);\n"
    "    uint sw, sh; searchTex.GetDimensions(sw, sh);\n"
    "    float2 searchTexel = float2(1.0 / (float)sw, 1.0 / (float)sh);\n"
    "    float2 pixcoord = IN.tc * float2((float)w, (float)h);\n"
    "    float4 offset[3];\n"
    "    offset[0] = mad(texelSize.xyxy, float4(-0.25, -0.125, 0.25, -0.125), IN.tc.xyxy);\n"
    "    offset[1] = mad(texelSize.xyxy, float4(-0.125, -0.25, -0.125, 0.25), IN.tc.xyxy);\n"
    "    offset[2] = mad(texelSize.xyxy, float4(-2.0, -2.0, 2.0, 2.0), IN.tc.xyxy);\n"
    "    float4 subsampleIndices = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    float4 weights = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    float2 e = edgesTex.SampleLevel(smaaSampler, IN.tc, 0).rg;\n"
    "    [branch]\n"
    "    if (e.g > 0.0) {\n"
    "        weights.rg = SMAACalculateDiagWeights(rt.xy, areaTexel, IN.tc, e, subsampleIndices);\n"
    "        [branch]\n"
    "        if (weights.r == -weights.g) {\n"
    "            float2 d;\n"
    "            float3 coords;\n"
    "            coords.x = SMAASearchXLeft(rt.xy, areaTexel, offset[0].xy, offset[2].x);\n"
    "            coords.y = offset[1].y;\n"
    "            d.x = coords.x;\n"
    "            float e1 = SMAASampleLevelZero(edgesTex, coords.xy).r;\n"
    "            coords.z = SMAASearchXRight(rt.xy, areaTexel, offset[0].zw, offset[2].y);\n"
    "            d.y = coords.z;\n"
    "            d = abs(round(mad(rt.zz, d, -pixcoord.xx)));\n"
    "            float2 sqrt_d = sqrt(d);\n"
    "            float e2 = SMAASampleLevelZeroOffset(edgesTex, coords.zy, int2(1, 0)).r;\n"
    "            weights.rg = SMAAArea(areaTexel, sqrt_d, e1, e2, subsampleIndices.y);\n"
    "            coords.y = IN.tc.y;\n"
    "            SMAADetectHorizontalCornerPattern(weights.rg, coords.xyzy, d);\n"
    "        } else {\n"
    "            e.r = 0.0;\n"  // 命中斜向 ⇒ 跳过竖直方向的搜索
    "        }\n"
    "    }\n"
    "    [branch]\n"
    "    if (e.r > 0.0) {\n"
    "        float2 d;\n"
    "        float3 coords;\n"
    "        coords.y = SMAASearchYUp(rt.xy, areaTexel, offset[1].xy, offset[2].z);\n"
    "        coords.x = offset[0].x;\n"
    "        d.x = coords.y;\n"
    "        float e1 = SMAASampleLevelZero(edgesTex, coords.xy).g;\n"
    "        coords.z = SMAASearchYDown(rt.xy, areaTexel, offset[1].zw, offset[2].w);\n"
    "        d.y = coords.z;\n"
    "        d = abs(round(mad(rt.ww, d, -pixcoord.yy)));\n"
    "        float2 sqrt_d = sqrt(d);\n"
    "        float e2 = SMAASampleLevelZeroOffset(edgesTex, coords.xz, int2(0, 1)).g;\n"
    "        weights.ba = SMAAArea(areaTexel, sqrt_d, e1, e2, subsampleIndices.x);\n"
    "        coords.x = IN.tc.x;\n"
    "        SMAADetectVerticalCornerPattern(weights.ba, coords.xyxz, d);\n"
    "    }\n"
    "    return weights;\n"
    "}\n";

// ③ 邻域混合：取四邻权重里最大的那个，朝该方向做半像素偏移采样后按权重插值。
// 源色用同一个线性采样器 ⇒ 源分辨率 < 窗口时这一步同时完成升采样。
const char kSmaaBlendPS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "Texture2D<float4> colorTex : register(t0, space0);\n"
    "Texture2D<float2> weightsTex : register(t1, space0);\n"
    "SamplerState smaaSampler : register(s0, space0);\n"
    "float4 main(VS_OUT IN) : SV_Target {\n"
    "    uint w, h; weightsTex.GetDimensions(w, h);\n"
    "    float2 texel = float2(1.0f / (float)w, 1.0f / (float)h);\n"
    "    float wl = weightsTex.SampleLevel(smaaSampler, IN.tc - float2(texel.x, 0.0f), 0).r;\n"
    "    float wr = weightsTex.SampleLevel(smaaSampler, IN.tc + float2(texel.x, 0.0f), 0).r;\n"
    "    float wu = weightsTex.SampleLevel(smaaSampler, IN.tc - float2(0.0f, texel.y), 0).g;\n"
    "    float wd = weightsTex.SampleLevel(smaaSampler, IN.tc + float2(0.0f, texel.y), 0).g;\n"
    "    float best = 0.0f;\n"
    "    float2 dir = float2(0.0f, 0.0f);\n"
    "    if (wl > best) { best = wl; dir = float2(-1.0f, 0.0f); }\n"
    "    if (wr > best) { best = wr; dir = float2(1.0f, 0.0f); }\n"
    "    if (wu > best) { best = wu; dir = float2(0.0f, -1.0f); }\n"
    "    if (wd > best) { best = wd; dir = float2(0.0f, 1.0f); }\n"
    "    float3 center = colorTex.SampleLevel(smaaSampler, IN.tc, 0).rgb;\n"
    "    if (best <= 0.0f) return float4(center, 1.0f);\n"
    "    float3 blended = colorTex.SampleLevel(smaaSampler, IN.tc + dir * texel * 0.5f, 0).rgb;\n"
    "    return float4(lerp(center, blended, saturate(best)), 1.0f);\n"
    "}\n";

// SMAA 中间纹理当前是否停在 PIXEL_SHADER_RESOURCE（刚创建时是 COMMON）。
bool gSmaaTargetsInSrvState = false;

// 建/取中间纹理（尺寸变化时重建）。
mcdr::Result<void> smaaEnsureTargets(UINT64 w, UINT64 h) {
    if (gCtx.smaaEdges && gCtx.smaaWidth == w && gCtx.smaaHeight == h) {
        return {};
    }
    if (!gCtx.smaaRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        // P104b：edges / weights / worldResult（就地 SMAA 的第三趟输出）三张。
        hd.NumDescriptors = 3;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hr = gCtx.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gCtx.smaaRtvHeap));
        if (FAILED(hr)) {
            return mcdr::err("smaaEnsureTargets", "CreateDescriptorHeap(RTV 3) hr=0x" + hrText(hr));
        }
        gCtx.smaaEdgesRtv = gCtx.smaaRtvHeap->GetCPUDescriptorHandleForHeapStart();
        gCtx.smaaWeightsRtv = gCtx.smaaEdgesRtv;
        gCtx.smaaWeightsRtv.ptr += gCtx.rtvInc;
        gCtx.smaaWorldResultRtv = gCtx.smaaEdgesRtv;
        gCtx.smaaWorldResultRtv.ptr += gCtx.rtvInc * 2;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = static_cast<UINT>(h);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    gCtx.smaaEdges.Reset();
    gCtx.smaaWeights.Reset();
    HRESULT hr = gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&gCtx.smaaEdges));
    if (FAILED(hr)) {
        return mcdr::err("smaaEnsureTargets", "edges CreateCommittedResource hr=0x" + hrText(hr));
    }
    hr = gCtx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&gCtx.smaaWeights));
    if (FAILED(hr)) {
        return mcdr::err("smaaEnsureTargets", "weights CreateCommittedResource hr=0x" + hrText(hr));
    }
    gCtx.device->CreateRenderTargetView(gCtx.smaaEdges.Get(), nullptr, gCtx.smaaEdgesRtv);
    gCtx.device->CreateRenderTargetView(gCtx.smaaWeights.Get(), nullptr, gCtx.smaaWeightsRtv);
    gCtx.smaaWidth = w;
    gCtx.smaaHeight = h;
    gSmaaTargetsInSrvState = false;
    dbgLogInfo("P104: SMAA intermediate targets %llux%llu (R8G8_UNORM) ready", static_cast<unsigned long long>(w),
               static_cast<unsigned long long>(h));
    return {};
}

// ---------------------------------------------------------------------------
// P104：官方 LUT（AreaTex 160x560 RG8 / SearchTex 64x16 R8）
// ---------------------------------------------------------------------------
// 数据来自 include\mcdr_smaa_luts.h（定义在 src\advanced\smaa_luts.cpp，字节级等于上游
// Textures\AreaTexDX10.dds / SearchTex.dds，见该头文件里的来源与 SHA-1）。
//
// 只需 **一次** 上传，所以走 createCommandList + 队列 fence 的一次性提交（与
// dbgReadbackTexturePixels 同一手法），而不是挂在每帧的 CommandContext 上：
// 初始化发生在 applySmaaToTexture 里「世界帧尚未开始录制」的时刻（Java 侧在画世界之前
// 就调 setAntiAliasing + 绑定世界目标），且提交后立刻 deviceWaitIdle，GPU 侧资源一定
// 已经落地，后续每帧只读。
//
// 失败语义：**任何一步失败都只返回错误，绝不让 SMAA 变黑/崩溃** —— 调用方
// （smaaEnsureLuts 的调用点，见 applySmaaToTexture）会把权重趟退回既有的解析式
// kSmaaWeightPS 并 WARN 一行。
constexpr D3D12_RESOURCE_STATES kSmaaLutReadyState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// 建一张只读 LUT 纹理并把它 copy 到 DEFAULT heap。srcRowBytes = 每行真实字节数
// （2 或 1），行距按 D3D12_TEXTURE_DATA_PITCH_ALIGNMENT 向上取整。
mcdr::Result<void> smaaCreateLutTexture(const unsigned char* data, UINT width, UINT height, UINT srcRowBytes,
                                        DXGI_FORMAT format, ComPtr<ID3D12Resource>& out) {
    if (data == nullptr || width == 0 || height == 0 || srcRowBytes == 0) {
        return mcdr::err("smaaCreateLutTexture", "invalid LUT parameters");
    }
    const UINT rowPitch = (srcRowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                          & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 uploadBytes = static_cast<UINT64>(rowPitch) * height;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    out.Reset();
    HRESULT hr = gCtx.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&out));
    if (FAILED(hr)) {
        return mcdr::err("smaaCreateLutTexture", "CreateCommittedResource(texture " + std::to_string(width) + "x"
                                                     + std::to_string(height) + ") hr=0x" + hrText(hr));
    }

    D3D12_RESOURCE_DESC upDesc{};
    upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upDesc.Width = uploadBytes;
    upDesc.Height = 1;
    upDesc.DepthOrArraySize = 1;
    upDesc.MipLevels = 1;
    upDesc.SampleDesc.Count = 1;
    upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    upDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES upHeap{};
    upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload;
    hr = gCtx.device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(hr)) {
        return mcdr::err("smaaCreateLutTexture", "CreateCommittedResource(upload " + std::to_string(uploadBytes)
                                                     + "B) hr=0x" + hrText(hr));
    }
    void* mapped = nullptr;
    hr = upload->Map(0, nullptr, &mapped);
    if (FAILED(hr) || mapped == nullptr) {
        return mcdr::err("smaaCreateLutTexture", "upload Map hr=0x" + hrText(hr));
    }
    auto* dst = static_cast<unsigned char*>(mapped);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<SIZE_T>(y) * rowPitch,
                    data + static_cast<SIZE_T>(y) * srcRowBytes, srcRowBytes);
    }
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> alloc;
    hr = gCtx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (FAILED(hr)) {
        return mcdr::err("smaaCreateLutTexture", "CreateCommandAllocator hr=0x" + hrText(hr));
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    hr = gCtx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                        IID_PPV_ARGS(&list));
    if (FAILED(hr)) {
        return mcdr::err("smaaCreateLutTexture", "CreateCommandList hr=0x" + hrText(hr));
    }
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format = format;
    srcLoc.PlacedFootprint.Footprint.Width = width;
    srcLoc.PlacedFootprint.Footprint.Height = height;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;
    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = out.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;
    list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = out.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = kSmaaLutReadyState;
    list->ResourceBarrier(1, &barrier);
    hr = list->Close();
    if (FAILED(hr)) {
        return mcdr::err("smaaCreateLutTexture", "Close hr=0x" + hrText(hr));
    }
    ID3D12CommandList* lists[] = {list.Get()};
    gCtx.queue->ExecuteCommandLists(1, lists);
    // 等 GPU 真正写完再返回：upload 与 list 都是局部对象，离开作用域即释放。
    if (auto waited = deviceWaitIdle(); !waited) {
        return mcdr::err("smaaCreateLutTexture", "deviceWaitIdle: " + errorMessage(waited.error()));
    }
    return {};
}

// 懒初始化两张 LUT + 它们的持久 SRV。成功后 gCtx.smaaLutsReady=true，
// 权力趟用官方算法的 kSmaaWeightLutPS；失败则保持 false（调用方回退解析式权重并 WARN）。
mcdr::Result<void> smaaEnsureLuts() {
    if (gCtx.smaaLutsReady) return {};
    if (!gCtx.device || !gCtx.queue) {
        return mcdr::err("smaaEnsureLuts", "device not initialized");
    }

    using namespace d3d12_mc::smaa_luts;
    ComPtr<ID3D12Resource> areaTex;
    ComPtr<ID3D12Resource> searchTex;
    if (auto r = smaaCreateLutTexture(kAreaTex, kAreaTexWidth, kAreaTexHeight, kAreaTexBytesPerPixel * kAreaTexWidth,
                                      DXGI_FORMAT_R8G8_UNORM, areaTex);
        !r) {
        return mcdr::err("smaaEnsureLuts", "AreaTex: " + errorMessage(r.error()));
    }
    if (auto r = smaaCreateLutTexture(kSearchTex, kSearchTexWidth, kSearchTexHeight,
                                      kSearchTexBytesPerPixel * kSearchTexWidth, DXGI_FORMAT_R8_UNORM, searchTex);
        !r) {
        return mcdr::err("smaaEnsureLuts", "SearchTex: " + errorMessage(r.error()));
    }
    if (!gCtx.smaaLutSrvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;  // 0 = AreaTex，1 = SearchTex
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hr = gCtx.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gCtx.smaaLutSrvHeap));
        if (FAILED(hr)) {
            return mcdr::err("smaaEnsureLuts", "CreateDescriptorHeap(SRV 2) hr=0x" + hrText(hr));
        }
        gCtx.smaaLutSrvGpu = gCtx.smaaLutSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gCtx.smaaLutSrvGpuInc = gCtx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = gCtx.smaaLutSrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuArea = cpuBase;
    gCtx.device->CreateShaderResourceView(areaTex.Get(), &srvDesc, cpuArea);
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSearch = cpuBase;
    cpuSearch.ptr += gCtx.smaaLutSrvGpuInc;
    gCtx.device->CreateShaderResourceView(searchTex.Get(), &srvDesc, cpuSearch);

    gCtx.smaaAreaTex = areaTex;
    gCtx.smaaSearchTex = searchTex;
    gCtx.smaaLutsReady = true;
    return {};
}

// P104b：就地路径的第三趟输出（与**源**同尺寸同格式），尺寸或格式变化时重建。
// 每帧用完都回 COMMON，所以本资源的稳定态就是 COMMON（与 transitionTo 的初始锚点一致）。
mcdr::Result<void> smaaEnsureWorldResult(const D3D12_RESOURCE_DESC& srcDesc) {
    if (!gCtx.smaaRtvHeap) {
        return mcdr::err("smaaEnsureWorldResult", "RTV heap not created (smaaEnsureTargets must run first)");
    }
    if (gCtx.smaaWorldResult && gCtx.smaaWorldResultFormat == srcDesc.Format &&
        gCtx.smaaWidth == srcDesc.Width && gCtx.smaaHeight == srcDesc.Height) {
        return {};
    }

    D3D12_RESOURCE_DESC desc = srcDesc;  // 宽高 / mip / array / 格式全部对齐，CopyResource 才合法
    desc.Alignment = 0;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;  // 只要 RTV；COPY_SOURCE 无需 flag
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    gCtx.smaaWorldResult.Reset();
    const HRESULT hr = gCtx.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&gCtx.smaaWorldResult));
    if (FAILED(hr)) {
        return mcdr::err("smaaEnsureWorldResult", "CreateCommittedResource hr=0x" + hrText(hr));
    }
    gCtx.device->CreateRenderTargetView(gCtx.smaaWorldResult.Get(), nullptr, gCtx.smaaWorldResultRtv);
    gCtx.smaaWorldResultFormat = srcDesc.Format;
    dbgLogInfo("P104: SMAA in-place result target %llux%llu (fmt=%d) ready",
               static_cast<unsigned long long>(srcDesc.Width),
               static_cast<unsigned long long>(srcDesc.Height), static_cast<int>(srcDesc.Format));
    return {};
}

// 编译一个 SMAA / TAA 像素着色器：硬要求 SM 6.9 + DXC，没有 5.1(DXBC) 兜底（2026-10-25）。
mcdr::Result<void> smaaCompilePs(const char* src, const char* debugName, const std::string& smProfile,
                                 std::vector<uint8_t>& dxilOut, const void*& code, SIZE_T& size) {
    code = nullptr;
    size = 0;
    if (smProfile.empty()) {
        return mcdr::err("smaaCompilePs", std::string(debugName) + ": " + sm69RequirementText());
    }
    const std::string text(src);
    auto r = compileHlslToDxil(text, "main", "ps_" + smProfile, dxilOut);
    if (!r) {
        return mcdr::err("smaaCompilePs", std::string(debugName) + " (DXC): " + firstLine(r.error().message));
    }
    code = dxilOut.data();
    size = dxilOut.size();
    return {};
}

// 用 SMAA 的根签名 + 给定 PS 建 PSO（输入布局/混合/光栅状态与 blit 完全一致）。
mcdr::Result<ComPtr<ID3D12PipelineState>> smaaCreatePso(const ComPtr<ID3D12RootSignature>& rootSig,
                                                       const void* vsCode, SIZE_T vsCodeSize,
                                                       const void* psCode, SIZE_T psCodeSize,
                                                       DXGI_FORMAT rtFormat) {
    D3D12_INPUT_ELEMENT_DESC ie[2] = {};
    ie[0].SemanticName = "POSITION";
    ie[0].SemanticIndex = 0;
    ie[0].Format = DXGI_FORMAT_R32G32_FLOAT;
    ie[0].InputSlot = 0;
    ie[0].AlignedByteOffset = 0;
    ie[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    ie[0].InstanceDataStepRate = 0;
    ie[1].SemanticName = "TEXCOORD";
    ie[1].SemanticIndex = 0;
    ie[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    ie[1].InputSlot = 0;
    ie[1].AlignedByteOffset = 8;
    ie[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    ie[1].InstanceDataStepRate = 0;

    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.BlendEnable = FALSE;
    rtBlend.LogicOpEnable = FALSE;
    rtBlend.SrcBlend = D3D12_BLEND_ONE;
    rtBlend.DestBlend = D3D12_BLEND_ZERO;
    rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_BLEND_DESC blendDesc{};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0] = rtBlend;

    D3D12_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterDesc.DepthClipEnable = TRUE;
    rasterDesc.MultisampleEnable = FALSE;
    rasterDesc.AntialiasedLineEnable = FALSE;

    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable = FALSE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    dsDesc.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = {vsCode, vsCodeSize};
    psoDesc.PS = {psCode, psCodeSize};
    psoDesc.InputLayout = {ie, 2};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = rtFormat;
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.BlendState = blendDesc;
    psoDesc.RasterizerState = rasterDesc;
    psoDesc.DepthStencilState = dsDesc;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAGS(0);

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = gCtx.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        // P104 fix：E_INVALIDARG 这类失败光看 HRESULT 无法定位。调试层开着时 InfoQueue 里会有
        // 一条明确指出「着色器用到的某个寄存器没被根签名覆盖」之类的消息，dump 出来便于下一次定位。
        dumpInfoQueueMessages();
        return mcdr::err("smaaCreatePso", "CreateGraphicsPipelineState hr=0x" + hrText(hr));
    }
    return pso;
}

// 懒初始化 SMAA 的三条管线（rootSig 一张，PSO 三条）。
mcdr::Result<void> smaaEnsurePipelines(DXGI_FORMAT rtFormat) {
    if (gCtx.smaaPipelinesReady && gCtx.smaaBlendFormat == rtFormat) {
        return {};
    }

    // 三张 SRV 表：t0 / t1 / t2，各占一个根参数（0 / 1 / 2）。
    //
    // P104 fix（E_INVALIDARG）：**必须是 3 个**，这是本次运行期回归的根因。
    // 权重趟的 kSmaaWeightLutPS 声明了三张 SRV（dxc 反射：
    // `edgesTex t0 / areaTex t1 / searchTex t2`，另加 `smaaSampler s0 / smaaLutSampler s1`），
    // 而原实现只序列化了 t0/t1 两个描述符表。D3D12 要求 PSO 里着色器用到的**每一个**
    // 寄存器（t/s/b/u）都被根签名覆盖，缺 t2 ⇒ 该 PSO 的着色器/根签名组合非法 ⇒
    // CreateGraphicsPipelineState 返回 0x80070057（E_INVALIDARG）。
    // 表现就是 smaaEnsurePipelines 里**第一条** PSO（edges，用的是同一张根签名）创建失败、
    // `P104: SMAA pipelines ready …` 永远不打印、SMAA 被一次性关掉 —— 与实机日志一致。
    //
    // 槽位对应表（= 反射实测值）：
    //   ① 边检测 kSmaaEdgePS      : t0=srcTex              s0=smaaSampler
    //   ② 解析权重 kSmaaWeightPS  : t0=edgesTex            s0=smaaSampler
    //   ②′ LUT 权重 kSmaaWeightLutPS: t0=edgesTex t1=areaTex t2=searchTex  s0=smaaSampler s1=smaaLutSampler
    //   ③ 邻域混合 kSmaaBlendPS   : t0=colorTex t1=weightsTex s0=smaaSampler
    // 只有②′用到 t2；① ② ③ 不用的表保持未绑定（与本次改动前 t1 在 ① ② 未绑定的既有行为一致，
    // 未绑定的根参数在 D3D12 里合法，只是不能是「着色器用了但根签名没有」）。
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    for (UINT i = 0; i < 3; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;  // t0 = 源色 / t1 = 辅助纹理 / t2 = SearchTex
        ranges[i].RegisterSpace = 0;
        ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }
    D3D12_ROOT_PARAMETER params[3] = {};
    for (UINT i = 0; i < 3; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_STATIC_SAMPLER_DESC staticSam[2] = {};
    staticSam[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSam[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSam[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSam[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSam[0].MipLODBias = 0.0f;
    staticSam[0].MaxAnisotropy = 1;
    staticSam[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSam[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticSam[0].MinLOD = 0.0f;
    staticSam[0].MaxLOD = D3D12_FLOAT32_MAX;
    staticSam[0].ShaderRegister = 0;
    staticSam[0].RegisterSpace = 0;
    staticSam[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // P104 LUT 采样器（s1）：上游 SMAA.hlsl 的 LinearSampler 就是
    // MIN_MAG_LINEAR_MIP_POINT + Clamp —— AreaTex 按 texel 中心采样、SearchTex 查表，
    // 只有 mip 级别的插值被关掉。改成纯 LINEAR 会破坏 SearchTex 的查表结果。
    staticSam[1].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    staticSam[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSam[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSam[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSam[1].MipLODBias = 0.0f;
    staticSam[1].MaxAnisotropy = 1;
    staticSam[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSam[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticSam[1].MinLOD = 0.0f;
    staticSam[1].MaxLOD = D3D12_FLOAT32_MAX;
    staticSam[1].ShaderRegister = 1;
    staticSam[1].RegisterSpace = 0;
    staticSam[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = staticSam;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        return mcdr::err("smaaEnsurePipelines", "SerializeRootSignature hr=0x" + hrText(hr));
    }
    ComPtr<ID3D12RootSignature> rootSig;
    hr = gCtx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        return mcdr::err("smaaEnsurePipelines", "CreateRootSignature hr=0x" + hrText(hr));
    }

    // VS 与 blit 同一份 HLSL（全屏四边形 + 透传 texcoord）。硬要求 SM 6.9 + DXC，无 5.1 兜底。
    const std::string smProfile = graphicsShaderModelProfile();
    if (smProfile.empty()) {
        return mcdr::err("smaaEnsurePipelines", "smaaEnsurePipelines: " + sm69RequirementText());
    }
    std::vector<uint8_t> vsDxil;
    const std::string vsSrc(kBlitVS, sizeof(kBlitVS) - 1);
    auto vsDxc = compileHlslToDxil(vsSrc, "main", "vs_" + smProfile, vsDxil);
    if (!vsDxc) {
        return mcdr::err("smaaEnsurePipelines", "smaa_vs (DXC): " + firstLine(vsDxc.error().message));
    }
    const void* vsCode = vsDxil.data();
    SIZE_T vsCodeSize = vsDxil.size();

    // 三个 PS：DXIL 缓冲必须活到 PSO 建完（不再有 ID3DBlob 那条路）。
    std::vector<uint8_t> edgeDxil, weightDxil, blendDxil, weightLutDxil;
    const void* edgeCode = nullptr;
    const void* weightCode = nullptr;
    const void* blendCode = nullptr;
    const void* weightLutCode = nullptr;
    SIZE_T edgeSize = 0, weightSize = 0, blendSize = 0, weightLutSize = 0;
    if (auto r = smaaCompilePs(kSmaaEdgePS, "smaa_edge_ps", smProfile, edgeDxil, edgeCode, edgeSize); !r) {
        return mcdr::err("smaaEnsurePipelines", errorMessage(r.error()));
    }
    if (auto r = smaaCompilePs(kSmaaWeightPS, "smaa_weight_ps", smProfile, weightDxil, weightCode, weightSize);
        !r) {
        return mcdr::err("smaaEnsurePipelines", errorMessage(r.error()));
    }
    // P104：官方 LUT 版权重趟。这是本条路径的主算法；下面的解析版只是 LUT 初始化失败时的回退。
    if (auto r = smaaCompilePs(kSmaaWeightLutPS, "smaa_weight_lut_ps", smProfile, weightLutDxil, weightLutCode,
                               weightLutSize);
        !r) {
        return mcdr::err("smaaEnsurePipelines", errorMessage(r.error()));
    }
    if (auto r = smaaCompilePs(kSmaaBlendPS, "smaa_blend_ps", smProfile, blendDxil, blendCode, blendSize); !r) {
        return mcdr::err("smaaEnsurePipelines", errorMessage(r.error()));
    }

    auto edgePso = smaaCreatePso(rootSig, vsCode, vsCodeSize, edgeCode, edgeSize, DXGI_FORMAT_R8G8_UNORM);
    if (!edgePso) return mcdr::err("smaaEnsurePipelines", errorMessage(edgePso.error()));
    auto weightPso = smaaCreatePso(rootSig, vsCode, vsCodeSize, weightCode, weightSize, DXGI_FORMAT_R8G8_UNORM);
    if (!weightPso) return mcdr::err("smaaEnsurePipelines", errorMessage(weightPso.error()));
    auto weightLutPso =
        smaaCreatePso(rootSig, vsCode, vsCodeSize, weightLutCode, weightLutSize, DXGI_FORMAT_R8G8_UNORM);
    if (!weightLutPso) return mcdr::err("smaaEnsurePipelines", errorMessage(weightLutPso.error()));
    auto blendPso = smaaCreatePso(rootSig, vsCode, vsCodeSize, blendCode, blendSize, rtFormat);
    if (!blendPso) return mcdr::err("smaaEnsurePipelines", errorMessage(blendPso.error()));

    gCtx.smaaRootSig = rootSig;
    gCtx.smaaEdgePso = edgePso.value();
    gCtx.smaaWeightPso = weightPso.value();
    gCtx.smaaWeightLutPso = weightLutPso.value();
    gCtx.smaaBlendPso = blendPso.value();
    gCtx.smaaBlendFormat = rtFormat;
    gCtx.smaaPipelinesReady = true;
    dbgLogInfo("P104: SMAA pipelines ready (blend RTV fmt=%d, SM=%s)", static_cast<int>(rtFormat),
               smProfile.c_str());
    // 两条权重管线只有一条会被用到，但两条都建好 ⇒ 任何时候都不需要重建、不需要重试。
    // 「官方 LUT」那行日志**不能**放在这里：smaaEnsurePipelines 先于 smaaEnsureLuts 执行，
    // 此刻 smaaLutsReady 一定是 false。它现在打在 applySmaaToTexture 选路之后。
    return {};
}

// 把一张纹理的 SRV 写进本帧 drawHeap 的瞬态槽位并绑到根参数 rootParam 上
// （与 blitBindSourceTexture 同一机制：SRV 必须落在命令列表当前设置的堆里）。
mcdr::Result<void> smaaBindSrv(CommandContext* ctx, ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                               DXGI_FORMAT viewFmt, UINT rootParam) {
    if (!res) {
        return mcdr::err("smaaBindSrv", "smaaBindSrv: null resource");
    }
    if (ctx->nextDrawSlot + 1 > syncRingCapacity()) {
        return mcdr::err("smaaBindSrv", "smaaBindSrv: draw descriptor heap exhausted for this frame");
    }
    SIZE_T base = static_cast<SIZE_T>(ctx->drawHeapSlotBase + ctx->nextDrawSlot) * gCtx.drawInc;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = gCtx.drawHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += base;
    ctx->nextDrawSlot += 1;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = viewFmt;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    gCtx.device->CreateShaderResourceView(res, &srvDesc, srvCpu);

    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = gCtx.drawHeap->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += base;
    cmd->SetGraphicsRootDescriptorTable(rootParam, srvGpu);
    return {};
}

// P104：把两张官方 LUT 的 **持久** SRV 绑到权重趟的根参数 1（t1 = AreaTex）与 2（t2 = SearchTex）。
// 与 smaaBindSrv 的区别：这两张 SRV 早在 smaaEnsureLuts 里就写进 gCtx.smaaLutSrvHeap 了，
// 每帧只需要把「堆里已有的表」挂到根参数上 —— 不占每帧的 drawHeap 槽位，也不重新建视图。
mcdr::Result<void> smaaBindLuts(ID3D12GraphicsCommandList* cmd) {
    if (!gCtx.smaaLutSrvHeap || !gCtx.smaaLutSrvGpu.ptr || gCtx.smaaLutSrvGpuInc == 0) {
        return mcdr::err("smaaBindLuts", "LUT SRV heap is not ready");
    }
    D3D12_GPU_DESCRIPTOR_HANDLE areaGpu = gCtx.smaaLutSrvGpu;
    D3D12_GPU_DESCRIPTOR_HANDLE searchGpu = gCtx.smaaLutSrvGpu;
    searchGpu.ptr += gCtx.smaaLutSrvGpuInc;
    cmd->SetGraphicsRootDescriptorTable(1, areaGpu);
    cmd->SetGraphicsRootDescriptorTable(2, searchGpu);
    return {};
}

}  // namespace

const char* antiAliasingName(int mode) {
    switch (mode) {
        case 0: return "OFF";
        case 1: return "SMAA";
        case 2: return "TAA";
        case 3: return "DLAA";
        default: return "UNKNOWN";
    }
}

int setAntiAliasing(int mode) {
    if (mode < 0 || mode > 3) {
        dbgLogError("P104: invalid anti-aliasing mode %d (valid: 0=OFF 1=SMAA 2=TAA 3=DLAA)", mode);
        return -1;
    }
    // P77/P152：TAA（2）不再降级，且从 P152 起它的 resolve 也在 native 侧
    // （applyTaaToTexture：历史纹理乒乓 + jitter + 管线都由本模块持有），JVM 只送参数。
    // `applySmaaToTexture` 的闸门是 `aaMode != 1`，所以 TAA 模式下 SMAA 自动让路。
    // DLAA（3）仍然由 DLSS 特性实现：JVM 侧会把它映射成 OFF 再传下来，真收到 3 说明另有
    // 调用方，同样按"无 native 后处理"处理，不该再用"未实现"把用户的选项踢回 OFF。
    const int prev = gCtx.aaMode;
    gCtx.aaMode = mode;
    // 换模式时把两条路径的"一次性失败"闸门都复位：用户可以靠改设置再试一次。
    if (mode != prev) {
        gCtx.smaaDisabled = false;
        gCtx.smaaUnavailableLogged = false;
        gCtx.smaaAppliedFrames = 0;
        gCtx.taaDisabled = false;
        gCtx.taaUnavailableLogged = false;
        gCtx.taaSharpenFallbackWarned = false;
        gCtx.taaAppliedFrames = 0;
        // 历史内容属于上一种模式下的画面，切回来时不可信（下一帧以 reset=1 开始）。
        gCtx.taaHistoryValid = false;
    }
    if (mode != prev) {
        dbgLogInfo("P77: anti-aliasing = %s (%s)", antiAliasingName(mode),
                   mode == 1 ? "SMAA, three passes in place after the world and before the GUI"
                   : mode == 2 ? "TAA, resolved natively after the world layer (RGBA16F history)"
                               : "no post-processing; plain blit");
    }
    // 契约（JVM 的 `D3D12AntiAliasing.push` 用 `status == pushed` 判成功）：成功时返回**生效的
    // 模式本身**，0 是合法成功值（OFF）。旧实现恒返回 1，于是 OFF 被误报成"返回值异常"。
    return mode;
}

// P108：DLSS 的**独立开关**（用户 m10920：DLSS 不再挂在抗锯齿上）。
//
// 为什么从 `aaMode == 4` 拆出来：DLSS 是"升采样"，抗锯齿是"边缘/时间性平滑"，两件事。
// 挂在同一个枚举上会逼用户二选一，也会让 `dxrMcAaMode` 同时承担两种语义。现在：
//   * `dxrMcAaMode` 只管 0=OFF / 1=SMAA（2/3 仍未实现），语义回到 P104；
//   * `dxrMcDlss` 独立控制是否做 DLSS 升采样；
//   * 两者**互相独立**，都开时先对低分辨率世界层做 SMAA，再由 DLSS 升采样。
// 与 SMAA 的"一次性失败"闸门同理：开关翻动时把 DLSS 的一次性失败闸门复位，让用户可以靠
// 关掉再打开重试（NGX 状态本身仍在实现单元里保持，不去重建已加载的运行库）。
int setDlssEnabled(int enabled) {
    const bool want = enabled != 0;
    if (want == gCtx.dlssEnabled) {
        return want ? 1 : 0;
    }
    gCtx.dlssEnabled = want;
    dbgLogInfo("P108: DLSS %s (independent of anti-aliasing; aaMode=%d)",
               want ? "enabled" : "disabled", gCtx.aaMode);
    if (want) {
        // 让下一次 applyDlssToTexture 有机会重来一次（例如上次是 NGX 还没就绪时失败的）。
        dbgLogInfo("P108: DLSS gate is now open; the next world frame will try to create/evaluate");
    }
    return want ? 1 : 0;
}

// ---------------------------------------------------------------------------
// P108: DLSS（超分辨率）—— 数据面接线 + 真实 NVSDK_NGX 评价调用
//
// 这一段的职责是**翻译**：把设备模块内的句柄（Dx12Object* / CommandContext* /
// ID3D12Device*）翻成 d3d12_dlss.cpp 的 C 接口，并把结果翻译回日志与返回值。所有 NGX
// 调用与 DLSS 状态都在那个普通 TU 里，本模块始终看不到一个 NGX 类型。
//
// 与 P104b SMAA 的关系：两者是同一个抗锯齿选项的两个取值（aaMode 1 / 4），互斥。
// 模式 4 时 applySmaaToTexture 直接返回 false，一个趟都不跑。
//
// 接线用的两个辅助函数（wireDlssImpl / notifyDlssDeviceReady）定义在本文件靠前的位置
// —— ensureDevice() 在 1520 行附近就要调用后者，定义放这里会"找不到标识符"（C3861）。
// ---------------------------------------------------------------------------

// B4：实现单元请求的私有纹理创建（参数见 mcdr_dlss_impl.h 的 DlssTextureDesc）。
//
// 与"实现单元自己 CreateCommittedResource"的差别不是风格：这里建出来的资源从出生起就登记进
// `ctx->resourceState`（初始 COMMON），所以本列表内对它做的第一次过渡有真实锚点，`stateOf`
// 也能答出状态；销毁走 `destroyObject` 的延迟删除路径，不会在有打开的命令列表时裸 Release
// （那会报 "deleted prior to closing the command list"）。实现单元只拿到两个不透明值：句柄
// （只能交回 destroyDlssTexture）与裸 `ID3D12Resource*`。
// 注意：这两个名字在 wireDlssImpl（本文件靠前，匿名 namespace 里）已经声明过，而 TU 里所有
// 匿名 namespace 是同一个，所以定义必须放在匿名 namespace 内 —— 否则就是**两个不同链接性**
// 的同名函数（MSVC 会报 C5046，链接时也取不到定义）。
namespace {
int32_t createDlssTexture(CommandContext* ctx, const DlssTextureDesc* desc, void** outObject,
                          void** outResource) {
    if (outObject != nullptr) *outObject = nullptr;
    if (outResource != nullptr) *outResource = nullptr;
    if (desc == nullptr || outObject == nullptr || outResource == nullptr) {
        return 0;
    }
    if (gCtx.device == nullptr || desc->dxgiFormat == 0 /*DXGI_FORMAT_UNKNOWN*/ || desc->width == 0 ||
        desc->height == 0) {
        return 0;  // 参数/设备不可用：不算失败，实现单元会退回"没有私有纹理"
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rdesc{};
    rdesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rdesc.Width = desc->width;
    rdesc.Height = desc->height;
    rdesc.DepthOrArraySize = static_cast<UINT16>((desc->arraySize > 1) ? desc->arraySize : 1);
    rdesc.MipLevels = static_cast<UINT16>((desc->mipLevels > 1) ? desc->mipLevels : 1);
    rdesc.Format = static_cast<DXGI_FORMAT>(desc->dxgiFormat);
    rdesc.SampleDesc.Count = (desc->sampleCount > 0) ? desc->sampleCount : 1;
    rdesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rdesc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(desc->flags);
    auto obj = std::make_unique<Dx12Object>();
    obj->kind = Dx12Object::Kind::Texture;
    obj->heapType = D3D12_HEAP_TYPE_DEFAULT;
    obj->dxgiFormat = rdesc.Format;
    obj->size = static_cast<long long>(desc->width) * desc->height;
    const HRESULT hr = gCtx.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &rdesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&obj->resource));
    if (FAILED(hr)) {
        dbgLogWarn("B4: DLSS private texture creation failed (%ux%u fmt=%d flags=0x%X): %s",
                   desc->width, desc->height, static_cast<int>(rdesc.Format),
                   static_cast<unsigned>(desc->flags), hrText(hr).c_str());
        return -1;
    }
    Dx12Object* raw = obj.release();
    // 创建即登记：本列表内第一次过渡就有真实锚点（COMMON），不再"按 COMMON 兜底猜"。
    if (ctx != nullptr) {
        ctx->resourceState[raw->resource.Get()] = D3D12_RESOURCE_STATE_COMMON;
    }
    *outObject = static_cast<void*>(raw);
    *outResource = static_cast<void*>(raw->resource.Get());
    dbgLogInfo("B4: module created a private DLSS texture: %ux%u fmt=%d mips=%u array=%u samples=%u "
               "flags=0x%X handle=%p resource=%p; registered in the tracked state map (COMMON, ctx=%p)",
               desc->width, desc->height, static_cast<int>(rdesc.Format),
               static_cast<unsigned>(rdesc.MipLevels), static_cast<unsigned>(rdesc.DepthOrArraySize),
               static_cast<unsigned>(rdesc.SampleDesc.Count), static_cast<unsigned>(desc->flags),
               static_cast<void*>(raw), static_cast<void*>(raw->resource.Get()),
               static_cast<void*>(ctx));
    return 1;
}

void destroyDlssTexture(CommandContext* ctx, void* object) {
    auto* obj = static_cast<Dx12Object*>(object);
    if (obj == nullptr) {
        return;
    }
    // 先把键从当前 ctx 的跟踪表里摘掉（键是它自己的裸资源指针），再走延迟删除。
    if (ctx != nullptr && obj->resource) {
        ctx->resourceState.erase(obj->resource.Get());
    }
    dbgLogInfo("B4: module destroys a private DLSS texture: handle=%p resource=%p", object,
               obj->resource ? static_cast<void*>(obj->resource.Get()) : nullptr);
    destroyObject(obj);
}
}  // namespace

bool applyDlssToTexture(CommandContext* ctx, const mcdr_dlss_input& in) {
    // P108（用户 m10920）：闸门是**独立的 DLSS 开关**，不再看 aaMode。DLSS 与抗锯齿互不
    // 相干，两者都开时先 SMAA 再 DLSS 升采样（SMAA 的三趟在本函数之前跑，作用在同一张
    // 低分辨率世界层上）。
    if (!gCtx.dlssEnabled) {
        return false;  // 开关关着：一个动作都没有
    }
    if (!ctx || !ctx->listOpen || ctx->commandList == nullptr) {
        return false;
    }
    if (in.pInColor == nullptr || in.pInDepth == nullptr || in.pInMotionVectors == nullptr ||
        in.pInOutput == nullptr) {
        return false;
    }
    wireDlssImpl();
    // P108 根因修复（2026-10-16，hs_err_pid52140 / pid50060）：四个 `pIn*` 是 JVM 侧的
    // **`Dx12Object*` 包装句柄**（`D3D12GpuTexture.kt` 的 KDoc：「returns a handle
    // ({@code Dx12Object*})」，由 `MCDR_ABI.ID3D12CreateTexture` 返回），**不是
    // `ID3D12Resource*`**。DLSS 实现单元是普通 TU、看不到 `Dx12Object` 的布局，所以它
    // 只能把句柄原样交给 NGX —— NGX 拿到的是包装体地址，`lpVtbl` 读出来是垃圾/0，于是首次
    // `mv->GetDesc()` 就在 `EXCEPTION_ACCESS_VIOLATION reading 0x50`、`call 0` 上崩掉
    // （寄存器 RAX=0）。**解包必须在本模块里做**：这里知道 Dx12Object 的真实布局
    // （`ComPtr<ID3D12Resource> resource`），取出裸 `ID3D12Resource*` 后再往下传。
    auto unwrapResource = [](mcdr_object* handle) -> void* {
        auto* obj = reinterpret_cast<Dx12Object*>(handle);
        return obj != nullptr ? reinterpret_cast<void*>(obj->resource.Get()) : nullptr;
    };
    mcdr_dlss_input resolved = in;
    resolved.pInColor = reinterpret_cast<mcdr_object*>(unwrapResource(in.pInColor));
    resolved.pInDepth = reinterpret_cast<mcdr_object*>(unwrapResource(in.pInDepth));
    resolved.pInMotionVectors = reinterpret_cast<mcdr_object*>(unwrapResource(in.pInMotionVectors));
    resolved.pInOutput = reinterpret_cast<mcdr_object*>(unwrapResource(in.pInOutput));
    dbgLogInfo("P108: DLSS resource unwrap: color=%p depth=%p mv=%p out=%p (from Dx12Object "
               "handles %p/%p/%p/%p)",
               resolved.pInColor, resolved.pInDepth, resolved.pInMotionVectors, resolved.pInOutput,
               static_cast<void*>(in.pInColor), static_cast<void*>(in.pInDepth),
               static_cast<void*>(in.pInMotionVectors), static_cast<void*>(in.pInOutput));
    // 运动矢量缺失时 DLSS 无法工作：这里显式降级（打一次 INFO），而不是把一个空资源交给 NGX。
    if (resolved.pInColor == nullptr || resolved.pInDepth == nullptr || resolved.pInOutput == nullptr) {
        static bool s_loggedMissing = false;
        if (!s_loggedMissing) {
            s_loggedMissing = true;
            dbgLogWarn("P108: DLSS skipped — color/depth/output did not unwrap to a D3D12 resource "
                       "(color=%p depth=%p out=%p); the data plane handed over a handle that is not "
                       "a live texture",
                       resolved.pInColor, resolved.pInDepth, resolved.pInOutput);
        }
        return false;
    }
    if (resolved.pInMotionVectors == nullptr) {
        static bool s_loggedNoMv = false;
        if (!s_loggedNoMv) {
            s_loggedNoMv = true;
            dbgLogWarn("P108: DLSS skipped — no motion-vector texture this frame (the velocity pass "
                       "is off or did not record); DLSS needs per-pixel motion vectors, so it stays "
                       "off until one exists. Enable it with -Pdxrmc.temporal.velocity=true.");
        }
        return false;
    }
    // DLSS 的初始化与建特征都必须在**命令列表打开**的时候做（CreateFeature 会录制 NGX
    // 自己的命令），所以两者都走这里，而不是在 setAntiAliasing 里提前做。
    if (mcdr_dlss_can_run(reinterpret_cast<void*>(gCtx.device.Get())) != 1) {
        return false;  // 失败已经一次性 WARN 过；保持既有路径
    }
    // P108：资源状态过渡交给本模块（DLSS 实现单元看不到 ctx->resourceState）。它按
    // transitionTextureTo 的跟踪状态走 barrier，from==to 时自动跳过 —— 这修掉了旧代码手写
    // Before 与 P11（endRenderPass 把附件回切 COMMON）不符的问题。
    DlssTransitionFn tl = [](void* opaqueCtx, void* resource, int32_t toState) {
        auto* c = static_cast<CommandContext*>(opaqueCtx);
        if (c == nullptr || resource == nullptr) {
            return;
        }
        transitionTo(c, static_cast<ID3D12Resource*>(resource), D3D12_RESOURCE_STATE_COMMON,
                     static_cast<D3D12_RESOURCE_STATES>(toState));
    };
    // The tracked-state query is only needed by the depth snapshot path (snapshotDlssDepth);
    // the evaluate path never asks for a state, so no query callback is built here.
    // P108 (user m11147): keep the implementation's tier in sync with the device state on every
    // frame. `setDlssQuality` pushes changes immediately, but this is what makes the default (2 =
    // Quality, which is what the old ratio-based choice picked at 640x360->854x480) actually take
    // effect even when the JVM never calls the setter.
    mcdr_dlss_request_quality(gCtx.dlssQualityTier);
    // P108ac (user m08033): same re-assert for the DLSS model, and for the same reason — the
    // implementation unit's state can be reset behind the device module's back, and a silently
    // reverted preset would look exactly like "the button does nothing" in the user's report.
    mcdr_dlss_request_preset(gCtx.dlssPreset);
    // P108 (user m11147 follow-up): re-assert the JVM-supplied output texture EVERY frame, next to
    // the tier above and for the same reason — the hand-off is one-shot on the JVM side
    // (`handed … 1`, `dx12SetDlssOutput(0)` never called), while the implementation unit's state can
    // be reset behind its back. Diagnosed from a joint run where `override accepted` (01:24:44.258)
    // and the first evaluate (01:24:45.458) were on the SAME thread with no device recreate, yet
    // the evaluate chose the private fallback: `s.externalOutput` had gone null with no
    // `override cleared` line, and the only writer that can do that silently is
    // `mcdr_dlss_shutdown`'s blanket `s = State{}`. Re-asserting here makes the hand-off idempotent
    // and self-healing rather than depending on nothing in between touching that state.
    // `mcdr_dlss_set_output_texture` early-returns when the pointer is unchanged.
    {
        void* overrideRaw = nullptr;
        if (gCtx.dlssOutputOverride != nullptr) {
            auto* oObj = reinterpret_cast<Dx12Object*>(gCtx.dlssOutputOverride);
            if (oObj->kind == Dx12Object::Kind::Texture && oObj->resource) {
                overrideRaw = reinterpret_cast<void*>(oObj->resource.Get());
            }
        }
        mcdr_dlss_set_output_texture(overrideRaw);
    }
    const int32_t created = mcdr_dlss_create(reinterpret_cast<void*>(ctx->commandList), &resolved,
                                             tl, ctx);
    if (created != 1) {
        return false;
    }
    // NGX records a COMPUTE dispatch, and D3D12 forbids that while render targets are still
    // bound — the classic source of DXGI_ERROR_INVALID_CALL (0x887A0001). Ending a pass that is
    // still open returns every attachment to COMMON and makes the dispatch legal.
    if (ctx->inRenderPass) {
        // Whoever left a pass open (the JVM hook sits after the world pass) has to be named in the
        // log, otherwise this stays invisible: the pass state and the call site disagree.
        dbgLogWarn("P108x: a render pass was still open at the DLSS evaluate (inRenderPass=1); ending it "
                "so NGX's compute dispatch is legal. The caller is expected to begin its own pass "
                "afterwards — nothing is restarted here.");
        (void)endRenderPass(ctx);
    }
    // NOTE: an explicit `OMSetRenderTargets(0, nullptr, ...)` is NOT used here — this TU's command
    // list handle is not a plain `ID3D12GraphicsCommandList*` for that call. Ending the tracked pass
    // (above) already returns every attachment to COMMON, which is what makes the compute dispatch
    // legal; if the device still reports INVALID_CALL we will look at descriptor heaps / list state
    // next, with the debug layer on.
    const int32_t evaluated = mcdr_dlss_evaluate(reinterpret_cast<void*>(ctx->commandList), &resolved, tl, ctx);
    if (evaluated != 1) {
        return false;
    }
    return true;
}

// P108（用户 m11147 后续）：登记 JVM 送来的 UAV 输出纹理（(B) 方案）。
//
// 本函数做**解包**：`Dx12Object` 的布局只有本模块知道，而 DLSS 实现单元是普通 TU、看不到它
// （历史教训：那边曾把包装体地址当 ID3D12Resource* 用，`[wrapper+0]` 读到的其实是 Kind 那个
// int，于是 lpVtbl=0、首次 `mv->GetDesc()` 就 `call 0` 崩溃）。所以这里取出裸资源再往下传。
//
// 只登记句柄、不持有引用；资源生命周期由 JVM 侧那个 Dx12Object 保证。
// 返回 1=已接受 0=已清除 -1=句柄不是纹理资源（调用方应视为无效、保持原状）。
int setDlssOutput(mcdr_object* tex) {
    if (tex == nullptr) {
        gCtx.dlssOutputOverride = nullptr;
        mcdr_dlss_set_output_texture(nullptr);
        dbgLogInfo("P108: DLSS output override cleared (back to the private fallback)");
        return 0;
    }
    auto* obj = reinterpret_cast<Dx12Object*>(tex);
    if (obj->kind != Dx12Object::Kind::Texture || !obj->resource) {
        dbgLogError("P108: mcdr_dx12SetDlssOutput got a handle that is not a live texture "
                    "(kind=%d); ignored",
                    static_cast<int>(obj->kind));
        return -1;
    }
    gCtx.dlssOutputOverride = tex;
    const int32_t r = mcdr_dlss_set_output_texture(reinterpret_cast<void*>(obj->resource.Get()));
    dbgLogInfo("P108: DLSS output override accepted (raw resource %p); the evaluate will write "
               "there instead of the private UAV texture",
               static_cast<void*>(obj->resource.Get()));
    return r == 1 ? 1 : 0;
}

// P108（用户 m11147）：DLSS 挡位显式指定（Java 侧已把"性能/平衡/质量"做成独立按钮）。
//
// 语义：0=Performance 1=Balanced 2=Quality（与 JVM 枚举同序）。档位写进 gCtx 之后，实现单元
// 在下一次 ensureFeature 看到变化就**重建特征**（尺寸没变也重建）——`PerfQualityValue` 是
// CreateFeature 的输入，不能只在 Evaluate 参数上改。
// 越界返回 -1 且不改变现状；返回归一化后的档位。
int setDlssQuality(int tier) {
    if (tier < 0 || tier > 4) {
        dbgLogError("P108: invalid DLSS quality tier %d (valid: 0=Performance 1=Balanced "
                    "2=Quality 3=DLAA)",
                    tier);
        return -1;
    }
    const int prev = gCtx.dlssQualityTier;
    gCtx.dlssQualityTier = tier;
    if (tier != prev) {
        // 让实现单元下一帧重建，并把"一次性失败"闸门复位（换档也能把之前失败的 DLSS 再试）。
        mcdr_dlss_request_quality(tier);
        dbgLogInfo("P108: DLSS quality tier = %d (%s) — the feature will be recreated on the next "
                   "frame",
                   tier, tier == 0 ? "Performance"
                                   : (tier == 1 ? "Balanced" : (tier == 2 ? "Quality" : "DLAA")));
    }
    return tier;
}

// P108ac（用户 m08033）：DLSS 的**模型**（NGX 的 render preset 提示）。用户反馈"剩下的噪点闪烁
// 是 DLSS 自身的问题，所有用 DLSS 的游戏都会出现，优化方法是更换 DLSS 的模型"。
//
// 语义与 setDlssQuality 完全平行：0=Default（交给 NGX 按挡位自选，也是历史行为），
// 5=E / 6=F（已弃用的 CNN 预设），10=J / 11=K / 12=L / 13=M（transformer）。预设是
// CreateFeature 的输入，所以值一变化就让实现单元在下一帧重建特征。
// 越界返回 -1 且不改变现状；返回实际记录的预设值。
int setDlssRenderPreset(int preset) {
    if (preset < 0 || preset > 15) {
        dbgLogError("P108: invalid DLSS render preset %d (valid: 0=Default 5=E 6=F 10=J 11=K "
                    "12=L 13=M)",
                    preset);
        return -1;
    }
    const int prev = gCtx.dlssPreset;
    gCtx.dlssPreset = preset;
    if (preset != prev) {
        mcdr_dlss_request_preset(preset);
        dbgLogInfo("P108: DLSS render preset = %d — the feature will be recreated on the next "
                   "frame",
                   preset);
    }
    return preset;
}

// P108：深度快照（详见 mcdr_dlss_impl.h）。JVM 在**清深度之前**那一处调用它；本函数只做
// 解包 + 把本模块的跟踪型过渡回调交给实现单元，快照资源与状态恢复都在实现单元里。
int snapshotDlssDepth(CommandContext* ctx, mcdr_object* depthHandle) {
    if (ctx == nullptr || ctx->commandList == nullptr || depthHandle == nullptr) {
        return 0;
    }
    auto* obj = reinterpret_cast<Dx12Object*>(depthHandle);
    ID3D12Resource* res = (obj != nullptr) ? obj->resource.Get() : nullptr;
    if (res == nullptr) {
        return 0;
    }
    DlssTransitionFn tl = [](void* opaqueCtx, void* resource, int32_t toState) {
        auto* c = static_cast<CommandContext*>(opaqueCtx);
        if (c == nullptr || resource == nullptr) {
            return;
        }
        transitionTo(c, static_cast<ID3D12Resource*>(resource), D3D12_RESOURCE_STATE_COMMON,
                     static_cast<D3D12_RESOURCE_STATES>(toState));
    };
    DlssQueryStateFn queryState = [](void* opaqueCtx, void* resource) -> int32_t {
        auto* c = static_cast<CommandContext*>(opaqueCtx);
        if (c == nullptr || resource == nullptr) {
            return static_cast<int32_t>(D3D12_RESOURCE_STATE_COMMON);
        }
        auto it = c->resourceState.find(static_cast<ID3D12Resource*>(resource));
        return static_cast<int32_t>(it != c->resourceState.end() ? it->second
                                                                 : D3D12_RESOURCE_STATE_COMMON);
    };
    return mcdr_dlss_snapshot_depth(reinterpret_cast<void*>(ctx->commandList),
                                    reinterpret_cast<void*>(res), tl, ctx, queryState);
}

int queryDlss(char* out, int cap) {
    if (cap < 0 || (cap > 0 && out == nullptr)) {
        return -1;
    }
    const auto fail = [out, cap](const char* what) {
        if (out != nullptr && cap > 0) {
            std::snprintf(out, static_cast<size_t>(cap), "%s", what);
        }
        return 0;
    };
    if (auto d = ensureDevice(); !d) {
        return fail("device not created");
    }
    wireDlssImpl();
    // 探测缓存可能是在厂商 DLL 还没就位时建的（那时只报告"运行期不在"）。这里给一次
    // 重探的机会：只有"上次没查成 + 现在运行期在"，才允许重探一次，避免每次查询都重新
    // LoadLibrary。判据取自探测自己的 dlssQueried / ngxRuntime，不再另设状态。
    if (!gCtx.rtx.dlssQueried && gCtx.rtx.ngxRuntime) {
        mcdr::resetRtxSupportCache();
        gCtx.rtx = mcdr::probeRtxSupport(gCtx.adapter.Get());
        gDlssAvailabilityLogged = false;
        notifyDlssDeviceReady();
    }
    if (!gCtx.rtx.ngxRuntime) {
        return fail(gCtx.rtx.ngxNote.empty() ? "ngx runtime not found" : gCtx.rtx.ngxNote.c_str());
    }
    // P108：**真正的门是 NGX 的 Init**，不是探测那步 GetFeatureRequirements。实机证明
    // 后者会在运行期 DLL 尚未加载时返回 FAIL_InvalidParameter，那是"答不了"而不是
    // "不支持"；如果在这里因为 dlssSuperResolution=false 就拒掉，DLSS 永远走不到 Init。
    // 顺序：Init 成功 ⇒ 可用（适配器那步只作为失败时的补充说明）。
    if (mcdr_dlss_can_run(reinterpret_cast<void*>(gCtx.device.Get())) != 1) {
        if (gCtx.rtx.dlssQueried && !gCtx.rtx.dlssSuperResolution) {
            char why[192] = {};
            std::snprintf(why, sizeof(why), "NGX init failed and the adapter reports no DLSS: %s",
                          gCtx.rtx.dlssSuperResolutionWhy.empty()
                              ? "unknown"
                              : gCtx.rtx.dlssSuperResolutionWhy.c_str());
            return fail(why);
        }
        return fail("NGX init failed (see the P108 WARN line)");
    }
    char note[256] = {};
    char optimal[256] = {};
    char runtime[512] = {};
    uint32_t flags = 0;
    mcdr_dlss_query(note, sizeof(note), optimal, sizeof(optimal), runtime, sizeof(runtime), &flags);
    if (out != nullptr && cap > 0) {
        std::snprintf(out, static_cast<size_t>(cap), "%s | %s | runtime=%s | flags=0x%X", note,
                      optimal[0] != '\0' ? optimal : "(no optimal settings yet)",
                      runtime[0] != '\0' ? runtime : "(unknown)",
                      static_cast<unsigned>(flags));
    }
    return 1;
}

// P104b：对一张**已渲染完成**的纹理就地做 SMAA（世界画完、GUI 还没画时由 Java 侧调用）。
//
// 为什么是「就地」而不是在最终 blit 上做：blit 时整帧（世界 + GUI）已经合成完毕，SMAA
// 会连同界面一起柔化，用户看到的是「字发虚、按钮发糊」。把三阶段挪到 GUI 之前，就只对
// 世界层生效，blit 回到纯线性升采样（行为与关掉 AA 时完全一致）。
//
// 同一资源不能既当 SRV（第 1/3 趟要采样）又当 RTV，所以第 3 趟写进一张与源同尺寸同格式的
// scratch（smaaWorldResult），再 CopyResource 拷回；返回时源纹理停在 RENDER_TARGET，
// GUI 的 render pass 可以直接接着当附件用。
//
// aaMode != SMAA / 参数非法 / 中途失败 → 返回 false（失败会一次性关掉 SMAA 并打 ERROR）。
bool applySmaaToTexture(CommandContext* ctx, Dx12Object* tex) {
    // P104：只有 1(SMAA) 才走三趟；0/2/3 一律跳过（语义与 P104 时一致）。
    // P108（用户 m10920）：DLSS 已从抗锯齿枚举里拆出去，**不再**在这里判 DLSS —— SMAA 与
    // DLSS 现在是独立开关，两者都开时这一趟先跑（在低分辨率世界层上），再由
    // applyDlssToTexture 把它升采样到窗口尺寸。
    if (gCtx.aaMode != 1 || gCtx.smaaDisabled) {
        return false;
    }
    if (!ctx || !ctx->listOpen || !tex || !tex->resource) {
        return false;
    }
    ID3D12GraphicsCommandList* cmd = ctx->commandList;
    if (cmd == nullptr) {
        return false;
    }
    const D3D12_RESOURCE_DESC srcDesc = tex->resource->GetDesc();
    const UINT64 srcW64 = srcDesc.Width;
    const UINT64 srcH64 = srcDesc.Height;
    if (srcW64 == 0 || srcH64 == 0) {
        return false;
    }
    if (srcDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || srcDesc.SampleDesc.Count != 1) {
        // 非 2D / MSAA 目标既不能当 SRV 采样也不能 CopyResource：静默不适用（不是错误）。
        if (!gCtx.smaaUnavailableLogged) {
            gCtx.smaaUnavailableLogged = true;
            dbgLogWarn("P104: target is not a single-sampled 2D texture; skipping the in-place SMAA path");
        }
        return false;
    }
    DXGI_FORMAT srcFormat = srcDesc.Format;
    if (srcFormat == DXGI_FORMAT_UNKNOWN) {
        srcFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    // 任何一步失败都彻底关掉 SMAA，避免每帧重试刷日志 /
    // 把中间纹理留在与预期不符的状态上继续用。
    auto fail = [](const char* where, const std::string& what) {
        gCtx.smaaDisabled = true;
        gCtx.aaMode = 0;
        dbgLogError("P104: SMAA failed in %s; SMAA disabled: %s", where, firstLine(what).c_str());
        return false;
    };
    if (auto r = smaaEnsurePipelines(srcFormat); !r) {
        gCtx.smaaUnavailableLogged = true;
        return fail("smaaEnsurePipelines", r.error().message);
    }
    // 官方 LUT：两条权重管线都已建好，这里只是初始化两张 LUT（一次，幂等）并选一条。
    // 失败**不是**致命错误：退回解析式权重趟（kSmaaWeightPS），只 WARN 一行、只报一次。
    // 注意用独立的 smaaLutFallbackWarned，别占用 smaaUnavailableLogged（那是
    // 「目标不是单采样 2D 纹理」的一次性提示，两者是不同的语义）。
    if (!gCtx.smaaLutsReady) {
        auto lut = smaaEnsureLuts();
        if (!lut && !gCtx.smaaLutFallbackWarned) {
            gCtx.smaaLutFallbackWarned = true;
            dbgLogWarn("P104: SMAA falling back to the analytic blending-weight shader; official LUTs unavailable: %s",
                       firstLine(lut.error().message).c_str());
        }
    }
    const bool useLutWeights = gCtx.smaaLutsReady && gCtx.smaaWeightLutPso;
    if (!useLutWeights) {
        if (!gCtx.smaaLutFallbackWarned) {
            gCtx.smaaLutFallbackWarned = true;
            dbgLogWarn("P104: SMAA falling back to the analytic blending-weight shader (official LUTs unavailable)");
        }
    } else if (gCtx.smaaAppliedFrames == 0 && !gCtx.smaaLutReadyLogged) {
        // P104 fix：这一行原来挂在 smaaEnsurePipelines 里的 `if (gCtx.smaaLutsReady)` 分支上，
        // 但**调用顺序是 pipelines → LUTs**，那一刻 smaaLutsReady 必然还是 false ⇒ 永远打不出来。
        // 挪到这里（LUT 刚初始化完）才反映真实状态。
        // 必须用一次性闸门：applySmaaToTexture **每帧**都会走到这里，没有闸门就会逐帧刷屏
        // （实测 18:09:53–18:10:08 刷了 15 秒）。smaaAppliedFrames==0 表示「首帧之前」。
        gCtx.smaaLutReadyLogged = true;
        dbgLogInfo("P104: SMAA using the official AreaTex/SearchTex LUTs (160x560 RG8, 64x16 R8)");
    }
    if (auto r = smaaEnsureTargets(srcW64, srcH64); !r) {
        return fail("smaaEnsureTargets", r.error().message);
    }
    if (auto r = smaaEnsureWorldResult(srcDesc); !r) {
        return fail("smaaEnsureWorldResult", r.error().message);
    }
    if (auto r = initBlitPipeline(); !r) {
        return fail("initBlitPipeline", r.error().message);
    }
    const BlitPipeline* bp = getBlitPipeline();
    if (bp == nullptr) {
        return fail("getBlitPipeline", "blit pipeline unavailable");
    }

    const D3D12_VIEWPORT srcVp{0.0f, 0.0f, static_cast<FLOAT>(srcW64), static_cast<FLOAT>(srcH64), 0.0f, 1.0f};
    const D3D12_RECT srcSc{0, 0, static_cast<LONG>(srcW64), static_cast<LONG>(srcH64)};

    cmd->SetGraphicsRootSignature(gCtx.smaaRootSig.Get());
    cmd->IASetVertexBuffers(0, 1, &bp->vbView);
    cmd->IASetIndexBuffer(&bp->ibView);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const D3D12_RESOURCE_STATES targetsPrev =
        gSmaaTargetsInSrvState ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;

    // ① 边检测：source → edges
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTo(ctx, gCtx.smaaEdges.Get(), targetsPrev, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->OMSetRenderTargets(1, &gCtx.smaaEdgesRtv, FALSE, nullptr);
    cmd->RSSetViewports(1, &srcVp);
    cmd->RSSetScissorRects(1, &srcSc);
    cmd->SetPipelineState(gCtx.smaaEdgePso.Get());
    if (auto r = smaaBindSrv(ctx, cmd, tex->resource.Get(), srcFormat, 0); !r) {
        return fail("pass1.bind", r.error().message);
    }
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    // ② 混合权重：edges (+ AreaTex/SearchTex) → weights
    transitionTo(ctx, gCtx.smaaEdges.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTo(ctx, gCtx.smaaWeights.Get(), targetsPrev, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->OMSetRenderTargets(1, &gCtx.smaaWeightsRtv, FALSE, nullptr);
    cmd->RSSetViewports(1, &srcVp);
    cmd->RSSetScissorRects(1, &srcSc);
    cmd->SetPipelineState(useLutWeights ? gCtx.smaaWeightLutPso.Get() : gCtx.smaaWeightPso.Get());
    if (auto r = smaaBindSrv(ctx, cmd, gCtx.smaaEdges.Get(), DXGI_FORMAT_R8G8_UNORM, 0); !r) {
        return fail("pass2.bind", r.error().message);
    }
    if (useLutWeights) {
        // 官方 LUT 路径额外需要 t1 = AreaTex、t2 = SearchTex（SRV 常驻，见 smaaBindLuts）。
        if (auto r = smaaBindLuts(cmd); !r) {
            return fail("pass2.bind.luts", r.error().message);
        }
    }
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    // ③ 邻域混合：source + weights → scratch（**源纹理本身正在被采样，不能同时当 RTV**）
    transitionTo(ctx, gCtx.smaaWeights.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gSmaaTargetsInSrvState = true;
    transitionTo(ctx, gCtx.smaaWorldResult.Get(), D3D12_RESOURCE_STATE_COMMON,
                 D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->OMSetRenderTargets(1, &gCtx.smaaWorldResultRtv, FALSE, nullptr);
    cmd->RSSetViewports(1, &srcVp);
    cmd->RSSetScissorRects(1, &srcSc);
    cmd->SetPipelineState(gCtx.smaaBlendPso.Get());
    if (auto r = smaaBindSrv(ctx, cmd, tex->resource.Get(), srcFormat, 0); !r) {
        return fail("pass3.bind.t0", r.error().message);
    }
    if (auto r = smaaBindSrv(ctx, cmd, gCtx.smaaWeights.Get(), DXGI_FORMAT_R8G8_UNORM, 1); !r) {
        return fail("pass3.bind.t1", r.error().message);
    }
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    // ④ 拷回并交还：scratch(COPY_SOURCE) → 源纹理(COPY_DEST) → 源纹理回 RENDER_TARGET，
    // 这样紧跟着的 GUI render pass 可以照常把主渲染目标当附件用；scratch 回 COMMON
    // （它的稳定态就是 COMMON，与 transitionTo 的初始锚点一致）。
    transitionTo(ctx, gCtx.smaaWorldResult.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(tex->resource.Get(), gCtx.smaaWorldResult.Get());
    transitionTextureTo(ctx, tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transitionTo(ctx, gCtx.smaaWorldResult.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                 D3D12_RESOURCE_STATE_COMMON);

    ++gCtx.smaaAppliedFrames;
    if (gCtx.smaaAppliedFrames == 1) {
        dbgLogInfo("P104: SMAA applied on the first frame: world layer %llux%llu anti-aliased in place (fmt=%d)",
                   static_cast<unsigned long long>(srcW64), static_cast<unsigned long long>(srcH64),
                   static_cast<int>(srcFormat));
    }
    return true;
}

// ---------------------------------------------------------------------------
// P152：TAA（时域抗锯齿）—— 历史与管线都在 native 侧
// ---------------------------------------------------------------------------
// 与 SMAA 的区别不只是"又一个后处理"，而是**状态归属**：SMAA 是单帧形态学，三趟只借自身的
// 中间纹理；TAA 需要跨帧状态（历史纹理乒乓、jitter、历史代），此前全部由 JVM 的 D3D12TAA
// 持有，并且每帧要录一趟 beginRenderPass + draw + 回填 blit。这里把整条链路搬进 native：
// JVM 每帧只调一次 applyTaaToTexture，不再跨语言搬运 GPU 资源句柄。
//
// 两趟：
//   ① resolve  scene(t1) + history[read](t2) + motion(t3) → history[write]（RGBA16F）
//   ② 回填    history[write] → 主目标（锐化量 > 0 走 5-tap unsharp，否则纯采样 blit）
// 历史**不参与锐化**：把锐化结果写回历史，会随每帧的 lerp 递归叠加，几十帧后边缘出现振铃。
// 历史也不是 RGBA8：混合权重是 1/16，8 位历史会把每帧 1/16 的增量就地量化掉，静止画面会
// 出现缓慢漂移的时间性带状。
namespace {

// 历史纹理格式：16 位浮点。8 位历史会把每帧 1/16 的混合增量就地量化掉。
constexpr DXGI_FORMAT kTaaHistoryFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// 参数块：10 个有效 float + 2 个对齐 pad。走 32-bit 根常量，不需要 CBV 与上传缓冲
// （JVM 侧当年用 CBV，是因为它只能走通用的 pushDescriptors 路径）。
constexpr UINT kTaaParam32BitCount = 12;

// 上一帧结束时两张历史纹理是否都停在 PIXEL_SHADER_RESOURCE。首帧（刚创建）与
// taaEnsureHistory 重建之后都是 COMMON，所以需要这个跨帧标记 —— 与 SMAA 的
// gSmaaTargetsInSrvState 同一套理由（D3D12 不允许在同一个命令列表里显式回切 COMMON）。
bool gTaaHistoryInSrvState = false;

// resolve 的像素着色器：P152 起这是**唯一**实现（JVM 侧的 D3D12TAA 已删掉历史/管线/HLSL，
// 只负责算出每帧参数再调本函数）。
// 全部用 Load + 手算双线性 —— 静态采样器是点采样 + 环绕，点采样会丢掉亚像素精度，环绕取样
// 在 UV 出界时会从屏幕另一侧拖内容进来，这两件事都必须变成显式行为。
static const char kTaaPS[] =
    "struct VS_OUT { float4 pos : SV_Position; float2 tc : TEXCOORD0; };\n"
    "cbuffer Params : register(b0, space0) {\n"
    "    float2 ScreenSize;\n"
    "    float2 RcpScreenSize;\n"
    "    float Blend;\n"
    "    float ResetHistory;\n"
    "    float2 Jitter;\n"
    "    float2 PrevJitter;\n"
    "    float2 ParamsPad;\n"
    "};\n"
    "Texture2D<float4> Scene : register(t1, space0);\n"
    "Texture2D<float4> HistoryBuffer : register(t2, space0);\n"
    "Texture2D<float2> Motion : register(t3, space0);\n"
    "float3 toYCoCg(float3 c) {\n"
    "    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)),\n"
    "                  dot(c, float3(-0.25, 0.5, -0.25)));\n"
    "}\n"
    "float3 fromYCoCg(float3 y) {\n"
    "    float t = y.x - y.z;\n"
    "    return float3(t + y.y, y.x + y.z, t - y.y);\n"
    "}\n"
    "float3 fetchScene(int2 coord, int2 maxCoord) {\n"
    "    return Scene.Load(int3(clamp(coord, int2(0, 0), maxCoord), 0)).rgb;\n"
    "}\n"
    "float3 fetchHistory(int2 coord, int2 maxCoord) {\n"
    "    return HistoryBuffer.Load(int3(clamp(coord, int2(0, 0), maxCoord), 0)).rgb;\n"
    "}\n"
    "float3 bilinearScene(float2 uv, int2 maxCoord) {\n"
    "    float2 texel = uv * ScreenSize - 0.5;\n"
    "    int2 baseCoord = int2(floor(texel));\n"
    "    float2 f = texel - float2(baseCoord);\n"
    "    float3 c00 = fetchScene(baseCoord, maxCoord);\n"
    "    float3 c10 = fetchScene(baseCoord + int2(1, 0), maxCoord);\n"
    "    float3 c01 = fetchScene(baseCoord + int2(0, 1), maxCoord);\n"
    "    float3 c11 = fetchScene(baseCoord + int2(1, 1), maxCoord);\n"
    "    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);\n"
    "}\n"
    "float3 bilinearHistory(float2 uv, int2 maxCoord) {\n"
    "    float2 texel = uv * ScreenSize - 0.5;\n"
    "    int2 baseCoord = int2(floor(texel));\n"
    "    float2 f = texel - float2(baseCoord);\n"
    "    float3 c00 = fetchHistory(baseCoord, maxCoord);\n"
    "    float3 c10 = fetchHistory(baseCoord + int2(1, 0), maxCoord);\n"
    "    float3 c01 = fetchHistory(baseCoord + int2(0, 1), maxCoord);\n"
    "    float3 c11 = fetchHistory(baseCoord + int2(1, 1), maxCoord);\n"
    "    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);\n"
    "}\n"
    "float4 main(VS_OUT IN) : SV_Target0 {\n"
    "    int2 size = int2(ScreenSize + 0.5);\n"
    "    int2 maxCoord = size - 1;\n"
    "    int2 coord = clamp(int2(IN.pos.xy), int2(0, 0), maxCoord);\n"
    "    // 累加网格不抖动：本帧颜色按 jitter 的反方向重采样回未抖动网格，历史只按真实运动\n"
    "    // 矢量取样。jitter 注入的是投影矩阵列 2，内容在 NDC 里往 +2j/尺寸 走；换算成像素\n"
    "    // 坐标（y 向下）就是内容整体移动 (+J.x, -J.y) ⇒ 要取回 I(c) 得采样 c - (+J.x, -J.y)。\n"
    "    float2 curUv = (float2(coord) + 0.5 - float2(Jitter.x, -Jitter.y)) * RcpScreenSize;\n"
    "    float3 currentRgb = bilinearScene(curUv, maxCoord);\n"
    "    float currentA = Scene.Load(int3(coord, 0)).a;\n"
    "    // 被拒绝的像素用像素中心的原始样本：重采样在不连续处会按亚像素比例混合前景/背景，\n"
    "    // 这个比例每帧随 Jitter 变却不随场景移动 ⇒ 边缘看起来不跟随摄像机。\n"
    "    float3 rawRgb = Scene.Load(int3(coord, 0)).rgb;\n"
    "    // 运动矢量约定 prevPos - curPos（像素单位，真实运动、不含 jitter）。\n"
    "    float2 mv = Motion.Load(int3(coord, 0)).xy;\n"
    "    float2 historyUv = (float2(coord) + 0.5 + mv) * RcpScreenSize;\n"
    "    // 3x3 邻域的 YCoCg 统计（variance clipping 的包围盒）。邻域样本必须和 currentRgb\n"
    "    // 处在同一个域（未抖动网格），否则包围盒本身逐帧随 Jitter 抖。\n"
    "    float3 sum = 0.0;\n"
    "    float3 sumSq = 0.0;\n"
    "    [unroll] for (int dy = -1; dy <= 1; ++dy) {\n"
    "        [unroll] for (int dx = -1; dx <= 1; ++dx) {\n"
    "            float3 y = toYCoCg(bilinearScene(curUv + float2(dx, dy) * RcpScreenSize, maxCoord));\n"
    "            sum += y;\n"
    "            sumSq += y * y;\n"
    "        }\n"
    "    }\n"
    "    const float samples = 9.0;\n"
    "    float3 mean = sum / samples;\n"
    "    float3 variance = max(sumSq / samples - mean * mean, 0.0);\n"
    "    float3 lo = mean - 1.5 * sqrt(variance);\n"
    "    float3 hi = mean + 1.5 * sqrt(variance);\n"
    "    float3 history = bilinearHistory(historyUv, maxCoord);\n"
    "    // variance clipping 只钳不拒，权重恒为 1 - Blend：按「钳掉多少」调制权重会让静态边缘\n"
    "    // 逐帧跳，用户看到的就是「静止也有随机抖动」。\n"
    "    float3 clamped = clamp(toYCoCg(history), lo, hi);\n"
    "    float3 resolved = lerp(fromYCoCg(clamped), currentRgb, Blend);\n"
    "    float motionLength = length(mv);\n"
    "    bool outside = (historyUv.x < 0.0) || (historyUv.y < 0.0) || (historyUv.x > 1.0) ||\n"
    "                   (historyUv.y > 1.0);\n"
    "    bool absurd = !(motionLength <= 0.25 * max(ScreenSize.x, ScreenSize.y));\n"
    "    if (ResetHistory > 0.5 || outside || absurd) {\n"
    "        resolved = rawRgb;\n"
    "    }\n"
    "    return float4(resolved, currentA);\n"
    "}\n";

// 建 TAA 的根签名 + resolve PSO（懒初始化，幂等）。
// 根参数：0 = b0 的 12 个 32-bit 常量；1..3 = t1/t2/t3 各一张 SRV 表。三张表分开建，
// 而不是塞进一张 NumDescriptors=3 的表 —— smaaBindSrv 一次只写一个槽位并把该槽位挂成表，
// 连续三张分开的表正好与它兼容（也就不需要再写一套堆槽位计算）。
mcdr::Result<void> taaEnsurePipelines() {
    if (gCtx.taaRootSig && gCtx.taaPso) {
        return {};
    }

    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    for (UINT i = 0; i < 3; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i + 1;  // t1 = scene / t2 = history / t3 = motion
        ranges[i].RegisterSpace = 0;
        ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = kTaaParam32BitCount;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT i = 0; i < 3; ++i) {
        params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        params[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 4;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 0;
    rsDesc.pStaticSamplers = nullptr;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        return mcdr::err("taaEnsurePipelines", "SerializeRootSignature hr=0x" + hrText(hr));
    }
    ComPtr<ID3D12RootSignature> rootSig;
    hr = gCtx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        return mcdr::err("taaEnsurePipelines", "CreateRootSignature hr=0x" + hrText(hr));
    }

    // VS 与 blit / SMAA 同一份全屏四边形 HLSL（POSITION + TEXCOORD，配合 blit 的顶点缓冲）。
    // 硬要求 SM 6.9 + DXC，无 5.1(DXBC) 兜底。
    const std::string smProfile = graphicsShaderModelProfile();
    if (smProfile.empty()) {
        return mcdr::err("taaEnsurePipelines", "taaEnsurePipelines: " + sm69RequirementText());
    }
    std::vector<uint8_t> vsDxil;
    const std::string vsSrc(kBlitVS, sizeof(kBlitVS) - 1);
    auto vsDxc = compileHlslToDxil(vsSrc, "main", "vs_" + smProfile, vsDxil);
    if (!vsDxc) {
        return mcdr::err("taaEnsurePipelines", "taa_vs (DXC): " + firstLine(vsDxc.error().message));
    }
    const void* vsCode = vsDxil.data();
    SIZE_T vsCodeSize = vsDxil.size();

    // PS：DXIL 缓冲必须活到 PSO 建完（不再有 ID3DBlob 那条路）。
    std::vector<uint8_t> psDxil;
    const void* psCode = nullptr;
    SIZE_T psCodeSize = 0;
    if (auto r = smaaCompilePs(kTaaPS, "taa_ps", smProfile, psDxil, psCode, psCodeSize); !r) {
        return mcdr::err("taaEnsurePipelines", errorMessage(r.error()));
    }

    auto pso = smaaCreatePso(rootSig, vsCode, vsCodeSize, psCode, psCodeSize, kTaaHistoryFormat);
    if (!pso) {
        return mcdr::err("taaEnsurePipelines", errorMessage(pso.error()));
    }

    gCtx.taaRootSig = rootSig;
    gCtx.taaPso = pso.value();
    return {};
}

// 懒创建两张 RGBA16F 历史纹理 + 2 槽 RTV 堆；尺寸变化时整组重建。
mcdr::Result<void> taaEnsureHistory(UINT64 w, UINT64 h) {
    if (gCtx.taaHistory[0] && gCtx.taaHistory[1] && gCtx.taaWidth == w && gCtx.taaHeight == h) {
        return {};
    }
    gCtx.taaHistory[0].Reset();
    gCtx.taaHistory[1].Reset();
    gCtx.taaHistoryRtv[0] = {};
    gCtx.taaHistoryRtv[1] = {};
    gCtx.taaRtvHeap.Reset();
    gCtx.taaHistoryValid = false;
    gTaaHistoryInSrvState = false;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 2;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HRESULT hr = gCtx.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gCtx.taaRtvHeap));
    if (FAILED(hr)) {
        return mcdr::err("taaEnsureHistory", "CreateDescriptorHeap(RTV) hr=0x" + hrText(hr));
    }
    const UINT rtvInc = gCtx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = gCtx.taaRtvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = w;
    texDesc.Height = static_cast<UINT>(h);
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = kTaaHistoryFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    for (int i = 0; i < 2; ++i) {
        hr = gCtx.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                  D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&gCtx.taaHistory[i]));
        if (FAILED(hr)) {
            return mcdr::err("taaEnsureHistory", "CreateCommittedResource(history) hr=0x" + hrText(hr));
        }
        gCtx.taaHistoryRtv[i] = handle;
        gCtx.device->CreateRenderTargetView(gCtx.taaHistory[i].Get(), nullptr, handle);
        handle.ptr += rtvInc;
    }
    gCtx.taaWidth = w;
    gCtx.taaHeight = h;
    return {};
}

}  // namespace
int applyTaaToTexture(CommandContext* ctx, Dx12Object* scene, Dx12Object* motion, float jitterX,
                      float jitterY, float prevJitterX, float prevJitterY, float blend, int reset,
                      float sharpenAmount) {

    if (gCtx.aaMode != 2 || gCtx.taaDisabled) {
        return 0;
    }
    if (!ctx || !ctx->listOpen || ctx->commandList == nullptr || !scene || !scene->resource || !motion) {
        return 0;
    }

    // 运动矢量按 TextureView 传进来（D3D12Velocity 的 view）：解包到底层纹理。
    // 注意 view 对象自身的 resource 是空的（它只是底层纹理的一个视图），所以这里不能像
    // scene 那样要求 !motion->resource —— 早期版本这么写过，结果 TAA 每帧静默返回 0。
    Dx12Object* motionTex = motion;
    if (motion->kind == Dx12Object::Kind::TextureView) {
        if (motion->sourceTexture == nullptr || !motion->sourceTexture->resource) {
            return 0;
        }
        motionTex = motion->sourceTexture;
    } else if (!motion->resource) {
        return 0;
    }

    ID3D12GraphicsCommandList* cmd = ctx->commandList;
    const D3D12_RESOURCE_DESC sceneDesc = scene->resource->GetDesc();
    const D3D12_RESOURCE_DESC motionDesc = motionTex->resource->GetDesc();
    if (sceneDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sceneDesc.SampleDesc.Count != 1 ||
        motionDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || motionDesc.SampleDesc.Count != 1) {
        if (!gCtx.taaUnavailableLogged) {
            gCtx.taaUnavailableLogged = true;
            dbgLogWarn("P152: TAA needs single-sampled 2D scene and motion textures; skipping TAA");
        }
        return 0;
    }
    const UINT64 w = sceneDesc.Width;
    const UINT64 h = sceneDesc.Height;
    if (w == 0 || h == 0 || motionDesc.Width != w || motionDesc.Height != h) {
        if (!gCtx.taaUnavailableLogged) {
            gCtx.taaUnavailableLogged = true;
            dbgLogWarn("P152: TAA needs matching scene/motion sizes (%llux%llu vs %llux%llu); skipping TAA",
                       static_cast<unsigned long long>(w), static_cast<unsigned long long>(h),
                       static_cast<unsigned long long>(motionDesc.Width),
                       static_cast<unsigned long long>(motionDesc.Height));
        }
        return 0;
    }
    // 回填走 blit / sharpen 两条现成管线，它们的 RTVFormats[0] 都写死 R8G8B8A8_UNORM ⇒ 场景
    // 不是这个格式就没有可用的目标管线。UNKNOWN 按 SMAA 的既有约定视为 RGBA8。
    DXGI_FORMAT sceneFormat = sceneDesc.Format;
    if (sceneFormat == DXGI_FORMAT_UNKNOWN) {
        sceneFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (sceneFormat != DXGI_FORMAT_R8G8B8A8_UNORM) {
        if (!gCtx.taaUnavailableLogged) {
            gCtx.taaUnavailableLogged = true;
            dbgLogWarn("P152: TAA needs an RGBA8 scene target (fmt=%d); skipping TAA",
                       static_cast<int>(sceneFormat));
        }
        return 0;
    }

    // 任何一步失败都彻底关掉 TAA：避免每帧重试刷日志，也避免把历史留在与预期不符的状态上。
    auto fail = [](const char* where, const std::string& what) {
        gCtx.taaDisabled = true;
        dbgLogError("P152: TAA failed in %s; TAA disabled: %s", where, firstLine(what).c_str());
        return -1;
    };
    if (auto r = taaEnsurePipelines(); !r) {
        gCtx.taaUnavailableLogged = true;
        return fail("taaEnsurePipelines", r.error().message);
    }
    if (auto r = taaEnsureHistory(w, h); !r) {
        gCtx.taaUnavailableLogged = true;
        return fail("taaEnsureHistory", r.error().message);
    }
    if (auto r = initBlitPipeline(); !r) {
        return fail("initBlitPipeline", r.error().message);
    }
    const BlitPipeline* bp = getBlitPipeline();
    if (bp == nullptr) {
        return fail("getBlitPipeline", "blit pipeline unavailable");
    }

    // 尺寸/格式变化后历史不可信：本帧必须以 reset 开始（JVM 侧 reset() 时同样会置 false）。
    const bool resetNow = reset != 0 || !gCtx.taaHistoryValid;
    const int readIndex = gCtx.taaReadIndex;
    const int writeIndex = gCtx.taaWriteIndex;

    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<FLOAT>(w), static_cast<FLOAT>(h), 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};

    // 历史在上一帧结束时**两张都停在 PIXEL_SHADER_RESOURCE**（见本节末尾的回切），所以本帧
    // 的初始锚点是它；首帧（刚创建）是 COMMON。不用 transitionTo 的 COMMON 回落语义：
    // D3D12 禁止在同一个命令列表里把已提升的资源显式回切 COMMON。
    const D3D12_RESOURCE_STATES historyPrev =
        gTaaHistoryInSrvState ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;

    // ① resolve：scene + history[read] + motion → history[write]
    transitionTextureTo(ctx, scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTo(ctx, gCtx.taaHistory[readIndex].Get(), historyPrev,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTextureTo(ctx, motionTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionTo(ctx, gCtx.taaHistory[writeIndex].Get(), historyPrev, D3D12_RESOURCE_STATE_RENDER_TARGET);

    cmd->SetGraphicsRootSignature(gCtx.taaRootSig.Get());
    cmd->SetPipelineState(gCtx.taaPso.Get());
    cmd->IASetVertexBuffers(0, 1, &bp->vbView);
    cmd->IASetIndexBuffer(&bp->ibView);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->OMSetRenderTargets(1, &gCtx.taaHistoryRtv[writeIndex], FALSE, nullptr);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    // b0 = {w, h, 1/w, 1/h, blend, reset, jitter, prevJitter, pad}（顺序与 kTaaPS 的 cbuffer 一致）
    const float params[kTaaParam32BitCount] = {static_cast<float>(w),
                                               static_cast<float>(h),
                                               w == 0 ? 0.0f : 1.0f / static_cast<float>(w),
                                               h == 0 ? 0.0f : 1.0f / static_cast<float>(h),
                                               blend,
                                               resetNow ? 1.0f : 0.0f,
                                               jitterX,
                                               jitterY,
                                               prevJitterX,
                                               prevJitterY,
                                               0.0f,
                                               0.0f};
    cmd->SetGraphicsRoot32BitConstants(0, kTaaParam32BitCount, params, 0);

    if (auto r = smaaBindSrv(ctx, cmd, scene->resource.Get(), sceneFormat, 1); !r) {
        return fail("pass1.bind.scene", r.error().message);
    }
    if (auto r = smaaBindSrv(ctx, cmd, gCtx.taaHistory[readIndex].Get(), kTaaHistoryFormat, 2); !r) {
        return fail("pass1.bind.history", r.error().message);
    }
    if (auto r = smaaBindSrv(ctx, cmd, motionTex->resource.Get(), motionTex->dxgiFormat, 3); !r) {
        return fail("pass1.bind.motion", r.error().message);
    }
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    // ② 回填：history[write] → 主目标。不能 CopyResource —— 历史是 16F、主目标是 8 位。
    // 栈上临时包一层 Dx12Object，好复用 blitTextToTexture / sharpenTextureToTexture
    // （它们按 ID3D12Resource* 追踪状态，所以这个包装不需要进 gCtx）。
    Dx12Object historyObj{};
    historyObj.kind = Dx12Object::Kind::Texture;
    historyObj.resource = gCtx.taaHistory[writeIndex];
    historyObj.dxgiFormat = kTaaHistoryFormat;
    bool filled = false;
    if (sharpenAmount > 0.001f) {
        auto sr = sharpenTextureToTexture(ctx, &historyObj, scene, sharpenAmount);
        if (sr) {
            filled = true;
        } else if (!gCtx.taaSharpenFallbackWarned) {
            gCtx.taaSharpenFallbackWarned = true;
            dbgLogWarn("P152: TAA sharpen pass failed; falling back to a plain blit: %s",
                       firstLine(sr.error().message).c_str());
        }
    }
    if (!filled) {
        auto br = blitTextureToTexture(ctx, &historyObj, scene);
        if (!br) {
            return fail("pass2.blit", br.error().message);
        }
    }

    // 历史回切到 PIXEL_SHADER_RESOURCE：两张都停在这里，下一帧的锚点就是常量
    // （blit/sharpen 收尾会把源放回 RENDER_TARGET，这一步必须显式做）。
    transitionTo(ctx, gCtx.taaHistory[writeIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gTaaHistoryInSrvState = true;

    // 交换乒乓：本帧的写缓冲就是下一帧的历史。
    gCtx.taaReadIndex = writeIndex;
    gCtx.taaWriteIndex = readIndex;
    gCtx.taaHistoryValid = true;
    ++gCtx.taaAppliedFrames;
    if (gCtx.taaAppliedFrames == 1) {
        dbgLogInfo("P152: TAA applied on the first frame: %llux%llu, history RGBA16F double buffered, blend=%.3f",
                   static_cast<unsigned long long>(w), static_cast<unsigned long long>(h),
                   static_cast<double>(blend));
    }
    return 1;
}
// ---------------------------------------------------------------------------
// P33 async：独立的 C++ 渲染线程
// ---------------------------------------------------------------------------
namespace {

// 四阶段 Windows events（manual-reset，多线程安全）
HANDLE gEvtBeginFrame = nullptr;  // 主线程→渲染线程：开始新帧
HANDLE gEvtRecordingReady = nullptr;  // 渲染线程→主线程：可以 push 命令了
HANDLE gEvtCommandsReady = nullptr;  // 主线程→渲染线程：所有命令已入队
HANDLE gEvtSubmitDone = nullptr;  // 渲染线程→主线程：命令已提交（不再等 Present）

// P98：渲染线程正在处理一帧（从取到 ctx 起，到此帧 Present 收尾止）。
// 主线程在 configureSurface 里必须确认「渲染线程没有在飞帧」——旧实现无条件
// SetEvent(gEvtBeginFrame) + 等 gEvtSubmitDone：当 gAsyncRenderCtx 为空（上一帧早已
// 收尾）时渲染线程被唤醒后只走 `continue`，永远不会置位 submitDone ⇒ 主线程白等满
// 10 s。P98 实测：世界入场 configureSurface total=10635.9 ms / submitDone=10000.1 ms
// (waitResult=258=WAIT_TIMEOUT) / pendingPresents=0.0 → 主线程帧间隔 12195.6 ms，
// 即用户报告的「加载世界弹出瞬间帧时间骤增 / 启动加载界面冻结」。
std::atomic<bool> gRenderThreadBusy{false};

// P48（多帧飞行）：Present 完成护栏。
// 主线程不再每帧等 Present 返回，因此它可能在上一帧 Present 尚未完成时就去
// acquire 下一个 back buffer。GetCurrentBackBufferIndex 只在 Present 之后轮转，
// 提前 acquire 会拿到同一个 buffer（两帧写同一处 → 闪帧/撕裂）。
// acquireSurface 在真正 acquire 前等待「已提交帧数 == 已完成 Present 帧数」，
// 通常为 0ms（主线程录制耗时 ≫ 渲染线程 Present 耗时）。
std::mutex gPresentMtx;
std::condition_variable gPresentCv;
UINT64 gSubmittedFrames = 0;  // 已完成 ExecuteCommandLists 的帧数
UINT64 gPresentedFrames = 0;  // 已完成 Present 的帧数

// 渲染线程函数
static DWORD WINAPI renderThreadFunc(LPVOID /*param*/) {
    // 主线程每帧都会阻塞等待本线程完成提交，因此本线程处于帧关键路径上。
    // 原为 THREAD_PRIORITY_LOWEST，会被 MC 的区块构建 worker 抢占，拉长主线程
    // 的等待时间；提升到 NORMAL 以减少每帧固定同步开销。工作内容仍是
    // Close + Execute + Present，不会与主线程长时间竞争 CPU。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    // P33 诊断：记录渲染线程 OS 线程 id，便于与崩溃报告里的 tid 对照，
    // 判断异常发生在渲染线程还是主线程。
    dbgLogInfo("renderThread: started tid=%lu", static_cast<unsigned long>(GetCurrentThreadId()));

    while (true) {
        // 步骤 1：等待主线程请求开始新帧（500ms 超时 + 每轮检查 gRenderRunning 退出）
        for (;;) {
            if (!gRenderRunning) return 0;
            DWORD r = WaitForSingleObject(gEvtBeginFrame, 500);
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_TIMEOUT && !gRenderRunning) return 0;
        }
        if (!gRenderRunning) return 0;
        if (!gAsyncRenderCtx) continue;

        CommandContext* ctx = gAsyncRenderCtx;

        // P98：本帧处理期间置 busy=1，所有退出路径（continue/break/return）由 RAII 清除，
        // 供主线程的 waitForRenderThreadSubmit 判断「有没有在飞的帧」。
        struct RenderBusyGuard {
            RenderBusyGuard() { gRenderThreadBusy.store(true, std::memory_order_release); }
            ~RenderBusyGuard() { gRenderThreadBusy.store(false, std::memory_order_release); }
        };
        [[maybe_unused]] RenderBusyGuard renderBusyGuard;

        // P33 fix（帧序倒置修复）：命令录制发生在主线程调用 submit() **之前**，
        // 因此渲染线程绝不能再 Reset allocator / command list —— 那会把主线程刚录制
        // 的全部命令（render pass / clear / blit / draw）丢掉，back buffer 永远不被
        // 写入，Present 只能显示未初始化内容（窗口闪烁各种颜色的根因）。
        //
        // 现在本线程只负责：Close + ExecuteCommandLists + Present。
        // allocator 的 Reset 与列表 begin 由主线程在帧开始时调用 beginCommandList
        // 完成（内部按 fenceValue-2 等待 allocator 不再被 GPU 占用）。
        // P30/P45：以下握手日志处于**每帧关键路径**（主线程阻塞等待本线程），
        // 且发生在 SetEvent(RecordingReady) 之前——stderr/文件写会直接拉长主线程
        // 的等待时间。默认日志级别（WARN=1）下必须静默，故降为 DEBUG（级别 3）。
        dbgLogDebug("renderThread: wake ctx=%p listOpen=%d", reinterpret_cast<void*>(ctx), ctx->listOpen);

        // 步骤 2/3：acquireSurface 与 GPU fence 等待均已移交主线程
        // （dx12AcquireSurface + beginCommandList），本线程不再触碰 allocator/list。

        // 步骤 5：通知主线程命令已可提交
        dbgLogDebug("renderThread: signaling recordingReady ctx=%p", reinterpret_cast<void*>(ctx));
        SetEvent(gEvtRecordingReady);

        // 步骤 6：等待主线程完成命令录制（超时保护：5s 后放弃当前帧）
        dbgLogDebug("renderThread: waiting for gEvtCommandsReady ctx=%p", reinterpret_cast<void*>(ctx));
        // P3-native 插桩（DX12_PROF=1）：把主线程观测到的 waitSubmit 拆成
        // 「等主线程命令就绪 / Close+Execute+Signal / Present」三段，定位耗时归属。
        static const bool sProf = (getenv("DX12_PROF") != nullptr);
        static UINT64 sProfFrames = 0;
        static double sProfWaitCmdMs = 0.0, sProfSubmitMs = 0.0, sProfPresentMs = 0.0;
        const double tProfWait0 = sProf ? nowMs() : 0.0;
        int commandsWaitLoops = 0;
        for (;;) {
            if (!gRenderRunning) {
                destroySurfaceNoWaitIdle(getActiveSurface());
                return 0;
            }
            DWORD r = WaitForSingleObject(gEvtCommandsReady, 500);
            if (r == WAIT_OBJECT_0) {
                dbgLogDebug("renderThread: commandsReady ctx=%p", reinterpret_cast<void*>(ctx));
                break;
            }
            if (r == WAIT_TIMEOUT && !gRenderRunning) {
                destroySurfaceNoWaitIdle(getActiveSurface());
                return 0;
            }
            // P41：超时保护——5s 内未收到 CommandsReady，说明主线程卡死，放弃本帧
            if (++commandsWaitLoops >= 10) {
                dbgLogError("renderThread: commandsReady timeout after %ds, aborting frame ctx=%p",
                       commandsWaitLoops * 500 / 1000, reinterpret_cast<void*>(ctx));
                if (ctx->listOpen) {
                    // P44 fix：endCommandList 内部已统一递减 gOpenListCount，
                    // 此处不再重复递减。
                    (void)endCommandList(ctx);
                }
                gAsyncRenderCtx = nullptr;
                SetEvent(gEvtSubmitDone);
                break;
            }
        }

        // 步骤 7：end + submit + present
        const double tProfWait1 = sProf ? nowMs() : 0.0;
        auto submitRes = submitCommandList(ctx);
        const double tProfSubmit1 = sProf ? nowMs() : 0.0;
        if (!submitRes) {
            dbgLogError("renderThread: submitCommandList FAILED: %s", errorMessage(submitRes.error()).c_str());
            // P41 fix：不清除 surface（submit 失败不代表 surface 无效）
            if (gAsyncRenderCtx == ctx) gAsyncRenderCtx = nullptr;
            SetEvent(gEvtSubmitDone);
            continue;
        }
        [[maybe_unused]] const UINT64 value = *submitRes;

        // 全局 queue fence（submitCommandList 已推进 gCtx.queueFenceValue）
        ctx->lastSubmitQueueFence = gCtx.queueFenceValue;

        // per-backbuffer fence
        Dx12Surface* s = getActiveSurface();
        // P33 fix：back buffer index 必须在 presentSurface 之前捕获——presentSurface
        // 结尾会把 currentImageIndex 重置为 -1（释放 backbuffer 所有权），之后再读
        // 就只能拿到 -1，导致 surfaceFences / surfacePresentFences 永远写不进去。
        const int bbIdx = (s && s->currentImageIndex >= 0) ? s->currentImageIndex : -1;
        if (s && bbIdx >= 0) {
            if (s->surfaceFences.size() < static_cast<size_t>(kSurfaceBufferCount))
                s->surfaceFences.resize(kSurfaceBufferCount, 0);
            s->surfaceFences[static_cast<size_t>(bbIdx)] = gCtx.queueFenceValue;
        }

        DBG_LOG_DEBUG("renderThread: submit done v=%llu qf=%llu", (unsigned long long)value,
                      (unsigned long long)gCtx.queueFenceValue);

        // P48（多帧飞行）：命令已入队（ExecuteCommandLists + Signal 完成）。
        // 主线程只需知道「命令已提交」即可继续录制下一帧，**不必**再等 Present
        // 返回——把 submitDone 提前到 present 之前置位，让主线程的下一帧录制与
        // 本帧 Present 重叠执行（Present 实测 0.13-1.7ms，原为主线程每帧死等）。
        // 正确性由两点保证：
        //  1) back buffer 的 acquire 已推迟到 blit 之前，且 acquireSurface 会等待
        //     上一帧 Present 完成（gPresentCv），不会两帧写同一 back buffer；
        //  2) beginCommandList 等待本帧的 per-list fence 完成，命令列表不会被
        //     主线程在 GPU 执行期间 Reset。
        // P33 fix：标记本帧 Present 由本线程完成，让 Java 侧 present() 跳过重复
        // Present（重复 Present 会让 swapchain 连续 flip 两次 → 窗口闪烁各种颜色）。
        gAsyncOwnsPresent = true;
        {
            std::lock_guard<std::mutex> lk(gPresentMtx);
            ++gSubmittedFrames;
        }
        SetEvent(gEvtSubmitDone);

        // present（presentSurface 返回 void）
        dbgLogDebug("renderThread: about to presentSurface qf=%llu bbIdx=%d",
                    static_cast<unsigned long long>(gCtx.queueFenceValue), bbIdx);
        presentSurface(getActiveSurface());
        // P3-native 插桩：累加三段耗时，每 300 帧打印一次均值。
        if (sProf) {
            const double tProfPresent1 = nowMs();
            sProfWaitCmdMs += tProfWait1 - tProfWait0;
            sProfSubmitMs += tProfSubmit1 - tProfWait1;
            sProfPresentMs += tProfPresent1 - tProfSubmit1;
            if (++sProfFrames % 300 == 0) {
                const double inv = 1.0 / 300.0;
                dbgLogDebug("P3-native prof n=300: waitCommands=%.2fms close+exec+signal=%.2fms present=%.2fms",
                       sProfWaitCmdMs * inv, sProfSubmitMs * inv, sProfPresentMs * inv);
                sProfWaitCmdMs = sProfSubmitMs = sProfPresentMs = 0.0;
            }
        }
        // P48：Present 完成，放行主线程的 back buffer acquire。
        {
            std::lock_guard<std::mutex> lk(gPresentMtx);
            ++gPresentedFrames;
        }
        gPresentCv.notify_all();

        // P33 fix：signal per-surface present fence，确保 Display Controller 完成
        // flip 后再销毁 surface。deviceWaitIdle 仅等命令队列，不等待 display controller。
        if (s && bbIdx >= 0) {
            UINT64 pv = ++gCtx.queueFenceValue;
            gCtx.queue->Signal(gCtx.queueFence.Get(), pv);
            if (s->surfacePresentFences.size() < static_cast<size_t>(kSurfaceBufferCount))
                s->surfacePresentFences.resize(kSurfaceBufferCount, 0);
            s->surfacePresentFences[static_cast<size_t>(bbIdx)] = pv;
        }

        // P33 fix：在 present fence signal 之后才 flush 延迟删除对象，确保所有
        // GPU 工作（含 display controller flip）完成后才释放资源，避免 CORRUPTION。
        dbgLogDebug("renderThread: post-present qf=%llu openList=%d",
                    static_cast<unsigned long long>(gCtx.queueFenceValue), gOpenListCount);
        if (gOpenListCount == 0) flushPendingDeletes();

        // 步骤 8：本帧结束。submitDone 已在本帧 present **之前**置位（P48 多帧飞行），
        // 以便主线程的下一帧录制与本帧 Present 重叠，这里不再重复置位。
        dbgLogDebug("renderThread: frame done, looping back");
    }

    return 0;
}

}  // namespace

// P33 async renderer thread（外部可访问，供 destroySurface 调用）
std::thread gRenderThread;
// P33：渲染线程运行标志（外部可访问）
bool gRenderRunning = false;
// P33：当前正在处理的 ctx（外部可访问，供诊断读回检测 ASYNC 管道活跃）
CommandContext* gAsyncRenderCtx = nullptr;
// P33：本帧 Present 已由渲染线程完成（Java 侧 present() 需跳过，避免重复 Present）
bool gAsyncOwnsPresent = false;

// P48（多帧飞行）：等待所有已提交帧完成 Present。
// 主线程的 back buffer acquire 已推迟到 blit 之前（见 Dx12GpuSurface），
// 此时上一帧 Present 通常早已完成（渲染线程耗时 ≪ 主线程录制耗时）→ 通常 0ms。
// 若渲染线程落后则在此短暂阻塞——这是必要的正确性保证：GetCurrentBackBufferIndex
// 只在 Present 之后轮转，提前 acquire 会拿到同一个 back buffer（两帧写同一处
// → 闪帧/撕裂）。
// P68（GLFW_NO_API 崩溃根因）：超时后**不再放行**（旧实现在此 return 让主线程照常
// acquire，于是本帧 blit 写到 Present 已翻转走的 back buffer → D3D12 ERROR #907 →
// RemoveDevice 0x887A0005=DXGI_ERROR_ACCESS_DENIED，GLFW_NO_API 冷启动必崩）。
// 现在返回 false，由 acquireSurface 放弃本帧（Java 侧跳过 blit，下一帧让 MC 重建 surface）。
bool waitForPendingPresents(unsigned timeoutMs) {
    if (!gRenderRunning) return true;
    std::unique_lock<std::mutex> lk(gPresentMtx);
    if (gPresentedFrames >= gSubmittedFrames) return true;
    if (!gPresentCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                             [] { return gPresentedFrames >= gSubmittedFrames || !gRenderRunning; })) {
        dbgLogError("waitForPendingPresents: TIMEOUT submitted=%llu presented=%llu (render thread stalled)",
               static_cast<unsigned long long>(gSubmittedFrames),
               static_cast<unsigned long long>(gPresentedFrames));
        return false;
    }
    // 两种唤醒条件都是安全的：帧已呈现（index 已轮转完毕），或渲染线程已停止（index 不会再动）。
    return true;
}

// 等待渲染线程完成当前帧：信号 beginFrame + 等 submitDone，不销毁线程本身。
// 用于 destroySurface 场景：确保渲染线程不再并发操作 swapchain 后再调用 deviceWaitIdle。
void waitForRenderThreadSubmit() {
    if (!gRenderRunning || !gRenderThread.joinable()) return;
    // P98 修复：没有在飞的帧时不再做 submitDone 握手。渲染线程空闲时只在
    // WaitForSingleObject(gEvtBeginFrame) 处空转，不碰 swapchain；旧实现仍无条件
    // SetEvent + 等满 10 s 超时（见 gRenderThreadBusy 的注释与实测数据）。
    // 仍在飞的帧由 waitForPendingPresents 兜底（Present 完成后才返回）。
    if (!gRenderThreadBusy.load(std::memory_order_acquire)) {
        waitForPendingPresents();
        return;
    }
    // P98 诊断：量出实际等待时长（世界入场瞬间曾在日志里留下 10.5 s 空档，
    // 用户报告为「帧时间骤增 / 启动加载界面冻结」）。
    const double p98t0 = nowMs();
    // 若渲染线程正在 WaitForSingleObject(gEvtBeginFrame)，SetEvent 会唤醒它；
    // 若渲染线程已完成帧循环等待 gEvtSubmitDone，SetEvent 无效（不影响）。
    SetEvent(gEvtBeginFrame);
    // 等待本帧提交完成（10s 超时，避免永久阻塞）。
    DWORD r = WaitForSingleObject(gEvtSubmitDone, 10000);
    const double p98submitDoneMs = nowMs() - p98t0;
    if (r != WAIT_OBJECT_0) {
        // Shutdown path only (destroySurface): the run still tears down cleanly, so
        // this is a warning, not an error — otherwise every good session ends with a
        // scary line even at the default log level.
        dbgLogWarn("waitForRenderThreadSubmit: timeout (render thread may be stuck)");
    }
    // P48：submitDone 现在在 Present **之前**置位，这里再等 Present 真正完成，
    // 保证后续 destroySurface 的 swapchain 操作（ResizeBuffers/Release）不与
    // 并发的 Present 竞争。
    const double p98t1 = nowMs();
    waitForPendingPresents();
    const double p98presentsMs = nowMs() - p98t1;
    if (p98submitDoneMs > 30.0 || p98presentsMs > 30.0) {
        dbgLogInfo("P98 waitForRenderThreadSubmit: submitDone=%.1f ms (waitResult=%lu) pendingPresents=%.1f ms",
               p98submitDoneMs, static_cast<unsigned long>(r), p98presentsMs);
    }
}

// 形参 unnamed：workerCount 是 ABI 已经发布出去的参数（mcdr_dx12InitAsyncRenderer 的第一个
// 入参，名字保留在 mcdr_device.cppm 的声明上），但当前渲染线程只有一条，实现里用不到它。
// 不写名字即可避免 /W4 的 C4100 未引用形参，同时不改动 C ABI。
bool initAsyncRenderer(UINT) {
    if (auto devRes = ensureDevice(); !devRes) {
        dbgLogError("initAsyncRenderer: ensureDevice failed: %s", errorMessage(devRes.error()).c_str());
        return false;
    }
    if (gRenderRunning) return true;

    // P33 fix：gEvtBeginFrame 必须是 **auto-reset**（每帧一次的触发信号）。
    // 若用 manual-reset 且无人 Reset，渲染线程完成一帧后回到等待点会立刻再次通过
    // （信号仍有效），在主线程尚未 asyncRenderBeginFrame 的情况下自旋进入下一帧；
    // 此时 gAsyncRenderCtx 可能已被 asyncRenderWaitComplete 置空、或 ctx 已被
    // destroyCommandEncoder 释放，渲染线程随即在 waitForFenceValue 里解引用悬垂
    // 指针 → 0xC0000005（ACCESS_VIOLATION）。
    // 其余三个事件保持 manual-reset（广播语义：多个等待者需同时被唤醒）。
    gEvtBeginFrame = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gEvtRecordingReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gEvtCommandsReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gEvtSubmitDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!gEvtBeginFrame || !gEvtRecordingReady || !gEvtCommandsReady || !gEvtSubmitDone) {
        dbgLogError("initAsyncRenderer: CreateEvent failed");
        if (gEvtBeginFrame) CloseHandle(gEvtBeginFrame);
        if (gEvtRecordingReady) CloseHandle(gEvtRecordingReady);
        if (gEvtCommandsReady) CloseHandle(gEvtCommandsReady);
        if (gEvtSubmitDone) CloseHandle(gEvtSubmitDone);
        return false;
    }

    gAsyncRenderCtx = nullptr;
    // P48：重置 Present 护栏计数（submit / present 各一，保持平衡）。
    {
        std::lock_guard<std::mutex> lk(gPresentMtx);
        gSubmittedFrames = 0;
        gPresentedFrames = 0;
    }
    gRenderRunning = true;
    gRenderThread = std::thread([]() {
        DWORD ret = renderThreadFunc(nullptr);
        (void)ret;
    });

    dbgLogInfo("initAsyncRenderer: started render thread");
    return true;
}

void destroyAsyncRenderer() {
    if (!gRenderRunning) return;

    if (gRenderThread.joinable()) {
        // 先发信号让渲染线程从任何等待点苏醒（manual-reset events 持久有效）
        SetEvent(gEvtBeginFrame);
        SetEvent(gEvtRecordingReady);
        SetEvent(gEvtCommandsReady);
        SetEvent(gEvtSubmitDone);
        // 再设标志，线程在下一次循环检查时看到 false 并退出
        gRenderRunning = false;
        gRenderThread.join();
    } else {
        gRenderRunning = false;
    }

    if (gEvtBeginFrame) {
        CloseHandle(gEvtBeginFrame);
        gEvtBeginFrame = nullptr;
    }
    if (gEvtRecordingReady) {
        CloseHandle(gEvtRecordingReady);
        gEvtRecordingReady = nullptr;
    }
    if (gEvtCommandsReady) {
        CloseHandle(gEvtCommandsReady);
        gEvtCommandsReady = nullptr;
    }
    if (gEvtSubmitDone) {
        CloseHandle(gEvtSubmitDone);
        gEvtSubmitDone = nullptr;
    }
    gAsyncRenderCtx = nullptr;
    dbgLogInfo("destroyAsyncRenderer: done");
}

mcdr::Result<void> asyncRenderBeginFrame(CommandContext* ctx) {
    if (!gRenderRunning) {
        // 渲染线程未运行 → 本帧必然走 SYNC 路径，Java 侧 present() 必须真正
        // 执行 Present，因此清除上一帧遗留的所有权标记。
        gAsyncOwnsPresent = false;
        return mcdr::err("asyncRenderBeginFrame", "asyncRenderBeginFrame: renderer not running");
    }
    if (getActiveSurface() == nullptr) {
        // 初始化阶段无 surface（窗口未创建），回退到同步路径
        dbgLogInfo("asyncRenderBeginFrame: no active surface, fallback sync");
        gAsyncOwnsPresent = false;
        return mcdr::err("asyncRenderBeginFrame", "asyncRenderBeginFrame: no active surface, fallback sync");
    }
    if (gAsyncRenderCtx != nullptr) {
        return mcdr::err("asyncRenderBeginFrame", "asyncRenderBeginFrame: previous frame not complete");
    }
    gAsyncRenderCtx = ctx;
    // P33 fix：新一帧开始，撤销上一帧的 Present 所有权标记。本帧稍后由渲染线程
    // present 后重新置 true；若本帧走 SYNC fallback（不经过本函数），标记保持
    // false，Java 侧 present() 会正常执行真正的 Present。
    gAsyncOwnsPresent = false;
    // Reset events for this frame
    ResetEvent(gEvtRecordingReady);
    ResetEvent(gEvtCommandsReady);
    ResetEvent(gEvtSubmitDone);
    // Signal start
    SetEvent(gEvtBeginFrame);
    dbgLogDebug("asyncRenderBeginFrame: signaled beginFrame ctx=%p prevFence=%llu",
                reinterpret_cast<void*>(ctx), static_cast<unsigned long long>(ctx->prevFenceValue));
    return {};
}

mcdr::Result<void> asyncRenderWaitComplete(CommandContext* ctx, UINT64 timeoutMs) {
    (void)ctx;
    if (!gRenderRunning) {
        return mcdr::err("asyncRenderWaitComplete", "asyncRenderWaitComplete: renderer not running");
    }
    // 等待 SUBMIT_DONE
    DWORD r = WaitForSingleObject(gEvtSubmitDone, static_cast<DWORD>(timeoutMs));
    if (r == WAIT_OBJECT_0) {
        gAsyncRenderCtx = nullptr;
        // 每帧成功路径：级别降到 debug（INFO 下曾占 16k 行/45s，见 P78 的日志分级原则）。
        dbgLogDebug("asyncRenderWaitComplete: OK timeout=%llu", static_cast<unsigned long long>(timeoutMs));
        return {};
    }
    // The caller falls back to a synchronous path, so a timeout degrades rather than
    // breaks: warning, not error (and the JVM side logs its fallback at WARN too).
    dbgLogWarn("asyncRenderWaitComplete: TIMEOUT timeout=%llu", static_cast<unsigned long long>(timeoutMs));
    return mcdr::err("asyncRenderWaitComplete", "asyncRenderWaitComplete: timeout");
}

bool asyncRenderIsRecordingReady(CommandContext* ctx) {
    (void)ctx;
    if (!gRenderRunning) return false;
    // 检查 RECORDING_READY 是否已设置
    // 用 0 timeout 查询（非阻塞）
    DWORD r = WaitForSingleObject(gEvtRecordingReady, 0);
    return r == WAIT_OBJECT_0;
}

bool asyncRenderWaitRecordingReady(CommandContext* ctx, UINT64 timeoutMs) {
    (void)ctx;
    if (!gRenderRunning) return false;
    // 阻塞等待渲染线程发出 RECORDING_READY（每帧一开始就置位，正常为微秒级）。
    // 直接用 WaitForSingleObject 取代 Java 的 sleep(1) 轮询：休眠 1ms 会带来
    // 调度器/timer 粒度级别的固定开销，而本等待在主线程的帧关键路径上。
    DWORD r = WaitForSingleObject(gEvtRecordingReady, static_cast<DWORD>(timeoutMs));
    return r == WAIT_OBJECT_0;
}

bool isListOpen(CommandContext* ctx) { return ctx && ctx->listOpen; }

UINT currentDrawFrameSlot(CommandContext* ctx) {
    if (!ctx) return 0;
    return static_cast<UINT>(ctx->fenceValue % kDrawHeapSections);
}

void asyncSendCommandsReady(CommandContext* ctx) {
    (void)ctx;
    if (!gRenderRunning) return;
    SetEvent(gEvtCommandsReady);
}

void clearAsyncRenderCtx() { gAsyncRenderCtx = nullptr; }

}  // namespace d3d12_mc


