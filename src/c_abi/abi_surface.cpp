// abi_surface.cpp — DXGI swapchain surface: create/configure/acquire/blit/present, back-buffer handles, pixel readback
//
// Group: DXGI surface (originally abi_bridge_p5.cpp).
// 合并自：abi_bridge_p5.cpp
//
module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件
#include "mcdr_hdr_state.h"  // P198: 建链时实际用掉的 HDR 模式（普通 TU 的 C 状态）
#include "mcdr_rt_state.h"  // P117: 光追可见性分层总开关（普通 TU 的 C 状态）

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
// 来源单元：abi_bridge_p5.cpp
// ---------------------------------------------------------------------------

// abi_surface.cpp — DXGI swapchain surface entry points.（合并自 abi_bridge_p5.cpp）
//
// Replaces jni_bridge_p5.cpp (438 lines). Two things changed beyond the
// mechanical translation:
//
//  1. The two 3x3 pixel-readback functions were 150 lines of near-duplicate
//     staging/allocator/command-list setup between them. They now share one
//     file-local readbackSamples36() helper, which also means they share one
//     cached staging resource instead of two. The observable behaviour is
//     identical: same size guard, same 256-byte pitch alignment, same sampling
//     coordinates, same raw 0-255 byte values, same 36-int output layout.
//
//  2. Failures report through mcdrLastError() instead of returning a Java null
//     array; the diagnostic stderr lines the source project relied on are kept
//     verbatim so existing log captures still work.

namespace {

// 把被调方的 Result 失败原因折进本函数的纯文本诊断串（不带 "where: " 前缀，
// 下游 failWith 自己补前缀），成功则原样继续。readbackSamples36 的 err 出参
// 保持纯文本，是为了不改变这些既有诊断字符串的内容。
int32_t bindErr(const mcdr::Result<void>& r, std::string& err) {
    if (!r) {
        err = r.error().message;
        return 0;
    }
    return 1;
}

// Reads a 3x3 sample grid out of `res` and writes 9 pixels x RGBA as 36 ints.
// Values are the raw bytes (0-255), not normalised — matching the original.
int32_t readbackSamples36(ID3D12Resource* res, UINT w, UINT h, UINT bpp, DXGI_FORMAT fbFmt,
                          D3D12_RESOURCE_STATES finalState, int32_t* out, std::string& err) {
    DeviceContext& ctx = deviceContext();
    if (!ctx.device || !ctx.queue || !res) {
        err = "device not initialized";
        return 0;
    }
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        err = "bad dimensions";
        return 0;
    }
    if (const auto r = deviceWaitIdle(); !bindErr(r, err)) {
        return 0;
    }

    const UINT64 rowBytes = static_cast<UINT64>(w) * bpp;
    const UINT64 pitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                         & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 total = pitch * h;

    static ComPtr<ID3D12Resource> staging;
    static ComPtr<ID3D12CommandAllocator> alloc;
    static ComPtr<ID3D12GraphicsCommandList> cl;
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
        if (FAILED(ctx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&staging)))) {
            err = "readback staging create failed";
            return 0;
        }
    }
    if (!alloc) {
        ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    }
    if (!cl) {
        ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                      IID_PPV_ARGS(&cl));
    } else {
        alloc->Reset();
        cl->Reset(alloc.Get(), nullptr);
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = res;
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
    b.Transition.StateAfter = finalState;
    cl->ResourceBarrier(1, &b);
    const HRESULT closeHr = cl->Close();
    if (FAILED(closeHr)) {
        dbgLogError("readbackSamples36: Close hr=0x%08X fmt=%d %ux%u", static_cast<unsigned>(closeHr),
                    static_cast<int>(fbFmt), w, h);
    }

    ID3D12CommandList* lists[] = {cl.Get()};
    ctx.queue->ExecuteCommandLists(1, lists);
    const UINT64 fv = ++ctx.queueFenceValue;
    ctx.queue->Signal(ctx.queueFence.Get(), fv);
    if (const auto r = waitForQueueFenceValue(fv, 5'000'000'000ULL); !bindErr(r, err)) {
        return 0;
    }

    void* ptr = nullptr;
    if (FAILED(staging->Map(0, nullptr, &ptr))) {
        const HRESULT rr2 = ctx.device ? ctx.device->GetDeviceRemovedReason() : E_FAIL;
        dbgLogError("readbackSamples36: Map failed fmt=%d %ux%u removedReason=0x%08X", static_cast<int>(fbFmt),
                    w, h, static_cast<unsigned>(rr2));
        err = "staging Map failed";
        return 0;
    }
    const uint8_t* base = static_cast<const uint8_t*>(ptr);
    const int xs[3] = {0, static_cast<int>(w) / 2, static_cast<int>(w) - 1};
    const int ys[3] = {0, static_cast<int>(h) / 2, static_cast<int>(h) - 1};
    for (int yi = 0; yi < 3; ++yi) {
        for (int xi = 0; xi < 3; ++xi) {
            const uint8_t* p = base + static_cast<UINT64>(ys[yi]) * pitch + static_cast<UINT64>(xs[xi]) * bpp;
            const int o = (yi * 3 + xi) * 4;
            out[o + 0] = p[0];
            out[o + 1] = p[1];
            out[o + 2] = p[2];
            out[o + 3] = p[3];
        }
    }
    staging->Unmap(0, nullptr);
    return 1;
}

}  // namespace

