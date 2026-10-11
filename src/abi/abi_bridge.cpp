// abi_bridge.cpp — device, resource and hidden-window entry points.
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

#include "abi_common.h"
#include "../dx12/dx12_device.h"

import std;
import std.compat;

extern "C" {

int32_t MCDR_CALL mcdr_dx12CreateDevice(char* buf, int32_t cap) {
    using mcdr::abi::writeString;

    std::string err;
    if (!d3d12_mc::ensureDevice(err)) {
        return writeString("ERROR: " + err, buf, cap);
    }

    // P15: the log level used to be read here, inside dx12CreateDevice, via
    // getenv("DX12_LOG_VERBOSE"). Native code no longer reads the process
    // environment at all — the Java side owns that decision and calls
    // mcdrSetLogVerbose() before this function.
    d3d12_mc::DeviceContext& ctx = d3d12_mc::deviceContext();
    const int level = static_cast<int>(ctx.featureLevel) & 0xffff;
    char levelName[64];
    std::snprintf(levelName, sizeof(levelName), "D3D_FEATURE_LEVEL_%d_%d", level / 10, level % 10);

    std::string result = ctx.adapterName.empty() ? "<unknown adapter>" : ctx.adapterName;
    result += " (";
    result += levelName;
    result += "); ";
    // P1 probe + P2 resource-layer self-test; the detail string is what the Java
    // side logs when the four-round createDevice() self-test reports.
    result += d3d12_mc::runResourceSelfTest();
    return writeString(result, buf, cap);
}

mcdr_object* MCDR_CALL mcdr_dx12CreateTexture(int32_t usage, int32_t format, int32_t width, int32_t height,
                                              int32_t depthOrLayers, int32_t mipLevels) {
    std::string err;
    d3d12_mc::Dx12Object* obj =
        d3d12_mc::createTexture(usage, format, width, height, depthOrLayers, mipLevels, err);
    if (!obj) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateTexture", err);
    }
    return obj;
}

mcdr_object* MCDR_CALL mcdr_dx12CreateBuffer(int32_t usage, int64_t size) {
    std::string err;
    d3d12_mc::Dx12Object* obj = d3d12_mc::createBuffer(usage, size, err);
    if (!obj) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateBuffer", err);
    }
    return obj;
}

mcdr_object* MCDR_CALL mcdr_dx12CreateSampler(int32_t addressU, int32_t addressV, int32_t minFilter,
                                              int32_t magFilter, int32_t maxAnisotropy, float maxLod) {
    std::string err;
    d3d12_mc::Dx12Object* obj =
        d3d12_mc::createSampler(addressU, addressV, minFilter, magFilter, maxAnisotropy, maxLod, err);
    if (!obj) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateSampler", err);
    }
    return obj;
}

mcdr_object* MCDR_CALL mcdr_dx12CreateTextureView(mcdr_object* texture, int32_t baseMipLevel,
                                                  int32_t mipLevels) {
    std::string err;
    d3d12_mc::Dx12Object* obj = d3d12_mc::createTextureView(texture, baseMipLevel, mipLevels, err);
    if (!obj) {
        return mcdr::abi::failWithPtr<d3d12_mc::Dx12Object>("mcdr_dx12CreateTextureView", err);
    }
    return obj;
}

void MCDR_CALL mcdr_dx12DestroyResource(mcdr_object* object) { d3d12_mc::destroyObject(object); }

void* MCDR_CALL mcdr_dx12MapBuffer(mcdr_object* buffer, int64_t offset, int64_t length, int32_t read,
                                   int32_t write, int64_t* outLength) {
    // The mapped range is always the caller-requested length; report it back so
    // the Java side can size its reinterpret() without a second call.
    if (outLength) {
        *outLength = 0;
    }
    std::string err;
    void* ptr = d3d12_mc::mapBuffer(buffer, offset, length, read != 0, write != 0, err);
    if (!ptr) {
        mcdr::abi::failVoid("mcdr_dx12MapBuffer", err);
        return nullptr;
    }
    if (outLength) {
        *outLength = length;
    }
    return ptr;
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
