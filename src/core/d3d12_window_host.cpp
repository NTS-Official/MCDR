// d3d12_window_host.cpp — P267 SeamlessWindowHost：呈现子窗口宿主实现（普通 TU）。
//
// 设计目标（用户 m04532 的规格）：
//   1. 物理隔离 GL：承载 swapchain 的 HWND 从创建到销毁都不碰 OpenGL；建链前后
//      临时 wglMakeCurrent(nullptr, nullptr) 再恢复，避免 DXGI 看到"被 GL 用过的窗口"。
//   2. 输入零侵入：子窗口 WS_EX_TRANSPARENT + WM_NCHITTEST=HTTRANSPARENT，
//      键盘/鼠标焦点永远留在 GLFW 主窗口。
//   3. 无白屏/黑屏闪烁：子窗口**创建后先不显示**，第一帧 Present 成功才 SW_SHOWNA。
//   4. 几何同步：父窗口被子类化，WM_SIZE 里按客户区尺寸 1:1 调整子窗口，并维护
//      最小化标记供 Present 跳过。
//
// 子窗口位置/尺寸用**客户区坐标**表达，因此天然扣除了标题栏与边框，不需要额外补偿。
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "mcdr_window_host.h"

namespace {

constexpr wchar_t kHostClass[] = L"MCDR_D3D12_SeamlessHost";
constexpr wchar_t kHostProp[] = L"MCDR_D3D12_SeamlessHostState";
constexpr UINT kMsgReveal = WM_APP + 0x4D1;  // 渲染线程 -> 窗口线程：显示子窗口

}  // namespace

// 头文件里是前置声明（opaque），这里给定义。单实例即可：游戏只有一个主窗口，
// 自检 surface 与主 surface 共享同一个子窗口，用 refs 记引用。
struct MCDR_WindowHost {
    HWND parent = nullptr;
    HWND child = nullptr;
    WNDPROC parentProc = nullptr;
    std::atomic<int32_t> minimized{0};
    std::atomic<int32_t> revealed{0};
    std::atomic<int32_t> refs{0};
    int width = 1;
    int height = 1;
};

