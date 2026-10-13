# MCDR

D3D12 native library part of the project DXRMC.

纯 C ABI + C++23 模块的 native DLL。构建：msvc **v145**（VS2026 / MSBuild）+
`/std:c++latest`，警告级别 **/W4 且 `TreatWarningAsError=true`**，`MCDR\MCDR.vcxproj`，
配置 Release|x64（Debug|x64 亦可）。

## 布局

```
include\mcdr_abi.h              唯一保留的头文件：extern "C" C ABI（JVM 侧经 FFM 调用）
include\_d3d12_module_prelude.h 各模块全局模块片段共用的系统头（含 NOMINMAX 保护、DBG_LOG_DEBUG）
src\module\c_abi\               mcdr.abi.common 模块接口单元
src\module\core\                8 个 dx12 模块接口单元中的 core 组
src\module\advanced\            mcdr.dx12.mesh 接口单元
src\c_abi\                      C ABI 适配 TU，按功能命名（见下表）
src\core\                       D3D12 实现单元（模块实现单元）+ DLL 入口点 dllmain.cpp
src\advanced\                   mesh 管线的非模块实现
```

C++ 源码已全量迁移到 C++23 模块（2026-10-13）；`src\` 下不再有自研 `.h`。

| 模块名 | 接口单元 | 实现单元 |
| --- | --- | --- |
| `mcdr.abi.common` | `src\module\c_abi\mcdr_abi_common.cppm` | — |
| `mcdr.dx12.descriptor_allocator` | `src\module\core\d3d12_descriptor_allocator.cppm` | `src\core\d3d12_descriptor_allocator.cpp` |
| `mcdr.dx12.async_descriptor` | `src\module\core\d3d12_async_descriptor.cppm` | `src\core\d3d12_async_descriptor.cpp` |
| `mcdr.dx12.async_fence` | `src\module\core\d3d12_async_fence.cppm` | `src\core\d3d12_async_fence.cpp` |
| `mcdr.dx12.async_executor` | `src\module\core\d3d12_async_executor.cppm` | —（并入接口单元） |
| `mcdr.dx12.async_bundle` | `src\module\core\d3d12_async_bundle.cppm` | —（并入接口单元） |
| `mcdr.dx12.device` | `src\module\core\mcdr_device.cppm` | `src\core\d3d12_device.cpp`、`src\core\d3d12_surface.cpp` |
| `mcdr.dx12.mesh` | `src\module\advanced\d3d12_mesh.cppm` | `src\advanced\d3d12_mesh_base.cpp` |

新增 `.cppm` 只需作为 `<ClCompile>` 项加入 vcxproj：`LanguageStandard=stdcpplatest` 会让
MSBuild 自动按 `CompileAsCppModule` 编译，并自动开启 `ScanSourceForModuleDependencies`。

## C ABI 适配层（`src\c_abi\`）

原先按提交顺序命名的 `abi_bridge.cpp` / `abi_bridge_p3..p7.cpp` / `abi_bridge_p33.cpp`
已按功能重新命名并合并（合并只做拼接，函数体逐字未改）：

| 文件 | 功能关键词 |
| --- | --- |
| `abi_common.cpp` | 公共错误状态与字符串写出（`mcdrLastError`、`mcdrSetLogVerbose`） |
| `abi_meta.cpp` | ABI 元信息与布局自检（`mcdrABIVersion`、`mcdrABILayout`） |
| `abi_device.cpp` | 设备、资源与隐藏窗口入口 |
| `abi_command.cpp` | 命令层：encoder 生命周期、拷贝、清屏、render pass、时间戳查询、异步渲染控制通道 |
| `abi_pipeline.cpp` | 图形管线创建/销毁 |
| `abi_draw.cpp` | render pass 内的绘制录制 |
| `abi_mesh.cpp` | P72 mesh 管线与 HLSL → DXIL 编译 |
| `abi_surface.cpp` | DXGI 交换链 surface：创建/配置/获取/Blit/Present、back buffer 句柄、像素回读 |
| `abi_async.cpp` | 异步渲染基础设施：分区描述符分配器、bundle 录制池、fence 管理器、主列表执行器、屏障预处理 |

排序与包含关系：`abi_common` → `abi_meta` → `abi_device` → `abi_command` → `abi_pipeline`
→ `abi_draw` → `abi_mesh` → `abi_surface` → `abi_async`（也是 vcxproj 与 `.filters` 中的顺序）。

## 改 native 时的三条硬约束

1. `.cppm` 内声明的导出函数必须在**同一个模块单元**内定义（接口单元声明 + `module <名>;` 实现单元
   定义，或直接定义在 `.cppm` 里），否则定义侧与引用侧修饰名不一致 → `LNK2001`/`LNK2019`。
2. 导出签名里**不要出现别的模块拥有的类型**。v145 会把它们记成模块私有名
   （`Dx12Pipeline@$$_Amcdr.dx12.device`），而普通 TU 消费方按 `mcdr_abi.h` 的前向声明只能得到公共名
   `@1`，两侧永远对不上。跨界只用不透明句柄（`void*` / `uintptr_t`），例如
   `createMeshPipeline`、`dispatchMesh`。
3. `#include` 一律放在全局模块片段（`module;` 与 `export module` 之间）。放进模块范围内会触发
   `warning C5244`，本项目 `TreatWarningAsError=true`，直接构建失败。内部统一走
   `_d3d12_module_prelude.h`。

`src\core\dllmain.cpp` 是刻意的**普通翻译单元**（`extern "C"` + `DllMain`），不属于任何命名模块：
模块无法导出 C 语言链接的实体，而 `DllMain` 由 CRT 按名字查找，C 链接才稳。别把它并进 `.cppm`。

细节与实测记录见仓库根目录 `known_bugs.md` 第 13 节。
