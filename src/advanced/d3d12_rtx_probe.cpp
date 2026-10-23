// MCDR 超分/降噪 SDK 可用性探测实现 —— 见 include\mcdr_rtx_support.h 的设计说明。
//
// 这个文件是普通翻译单元（不是模块实现单元），理由和头文件一样：它被设备模块
// 调用，而设备模块不能反过来 import 它。它自身不 import 任何 mcdr.* 模块，
// 因此也不会形成环。
#include <windows.h>
#include <winver.h>  // GetFileVersionInfoW / VerQueryValueW：取 DLL 的文件版本资源

#include "mcdr_rtx_support.h"

// P108：NGX 运行期加载现在由 d3d12_dlss.cpp 负责（它才有设备模块注入的 dbgLog*，
// 而逐条 LoadLibrary 取证必须写进 native 日志）。这里只声明用到的那两个入口，
// 故意不 include mcdr_dlss_impl.h —— 探测不需要该头里的其它声明。
// 必须是 extern "C"：普通 TU 之间只有 C 链接的名字才能对上（实现在 dlss.cpp 的
// extern "C" 块里）。
extern "C" {
void* mcdr_dlss_load_runtime(void);
const char* mcdr_dlss_runtime_path(void);
// P108: the NVSDK_NGX_FeatureCommonInfo whose PathListInfo names the vendor directory. NGX
// needs it to find its feature DLLs; without it the requirement query answers
// FAIL_InvalidParameter (verified in-game and out).
void* mcdr_dlss_ngx_feature_info(void);
// P108: registers the vendor directory with the DLSS implementation, which also builds the
// NVSDK_NGX_FeatureCommonInfo path list. MUST run before the requirement query below,
// otherwise the query passes FeatureInfo=NULL and NGX cannot find its feature DLLs.
int32_t mcdr_dlss_register_vendor_dir(const char* utf8Dir, int32_t cap);
}

// JSON 一律用 vcpkg 的 nlohmann/json（vcpkg.json 已声明 nlohmann-json）。这里拼的是
// 「字段串」（不含外侧花括号），由调用方并入更大的对象。
// 必须放在 import std 之前：该头会 include <string>/<map> 等标准头，
// 在 import std 之后再 include 会被 MSVC 拒绝。
#include <nlohmann/json.hpp>

import std;
// ---------------------------------------------------------------------------
// NRD：静态库，直接链进 MCDR.dll，没有运行期 DLL
// ---------------------------------------------------------------------------
#if defined(MCDR_HAS_NRD)
#include "NRD.h"
#endif

// ---------------------------------------------------------------------------
// DLSS / NGX：导入库只给入口，真正实现是运行期的 nvngx_dlss.dll
// ---------------------------------------------------------------------------
#if defined(MCDR_HAS_DLSS)
#include "nvsdk_ngx.h"

// 厂商建议在缺省 NVIDIA 应用 ID 时用 ProjectId + ENGINE_TYPE_CUSTOM，
// 见 nvsdk_ngx_defs.h 中 NVSDK_NGX_Application_Identifier 的注释。
// P108：ProjectId 用 **36 字符 GUID**（与 d3d12_dlss.cpp 的 MCDR_NGX_PROJECT_ID 逐字相同）。
// 依据：与我们同代（310.x）的生产实现（ETS2/ATS DLAA mod，src/dlaa.cpp）就是这么写的，
// 注释为 "any stable GUID works for unregistered apps"。两处必须一致 —— 探测与 Init 用
// 同一个应用身份，NGX 的每次查询才归到同一个 app 上。
#define MCDR_NGX_PROJECT_ID "7c1f4a52-9b3e-4d68-8f21-5a6c0e2d9b74"
#define MCDR_NGX_ENGINE_VERSION "1.0"

// NGX 运行期 DLL。SDK 里同时存在 nvngx_dlss.dll（超分/帧生成）与
// nvngx_dlssd.dll（光线重建），探测阶段只确认主运行时能加载：
// GetFeatureRequirements 自己会去摸它需要的那些。
#define MCDR_NGX_RUNTIME_DLL L"nvngx_dlss.dll"

// NGX 在 ApplicationDataPath 下写日志/缓存，给个相对目录即可（进程工作目录）。
#define MCDR_NGX_APPDATA L".\\ngx"
#endif

// ---------------------------------------------------------------------------
// XeSS（Intel）：头文件只用于**取版本结构体**，不用它声明函数
// ---------------------------------------------------------------------------
// 故意不把 xess.h 里的 XESS_API 函数声明引进来：那些声明要配合导入库
// （libxess.lib）才能链接，而本探测一律走 LoadLibrary + GetProcAddress，
// 这样既不用改 AdditionalDependencies，也不会让 MCDR.dll 在缺少 libxess.dll
// 的机器上直接加载失败。xess_version_t 自己再声明一遍（布局与上游一致，
// 见 xess\inc\xess\xess.h:55-70：4 个 uint16_t）。
#if defined(MCDR_HAS_XESS)
struct MCDR_XessVersion
{
    unsigned short major;
    unsigned short minor;
    unsigned short patch;
    unsigned short reserved;
};
// xessGetVersion 是**全局**函数，不需要设备、不需要上下文，所以版本号一加载
// DLL 就能拿到。（xessD3D12CreateContext 才需要 ID3D12Device，那属于「真能跑」
// 那一层，本探测不做。）
using mcdr_xessGetVersion_fn = int (*)(MCDR_XessVersion* pVersion);
// xess_result_t: XESS_RESULT_SUCCESS = 0（见 xess.h 的枚举顺序）。
#define MCDR_XESS_RESULT_SUCCESS 0
#endif

