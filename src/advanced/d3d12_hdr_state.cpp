// d3d12_hdr_state.cpp — P197（原生 HDR）的进程级状态，普通 TU（非模块单元）。
//
// 只有两个 atomic：HDR 模式（配置推来）与「当前 surface 是否真的以 scRGB 呈现」
// （surface 创建时写，DLSS 选 colorBuffersHDR 时读）。刻意不放进模块单元，理由见
// include\mcdr_hdr_state.h 的注释。
#include <atomic>
#include <cstdint>

#include "mcdr_hdr_state.h"

namespace {

std::atomic<int32_t> gHdrMode{0};         // 0 auto / 1 on / 2 off
std::atomic<int32_t> gHdrActive{0};       // 0/1
std::atomic<int32_t> gHdrAppliedMode{-1}; // 建链时用掉的模式，-1 = 还没建过链

}  // namespace

extern "C" {

void mcdr_hdr_state_set_mode(int32_t mode) {
    if (mode < 0 || mode > 2) mode = 0;
    gHdrMode.store(mode, std::memory_order_relaxed);
}

int32_t mcdr_hdr_state_mode(void) { return gHdrMode.load(std::memory_order_relaxed); }

void mcdr_hdr_state_set_applied_mode(int32_t mode) {
    gHdrAppliedMode.store(mode, std::memory_order_relaxed);
}

int32_t mcdr_hdr_state_applied_mode(void) {
    return gHdrAppliedMode.load(std::memory_order_relaxed);
}

void mcdr_hdr_state_set_active(int32_t active) {
    gHdrActive.store(active != 0 ? 1 : 0, std::memory_order_relaxed);
}

int32_t mcdr_hdr_state_active(void) { return gHdrActive.load(std::memory_order_relaxed); }

}  // extern "C"
