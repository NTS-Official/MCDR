module;

#include "_d3d12_module_prelude.h"
#include "mcdr_dlss_impl.h"  // P113：mcdr_reflex_before_present
#include "mcdr_hdr_state.h"  // P197：HDR 模式/激活状态（surface 与 DLSS 两个 TU 共享）
#include "mcdr_surface_opts_state.h"  // P251：allowTearing / presentChild（配置推来）
#include "mcdr_window_host.h"  // P267: SeamlessWindowHost
#include <algorithm>          // P197：std::clamp
#include <cwctype>            // P252：towlower（模块名小写化）
#include <tlhelp32.h>         // P252：Module32First / 进程模块快照

module mcdr.d3d12.device;

import mcdr.error;

// dx12-mc 原生 D3D12 层（C++）
// P5 目标：DXGI swapchain（GpuSurfaceBackend 对应物）。
// 设计参考：官方 com.mojang.blaze3d.vulkan.VulkanGpuSurface。
//
// PresentMode 序数 = 官方枚举 ordinal：
//   IMMEDIATE=0, MAILBOX=1, FIFO=2, FIFO_RELAXED=3
// DXGI FLIP 模型支持 {IMMEDIATE, FIFO, FIFO_RELAXED}（MAILBOX 无直接对应）。

namespace d3d12_mc {

namespace {

std::string hrText(HRESULT hr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// 把当前线程消息队列抽干 ms 毫秒。CreateSwapChainForHwnd 返回 E_ACCESSDENIED
// (0x80070005) 时多为瞬态：窗口刚由 GLFW 创建、DWM 尚未完成重定向，或被第三方
// 覆盖层/输入法短暂占用。泵消息 + 短等待后重试即可成功（本机实测偶发，重试必成）。
void pumpWindowMessages(int ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(ms > 0 ? ms : 0);
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetTickCount() >= deadline) break;
        Sleep(8);
    }
}

// swapchain 创建结果（含 DirectComposition 回退资源）。
struct SwapChainBundle {
    ComPtr<IDXGISwapChain1> swapChain;
    bool composition = false;   // true = 由 CreateSwapChainForComposition 创建
    bool allowTearing = false;  // 仅 HWND + ALLOW_TEARING tier 为 true
    ComPtr<IDCompositionDevice> dcompDevice;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> dcompVisual;
};

// 动态解析 dcomp.dll 的 DCompositionCreateDevice（避免链接期依赖 dcomp.lib）。
typedef HRESULT(WINAPI* PFN_DCompositionCreateDevice)(IUnknown*, REFIID, void**);
PFN_DCompositionCreateDevice resolveDCompCreateDevice() {
    static PFN_DCompositionCreateDevice fn = []() -> PFN_DCompositionCreateDevice {
        HMODULE mod = LoadLibraryW(L"dcomp.dll");
        if (!mod) return nullptr;
        return reinterpret_cast<PFN_DCompositionCreateDevice>(
            GetProcAddress(mod, "DCompositionCreateDevice"));
    }();
    return fn;
}

// P248/P249：DXGI 工厂统一走原生 CreateDXGIFactory1，**再**把它升级成 SL 的代理工厂。
// 从 P248 起 slInit 置了 `sl::PreferenceFlags::eUseDXGIFactoryProxy`（见 d3d12_device.cpp）：
// 这个模式下 SL 不改写工厂/交换链的 v-table，而是要求调用方拿着它给的代理工厂建链
// （ProgrammingGuideManualHooking.md:190-207；sl_core_api.h:265-275 要求接口创建后**立刻**
// 升级）。因此这里不能再 LoadLibrary sl.interposer.dll 取它的 CreateDXGIFactory1 —— 那正是
// 被这个 flag 取代的 v-table 集成方式，会让 Present 槽解析到 sl.interposer.dll，注入式
// overlay（RTSS/Afterburner）挂不上 OSD。
//
// 曾经漏掉「升级」这一步的后果（P249 用户 m03032 报「开了帧生成但没生效」）：链是纯原生的，
// SL 的 slHookPresent/CreateSwapChainForHwnd 钩子虽然装上了却一次都触发不到 ——
// `slDLSSGSetOptions` 返回 eOk、`slDLSSGGetState` 也 Ok，但 numFramesActuallyPresented 恒 0，
// 且 sl.log 报 `Streamline presentCommon() was not observed`。

// 统一工厂入口：原生创建 + 立刻升级成代理（SL 未启用时升级是空操作）。
template <typename T>
HRESULT createDxgiFactory(ComPtr<T>& factory) {
    const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    void* raw = factory.Get();
    if (mcdr_sl_upgrade_interface(&raw) != 0 && raw != nullptr) {
        // 成功时是 replace in place：原生那一个引用已经变成代理的引用，Attach 不 AddRef。
        factory.Attach(static_cast<T*>(raw));
    }
    return hr;
}

// 解绑/绑定 composition visual 的内容。DirectComposition 会持有 swapchain 的
// backbuffer 引用，因此在 ResizeBuffers / 释放 swapchain 前必须先
// SetContent(nullptr) + Commit，否则 ResizeBuffers 返回 DXGI_ERROR_INVALID_CALL。
mcdr::Result<void> setCompositionContent(IDCompositionVisual* visual, IDCompositionDevice* dev,
                                        IUnknown* content) {
    if (!visual || !dev) {
        return mcdr::err("setCompositionContent", "composition visual/device null");
    }
    HRESULT hr = visual->SetContent(content);
    if (FAILED(hr)) {
        return mcdr::err("setCompositionContent", "IDCompositionVisual::SetContent failed " + hrText(hr));
    }
    hr = dev->Commit();
    if (FAILED(hr)) {
        return mcdr::err("setCompositionContent", "IDCompositionDevice::Commit failed " + hrText(hr));
    }
    return {};
}

// 为本 HWND 建立 DirectComposition 合成链（device/target/visual）。
mcdr::Result<void> initComposition(HWND win, SwapChainBundle& b) {
    PFN_DCompositionCreateDevice createDevice = resolveDCompCreateDevice();
    if (!createDevice) {
        return mcdr::err("initComposition", "dcomp.dll / DCompositionCreateDevice unavailable");
    }
    HRESULT hr = createDevice(nullptr, __uuidof(IDCompositionDevice),
                              reinterpret_cast<void**>(b.dcompDevice.GetAddressOf()));
    if (FAILED(hr)) {
        return mcdr::err("initComposition", "DCompositionCreateDevice failed " + hrText(hr));
    }
    hr = b.dcompDevice->CreateTargetForHwnd(win, TRUE, b.dcompTarget.GetAddressOf());
    if (FAILED(hr)) {
        return mcdr::err("initComposition",
                         "IDCompositionDevice::CreateTargetForHwnd failed " + hrText(hr));
    }
    hr = b.dcompDevice->CreateVisual(b.dcompVisual.GetAddressOf());
    if (FAILED(hr)) {
        return mcdr::err("initComposition", "IDCompositionDevice::CreateVisual failed " + hrText(hr));
    }
    hr = b.dcompTarget->SetRoot(b.dcompVisual.Get());
    if (FAILED(hr)) {
        return mcdr::err("initComposition", "IDCompositionTarget::SetRoot failed " + hrText(hr));
    }
    return {};
}

// 创建 composition swapchain 并把内容绑定到 visual 上。
mcdr::Result<void> createCompositionSwapChain(IDXGIFactory2* factory2, ID3D12CommandQueue* queue, UINT width,
                                             UINT height, DXGI_FORMAT format, SwapChainBundle& b) {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width > 0 ? width : 1;
    sd.Height = height > 0 ? height : 1;
    sd.Format = format;
    sd.Stereo = FALSE;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kSurfaceBufferCount;
    sd.Scaling = DXGI_SCALING_STRETCH;                 // composition 仅支持 STRETCH
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;  // composition 支持 SEQUENTIAL/DISCARD
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;      // composition 不接受 UNSPECIFIED
    sd.Flags = 0;                                      // composition 不支持 ALLOW_TEARING
    HRESULT hr = factory2->CreateSwapChainForComposition(queue, &sd, nullptr, &b.swapChain);
    if (FAILED(hr)) {
        return mcdr::err("createCompositionSwapChain",
                         "CreateSwapChainForComposition failed " + hrText(hr));
    }
    auto bind = setCompositionContent(b.dcompVisual.Get(), b.dcompDevice.Get(), b.swapChain.Get());
    if (!bind) return bind;
    b.composition = true;
    b.allowTearing = false;
    return {};
}

// ---------------------------------------------------------------------------
// 呈现子窗口（RTSS/OSD 修复）。
//
// 背景：GLFW 主窗口一旦通过 GL 呈现过至少一帧（SwapBuffers），DXGI 就再也不会
// 接受在该 HWND 上创建 flip-model swapchain —— CreateSwapChainForHwnd 对所有
// tier / 描述组合都返回 E_ACCESSDENIED (0x80070005)。这不是瞬态错误，而是
// "该 HWND 的呈现权已经归 GL"的持久状态（实测脚本 _port\scprobe2.cpp：GL 上下文
// 只创建不呈现 → S_OK；呈现过 5 帧 → 所有变体全 80070005）。
//
// 于是给呈现单独造一个子窗口：没有 GL 上下文、窗口类不含 CS_OWNDC，因此可以
// 正常建立 HWND swapchain（_port\scprobe3.cpp：父窗口呈现过 GL 后，子窗口仍
// S_OK）。除能呈现外，这条路径还让 OSD 注入器（RTSS / MSI Afterburner）能从
// IDXGISwapChain1::GetDesc1().OutputWindow 拿到非空句柄，从而把覆盖层画进我们的
// back buffer —— composition swapchain 的 OutputWindow 是空的，RTSS 因此无法
// 定位 OSD（用户报告的"overlay 无法正确渲染"）。
// ---------------------------------------------------------------------------
constexpr wchar_t kPresentWindowClass[] = L"MCDR_D3D12_PresentChild";

// P96（启动/退出"白幕"）：把窗口客户区刷成黑色。
// 背景：GLFW 主窗口自身从不接收任何内容 —— D3D12 的呈现全部发生在上面那个自建子窗口
// （见前文说明），而子窗口的类背景刷是 nullptr 且 WM_ERASEBKGND 返回 1（故意不擦），
// 于是两个"没有任何呈现"的窗口期里，DWM 显示的是**未绘制的白色表面**：
//   - 启动：窗口创建/显示之后、第一帧 Present 之前（数字捕获实测 6 帧近白：ours1 idx
//     408-413，白像素占客户区 94.7%）；
//   - 退出：子窗口被销毁、而 GLFW 主窗口尚未销毁的瞬间（实测 ours1 idx 1880-1881，
//     白像素占整个捕获矩形 99.9% = 用户报告的"退出游戏时出现一个画幅尺寸为启动时
//     窗口尺寸的白幕"）。
// 我们没有能力把 swapchain 的画面搬回 GDI 表面，所以这两个时刻用黑幕代替白幕：
// 黑色是安全默认值（不刺眼、不误导为"画面还在"）。
static void paintWindowBlack(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) return;
    HDC dc = GetDC(hwnd);
    if (dc == nullptr) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    ReleaseDC(hwnd, dc);
}

// 销毁呈现子窗口前调用：子窗口消失后暴露出来的是父窗口（GLFW）从未绘制过的客户区
// （白色）。先把父窗口刷黑，退出时看到的就是黑幕而不是白幕。
static void paintParentBlackBeforeDestroy(HWND child) {
    if (child == nullptr) return;
    paintWindowBlack(GetParent(child));
}

// P96 诊断（临时）：把窗口/子窗口的几何、可见性与"类背景刷"打进日志。
// 用途：定位"退出游戏时出现一个画幅尺寸为启动时窗口尺寸的白幕"的来源 ——
// 白幕只可能是 (a) 父窗口从未绘制的客户区、(b) 类背景刷被系统擦出来的颜色、
// (c) 窗口在被销毁前后被 resize 成别的尺寸导致捕获矩形整块落在客户区里。
static void logWindowState(const char* tag, HWND hwnd) {
    if (hwnd == nullptr) {
        dbgLogInfo("P96 %s: hwnd=null", tag);
        return;
    }
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);
    auto brush = reinterpret_cast<void*>(GetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND));
    dbgLogInfo("P96 %s: hwnd=%p cls=%ls rect=%ld,%ld %ldx%ld visible=%d iconic=%d classBrush=%p", tag,
               reinterpret_cast<void*>(hwnd), cls, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
               static_cast<int>(IsWindowVisible(hwnd)), static_cast<int>(IsIconic(hwnd)), brush);
}

// 呈现子窗口的消息处理。三条都必须有，否则子窗口会破坏游戏输入/画面：
//   - WM_NCHITTEST == HTTRANSPARENT：WS_EX_TRANSPARENT 只影响绘制顺序，不参与命中
//     测试；子窗口铺满客户区时会把鼠标消息全部吃掉，表现为「游戏界面点不动」。
//     返回 HTTRANSPARENT 后点击/滚轮穿透到父窗口（GLFW），原版输入路径不受影响。
//   - WM_ERASEBKGND == 1：不擦背景。父窗口 resize 时本窗口会被 SetWindowPos 调整尺寸，
//     若让系统用背景刷子擦一遍，就会在 swapchain 重新出画前整片闪黑（用户报告的
//     「窗口 resize 后直接变为黑屏」）。
//   - WM_MOUSEACTIVATE == MA_NOACTIVATE：点击穿透时不要把激活焦点从父窗口抢走。
LRESULT CALLBACK presentationWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