// ---------------------------------------------------------------------------
// FidelityFX（AMD FSR）：完全没有导入库，api 就在 DLL 里
// ---------------------------------------------------------------------------
// 上游不发 .lib：ffx_api.h:142-166 的导出函数用 FFX_API_ENTRY = __declspec(dllexport)
// 声明，配套的 ffx_api_loader.h:50-61 就是一份 GetProcAddress 加载器。所以这里
// 直接照抄那 5 个入口的类型和名字（x64 上默认 __cdecl，无修饰名）。
// 结构体声明用得到，故包含上游头；它们在 import std; 之后包含，符合本仓库
// 既有的 vendor 头包含顺序。
#if defined(MCDR_HAS_FFX)
#include "ffx_api.h"
using mcdr_ffxCreateContext_fn = PfnFfxCreateContext;
using mcdr_ffxDestroyContext_fn = PfnFfxDestroyContext;
using mcdr_ffxConfigure_fn = PfnFfxConfigure;
using mcdr_ffxQuery_fn = PfnFfxQuery;
using mcdr_ffxDispatch_fn = PfnFfxDispatch;
#endif

namespace
{
    std::string hex32(unsigned long long v)
    {
        char b[32] = {};
        std::snprintf(b, sizeof(b), "0x%08llX", v);
        return std::string(b);
    }

    // 取本模块（MCDR.dll）所在目录，末尾带反斜杠。失败返回空串。
    std::string moduleDirectory()
    {
        wchar_t path[MAX_PATH * 2] = {};
        const DWORD n = ::GetModuleFileNameW(nullptr, path, std::size(path));
        if (n == 0 || n >= std::size(path))
        {
            return {};
        }
        std::wstring w(path, n);
        const std::wstring::size_type slash = w.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            return {};
        }
        w.resize(slash + 1);
        // 只需要拼路径用，转成 UTF-8 交给 std::string（项目其它地方的路径都是 UTF-8）。
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (need <= 0)
        {
            return {};
        }
        std::string out(static_cast<std::size_t>(need), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                              nullptr, nullptr);
        return out;
    }

    // std::wstring -> UTF-8 std::string，非法字符不报错（诊断文字，不值得为它失败）。
    std::string narrow(const std::wstring& w)
    {
        if (w.empty())
        {
            return {};
        }
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (need <= 0)
        {
            return {};
        }
        std::string out(static_cast<std::size_t>(need), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                              nullptr, nullptr);
        return out;
    }

    // 取已加载模块的实际路径（UTF-8）。失败返回空串。
    std::string loadedModulePath(HMODULE mod)
    {
        wchar_t path[MAX_PATH * 2] = {};
        const DWORD n = ::GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path)));
        if (n == 0)
        {
            return {};
        }
        return narrow(std::wstring(path, n));
    }

    // 按「模块同目录优先、其次系统搜索路径」加载一个运行期 DLL。
    // 顺序的理由：实机部署时厂商 DLL 与 MCDR.dll 放在一起，而进程工作目录未必
    // 是那里；同时系统目录里的同名声（如果用户装了驱动自带版本）也应当能用。
    // 返回已加载的模块，失败返回 nullptr。
    HMODULE loadRuntimeDll(const wchar_t* dllName)
    {
        HMODULE mod = nullptr;
        const std::string dir = moduleDirectory();
        if (!dir.empty())
        {
            const std::wstring wdir(dir.begin(), dir.end());
            mod = ::LoadLibraryW((wdir + dllName).c_str());
        }
        if (mod == nullptr)
        {
            mod = ::LoadLibraryW(dllName);
        }
        return mod;
    }

    // 只查文件版本资源，不加载 DLL。XeSS / FidelityFX 的版本号都可以这样白拿，
    // 而 FidelityFX 甚至连「加载」这一步都不必做就能报出版本。
    // 返回形如 "2.3.0.0"；拿不到返回空串。
    std::string fileVersionOf(const std::wstring& fullPath)
    {
        DWORD dummy = 0;
        const DWORD size = ::GetFileVersionInfoSizeW(fullPath.c_str(), &dummy);
        if (size == 0)
        {
            return {};
        }
        std::vector<unsigned char> buf(size);
        if (::GetFileVersionInfoW(fullPath.c_str(), 0, size, buf.data()) == FALSE)
        {
            return {};
        }
        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT len = 0;
        if (::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) == FALSE ||
            ffi == nullptr || len < sizeof(VS_FIXEDFILEINFO))
        {
            return {};
        }
        char v[64] = {};
        std::snprintf(v, sizeof(v), "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS),
                      LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS),
                      LOWORD(ffi->dwFileVersionLS));
        return std::string(v);
    }

