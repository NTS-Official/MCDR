# MCDR
[[English](../README.md)] | [简体中文]

> [!Note]
> 着色器编译链正在经历重写，混合光追方案仍在非常不成熟的早期当中。


<div align="center">

<img src="https://img.shields.io/github/actions/workflow/status/NTS-Official/MCDR/msbuild.yml?style=for-the-badge">

</div>

## 介绍

对 DirectXMod 的原生库部分进行了现代化，以及构建系统的变更：

- CMake 构建系统更换为 MSBuild，以完美适配 Visual Studio IDE 与 Windows 目标平台。
- 访问外部库的形式从 JNI/JNA 换到了 Project Panama 绑定。
- 从 C++17 换到了 MSVC v145 的 `/std:c++latest` （即 C++23 以及少量的 26 特性）。
- 使用了最新的 Agility SDK，将 SM5.1 升级到了 SM6.8。
- 添加了各种小的工具性的包，例如 nlohmann-json 用来方便地拼 JSON。
- ……

## 设计文档
设计文档在 `docs/`，从 [`docs/overview.md`](docs/overview.md) 开始读：C ABI 契约、设备 / 交换链 / 异步三层、DLSS、帧生成、超分、mesh 与光追能力探测，以及构建与模块规则。源码注释只写“违反就会出错”的约束，数据流、不变式与已知缺陷都在文档里。

# 鸣谢
这是鸣谢部分：

> [!NOTE]
> ### CC0
>
> 本项目的部分代码修改自 [DirectXMod](https://github.com/366862732/DirectXmod)，作者为 [爱睡觉的白洲梓](https://github.com/366862732)。