HWND createPresentationWindow(HWND parent, UINT width, UINT height) {
    if (!parent) return nullptr;
    static const bool registered = []() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;  // 关键：不能带 CS_OWNDC
        wc.lpfnWndProc = presentationWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 不设背景刷子：本窗口的内容只来自 swapchain，任何系统擦除都会先露出黑底
        // （配合 WM_ERASEBKGND 返回 1 一起避免 resize 期间闪黑）。
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kPresentWindowClass;
        if (RegisterClassExW(&wc)) return true;
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered) return nullptr;
    const int w = width > 0 ? static_cast<int>(width) : 1;
    const int h = height > 0 ? static_cast<int>(height) : 1;
    // WS_EX_TRANSPARENT：命中最上层窗口判定跳过本窗口，鼠标消息仍由父窗口（GLFW）
    // 接收，否则子窗口铺满客户区会把游戏输入全吃掉。
    HWND child = CreateWindowExW(WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY, kPresentWindowClass,
                                 L"MCDR D3D12 present", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, w,
                                 h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!child) return nullptr;
    // P96：子窗口在第一次 Present 之前是一块未绘制的白面（类背景刷为 nullptr、
    // WM_ERASEBKGND 返回 1），父窗口客户区同样从未被绘制过 —— 先都刷黑，
    // 让"启动到第一帧"这段窗口期显示黑幕而不是白幕。
    paintWindowBlack(child);
    paintWindowBlack(parent);
    SetWindowPos(child, HWND_TOP, 0, 0, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return child;
}

void resizePresentationWindow(HWND child, UINT width, UINT height) {
    if (!child || width == 0 || height == 0) return;
    SetWindowPos(child, HWND_TOP, 0, 0, static_cast<int>(width), static_cast<int>(height),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RECT rc{};
    GetWindowRect(child, &rc);
    dbgLogDebug("resizePresentationWindow: child=%p -> %ux%u (visible=%d rect=%dx%d)",
           reinterpret_cast<void*>(child), width, height, IsWindowVisible(child) ? 1 : 0,
           static_cast<int>(rc.right - rc.left), static_cast<int>(rc.bottom - rc.top));
}

// 为窗口创建 swapchain：
//   1) 首选 CreateSwapChainForHwnd：多 tier（FLIP_DISCARD±TEARING / FLIP_SEQUENTIAL）
//      并重试 3 轮（E_ACCESSDENIED 多为瞬态）；
//   2) 全部被拒时回退 CreateSwapChainForComposition + DirectComposition（不绑定 HWND）。
// reuse 非空且已是 composition 模式时直接复用其 composition 对象重建——对同一 HWND
// 重复 CreateTargetForHwnd 会返回 DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED。
// P252：把进程里所有 d3d12* / dxgi / RTSS / SL / nvngx 模块连同基址打出来。
// RTSS 的 FnOffsetCache（Profiles\Config 的 [FnOffsetCache64]）是按 "D3D12Core.DLL 的偏移"
// 解析 ID3D12CommandQueue::ExecuteCommandLists 的；进程里若同时存在 inbox core
// （system32\d3d12core.dll，10.0.26100.x）和 Agility core（dxrmc\D3D12Core.dll，1.619.x），
// 它可能把 hook 挂到没在用的那个上 —— OSD 于是整体失效。dev（OSD 正常）与用户 Prism
// 实例（OSD 不来）各跑一次，对比这几行就能定案。
static void logOverlayModules(const char* tag) {
    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (::Module32FirstW(snap, &me) != 0) {
        do {
            std::wstring lower(me.szModule);
            for (auto& c : lower) c = static_cast<wchar_t>(::towlower(c));
            if (lower.find(L"d3d12") == std::wstring::npos &&
                lower.find(L"dxgi") == std::wstring::npos &&
                lower.find(L"rtss") == std::wstring::npos &&
                lower.find(L"interposer") == std::wstring::npos &&
                lower.find(L"nvngx") == std::wstring::npos) {
                continue;
            }
            dbgLog("%s module %ls base=%p size=%lu path=%ls", tag, me.szModule,
                   reinterpret_cast<void*>(me.modBaseAddr), static_cast<unsigned long>(me.modBaseSize),
                   me.szExePath);
        } while (::Module32NextW(snap, &me) != 0);
    }
    ::CloseHandle(snap);
}

// RTSS/Afterburner 等 overlay 通过改写交换链 vtable 的 Present 槽（索引 8）来挂 OSD。
// 该槽若仍解析到 system32\dxgi.dll，说明本进程这条交换链根本没被 hook —— 这是
// "RTSS OSD 不显示" 的决定性证据；若解析到 RTSSHooks64.dll，说明 hook 生效，问题在别处。
static void logSwapChainPresentHook(IDXGISwapChain* sc) {
    if (sc == nullptr) return;
    void** vtbl = *reinterpret_cast<void***>(sc);
    if (vtbl == nullptr) return;
    void* present = vtbl[8];
    if (present == nullptr) return;
    HMODULE owner = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(present), &owner);
    std::wstring ownerPath(MAX_PATH, L'\0');
    const DWORD n = owner != nullptr ? ::GetModuleFileNameW(owner, ownerPath.data(), MAX_PATH) : 0;
    ownerPath.resize(n);
    std::wstring_view sv(ownerPath);
    const size_t slash = sv.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) sv.remove_prefix(slash + 1);
    const auto* b = reinterpret_cast<const unsigned char*>(present);
    dbgLog("swapchain Present slot=%p owner=%ls bytes=%02X %02X %02X %02X",
           present, sv.empty() ? L"(unknown)" : std::wstring(sv).c_str(),
           b[0], b[1], b[2], b[3]);

    // P250：把「RTSS 到底注入没注入这个进程」和「链长什么样」一起记下来，否则
    // "owner=dxgi.dll" 既可能是没注入、也可能是注入了但没挂到这条链上，分不开。
    const HMODULE rtssHooks = ::GetModuleHandleW(L"RTSSHooks64.dll");
    const HMODULE rtssLoader = ::GetModuleHandleW(L"RTSSHooksLoader64.dll");
    DXGI_SWAP_CHAIN_DESC desc{};
    const bool haveDesc = SUCCEEDED(sc->GetDesc(&desc));
    dbgLog("P250 overlay probe: RTSSHooks64=%p RTSSHooksLoader64=%p desc.ok=%d OutputWindow=%p "
           "Format=%d BufferCount=%u SwapEffect=%d Windowed=%d Refresh=%u/%u",
           reinterpret_cast<void*>(rtssHooks), reinterpret_cast<void*>(rtssLoader), haveDesc ? 1 : 0,
           haveDesc ? reinterpret_cast<void*>(desc.OutputWindow) : nullptr,
           haveDesc ? static_cast<int>(desc.BufferDesc.Format) : -1,
           haveDesc ? desc.BufferCount : 0u, haveDesc ? static_cast<int>(desc.SwapEffect) : -1,
           haveDesc ? (desc.Windowed ? 1 : 0) : -1,
           haveDesc ? desc.BufferDesc.RefreshRate.Numerator : 0u,
           haveDesc ? desc.BufferDesc.RefreshRate.Denominator : 0u);
    logOverlayModules("P252");
}

// P197：查某显示器的「SDR 白」亮度（nits）。DXGI_OUTPUT_DESC1 里没有这个值 ——
// 它属于 display config（DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL），
// 单位是 1/1000 nits（Windows HDR 滑块 50% 时约 200000 → 200 nits）。
// 先按 HMONITOR 取 GDI 设备名，再用 QueryDisplayConfig 把 source 与 target 对上。
float querySdrWhiteNits(HMONITOR monitor) {
    if (monitor == nullptr) return 0.0f;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return 0.0f;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        return 0.0f;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        return 0.0f;
    }
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) return 0.0f;
        return static_cast<float>(white.SDRWhiteLevel) / 1000.0f;
    }
    return 0.0f;
}

// P197（原生 HDR）：窗口所在输出的高级颜色状态。
// 只信 DXGI：SDL 的 `SDL_PROP_WINDOW_HDR_ENABLED` / `SDR_WHITE_LEVEL` 在 Windows 上
// 也是从同一份 DXGI 输出信息推出来的，而我们本来就要拿 IDXGIOutput6 判 scRGB 支持。
struct HdrProbe {
    bool hdrOn = false;      // Windows 高级颜色已开启（输出色彩空间是 PQ/HDR10）
    float sdrWhiteNits = 200.0f;  // 系统「SDR 内容亮度」，scRGB 换算的分母
    float maxLuminanceNits = 0.0f;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool found = false;      // 是否成功定位到输出（false 时上面的值都是默认）
};

HdrProbe probeHdrForWindow(IDXGIFactory4* factory, HWND win) {
    HdrProbe probe{};
    if (factory == nullptr || win == nullptr) return probe;

    // 窗口客户区中心点落在哪块输出上 —— 多显示器下这是"这个窗口现在在哪块屏"的判据。
    POINT center{0, 0};
    RECT cr{};
    if (GetClientRect(win, &cr)) {
        center.x = (cr.right - cr.left) / 2;
        center.y = (cr.bottom - cr.top) / 2;
        ClientToScreen(win, &center);
    }

    ComPtr<IDXGIOutput> matched;
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(factory->EnumAdapters(a, adapter.GetAddressOf()))) break;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(o, output.GetAddressOf()))) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;
            const RECT& r = desc.DesktopCoordinates;
            if (center.x >= r.left && center.x < r.right && center.y >= r.top && center.y < r.bottom) {
                matched = output;
                break;
            }
        }
        if (matched) break;
    }
    if (!matched) return probe;

    ComPtr<IDXGIOutput6> output6;
    if (FAILED(matched.As(&output6))) return probe;

    DXGI_OUTPUT_DESC1 desc1{};
    if (FAILED(output6->GetDesc1(&desc1))) return probe;

    probe.found = true;
    probe.colorSpace = desc1.ColorSpace;
    probe.maxLuminanceNits = desc1.MaxLuminance;
    // SDR 白点来自 display config；查不到时退回 200 nits（Windows HDR 滑块 50% 的默认值）。
    const float sdrWhite = querySdrWhiteNits(desc1.Monitor);
    probe.sdrWhiteNits = sdrWhite > 1.0f ? sdrWhite : 200.0f;
    probe.hdrOn = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                  desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
    return probe;
}

mcdr::Result<void> createSwapChainForWindow(IDXGIFactory4* factory, ID3D12CommandQueue* queue, HWND win,
                                           HWND presentWin, UINT width, UINT height,
                                           const SwapChainBundle* reuse, SwapChainBundle& out,
                                           DXGI_FORMAT format) {
    static constexpr struct {
        DXGI_SWAP_EFFECT effect;
        UINT flags;
        const char* label;
    } kTiers[] = {
        {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, "FLIP_DISCARD+TEARING"},
        {DXGI_SWAP_EFFECT_FLIP_DISCARD, 0, "FLIP_DISCARD"},
        {DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 0, "FLIP_SEQUENTIAL"},
    };

    // RTSS/OSD 修复：优先把 swapchain 建在自建呈现子窗口上（presentWin）。GL 呈
    // 现过的父窗口会被 DXGI 永久拒绝（E_ACCESSDENIED），子窗口不会。
    const HWND chainWin = presentWin != nullptr ? presentWin : win;

    if (!reuse || !reuse->composition) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        // resize 修复（关键）：这里必须用调用方给的目标尺寸建链，绝不能再用 1x1 占位。
        // 原先固定 1x1、指望 configureSurface 的 ResizeBuffers 事后调整，但 flip model +
        // 异步呈现线程持续 Present 时 ResizeBuffers 会一直返回 DXGI_ERROR_INVALID_CALL
        // (0x887A0001)，configureSurface 于是走「重建 swapchain」回退，而重建又建出一个
        // 1x1 的链，且重建路径不会再调 ResizeBuffers → 1x1 缓冲被拉伸铺满整个窗口，
        // 用户看到的就是「resize 后黑屏」（实测：日志 recreated swapchain 2560x1334，
        // 但读回 back buffer 尺寸仍是 1x1、颜色恒为 RGBA(24,22,14)）。
        sd.Width = width > 0 ? width : 1;
        sd.Height = height > 0 ? height : 1;
        sd.Format = format;
        sd.Stereo = FALSE;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kSurfaceBufferCount;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

        const int kPasses = 3;
        HRESULT lastHr = E_FAIL;
        for (int pass = 0; pass < kPasses && !out.swapChain; ++pass) {
            if (pass > 0) {
                dbgLogError("createSwapChainForWindow: all HWND tiers failed (last hr=0x%08X), "
                       "retry pass=%d (pump %dms)",
                       static_cast<unsigned>(lastHr), pass, pass * 60);
                pumpWindowMessages(pass * 60);
            }
            for (const auto& tier : kTiers) {
                // P251：配置关掉 tearing 时跳过第一档，链只能进 DWM 合成翻页
                // （RTSS/Afterburner 的 OSD 只在合成翻页下画得出来）。
                if ((tier.flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0 &&
                    mcdr_surface_opts_allow_tearing() == 0) {
                    continue;
                }
                sd.SwapEffect = tier.effect;
                sd.Flags = tier.flags;
                HRESULT hr =
                    factory->CreateSwapChainForHwnd(queue, chainWin, &sd, nullptr, nullptr, &out.swapChain);
                if (SUCCEEDED(hr)) {
                    out.allowTearing = (tier.flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
                    dbgLogInfo("swapchain OK (tier=%s pass=%d hwnd=%s) — OSD/overlay-capable",
                               tier.label, pass,
                               presentWin != nullptr ? "present-child" : "game-window");
                    // P195：对齐 26.4-snapshot-5 的 DXGI 封装（DXGIFactory5.makeWindowAssociation(hwnd, 3)）。
                    // 我们的呈现窗口是自建子窗口，26.3 起全屏由 SDL 改显示模式；若放任 DXGI 自己响应
                    // Alt+Enter 去对子窗口做 SetFullscreenState，要么失败刷屏、要么把子窗口改成弹窗式
                    // 全屏盖住父窗口。NO_WINDOW_CHANGES 同时禁掉 DXGI 私自改窗口样式/位置。
                    // P195/P196：只禁 Alt+Enter。**不能**带 DXGI_MWA_NO_WINDOW_CHANGES ——
                    // 它禁止 DXGI 改窗口样式/位置，而 SetFullscreenState(TRUE) 正是靠改窗口
                    // 样式进全屏；带上它会让独占请求直接失败（实测 0x887A0022
                    // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE）。
                    const HRESULT assocHr = factory->MakeWindowAssociation(chainWin, DXGI_MWA_NO_ALT_ENTER);
                    dbgLog("MakeWindowAssociation(hwnd, NO_ALT_ENTER) hr=%08X",
                           static_cast<unsigned>(assocHr));
                    logSwapChainPresentHook(out.swapChain.Get());
                    break;
                }
                lastHr = hr;
                dbgLog("swapchain tier %s failed hr=%08X (pass=%d hwnd=%s)", tier.label,
                       static_cast<unsigned>(hr), pass,
                       presentWin != nullptr ? "present-child" : "game-window");
            }
        }
    } else {
        dbgLogInfo("createSwapChainForWindow: composition surface -> recreate composition directly");
    }

    if (out.swapChain) return {};

    // DirectComposition 回退
    if (win == nullptr) {
        return mcdr::err("createSwapChainForWindow",
                         "swapchain creation failed: HWND invalid, cannot use composition fallback");
    }
    ComPtr<IDXGIFactory2> factory2;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
        return mcdr::err("createSwapChainForWindow",
                         "swapchain creation failed (HWND tiers) and factory lacks IDXGIFactory2");
    }

    if (reuse && reuse->composition) {
        out.dcompDevice = reuse->dcompDevice;
        out.dcompTarget = reuse->dcompTarget;
        out.dcompVisual = reuse->dcompVisual;
        (void)setCompositionContent(out.dcompVisual.Get(), out.dcompDevice.Get(), nullptr);
    } else {
        auto comp = initComposition(win, out);
        if (!comp) {
            return mcdr::err("createSwapChainForWindow",
                             "swapchain HWND tiers failed; composition fallback failed: " +
                                 comp.error().message);
        }
    }
    auto created = createCompositionSwapChain(factory2.Get(), queue, width, height, format, out);
    if (!created) {
        return mcdr::err("createSwapChainForWindow",
                         "swapchain HWND tiers failed; composition fallback failed: " +
                             created.error().message);
    }
    dbgLogInfo("composition swapchain OK (%ux%u)", width, height);
    return {};
}