#if defined(MCDR_HAS_DLSS)
    // 把 NGX 返回码翻译成人能读的短句。只列常见的那几个，其余给十六进制。
    const char* ngxResultText(NVSDK_NGX_Result r)
    {
        switch (r)
        {
        case NVSDK_NGX_Result_Success: return "success";
        case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "feature not supported";
        case NVSDK_NGX_Result_FAIL_PlatformError: return "platform error";
        case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "feature not found";
        case NVSDK_NGX_Result_FAIL_InvalidParameter: return "invalid parameter";
        case NVSDK_NGX_Result_FAIL_NotInitialized: return "not initialized";
        case NVSDK_NGX_Result_FAIL_Denied: return "denied";
        case NVSDK_NGX_Result_FAIL_OutOfDate: return "out of date";
        case NVSDK_NGX_Result_FAIL_NotImplemented: return "not implemented";
        default: return nullptr;
        }
    }

    std::string ngxResultString(NVSDK_NGX_Result r)
    {
        if (const char* t = ngxResultText(r))
        {
            return std::string(t) + " (" + hex32(r) + ")";
        }
        return hex32(r);
    }

    // FeatureSupported 是「不支持的原因」位域，0 才表示支持。
    std::string supportBitsText(NVSDK_NGX_Feature_Support_Result bits)
    {
        const unsigned v = static_cast<unsigned>(bits);
        if (v == 0)
        {
            return {};
        }
        std::string s;
        const auto add = [&s](const char* name) {
            if (!s.empty())
            {
                s += "|";
            }
            s += name;
        };
        if (v & NVSDK_NGX_FeatureSupportResult_CheckNotPresent) add("check-not-present");
        if (v & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) add("driver-too-old");
        if (v & NVSDK_NGX_FeatureSupportResult_AdapterUnsupported) add("adapter-unsupported");
        if (v & NVSDK_NGX_FeatureSupportResult_OSVersionBelowMinimumSupported) add("os-too-old");
        if (v & NVSDK_NGX_FeatureSupportResult_NotImplemented) add("not-implemented");
        if (s.empty())
        {
            s = "unknown-bits(" + hex32(v) + ")";
        }
        return s;
    }

    NVSDK_NGX_FeatureDiscoveryInfo makeDiscovery(NVSDK_NGX_Feature feature)
    {
        NVSDK_NGX_FeatureDiscoveryInfo info;
        // P108: NOT NVSDK_NGX_Version_API. The SDK header pins that to 0x0000015
        // (NGX_VERSION_DOT 1.5.0), but the shipped runtime is 310.9.1 and gate-checks the
        // API struct version: with 0x15 BOTH NVSDK_NGX_D3D12_Init_with_ProjectID and
        // NVSDK_NGX_D3D12_GetFeatureRequirements answer FAIL_InvalidParameter (0xBAD00005),
        // even with a correct adapter and a correct NVSDK_NGX_FeatureCommonInfo path list.
        // Measured outside the game (PowerShell P/Invoke, real IDXGIAdapter + D3D12 device):
        //   GetFeatureRequirements(adapter, FeatureInfo=path list, SDKVersion=0x16)
        //     -> 0x00000001 Success, FeatureSupported = 0x0 (supported), MinOSVersion "10.0.0"
        // The header documents these "Fields below were introduced in SDK version 0x0000013 /
        // 0x0000014" markers, so newer structs exist; 0x16 is the lowest version that works
        // with this runtime.
        info.SDKVersion = static_cast<NVSDK_NGX_Version>(0x0000018);
        info.FeatureID = feature;
        info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
        info.Identifier.v.ProjectDesc.ProjectId = MCDR_NGX_PROJECT_ID;
        info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
        info.Identifier.v.ProjectDesc.EngineVersion = MCDR_NGX_ENGINE_VERSION;
        info.ApplicationDataPath = MCDR_NGX_APPDATA;
        // P108: NGX looks for its feature DLLs in the APPLICATION folder by default, which in
        // a Minecraft dev run is the JDK's bin\. This path list (built by d3d12_dlss.cpp from
        // the vendor directory) is what makes the query below answer Success instead of
        // FAIL_InvalidParameter. NULL is still valid — it just means "not known yet".
        info.FeatureInfo = static_cast<const NVSDK_NGX_FeatureCommonInfo*>(mcdr_dlss_ngx_feature_info());
        return info;
    }

    // 查一个特性。返回 true 表示受支持；false 时 why 里带上原因。
    // 注意：不接收适配器 —— GetFeatureRequirements 要 IDXGIAdapter，而探测阶段
    // 设备可能还没建好，所以走的是「无适配器」重载（Adapter 传 nullptr 会失败），
    // 这里改为只查运行期 DLL 是否存在 + 该特性在 SDK 侧是否被实现。
    struct FeatureProbe
    {
        bool ok = false;
        std::string why;
    };
#endif
}  // namespace

