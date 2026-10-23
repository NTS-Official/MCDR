// mcdr_hdr_state.h — P197（原生 HDR）的进程级状态。
//
// 为什么单独一个小 TU：HDR 开关由 JVM 侧配置推来（`mcdr_dx12SetHdrMode`），
// 由 surface 单元在创建 swapchain 时消费，还要被 DLSS 单元读去填
// `sl::DLSSOptions::colorBuffersHDR`。surface 与 DLSS 分属「模块单元」与
// 「普通 TU」，模块 purview 里的定义无法被普通 TU 链接，因此状态放在一个普通
// 翻译单元（src\core\d3d12_hdr_state.cpp）里，经这组 C 链接函数共享。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0 = auto（按 DXGI 输出当前色彩空间判定），1 = 强制开，2 = 强制关。
void mcdr_hdr_state_set_mode(int32_t mode);
int32_t mcdr_hdr_state_mode(void);

// 最近一次创建的 surface 是否真的以 scRGB 呈现（0/1）。
void mcdr_hdr_state_set_active(int32_t active);
int32_t mcdr_hdr_state_active(void);

// 最近一次建链时**真正用掉**的模式（0/1/2）。JVM 侧拿它和当前选项比较，判断是否需要重启：
// 换格式必须重建交换链，而我们在运行时只重建、不换格式（P197）。
// 初值 -1 = 还没建过链（此时不报“需要重启”）。
void mcdr_hdr_state_set_applied_mode(int32_t mode);
int32_t mcdr_hdr_state_applied_mode(void);

#ifdef __cplusplus
}
#endif
