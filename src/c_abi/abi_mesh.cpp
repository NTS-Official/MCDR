// abi_mesh.cpp — P72 mesh-shader pipeline and HLSL -> DXIL compilation
//
// Group: mesh shading (originally abi_bridge_p7.cpp).
// 合并自：abi_bridge_p7.cpp
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
// 来源单元：abi_bridge_p7.cpp
// ---------------------------------------------------------------------------

// abi_mesh.cpp — P72: mesh-shader pipeline + HLSL -> DXIL compilation. (合并自 abi_bridge_p7.cpp)
//
// The graphics bridge (abi_pipeline.cpp) feeds D3DCompile, which stops at
// shader model 5.1 and therefore cannot produce amplification/mesh shaders at
// all. This bridge adds the DXC compilation entry point and the two calls that
// consume its output: createMeshPipeline() (which builds a
// D3D12_PIPELINE_STATE_STREAM_DESC and goes through
// ID3D12Device2::CreatePipelineState) and dispatchMesh()
// (ID3D12GraphicsCommandList6::DispatchMesh).
//
// The descriptor is still a real struct (mcdr_mesh_pipeline_desc) mirrored by
// MemoryLayout on the Java side and verified at load time by mcdrABILayout(),
// so -- exactly like abi_pipeline.cpp -- there is nothing to unpack here.

namespace {

// The same bounds abi_pipeline.cpp enforces: they are the contract with the
// pipeline builders, not incidental limits.
constexpr int32_t kMaxColorTargets = 8;
constexpr int32_t kMaxPipelineBindings = 64;

// A shader source is kilobytes; this only catches a bogus sourceLen before it
// turns into a multi-gigabyte string copy.
constexpr int32_t kMaxSourceBytes = 4 * 1024 * 1024;

constexpr const char* kCompile = "mcdr_dx12CompileShaderDxc";
constexpr const char* kCreateMesh = "mcdr_dx12CreateMeshPipeline";
constexpr const char* kDispatch = "mcdr_dx12DispatchMesh";

using PipelinePtr = d3d12_mc::Dx12Pipeline;

}  // namespace

