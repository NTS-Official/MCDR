// d3d12_mesh_base.cpp — mcdr.d3d12.mesh 的实现（P72：mesh shader 管线）
// ---------------------------------------------------------------------------
// 本文件是【普通翻译单元】（不是模块实现单元），这一点是刻意的：
//   * 导出声明写在 src\module\advanced\d3d12_mesh.cppm 的 `export namespace d3d12_mc` 里；
//   * 若把定义放进该模块的实现单元，MSVC 会把签名里的 device 类型记成
//     “模块私有名”（Dx12Pipeline@$$_Amcdr.dx12.device），而消费方
//     （abi_mesh.cpp，普通 .cpp，按 mcdr_abi.h 的前向声明看类型）
//     按公共名 @1 引用 → 链接期 LNK2001。
//   放在普通翻译单元里，两侧都把类型记成公共名 d3d12_mc::Dx12Pipeline，
//   修饰名一致。
// device 模块给出的只是声明（import 即可见，链接期由 dx12_device.cpp 定义）。
//
// P72 只铺基础：AS/MS/PS + 不透明写入 + 渲染目标格式 + 可选深度。混合状态
// （blend）与 mesh 专属特性（payload 校验、amplification 输出组数）留待后续。

#include "_d3d12_module_prelude.h"

#include "mcdr_abi.h"   // mcdr::abi::* 之外只需要它的存在性（保持包含顺序与其它 TU 一致）

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;

namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
std::string hrHex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return std::string(buf);
}

// MC CompareOp ordinal -> D3D12_COMPARISON_FUNC（与 dx12_device.cpp 的
// toD3d12Compare 同一张表；graphics 与 mesh 必须逐项一致）。
D3D12_COMPARISON_FUNC toCompare(int op) {
    switch (op) {
        case 0: return D3D12_COMPARISON_FUNC_ALWAYS;  // ALWAYS_PASS
        case 1: return D3D12_COMPARISON_FUNC_LESS;  // LESS_THAN
        case 2: return D3D12_COMPARISON_FUNC_LESS_EQUAL;  // LESS_THAN_OR_EQUAL
        case 3: return D3D12_COMPARISON_FUNC_EQUAL;  // EQUAL
        case 4: return D3D12_COMPARISON_FUNC_NOT_EQUAL;  // NOT_EQUAL
        case 5: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // GREATER_THAN_OR_EQUAL
        case 6: return D3D12_COMPARISON_FUNC_GREATER;  // GREATER_THAN
        case 7: return D3D12_COMPARISON_FUNC_NEVER;  // NEVER_PASS
        default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

// type==0 走 root CBV（每 draw 直接 SetGraphicsRootConstantBufferView）；
// type==1 是 SRV + static sampler，type==2 只是 SRV（texel buffer）。
// 与 d3d12_device.cpp 的 createGraphicsPipeline 完全同构，保证
// mcdr_dx12PushDescriptors 无需 mesh 专用分支即可复用。
D3D12_STATIC_SAMPLER_DESC makeStaticSampler(uint8_t reg) {
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    // WRAP 对齐 MC 默认采样器（同图形管线的既有修复：CLAMP 会让 UV 超出
    // [0,1] 的纹理退化到边缘像素）。
    s.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.MipLODBias = 0.0f;
    s.MaxAnisotropy = 1;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    s.MinLOD = 0.0f;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderRegister = reg;
    s.RegisterSpace = 0;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return s;
}

// ---------------------------------------------------------------------------
// D3D12_PIPELINE_STATE_STREAM_DESC 的子对象封送。
// 本机 SDK 不提供 d3dx12.h（已确认 um\ 下没有 d3dx12*.h），因此自带一份最小
// 实现：每个子对象是一个 {类型标签, 负载} 的 8 字节对齐结构，连续排列后整体
// 传给 ID3D12Device2::CreatePipelineState。负载值语义与 d3dx12.h 的
// CD3DX12_PIPELINE_STATE_STREAM_* 一致。
// ---------------------------------------------------------------------------
// C4324 "由于对齐说明符，结构被填充"：这里正是**有意**要那份尾部填充。
// D3D12_PIPELINE_STATE_STREAM_DESC 要求每个子对象在流里 8 字节对齐，尾部填充是
// 布局的一部分（下一个子对象的类型标签必须落在 8 字节边界上）。当前 SDK 的
// D3D12_PIPELINE_STATE_SUBOBJECT_TYPE 恰好是 4 字节宽，于是枚举(4B)+负载(4B)
// 的小负载类型（D3D12_BLEND_DESC / DXGI_SAMPLE_DESC）会被判定为"因对齐而填充"。
// 这与注释里那份"最小版 d3dx12.h"完全一致，改不得，故就近关掉这一条诊断。
#pragma warning(push)
#pragma warning(disable : 4324)
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Payload>
struct alignas(void*) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    Payload payload{};
};
#pragma warning(pop)

// d3dx12.h 的同名结构：NumRenderTargets + RTVFormats[8]。RTVFormats 的偏移在
// d3dx12.h 里靠编译器决定，这里显式写出同一布局。
struct StreamRenderTargets {
    UINT NumRenderTargets = 0;
    DXGI_FORMAT RTVFormats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
};

// 把子对象按调用顺序连续排布（每个 Subobject 自带 8 字节对齐与类型标签）。
// 用递归重载而不是折叠表达式：后者在对齐模板的实参推导上会被 MSVC 拒绝。
inline void appendSubobjects(std::vector<unsigned char>&) {}

template <typename T, typename... Rest>
void appendSubobjects(std::vector<unsigned char>& bytes, const T& first, const Rest&... rest) {
    const size_t at = bytes.size();
    bytes.resize(at + sizeof(T));
    std::memcpy(bytes.data() + at, &first, sizeof(T));
    appendSubobjects(bytes, rest...);
}

// GpuFormat ordinal -> DSV 格式。与 dx12_device.cpp 的 createGraphicsPipeline
// 同一策略：DSV 固定 D32_FLOAT（镜像官方 depthAttachmentFormat=126），
// depthFormat 只参与 "有没有深度" 的判断。
DXGI_FORMAT depthStencilFormat(const d3d12_mc::MeshPipelineDesc& desc) {
    return desc.hasDepth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
}

}  // namespace

