<#
.SYNOPSIS
    把 external\ 下的四个子模块整理成 vcxproj 期望的布局，并检查运行时 DLL 是否就位。

.DESCRIPTION
    四个外部 SDK 现在都是 git 子模块（.gitmodules）：
        external\dlss       NVIDIA/DLSS                            v310.9.1
        external\nrd        NVIDIA-RTX/NRD                         v4.18.0
        external\xess       intel/xess                             v3.0.2
        external\fidelityfx GPUOpen-LibrariesAndSDKs/FidelityFX-SDK v2.3.0
    另有第五个（P111，Streamline 探针用）：
        external\streamline NVIDIA-RTX/Streamline                  v2.14.16
    以及第六个（P2xx，神经辐照缓存 NRC，二进制发行包）：
        external\nrc        NVIDIA-RTX/NRC                         v0.16.0.0

    只有 XeSS 的上游布局和本项目一致（inc\ + lib\ + bin\）。其余三个需要少量搬运，
    本脚本把这些搬运固定下来，保证“新克隆 -> 干净构建”可复现：

      1. 子模块初始化：git submodule update --init --depth 1 external/<lib>
      2. DLSS：导入库在上游 lib\Windows_x86_64\x64\，vcxproj 直接指向那里；
         运行时 nvngx_*.dll 不在上游仓库里，必须是 MCDR\lib\dlss\ 下单独准备的那份
         （git 不记录，见 .gitignore 的 /lib/）。
      3. NRD：上游不带预编译静态库，Lib\x64\<cfg>\NRD.lib 与 ShaderMakeBlob.lib
         需用 external\nrd 的 CMake 工程编译后放进去（本脚本缺失时只报命令，不自动编）。
      4. FidelityFX：头文件/导入库从 Kits\FidelityFX\api\include\ 与
         Kits\FidelityFX\signedbin\ 同步到 external\fidelityfx\include|lib\
         （派生目录，已 gitignore）；运行时 amd_fidelityfx_*.dll 同样在
         MCDR\lib\fidelityfx\（git 不记录）。

.PARAMETER Check
    只检查、不复制。
