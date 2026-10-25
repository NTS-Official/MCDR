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
// AS payload 与子组派发是 HLSL 契约；此处负责 PSO、绑定及直接派发校验。

#include "_d3d12_module_prelude.h"

#include "mcdr_abi.h"   // mcdr::abi::* 之外只需要它的存在性（保持包含顺序与其它 TU 一致）

import mcdr.d3d12.device;
import mcdr.d3d12.mesh;
import mcdr.error;   // Result<T> / err()：失败上报改走 std::expected

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

D3D12_BLEND meshBlendFactor(uint8_t f) {
    constexpr D3D12_BLEND factors[] = {
        D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_DEST_ALPHA,
        D3D12_BLEND_DEST_COLOR, D3D12_BLEND_ONE, D3D12_BLEND_INV_BLEND_FACTOR,
        D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_INV_DEST_COLOR,
        D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_SRC_ALPHA,
        D3D12_BLEND_SRC_ALPHA_SAT, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_ZERO};
    return f < 15 ? factors[f] : D3D12_BLEND_ONE;
}

D3D12_BLEND_OP meshBlendOp(uint8_t op) {
    return op < 5 ? static_cast<D3D12_BLEND_OP>(D3D12_BLEND_OP_ADD + op) : D3D12_BLEND_OP_ADD;
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

// 官方结构是 d3d12.h 的 D3D12_RT_FORMAT_ARRAY，字段顺序为 **RTFormats[8] 在前、
// NumRenderTargets 在后**（与 d3dx12.h 的子对象包装一致）。P124 之前这里写反了顺序，
// 运行期把 NumRenderTargets 当成 RTFormats[0] 读，于是调试层报
// "RTVFormats[] must be DXGI_FORMAT_UNKNOWN for all indices >= NumRenderTargets"，
// CreatePipelineState 返回 E_INVALIDARG。直接复用官方类型，避免再次手写错布局。
using StreamRenderTargets = D3D12_RT_FORMAT_ARRAY;

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
        return reject("feature level below 12_2 (project support policy; mesh API starts at SM 6.5)");
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
// P137：mesh 根签名的尾参画像
// ---------------------------------------------------------------------------
// 段派发要知道 b8（段表）与 t14/t15/t16（顶点/索引/区块）在根签名里的参数索引。
// 索引取决于「CBV 稠密编号个数」与「地形到底有没有 SRV 表」两件事，只有建签名那一刻
// 才算得出，所以建签名时记一笔，createMeshPipeline 时抄进 Dx12Pipeline。
// 键是 ID3D12RootSignature* 的地址值（与 ABI 上的不透明句柄同一身份）。
struct MeshRootSigInfo {
    UINT segmentParam = 0;
    UINT geometryParam = 0;
};

std::mutex gMeshRootSigMutex;
std::unordered_map<uint64_t, MeshRootSigInfo> gMeshRootSigs;

void recordMeshRootSig(uint64_t handle, UINT segmentParam, UINT geometryParam) {
    std::lock_guard<std::mutex> lock(gMeshRootSigMutex);
    gMeshRootSigs[handle] = MeshRootSigInfo{segmentParam, geometryParam};
}

MeshRootSigInfo lookupMeshRootSig(uint64_t handle) {
    std::lock_guard<std::mutex> lock(gMeshRootSigMutex);
    const auto it = gMeshRootSigs.find(handle);
    return it == gMeshRootSigs.end() ? MeshRootSigInfo{} : it->second;
}

void forgetMeshRootSig(uint64_t handle) {
    std::lock_guard<std::mutex> lock(gMeshRootSigMutex);
    gMeshRootSigs.erase(handle);
}

// ---------------------------------------------------------------------------
// createMeshPipeline
// ---------------------------------------------------------------------------
namespace {
mcdr::Result<void*> createMeshPipelineImpl(const MeshPipelineDesc& desc, uint64_t externalRootSignature) {
    if (const auto r = ensureDevice(); !r)
        return mcdr::err("createMeshPipeline", r.error().message);
    std::string why;
    if (!meshPipelineSupported(&why)) return mcdr::err("createMeshPipeline", why);

    if (desc.msDxil.empty()) {
        return mcdr::err("createMeshPipeline", "msDxil is empty (mesh shader bytecode is required)");
    }

    // mesh 管线必须走 ID3D12Device2::CreatePipelineState（流描述）。
    ComPtr<ID3D12Device2> device2;
    HRESULT hr = deviceContext().device.As(&device2);
    if (FAILED(hr) || !device2) {
        return mcdr::err("createMeshPipeline", "ID3D12Device2 unavailable hr=" + hrHex(hr));
    }

    // 1) root signature：与图形管线同构。CBV -> root descriptor（按 binding 顺序
    //    稠密编号），SRV 汇入单个 descriptor table，type==1 额外挂 static sampler。
    //    mesh 管线没有 IA 阶段：flags 必须【不带】
    //    ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT（带上会让 PSO 创建失败），
    //    根签名版本与 mesh 无关；用匹配的版本化描述符封送现有 1.0 布局。
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

    ComPtr<ID3D12RootSignature> rootSig;
    // P137：外部签名（地形签名 + mesh 尾参）的尾参参数索引从注册表取。
    UINT meshSegmentParam = 0;
    UINT meshGeometryParam = 0;
    if (externalRootSignature != 0) {
        // P129：复用 JVM 侧交给我们的根签名（地形签名 + 段表 b8 / 顶点 t14 / 索引 t15 三个尾参）。
        // 这里 AddRef，让它与内部创建的路径拥有同样的生命周期语义（ComPtr 析构时 Release）。
        ID3D12RootSignature* external = reinterpret_cast<ID3D12RootSignature*>(
            static_cast<uintptr_t>(externalRootSignature));
        external->AddRef();
        rootSig.Attach(external);
        const MeshRootSigInfo info = lookupMeshRootSig(externalRootSignature);
        meshSegmentParam = info.segmentParam;
        meshGeometryParam = info.geometryParam;
        dbgLogInfo("createMeshPipeline: rs=0x%llx segParam=%u geoParam=%u as=%d\n",
                   static_cast<unsigned long long>(externalRootSignature), info.segmentParam,
                   info.geometryParam, desc.asDxil.empty() ? 0 : 1);
    } else {
    ComPtr<ID3DBlob> rsBlob, rsErr;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    versioned.Desc_1_0 = rsDesc;
    hr = D3D12SerializeVersionedRootSignature(&versioned, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        std::string msg = "D3D12SerializeVersionedRootSignature hr=" + hrHex(hr);
        if (rsErr && rsErr->GetBufferSize() > 0) {
            msg += " ";
            msg.append(static_cast<const char*>(rsErr->GetBufferPointer()), rsErr->GetBufferSize());
        }
        return mcdr::err("createMeshPipeline", std::move(msg));
    }
    hr = deviceContext().device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                                     IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        return mcdr::err("createMeshPipeline", "CreateRootSignature hr=" + hrHex(hr));
    }
    }

    // 2) 共享状态：与图形管线相同的混合枚举及光栅化状态。
    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = TRUE;
    // IndependentBlendEnable=TRUE 时 D3D12 会校验**全部 8 个** RenderTarget 项，未使用槽位
    // 里的 0 不是合法枚举（D3D12_BLEND_ZERO 实际等于 1）⇒ CreatePipelineState 返回
    // E_INVALIDARG（P124 自检实测 hr=0x80070057）。先把 8 个槽位都填成合法默认值，再覆盖用到的。
    for (D3D12_RENDER_TARGET_BLEND_DESC& slot : blend.RenderTarget) {
        slot.BlendEnable = FALSE;
        slot.LogicOpEnable = FALSE;
        slot.SrcBlend = D3D12_BLEND_ONE;
        slot.DestBlend = D3D12_BLEND_ZERO;
        slot.BlendOp = D3D12_BLEND_OP_ADD;
        slot.SrcBlendAlpha = D3D12_BLEND_ONE;
        slot.DestBlendAlpha = D3D12_BLEND_ZERO;
        slot.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        slot.LogicOp = D3D12_LOGIC_OP_NOOP;
        slot.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    int numRT = 0;
    for (int i = 0; i < desc.colorCount; ++i) {
        const PipelineDesc::ColorTarget& ct = desc.colorTargets[static_cast<size_t>(i)];
        if (ct.format < 0) continue;  // 未使用槽位
        numRT = i + 1;
        D3D12_RENDER_TARGET_BLEND_DESC& rt = blend.RenderTarget[i];
        rt.BlendEnable = ct.blendEnabled;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = ct.blendEnabled ? meshBlendFactor(ct.srcColor) : D3D12_BLEND_ONE;
        rt.DestBlend = ct.blendEnabled ? meshBlendFactor(ct.dstColor) : D3D12_BLEND_ZERO;
        rt.BlendOp = meshBlendOp(ct.colorOp);
        rt.SrcBlendAlpha = ct.blendEnabled ? meshBlendFactor(ct.srcAlpha) : D3D12_BLEND_ONE;
        rt.DestBlendAlpha = ct.blendEnabled ? meshBlendFactor(ct.dstAlpha) : D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = meshBlendOp(ct.alphaOp);
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
    for (int i = 0; i < desc.colorCount; ++i) {
        const PipelineDesc::ColorTarget& ct = desc.colorTargets[static_cast<size_t>(i)];
        rtFormats.RTFormats[i] = ct.format < 0 ? DXGI_FORMAT_UNKNOWN : toDxgiFormat(ct.format);
    }
    rtFormats.NumRenderTargets = static_cast<UINT>(numRT);

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
    pipeline->segmentParam = meshSegmentParam;
    pipeline->geometryParam = meshGeometryParam;

    auto buildPso = [&](bool withAs, bool withPs, ComPtr<ID3D12PipelineState>& out) -> mcdr::Result<void> {
        std::vector<unsigned char> stream;
        stream.reserve(512);
        appendSubobjects(stream, sRoot, sMs);
        if (withAs) appendSubobjects(stream, sAs);
        if (withPs) appendSubobjects(stream, sPs);
        // P124 诊断：NODE_MASK 子对象先不加（值 0 在部分驱动上被判 E_INVALIDARG；缺省即 node 0）。
        appendSubobjects(stream, sBlend, sRaster, sDepth, sRtv, sDsv, sSample, sMask);

        D3D12_PIPELINE_STATE_STREAM_DESC sd{};
        sd.SizeInBytes = static_cast<SIZE_T>(stream.size());
        sd.pPipelineStateSubobjectStream = stream.data();

        dbgLogInfo("createMeshPipeline: stream=%zuB as=%d ps=%d rt=%d dsv=%d cull=%d wire=%d\n",
               static_cast<size_t>(sd.SizeInBytes), withAs ? 1 : 0, withPs ? 1 : 0, numRT,
               static_cast<int>(sDsv.payload), desc.cullEnabled ? 1 : 0, desc.polygonMode == 1 ? 1 : 0);
        const HRESULT h = device2->CreatePipelineState(&sd, IID_PPV_ARGS(&out));
        if (FAILED(h)) {
            // 调试层开着时（环境变量 DX12_DEBUG_LAYER=1），InfoQueue 会给出被拒的具体字段。
            ComPtr<ID3D12InfoQueue> iq;
            if (SUCCEEDED(device2->QueryInterface(IID_PPV_ARGS(&iq))) && iq) {
                const UINT64 stored = iq->GetNumStoredMessages();
                dbgLogWarn("PSO infoqueue: %llu stored message(s)", (unsigned long long)stored);
                for (UINT64 k = 0; k < stored && k < 8; ++k) {
                    SIZE_T len = 0;
                    if (FAILED(iq->GetMessage(k, nullptr, &len)) || len == 0) continue;
                    std::vector<char> buf(len);
                    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                    if (SUCCEEDED(iq->GetMessage(k, msg, &len))) {
                        dbgLogWarn("PSO infoqueue[%d]: %s", static_cast<int>(msg->Severity),
                                   msg->pDescription ? msg->pDescription : "");
                    }
                }
            }
            return mcdr::err("createMeshPipeline", "CreatePipelineState hr=" + hrHex(h));
        }
        return {};
    };

    // 4) 与图形管线同构地建两个 PSO：withDepth 总是建（DSV=D32_FLOAT），
    //    仅当管线无深度状态时再建 withoutDepth（DSV=UNKNOWN）。
    if (auto r = buildPso(!desc.asDxil.empty(), !desc.psDxil.empty(), pipeline->withDepth); !r)
        return std::unexpected(r.error());
    if (!desc.hasDepth) {
        const DXGI_FORMAT saved = sDsv.payload;
        sDsv.payload = DXGI_FORMAT_UNKNOWN;
        ds.DepthEnable = FALSE;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sDepth.payload = ds;
        auto r = buildPso(!desc.asDxil.empty(), !desc.psDxil.empty(), pipeline->withoutDepth);
        sDsv.payload = saved;
        if (!r) return std::unexpected(r.error());
    }

    return static_cast<void*>(pipeline.release());
}

}  // namespace

// ---------------------------------------------------------------------------
// dispatchMesh
// ---------------------------------------------------------------------------
mcdr::Result<void> dispatchMesh(void* ctxHandle, int x, int y, int z) {
    auto* ctx = static_cast<CommandContext*>(ctxHandle);
    if (ctx == nullptr) {
        return mcdr::err("dispatchMesh", "command context is null");
    }
    if (ctx->commandList == nullptr || !ctx->listOpen) {
        return mcdr::err("dispatchMesh", "command list is not open");
    }
    if (!ctx->inRenderPass) return mcdr::err("dispatchMesh", "render pass is not open");
    if (!ctx->currentPipeline || !ctx->currentPipeline->isMesh || !ctx->cachedPso)
        return mcdr::err("dispatchMesh", "mesh pipeline is not bound");
    if (x <= 0 || y <= 0 || z <= 0 || x > 65535 || y > 65535 || z > 65535
        || static_cast<uint64_t>(x) * y * z > (1ull << 22))
        return mcdr::err("dispatchMesh", "invalid group counts (axis <=65535, product <=4194304)");

    // DispatchMesh 从 SM 6.5 起支持；项目保留 FL 12_2 门控。
    ComPtr<ID3D12GraphicsCommandList6> list6;
    HRESULT hr = ctx->commandList->QueryInterface(IID_PPV_ARGS(&list6));
    if (FAILED(hr) || !list6) {
        return mcdr::err("dispatchMesh", "ID3D12GraphicsCommandList6 unavailable hr=" + hrHex(hr));
    }

    list6->DispatchMesh(static_cast<UINT>(x), static_cast<UINT>(y), static_cast<UINT>(z));
    ctx->colorTargetsWritten = true;
    for (size_t i = 0; i < ctx->activeColorTargetsTouched.size(); ++i)
        ctx->activeColorTargetsTouched[i] = true;
    return {};
}

}  // namespace d3d12_mc

namespace d3d12_mc {


// ---------------------------------------------------------------------------
// P129: 地形 + mesh 尾参的根签名
//
// 与 createMeshPipelineImpl 里的内联构造同构，只在 meshExtras 时多两个参数。
// 单独建出来的理由：地形 PSO 与 mesh PSO 必须共用同一份签名，否则 draw 侧
// 推的绑定（按地形布局算的槽位）对不上 mesh PSO。
// ---------------------------------------------------------------------------
mcdr::Result<uint64_t> createMeshRootSignature(const std::vector<PipelineDesc::Binding>& bindings,
                                               bool meshExtras) {
    if (const auto r = ensureDevice(); !r)
        return mcdr::err("createMeshRootSignature", r.error().message);

    std::vector<uint8_t> cbvRegs;
    std::vector<D3D12_DESCRIPTOR_RANGE> srvRanges;
    std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
    for (const PipelineDesc::Binding& b : bindings) {
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

    // 尾参：无描述符的 root CBV（段表）用 8 字节常量？不 —— 段表是 GPU 缓冲，走 root CBV。
    D3D12_ROOT_PARAMETER segmentCbv{};
    segmentCbv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    segmentCbv.Descriptor.ShaderRegister = 8;
    segmentCbv.Descriptor.RegisterSpace = 0;
    segmentCbv.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 顶点/索引缓冲：第二个描述符表（t14/t15），与地形的 SRV 表分开，避免影响
    // 地形 PSO 已固定的表内偏移。
    D3D12_DESCRIPTOR_RANGE vbRange{};
    vbRange.NumDescriptors = 1;
    vbRange.RegisterSpace = 0;
    vbRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    vbRange.BaseShaderRegister = 14;
    vbRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    D3D12_DESCRIPTOR_RANGE ibRange = vbRange;
    ibRange.BaseShaderRegister = 15;
    // P217：段表走 t15、meshlet 表走 t16，一条 DispatchMesh 覆盖整批。
    D3D12_DESCRIPTOR_RANGE sectionRange = vbRange;
    sectionRange.BaseShaderRegister = 16;
    const D3D12_DESCRIPTOR_RANGE geometryRanges[3] = {vbRange, ibRange, sectionRange};

    std::vector<D3D12_ROOT_PARAMETER> params;
    params.reserve(cbvRegs.size() + 3);
    for (uint8_t reg : cbvRegs) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = reg;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    if (!srvRanges.empty()) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(srvRanges.size());
        p.DescriptorTable.pDescriptorRanges = srvRanges.data();
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    if (meshExtras) {
        params.push_back(segmentCbv);
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable.NumDescriptorRanges = 3u;
        p.DescriptorTable.pDescriptorRanges = geometryRanges;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }

    // P217：尾参的参数索引 —— CBV 按 binding 顺序稠密排，之后是（可能存在的）地形
    // SRV 表，再之后才是 b8 小常量与 t14/t15/t16 表。这个顺序由上面 params 的推入
    // 顺序决定；drawMeshlets 靠它去 SetGraphicsRootConstantBufferView / DescriptorTable。
    const UINT segmentParam = static_cast<UINT>(cbvRegs.size()) + (srvRanges.empty() ? 0u : 1u);
    const UINT geometryParam = segmentParam + 1u;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(params.size());
    rsDesc.pParameters = params.empty() ? nullptr : params.data();
    rsDesc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
    rsDesc.pStaticSamplers = staticSamplers.empty() ? nullptr : staticSamplers.data();
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (!meshExtras) {
        // 图形管线（地形）要保留 IA 输入布局标志；mesh 变体不能有。
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    }

    ComPtr<ID3DBlob> rsBlob, rsErr;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    versioned.Desc_1_0 = rsDesc;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&versioned, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        std::string msg = "D3D12SerializeVersionedRootSignature hr=" + hrHex(hr);
        if (rsErr && rsErr->GetBufferSize() > 0) {
            msg += " ";
            msg.append(static_cast<const char*>(rsErr->GetBufferPointer()), rsErr->GetBufferSize());
        }
        return mcdr::err("createMeshRootSignature", std::move(msg));
    }
    ComPtr<ID3D12RootSignature> rootSig;
    hr = deviceContext().device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                                     IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        return mcdr::err("createMeshRootSignature", "CreateRootSignature hr=" + hrHex(hr));
    }
    ID3D12RootSignature* raw = rootSig.Detach();
    const uint64_t handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(raw));
    // 只有带尾参的签名才需要画像（图形管线的签名不进 drawMeshlets）。
    if (meshExtras) recordMeshRootSig(handle, segmentParam, geometryParam);
    return handle;
}

void releaseMeshRootSignature(uint64_t rootSignature) {
    if (rootSignature == 0) return;
    forgetMeshRootSig(rootSignature);
    auto* sig = reinterpret_cast<ID3D12RootSignature*>(static_cast<uintptr_t>(rootSignature));
    sig->Release();
}

mcdr::Result<void*> createMeshPipeline(const MeshPipelineDesc& desc) {
    return createMeshPipelineImpl(desc, 0);
}

mcdr::Result<void*> createMeshPipeline(const MeshPipelineDesc& desc, uint64_t externalRootSignature) {
    return createMeshPipelineImpl(desc, externalRootSignature);
}

}  // namespace d3d12_mc