extern "C" {

mcdr_surface* MCDR_CALL mcdr_dx12CreateSurface(uint64_t hwnd) {
    dbgLogDebug("dx12CreateSurface: hwnd=0x%llx (long=%lld)", static_cast<unsigned long long>(hwnd),
                static_cast<long long>(hwnd));
    const auto r = createSurface(static_cast<uintptr_t>(hwnd));
    if (!r) {
        return mcdr::abi::failWithPtr<Dx12Surface>("mcdr_dx12CreateSurface", r.error().message);
    }
    return static_cast<Dx12Surface*>(*r);
}

int32_t MCDR_CALL mcdr_dx12SurfacePresentModes(int32_t* out, int32_t cap) {
    // Returns the TOTAL number of modes so a caller can size-then-retry; only
    // min(cap, total) entries are written.
    const std::vector<int> modes = surfacePresentModes();
    const int32_t total = static_cast<int32_t>(modes.size());
    if (out) {
        const int32_t n = cap < total ? cap : total;
        for (int32_t i = 0; i < n; ++i) {
            out[i] = modes[static_cast<size_t>(i)];
        }
    }
    return total;
}

int32_t MCDR_CALL mcdr_dx12ConfigureSurface(mcdr_surface* surface, int32_t width, int32_t height,
                                            int32_t presentMode) {
    if (const auto r = configureSurface(surface, width, height, presentMode); !r) {
        // A failed configure is exactly the surfaceIsInvalid -> frozen-picture
        // root cause, so it must reach dx12-native.log rather than only stderr.
        d3d12_mc::dbgLogError("dx12ConfigureSurface FAILED %dx%d mode=%d: %s", width, height, presentMode,
                         r.error().message.c_str());
        return mcdr::abi::failWith("mcdr_dx12ConfigureSurface", r.error().message);
    }
    d3d12_mc::dbgLogInfo("dx12ConfigureSurface: ok %dx%d mode=%d", width, height, presentMode);
    return 1;
}

// P196：真独占全屏开关。26.3 的 SDL 独占只改显示模式，DXGI 侧需要显式 SetFullscreenState。
int32_t MCDR_CALL mcdr_dx12SetExclusiveFullscreen(mcdr_surface* surface, int32_t on) {
    if (const auto r = setExclusiveFullscreen(surface, on != 0); !r) {
        // 非致命：驱动/覆盖层拒绝时画面仍可走窗口化，记 WARN 即可，不要升级成 ERROR。
        d3d12_mc::dbgLogWarn("dx12SetExclusiveFullscreen(%d) failed: %s", on ? 1 : 0,
                             r.error().message.c_str());
        return 0;
    }
    return 1;
}

int32_t MCDR_CALL mcdr_dx12AcquireSurface(mcdr_surface* surface) {
    if (const auto r = acquireSurface(surface); !r) {
        return mcdr::abi::failWith("mcdr_dx12AcquireSurface", r.error().message);
    }
    // Once per frame: an unconditional stderr + file flush here was a real cost,
    // so it is DEBUG level (needs DX12_LOG_VERBOSE=1).
    d3d12_mc::dbgLogDebug("acquireSurface: ok surface=%p", static_cast<void*>(surface));
    return 1;
}

void MCDR_CALL mcdr_dx12BlitSurface(mcdr_command_ctx* ctx, mcdr_surface* surface, mcdr_object* texture) {
    if (const auto r = blitSurface(ctx, surface, texture); !r) {
        // P95 可观测性：blit 失败时 surface->blittedThisFrame 仍为 false，presentSurface()
        // 会跳过这一帧的 Present（不会 flip 到未写入的 back buffer）。failVoid 只把原因写进
        // lastError / [mcdr] sink，这里额外按次数在 [dx12] sink 打点，便于把"跳帧"与
        // "blit 失败"在日志里对上（两个 sink 分开写，只 grep 一个会得到"没有错误"的假象）。
        static unsigned long long blitFailures = 0;
        ++blitFailures;
        mcdr::abi::failVoid("mcdr_dx12BlitSurface", r.error().message);
        if (blitFailures <= 5 || blitFailures % 60 == 0) {
            d3d12_mc::dbgLogWarn(
                "dx12BlitSurface: failure #%llu — present will be skipped for this frame",
                blitFailures);
        }
        return;
    }
    // 每帧成功路径：级别降到 debug（INFO 下曾占 16k 行/45s）。
    d3d12_mc::dbgLogDebug("blitSurface: ok ctx=%p surface=%p texture=%p", static_cast<void*>(ctx),
                         static_cast<void*>(surface), static_cast<void*>(texture));
}

void MCDR_CALL mcdr_dx12PresentSurface(mcdr_surface* surface) {
    // P33 + P61 duplicate-Present fix. Under the async path the render thread has
    // already called presentSurface() after submitCommandList and set
    // gAsyncOwnsPresent; a second Present flips the swapchain twice, so one
    // frame's back buffer is never rendered into and the window flickers colours.
    // Checking gAsyncOwnsPresent alone is not enough (P61): the Java side no longer
    // blocks on the render thread after submit(), so GpuSurface.present() can run
    // before that flag is set, and the main thread's Present would then race the
    // render thread's ExecuteCommandLists/Present on the same swapchain ->
    // DEVICE_REMOVED 0x887A0005. gAsyncRenderCtx is set by asyncRenderBeginFrame
    // and cleared only by the next frame's settle, so a non-null value proves the
    // render thread owns this frame's Present.
    if (gAsyncOwnsPresent || gAsyncRenderCtx != nullptr) {
        gAsyncOwnsPresent = false;
        d3d12_mc::dbgLogDebug("dx12PresentSurface: skipped (async render thread owns present) surface=%p",
                              static_cast<void*>(surface));
        return;
    }
    presentSurface(surface);
    // The real result (ok / OCCLUDED / MODE_CHANGED / FAILED) is logged inside
    // presentSurface(); logging "ok" here unconditionally used to mask an
    // OCCLUDED/MODE_CHANGED result, which is what made MC misjudge suboptimal.
}

uint64_t MCDR_CALL mcdr_dx12GetBackBufferHandle(mcdr_surface* surface) {
    return static_cast<uint64_t>(getBackBufferHandle(surface));
}

int32_t MCDR_CALL mcdr_dx12ReadbackTexturePixels(mcdr_object* texture, int32_t* out) {
    if (!out) {
        return mcdr::abi::failWith("mcdr_dx12ReadbackTexturePixels", "out buffer is null");
    }
    DeviceContext& ctx = deviceContext();
    Dx12Object* tex = texture;
    if (!ctx.device || !ctx.queue || !tex || tex->kind != Dx12Object::Kind::Texture || !tex->resource) {
        return mcdr::abi::failWith(
            "mcdr_dx12ReadbackTexturePixels",
            "invalid handle (kind=" + std::to_string(tex ? static_cast<int>(tex->kind) : -1) + ")");
    }
    ID3D12Resource* r = tex->resource.Get();
    const D3D12_RESOURCE_DESC td = r->GetDesc();
    const UINT w = static_cast<UINT>(td.Width);
    const UINT h = static_cast<UINT>(td.Height);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        return mcdr::abi::failWith("mcdr_dx12ReadbackTexturePixels",
                                   "bad dims " + std::to_string(w) + "x" + std::to_string(h));
    }
    UINT bpp = 4;
    DXGI_FORMAT fbFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (td.Format) {
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
        default:
            bpp = 4;
            fbFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
    }
    // P235：1 字节格式（R8_*）在原实现里落进 default 分支（bpp=4 + RGBA8 足迹），
    // 拷贝足迹与源格式不兼容 ⇒ CopyTextureRegion 判非法 ⇒ 设备被移除 ⇒ staging 的
    // Map 失败（2026-10-25 SRI 自检实测）。只给 R8 家族开一条专用分支：
    // bpp=1、足迹格式 = 源格式。其余格式保持原有映射不动 —— 改成"一律用源格式"
    // 会让 compute 自检的 RGBA8 读回变成全 0（同一实测轮次），原因未查明，不冒险。
    if (td.Format == DXGI_FORMAT_R8_UNORM || td.Format == DXGI_FORMAT_R8_UINT
        || td.Format == DXGI_FORMAT_R8_SNORM || td.Format == DXGI_FORMAT_R8_SINT) {
        bpp = 1;
        fbFmt = td.Format;
    }
    std::string err;
    if (!readbackSamples36(r, w, h, bpp, fbFmt, D3D12_RESOURCE_STATE_COMMON, out, err)) {
        return mcdr::abi::failWith("mcdr_dx12ReadbackTexturePixels", err);
    }
    // Grid index of the centre sample is (yi=1, xi=1) -> element offset (1*3+1)*4 = 16.
    constexpr int kCenter = (1 * 3 + 1) * 4;
    dbgLogInfo("[dx12-java] rbTex %ux%u center=(%d,%d) RGBA=(%d,%d,%d,%d) black=%d", w, h,
               static_cast<int>(w) / 2, static_cast<int>(h) / 2, out[kCenter + 0], out[kCenter + 1],
               out[kCenter + 2], out[kCenter + 3],
               (out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0) ? 1 : 0);
    return 1;
}

