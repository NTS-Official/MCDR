# MCDR
[English] | [[简体中文](docs/README_ZH.MD)]

> [!Note]
> Shaders compiling chain is undergoing a rewrite. Hybrid RT/PT solution is still in very early development.

<div align="center">

<img src="https://img.shields.io/github/actions/workflow/status/NTS-Official/MCDR/msbuild.yml?style=for-the-badge">

</div>

## Introduction
MCDR is the Direct3D 12 native library part of the project DXRMC.

The native library portion of DirectXMod has been modernized, along with a set of build system changes:

- The CMake build system was replaced with MSBuild, so it fits the Visual Studio IDE and the Windows target platform perfectly.
- External libraries are now accessed through Project Panama bindings instead of JNI/JNA.
- C++17 was replaced with `/std:c++latest` of MSVC v145 (that is, C++23 plus a small number of C++26 features).
- The latest Agility SDK is used, upgrading SM5.1 to SM6.8.
- Various small utility packages were added, such as nlohmann-json for conveniently assembling JSON.
- …

## Documentation
Design docs live in `docs/`, starting at [`docs/overview.md`](docs/overview.md). They cover the C ABI contract, the device / surface / async layers, DLSS, frame generation, super resolution, mesh shaders and the ray tracing capability probe, plus the build and module rules. Source comments only carry constraints that break the build or the picture when violated; data flow, invariants and known defects live in the docs.

# Acknowledgments
This is the acknowledgements part of the project DXRMC.

> [!NOTE]
> ### CC0
>
> Part code of this project was modified from [DirectXMod](https://github.com/366862732/DirectXmod), whose author is [xiaozi](https://github.com/366862732).
