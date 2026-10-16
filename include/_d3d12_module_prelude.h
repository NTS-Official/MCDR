#pragma once

// dx12-mc 模块前置头（模块迁移专用）
//
// 用途：本项目的 .cppm 模块接口单元在「全局模块片段」(global module fragment)
// 中统一 #include 本文件。全局模块片段里 #include 的声明归属于全局模块，不进入
// 任何具名模块的 purview，因此：
//   * 它们可以出现在模块接口中而不违反「模块不得导出宏 / 不得导出全局模块实体」；
//   * 所有头文件宏（NOMINMAX、WIN32_LEAN_AND_MEAN 等）在任何 d3d12/dxgi 头之前
//     就已定义，与迁移前的头文件行为完全一致；
//   * 多个模块单元各自 #include 本文件不会产生 ODR 冲突（头文件有 include guard，
//     且全局模块实体在所有 TU 中是同一实体）。
//
// 注意：这里必须保持为「头文件 + #include」，不要改成 import std;。
// 原因有两条，第二条是硬约束：
//   1. 项目内其余尚未迁移的 .cpp TU 仍在使用 #include <...>，同一个 TU 里混用
//      #include <vector> 与 import std; 是非法组合（C2011 重定义 / C2953 /
//      致命错误 C1116），所以必须整体迁移，不能逐个文件切。
//   2. import std; 只能出现在「全局模块片段」之外的模块 purview 里。本文件的
//      全部内容都在全局模块片段中（module; 之后、export module 之前），在那里
//      写 import std; 会被判为「全局模块片段仅可包含预处理器指令」（C5202）。
// 也就是说：本前置头与 import std; 在结构上互斥，与是否开启 ISO C++23 标准库
// 模块无关。详见 known_bugs.md 的 B2/B3 两节（含实测边界与迁移清单）。

// 禁用 Windows min/max 宏。必须位于任何 windows.h/d3d12.h 之前。
// 项目已在 MCDR.vcxproj 全局定义 NOMINMAX，此处再守一道，使本文件脱离项目
// 属性表单独编译时也安全（TreatWarningAsError=true 下，宏 min/max 与
// std::min/std::max 相撞是硬错误 C2589/C2059）。
#ifndef NOMINMAX
#define NOMINMAX
#endif

// ---------------------------------------------------------------------------
// Windows / COM / D3D12 / DXGI
// ---------------------------------------------------------------------------
#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>  // ID3D12InfoQueue / ID3D12Debug
#include <d3dcompiler.h>     // D3DCompile（vs_5_1 / ps_5_1 图形管线）
#include <dcomp.h>           // DirectComposition（composition swapchain 回退路径）
#include <dxcore_interface.h> // IDXCoreAdapterFactory / DXCoreAdapterProperty（adapterInfoJson 用）
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>       // IDXGIFactory6 / DXGI_GPU_PREFERENCE（按 GPU 偏好选适配器）
#include <wrl/client.h>

// ---------------------------------------------------------------------------
// C 运行库
// ---------------------------------------------------------------------------
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>  // mcdr::Result<T>（mcdr_error.cppm）；<string>/<utility> 见下方标准库段

// ---------------------------------------------------------------------------
// C++ 标准库
// ---------------------------------------------------------------------------
#include <array>
#include <atomic>
#include <climits>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 高频调试宏（迁移前定义在 dx12_device.h 内）
// ---------------------------------------------------------------------------
// 【为什么放在这里】具名模块不导出宏，所以 DBG_LOG_DEBUG 无法随 mcdr.d3d12.device
// 一起导出。它唯一的使用者 MCDR\src\c_abi\abi_command.cpp 也不该为了一个宏去
// import 整个设备模块，因此该宏随本前置头一起 include（全局模块片段里的宏对
// 包含它的 TU 全程可见）。
//
// 【为什么展开必须写全命名空间】宏展开必须写成 ::d3d12_mc::dbgLogDebug —— 否则在
// namespace 之外使用该宏的 TU（例如 MCDR\src\c_abi\abi_command.cpp 的
// mcdr_dx12CopyBuffer）在 DX12_DEBUG_VERBOSE 打开时会报 C3861「找不到标识符」。
//
// 【为什么这里要 #ifndef 守卫】头部（生成自 dx12_device.h）里已有一份等价定义，
// 保留守卫即可让两种包含顺序都不产生重定义告警（TreatWarningAsError=true 下
// 宏重定义是硬错误）。
#ifndef DBG_LOG_DEBUG
#ifdef DX12_DEBUG_VERBOSE
#define DBG_LOG_DEBUG(...) ::d3d12_mc::dbgLogDebug(__VA_ARGS__)
#else
#define DBG_LOG_DEBUG(...) ((void)0)
#endif
#endif