namespace {

MCDR_WindowHost* gHost = nullptr;
HDC gSavedDc = nullptr;
HGLRC gSavedGlrc = nullptr;

// ---- GL 上下文隔离：动态取 opengl32!wgl*，不引入链接依赖 ----------------------
struct GlApi {
    BOOL(WINAPI* makeCurrent)(HDC, HGLRC) = nullptr;
    HDC(WINAPI* getCurrentDC)(void) = nullptr;
    HGLRC(WINAPI* getCurrentContext)(void) = nullptr;
};

const GlApi& glApi() {
    static const GlApi api = []() {
        GlApi a{};
        HMODULE mod = ::GetModuleHandleW(L"opengl32.dll");
        if (mod == nullptr) mod = ::LoadLibraryW(L"opengl32.dll");
        if (mod != nullptr) {
            a.makeCurrent =
                reinterpret_cast<BOOL(WINAPI*)(HDC, HGLRC)>(::GetProcAddress(mod, "wglMakeCurrent"));
            a.getCurrentDC =
                reinterpret_cast<HDC(WINAPI*)(void)>(::GetProcAddress(mod, "wglGetCurrentDC"));
            a.getCurrentContext = reinterpret_cast<HGLRC(WINAPI*)(void)>(
                ::GetProcAddress(mod, "wglGetCurrentContext"));
        }
        return a;
    }();
    return api;
}

void resizeChild(MCDR_WindowHost* host, int width, int height) {
    if (host == nullptr || host->child == nullptr) return;
    if (width <= 0 || height <= 0) return;
    host->width = width;
    host->height = height;
    // SWP_NOCOPYBITS|SWP_DEFERERASE：resize 时不要先搬旧内容再擦，否则会闪一帧。
    ::SetWindowPos(host->child, nullptr, 0, 0, width, height,
                   SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_DEFERERASE);
}

// 子窗口消息处理：三条"必须" + 第一帧显示。
LRESULT CALLBACK hostChildProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST:
        // 命中测试穿透到父窗口，否则铺满客户区的子窗口会把鼠标消息全吃掉。
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        // 不擦背景：系统擦除会在 swapchain 重画前整片闪黑。
        return 1;
    case WM_MOUSEACTIVATE:
        // 不抢激活焦点（否则点游戏窗口不会激活它）。
        return MA_NOACTIVATE;
    case WM_PAINT: {
        // 子窗口内容只来自 swapchain；显示前收到 WM_PAINT 时给个空绘制，避免白面。
        PAINTSTRUCT ps{};
        ::BeginPaint(hwnd, &ps);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case kMsgReveal: {
        MCDR_WindowHost* host = gHost;
        if (host != nullptr && host->child == hwnd && host->revealed.exchange(1) == 0) {
            ::ShowWindow(hwnd, SW_SHOWNA);
            ::SetWindowPos(hwnd, HWND_TOP, 0, 0, host->width, host->height,
                           SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        return 0;
    }
    default:
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// 父窗口子类化：只关心尺寸与可见性，其余原样转给原窗口过程。
LRESULT CALLBACK hostParentProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MCDR_WindowHost* host = static_cast<MCDR_WindowHost*>(::GetPropW(hwnd, kHostProp));
    if (host == nullptr) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            host->minimized.store(1, std::memory_order_relaxed);
        } else {
            host->minimized.store(0, std::memory_order_relaxed);
            resizeChild(host, static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
        }
        break;
    case WM_SHOWWINDOW:
        host->minimized.store(wParam != 0 ? 0 : 1, std::memory_order_relaxed);
        break;
    case WM_NCDESTROY: {
        WNDPROC prev = host->parentProc;
        ::RemovePropW(hwnd, kHostProp);
        host->parentProc = nullptr;
        host->parent = nullptr;
        if (prev != nullptr) return ::CallWindowProcW(prev, hwnd, msg, wParam, lParam);
        break;
    }
    default:
        break;
    }
    if (host->parentProc == nullptr) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    return ::CallWindowProcW(host->parentProc, hwnd, msg, wParam, lParam);
}

bool registerHostClass() {
    static const bool registered = []() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        // 不能带 CS_OWNDC：那会让类与 GDI/GL 绑定；也不能设背景刷子。
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = hostChildProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kHostClass;
        if (::RegisterClassExW(&wc) != 0) return true;
        return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

// 销毁子窗口与子类化（最后一个引用释放时调用）。
void teardownHost(MCDR_WindowHost* host) {
    if (host == nullptr) return;
    if (host->parent != nullptr && host->parentProc != nullptr &&
        ::IsWindow(host->parent) != 0) {
        ::SetWindowLongPtrW(host->parent, GWLP_WNDPROC,
                            reinterpret_cast<LONG_PTR>(host->parentProc));
        ::RemovePropW(host->parent, kHostProp);
    }
    host->parentProc = nullptr;
    if (host->child != nullptr && ::IsWindow(host->child) != 0) {
        ::DestroyWindow(host->child);
    }
    host->child = nullptr;
}

}  // namespace

extern "C" {

MCDR_WindowHost* mcdr_host_create(void* parentHwnd, int32_t width, int32_t height) {
    HWND parent = reinterpret_cast<HWND>(parentHwnd);
    if (parent == nullptr || ::IsWindow(parent) == 0) return nullptr;
    if (gHost != nullptr) {
        // 复用：同一个父窗口（主 surface 与自检 surface 共用）。
        if (gHost->parent == parent && gHost->child != nullptr) {
            gHost->refs.fetch_add(1, std::memory_order_relaxed);
            resizeChild(gHost, static_cast<int>(width), static_cast<int>(height));
            return gHost;
        }
        return nullptr;
    }
    if (!registerHostClass()) return nullptr;

    MCDR_WindowHost* host = new MCDR_WindowHost();
    host->parent = parent;
    host->width = width > 0 ? static_cast<int>(width) : 1;
    host->height = height > 0 ? static_cast<int>(height) : 1;
    // 样式：WS_CHILD | WS_CLIPSIBLINGS（**没有 WS_VISIBLE**，等第一帧 Present）。
    // 扩展样式：WS_EX_TRANSPARENT（绘制顺序/命中让位）+ WS_EX_NOPARENTNOTIFY（不打扰父窗）。
    host->child = ::CreateWindowExW(WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY, kHostClass,
                                    L"MCDR D3D12 seamless host", WS_CHILD | WS_CLIPSIBLINGS, 0, 0,
                                    host->width, host->height, parent, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
    if (host->child == nullptr) {
        delete host;
        return nullptr;
    }
    ::SetPropW(parent, kHostProp, reinterpret_cast<HANDLE>(host));
    host->parentProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        parent, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hostParentProc)));
    if (host->parentProc == nullptr) {
        ::RemovePropW(parent, kHostProp);
        ::DestroyWindow(host->child);
        delete host;
        return nullptr;
    }
    host->refs.store(1, std::memory_order_relaxed);
    gHost = host;
    return host;
}

void* mcdr_host_child(MCDR_WindowHost* host) {
    return host != nullptr ? reinterpret_cast<void*>(host->child) : nullptr;
}

void mcdr_host_resize(MCDR_WindowHost* host, int32_t width, int32_t height) {
    resizeChild(host, static_cast<int>(width), static_cast<int>(height));
}

void mcdr_host_reveal(MCDR_WindowHost* host) {
    if (host == nullptr || host->child == nullptr) return;
    // 渲染线程调用：投递到窗口线程，避免跨线程 ShowWindow。
    ::PostMessageW(host->child, kMsgReveal, 0, 0);
}

int32_t mcdr_host_minimized(MCDR_WindowHost* host) {
    if (host == nullptr) return 0;
    return host->minimized.load(std::memory_order_relaxed);
}

int32_t mcdr_host_suspend_gl(void) {
    const GlApi& api = glApi();
    if (api.makeCurrent == nullptr || api.getCurrentContext == nullptr ||
        api.getCurrentDC == nullptr) {
        return 0;
    }
    HGLRC rc = api.getCurrentContext();
    if (rc == nullptr) return 0;
    HDC dc = api.getCurrentDC();
    if (dc == nullptr) return 0;
    if (api.makeCurrent(nullptr, nullptr) == FALSE) return 0;
    gSavedDc = dc;
    gSavedGlrc = rc;
    return 1;
}

void mcdr_host_restore_gl(void) {
    if (gSavedGlrc == nullptr) return;
    const GlApi& api = glApi();
    if (api.makeCurrent != nullptr) api.makeCurrent(gSavedDc, gSavedGlrc);
    gSavedDc = nullptr;
    gSavedGlrc = nullptr;
}

void mcdr_host_release(MCDR_WindowHost* host) {
    if (host == nullptr) return;
    if (host->refs.fetch_sub(1, std::memory_order_acq_rel) > 1) return;
    if (gHost == host) gHost = nullptr;
    teardownHost(host);
    delete host;
}

}  // extern "C"
