// P117：光追可见性分层的跨 TU 状态（纯 C，无 D3D12 依赖）。
#include "mcdr_rt_state.h"

#include <atomic>

namespace {
std::atomic<int32_t> gRtMode{0};
std::atomic<int32_t> gGbufferReady{0};
std::atomic<int32_t> gGbufferWidth{0};
std::atomic<int32_t> gGbufferHeight{0};
}  // namespace

extern "C" void mcdr_rt_state_set_mode(int32_t mode) {
    gRtMode.store(mode, std::memory_order_relaxed);
}

extern "C" int32_t mcdr_rt_state_mode(void) {
    return gRtMode.load(std::memory_order_relaxed);
}

extern "C" void mcdr_rt_state_set_gbuffer_ready(int32_t ready) {
    gGbufferReady.store(ready, std::memory_order_relaxed);
}

extern "C" int32_t mcdr_rt_state_gbuffer_ready(void) {
    return gGbufferReady.load(std::memory_order_relaxed);
}

extern "C" void mcdr_rt_state_set_gbuffer_size(int32_t width, int32_t height) {
    gGbufferWidth.store(width, std::memory_order_relaxed);
    gGbufferHeight.store(height, std::memory_order_relaxed);
}

extern "C" int32_t mcdr_rt_state_gbuffer_width(void) {
    return gGbufferWidth.load(std::memory_order_relaxed);
}

extern "C" int32_t mcdr_rt_state_gbuffer_height(void) {
    return gGbufferHeight.load(std::memory_order_relaxed);
}