void MCDR_CALL mcdr_dx12DumpTextureToFile(mcdr_object* texture, const char* tag) {
    // Reuses dbgReadbackTexturePixels (waitIdle + its own command list +
    // dbgDumpPixelsToFile writing dx12_dump_<tag>.bmp plus an ASCII thumbnail).
    // The texture must be in COMMON state (i.e. outside a render pass) or the
    // barrier validation fails.
    dbgReadbackTexturePixels(texture, (tag && *tag) ? tag : "tex");
}

int32_t MCDR_CALL mcdr_dx12IsSurfaceSuboptimal(mcdr_surface* surface) {
    return (surface && surface->suboptimal) ? 1 : 0;
}

void MCDR_CALL mcdr_dx12SetHdrMode(int32_t mode) { setHdrMode(mode); }

// P117: 光追可见性分层总开关。此刻只做状态存储与日志，G-Buffer/采样的创建在 device 侧。
void MCDR_CALL mcdr_dx12SetRaytracingMode(int32_t mode) {
    mcdr_rt_state_set_mode(mode);
    dbgLog("[P117] raytracing mode=%d (0=off 1=ao 2=ao_shadows 3=full)", static_cast<int>(mode));
}

// P198: 建链时实际用掉的 HDR 模式（-1 = 还没建过链），JVM 侧据此提示“需要重启”。
int32_t MCDR_CALL mcdr_dx12GetHdrAppliedMode(void) { return mcdr_hdr_state_applied_mode(); }