namespace mcdr
{
    std::string RtxSupport::summary() const
    {
        std::string s = "ngx=";
        if (ngxRuntime)
        {
            s += "yes(";
            bool first = true;
            const auto add = [&](const char* n) {
                if (!first)
                {
                    s += ",";
                }
                first = false;
                s += n;
            };
            if (dlssSuperResolution) add("SR");
            if (dlssFrameGeneration) add("FG");
            if (dlssRayReconstruction) add("RR");
            if (first)
            {
                s += "runtime-only";
            }
            s += ")";
        }
        else
        {
            s += "no";
        }

        s += " nrd=";
        if (nrdLinked)
        {
            s += nrdVersion.empty() ? std::string("yes") : nrdVersion;
            s += "[";
            for (std::size_t i = 0; i < nrdDenoisers.size(); ++i)
            {
                if (i)
                {
                    s += ",";
                }
                s += nrdDenoisers[i];
            }
            s += "]";
        }
        else
        {
            s += "off";
        }

        // ---- XeSS ----
        s += " xess=";
        if (xessRuntime)
        {
            // 摘要里用文件版本（2.0.2.68）而不是 xessGetVersion 的四段里只有三段
            // 的 2.0.2：前者与 FidelityFX 段的写法一致，且能分辨 libxess.dll 的
            // 具体构建。两个数都在 jsonFragment 里，摘要只留一个。
            if (!xessFileVersion.empty())
            {
                s += xessFileVersion;
            }
            else
            {
                s += xessVersion.empty() ? std::string("yes") : xessVersion;
            }
            // 方括号里列的是**运行时文件齐不齐**，不是硬件支不支持。
            std::string caps;
            const auto addCap = [&caps](const char* n) {
                if (!caps.empty())
                {
                    caps += ",";
                }
                caps += n;
            };
            if (xessFrameGenPresent) addCap("fg");
            if (xessLowLatencyPresent) addCap("ll");
            if (!caps.empty())
            {
                s += "[" + caps + "]";
            }
        }
        else
        {
            s += "no";
        }

        // ---- FidelityFX ----
        s += " ffx=";
        if (ffxLoaderLoaded)
        {
            s += ffxLoaderVersion.empty() ? std::string("yes") : ffxLoaderVersion;
            std::string caps;
            const auto addCap = [&caps](const char* n) {
                if (!caps.empty())
                {
                    caps += ",";
                }
                caps += n;
            };
            if (ffxUpscalerPresent) addCap("sr");
            if (ffxFrameGenPresent) addCap("fg");
            if (ffxDenoiserPresent) addCap("dn");
            if (ffxRadianceCachePresent) addCap("rc");
            if (!caps.empty())
            {
                s += "[" + caps + "]";
            }
        }
        else
        {
            s += "no";
        }
        return s;
    }

    // nlohmann 对象 -> 「字段串」：`{"a":1,"b":2}` → `"a":1,"b":2`。
    // 空对象返回空串，调用方据此跳过逗号。
    static std::string jsonFieldsOf(const nlohmann::json& obj)
    {
        std::string text = obj.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        if (text.size() < 2 || text.front() != '{' || text.back() != '}')
        {
            return std::string();
        }
        text.erase(text.size() - 1);
        text.erase(0, 1);
        return text;
    }

    std::string RtxSupport::jsonFragment() const
    {
        nlohmann::json j;
        j["rtx_dlss_probed"] = dlssProbed;
        j["rtx_dlss_runtime"] = ngxRuntime;
        j["rtx_dlss_sr"] = dlssSuperResolution;
        j["rtx_dlss_fg"] = dlssFrameGeneration;
        j["rtx_dlss_rr"] = dlssRayReconstruction;
        j["rtx_dlss_runtime_path"] = ngxRuntimePath;
        j["rtx_dlss_note"] = ngxNote;
        j["rtx_dlss_sr_why"] = dlssSuperResolutionWhy;
        j["rtx_dlss_fg_why"] = dlssFrameGenerationWhy;
        j["rtx_dlss_rr_why"] = dlssRayReconstructionWhy;

        j["rtx_nrd_linked"] = nrdLinked;
        j["rtx_nrd_version"] = nrdVersion;
        j["rtx_nrd_version_date"] = nrdVersionDate;
        j["rtx_nrd_denoisers"] = nrdDenoisers;

        // ---- XeSS ----
        j["rtx_xess_probed"] = xessProbed;
        j["rtx_xess_runtime"] = xessRuntime;
        j["rtx_xess_version"] = xessVersion;
        j["rtx_xess_file_version"] = xessFileVersion;
        j["rtx_xess_runtime_path"] = xessRuntimePath;
        j["rtx_xess_note"] = xessNote;
        // 注意键名是 present：只代表运行时文件在，不代表硬件支持。
        j["rtx_xess_fg_present"] = xessFrameGenPresent;
        j["rtx_xess_low_latency_present"] = xessLowLatencyPresent;

        // ---- FidelityFX ----
        j["rtx_ffx_probed"] = ffxProbed;
        j["rtx_ffx_loader"] = ffxLoaderLoaded;
        j["rtx_ffx_loader_version"] = ffxLoaderVersion;
        j["rtx_ffx_loader_path"] = ffxLoaderPath;
        j["rtx_ffx_note"] = ffxNote;
        j["rtx_ffx_upscaler_present"] = ffxUpscalerPresent;
        j["rtx_ffx_upscaler_version"] = ffxUpscalerVersion;
        j["rtx_ffx_framegen_present"] = ffxFrameGenPresent;
        j["rtx_ffx_framegen_version"] = ffxFrameGenVersion;
        j["rtx_ffx_denoiser_present"] = ffxDenoiserPresent;
        j["rtx_ffx_denoiser_version"] = ffxDenoiserVersion;
        j["rtx_ffx_radiancecache_present"] = ffxRadianceCachePresent;
        j["rtx_ffx_radiancecache_version"] = ffxRadianceCacheVersion;
        return jsonFieldsOf(j);
    }

