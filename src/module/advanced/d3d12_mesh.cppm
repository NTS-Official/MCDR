// dx12_mesh.h — P72：mesh shader（amplification / mesh）管线基础
// ---------------------------------------------------------------------------
// 与 dx12_device.cpp 里的图形管线（D3DCompile vs_5_1/ps_5_1 + D3D12_GRAPHICS_
// PIPELINE_STATE_DESC）刻意分开：
//   * mesh 管线只能由 DXC 产出的 DXIL（as_6_x / ms_6_x）经
//     D3D12_PIPELINE_STATE_STREAM_DESC + ID3D12Device2::CreatePipelineState 创建；
//   * 绘制走 ID3D12GraphicsCommandList6::DispatchMesh（不是 Draw*）。
//
// P72 只铺基础：AS/MS/PS + 不透明写入 + 渲染目标格式 + 可选深度。混合状态（blend）
// 与 mesh 专属特性（payload 校验、amplification 输出组数）留待后续。
//
// 迁移说明：本文件由 dx12_mesh.h 逐行改写而来。原头文件包含 dx12_device.h，
// 这里改为 import mcdr.d3d12.device —— mesh 依赖设备模块的 PipelineDesc /
// Dx12Pipeline / CommandContext / deviceContext，方向单一（设备模块不反向依赖
// mesh），没有 import 环。

module;

#include "_d3d12_module_prelude.h"

export module mcdr.d3d12.mesh;

// 只 import 不 export：导出签名里已经不含 device 模块的类型（见下方注释），
// 消费方不需要经本模块看到 Dx12Pipeline / CommandContext，也就不需要 re-export。
// （曾经用 export import 试过绕开 LNK2001，实测无效——真正的修法见下。）
import mcdr.d3d12.device;   // PipelineDesc / Dx12Pipeline / CommandContext / deviceContext（仅本单元内部使用）

export namespace d3d12_mc {

using Microsoft::WRL::ComPtr;
// Java 侧 dx12CreateMeshPipeline 的 desc 解析产物。
struct MeshPipelineDesc {
    // DXC 编出的 DXIL 字节码（不是 HLSL 源码）。asDxil 可空（不用 amplification），
    // msDxil 必填；psDxil 可空（无 PS 时不写颜色目标）。
    std::vector<uint8_t> asDxil;
    std::vector<uint8_t> msDxil;
    std::vector<uint8_t> psDxil;

    int colorCount = 0;
    std::vector<PipelineDesc::ColorTarget> colorTargets;
    bool hasDepth = false;
    int depthFormat = 0;  // GpuFormat ordinal（与图形管线同义；DSV 固定 D32_FLOAT，同图形管线）
    bool depthWrite = false;
    int depthCompareOp = 0;
    bool cullEnabled = false;
    int polygonMode = 0;  // 0=FILL，1=WIREFRAME
    // 与图形管线同构：0=CBV（root descriptor），1=SRV+static sampler，2=SRV（texel buffer）。
    // 顺序即 register 顺序，保证 mcdr_dx12PushDescriptors 无需改动即可复用。
    std::vector<PipelineDesc::Binding> bindings;
};

// 创建 mesh 管线（失败返回 nullptr 并把原因写入 err）。root signature 用 1.1 版本且
// 不带 ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT（mesh 管线没有 IA 阶段）。
//
// 返回/入参刻意用不透明指针（void*）而不是 Dx12Pipeline*/CommandContext*：
// 这两个类型属于 mcdr.d3d12.device 模块，普通翻译单元（消费方 abi_mesh.cpp）
// 按 mcdr_abi.h 的前向声明只能得到公共名 @1，而定义侧一定记成模块私有名
// （Dx12Pipeline@$$_Amcdr.dx12.device）—— 两侧修饰名不可能一致，LNK2001。
// 句柄在 Java 侧本来就只是 long，消费方全程只搬运不解引用，因此换上不透明指针
// 后 ABI 语义不变，且彻底摆脱跨模块类型修饰差异。

void* createMeshPipeline(const MeshPipelineDesc& desc, std::string& err);

// 本机是否具备 mesh 管线的最小条件（12_2 + mesh shader tier ≥ 1 + DXC 能编 ms_6_x
// + 能拿到 ID3D12Device2）。why 收到不满足的原因（可为 nullptr）。
bool meshPipelineSupported(std::string* why);

// DispatchMesh：x/y/z = 线程组数量（ctx 为 mcdr_command_ctx 句柄）。要求当前已
// setPipeline 一个 mesh 管线。
bool dispatchMesh(void* ctx, int x, int y, int z, std::string& err);

}  // export namespace d3d12_mc —— 导出接口到此为止

// 实现见 dx12_mesh_impl.cpp（模块实现单元）。导出函数的定义【不能】放在接口单元里：
// 实测接口单元内的定义会把签名中的 device 类型记成模块私有名
// （Dx12Pipeline@$$_Amcdr.dx12.device），而消费方按公共名 @1 引用 → LNK2001。
// 放到实现单元后两侧修饰名一致（与 mcdr_device.cppm + dx12_device.cpp 同一套路数）。
// 注意：即便放进实现单元，只要签名里仍出现 Dx12Pipeline*/CommandContext*，修饰名依旧
// 是模块私有名；真正的修法是把这两个类型从导出签名里去掉（上方 void* 句柄）。