extern "C" {

int32_t MCDR_CALL mcdr_dx12CompileShaderDxc(const char* source, int32_t sourceLen, const char* entry,
                                            const char* profile, void* outDxil, int32_t outCapacity) {
    using mcdr::abi::failWith;
    using mcdr::abi::setLastErrorf;

    if (source == nullptr || sourceLen <= 0) {
        return failWith(kCompile, "source/sourceLen is empty");
    }
    if (sourceLen > kMaxSourceBytes) {
        setLastErrorf(kCompile, "sourceLen %d exceeds the %d byte cap", static_cast<int>(sourceLen),
                      static_cast<int>(kMaxSourceBytes));
        return 0;
    }
    if (profile == nullptr || profile[0] == '\0') {
        return failWith(kCompile, "profile is empty (expected e.g. \"ms_6_6\")");
    }
    if (outCapacity < 0) {
        return failWith(kCompile, "outCapacity is negative");
    }
    if (outDxil == nullptr && outCapacity != 0) {
        return failWith(kCompile, "outDxil is null but outCapacity is not 0");
    }

    std::vector<uint8_t> dxil;
    // An empty entry point means "main", matching the graphics path's hardcoded
    // entry and keeping the Java side from having to know the default.
    const std::string entryPoint =
        (entry != nullptr && entry[0] != '\0') ? std::string(entry) : std::string("main");

    if (const auto r = d3d12_mc::compileHlslToDxil(std::string(source, static_cast<size_t>(sourceLen)),
                                                   entryPoint, profile, dxil);
        !r) {
        return failWith(kCompile,
                        r.error().message.empty() ? std::string("DXC produced no bytecode")
                                                  : r.error().message);
    }

    const auto size = static_cast<int32_t>(dxil.size());
    if (outDxil == nullptr || outCapacity < size) {
        // Size probe: report what the caller has to allocate, write nothing, and
        // let it call again with a buffer of that size (the same contract
        // mcdrLastError() uses for strings).
        return size;
    }
    std::memcpy(outDxil, dxil.data(), static_cast<size_t>(size));
    return size;
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateMeshPipeline(const mcdr_mesh_pipeline_desc* desc) {
    using mcdr::abi::failWithPtr;
    using mcdr::abi::setLastErrorf;

    if (!desc) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "descriptor is null");
    }

    d3d12_mc::MeshPipelineDesc d;

    // --- shader bytecode (DXIL, never HLSL) --------------------------------
    if (desc->msLen <= 0 || desc->msData == nullptr) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "msData/msLen is required (mesh shader)");
    }
    {
        const auto* first = static_cast<const uint8_t*>(desc->msData);
        d.msDxil.assign(first, first + desc->msLen);
    }
    if (desc->asLen < 0 || (desc->asLen > 0 && desc->asData == nullptr)) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "asData/asLen mismatch");
    }
    if (desc->asLen > 0) {
        const auto* first = static_cast<const uint8_t*>(desc->asData);
        d.asDxil.assign(first, first + desc->asLen);
    }
    if (desc->psLen < 0 || (desc->psLen > 0 && desc->psData == nullptr)) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "psData/psLen mismatch");
    }
    if (desc->psLen > 0) {
        const auto* first = static_cast<const uint8_t*>(desc->psData);
        d.psDxil.assign(first, first + desc->psLen);
    }

    // --- colour targets ----------------------------------------------------
    if (desc->colorCount < 0 || desc->colorCount > kMaxColorTargets) {
        setLastErrorf(kCreateMesh, "bad colorCount %d", static_cast<int>(desc->colorCount));
        return nullptr;
    }
    if (desc->colorCount > 0 && desc->colorTargets == nullptr) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "colorTargets is null");
    }
    d.colorCount = desc->colorCount;
    d.colorTargets.resize(static_cast<size_t>(desc->colorCount));
    for (int32_t i = 0; i < desc->colorCount; ++i) {
        const mcdr_color_target_desc& src = desc->colorTargets[i];
        d3d12_mc::PipelineDesc::ColorTarget& dst = d.colorTargets[static_cast<size_t>(i)];
        dst.format = src.format;
        dst.writeMask = src.writeMask;
        dst.blendEnabled = src.blendEnabled != 0;  // P72 ignores blend, see dx12_mesh.h
        dst.srcColor = src.srcColor;
        dst.dstColor = src.dstColor;
        dst.colorOp = src.colorOp;
        dst.srcAlpha = src.srcAlpha;
        dst.dstAlpha = src.dstAlpha;
        dst.alphaOp = src.alphaOp;
    }

    // --- depth state -------------------------------------------------------
    d.hasDepth = desc->hasDepth != 0;
    if (d.hasDepth) {
        d.depthFormat = desc->depth.depthFormat;
        d.depthWrite = desc->depth.depthWrite != 0;
        d.depthCompareOp = desc->depth.depthCompareOp;
    }

    d.cullEnabled = desc->cullEnabled != 0;
    d.polygonMode = desc->polygonMode;

    // --- root bindings -----------------------------------------------------
    // Identical contract to the graphics pipeline, so mcdr_dx12PushDescriptors
    // needs no mesh-specific variant.
    if (desc->bindingCount < 0 || desc->bindingCount > kMaxPipelineBindings) {
        setLastErrorf(kCreateMesh, "bad bindingCount %d", static_cast<int>(desc->bindingCount));
        return nullptr;
    }
    if (desc->bindingCount > 0 && desc->bindings == nullptr) {
        return failWithPtr<PipelinePtr>(kCreateMesh, "bindings is null");
    }
    d.bindings.resize(static_cast<size_t>(desc->bindingCount));
    for (int32_t i = 0; i < desc->bindingCount; ++i) {
        d.bindings[static_cast<size_t>(i)].type = desc->bindings[i].type;
        d.bindings[static_cast<size_t>(i)].reg = desc->bindings[i].reg;
    }

    auto r = d3d12_mc::createMeshPipeline(d);
    if (!r) {
        // 全限定：ADL 不会把 mcdr::abi 拉进来（实参是 std::unexpected<mcdr::Error>，
        // 关联命名空间只有 std 与 mcdr），所以这里不能靠 ADL 找 failResultPtr。
        return mcdr::abi::failResultPtr<PipelinePtr>(kCreateMesh, r.error());
    }
    return static_cast<PipelinePtr*>(*r);
}

int32_t MCDR_CALL mcdr_dx12DispatchMesh(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t z) {
    using mcdr::abi::failWith;

    if (ctx == nullptr) {
        return failWith(kDispatch, "command context is null");
    }
    if (x <= 0 || y <= 0 || z <= 0) {
        return failWith(kDispatch, "thread group counts must be positive");
    }

    if (auto r = d3d12_mc::dispatchMesh(ctx, x, y, z); !r) {
        return mcdr::abi::failResult(kDispatch, r.error());
    }
    return 1;
}

}  // extern "C"