    namespace
    {
        RtxSupport& cache()
        {
            static RtxSupport s;
            return s;
        }

        void probeNrd(RtxSupport& s)
        {
#if defined(MCDR_HAS_NRD)
            s.nrdLinked = true;

            char v[48] = {};
            std::snprintf(v, sizeof(v), "%u.%u.%u", static_cast<unsigned>(NRD_VERSION_MAJOR),
                          static_cast<unsigned>(NRD_VERSION_MINOR),
                          static_cast<unsigned>(NRD_VERSION_BUILD));
            s.nrdVersion = v;
            s.nrdVersionDate = NRD_VERSION_DATE;

            // GetLibraryDesc() 不需要设备、不需要建实例，是 NRD 唯一一个「纯查询」
            // 入口（见 NRD.h 的注释分组）。它给出的 supportedDenoisers 就是这次
            // 链接进来的静态库里实际编进去的降噪器。
            if (const nrd::LibraryDesc* d = nrd::GetLibraryDesc(); d != nullptr)
            {
                s.nrdVersion = std::to_string(static_cast<unsigned>(d->versionMajor)) + "." +
                               std::to_string(static_cast<unsigned>(d->versionMinor)) + "." +
                               std::to_string(static_cast<unsigned>(d->versionBuild));
                for (uint32_t i = 0; i < d->supportedDenoisersNum; ++i)
                {
                    const char* name = nrd::GetDenoiserString(d->supportedDenoisers[i]);
                    if (name != nullptr && name[0] != '\0')
                    {
                        s.nrdDenoisers.emplace_back(name);
                    }
                }
            }
            if (s.nrdDenoisers.empty())
            {
                // 静态库在但清单是空的：只可能是 GetLibraryDesc 失败了。
                s.nrdVersion += " (desc unavailable)";
            }
#else
            s.nrdLinked = false;
            s.nrdVersion = "not built for this platform";
#endif
        }

