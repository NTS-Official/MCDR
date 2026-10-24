# MCDR
[[English](README.md)] | [简体中文]

<div align="center">

<img src="https://img.shields.io/github/actions/workflow/status/NTS-Official/MCDR/msbuild.yml?style=for-the-badge">
<img src="https://img.shields.io/badge/Powered%20by%20Kotlin-Java%20FFM%20API-orange?style=for-the-badge&logo=kotlin">
<img src="https://img.shields.io/badge/DirectX%2012-grey?style=for-the-badge&logo=cplusplus&label=C%2B%2B23&labelColor=blue">
<img src="https://img.shields.io/badge/Powered%20by%20Streamline-green?style=for-the-badge&logo=nvidia&labelColor=grey">
<img src="https://img.shields.io/badge/Powered%20by%20Fidelity%20FX-grey?style=for-the-badge&logo=amd&labelColor=red">
<img src="https://img.shields.io/badge/Powered%20by%20XeSS-grey?style=for-the-badge&logo=intel&labelColor=blue">

</div>

## 介绍

对 DirectXMod 的原生库部分进行了现代化，以及构建系统的优化：

- 把构建系统从 CMake 换到了 MSBuild，以换取 Visual Studio 上更好的使用体验。<!-- 很显然，作为一个 dx12 的项目，使用除 msvc 以外的编译器和 msbuild 以外的构建系统是没有必要且徒增负担的，因为你的目标平台只可能是 windows -->
- 访问外部库的形式从 JNI 换到了 Project Panama 绑定。
- 从 C++17 换到了 MSVC v145 的 /std:c++latest （即 C++23 以及少量的 26 特性）。
- 使用了较新的 Agility SDK，将 SM5.1 升级到了 SM6.8。
- 添加了各种小的工具性的包，例如 nlohmann-json 用来方便地拼 JSON。
- ……

# 鸣谢
这是鸣谢部分：

> [!NOTE]
> ### CC0
>
> 本项目的部分代码修改自 [DirectXMod](https://github.com/366862732/DirectXmod)，作者为 [爱睡觉的白洲梓](https://github.com/366862732)。