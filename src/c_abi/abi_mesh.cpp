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

int32_t MCDR_CALL mcdr_dx12QueryMeshSupport(char* outReason, int32_t capacity) {
    if (capacity < 0 || (!outReason && capacity != 0))
        return mcdr::abi::failWith("mcdr_dx12QueryMeshSupport", "invalid reason buffer/capacity");
    std::string reason;
    const bool supported = d3d12_mc::meshPipelineSupported(&reason);
    if (outReason && capacity > 0) {
        const size_t count = (std::min)(reason.size(), static_cast<size_t>(capacity - 1));
        std::memcpy(outReason, reason.data(), count);
        outReason[count] = '\0';
    }
    return supported ? 1 : 0;
}

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

static PipelinePtr* createMeshPipelineErased(const mcdr_mesh_pipeline_desc* desc, uint64_t rootSignature) {
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
        dst.blendEnabled = src.blendEnabled != 0;
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

    auto r = rootSignature == 0 ? d3d12_mc::createMeshPipeline(d)
                               : d3d12_mc::createMeshPipeline(d, rootSignature);
    if (!r) {
        // 全限定：ADL 不会把 mcdr::abi 拉进来（实参是 std::unexpected<mcdr::Error>，
        // 关联命名空间只有 std 与 mcdr），所以这里不能靠 ADL 找 failResultPtr。
        return mcdr::abi::failResultPtr<PipelinePtr>(kCreateMesh, r.error());
    }
    return static_cast<PipelinePtr*>(*r);
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateMeshPipeline(const mcdr_mesh_pipeline_desc* desc) {
    return createMeshPipelineErased(desc, 0);
}

uint64_t MCDR_CALL mcdr_dx12CreateMeshRootSignature(const mcdr_pipeline_binding_desc* bindings,
                                                    int32_t count, int32_t meshExtras) {
    using mcdr::abi::setLastErrorf;
    if (count < 0 || (count > 0 && bindings == nullptr)) {
        setLastErrorf("mcdr_dx12CreateMeshRootSignature", "bad binding list (count=%d)", static_cast<int>(count));
        return 0;
    }
    std::vector<d3d12_mc::PipelineDesc::Binding> list;
    list.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        d3d12_mc::PipelineDesc::Binding b{};
        b.type = bindings[i].type;
        b.reg = bindings[i].reg;
        list.push_back(b);
    }
    auto r = d3d12_mc::createMeshRootSignature(list, meshExtras != 0);
    if (!r) {
        setLastErrorf("mcdr_dx12CreateMeshRootSignature", "%s", r.error().message.c_str());
        return 0;
    }
    return *r;
}

void MCDR_CALL mcdr_dx12ReleaseRootSignature(uint64_t rootSignature) {
    d3d12_mc::releaseMeshRootSignature(rootSignature);
}

PipelinePtr* MCDR_CALL mcdr_dx12CreateMeshPipelineWithRootSignature(
    const mcdr_mesh_pipeline_desc* desc, uint64_t rootSignature) {
    return createMeshPipelineErased(desc, rootSignature);
}

int32_t MCDR_CALL mcdr_dx12DispatchMesh(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t z) {
    using mcdr::abi::failWith;

    if (ctx == nullptr) {
        return failWith(kDispatch, "command context is null");
    }
    if (x <= 0 || y <= 0 || z <= 0) {
        return failWith(kDispatch, "thread group counts must be positive");
    }
    // D3D12 的 DispatchMesh 三个维度各自上限 65535；超了驱动层会静默丢弃这一调用。
    constexpr int32_t kMaxGroups = 65535;
    if (x > kMaxGroups || y > kMaxGroups || z > kMaxGroups) {
        return failWith(kDispatch, "thread group counts exceed 65535");
    }

    if (auto r = d3d12_mc::dispatchMesh(ctx, x, y, z); !r) {
        return mcdr::abi::failResult(kDispatch, r.error());
    }
    return 1;
}

// P217：meshlet 版地形派发。b8 = 小常量，t14/t15/t16 = 顶点池 / 段表 / meshlet 表。
int32_t MCDR_CALL mcdr_dx12MeshDrawMeshlets(
    mcdr_command_ctx* ctx, mcdr_object* vertexBuffer, int64_t vertexOffset, mcdr_object* sections,
    int64_t sectionsOffset, int64_t sectionsLength, mcdr_object* meshlets, int64_t meshletsOffset,
    int64_t meshletsLength, mcdr_object* globals, int64_t globalsOffset, int32_t meshletCount) {
    using mcdr::abi::failWith;
    constexpr const char* kMeshlets = "mcdr_dx12MeshDrawMeshlets";

    if (ctx == nullptr) {
        return failWith(kMeshlets, "command context is null");
    }
    if (vertexBuffer == nullptr || sections == nullptr || meshlets == nullptr || globals == nullptr) {
        return failWith(kMeshlets, "buffer handle is null");
    }
    if (meshletCount <= 0) {
        return failWith(kMeshlets, "meshletCount must be positive");
    }
    if (auto r = d3d12_mc::drawMeshlets(ctx, vertexBuffer, static_cast<long long>(vertexOffset), sections,
                                        static_cast<long long>(sectionsOffset),
                                        static_cast<long long>(sectionsLength), meshlets,
                                        static_cast<long long>(meshletsOffset),
                                        static_cast<long long>(meshletsLength), globals,
                                        static_cast<long long>(globalsOffset), static_cast<int>(meshletCount));
        !r) {
        return mcdr::abi::failResult(kMeshlets, r.error());
    }
    return 1;
}

}  // extern "C"
