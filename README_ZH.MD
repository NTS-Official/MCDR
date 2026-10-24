# MCDR
[[English](README.md)] | [简体中文]

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


# 鸣谢
这是鸣谢部分：

> [!NOTE]
> ### CC0
>
> 本项目的部分代码修改自 [DirectXMod](https://github.com/366862732/DirectXmod)，作者为 [爱睡觉的白洲梓](https://github.com/366862732)。