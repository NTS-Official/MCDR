// d3d12_surface_opts_state.cpp — P251：交换链建链选项的进程级状态，普通 TU（非模块单元）。
//
// 两个 atomic：是否允许 ALLOW_TEARING 建链、是否强制把链建在自建呈现子窗口上。JVM 侧配置
// 经 mcdr_dx12SetSwapchainOptions 推来，surface 在建链时读。理由见
// include\mcdr_surface_opts_state.h 的注释。
#include <atomic>
#include <cstdint>

#include "mcdr_surface_opts_state.h"

namespace {

std::atomic<int32_t> gAllowTearing{1};       // 默认允许（能进 independent flip / VRR）
std::atomic<int32_t> gForcePresentChild{1};  // P267：默认用 SeamlessWindowHost 子窗口建链

}  // namespace

extern "C" {

void mcdr_surface_opts_set_allow_tearing(int32_t allow) {
    gAllowTearing.store(allow != 0 ? 1 : 0, std::memory_order_relaxed);
}

int32_t mcdr_surface_opts_allow_tearing(void) {
    return gAllowTearing.load(std::memory_order_relaxed);
}

void mcdr_surface_opts_set_force_present_child(int32_t force) {
    gForcePresentChild.store(force != 0 ? 1 : 0, std::memory_order_relaxed);
}

int32_t mcdr_surface_opts_force_present_child(void) {
    return gForcePresentChild.load(std::memory_order_relaxed);
}

}  // extern "C"