namespace d3d12_mc {

// ---------------------------------------------------------------------------
// meshPipelineSupported
// ---------------------------------------------------------------------------
bool meshPipelineSupported(std::string* why) {
    auto reject = [why](const char* reason) {
        if (why != nullptr) *why = reason;
        return false;
    };

    DeviceContext& ctx = deviceContext();
    if (!ctx.device) return reject("device not initialized");

    if (ctx.caps.featureLevel < D3D_FEATURE_LEVEL_12_2)
        return reject("feature level below 12_2 (mesh shader requires 12_2)");
    if (!ctx.caps.meshShadersSupported())
        return reject("D3D12 mesh shader tier is 0 (not supported by this adapter)");
    if (!ctx.caps.dxcAvailable) return reject("dxcompiler.dll not available");
    if (!ctx.caps.meshShaderCompileOk)
        return reject(ctx.caps.dxcNote.empty() ? "DXC could not compile an ms_6_x profile"
                                               : ctx.caps.dxcNote.c_str());

    ComPtr<ID3D12Device2> device2;
    if (FAILED(ctx.device.As(&device2)) || !device2)
        return reject("ID3D12Device2 is not available (CreatePipelineState required)");

    if (why != nullptr) why->clear();
    return true;
}

// ---------------------------------------------------------------------------
// createMeshPipeline
// ---------------------------------------------------------------------------
void* createMeshPipeline(const MeshPipelineDesc& desc, std::string& err) {
    if (!ensureDevice(err)) return nullptr;

    if (desc.msDxil.empty()) {
        err = "createMeshPipeline: msDxil is empty (mesh shader bytecode is required)";
        return nullptr;
    }

    // mesh 管线必须走 ID3D12Device2::CreatePipelineState（流描述）。
    ComPtr<ID3D12Device2> device2;
    HRESULT hr = deviceContext().device.As(&device2);
    if (FAILED(hr) || !device2) {
        err = "createMeshPipeline: ID3D12Device2 unavailable hr=" + hrHex(hr);
        return nullptr;
    }

    // 1) root signature：与图形管线同构。CBV -> root descriptor（按 binding 顺序
    //    稠密编号），SRV 汇入单个 descriptor table，type==1 额外挂 static sampler。
    //    mesh 管线没有 IA 阶段：flags 必须【不带】
    //    ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT（带上会让 PSO 创建失败），
    //    因此 serialize 用 1.1 版本。
    std::vector<uint8_t> cbvRegs;
    std::vector<D3D12_DESCRIPTOR_RANGE> srvRanges;
    std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
    cbvRegs.reserve(desc.bindings.size());
    srvRanges.reserve(desc.bindings.size());
    staticSamplers.reserve(desc.bindings.size());
    for (const PipelineDesc::Binding& b : desc.bindings) {
        if (b.type == 0) {
            cbvRegs.push_back(b.reg);
            continue;
        }
        D3D12_DESCRIPTOR_RANGE r{};
        r.NumDescriptors = 1;
        r.RegisterSpace = 0;
        r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        r.BaseShaderRegister = b.reg;
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges.push_back(r);
        if (b.type == 1) staticSamplers.push_back(makeStaticSampler(b.reg));
    }

    std::vector<D3D12_ROOT_PARAMETER> params;
    params.reserve(cbvRegs.size() + 1);
    for (uint8_t reg : cbvRegs) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = reg;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    if (!srvRanges.empty()) {
        D3D12_ROOT_DESCRIPTOR_TABLE table{};
        table.NumDescriptorRanges = static_cast<UINT>(srvRanges.size());
        table.pDescriptorRanges = srvRanges.data();
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable = table;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(params.size());
    rsDesc.pParameters = params.empty() ? nullptr : params.data();
    rsDesc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
    rsDesc.pStaticSamplers = staticSamplers.empty() ? nullptr : staticSamplers.data();
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        err = "createMeshPipeline: D3D12SerializeRootSignature hr=" + hrHex(hr);
        if (rsErr && rsErr->GetBufferSize() > 0) {
            err += " ";
            err.append(static_cast<const char*>(rsErr->GetBufferPointer()), rsErr->GetBufferSize());
        }
        return nullptr;
    }
    ComPtr<ID3D12RootSignature> rootSig;
    hr = deviceContext().device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                                     IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        err = "createMeshPipeline: CreateRootSignature hr=" + hrHex(hr);
        return nullptr;
    }

