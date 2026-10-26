// abi_device.cpp — device, resource and hidden-window entry points
//
// Group: device surface (originally abi_bridge.cpp).
// 合并自：abi_bridge.cpp
//
module;

#include "_d3d12_module_prelude.h"

module mcdr.abi.common;

#include "mcdr_abi.h"   // public C ABI：extern "C" 声明无法从模块导出，故保持头文件

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.d3d12.async_bundle;
import mcdr.d3d12.async_descriptor;
import mcdr.d3d12.async_executor;
import mcdr.d3d12.async_fence;

using namespace d3d12_mc;

// ===========================================================================
// 本文件是合并后的 C ABI 适配单元，按**功能**命名（不再按移植阶段 pX）。
// 共享前言（prelude / mcdr_abi.h / 各模块 import）统一放在文件顶部，下面每一段
// 保留原单元的文件头注释与代码本体，内容逐字节未改。
// ===========================================================================

// ---------------------------------------------------------------------------
// 来源单元：abi_bridge.cpp
// ---------------------------------------------------------------------------

// abi_device.cpp — device, resource and hidden-window entry points.（合并自 abi_bridge.cpp）
//
// Replaces jni_bridge.cpp (141 lines). This is the one place the old bridge had
// to invent an error channel: dx12CreateDevice returned a String, so a failure
// could only be signalled by prefixing it with "ERROR: ". That convention is
// preserved here (the Java side still branches on the prefix) because the
// signature is unchanged, but everything else now reports through the
// thread-local last-error string like the rest of the ABI.
//
// Also note what disappeared: dx12MapBuffer used to return
// env->NewDirectByteBuffer(ptr, length). That call defaults to BIG_ENDIAN, and
// the source project's P26 black screen was caused by exactly that — every UBO
// matrix arrived byte-swapped. Here the entry point returns the raw address and
// the Java side wraps it with MemorySegment.ofAddress(...).reinterpret(len),
// whose access layouts are native-order (little-endian on Windows x64). There is
// no byte-order trap left to fall into.