#>
[CmdletBinding()]
param(
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $PSScriptRoot          # MCDR\
$ext    = Join-Path $root 'external'
$libDir = Join-Path $root 'lib'

function Say([string]$msg) { "  $msg" }
function Ok([string]$msg)  { "  [ok]   $msg" }
function Bad([string]$msg) { "  [miss] $msg" }

function Copy-Tree([string]$from, [string]$to) {
    if (-not (Test-Path $from)) { Bad "源目录不存在: $from"; return }
    if ($Check) { Say "would copy $from -> $to"; return }
    New-Item -ItemType Directory -Path $to -Force | Out-Null
    Get-ChildItem $from -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($from.Length).TrimStart('\')
        $dst = Join-Path $to $rel
        New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
        Copy-Item $_.FullName $dst -Force
    }
    Ok "copied $from -> $to"
}

'== 1) 子模块 =='
foreach ($s in @('dlss','nrd','xess','fidelityfx','nrc')) {
    $p = Join-Path $ext $s
    if (Test-Path (Join-Path $p '.git')) { Ok "$s 已初始化" }
    else { Bad "$s 未初始化：git -C `"$root`" submodule update --init --depth 1 external/$s" }
}

'== 2) DLSS 导入库（vcxproj 指向 lib\Windows_x86_64\x64）=='
$dlssLib = Join-Path $ext 'dlss\lib\Windows_x86_64\x64'
if (Test-Path $dlssLib) { Ok "DLSS 导入库就位" } else { Bad "缺少 $dlssLib（子模块未初始化？）" }

'== 3) NRD 预编译静态库（上游不带，需自行编译）=='
foreach ($cfg in @('Release','Debug')) {
    $nrdLib = Join-Path $ext "nrd\Lib\x64\$cfg\NRD.lib"
    if (Test-Path $nrdLib) { Ok "NRD.lib ($cfg) 就位" }
    else {
        Bad "缺少 $nrdLib"
        Say "用 CMake 编译 external\nrd，再把 NRD.lib 与 ShaderMakeBlob.lib 放进 Lib\x64\<cfg>\；"
        Say "参考：cmake -S external\nrd -B external\nrd\_build -A x64 -DNRD_BUILD_SHARED_LIBS=OFF 然后 cmake --build external\nrd\_build --config Release"
    }
}

'== 4) FidelityFX 头/导入库同步 + 运行时 DLL 检查 =='
Copy-Tree (Join-Path $ext 'fidelityfx\Kits\FidelityFX\api\include') (Join-Path $ext 'fidelityfx\include')
$ffxBin = Join-Path $ext 'fidelityfx\Kits\FidelityFX\signedbin'
if (Test-Path $ffxBin) {
    $src = Join-Path $ffxBin 'amd_fidelityfx_loader_dx12.lib'
    $dstDir = Join-Path $ext 'fidelityfx\lib'
    if (Test-Path $src) {
        if (-not $Check) { New-Item -ItemType Directory -Path $dstDir -Force | Out-Null; Copy-Item $src (Join-Path $dstDir 'amd_fidelityfx_loader_dx12.lib') -Force }
        Ok "FFX loader 导入库已同步"
    } else { Bad "缺少 $src" }
} else { Bad "缺少 $ffxBin" }

$runtime = [ordered]@{
    'dlss'       = @('nvngx_dlss.dll','nvngx_dlssd.dll','nvngx_dlssg.dll')
    'xess'       = @('libxess.dll','libxess_fg.dll','libxell.dll')
    'fidelityfx' = @('amd_fidelityfx_loader_dx12.dll','amd_fidelityfx_upscaler_dx12.dll',
                     'amd_fidelityfx_denoiser_dx12.dll','amd_fidelityfx_radiancecache_dx12.dll',
                     'amd_fidelityfx_framegeneration_dx12.dll')
}
foreach ($k in $runtime.Keys) {
    foreach ($n in $runtime[$k]) {
        $p = Join-Path $libDir "$k\$n"
        if (Test-Path $p) { Ok "运行时 DLL: lib\$k\$n" } else { Bad "缺少运行时 DLL: $p" }
    }
}

'== 5) Streamline（P111 探针；上游仓库不含 lib\/bin，运行时来自 release zip）=='
$slInclude = Join-Path $ext 'streamline\include\sl.h'
if (Test-Path $slInclude) { Ok "Streamline 头文件就位（external\streamline\include）" }
else { Bad "缺少 $slInclude：git -C `"$root`" submodule update --init --depth 1 external/streamline" }

$slLib = Join-Path $libDir 'streamline\lib\x64\sl.interposer.lib'
if (Test-Path $slLib) { Ok "sl.interposer.lib 就位" }
else {
    Bad "缺少 $slLib"
    Say "从 release zip 取：https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip"
    Say "解压后把 lib\x64\sl.interposer.lib 放到 MCDR\lib\streamline\lib\x64\，"
    Say "把 bin\x64\sl.*.dll 与 nvngx_deepdvc.dll 放到 MCDR\lib\streamline\（git 不记录 lib\）。"
}

foreach ($n in @('sl.interposer.dll','sl.common.dll','sl.dlss.dll','sl.dlss_g.dll','sl.reflex.dll','nvngx_deepdvc.dll')) {
    $p = Join-Path $libDir "streamline\$n"
    if (Test-Path $p) { Ok "运行时 DLL: lib\streamline\$n" } else { Bad "缺少运行时 DLL: $p" }
}

'== 6) NRC 运行期（external\nrc\Bin\x64 -> lib\nrc；NVRTC/CUDA 必须与 MCDR.dll 同目录）=='
$nrcBin = Join-Path $ext 'nrc\Bin\x64'
$nrcDst = Join-Path $libDir 'nrc'
if (Test-Path $nrcBin) {
    foreach ($n in @('NRC_D3D12.dll','cudart64_13.dll','nvrtc64_130_0.dll','nvrtc-builtins64_134.dll')) {
        $src = Join-Path $nrcBin $n
        if (-not (Test-Path $src)) { Bad "缺少 $src"; continue }
        if ($Check) { Say "would copy $src -> $(Join-Path $nrcDst $n)"; continue }
        New-Item -ItemType Directory -Path $nrcDst -Force | Out-Null
        Copy-Item $src (Join-Path $nrcDst $n) -Force
        Ok "copied lib\nrc\$n"
    }
    $nrcLib = Join-Path $ext 'nrc\Lib\x64\NRC_D3D12.lib'
    if (Test-Path $nrcLib) { Ok "NRC_D3D12.lib 就位（vcxproj 的 include 指向 external\nrc\Include）" }
    else { Bad "缺少 $nrcLib" }
} else {
    Bad "缺少 $nrcBin：git -C `"$root`" submodule update --init --depth 1 external/nrc"
    Say 'NRC 是二进制发行包（DLL + import lib + CUDA 13 运行库 + 头文件），初始化子模块后本步骤才搬运行期。'
}

''
if ($Check) { Say '检查模式结束（未写入任何文件）' } else { Say '同步完成' }