        // adapter 只在 MCDR_HAS_DLSS 时被用到（非 x64 平台没有 NGX 导入库，
        // 整个特性查询分支会被编译掉）。不加 [[maybe_unused]] 会在 Win32 下
        // 触发 C4100，而本项目 /WX，告警即失败。
        void probeDlss(RtxSupport& s, [[maybe_unused]] IDXGIAdapter* adapter)
        {
            s.dlssProbed = true;
#if defined(MCDR_HAS_DLSS)
            // 第一步：运行期 DLL 在不在。NGX 的导入库只是入口，没有这个 DLL
            // 一切特性都不可能可用，所以先把它探清楚，后面的失败原因才不会被
            // 误读成「显卡不支持」。
            // P108：加载动作**搬到 d3d12_dlss.cpp** 了（mcdr_dlss_load_runtime）。原因是
            // 探测是普通 TU，拿不到设备模块的 dbgLog*，而「运行期没找到」正是最需要逐条
            // 取证的那类失败：现在会按 裸名(含已拓宽的搜索路径) / JVM 给的厂商目录 /
            // MCDR.dll 同目录 / cwd 依次尝试，每次失败都带自己的 GetLastError 打进 P108 行。
            HMODULE mod = reinterpret_cast<HMODULE>(mcdr_dlss_load_runtime());
            if (mod == nullptr)
            {
                // 兜底：实现侧四条候选全失败时（例如 install_log 还没接上），仍旧按
                // 普通搜索路径与 MCDR.dll 同目录各试一次，保持原语义：报「找不到」，
                // 绝不把「没找到 DLL」误报成「显卡不支持」。
                std::wstring dllName = MCDR_NGX_RUNTIME_DLL;
                mod = ::LoadLibraryW(dllName.c_str());
                if (mod == nullptr && !moduleDirectory().empty())
                {
                    const std::string dir = moduleDirectory();
                    std::wstring wdir(dir.begin(), dir.end());
                    mod = ::LoadLibraryW((wdir + dllName).c_str());
                }
            }
            if (mod == nullptr)
            {
                s.ngxRuntime = false;
                s.ngxNote = "nvngx_dlss.dll not found (GetLastError=" + hex32(::GetLastError()) + ")";
                s.dlssSuperResolutionWhy = "runtime missing";
                s.dlssFrameGenerationWhy = "runtime missing";
                s.dlssRayReconstructionWhy = "runtime missing";
                return;
            }
            s.ngxRuntime = true;
            // 路径优先用实现侧记下的那条（只有它知道是哪个候选命中的）；拿不到再回退到
            // GetModuleFileNameW。
            if (const char* recorded = mcdr_dlss_runtime_path(); recorded != nullptr && recorded[0] != '\0')
            {
                s.ngxRuntimePath = recorded;
            }
            else
            {
                wchar_t path[MAX_PATH * 2] = {};
                const DWORD n = ::GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path)));
                if (n > 0)
                {
                    const int need = ::WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(n),
                                                           nullptr, 0, nullptr, nullptr);
                    if (need > 0)
                    {
                        s.ngxRuntimePath.assign(static_cast<std::size_t>(need), '\0');
                        ::WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(n),
                                              s.ngxRuntimePath.data(), need, nullptr, nullptr);
                    }
                }
            }

            // P108：**必须在下面的特性查询之前**把厂商目录登记给 DLSS 实现。登记会同时
            // 建好 NVSDK_NGX_FeatureCommonInfo 的 PathListInfo —— NGX 默认只在"应用程序
            // 目录"（开发跑时是 JDK 的 bin\）里找特性 DLL，拿不到路径表就对 Init 与
            // GetFeatureRequirements 一律回 FAIL_InvalidParameter (0xBAD00005)。
            // 实测（PowerShell P/Invoke，真 IDXGIAdapter + D3D12 设备）：
            //   GetFeatureRequirements(adapter, FeatureInfo=厂商目录) -> 0x00000001 Success,
            //   FeatureSupported = 0x0（=支持）。
            // 之前这里在模块的 notifyDlssDeviceReady 里才登记，时机晚于查询，于是查询一直
            // 拿到 FeatureInfo=NULL，实机表现为 ngxRuntime=yes 却 dlssSupported=no。
            if (!s.ngxRuntimePath.empty())
            {
                std::wstring wdir;
                {
                    wchar_t dir[MAX_PATH * 2] = {};
                    const DWORD n = ::GetModuleFileNameW(mod, dir, static_cast<DWORD>(std::size(dir)));
                    if (n > 0 && n < std::size(dir))
                    {
                        std::wstring full(dir, n);
                        const std::wstring::size_type slash = full.find_last_of(L"\\/");
                        if (slash != std::wstring::npos)
                        {
                            wdir = full.substr(0, slash);
                        }
                    }
                }
                if (!wdir.empty())
                {
                    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wdir.c_str(),
                                                           static_cast<int>(wdir.size()), nullptr, 0,
                                                           nullptr, nullptr);
                    if (need > 0)
                    {
                        std::string utf8(static_cast<std::size_t>(need), '\0');
                        ::WideCharToMultiByte(CP_UTF8, 0, wdir.c_str(), static_cast<int>(wdir.size()),
                                              utf8.data(), need, nullptr, nullptr);
                        mcdr_dlss_register_vendor_dir(utf8.c_str(), need + 1);
                    }
                }
            }

            // 第二步：逐个特性问硬件/驱动。GetFeatureRequirements 明确不需要
            // 先 NVSDK_NGX_Init，这正是探测阶段要的性质。
            //
            // 它必须拿到真实 IDXGIAdapter 才能给出准确答案（见 nvsdk_ngx.h 的参数
            // 说明）。没有适配器时**不能**拿 nullptr 去问：那只会得到
            // FAIL_InvalidParameter，然后把「没适配器」显示成「显卡不支持」——
            // 对着一张 4090 报"不支持"比什么都不报更糟。所以这种情况只报
            // 「运行期在」，并把查询留到有适配器时。
            //
            // P108 实测补充：**即使 MCDR.dll 自己的适配器是对的，这一步仍会在
            // 「运行期 DLL 还没被加载」时返回 FAIL_InvalidParameter (0xBAD00005)** ——
            // 实机拿到 ngxRuntime=yes 而 dlssSupported=no，原因就是 NGX 那时还没初始化，
            // 它答不了这个问题。所以：
            //   * "查不着"（FAIL_InvalidParameter / FAIL_NotInitialized）绝不能被解读成
            //     "不支持"；这种情况下把三个支持位留空并写明原因，真正的门是 DLSS/NGX 的
            //     Init 是否成功（见 applyDlssToTexture → mcdr_dlss_can_run），以及
            //     queryDlss 在 Init 之后做的重探。
            if (adapter == nullptr)
            {
                s.dlssQueried = false;
                s.dlssQueryNote = "no DXGI adapter supplied; feature support not queried yet";
                return;
            }
            s.dlssQueried = true;
            // adapter 必须显式捕获：它是函数参数，lambda 无法隐式捕获它（C3493）。
            const auto ask = [&s, adapter](NVSDK_NGX_Feature feature, bool& outOk, std::string& outWhy) {
                const NVSDK_NGX_FeatureDiscoveryInfo info = makeDiscovery(feature);
                NVSDK_NGX_FeatureRequirement req = {};
                const NVSDK_NGX_Result r =
                    NVSDK_NGX_D3D12_GetFeatureRequirements(adapter, &info, &req);
                if (r != NVSDK_NGX_Result_Success)
                {
                    outOk = false;
                    outWhy = "query failed: " + ngxResultString(r);
                    return;
                }
                if (req.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported)
                {
                    outOk = true;
                    outWhy.clear();
                    return;
                }
                outOk = false;
                outWhy = supportBitsText(req.FeatureSupported);
                if (req.MinOSVersion[0] != '\0')
                {
                    outWhy += " (min os " + std::string(req.MinOSVersion) + ")";
                }
            };
            ask(NVSDK_NGX_Feature_SuperSampling, s.dlssSuperResolution, s.dlssSuperResolutionWhy);
            ask(NVSDK_NGX_Feature_FrameGeneration, s.dlssFrameGeneration,
                s.dlssFrameGenerationWhy);
            ask(NVSDK_NGX_Feature_RayReconstruction, s.dlssRayReconstruction,
                s.dlssRayReconstructionWhy);
            // P108：「查不着」不是「不支持」。把这一步的失败如实记下来，并让上层知道
            // 这些位不可信；DLSS 的真正判决交给 Init。
            if (s.dlssSuperResolutionWhy.rfind("query failed:", 0) == 0)
            {
                s.dlssQueried = false;
                s.dlssQueryNote = "feature requirement query was inconclusive (" +
                                  s.dlssSuperResolutionWhy + "); the real gate is the NGX init";
            }