extern "C" {

int32_t MCDR_CALL mcdr_dx12CreateDevice(char* buf, int32_t cap) {
    using mcdr::abi::writeString;

    const auto r = d3d12_mc::ensureDevice();
    if (!r) {
        return writeString("ERROR: " + r.error().message, buf, cap);
    }

    // P15: the log level used to be read here, inside dx12CreateDevice, via
    // getenv("DX12_LOG_VERBOSE"). Native code no longer reads the process
    // environment at all — the Java side owns that decision and calls
    // mcdrSetLogVerbose() before this function.
    d3d12_mc::DeviceContext& ctx = d3d12_mc::deviceContext();
    // P66: 旧的 level/10, level%10 是错的（0xC100 会打印成 "4940_8"），现在统一走
    // featureLevelName()；能力摘要附在后面，Java 侧那行 "Device probe ..." 日志就能
    // 直接看到 mesh shader tier 之类的 12_2 基线。
    std::string result = ctx.adapterName.empty() ? "<unknown adapter>" : ctx.adapterName;
    result += " (";
    result += d3d12_mc::featureLevelName(ctx.featureLevel);
    result += "; ";
    result += d3d12_mc::capabilitiesText(ctx.caps);
    result += "); ";
    // P1 probe + P2 resource-layer self-test; the detail string is what the Java
    // side logs when the four-round createDevice() self-test reports.
    result += d3d12_mc::runResourceSelfTest();
    return writeString(result, buf, cap);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateTexture(int32_t usage, int32_t format, int32_t width, int32_t height,
                                              int32_t depthOrLayers, int32_t mipLevels) {
    const auto r = d3d12_mc::createTexture(usage, format, width, height, depthOrLayers, mipLevels);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateTexture", r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateBuffer(int32_t usage, int64_t size) {
    const auto r = d3d12_mc::createBuffer(usage, size);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateBuffer", r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateSampler(int32_t addressU, int32_t addressV, int32_t minFilter,
                                              int32_t magFilter, int32_t maxAnisotropy, float maxLod) {
    const auto r = d3d12_mc::createSampler(addressU, addressV, minFilter, magFilter, maxAnisotropy, maxLod);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateSampler", r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateSamplerWithMipBias(int32_t addressU, int32_t addressV, int32_t minFilter,
                                                         int32_t magFilter, int32_t maxAnisotropy,
                                                         float maxLod, float mipLodBias) {
    const auto r = d3d12_mc::createSamplerWithMipBias(addressU, addressV, minFilter, magFilter, maxAnisotropy,
                                                      maxLod, mipLodBias);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateSamplerWithMipBias",
                                                            r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateTextureView(mcdr_object* texture, int32_t baseMipLevel,
                                                  int32_t mipLevels) {
    const auto r = d3d12_mc::createTextureView(texture, baseMipLevel, mipLevels);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateTextureView", r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

void MCDR_CALL mcdr_dx12DestroyResource(mcdr_object* object) { d3d12_mc::destroyObject(object); }

mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipeline(const void* dxil, int32_t dxilSize) {
    const auto r = d3d12_mc::createComputePipeline(dxil, dxilSize);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateComputePipeline",
                                                            r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}


// P235：着色率图像的 compute PSO（2×SRV + 1×UAV）与图像本身。
// P276：与下面同一个根签名，名字不带 ShadingRate（直接转发）。
mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineTexture3(const void* dxil, int32_t dxilSize) {
    return mcdr_dx12CreateComputePipelineShadingRate(dxil, dxilSize);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateComputePipelineShadingRate(const void* dxil, int32_t dxilSize) {
    const auto r = d3d12_mc::createComputePipelineShadingRate(dxil, dxilSize);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateComputePipelineShadingRate",
                                                             r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateShadingRateImage(int32_t width, int32_t height) {
    const auto r = d3d12_mc::createShadingRateImage(width, height);
    if (!r) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateShadingRateImage",
                                                             r.error().message);
    }
    return static_cast<d3d12_mc::Dx12Object*>(*r);
}
void* MCDR_CALL mcdr_dx12MapBuffer(mcdr_object* buffer, int64_t offset, int64_t length, int32_t read,
                                   int32_t write, int64_t* outLength) {
    // The mapped range is always the caller-requested length; report it back so
    // the Java side can size its reinterpret() without a second call.
    if (outLength) {
        *outLength = 0;
    }
    const auto r = d3d12_mc::mapBuffer(buffer, offset, length, read != 0, write != 0);
    if (!r) {
        mcdr::abi::failVoid("mcdr_dx12MapBuffer", r.error().message);
        return nullptr;
    }
    if (outLength) {
        *outLength = length;
    }
    return *r;
}

void MCDR_CALL mcdr_dx12UnmapBuffer(mcdr_object* buffer) { d3d12_mc::unmapBuffer(buffer); }

int32_t MCDR_CALL mcdr_dx12EnumerateAdapters(char* buf, int32_t cap) {
    // Purely informational: enumerating on a machine with no D3D12 adapter
    // legitimately yields "[]", so this never reports an error.
    return mcdr::abi::writeString(d3d12_mc::enumerateAdaptersJson(), buf, cap);
}
int32_t MCDR_CALL mcdr_dx12GetAdapterInfo(char* buf, int32_t cap) {
    // Same two-call buffer convention as mcdr_dx12EnumerateAdapters: first ask
    // with (nullptr, 0) for the required byte count including the NUL, then
    // pass a buffer that big. Empty means "no active device" (never created, or
    // already destroyed) -> 0, and the caller keeps its fallback values.
    //
    // Note the deliberate asymmetry with mcdr::abi::writeString, which returns
    // the size INCLUDING the NUL on both paths: here the writing path returns
    // the bytes written EXCLUDING the NUL, as the ABI documents.
    const std::string json = d3d12_mc::adapterInfoJson();
    if (json.empty()) {
        return 0;
    }
    if (buf == nullptr || cap <= 0) {
        return static_cast<int32_t>(json.size()) + 1;
    }
    mcdr::abi::writeString(json, buf, cap);
    return static_cast<int32_t>(json.size());
}

uint64_t MCDR_CALL mcdr_dx12GetQueueHandle(void) { return static_cast<uint64_t>(d3d12_mc::getQueueHandle()); }

uint64_t MCDR_CALL mcdr_dx12GetDeviceHandle(void) {
    return static_cast<uint64_t>(d3d12_mc::getDeviceHandle());
}

void MCDR_CALL mcdr_dx12DestroyDevice(void) { d3d12_mc::destroyDevice(); }

uint64_t MCDR_CALL mcdr_dx12CreateHiddenWindow(int32_t width, int32_t height) {
    return static_cast<uint64_t>(d3d12_mc::createHiddenWindow(width, height));
}

void MCDR_CALL mcdr_dx12DestroyHiddenWindow(uint64_t hwnd) {
    d3d12_mc::destroyHiddenWindow(static_cast<uintptr_t>(hwnd));
}

}  // extern "C"