    // 2) 共享状态：不透明写入 + 光栅化。P72 忽略 blend（Java 侧的 blendEnabled
    //    等字段在此不生效，见 dx12_mesh.cppm 的字段注释）。
    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = TRUE;
    int numRT = 0;
    for (int i = 0; i < desc.colorCount; ++i) {
        const PipelineDesc::ColorTarget& ct = desc.colorTargets[static_cast<size_t>(i)];
        if (ct.format < 0) continue;  // 未使用槽位
        numRT = i + 1;
        D3D12_RENDER_TARGET_BLEND_DESC& rt = blend.RenderTarget[i];
        rt.BlendEnable = FALSE;  // P72 不做混合
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = ct.writeMask;  // MC 掩码位序与 D3D12 一致
    }

    D3D12_RASTERIZER_DESC rs{};
    rs.FillMode = (desc.polygonMode == 1) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    rs.CullMode = desc.cullEnabled ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    // MC 前端为逆时针（同图形管线：D3D12 默认顺时针，需翻转）。
    rs.FrontCounterClockwise = TRUE;
    rs.DepthBias = 0;
    rs.DepthBiasClamp = 0.0f;
    rs.SlopeScaledDepthBias = 0.0f;
    rs.DepthClipEnable = TRUE;
    rs.MultisampleEnable = FALSE;
    rs.AntialiasedLineEnable = FALSE;
    rs.ForcedSampleCount = 0;
    rs.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = desc.hasDepth ? TRUE : FALSE;
    ds.DepthWriteMask = desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = desc.hasDepth ? toCompare(desc.depthCompareOp) : D3D12_COMPARISON_FUNC_ALWAYS;
    ds.StencilEnable = FALSE;
    ds.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    ds.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

    D3D12_SHADER_BYTECODE ms{};
    ms.pShaderBytecode = desc.msDxil.data();
    ms.BytecodeLength = desc.msDxil.size();
    D3D12_SHADER_BYTECODE as{};
    if (!desc.asDxil.empty()) {
        as.pShaderBytecode = desc.asDxil.data();
        as.BytecodeLength = desc.asDxil.size();
    }
    D3D12_SHADER_BYTECODE ps{};
    if (!desc.psDxil.empty()) {
        ps.pShaderBytecode = desc.psDxil.data();
        ps.BytecodeLength = desc.psDxil.size();
    }

    StreamRenderTargets rtFormats{};
    rtFormats.NumRenderTargets = static_cast<UINT>(numRT);
    for (int i = 0; i < desc.colorCount; ++i) {
        const PipelineDesc::ColorTarget& ct = desc.colorTargets[static_cast<size_t>(i)];
        rtFormats.RTVFormats[i] = ct.format < 0 ? DXGI_FORMAT_UNKNOWN : toDxgiFormat(ct.format);
    }

    // 3) 子对象流。AS 子对象仅在确有 amplification 字节码时加入——空 AS 会被
    //    验证层判为非法。
    using StreamRootSig = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*>;
    using StreamMs = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE>;
    using StreamAs = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE>;
    using StreamPs = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>;
    using StreamBlend = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC>;
    using StreamRaster = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>;
    using StreamDepth = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC>;
    using StreamRtv = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,
                                StreamRenderTargets>;
    using StreamDsv = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>;
    using StreamSampleDesc = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC>;
    using StreamSampleMask = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT>;
    using StreamNodeMask = Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, UINT>;