void MCDR_CALL mcdr_dx12DestroySurface(mcdr_surface* surface) { destroySurface(surface); }

mcdr_surface* MCDR_CALL mcdr_dx12GetActiveSurfaceHandle(void) { return getActiveSurface(); }

int32_t MCDR_CALL mcdr_dx12ReadbackSurfacePixels(mcdr_surface* surface, int32_t* out) {
    if (!out) {
        return mcdr::abi::failWith("mcdr_dx12ReadbackSurfacePixels", "out buffer is null");
    }
    DeviceContext& ctx = deviceContext();
    Dx12Surface* s = surface;
    if (!ctx.device || !ctx.queue || !s || s->backBuffers.empty()) {
        return mcdr::abi::failWith("mcdr_dx12ReadbackSurfacePixels", (!ctx.device || !ctx.queue)
                                                                         ? "device not initialized"
                                                                         : "surface has no back buffers");
    }
    int idx = s->currentImageIndex;
    if (idx < 0 || idx >= static_cast<int>(s->backBuffers.size())) {
        // present() resets currentImageIndex to -1, and GetCurrentBackBufferIndex()
        // would then report the NEXT frame's buffer, which has not been written
        // yet (an all-zero false black screen) — hence the lastBlitIndex fallback.
        if (s->lastBlitIndex >= 0 && s->lastBlitIndex < static_cast<int>(s->backBuffers.size())) {
            idx = s->lastBlitIndex;
        } else {
            idx = static_cast<int>(s->swapChain->GetCurrentBackBufferIndex());
        }
    }
    if (idx < 0 || idx >= static_cast<int>(s->backBuffers.size())) {
        return mcdr::abi::failWith("mcdr_dx12ReadbackSurfacePixels",
                                   "no valid back buffer (idx=" + std::to_string(idx) + ")");
    }
    ID3D12Resource* bb = s->backBuffers[static_cast<size_t>(idx)].Get();
    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    const UINT w = static_cast<UINT>(bd.Width);
    const UINT h = bd.Height;
    dbgLogDebug("readback surf=0x%p idx=%d w=%u h=%u", static_cast<void*>(s), idx, w, h);
    std::string err;
    // The back buffer is always R8G8B8A8_UNORM, and after the copy it must go back
    // to PRESENT (not COMMON) or the next Present fails.
    if (!readbackSamples36(bb, w, h, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PRESENT, out, err)) {
        return mcdr::abi::failWith("mcdr_dx12ReadbackSurfacePixels", err);
    }
    dbgLogInfo("[dx12-java] readback %ux%u center=(%d,%d)=(%d,%d,%d,%d) corners=%d black=%d", w,
               h, static_cast<int>(w) / 2, static_cast<int>(h) / 2, out[(4 * 3 + 4) + 0],
               out[(4 * 3 + 4) + 1], out[(4 * 3 + 4) + 2], out[(4 * 3 + 4) + 3],
               (out[0] != 0 || out[1] != 0 || out[2] != 0) ? 1 : 0,
               (out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0) ? 1 : 0);
    return 1;
}

}  // extern "C"
