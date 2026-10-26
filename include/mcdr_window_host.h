// mcdr_window_host.h — P267 SeamlessWindowHost：D3D12 呈现子窗口宿主（C 接口）。
//
// 为什么要有这一层：RTSS/Afterburner 这类注入式叠加层在 FML 的早期加载窗口（GL）上
// 能画 OSD，等 NeoForge 销毁它、改由 SDL 建主窗口、再由我们把 swapchain 挂到那个顶层
// 窗口上之后，OSD 就再也不出现。把它挂到一个**我们自己创建的、从不接触 GL 的子窗口**
// 上（历史 P196 之前的做法）是唯一能稳定观察到的差异点。
//
// 本模块只负责窗口本身，不碰 D3D12：
//   * 子窗口 1:1 贴合父窗口客户区，样式 WS_CHILD | WS_CLIPSIBLINGS，初始**不可见**；
//   * 父窗口被窗口子类化，WM_SIZE / WM_SHOWWINDOW 里同步子窗口几何与最小化状态；
//   * 子窗口的 WM_NCHITTEST 返回 HTTRANSPARENT，鼠标/键盘焦点始终留在父窗口（GLFW）；
//   * 第一帧真正 Present 成功之后由调用方 mcdr_host_reveal() 显示，避免启动黑/白闪烁；
//   * mcdr_host_suspend_gl()/restore_gl() 在 CreateSwapChainForHwnd 前后临时解绑当前
//     GL 上下文，保证 DXGI 拿到的 HWND 从未被 GL 挂过（"物理隔离 OpenGL 污染"）。
//
// 该接口是普通 C ABI，供模块单元 d3d12_surface.cpp 在全局模块片段里 include。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 不透明句柄（内部是 Host 结构体）。同一个父窗口重复 attach 会复用同一个子窗口，
// 引用计数归零时销毁；因此自检 surface 与主 surface 可以共享一个宿主。
typedef struct MCDR_WindowHost MCDR_WindowHost;

// 创建（或复用）父窗口 parentHwnd 的宿主。width/height 为**客户区**尺寸。
// 成功返回非空句柄；父窗口无效或 CreateWindowEx 失败返回空。
MCDR_WindowHost* mcdr_host_create(void* parentHwnd, int32_t width, int32_t height);

// 返回承载 swapchain 的子窗口 HWND（MCDR_WindowHost 的内部成员）。
void* mcdr_host_child(MCDR_WindowHost* host);

// 客户区尺寸变化时同步子窗口几何（WM_SIZE 已自动调用一次，这里用于 configureSurface）。
void mcdr_host_resize(MCDR_WindowHost* host, int32_t width, int32_t height);

// 第一帧 Present 成功后调用：显示子窗口（SW_SHOWNA，不抢焦点）。幂等。
void mcdr_host_reveal(MCDR_WindowHost* host);

// 父窗口是否处于最小化/隐藏状态（Present 应跳过，避免 DXGI 丢弃帧）。
int32_t mcdr_host_minimized(MCDR_WindowHost* host);

// 解绑当前 GL 上下文（有上下文时返回 1），restore 时恢复。用于建链前后的物理隔离。
int32_t mcdr_host_suspend_gl(void);
void mcdr_host_restore_gl(void);

// 释放一个引用；最后一个引用归零时恢复父窗口过程并销毁子窗口。
void mcdr_host_release(MCDR_WindowHost* host);

#ifdef __cplusplus
}
#endif