    StreamRootSig sRoot;
    sRoot.payload = rootSig.Get();
    StreamMs sMs;
    sMs.payload = ms;
    StreamAs sAs;
    sAs.payload = as;
    StreamPs sPs;
    sPs.payload = ps;
    StreamBlend sBlend;
    sBlend.payload = blend;
    StreamRaster sRaster;
    sRaster.payload = rs;
    StreamDepth sDepth;
    sDepth.payload = ds;
    StreamRtv sRtv;
    sRtv.payload = rtFormats;
    StreamDsv sDsv;
    sDsv.payload = depthStencilFormat(desc);
    StreamSampleDesc sSample;
    sSample.payload = DXGI_SAMPLE_DESC{1, 0};
    StreamSampleMask sMask;
    sMask.payload = UINT_MAX;
    StreamNodeMask sNode;
    sNode.payload = 0;

    auto pipeline = std::make_unique<Dx12Pipeline>();
    pipeline->rootSignature = rootSig;
    pipeline->isMesh = true;  // setPipeline 据此跳过 IASetPrimitiveTopology

    auto buildPso = [&](bool withAs, bool withPs, ComPtr<ID3D12PipelineState>& out,
                        std::string& e) -> bool {
        std::vector<unsigned char> stream;
        stream.reserve(512);
        appendSubobjects(stream, sRoot, sMs);
        if (withAs) appendSubobjects(stream, sAs);
        if (withPs) appendSubobjects(stream, sPs);
        appendSubobjects(stream, sBlend, sRaster, sDepth, sRtv, sDsv, sSample, sMask, sNode);

        D3D12_PIPELINE_STATE_STREAM_DESC sd{};
        sd.SizeInBytes = static_cast<SIZE_T>(stream.size());
        sd.pPipelineStateSubobjectStream = stream.data();

        dbgLog("createMeshPipeline: stream=%zuB as=%d ps=%d rt=%d dsv=%d cull=%d wire=%d\n",
               static_cast<size_t>(sd.SizeInBytes), withAs ? 1 : 0, withPs ? 1 : 0, numRT,
               static_cast<int>(sDsv.payload), desc.cullEnabled ? 1 : 0, desc.polygonMode == 1 ? 1 : 0);

        const HRESULT h = device2->CreatePipelineState(&sd, IID_PPV_ARGS(&out));
        if (FAILED(h)) {
            e = "createMeshPipeline: CreatePipelineState hr=" + hrHex(h);
            return false;
        }
        return true;
    };

    // 4) 与图形管线同构地建两个 PSO：withDepth 总是建（DSV=D32_FLOAT），
    //    仅当管线无深度状态时再建 withoutDepth（DSV=UNKNOWN）。
    if (!buildPso(!desc.asDxil.empty(), !desc.psDxil.empty(), pipeline->withDepth, err)) return nullptr;
    if (!desc.hasDepth) {
        const DXGI_FORMAT saved = sDsv.payload;
        sDsv.payload = DXGI_FORMAT_UNKNOWN;
        ds.DepthEnable = FALSE;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sDepth.payload = ds;
        const bool ok = buildPso(!desc.asDxil.empty(), !desc.psDxil.empty(), pipeline->withoutDepth, err);
        sDsv.payload = saved;
        if (!ok) return nullptr;
    }

    return pipeline.release();
}

// ---------------------------------------------------------------------------
// dispatchMesh
// ---------------------------------------------------------------------------
bool dispatchMesh(void* ctxHandle, int x, int y, int z, std::string& err) {
    auto* ctx = static_cast<CommandContext*>(ctxHandle);
    if (ctx == nullptr) {
        err = "dispatchMesh: command context is null";
        return false;
    }
    if (ctx->commandList == nullptr) {
        err = "dispatchMesh: command list is not open";
        return false;
    }

    // DispatchMesh 只在 ID3D12GraphicsCommandList6 上（SM 6.6+ / 12_2）。
    ComPtr<ID3D12GraphicsCommandList6> list6;
    HRESULT hr = ctx->commandList->QueryInterface(IID_PPV_ARGS(&list6));
    if (FAILED(hr) || !list6) {
        err = "dispatchMesh: ID3D12GraphicsCommandList6 unavailable hr=" + hrHex(hr);
        return false;
    }

    list6->DispatchMesh(static_cast<UINT>(x), static_cast<UINT>(y), static_cast<UINT>(z));
    return true;
}

}  // namespace d3d12_mc