// 取 surface 的 back buffer index 对应的 RTV（blit 后可用；P5 自检不用）。
}  // namespace

mcdr::Result<void*> createSurface(uintptr_t hwnd) {
    DeviceContext& ctx = deviceContext();
    if (!ctx.device || !ctx.queue) {
        return mcdr::err("createSurface", "device not initialized (call dx12CreateDevice first)");
    }

    DXGI_ADAPTER_DESC devDesc{};
    if (!ctx.adapter) {
        return mcdr::err("createSurface", "createSurface: ctx.adapter is null");
    }
    ctx.adapter->GetDesc(&devDesc);
    dbgLogDebug("createSurface: devLuid=%08X%08X desc=%S", devDesc.AdapterLuid.LowPart,
                devDesc.AdapterLuid.HighPart, devDesc.Description);
    HWND win = hwnd != 0 ? reinterpret_cast<HWND>(hwnd) : nullptr;

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = createDxgiFactory(factory);
    if (FAILED(hr)) {
        return mcdr::err("createSurface", "CreateDXGIFactory1 failed " + hrText(hr));
    }
    // 确保 factory 能访问 device 所在的 adapter（防止多 GPU 系统上 factory/adapter 不匹配）
    {
        LUID devLuid = ctx.device->GetAdapterLuid();
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter> adj;
            if (FAILED(factory->EnumAdapters(i, &adj))) break;
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adj->GetDesc(&desc)) && desc.AdapterLuid.LowPart == devLuid.LowPart
                && desc.AdapterLuid.HighPart == devLuid.HighPart) {
                ctx.adapter = adj;
                break;
            }
        }
    }

    // 初始尺寸：composition 回退路径需要真实客户区尺寸；HWND 路径仍用 1x1 占位
    // （由 configureSurface 的 ResizeBuffers 调整到目标尺寸）。
    UINT initW = 1, initH = 1;
    if (win != nullptr) {
        RECT crc{};
        if (GetClientRect(win, &crc) && crc.right > crc.left && crc.bottom > crc.top) {
            initW = static_cast<UINT>(crc.right - crc.left);
            initH = static_cast<UINT>(crc.bottom - crc.top);
        }
    }

    // P197：HDR 判定必须发生在建链之前 —— 格式是 CreateSwapChainForHwnd 的参数。
    // 只信 DXGI 输出（Windows 高级颜色开启时输出色彩空间就是 PQ/HDR10）；配置只有
    // auto/on/off 三态，on 用于在系统未开 HDR 时做管线自检。
    const int hdrMode = mcdr_hdr_state_mode();
    const HdrProbe probe = probeHdrForWindow(factory.Get(), win);
    const bool wantHdr = hdrMode == 1 || (hdrMode == 0 && probe.hdrOn);
    // 记录“真正用掉的”模式：JVM 侧拿它和当前选项比较，决定视频设置里要不要提示重启。
    mcdr_hdr_state_set_applied_mode(hdrMode);
    // scRGB：R16G16B16A16_FLOAT + DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709（1.0 = 80 nits）。
    // 选它而不是 HDR10(PQ) 的原因：线性、可直接输出 >1.0 的亮度，不需要在像素着色器里
    // 做 PQ 编码，也不会把 SDR 内容压进 10bit 的 PQ 曲线里。
    const DXGI_FORMAT chainFormat =
        wantHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    dbgLogInfo("[P197] HDR probe: mode=%d found=%d hdrOn=%d colorSpace=%d sdrWhite=%.1f nits "
               "max=%.1f nits -> wantHdr=%d chainFormat=%d",
               hdrMode, probe.found ? 1 : 0, probe.hdrOn ? 1 : 0, static_cast<int>(probe.colorSpace),
               probe.sdrWhiteNits, probe.maxLuminanceNits, wantHdr ? 1 : 0,
               static_cast<int>(chainFormat));

    // P196：present 链优先建在 SDL 顶层窗口（game window）上。26.3 起窗口由 SDL 创建、
    // 不再被 GL 呈过，26.2 那条「GL 用过的 HWND 被 DXGI 永久拒绝（E_ACCESSDENIED）」
    // 的前提已消失（实测 run30 建链成功且持续出画）。更重要的是：只有呈现窗口就是顶层
    // 窗口时，DXGI 才可能进入真独占全屏 —— 子窗口永远只是父窗口里的一个子表面，
    // SetFullscreenState 会失败或把界面变成弹窗式（见 createSwapChainForWindow 注释）。
    // 呈现子窗口因此降级为回退：顶层窗口建链失败时才创建它再试一次。
    // P267 SeamlessWindowHost: the chain lives on a child window we create ourselves that has
    // never been touched by GL (1:1 with the parent client area, hidden until the first Present).
    MCDR_WindowHost* host = nullptr;
    HWND presentChild = nullptr;
    if (win != nullptr && mcdr_surface_opts_force_present_child() != 0) {
        host = mcdr_host_create(win, static_cast<int32_t>(initW), static_cast<int32_t>(initH));
        if (host != nullptr) {
            presentChild = reinterpret_cast<HWND>(mcdr_host_child(host));
            dbgLogInfo("[P267] seamless host child=%p (%ux%u) parent=%p",
                       reinterpret_cast<void*>(presentChild), initW, initH,
                       reinterpret_cast<void*>(win));
            // 子窗口要等第一帧 Present 才显示，这段窗口期里 SDL 的客户区从未被绘制过
            // （默认白面）。先把父窗口刷黑，用户看到的就是"黑幕 -> 内容"，而不是白闪。
            paintWindowBlack(win);
        } else {
            dbgLogWarn("[P267] seamless host creation failed (err=%lu); using top-level window",
                       static_cast<unsigned long>(GetLastError()));
        }
    }

    // Physical isolation: unbind the current GL context while DXGI creates the chain so the
    // HWND it receives has never been used with OpenGL.
    const bool glWasSuspended = mcdr_host_suspend_gl() != 0;
    SwapChainBundle bundle;
    auto swapChainRes = createSwapChainForWindow(factory.Get(), ctx.queue.Get(), win, presentChild,
                                                initW, initH, nullptr, bundle, chainFormat);
    if (!swapChainRes && presentChild != nullptr) {
        dbgLog("createSurface: chain on seamless host failed (%s); retrying on game window",
               swapChainRes.error().message.c_str());
        mcdr_host_release(host);
        host = nullptr;
        presentChild = nullptr;
        swapChainRes = createSwapChainForWindow(factory.Get(), ctx.queue.Get(), win, nullptr, initW,
                                                initH, nullptr, bundle, chainFormat);
    }
    if (glWasSuspended) mcdr_host_restore_gl();
    if (bundle.composition && presentChild != nullptr) {
        // DirectComposition fallback: the visual composites onto the main window and a child
        // window on top would hide it. Release the host and keep the legacy path.
        dbgLogInfo("createSurface: composition fallback -> releasing seamless host");
        mcdr_host_release(host);
        host = nullptr;
        presentChild = nullptr;
    }
    ComPtr<IDXGISwapChain1> swapChain1 = bundle.swapChain;

    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain1.As(&swapChain3))) {
        return mcdr::err("createSurface", "swapchain does not support IDXGISwapChain3");
    }

    // P197：把链切到 scRGB。CheckColorSpaceSupport 必须先过 —— 少数驱动/覆盖层会拒绝，
    // 拒绝时不重建链（float 链在 G22 色彩空间下就是普通的 SDR 呈现，画面与 RGBA8 等价）。
    bool hdrActive = false;
    if (wantHdr) {
        UINT support = 0;
        const HRESULT csHr = swapChain3->CheckColorSpaceSupport(
            DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support);
        const bool canPresent =
            SUCCEEDED(csHr) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;
        const HRESULT setHr = canPresent
            ? swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
            : E_FAIL;
        if (SUCCEEDED(setHr)) {
            hdrActive = true;
        } else {
            dbgLogWarn("[P197] scRGB color space rejected (check=0x%08X support=0x%X set=0x%08X) — "
                       "presenting SDR through the float chain",
                       static_cast<unsigned>(csHr), support, static_cast<unsigned>(setHr));
        }
    }

    Dx12Surface* s = new Dx12Surface();
    s->hwnd = hwnd;
    s->presentWindow = presentChild;
    s->host = host;  // 非空 = swapchain 建在自建子窗口上（HWND 路径）
    s->swapChain = swapChain3;
    s->compositionMode = bundle.composition;
    s->dcompDevice = bundle.dcompDevice;
    s->dcompTarget = bundle.dcompTarget;
    s->dcompVisual = bundle.dcompVisual;
    // P65：记录实际创建是否带 ALLOW_TEARING（composition swapchain 不支持 tearing）。
    s->allowTearing = bundle.allowTearing;
    // P197：HDR 状态与 blit 需要的换算系数。scRGB 的 1.0 = 80 nits，而系统「SDR 白」
    // 通常是 200 nits 左右，所以 SDR 内容要乘 SDRWhiteLevel/80 才是正确的亮度。
    s->format = chainFormat;
    s->swapChainFormat = chainFormat;
    s->hdrActive = hdrActive;
    s->hdrMaxLuminance = probe.maxLuminanceNits;
    s->hdrSdrWhiteScale = hdrActive ? std::clamp(probe.sdrWhiteNits / 80.0f, 1.0f, 10.0f) : 1.0f;
    mcdr_hdr_state_set_active(hdrActive ? 1 : 0);
    if (hdrActive) {
        dbgLogInfo("[P197] HDR active: scRGB (R16G16B16A16_FLOAT + G10), SDR white %.1f nits "
                   "(scale %.3f), max luminance %.1f nits, presentation window %s",
                   probe.sdrWhiteNits, s->hdrSdrWhiteScale, probe.maxLuminanceNits,
                   presentChild ? "present-child" : "game-window");
    } else {
        dbgLogInfo("[P197] HDR inactive: SDR presentation (format=%d)",
                   static_cast<int>(chainFormat));
    }
    // 注意：不在此处 setActiveSurface！surface 必须在 configureSurface 完成后
    // 才设为 active，否则渲染线程会在 backBuffers 为空时尝试 acquire 导致无限循环。
    return static_cast<void*>(s);
}

// P196：真独占全屏。26.3 的 Window.applyExclusiveFullscreen() 只调 SDL 改显示模式
// （SDL_SetWindowFullscreenMode + SDL_SetWindowFullscreen），DXGI 的 swap chain 从不进入
// fullscreen state —— 于是画面永远停留在 DWM 合成窗口，用户看到的"独占全屏"只是窗口铺满
// 屏幕、分辨率/刷新率换了个值。这里由 Java 侧把 Window.isExclusiveFullscreen() 推进来，
// 真正调 SetFullscreenState：
//   ON  → TRUE ：链取得 output 所有权、DWM 让位（FLIP 模型下同样合法，tearing 标志在
//                全屏下被忽略）
//   OFF → FALSE：退回桌面组合
// 失败不致命（驱动/多显示器/覆盖层注入器都可能拒绝），只记 WARN，画面照旧走窗口化路径。
mcdr::Result<void> setExclusiveFullscreen(Dx12Surface* s, bool on) {
    if (s == nullptr) return mcdr::err("setExclusiveFullscreen", "surface is null");
    if (!s->swapChain) return mcdr::err("setExclusiveFullscreen", "surface has no swap chain");
    if (s->compositionMode) {
        return mcdr::err("setExclusiveFullscreen",
                         "composition swap chain cannot enter exclusive fullscreen");
    }
    if (s->host != nullptr) {
        // P267: the chain hangs off a child window; only a top-level swap chain can take the
        // output. Exclusive fullscreen therefore needs the host to go away first (next step).
        return mcdr::err("setExclusiveFullscreen",
                         "exclusive fullscreen needs a top-level swap chain (seamless host active)");
    }
    const BOOL want = on ? TRUE : FALSE;
    const HRESULT hr = s->swapChain->SetFullscreenState(want, nullptr);
    BOOL actual = FALSE;
    s->swapChain->GetFullscreenState(&actual, nullptr);
    if (FAILED(hr)) {
        dbgLogWarn("P196 setExclusiveFullscreen(%d): SetFullscreenState hr=0x%08X "
                   "(fullscreenState=%d) — staying windowed",
                   on ? 1 : 0, static_cast<unsigned>(hr), actual ? 1 : 0);
        return mcdr::err("setExclusiveFullscreen", "SetFullscreenState failed: " + hrText(hr));
    }
    s->exclusive = on;
    // P196b：真独占下 DXGI 要求链的窗口是前台窗口；我们绕过 SDL 直接改状态，SDL 不会替我们
    // 把窗口提到前台，窗口留在别的程序后面时 Present 会被 DWM 丢弃 —— 实测表现为"画面冻结"
    // （presented 计数不再前进、acquire 超时）。显式提到前台。
    if (on && s->hwnd != 0) {
        HWND hwnd = reinterpret_cast<HWND>(s->hwnd);
        ShowWindow(hwnd, SW_SHOW);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
    }
    // P196：SetFullscreenState 之后必须重建 back buffer —— DXGI 把链交给 output 时旧的
    // flip 序列作废，继续 Present 会返回 S_OK 但不再翻页（画面冻结在进全屏前那一帧）。
    // 同步做完再返回，避免调用方随后就往作废的序列上 present。
    if (s->width > 0 && s->height > 0) {
        auto resized = configureSurface(s, static_cast<int>(s->width), static_cast<int>(s->height),
                                        s->presentMode, /*force=*/true);
        if (!resized) {
            dbgLogWarn("P196 setExclusiveFullscreen(%d): forced ResizeBuffers failed: %s", on ? 1 : 0,
                       resized.error().message.c_str());
        }
    }
    dbgLogInfo("P196 setExclusiveFullscreen(%d): ok, fullscreenState=%d presentWindow=%p",
               on ? 1 : 0, actual ? 1 : 0, reinterpret_cast<void*>(s->presentWindow));
    return {};
}