#else
            s.ngxRuntime = false;
            s.ngxNote = "not built for this platform (x64 only)";
#endif
        }

        // -------------------------------------------------------------------
        // XeSS：加载 libxess.dll，用 xessGetVersion 拿真实版本号
        // -------------------------------------------------------------------
        void probeXess(RtxSupport& s)
        {
            s.xessProbed = true;
#if defined(MCDR_HAS_XESS)
            constexpr const wchar_t* kRuntime = L"libxess.dll";
            HMODULE mod = loadRuntimeDll(kRuntime);
            if (mod == nullptr)
            {
                s.xessRuntime = false;
                s.xessNote = "libxess.dll not found (GetLastError=" + hex32(::GetLastError()) + ")";
                return;
            }
            s.xessRuntime = true;
            s.xessRuntimePath = loadedModulePath(mod);

            // xessGetVersion 一定要解析成功才算「运行时可问」：DLL 在但入口缺失
            // 只说明装的是一份对不上的版本，那也必须报出来而不是当成可用。
            const auto getVersion = reinterpret_cast<mcdr_xessGetVersion_fn>(
                reinterpret_cast<void*>(::GetProcAddress(mod, "xessGetVersion")));
            if (getVersion == nullptr)
            {
                s.xessNote = "libxess.dll loaded but xessGetVersion is missing "
                             "(GetLastError=" + hex32(::GetLastError()) + ")";
                return;
            }
            MCDR_XessVersion v{};
            const int rc = getVersion(&v);
            if (rc != MCDR_XESS_RESULT_SUCCESS)
            {
                s.xessNote = "xessGetVersion failed (xess_result_t=" + std::to_string(rc) + ")";
                return;
            }
            char vb[64] = {};
            std::snprintf(vb, sizeof(vb), "%u.%u.%u", static_cast<unsigned>(v.major),
                          static_cast<unsigned>(v.minor), static_cast<unsigned>(v.patch));
            s.xessVersion = vb;
            // 同时取文件版本资源：包名（3.0.2）与运行期自报版本（2.0.2）对不上，
            // 两个数都报出来，看报告的人才能自己判断装的是哪一版。
            if (!s.xessRuntimePath.empty())
            {
                s.xessFileVersion =
                    fileVersionOf(std::wstring(s.xessRuntimePath.begin(), s.xessRuntimePath.end()));
            }

            // 帧生成与低延迟是**另外两个 DLL**。这里只确认它们能加载 —— 能加载
            // 不等于这台机器跑得动（那要 xessD3D12CreateContext + 实际设备），
            // 所以字段名里用的是 Present 而不是 Supported。
            if (HMODULE fg = loadRuntimeDll(L"libxess_fg.dll"); fg != nullptr)
            {
                s.xessFrameGenPresent = true;
            }
            if (HMODULE ll = loadRuntimeDll(L"libxell.dll"); ll != nullptr)
            {
                s.xessLowLatencyPresent = true;
            }
#else
            s.xessRuntime = false;
            s.xessNote = "not built for this platform (x64 only)";
#endif
        }

        // -------------------------------------------------------------------
        // FidelityFX：完全没有导入库，loader DLL 的 5 个入口全靠 GetProcAddress
        // -------------------------------------------------------------------
        void probeFidelityFx(RtxSupport& s)
        {
            s.ffxProbed = true;
#if defined(MCDR_HAS_FFX)
            // 每个效果一个独立 DLL，名字与 SDK 里 bin 目录下的完全一致。
            constexpr const wchar_t* kLoader = L"amd_fidelityfx_loader_dx12.dll";
            constexpr const wchar_t* kUpscaler = L"amd_fidelityfx_upscaler_dx12.dll";
            constexpr const wchar_t* kFrameGen = L"amd_fidelityfx_framegeneration_dx12.dll";
            constexpr const wchar_t* kDenoiser = L"amd_fidelityfx_denoiser_dx12.dll";
            constexpr const wchar_t* kRadiance = L"amd_fidelityfx_radiancecache_dx12.dll";

            if (const std::string dir = moduleDirectory(); !dir.empty())
            {
                const std::wstring wdir(dir.begin(), dir.end());
                // 版本号只读文件资源，不必加载：即使 loader 加载失败，也能报出
                // 「文件在，是 2.3.0.0」这种有用信息。
                const auto versionIfPresent = [&wdir](const wchar_t* name) {
                    const std::wstring full = wdir + name;
                    return ::GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES
                               ? std::string()
                               : fileVersionOf(full);
                };
                if (const std::string v = versionIfPresent(kUpscaler); !v.empty())
                {
                    s.ffxUpscalerPresent = true;
                    s.ffxUpscalerVersion = v;
                }
                if (const std::string v = versionIfPresent(kFrameGen); !v.empty())
                {
                    s.ffxFrameGenPresent = true;
                    s.ffxFrameGenVersion = v;
                }
                if (const std::string v = versionIfPresent(kDenoiser); !v.empty())
                {
                    s.ffxDenoiserPresent = true;
                    s.ffxDenoiserVersion = v;
                }
                if (const std::string v = versionIfPresent(kRadiance); !v.empty())
                {
                    s.ffxRadianceCachePresent = true;
                    s.ffxRadianceCacheVersion = v;
                }
            }

            HMODULE mod = loadRuntimeDll(kLoader);
            if (mod == nullptr)
            {
                s.ffxLoaderLoaded = false;
                s.ffxNote = "amd_fidelityfx_loader_dx12.dll not found (GetLastError=" +
                            hex32(::GetLastError()) + ")";
                return;
            }
            s.ffxLoaderPath = loadedModulePath(mod);
            s.ffxLoaderVersion = fileVersionOf(std::wstring(s.ffxLoaderPath.begin(),
                                                            s.ffxLoaderPath.end()));

            // 5 个入口必须**全部**解析到，少一个都说明这份 loader 与本 SDK 头
            // 不匹配；只报「加载成功」会掩盖这种错配。
            const mcdr_ffxCreateContext_fn createContext =
                reinterpret_cast<mcdr_ffxCreateContext_fn>(
                    reinterpret_cast<void*>(::GetProcAddress(mod, "ffxCreateContext")));
            const mcdr_ffxDestroyContext_fn destroyContext =
                reinterpret_cast<mcdr_ffxDestroyContext_fn>(
                    reinterpret_cast<void*>(::GetProcAddress(mod, "ffxDestroyContext")));
            const mcdr_ffxConfigure_fn configure = reinterpret_cast<mcdr_ffxConfigure_fn>(
                reinterpret_cast<void*>(::GetProcAddress(mod, "ffxConfigure")));
            const mcdr_ffxQuery_fn query = reinterpret_cast<mcdr_ffxQuery_fn>(
                reinterpret_cast<void*>(::GetProcAddress(mod, "ffxQuery")));
            const mcdr_ffxDispatch_fn dispatch = reinterpret_cast<mcdr_ffxDispatch_fn>(
                reinterpret_cast<void*>(::GetProcAddress(mod, "ffxDispatch")));
            if (createContext == nullptr || destroyContext == nullptr || configure == nullptr ||
                query == nullptr || dispatch == nullptr)
            {
                s.ffxLoaderLoaded = false;
                s.ffxNote = "loader loaded but not all of the 5 ffx entry points resolved";
                return;
            }
            s.ffxLoaderLoaded = true;
#else
            s.ffxLoaderLoaded = false;
            s.ffxNote = "not built for this platform (x64 only)";
#endif
        }
    }  // namespace

    const RtxSupport& probeRtxSupport(IDXGIAdapter* adapter)
    {
        RtxSupport& s = cache();
        // 「探测过没有」记在缓存自身（dlssProbed / nrdLinked），不用函数局部
        // static —— 那样 resetRtxSupportCache 清不掉，第二次调用就再也探测不了。
        if (!s.dlssProbed)
        {
            probeDlss(s, adapter);
        }
        else if (adapter != nullptr && !s.dlssQueried)
        {
            // 首次调用没带适配器（例如设备还没建好就查了能力），这次补上特性查询。
            // 只重做 DLSS 那一步，NRD 的结果不动。
            s.dlssQueried = false;
            s.dlssQueryNote.clear();
            probeDlss(s, adapter);
        }
        if (s.nrdVersion.empty())
        {
            probeNrd(s);
        }
        // XeSS / FidelityFX 都不需要适配器，所以和 NRD 一样：只在没测过时测一次，
        // 不会因为 first call 没带适配器而需要重做。
        if (!s.xessProbed)
        {
            probeXess(s);
        }
        if (!s.ffxProbed)
        {
            probeFidelityFx(s);
        }
        return s;
    }

    void resetRtxSupportCache()
    {
        cache() = RtxSupport{};
    }
}  // namespace mcdr