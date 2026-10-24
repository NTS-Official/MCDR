// P117：光追可见性分层的跨 TU 状态。
//
// 设计见 _research/p117_rt_visibility.md。这里只放“纯 C 的旋钮与统计”——和 mcdr_hdr_state.h 同样的
// 理由：`d3d12_dlss.cpp` 是普通 TU、`d3d12_device.cpp` 是模块实现单元，两边都要读同一个状态，
// 走模块 purview 会牵动导出边界，用一批 atomic 最省事。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0 = off（默认），1 = ao，2 = ao_shadows，3 = full。非光追机器上设置也不会崩，只是不生效。
void mcdr_rt_state_set_mode(int32_t mode);
int32_t mcdr_rt_state_mode(void);

// G-Buffer（P117a）是否已经按当前尺寸创建成功。着色器注入与合成都要先看这一位。
void mcdr_rt_state_set_gbuffer_ready(int32_t ready);
int32_t mcdr_rt_state_gbuffer_ready(void);

// 最近一次创建的 G-Buffer 尺寸（诊断用）。
void mcdr_rt_state_set_gbuffer_size(int32_t width, int32_t height);
int32_t mcdr_rt_state_gbuffer_width(void);
int32_t mcdr_rt_state_gbuffer_height(void);

#ifdef __cplusplus
}
#endif
