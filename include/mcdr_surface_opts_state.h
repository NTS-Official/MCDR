// mcdr_surface_opts_state.h — P251：交换链建链选项（由 JVM 侧配置推来）。
//
// 两个开关：
//   * allow_tearing       —— 是否允许建 FLIP_DISCARD+ALLOW_TEARING 的链（关掉 = 强制走
//                            DWM 合成翻页；第三方覆盖层如 RTSS/Afterburner 的 OSD 只在
//                            合成翻页下画得出来）。
//   * force_present_child —— 是否优先把链建在自建呈现子窗口上（P196 之后默认建在 SDL
//                            顶层窗口上）。
//
// 为什么单拎一个普通 TU：surface 是模块单元，模块 purview 里的定义无法被普通 TU（ABI
// 层）链接，故状态放在一个普通翻译单元（src\core\d3d12_surface_opts_state.cpp）里，经这
// 组 C 链接函数共享（与 mcdr_hdr_state.h 同一套路）。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0 = 不允许 tearing（只用 FLIP_DISCARD / FLIP_SEQUENTIAL），非 0 = 允许（默认）。
void mcdr_surface_opts_set_allow_tearing(int32_t allow);
int32_t mcdr_surface_opts_allow_tearing(void);

// 0 = 先试顶层窗口、失败再回退子窗口（默认），非 0 = 直接建在自建子窗口上。
void mcdr_surface_opts_set_force_present_child(int32_t force);
int32_t mcdr_surface_opts_force_present_child(void);

#ifdef __cplusplus
}
#endif