std::vector<int> surfacePresentModes() {
    // IMMEDIATE=0, FIFO=2, FIFO_RELAXED=3（MAILBOX=1 在 DXGI FLIP 下无直接对应）
    return {0, 2, 3};
}

mcdr::Result<void> configureSurface(Dx12Surface* s, int width, int height, int presentMode, bool force) {
    DeviceContext& ctx = deviceContext();
    // P98 诊断（用户报告：加载屏幕弹出瞬间帧时间骤增、启动加载界面进度条冻结）：
    // 把 configureSurface 各段耗时量出来，定位主线程停顿发生在哪一步。
    const double p98t0 = nowMs();
    double p98waitRenderMs = -1.0;
    double p98waitPresentsMs = -1.0;
    double p98idleMs = -1.0;
    if (!s || !s->swapChain) {
        return mcdr::err("configureSurface", "surface not created");
    }
    // 窗口最小化/边框切换瞬间 WM_SIZE 可能传 0 尺寸——ResizeBuffers 对 0 尺寸
    // 返回 DXGI_ERROR_INVALID_CALL (0x887A0001)，保持旧尺寸继续，不视为错误。
    // 非 0 尺寸时也偶发 INVALID_CALL（DWM 仍持有 backbuffer 引用，与 NOT_CURRENTLY_AVAILABLE
    // 同源竞态），交由下方重试循环处理。
    if (width <= 0 || height <= 0) {
        s->presentMode = presentMode;
        return {};
    }
    // 防御：glfwGetFramebufferSize 在某些时机（DWM 切换/显示器热插拔瞬间）会返回错误
    // 尺寸（如 854x1048 而非实际的 854x480）。若新高度相对旧高度偏差超过 2 倍且宽度
    // 未变化，视为无效回调，保留旧尺寸避免 swapchain 重建为错误比例导致黑屏。
    // 比较前把 UINT 成员显式收成 int：width/height 是 int 入参（可能为负），
    // 直接混比会触发 /W4 的 C4389 有符号/无符号不匹配。
    const int curW = static_cast<int>(s->width);
    const int curH = static_cast<int>(s->height);
    if (curW > 0 && curH > 0 && width == curW && height > curH * 2) {
        dbgLog("configureSurface: rejecting invalid height %d (current %d), keeping old", height, s->height);
        s->presentMode = presentMode;
        return {};
    }
    s->presentMode = presentMode;
    // 呈现子窗口跟随客户区尺寸（它是 swapchain 的实际宿主窗口）。
    // P267: keep the seamless host child 1:1 with the client area (WM_SIZE already did it
    // once, this covers the configureSurface path).
    if (s->host != nullptr) {
        mcdr_host_resize(static_cast<MCDR_WindowHost*>(s->host), width, height);
    } else {
        resizePresentationWindow(s->presentWindow, static_cast<UINT>(width), static_cast<UINT>(height));
    }

    // 新尺寸与当前 swapchain 一致时，无需 ResizeBuffers（避免已重建的 swapchain
    // 被错误尺寸锁死后再调 ResizeBuffers 仍报 INVALID_CALL）。
    // P196：进出真独占时尺寸不变，但必须重建 back buffer —— DXGI 在 SetFullscreenState
    // 之后把链交给 output，旧的 flip 序列作废；不重建的话 Present 返回 S_OK 却不再翻页，
    // 画面停在进全屏前那一帧（用户报的"独占全屏后画面冻结"）。此时由 force 绕过跳过。
    if (!force && static_cast<UINT>(width) == s->width && static_cast<UINT>(height) == s->height) {
        dbgLogInfo("configureSurface: size unchanged %dx%d, skip ResizeBuffers (P98 total=%.1f ms)", width, height,
               nowMs() - p98t0);
        return {};
    }

    // ResizeBuffers 前必须等 GPU 完全空闲：FLIP model 下 backbuffer 仍被
    // 上一帧命令队列引用时，ResizeBuffers 返回 DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
    // (0x887A0001)——游戏启动/窗口调整时多次 configure 失败即此原因。
    // deviceWaitIdle 现已确保等待一个绝对高于 GPU 已完成值的 fence，
    // 避免提前返回导致 DWM 仍持有 backbuffer 引用而失败（DXGI_ERROR_INVALID_CALL）。
    // 镜像官方 VulkanGpuSurface 调整 swapchain 前的 waitIdle 语义。
    //
    // P70：除了等 GPU，还要先把呈现管线本身"静止"下来——异步渲染线程提交中的帧
    // 会与 ResizeBuffers 并发访问同一条 swapchain，这是 INVALID_CALL 的主要来源。
    const double p98tWaitRender0 = nowMs();
    waitForRenderThreadSubmit();          // 等异步渲染线程完成当前帧提交
    p98waitRenderMs = nowMs() - p98tWaitRender0;
    if (!waitForPendingPresents(2000)) {  // 等已提交但尚未 Present 完成的帧
        dbgLog("configureSurface: pending presents did not drain before ResizeBuffers (continuing)");
    }
    p98waitPresentsMs = nowMs() - p98tWaitRender0 - p98waitRenderMs;
    const double p98tIdle0 = nowMs();
    {
        auto idleRes = deviceWaitIdle();
        if (!idleRes) {
            return mcdr::err("configureSurface", "deviceWaitIdle before ResizeBuffers failed: " +
                                                     idleRes.error().message);
        }
    }
    p98idleMs = nowMs() - p98tIdle0;
    dbgLogDebug("configureSurface: idle ok, ResizeBuffers %dx%d mode=%d", width, height, presentMode);
    // 防御：vanilla 在 acquire 与 present 之间若被窗口事件触发 configure，
    // 会残留"已 acquire 未 present"的 backbuffer；ResizeBuffers 对其返回
    // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE -> MC surfaceIsInvalid=true -> 之后
    // 不再 acquire/blit/present -> 画面冻结（渲染仍继续）。先 Present 释放。
    //
    // P95b（叠帧 + OSD 重叠）：判据必须是 bufferHeld，不能再用 currentImageIndex。
    // currentImageIndex 由渲染线程在 presentSurface 末尾置 -1，而本函数跑在主线程：
    // 异步呈现路径下这是一处数据竞争，主线程可能读到过期值（>=0），于是对一张**已经
    // present 过**的 back buffer 再 Present 一次。两个后果正好是用户报告的现象：
    //   1) flip 链被多推一帧 → 上一帧的画面再次上屏（"上一帧和这一帧叠在一起"）；
    //   2) 挂在 Present 上的 OSD 注入器（RTSS/Afterburner）对同一张 buffer 再画一遍
    //      OSD（错位双份）——因此"判据是 OSD 重叠"。
    // bufferHeld 是原子量并与 acquire/present 严格配对，读到 true 才代表真的还持有
    // 一张未 present 的 back buffer。
    if (s->bufferHeld.exchange(false)) {
        dbgLogDebug("configureSurface: releasing acquired backbuffer idx=%d before ResizeBuffers",
               s->currentImageIndex);
        s->swapChain->Present(0, 0);
        s->currentImageIndex = -1;
    }
    // composition 模式：DirectComposition 持有 backbuffer 引用，ResizeBuffers /
    // 释放 swapchain 前必须先解绑 visual 内容并 Commit，否则报 INVALID_CALL。
    if (s->compositionMode) {
        auto unbind = setCompositionContent(s->dcompVisual.Get(), s->dcompDevice.Get(), nullptr);
        if (!unbind) {
            dbgLogError("configureSurface: unbind composition content failed: %s",
                        unbind.error().message.c_str());
        }
    }
    // P71（真正的 INVALID_CALL 根因）：DXGI 明确规定 ResizeBuffers 之前必须释放所有对
    // back buffer 的 direct/indirect 引用（"You must release all outstanding references to
    // the swap chain's buffers before resizing"），否则固定返回 DXGI_ERROR_INVALID_CALL
    // (0x887A0001)。旧实现把 s->backBuffers 一直持有到 retry 循环之后，于是 10 次重试
    // 必然全败、每次都退化到「销毁并重建 swapchain」——重建之所以能成功，正是因为它在
    // Reset 旧链时把这些引用释放掉了。修正后 flags/等待都只是次要条件。
    for (auto& bb : s->backBuffers) {
        if (bb) bb.Reset();
    }
    s->backBuffers.clear();
    // P197：用建链时真正的格式（HDR 下是 R16G16B16A16_FLOAT）。旧代码硬编码
    // R8G8B8A8_UNORM —— 与链格式不符时 ResizeBuffers 固定返回 DXGI_ERROR_INVALID_CALL。
    const DXGI_FORMAT scFmt = s->swapChainFormat;
    // P70（resize 噪音根因）：ResizeBuffers 的 flags 必须与创建 swapchain 时一致。
    // 这里曾无条件传 DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING；而 createSwapChainForWindow
    // 的多 tier 重试中，"不带 TEARING" 的 tier 成功时 s->allowTearing == false，
    // 于是每次 ResizeBuffers 都因 flag 不匹配直接 INVALID_CALL，重试 10 次后走重建——
    // 这正是「大尺寸 resize 间歇性失败 + 日志刷屏 + 重建丢状态」的来源。
    // composition swapchain 不支持 ALLOW_TEARING，同样必须传 0。
    const UINT scFlags =
        s->compositionMode ? 0 : (s->allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    HRESULT hr = S_OK;
    // deviceWaitIdle 完成后 DWM 合成器可能仍在异步持有 backbuffer 引用（flip model
    // + 窗口/全屏切换时常见竞态）。多次重试 + 递增等待，最多等 ~1s。
    // P96 诊断：把"要 resize 到多少"与此刻窗口/子窗口的真实几何一起记录 —— 用于判断
    // 退出时 2560x1334 的重配置是否同时改变了窗口尺寸（白幕是否只是"整块落在客户区里"）。
    dbgLogInfo("P96 configureSurface: target=%ux%u presentWindow=%p", static_cast<unsigned>(width),
               static_cast<unsigned>(height), reinterpret_cast<void*>(s->presentWindow));
    logWindowState("configureSurface child", s->presentWindow);
    logWindowState("configureSurface parent", s->presentWindow ? GetParent(s->presentWindow) : nullptr);
    for (int retry = 0; retry < 10; ++retry) {
        if (retry > 0) {
            int waitMs = (1 << retry);  // 2,4,8,16,32,64,128,256,512 ms
            dbgLogDebug("configureSurface: retry %d after %dms", retry, waitMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
        }
        hr = s->swapChain->ResizeBuffers(kSurfaceBufferCount, static_cast<UINT>(width),
                                         static_cast<UINT>(height), scFmt, scFlags);
        if (SUCCEEDED(hr)) {
            if (retry > 0) {
                dbgLogInfo("configureSurface: ResizeBuffers ok after %d retries", retry);
            } else {
                dbgLogDebug("configureSurface: ResizeBuffers ok (retry=0)");
            }
            break;
        }
        // P70：重试属于预期内的 DWM/flip 竞态，不是错误——第一次失败打一条可见日志，
        // 后续降级到 DEBUG，避免拖动窗口时把日志刷满（原实现 10 次全为 WARN 级）。
        if (retry == 0) {
            dbgLog("configureSurface: ResizeBuffers failed %s (flags=0x%X tearing=%d) — retrying",
                   hrText(hr).c_str(), scFlags, static_cast<int>(s->allowTearing));
        } else {
            dbgLogDebug("configureSurface: ResizeBuffers failed %s (retry=%d)", hrText(hr).c_str(),
                        retry);
        }
        // INVALID_CALL 与 NOT_CURRENTLY_AVAILABLE 同源：DWM 异步持有 backbuffer 引用时
        // 均可能出现，统一重试（递增等待，最多 ~1s）。
        if (hr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE && hr != DXGI_ERROR_INVALID_CALL) break;
    }
    if (FAILED(hr)) {
        // ResizeBuffers 全部失败（flip model 限制）：参照 VulkanGpuSurface.configure()
        // 的做法——销毁旧 swapchain 后重建全新 swapchain（同样尺寸、同样 HWND）。
        // 必须先 Release 旧 swapchain，否则 CreateSwapChainForHwnd 返回 E_ACCESSDENIED。
        dbgLogError("configureSurface: ResizeBuffers FAILED after 10 retries, falling back to recreate");
        // 先 Present 释放任何残留的 acquired backbuffer（同样只用 bufferHeld 判定，
        // 避免对已 present 的 buffer 再推一帧 —— 见上文 P95b 说明）。
        if (s->bufferHeld.exchange(false)) {
            s->swapChain->Present(0, 0);
            s->currentImageIndex = -1;
        }
        (void)deviceWaitIdle();
        s->swapChain.Reset();
        s->backBuffers.clear();
        // P71：不清 rtvHandles —— 句柄在 rtvHeap 上是 bump 分配，保留复用即可；
        // 旧实现每次重建都重新分配 3 个槽，反复 resize 会把 kRtvHeapSize 用尽
        // （"RTV descriptor heap exhausted"），随后 configureSurface 直接失败。
        s->currentImageIndex = -1;
        s->lastBlitIndex = -1;

        // 重建 swapchain（与 createSurface 相同的策略：HWND 多 tier + 重试，
        // 全部被拒则回退 composition）。
        ComPtr<IDXGIFactory4> factory;
        hr = createDxgiFactory(factory);
        if (FAILED(hr)) {
            return mcdr::err("configureSurface", "CreateDXGIFactory1 failed " + hrText(hr));
        }
        // 已是 composition 模式时复用现有 composition 对象（对同一 HWND 重复
        // CreateTargetForHwnd 会返回 WINDOW_ALREADY_COMPOSED），直接重建 swapchain。
        SwapChainBundle reuse;
        if (s->compositionMode) {
            reuse.composition = true;
            reuse.dcompDevice = s->dcompDevice;
            reuse.dcompTarget = s->dcompTarget;
            reuse.dcompVisual = s->dcompVisual;
        }
        SwapChainBundle bundle;
        auto recreated = createSwapChainForWindow(factory.Get(), ctx.queue.Get(),
                                                  reinterpret_cast<HWND>(s->hwnd), s->presentWindow,
                                                  static_cast<UINT>(width), static_cast<UINT>(height),
                                                  s->compositionMode ? &reuse : nullptr, bundle,
                                                  s->swapChainFormat);
        if (!recreated) {
            return mcdr::err("configureSurface", recreated.error().message + " (recreate)");
        }
        if (bundle.composition && s->presentWindow != nullptr) {
            // 重建时退化成 composition：呈现子窗口只会盖住合成画面，销毁它。
            paintParentBlackBeforeDestroy(s->presentWindow);
            DestroyWindow(s->presentWindow);
            s->presentWindow = nullptr;
        }
        if (FAILED(bundle.swapChain.As(&s->swapChain))) {
            return mcdr::err("configureSurface", "swapchain does not support IDXGISwapChain3");
        }
        s->compositionMode = bundle.composition;
        s->dcompDevice = bundle.dcompDevice;
        s->dcompTarget = bundle.dcompTarget;
        s->dcompVisual = bundle.dcompVisual;
        s->allowTearing = bundle.allowTearing;
        dbgLogInfo("configureSurface: recreated swapchain %dx%d (composition=%d)", width, height,
               static_cast<int>(s->compositionMode));
    }

    // composition 模式：把 visual 内容重新绑回（ResizeBuffers 后的同一 swapchain，
    // 或重建后的新 swapchain）。
    if (s->compositionMode) {
        auto rebind = setCompositionContent(s->dcompVisual.Get(), s->dcompDevice.Get(), s->swapChain.Get());
        if (!rebind) {
            return mcdr::err("configureSurface",
                             "configureSurface: rebind composition content failed: " +
                                 rebind.error().message);
        }
    }

    // 仅在成功路径上更新尺寸：失败时保持旧尺寸，避免后续调用因 s->width/s->height
    // 已被设为错误值而反复尝试无效 ResizeBuffers。
    s->width = static_cast<UINT>(width);
    s->height = static_cast<UINT>(height);

    // 重新取 back buffers + RTV
    s->backBuffers.resize(kSurfaceBufferCount);
    s->surfaceFences.assign(kSurfaceBufferCount, 0);  // P18：per-backbuffer fence 初值
    // P71：RTV 句柄持久复用（见上文重建分支的说明）。只在本对象还没有句柄时分配。
    if (s->rtvHandles.size() != kSurfaceBufferCount) {
        s->rtvHandles.clear();
        for (UINT i = 0; i < kSurfaceBufferCount; ++i) {
            auto rtv = allocRtvHandle();
            if (!rtv) return std::unexpected<mcdr::Error>(std::move(rtv.error()));
            s->rtvHandles.push_back(rtv.value());
        }
    }
    for (UINT i = 0; i < kSurfaceBufferCount; ++i) {
        hr = s->swapChain->GetBuffer(i, IID_PPV_ARGS(&s->backBuffers[i]));
        if (FAILED(hr)) {
            return mcdr::err("configureSurface", "GetBuffer failed " + hrText(hr));
        }
        ctx.device->CreateRenderTargetView(s->backBuffers[i].Get(), nullptr, s->rtvHandles[i]);
    }
    // resize 修复后的校验：重建/调整后的链真实尺寸必须等于请求尺寸。
    // （1x1 占位 bug 时期这里会打印 1x1，就是用户看到的「resize 后黑屏」。）
    if (!s->backBuffers.empty() && s->backBuffers[0]) {
        const D3D12_RESOURCE_DESC bd0 = s->backBuffers[0]->GetDesc();
        dbgLogDebug("configureSurface: swapchain buffer0 = %llux%u (requested %dx%d)",
               static_cast<unsigned long long>(bd0.Width), static_cast<unsigned>(bd0.Height),
               static_cast<int>(width), static_cast<int>(height));
    }
    // P35：配置完成后才设为 active surface，确保渲染线程不会在 backBuffers 未就绪时 acquire。
    setActiveSurface(s);
    dbgLogInfo("configureSurface: registered active surface=%p bbCount=%u", reinterpret_cast<void*>(s),
               static_cast<UINT>(s->backBuffers.size()));
    {
        const double p98total = nowMs() - p98t0;
        const double p98other = p98total - (p98waitRenderMs < 0.0 ? 0.0 : p98waitRenderMs) -
                                (p98waitPresentsMs < 0.0 ? 0.0 : p98waitPresentsMs) -
                                (p98idleMs < 0.0 ? 0.0 : p98idleMs);
        dbgLogInfo("P98 configureSurface: %dx%d total=%.1f ms (waitRender=%.1f waitPresents=%.1f idle=%.1f resize+rest=%.1f)",
               width, height, p98total, p98waitRenderMs, p98waitPresentsMs, p98idleMs, p98other);
    }
    return {};
}

mcdr::Result<void> acquireSurface(Dx12Surface* s) {
    // P98 诊断：主线程帧间隔（>200 ms 视为停顿/掉帧），用于定位用户报告的
    // 「加载屏幕弹出瞬间帧时间骤增」。acquire 每帧一次，是主线程帧节拍的天然探针。
    {
        static double sP98LastAcquireMs = 0.0;
        const double p98now = nowMs();
        if (sP98LastAcquireMs > 0.0) {
            const double p98gap = p98now - sP98LastAcquireMs;
            if (p98gap > 200.0) {
                dbgLogInfo("P98 main-thread frame hitch: %.1f ms since previous acquire", p98gap);
            }
        }
        sP98LastAcquireMs = p98now;
    }
    if (!s || !s->swapChain) {
        return mcdr::err("acquireSurface", "surface not created");
    }
    // P48（多帧飞行）：Present 尚未完成时 GetCurrentBackBufferIndex 不会轮转，会返回
    // 上一帧已 Present 的同一个 back buffer（两帧写同一处 → 闪帧/撕裂）。真实 acquire
    // 已推迟到 blit 之前（Java Dx12GpuSurface.ensureAcquired），此时本帧录制已完成，
    // 上一帧 Present 通常早已结束 → 通常 0ms。同步/SYNC 路径（渲染线程未运行）下
    // waitForPendingPresents 内部直接返回，无额外开销。
    //
    // P68（GLFW_NO_API 崩溃根因）：这里**必须**把同步失败当致命处理。旧实现在
    // waitForPendingPresents 超时后照常往下 acquire → 本帧 blit 写到一个「Present 完成
    // 后会翻转走」的 back buffer 上 → D3D12 ERROR #907（writes to a swapchain back buffer
    // ... may only be executed when that back buffer is the current back buffer）→
    // RemoveDevice(0x887A0005, DXGI_ERROR_ACCESS_DENIED) → 游戏崩溃。冷启动第一帧渲染
    // 线程要 initBlitPipeline（实测 >100ms）必然触发，所以 GLFW_NO_API 模式下必崩。
    // 现在给足冷启动余量；仍同步不上就放弃本帧：Java 侧 ensureAcquired() 返回 false →
    // 跳过 blit（画面停上一帧，不写 swapchain），下一帧 acquireNextTexture() 补报
    // SurfaceException 让 MC 走 surface 重建。
    if (!waitForPendingPresents(2000)) {
        s->currentImageIndex = -1;
        return mcdr::err("acquireSurface",
                         "acquireSurface: present sync timeout (render thread stalled) — frame skipped to "
                         "avoid writing a non-current back buffer");
    }
    // 以下两条为逐帧诊断（原为 dbgLog，每帧 2 次 stderr+文件 flush，实测 ~0.1-0.2ms/条，
    // 属帧关键路径纯开销）→ 降为 DEBUG，需 DX12_LOG_VERBOSE=1 才输出。
    dbgLogDebug("acquireSurface: enter surface=%p", reinterpret_cast<void*>(s));
    // P34 诊断：记录 GetCurrentBackBufferIndex 的原始返回值和 HRESULT
    UINT rawIdx = s->swapChain->GetCurrentBackBufferIndex();
    s->currentImageIndex = static_cast<int>(rawIdx);
    UINT bbCount = static_cast<UINT>(s->backBuffers.size());
    dbgLogDebug("acquireSurface: rawIdx=%u bbCount=%u currentImageIndex=%d", rawIdx, bbCount,
                s->currentImageIndex);
    if (s->currentImageIndex < 0 || s->currentImageIndex >= static_cast<int>(bbCount)) {
        // P34 容错：若 backBuffers 为空说明 surface 尚未完成配置，直接返回 false
        // 让渲染线程稍后重试；若 backBuffers 已就绪但 index 异常，尝试通过
        // GetBufferCount 交叉验证 swapchain 状态。
        if (bbCount == 0) {
            return mcdr::err("acquireSurface",
                             "acquireSurface: backBuffers empty (surface not configured yet)");
        }
        // 交叉验证：从 swapchain desc 获取实际 buffer 数
        DXGI_SWAP_CHAIN_DESC1 sd{};
        HRESULT hr = s->swapChain->GetDesc1(&sd);
        return mcdr::err("acquireSurface",
                         "GetCurrentBackBufferIndex returned invalid index=" + std::to_string(rawIdx) +
                             " bbCount=" + std::to_string(bbCount) + " scGetDesc_hr=" + hrText(hr) +
                             " BufferCount=" + std::to_string(sd.BufferCount));
    }
    // P18：如果重用的是上一帧的 back buffer（上次 blit 可能还没完成），
    // 等待该 buffer 对应的 fence 完成，再允许 CPU 写入。
    // 对于 3-buffer swapchain 正常情况，GPU 早已完成，等待为 0ms。
    // 仅在 CPU 跑太快追上 GPU 时才短暂等待（比 submitCommandList 阻塞好，
    // 因为只在真正需要重用时才等，且等的是已提交的 blit 命令而非当前帧）。
    {
        UINT64 needed =
            s->surfaceFences.empty() ? 0 : s->surfaceFences[static_cast<size_t>(s->currentImageIndex)];
        DeviceContext& ctx = deviceContext();
        if (needed > 0 && ctx.queueFence) {
            UINT64 cv = ctx.queueFence->GetCompletedValue();
            if (cv < needed) {
                dbgLogDebug("acquireSurface: wait fence idx=%d needed=%llu cv=%llu", s->currentImageIndex,
                       static_cast<unsigned long long>(needed), static_cast<unsigned long long>(cv));
                auto fenceRes = waitForQueueFenceValue(needed, 5000000000ULL);
                if (!fenceRes) {
                    return mcdr::err("acquireSurface",
                                     "acquireSurface: " + fenceRes.error().message);
                }
            }
        }
    }
    // 同步 lastBlitIndex：当游戏直接通过 beginRenderPass 渲染到表面纹理
    //（而非走 blitSurface 路径）时，readbackSurfacePixels 需要用最新的
    // currentImageIndex 才能读到正确的 backBuffer，否则会读到上一帧 blit 的
    // 旧数据（表现为黑屏/错误内容）。
    s->lastBlitIndex = s->currentImageIndex;
    // P95：新的一帧开始（已 acquire），但还没有 blit 过 —— presentSurface 靠这个标志
    // 拒绝 present 一张本帧从未写入的 back buffer。
    s->blittedThisFrame.store(false, std::memory_order_release);
    // P95b：本 surface 现在真的持有"已 acquire 未 present"的 back buffer。
    // 只有这个原子标志为 true 时才允许在 configureSurface 里用 Present(0,0) 释放它。
    s->bufferHeld.store(true, std::memory_order_release);
    return {};
}

// 返回当前 acquire 的 back buffer 原始 ID3D12Resource 指针。
uintptr_t getBackBufferHandle(Dx12Surface* s) {
    if (!s || s->currentImageIndex < 0 || s->currentImageIndex >= static_cast<int>(s->backBuffers.size())) {
        return 0ULL;
    }
    return reinterpret_cast<uintptr_t>(s->backBuffers[static_cast<size_t>(s->currentImageIndex)].Get());
}

// 返回当前渲染 pass 中第一个活跃颜色附件的纹理句柄（在 pass 内调用有效）。
uintptr_t getActiveColorTextureHandle(CommandContext* ctx) {
    if (!ctx || !ctx->inRenderPass || ctx->activeColorTargets.empty()) {
        return 0ULL;
    }
    return reinterpret_cast<uintptr_t>(ctx->activeColorTargets[0]->resource.Get());
}

mcdr::Result<void> blitSurface(CommandContext* ctx, Dx12Surface* s, Dx12Object* srcTex) {
    if (!ctx || !ctx->commandList) {
        return mcdr::err("blitSurface", "no command list");
    }
    if (!s || s->currentImageIndex < 0) {
        return mcdr::err("blitSurface", "no acquired back buffer");
    }
    if (!ctx->listOpen) {
        return mcdr::err("blitSurface", "command list not open (call dx12BeginCommandList first)");
    }
    // srcTex 为 null 时仅做纯红色 clear（无 copy），用于渲染循环自检。
    if (!srcTex || !srcTex->resource) {
        srcTex = nullptr;  // 标记为无源纹理，走纯 clear 路径
    }

    ID3D12GraphicsCommandList* cmd = ctx->commandList;
    ID3D12Resource* dst = s->backBuffers[static_cast<size_t>(s->currentImageIndex)].Get();
    UINT w = s->width;
    UINT h = s->height;
    dbgLogDebug("blitSurface: cmd=%p dst=%p idx=%d w=%u h=%u srcTex=%p", reinterpret_cast<void*>(cmd),
                reinterpret_cast<void*>(dst), s->currentImageIndex, w, h, reinterpret_cast<void*>(srcTex));

    // P109（#538 修复）：back buffer 必须**显式登记**进 ctx->resourceState，其稳定态是
    // PRESENT（flip-model 下 Present 之后就是这么个状态；数值 0，调试层打印成
    // "COMMON|PRESENT"）。三张 back buffer 一起登记，轮转时按索引复用同一份登记，
    // 绝不依赖"未登记 → COMMON 兜底"（D.9 铁律 2：未登记资源上 stateOf/transitionTo
    // 拿到的 Before 都是 COMMON，等于没查）。
    for (UINT i = 0; i < s->backBuffers.size(); ++i) {
        ID3D12Resource* bb = s->backBuffers[i].Get();
        if (bb && ctx->resourceState.find(bb) == ctx->resourceState.end()) {
            ctx->resourceState[bb] = D3D12_RESOURCE_STATE_PRESENT;
        }
    }
    if (ctx->inRenderPass) {
        // 诊断（限 3 次）：blit 被录在一条**仍打开的 render pass** 里。这是 #538 的可疑成因——
        // blit 会把 back buffer 的 RTV 绑上，而 pass 自己的附件绑定已经被覆盖；pass 结束时
        // endRenderPass 只回切它自己的附件，不会管 back buffer。
        static int p109InPassBlits = 0;
        if (p109InPassBlits < 3) {
            ++p109InPassBlits;
            dbgLogWarn("P109 blitSurface recorded INSIDE an open render pass (ctx=%p idx=%d): "
                       "the pass' attachments were rebound to the back buffer",
                       reinterpret_cast<void*>(ctx), s->currentImageIndex);
        }
    }

    // 源纹理可能是本帧渲染 pass 的输出（RENDER_TARGET/DEPTH_WRITE），或刚
    // 上传完的 COMMON；按跟踪状态过渡到 COPY_SOURCE 再拷贝。
    if (srcTex) transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = dst;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    // srcTex 为 null 时（首次/自检路径）：backbuffer 内容未定义（驱动常显示
    // 红/杂色），用纯黑 clear 兜底避免启动红屏，再回切 PRESENT。
    if (!srcTex) {
        transitionTo(ctx, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = s->rtvHandles[static_cast<size_t>(s->currentImageIndex)];
        float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd->ClearRenderTargetView(rtv, black, 0, nullptr);
        transitionTo(ctx, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        dbgLogDebug("blitSurface: srcTex=null — clear backbuffer to black");
        return {};
    }

    // P22: Draw-based blit（全屏四边形）— 绕过 CopyTextureRegion 格式不兼容问题。
    // srcTex 格式（如 R32G32B32A32_FLOAT）与 backbuffer（R8G8B8A8_UNORM）不同，
    // CopyTextureRegion 静默失败；改用 GPU 光栅器采样+格式转换。
    //
    // 步骤：
    //   1. PRESENT → RENDER_TARGET（backbuffer）
    //   2. OMSetRenderTargets + SetGraphicsRootSignature + SetPipelineState
    //   3. 源纹理 → PIXEL_SHADER_RESOURCE（采样前必须过渡）
    //   4. 在 srvHeap 分配 SRV 槽位，绑定到根描述符表
    //   5. 绑定顶点/索引缓冲，DrawIndexedInstanced(6,1,0,0,0)
    //   6. RENDER_TARGET → PRESENT（backbuffer）
    //   7. 源纹理 → COMMON（下一帧准备）
    {
        // 1. PRESENT → RENDER_TARGET
        // 使用 transitionTo 而非裸 barrier：将 dst 纳入 ctx->resourceState 跟踪，
        // 使 endCommandList 的 PRESENT 清理循环能正确回切到 COMMON。
        // P109（#538 修复）：Before 锚点必须是 **PRESENT**，不是 COMMON。flip-model 下
        // back buffer 在 Present 之后（以及 acquire 之后）的稳定态就是 PRESENT
        // （= 0，调试层显示 COMMON|PRESENT）；上面已经把它显式登记进 resourceState，
        // 所以这里 fromInitial 只在"本 list 内还没登记过"时用到。绝不依赖 COMMON 兜底。
        transitionTo(ctx, dst, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        // 2. 绑定 RTV 和根签名
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = s->rtvHandles[static_cast<size_t>(s->currentImageIndex)];
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        // P69：viewport / scissor 属于【命令列表状态】，不是 PSO 状态。blit 之前的
        // 最后一次 render pass（GUI blur / post-chain 等）留下的 viewport/scissor 会
        // 被这个"整屏"四边形继承，于是 blit 只覆盖 backbuffer 的一小块：未覆盖区域
        // 保留 2-3 帧前的旧内容（旧场景 + 旧 RTSS OSD 文字），表现为用户报告的
        // 「上一场景叠在当前场景下层并闪烁」以及「RTSS OSD 也重叠一份」。
        // 尺寸取 backbuffer 资源自身描述，避免 s->width/height 在 resize 后短暂不一致。
        const D3D12_RESOURCE_DESC bbDesc = dst->GetDesc();
        D3D12_VIEWPORT blitVp{0.0f, 0.0f, static_cast<FLOAT>(bbDesc.Width),
                              static_cast<FLOAT>(bbDesc.Height), 0.0f, 1.0f};
        cmd->RSSetViewports(1, &blitVp);
        D3D12_RECT blitSc{0, 0, static_cast<LONG>(bbDesc.Width), static_cast<LONG>(bbDesc.Height)};
        cmd->RSSetScissorRects(1, &blitSc);
        dbgLogDebug("blitSurface: viewport/scissor set %llux%llu", bbDesc.Width, bbDesc.Height);
        // 确保 blit 管线已初始化（首帧调用时 initBlitPipeline 尚未运行）
        auto blitInit = initBlitPipeline();
        if (!blitInit) {
            dbgLogError("blitSurface: initBlitPipeline FAILED: %s", blitInit.error().message.c_str());
            return mcdr::err("blitSurface", "blitSurface: " + blitInit.error().message);
        }
        const BlitPipeline* bp = getBlitPipeline();
        if (bp == nullptr) {
            return mcdr::err("blitSurface", "blitSurface: blit pipeline unavailable after init");
        }
        // P104b：SMAA 已移到世界层（GameRenderer 画完世界、GUI 还没画时对主渲染目标就地
        // 应用，见 applySmaaToTexture）。最终 blit 回到**纯线性升采样**：这里若还做抗锯齿，
        // 整帧（世界 + GUI）会被一起柔化，界面就发虚了 —— 也就是用户报告的 GUI 柔化问题。
        {
            cmd->SetGraphicsRootSignature(bp->rootSig.Get());
            // P197：HDR 开启时用 RGBA16F 的 PSO，并把 sRGB→线性 + SDR 白系数交给根常量 b0；
            // SDR 路径也显式设成 (1,0,0,0)，根常量未设置时读到的内容是未定义的。
            const bool hdr = s->hdrActive;
            ID3D12PipelineState* presentPso = (hdr && bp->psoHdr) ? bp->psoHdr.Get() : bp->pso.Get();
            cmd->SetPipelineState(presentPso);
            const float hdrParams[4] = {hdr ? s->hdrSdrWhiteScale : 1.0f, hdr ? 1.0f : 0.0f, 0.0f, 0.0f};
            cmd->SetGraphicsRoot32BitConstants(1, 4, hdrParams, 0);
            dbgLogDebug("blitSurface: set blit rootSig=%p pso=%p hdr=%d scale=%.3f",
                        reinterpret_cast<void*>(bp->rootSig.Get()),
                        reinterpret_cast<void*>(presentPso), hdr ? 1 : 0,
                        static_cast<double>(hdrParams[0]));

            // 3+4. 源纹理过渡 + SRV 分配 + 根描述符表绑定（一步完成）
            auto blitBind = blitBindSourceTexture(ctx, srcTex, cmd);
            if (!blitBind) {
                dbgLogError("blitSurface: blitBindSourceTexture FAILED: %s",
                            blitBind.error().message.c_str());
                return blitBind;
            }

            // 5. 绑定顶点/索引缓冲并绘制
            cmd->IASetVertexBuffers(0, 1, &bp->vbView);
            cmd->IASetIndexBuffer(&bp->ibView);
            // P69：IA topology 同样是命令列表状态（D3D12 初值为 UNDEFINED），必须显式设置，
            // 否则继承上一次 pass 的拓扑 → 整屏四边形退化/丢失，backbuffer 只被部分覆盖。
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            dbgLogDebug("blitSurface: drawIndexed inst=1 firstIdx=0 firstVert=0");
            cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);
            dbgLogDebug("blitSurface: drawIndexed done");
        }

        // 6. RENDER_TARGET → PRESENT
        // P109（#538 修复）：blit 是本帧对 back buffer 的最后一次 GPU 写入，之后它必须
        // 回到 PRESENT 才能 Present。这一步的 Before 从跟踪表读（上一步刚写成 RT）。
        transitionTo(ctx, dst, D3D12_RESOURCE_STATE_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_PRESENT);
        // P109（#538 根因修复）：把 back buffer 的 RTV **解绑**。命令列表状态里的
        // OMSetRenderTargets 会一直留到下一次 OMSetRenderTargets，而 back buffer 这时
        // 已经切回 PRESENT(0)；只要同一列表里 blit 之后还有 draw（例如某个 pass 在
        // blit 之前 begin、blit 之后才继续画），调试层就会把每一条 draw 判成
        // "#538：拿 COMMON|PRESENT 当 RENDER_TARGET"。实测一轮 35 条全挤在 2 ms 内，
        // 就是这个形状。解绑后这类残留绑定不可能再触发 #538。
        cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

        // 7. 源纹理 → COMMON（为下一帧 beginRenderPass 准备）
        if (srcTex) transitionTextureTo(ctx, srcTex, D3D12_RESOURCE_STATE_COMMON);
    }
    // P6 诊断：源纹理实际尺寸（拷贝是 min(源, backbuffer) 区域，若源比窗口小
    // 画面会留黑边；若源未渲染则纯色）。
    // srcTex 为 null 时（纯 clear 路径）跳过诊断日志，避免解引用空指针。
    if (srcTex && srcTex->resource) {
        D3D12_RESOURCE_DESC srcDesc = srcTex->resource->GetDesc();
        bool srcWasWritten = ctx->colorTargetsWritten;
        dbgLogDebug("blitSurface: ctx=%p ctxW=%d src=%p -> backbuf=%ux%u", reinterpret_cast<void*>(ctx),
                    static_cast<int>(ctx->colorTargetsWritten), reinterpret_cast<void*>(srcTex), w, h);
        dbgLogDebug("blitSurface: src=%p srcW=%llu srcH=%llu fmt=%d wasWritten=%d -> backbuf=%ux%u",
                    reinterpret_cast<void*>(srcTex), static_cast<unsigned long long>(srcDesc.Width),
                    static_cast<unsigned long long>(srcDesc.Height), static_cast<int>(srcTex->dxgiFormat),
                    static_cast<int>(srcWasWritten), w, h);
    } else {
        dbgLogDebug("blitSurface: srcTex=null (pure red clear), no src diagnostic");
    }
    // 记录本帧 blit 写入的 back buffer 下标（present 后 currentImageIndex=-1，
    // readback 必须用此值才能读到真实画面）。
    s->lastBlitIndex = s->currentImageIndex;
    // P95：本帧已有内容 —— presentSurface 只在这种情况下才 Present。
    s->blittedThisFrame.store(true, std::memory_order_release);
    return {};
}

// P106：纹理 → 纹理（可缩放）blit。GUI 的独立全尺寸目标靠它把世界层结果从主渲染目标
// 搬过去：主渲染目标在 renderScale != 100 时是缩放尺寸（如 1281x720），GUI 目标是窗口
// 尺寸（854x480），所以必须走带线性采样的整屏三角形，而不是 1:1 的 CopyTextureRegion。
mcdr::Result<void> blitTextureToTexture(CommandContext* ctx, Dx12Object* src, Dx12Object* dst) {
    if (!ctx || !ctx->commandList || !src || !src->resource || !dst || !dst->resource) {
        return mcdr::err("blitTextureToTexture", "blitTextureToTexture: null ctx/src/dst");
    }
    if (src->resource.Get() == dst->resource.Get()) {
        return mcdr::err("blitTextureToTexture", "blitTextureToTexture: src and dst are the same resource");
    }

    ID3D12GraphicsCommandList* cmd = ctx->commandList;
    const D3D12_RESOURCE_DESC srcDesc = src->resource->GetDesc();
    const D3D12_RESOURCE_DESC dstDesc = dst->resource->GetDesc();
    if (dstDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dstDesc.SampleDesc.Count != 1) {
        return mcdr::err("blitTextureToTexture",
                         "blitTextureToTexture: dst is not a single-sampled 2D texture");
    }

    // 目标当渲染目标用：view/scissor 取目标自身尺寸（避免继承上一次 pass 的状态，
    // 那正是 P69 叠帧问题的成因）。
    auto rtv = acquireTextureRtv(ctx, dst);
    if (!rtv) {
        return mcdr::err("blitTextureToTexture", "blitTextureToTexture: " + rtv.error().message);
    }
    auto blitInit = initBlitPipeline();
    if (!blitInit) {
        return mcdr::err("blitTextureToTexture", "blitTextureToTexture: " + blitInit.error().message);
    }
    const BlitPipeline* bp = getBlitPipeline();
    if (bp == nullptr) {
        return mcdr::err("blitTextureToTexture", "blitTextureToTexture: blit pipeline unavailable");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dstRtv = rtv.value();
    cmd->OMSetRenderTargets(1, &dstRtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<FLOAT>(dstDesc.Width), static_cast<FLOAT>(dstDesc.Height), 0.0f, 1.0f};
    cmd->RSSetViewports(1, &vp);
    D3D12_RECT sc{0, 0, static_cast<LONG>(dstDesc.Width), static_cast<LONG>(dstDesc.Height)};
    cmd->RSSetScissorRects(1, &sc);
    transitionTextureTo(ctx, dst, D3D12_RESOURCE_STATE_RENDER_TARGET);

    cmd->SetGraphicsRootSignature(bp->rootSig.Get());
    cmd->SetPipelineState(bp->pso.Get());
    // P197：这条路径恒定是 SDR→SDR（内部纹理之间拷贝），显式关掉 HDR 变换。
    const float sdrHdrParams[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    cmd->SetGraphicsRoot32BitConstants(1, 4, sdrHdrParams, 0);
    auto bind = blitBindSourceTexture(ctx, src, cmd);
    if (!bind) {
        return mcdr::err("blitTextureToTexture", "blitTextureToTexture: " + bind.error().message);
    }

    cmd->IASetVertexBuffers(0, 1, &bp->vbView);
    cmd->IASetIndexBuffer(&bp->ibView);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    // 源回 RENDER_TARGET（世界层那一帧仍可能被 PostChain/清深度使用），目标留在
    // RENDER_TARGET 供随后 GUI pass 直接画。
    transitionTextureTo(ctx, src, D3D12_RESOURCE_STATE_RENDER_TARGET);
    dbgLogDebug("blitTextureToTexture: %llux%llu -> %llux%llu", srcDesc.Width, srcDesc.Height, dstDesc.Width,
                dstDesc.Height);
    return {};
}

mcdr::Result<void> sharpenTextureToTexture(CommandContext* ctx, Dx12Object* src, Dx12Object* dst,
                                          float amount) {
    if (!ctx || !ctx->commandList || !src || !src->resource || !dst || !dst->resource) {
        return mcdr::err("sharpenTextureToTexture", "sharpenTextureToTexture: null ctx/src/dst");
    }
    if (src->resource.Get() == dst->resource.Get()) {
        return mcdr::err("sharpenTextureToTexture",
                         "sharpenTextureToTexture: src and dst are the same resource");
    }

    ID3D12GraphicsCommandList* cmd = ctx->commandList;
    const D3D12_RESOURCE_DESC srcDesc = src->resource->GetDesc();
    const D3D12_RESOURCE_DESC dstDesc = dst->resource->GetDesc();
    if (dstDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dstDesc.SampleDesc.Count != 1) {
        return mcdr::err("sharpenTextureToTexture",
                         "sharpenTextureToTexture: dst is not a single-sampled 2D texture");
    }

    auto rtv = acquireTextureRtv(ctx, dst);
    if (!rtv) {
        return mcdr::err("sharpenTextureToTexture", "sharpenTextureToTexture: " + rtv.error().message);
    }
    auto init = initSharpenPipeline();
    if (!init) {
        return mcdr::err("sharpenTextureToTexture", "sharpenTextureToTexture: " + init.error().message);
    }
    const SharpenPipeline* sp = getSharpenPipeline();
    if (sp == nullptr) {
        return mcdr::err("sharpenTextureToTexture",
                         "sharpenTextureToTexture: sharpen pipeline unavailable");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dstRtv = rtv.value();
    cmd->OMSetRenderTargets(1, &dstRtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<FLOAT>(dstDesc.Width), static_cast<FLOAT>(dstDesc.Height),
                      0.0f, 1.0f};
    cmd->RSSetViewports(1, &vp);
    D3D12_RECT sc{0, 0, static_cast<LONG>(dstDesc.Width), static_cast<LONG>(dstDesc.Height)};
    cmd->RSSetScissorRects(1, &sc);
    transitionTextureTo(ctx, dst, D3D12_RESOURCE_STATE_RENDER_TARGET);

    cmd->SetGraphicsRootSignature(sp->rootSig.Get());
    cmd->SetPipelineState(sp->pso.Get());
    auto bind = blitBindSourceTexture(ctx, src, cmd);
    if (!bind) {
        return mcdr::err("sharpenTextureToTexture", "sharpenTextureToTexture: " + bind.error().message);
    }

    // b1 = {1/w, 1/h, amount, pad}：纹素步长按**源**尺寸算（采样源纹理）。
    const float params[4] = {
        srcDesc.Width == 0 ? 0.0f : 1.0f / static_cast<float>(srcDesc.Width),
        srcDesc.Height == 0 ? 0.0f : 1.0f / static_cast<float>(srcDesc.Height), amount, 0.0f};
    cmd->SetGraphicsRoot32BitConstants(1, 4, params, 0);

    cmd->IASetVertexBuffers(0, 1, &sp->vbView);
    cmd->IASetIndexBuffer(&sp->ibView);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);

    // 与 blitTextureToTexture 一致：源回到 RENDER_TARGET（世界层那一帧后续还要用）。
    transitionTextureTo(ctx, src, D3D12_RESOURCE_STATE_RENDER_TARGET);
    dbgLogDebug("sharpenTextureToTexture: %llux%llu -> %llux%llu amount=%.3f", srcDesc.Width,
                srcDesc.Height, dstDesc.Width, dstDesc.Height, amount);
    return {};
}

void presentSurface(Dx12Surface* s) {
    if (!s || !s->swapChain) {
        return;
    }
    // P109（#538 修复）状态约定：back buffer 的 RT→PRESENT 过渡**不能**在这里做——
    // 本函数跑在渲染线程上，调用时该帧的命令列表早已 Close+Execute（见
    // d3d12_device.cpp 的 renderThreadFunc：submitCommandList → presentSurface），
    // 往已关闭的列表里记 barrier 是非法的。正确的收口点在 blitSurface 尾部：那里在
    // 最后一次 draw 之后按跟踪状态 transitionTo(..., PRESENT) 并解绑 RTV。
    // 本函数只负责确认"看起来不合法"的情况并记账，不碰命令列表。
    // P95（叠帧/闪烁根因）：只有本帧真的 blit 过才 Present。vanilla 的 blit/present 门只看
    // 包装层的 hasBlittedTexture / isAcquired（com/mojang/blaze3d/systems/GpuSurface.java），
    // 与我们后端是否真的 acquire + blit 成功脱钩：P48 推迟 acquire 失败时
    // D3D12GpuSurface.ensureAcquired() 只是静默跳过 blit，vanilla 随后照旧 present()。
    // 此处若无条件 Present，DXGI 会 flip 到一张本帧从未写过的 back buffer —— 启动期是
    // 未初始化内存，稳态是 2~3 帧前的旧内容，表现为「上一帧和这一帧叠在一起 / 闪烁」，
    // 而挂在每次 Present 上的 OSD（RTSS/Afterburner）会在同一屏上画两遍文字（用户判据）。
    // 没有 blit 就没有本帧内容要显示：直接返回，让上一张已 blit 的帧继续留在屏幕上。
    // 这也避免对非当前 back buffer 做任何操作（P68 教训：写非当前 buffer 触发
    // D3D12 ERROR #907 → RemoveDevice(0x887A0005) 崩溃）。
    if (!s->blittedThisFrame || s->currentImageIndex < 0) {
        static unsigned long long skippedNoBlit = 0;
        ++skippedNoBlit;
        // 前 5 次与之后每 60 次记一行：这是"本帧没有内容可显示"的唯一直接证据。
        if (skippedNoBlit <= 5 || skippedNoBlit % 60 == 0) {
            dbgLogInfo("presentSurface: skipped (nothing blitted this frame) count=%llu idx=%d suboptimal=%d",
                       skippedNoBlit, s->currentImageIndex, static_cast<int>(s->suboptimal));
        }
        return;
    }
    // IMMEDIATE=0 -> Present(0, ALLOW_TEARING)；FIFO_RELAXED=3 -> Present(1, 0)；
    // 其余（FIFO=2 等）-> Present(1, 0)。
    //
    // P65（全屏掉帧根因）：flip model 下，**全屏**（独立翻转/独占）时 syncInterval=0
    // 但不带 DXGI_PRESENT_ALLOW_TEARING 的 Present 仍被 DXGI 同步到垂直刷新——
    // 实测 1920x1081 每帧 Present 阻塞 ~5.6ms（~150FPS），而窗口模式走 DWM 合成
    // Present 立即返回（~0.15ms）故无此问题。传入 ALLOW_TEARING 后全屏不再同步到
    // vblank（等价官方 Vulkan 的 IMMEDIATE present，实测官方全屏 1022FPS）。
    // 前提：swapchain 创建时带 ALLOW_TEARING（s->allowTearing），且 syncInterval==0。
    // P196（独占全屏冻结）：真独占下 swap chain 的所有权交给 output，DXGI 明确禁止在
    // fullscreen 时传 DXGI_PRESENT_ALLOW_TEARING（看起来 Present 仍返回 S_OK，但画面停在
    // 进全屏前那一帧 —— 用户报告的"应用独占全屏后画面冻结"）。全屏时一律 flags=0 且
    // syncInterval=1，交回 vsync 驱动翻转。
    const UINT syncInterval = (s->presentMode == 0 && !s->exclusive) ? 0 : 1;
    const UINT flags = (s->presentMode == 0 && s->allowTearing && !s->exclusive)
                           ? DXGI_PRESENT_ALLOW_TEARING
                           : 0;
    // P95：打印门从 `(currentImageIndex + 1) % 30 == 0` 改为"真实 present 计数"。
    // 旧门在 idx==-1 时恒真（每帧都打）、在健康的 3-buffer 链上恒假（从不打），语义正好
    // 反了：它只记录"没有 back buffer"的异常帧，正常帧一次也不记录。
    static unsigned long long presentLogCounter = 0;
    const bool logThisPresent = (++presentLogCounter % 30) == 0;
    // P15 诊断：每 30 帧打印 present 摘要（含 back buffer index + 结果）
    // P3.2 诊断：每帧检查 Backbuffer 格式和尺寸是否与窗口匹配
    if (!s->backBuffers.empty()) {
        ID3D12Resource* bb =
            s->backBuffers[static_cast<size_t>(s->currentImageIndex >= 0 ? s->currentImageIndex : 0)].Get();
        if (bb) {
            D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
            if (logThisPresent) {
                dbgLogInfo(
                    "presentSurface: idx=%d sync=%u suboptimal=%d bbFmt=%d bbW=%llu bbH=%llu winW=%u winH=%u",
                    s->currentImageIndex, syncInterval, static_cast<int>(s->suboptimal),
                    static_cast<int>(bbDesc.Format), static_cast<unsigned long long>(bbDesc.Width),
                    static_cast<unsigned long long>(bbDesc.Height), static_cast<unsigned>(s->width),
                    static_cast<unsigned>(s->height));
            }
        }
    }
    if (logThisPresent) {
        dbgLogInfo("presentSurface: idx=%d sync=%u flags=%u tearing=%d suboptimal=%d", s->currentImageIndex,
                   syncInterval, flags, static_cast<int>(s->allowTearing), static_cast<int>(s->suboptimal));
        // P196 诊断：独占全屏后画面冻结时，先看链是否真在 fullscreen、窗口尺寸/可见性是否正确。
        if (s->hwnd != 0) {
            BOOL fs = FALSE;
            s->swapChain->GetFullscreenState(&fs, nullptr);
            RECT wr{}, cr{};
            GetWindowRect(reinterpret_cast<HWND>(s->hwnd), &wr);
            GetClientRect(reinterpret_cast<HWND>(s->hwnd), &cr);
            dbgLogInfo("presentSurface: state fs=%d win=(%ld,%ld %ldx%ld) client=%ldx%ld visible=%d",
                       fs ? 1 : 0, static_cast<long>(wr.left), static_cast<long>(wr.top),
                       static_cast<long>(wr.right - wr.left), static_cast<long>(wr.bottom - wr.top),
                       static_cast<long>(cr.right - cr.left), static_cast<long>(cr.bottom - cr.top),
                       IsWindowVisible(reinterpret_cast<HWND>(s->hwnd)) ? 1 : 0);
        }
    }
    // P113：Reflex 要求 Present 之前 sleep（文档 ProgrammingGuideReflex.md §5）。
    mcdr_reflex_before_present();
    HRESULT hr = s->swapChain->Present(syncInterval, flags);
    // P267: only now that a real frame reached the screen do we show the child window that
    // carries it -- this is what keeps the startup from flashing an unpainted white/black face.
    if (s->host != nullptr && SUCCEEDED(hr) && !s->hostRevealed) {
        s->hostRevealed = true;
        mcdr_host_reveal(static_cast<MCDR_WindowHost*>(s->host));
        dbgLogInfo("[P267] seamless host revealed (first successful present)");
    }
    // P252：建链时记过一次 Present 槽，但 RTSS 也可能是链建立之后才把 hook 挂上/替换掉。
    // 在稳定出帧后（#1 / #120 / #600）再各记一次，dev 与用户环境可逐帧对比。
    {
        static std::atomic<int> p252PresentCounter{0};
        const int seen = p252PresentCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        if (seen == 1 || seen == 120 || seen == 600) {
            void** p252vtbl = *reinterpret_cast<void***>(s->swapChain.Get());
            if (p252vtbl != nullptr && p252vtbl[8] != nullptr) {
                HMODULE p252owner = nullptr;
                ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(p252vtbl[8]), &p252owner);
                std::wstring p252path(MAX_PATH, L'\0');
                const DWORD p252n =
                    p252owner != nullptr ? ::GetModuleFileNameW(p252owner, p252path.data(), MAX_PATH) : 0;
                p252path.resize(p252n);
                std::wstring_view p252sv(p252path);
                const size_t p252slash = p252sv.find_last_of(L"\\/");
                if (p252slash != std::wstring_view::npos) p252sv.remove_prefix(p252slash + 1);
                const auto* p252bytes = reinterpret_cast<const unsigned char*>(p252vtbl[8]);
                dbgLogInfo("P252 present #%d slot=%p owner=%ls bytes=%02X %02X %02X %02X", seen, p252vtbl[8],
                           p252sv.empty() ? L"(unknown)" : std::wstring(p252sv).c_str(), p252bytes[0],
                           p252bytes[1], p252bytes[2], p252bytes[3]);
            }
        }
    }
    mcdr_reflex_after_present();  // P115：PCL 的 ePresentEnd 标记
    // present 后 backbuffer 所有权已释放（vanilla 每帧 acquire->blit->present，
    // 若此处不重置，configureSurface 的 ResizeBuffers 会误以为仍有 acquired
    // backbuffer 而返回 DXGI_ERROR_NOT_CURRENTLY_AVAILABLE -> 画面冻结）。
    s->currentImageIndex = -1;
    // P95：本帧内容已随 Present 交给显示器；下一次 blit 之前都不应再 Present。
    s->blittedThisFrame.store(false, std::memory_order_release);
    // P95b：back buffer 已随 Present 交还 DXGI，本 surface 不再持有"未 present 的
    // back buffer"。configureSurface 只在这个标志为 true 时才做释放用的 Present(0,0)。
    s->bufferHeld.store(false, std::memory_order_release);
    if (hr == DXGI_STATUS_OCCLUDED) {
        dbgLogInfo("presentSurface: OCCLUDED (window fully occluded) -> suboptimal");
        s->suboptimal = true;
    } else if (hr == DXGI_STATUS_MODE_CHANGED) {
        dbgLogInfo("presentSurface: MODE_CHANGED -> suboptimal");
        s->suboptimal = true;
    } else if (FAILED(hr)) {
        dbgLogError("presentSurface: FAILED %s", hrText(hr).c_str());
        // P68：设备移除/重置时把真正的原因（GetDeviceRemovedReason + InfoQueue 最近消息）
        // 打进日志。未开调试层时，日志里只有一个 0x887A0005，看不出是「写了非当前
        // back buffer」（DXGI_ERROR_ACCESS_DENIED）还是真正的 GPU hang/TDR。
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            logDeviceStatus("presentSurface: FAILED");
        }
        s->suboptimal = true;
    } else {
        // 每帧成功路径：默认级别下静默（stderr/文件写位于帧关键路径上）。
        dbgLogDebug("presentSurface: ok (syncInterval=%u)", syncInterval);
        s->suboptimal = false;  // 正常 present 清除 suboptimal 标记
    }
}

// P197：HDR 模式切换（0 = auto / 1 = 强制开 / 2 = 强制关）。只写进程级状态，
// 真正生效在下次建链/重建链时 —— 换格式必须重建交换链，不能原地改。
void setHdrMode(int32_t mode) {
    mcdr_hdr_state_set_mode(mode);
    dbgLogInfo("[P197] HDR mode set to %d (applies on next swapchain creation)", mode);
}

int32_t hdrActiveFlag() { return mcdr_hdr_state_active(); }

void destroySurface(Dx12Surface* s) {
    if (!s) return;
    dbgLogDebug("destroySurface: enter surface=%p", reinterpret_cast<void*>(s));
    dbgLogProgress("[P184] destroySurface stage: enter");
    // P196：独占全屏必须在释放链之前退出，否则显示模式会停在独占状态（桌面黑屏 / 分辨率不回退）。
    if (s->swapChain && s->exclusive) {
        const HRESULT fsHr = s->swapChain->SetFullscreenState(FALSE, nullptr);
        s->exclusive = false;
        dbgLogInfo("destroySurface: left exclusive fullscreen hr=0x%08X", static_cast<unsigned>(fsHr));
    }
    // P33 修复：async render thread 可能在 destroySurface 期间仍在操作同一 swapchain。
    // 先等待渲染线程完成当前帧（仅同步，不销毁线程），再等 GPU 空闲，
    // 避免 deviceWaitIdle → queue->Signal 与渲染线程的 Present 并发访问 swapchain 导致崩溃。
    waitForRenderThreadSubmit();
    dbgLogProgress("[P184] destroySurface stage: waited render thread");
    (void)deviceWaitIdle();
    dbgLogProgress("[P184] destroySurface stage: device idle");
    // P96/P183：先立刻把父窗口刷黑 —— 退出时窗口消失前会短暂露出父窗口客户区，
    // 早一步上色就少一帧灰/白幕；刷黑只动 GDI，不依赖下面的 fence 等待。
    HWND presentChild = s->presentWindow;
    MCDR_WindowHost* const surfaceHost = static_cast<MCDR_WindowHost*>(s->host);
    s->host = nullptr;
    if (surfaceHost == nullptr) paintParentBlackBeforeDestroy(presentChild);
    dbgLogProgress("[P184] destroySurface stage: parent painted");
    // P33 fix：等待 per-surface present fence，确保 Display Controller 已完成
    // flip 并释放 backbuffer 引用。IMMEDIATE 模式 + FLIP_DISCARD 下，deviceWaitIdle
    // 不等待 display controller，backbuffer 仍可能被 DWM 异步读取。
    // P183：前面已经 deviceWaitIdle，绝大多数情况下 fence 早就完成了 —— 先走快路径
    // 直接查 GetCompletedValue，避免每次退出都白等满 5 秒（表现为"窗口先灰几秒"）。
    if (!s->surfacePresentFences.empty()) {
        UINT64 pv = 0;
        for (UINT64 v : s->surfacePresentFences)
            if (v > pv) pv = v;
        if (pv > 0) {
            DeviceContext& dc = deviceContext();
            if (dc.queueFence && dc.queueFence->GetCompletedValue() < pv) {
                HANDLE hEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (hEvt) {
                    HRESULT hr = dc.queueFence->SetEventOnCompletion(pv, hEvt);
                    if (SUCCEEDED(hr)) {
                        WaitForSingleObject(hEvt, 250);
                    }
                    CloseHandle(hEvt);
                }
            }
        }
    }
    // P33 fix：present fence 等 GPU 完成所有命令（含 present），此时再 flush
    // 延迟删除对象，确保不在 GPU 仍引用资源时释放它们（消除 PreSwapInfoQueue
    // CORRUPTION 警告的根本原因）。
    dbgLogProgress("[P184] destroySurface stage: fence wait done");
    flushPendingDeletes();
    dbgLogProgress("[P184] destroySurface stage: pending deletes flushed");
    if (getActiveSurface() == s) setActiveSurface(nullptr);
    // 先释放 swapchain（delete s），再销毁它的呈现宿主窗口，顺序不能反：
    // 窗口还活着时 DWM 仍持有该窗口的呈现资源引用。
    // presentChild 已在 fence 等待之前取出（P183），这里只清字段。
    s->presentWindow = nullptr;
    dbgLogProgress("[P184] destroySurface stage: deleting surface");
    delete s;
    dbgLogProgress("[P184] destroySurface stage: surface deleted");
    // P96：父窗口早在上面的 fence 等待之前就刷黑了（P183），这里只补日志与销毁子窗口。
    logWindowState("destroySurface child(before)", presentChild);
    logWindowState("destroySurface parent(before)", presentChild ? GetParent(presentChild) : nullptr);
    if (surfaceHost != nullptr) {
        mcdr_host_release(surfaceHost);  // 最后一个引用时恢复父窗口过程并销毁子窗口
    } else if (presentChild != nullptr) {
        DestroyWindow(presentChild);
    }
    logWindowState("destroySurface parent(after)", presentChild ? GetParent(presentChild) : nullptr);
    dbgLogDebug("destroySurface: done surface=%p", reinterpret_cast<void*>(s));
}

// 仅清理表面，不调用 deviceWaitIdle（供 C++ 内部使用：render thread 已自行退出后调用）
void destroySurfaceNoWaitIdle(Dx12Surface* s) {
    if (!s) return;
    if (getActiveSurface() == s) setActiveSurface(nullptr);
    HWND presentChild = s->presentWindow;
    s->presentWindow = nullptr;
    MCDR_WindowHost* const surfaceHost = static_cast<MCDR_WindowHost*>(s->host);
    s->host = nullptr;
    dbgLogProgress("[P184] destroySurface stage: deleting surface");
    delete s;
    dbgLogProgress("[P184] destroySurface stage: surface deleted");
    // P96：同 destroySurface —— 先刷黑父窗口再销毁子窗口（见该处说明）。
    if (surfaceHost != nullptr) {
        mcdr_host_release(surfaceHost);
    } else {
        paintParentBlackBeforeDestroy(presentChild);
        if (presentChild != nullptr) DestroyWindow(presentChild);
    }
}

// P6 诊断：读回 back buffer 采样像素。内部先等 GPU 完全空闲（同步一帧），
// 用一次性命令列表拷贝到 readback staging，Map 后打印 3x3 网格 RGBA。
// 每 ~60 帧调用一次，同步开销可忽略。结论判读：
//   纯红/纯黑/纯灰 = 画面只有 clear 色（绘制内容不可见/未生效）
//   多色且中心有内容 = 渲染正常，问题在别处（blit 区域/尺寸等）。
mcdr::Result<void> readbackSurfacePixels(Dx12Surface* s) {
    DeviceContext& ctx = deviceContext();
    if (!ctx.device || !ctx.queue) {
        return mcdr::err("readbackSurfacePixels", "device not initialized");
    }
    if (!s || s->backBuffers.empty()) {
        return mcdr::err("readbackSurfacePixels", "surface has no back buffers");
    }
    {
        auto idleRes = deviceWaitIdle();
        if (!idleRes) {
            return mcdr::err("readbackSurfacePixels",
                             "deviceWaitIdle failed: " + idleRes.error().message);
        }
    }

    // 选择要读回的 back buffer：
    // 1. 优先 currentImageIndex：表示当前已 acquire、本帧正在使用的 backBuffer。
    //    游戏可能直接通过 beginRenderPass 渲染到表面纹理而非走 blit 路径，
    //    此时 lastBlitIndex 是旧帧的残留值，用它会读到过时数据（黑屏/错误内容）。
    // 2. 其次 lastBlitIndex：当 currentImageIndex == -1（present 后尚未 acquire
    //    新帧）时，用最近一次 blit 的 backBuffer 作为兜底，保证测试自检能读到
    //    真实画面——GetCurrentBackBufferIndex 会跳到下一帧未写入的 buffer（全 0 假黑屏）。
    int idx = s->currentImageIndex;
    if (idx < 0 || idx >= static_cast<int>(s->backBuffers.size())) {
        if (s->lastBlitIndex >= 0 && s->lastBlitIndex < static_cast<int>(s->backBuffers.size())) {
            idx = s->lastBlitIndex;
        } else {
            idx = static_cast<int>(s->swapChain->GetCurrentBackBufferIndex());
        }
    }
    if (idx < 0 || idx >= static_cast<int>(s->backBuffers.size())) {
        return mcdr::err("readbackSurfacePixels",
                         "no valid back buffer for readback (idx=" + std::to_string(idx) + ")");
    }
    ID3D12Resource* bb = s->backBuffers[static_cast<size_t>(idx)].Get();
    D3D12_RESOURCE_DESC bd = bb->GetDesc();
    UINT w = static_cast<UINT>(bd.Width), h = bd.Height;
    UINT64 rowBytes = static_cast<UINT64>(w) * 4;
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
        if (FAILED(ctx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&staging)))) {
            return mcdr::err("readbackSurfacePixels", "CreateCommittedResource(readback staging) failed");
        }
    }

    static ComPtr<ID3D12CommandAllocator> alloc;
    static ComPtr<ID3D12GraphicsCommandList> cl;
    if (!alloc) {
        if (FAILED(
                ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) {
            return mcdr::err("readbackSurfacePixels", "CreateCommandAllocator failed");
        }
    }
    if (!cl) {
        if (FAILED(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                 IID_PPV_ARGS(&cl)))) {
            return mcdr::err("readbackSurfacePixels", "CreateCommandList failed");
        }
    } else {
        alloc->Reset();
        cl->Reset(alloc.Get(), nullptr);
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = staging.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1, &b);
    cl->Close();

    ID3D12CommandList* lists[] = {cl.Get()};
    ctx.queue->ExecuteCommandLists(1, lists);
    UINT64 fv = ++ctx.queueFenceValue;
    if (FAILED(ctx.queue->Signal(ctx.queueFence.Get(), fv))) {
        return mcdr::err("readbackSurfacePixels", "Signal(readback) failed");
    }
    {
        auto fenceRes = waitForQueueFenceValue(fv, 5'000'000'000ULL);
        if (!fenceRes) {
            return mcdr::err("readbackSurfacePixels",
                             "readback wait timeout: " + fenceRes.error().message);
        }
    }

    void* ptr = nullptr;
    if (FAILED(staging->Map(0, nullptr, &ptr))) {
        return mcdr::err("readbackSurfacePixels", "staging Map failed");
    }
    const uint8_t* base = static_cast<const uint8_t*>(ptr);
    int xs[3] = {0, static_cast<int>(w) / 2, static_cast<int>(w) - 1};
    int ys[3] = {0, static_cast<int>(h) / 2, static_cast<int>(h) - 1};
    for (int yi = 0; yi < 3; ++yi) {
        for (int xi = 0; xi < 3; ++xi) {
            const uint8_t* p = base + static_cast<UINT64>(ys[yi]) * pitch + static_cast<UINT64>(xs[xi]) * 4;
            dbgLogDebug("readback[%ux%u] (%d,%d) = RGBA(%3d,%3d,%3d,%3d)", w, h, xs[xi], ys[yi], p[0], p[1], p[2],
                   p[3]);
        }
    }
    // P6 可视化：整帧 dump 成 BMP + ASCII 缩略图，直接看 backbuffer 实际画面。
    dbgDumpPixelsToFile(base, w, h, pitch, "backbuf");
    staging->Unmap(0, nullptr);
    return {};
}

}  // namespace d3d12_mc
