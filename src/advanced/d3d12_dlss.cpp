// d3d12_dlss.cpp — P108: DLSS super-resolution over NVSDK_NGX (data plane + the real
// Evaluate call).
//
// WHY THIS IS A PLAIN TRANSLATION UNIT (not a module implementation unit):
//   Same reason as d3d12_rtx_probe.cpp — it hides every vendor type (ID3D12Resource,
//   NVSDK_NGX_*) behind a tiny C interface, so the device module (mcdr.d3d12.device)
//   never includes an NGX header and never inherits the DLSS license surface. The
//   dependency runs one way only: the module declares these entry points itself and
//   calls them; nothing here imports a mcdr.* module.
//
// WHERE IT SITS IN THE FRAME:
//   The Java side calls mcdr_dx12EvaluateDlss at the SAME point where it used to call
//   mcdr_dx12ApplySmaaToTexture — after the world has rendered, before the GUI. DLSS
//   upscales the render-scaled world layer (window * dxrMcRenderScale) into the
//   window-sized target the GUI draws over, which is exactly what the linear blit did.
//   DLSS and SMAA are two values of the same anti-aliasing option, never both.
//
// FAILURE POLICY (mirrors P104b SMAA exactly):
//   Every failure path logs at most ONE line (WARN for "not available", ERROR for
//   "broke while running") and then pins the state, so nothing is retried per frame.
//   DLSS never makes the frame fail: the caller falls back to the plain blit path.
//
// DEPTH CONVENTION (see the P108 report for the full evidence):
//   Minecraft 26.x renders REVERSE-Z. Projection.java:70-74 swaps near/far before
//   building the matrix; DepthStencilState.DEFAULT is GREATER_THAN_OR_EQUAL (mapped by
//   d3d12_device.cpp:3676 to D3D12_COMPARISON_FUNC_GREATER_EQUAL); the depth buffer is
//   cleared to 0.0 == far (GameRenderer.java:443/578, d3d12_device.cpp:3492-3498).
//   DLSS therefore gets NVSDK_NGX_DLSS_Feature_Flags_DepthInverted.
//
// MOTION VECTORS: D3D12Velocity.kt writes screen-space motion in PIXELS
// ((uv - prevUv) * ScreenSize), so InMVScaleX/Y stay 1.0. They are computed from the
// JITTERED current projection, so the jitter is inside them: DLSS is told both that the
// MVs are jittered (MVJittered) and to cancel that jitter
// (motionVectorJitterCancellation), which is the self-consistent combination.
#include <windows.h>

#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// P112 阶段 1：Streamline 接管 DLSS 的实现选择（默认仍是直连 NGX，行为与 P108 一致）。
#if __has_include(<sl.h>)
#include <sl.h>
#include <sl_dlss.h>   // sl::DLSSMode / sl::DLSSOptions（sl.h 只含 core）
#include <sl_reflex.h>  // P113：sl::ReflexOptions / sl::ReflexMode
#define MCDR_DLSS_HAVE_STREAMLINE 1
#else
#define MCDR_DLSS_HAVE_STREAMLINE 0
#endif

#include "mcdr_abi.h"
// P108: the extern "C" interface shared with the device module. It defines the transition
// callback types (DlssTransitionFn / DlssQueryStateFn) this unit stores in its state.
#include "mcdr_dlss_impl.h"

import std;

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

// ---------------------------------------------------------------------------
// Project constants (mirrors d3d12_rtx_probe.cpp so the SDK sees one application
// identity from both call sites).
//
// P108: the ProjectId is a **36-character GUID string**, not a short name. Rationale: the
// production integration that ships DLSS against this same 310.x runtime (ETS2/ATS DLAA
// mod, `src/dlaa.cpp`) uses exactly this shape — a fixed GUID with the comment "any stable
// GUID works for unregistered apps; ours, generated for this project". Length/charset is
// not documented as a hard requirement (UE-style integrations use 32 hex chars without
// dashes), so the invariant that matters is: **stable, non-empty, and GUID-shaped**.
// Generated once for DXRMC and pinned here; changing it invalidates NGX's per-app state.
// ---------------------------------------------------------------------------
#define MCDR_NGX_PROJECT_ID "7c1f4a52-9b3e-4d68-8f21-5a6c0e2d9b74"
#define MCDR_NGX_ENGINE_VERSION "1.0"
#define MCDR_NGX_APPDATA L".\\ngx"

// ---------------------------------------------------------------------------
// P108: NGX runtime discovery + diagnostics.
//
// The RTX probe used to do this itself (see d3d12_rtx_probe.cpp), but a plain TU cannot
// reach the device module's dbgLog*, and "the runtime could not be found" is exactly the
// failure that needs a per-attempt log line. So the loading now lives HERE, where the
// module's log pointers are installed, and the probe calls mcdr_dlss_load_runtime().
//
// What NGX actually needs (verified against external\dlss\include\nvsdk_ngx_loader.h and
// the shipped binaries):
//   * `nvngx_dlss.dll` — the feature runtime shipped in external\dlss\bin\x64. This is
//     the one file the loader hands to LoadLibraryW; a missing *dependency* is not
//     possible, because nvngx_dlss.dll imports only VERSION/ADVAPI32/USER32/KERNEL32
//     (checked with a PE import parse) and has no delay-load table at all.
//   * `nvngx.dll` / `_nvngx.dll` — the NGX CORE, which is NOT shipped by the SDK. NGX
//     finds it next to the *application executable* (GetModuleFileNameW(NULL)), or via
//     the driver's registry key. In a Minecraft dev run the executable is the JDK's
//     java.exe, so that lookup cannot succeed and NGX falls back to the registry; this is
//     why `nvngx_dlss.dll` being present is necessary but not sufficient.
//   * `applicationDataPath` (the 4th parameter of Init_with_ProjectID) is a directory for
//     NGX's own logs/cache, NOT a model search path. There is no `pathToNGXModel`
//     parameter in this SDK version.
// ---------------------------------------------------------------------------
namespace
{
    // -----------------------------------------------------------------------
    // Logging. The device module owns the log (level filtering + the line format that
    // mirrors the JVM's log4j pattern), so this unit never formats a prefix or a
    // timestamp of its own: the module hands over its own dbgLogInfo/dbgLogWarn/
    // dbgLogError pointers in mcdr_dlss_install_log (see the bottom of this file).
    // Resolving those names on the module's side is what makes the boundary legal —
    // this TU must never name a mcdr.* module entity itself.
    // -----------------------------------------------------------------------
    using LogFn = void (*)(const char* fmt, ...);
    LogFn gLogInfo = nullptr;
    LogFn gLogWarn = nullptr;
    LogFn gLogError = nullptr;

    // B4：模块提供的私有纹理创建/销毁（见 mcdr_dlss_impl.h 的 DlssTextureDesc）。本单元从此
    // **不再自己 CreateCommittedResource 任何纹理** —— 私有 UAV 与深度快照都从这里建，创建
    // 即登记进模块的跟踪表，销毁回模块的延迟删除路径。两条路留一条，别再走回去。
    using CreateTextureFn = int32_t (*)(void*, const DlssTextureDesc*, void**, void**);
    using DestroyTextureFn = void (*)(void*, void*);
    CreateTextureFn gCreateTexture = nullptr;
    DestroyTextureFn gDestroyTexture = nullptr;

    void logInfo(const char* fmt, ...) {
        if (gLogInfo == nullptr) {
            return;
        }
        char line[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        gLogInfo("%s", line);
    }
    void logWarn(const char* fmt, ...) {
        if (gLogWarn == nullptr) {
            return;
        }
        char line[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        gLogWarn("%s", line);
    }
    void logError(const char* fmt, ...) {
        if (gLogError == nullptr) {
            return;
        }
        char line[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        gLogError("%s", line);
    }

    // -----------------------------------------------------------------------
    // NGX result codes -> readable text.
    // -----------------------------------------------------------------------
    const char* ngxResultText(NVSDK_NGX_Result r) {
        switch (r) {
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

    // "text (0xXXXXXXXX)" when the code is a known one, plain hex otherwise.
    std::string ngxResultString(NVSDK_NGX_Result r) {
        char hex[32] = {};
        std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(r));
        if (const char* t = ngxResultText(r)) {
            return std::string(t) + " (" + hex + ")";
        }
        return std::string(hex);
    }
    // -----------------------------------------------------------------------
    // State. One process-wide DLSS instance (the device is a singleton too).
    // -----------------------------------------------------------------------
    struct State
    {
        bool initAttempted = false;   // NGX init was tried (success or not)
        bool initOk = false;          // NVSDK_NGX_D3D12_Init succeeded
        bool paramsOk = false;        // GetCapabilityParameters succeeded
        bool optimalOk = false;       // the optimal-settings query produced dimensions
        bool disabled = false;        // a failure already logged: never retry
        bool featureOk = false;       // a live NVSDK_NGX_Handle exists
        bool createdThisFrame = false;
        bool createdWarned = false;   // a create failure was already reported
        bool evalOk = false;
        bool evalFailWarned = false;  // an evaluate failure was already reported
        bool resetPending = false;    // history must be dropped on the next evaluate
        // P108: one-shot log so a failing run states where the evaluate wrote.
        bool evalDestinationLogged = false;
        // P108 (user m10920): DLSS is now an INDEPENDENT switch, not an anti-aliasing mode.
        // The authoritative gate lives in the device module (gCtx.dlssEnabled); this mirror
        // only exists so the status query and the log lines can report it.
        bool enabled = false;
        uint64_t submitCount = 0;     // successful Evaluate calls
        ID3D12Device* device = nullptr;
        NVSDK_NGX_Parameter* params = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;
        ID3D12Resource* depthStencil = nullptr;  // DLSS scratch buffer, DEFAULT heap
        // P108: DLSS must write into a resource created with ALLOW_UNORDERED_ACCESS. The
        // JVM's GUI/present target is created with ALLOW_RENDER_TARGET only (that flag is set
        // from usage bit 8 in d3d12_device.cpp and there is no UAV bit at all), and NGX
        // rejects it with `FAIL_RWFlagMissing` (0xBAD00009). Adding a UAV bit to the present
        // target would drag the whole present/blit/barrier path in, so instead DLSS writes
        // into this private, target-sized UAV texture and the result is copied into the real
        // output on the same command list. Self-contained: no JVM change needed.
        ID3D12Resource* dlssOutput = nullptr;    // private UAV intermediate
        // B4：这张纹理现在由**设备模块**创建（见 mcdr_dlss_impl.h 的 DlssTextureDesc）：本单元
        // 只持有一个不透明句柄 + 裸资源指针，销毁必须交回模块（destroyTrackedTexture），
        // **绝不**自己 Release —— 否则就又是一条绕过模块、绕过跟踪表的资源路径。
        void* dlssOutputObject = nullptr;
        // P108 (user m11147 follow-up): the UAV texture the JVM created through the module's
        // `createTexture` path. Preferred over `dlssOutput` for the reasons recorded at the
        // `ngxTarget` selection in mcdr_dlss_evaluate: this unit's own creation path produces a
        // resource the GPU does not write to. Empty (`nullptr`) means "fall back to private".
        // NOT owned by this unit — the module keeps the Dx12Object alive.
        ID3D12Resource* externalOutput = nullptr;
        UINT dlssOutputWidth = 0;
        UINT dlssOutputHeight = 0;
        DXGI_FORMAT dlssOutputFormat = DXGI_FORMAT_UNKNOWN;
        // P108x: one-shot diagnostic state (temporary — used to localise the beige-frame
        // report; the numbers localise, they never claim how the frame looks).
        bool diagDone = false;
        // B3：7 项 settled 读数已经请求过的帧数。模块的读回**延迟一帧**（见 dlssReadbackThunk），
        // 所以只请求一次只能得到 "(not ready)"；evaluate 里按 kP108xDumpRounds 跑有限几轮。
        int dumpRounds = 0;
        ID3D12Fence* diagFence = nullptr;

        // P108x: frame counter + the last private-output pointer, so a reading taken at the
        // start of frame N can say whether it is looking at frame N-1's settled output.
        uint64_t frameIndex = 0;
        ID3D12Resource* prevPrivateOutput = nullptr;
        uint64_t prevPrivateFrame = 0;
        UINT64 diagFenceValue = 0;
        bool diagClosedList = false;
        bool preEvalFillDone = false;  // P108x: one-shot write-side bisect (see the evaluate body)
        bool controlFillDone = false;  // P108x: CONTROL (module-created destination)
        // P108: tracked transitions are delegated to the device module (see transition()).
        DlssTransitionFn transitionFn = nullptr;
        // B2：模块读回回调（见 mcdr_dlss_impl.h 的 DlssReadbackFn 说明）。本轮只保存，
        // dump 路径仍是旧的 dumpManyResources；B3 才把 7 项读数切到它上面。
        DlssReadbackFn readbackFn = nullptr;
        void* transitionCtx = nullptr;
        // P108: the private D32 depth snapshot (see mcdr_dlss_snapshot_depth). When present it
        // replaces the JVM's `pInDepth`, which by the time DLSS runs has already been cleared.
        ID3D12Resource* depthSnapshot = nullptr;
        // B4：同上 —— 深度快照也由设备模块创建，这里只留句柄。
        void* depthSnapshotObject = nullptr;
        UINT depthSnapshotWidth = 0;
        UINT depthSnapshotHeight = 0;
        DXGI_FORMAT depthSnapshotFormat = DXGI_FORMAT_UNKNOWN;
        UINT depthSnapshotMips = 0;      // P108x: a full copy requires the whole shape to agree
        UINT depthSnapshotArray = 0;
        UINT depthSnapshotSamples = 0;
        bool depthSnapshotShapeLogged = false;
        UINT64 depthSnapshotFrame = 0;   // frame counter at the last snapshot
        bool depthSnapshotWarned = false;
        bool depthSnapshotUsedLogged = false;
        bool depthInputLogged = false;  // P108x: one-shot "which resource/format NGX really gets"

        ID3D12Resource* mvBuffer = nullptr;      // intermediate for a non-RG16 MV input
        // B4：MV 中间纹理也由设备模块创建（同一入口），这里只留句柄。
        void* mvBufferObject = nullptr;
        void* runtimeModule = nullptr;           // HMODULE of nvngx_dlss.dll once loaded
        UINT64 scratchBytes = 0;
        UINT32 mvCopyResourceState = D3D12_RESOURCE_STATE_COMMON;
        // The size tuple the current feature was created for. A change here forces a
        // destroy + recreate on the next Evaluate (and a history reset).
        int32_t renderWidth = 0;
        int32_t renderHeight = 0;
        int32_t targetWidth = 0;
        int32_t targetHeight = 0;
        uint32_t qualityValue = 0;
        // P108 (user m11147): the DLSS tier is now an explicit user choice (Performance /
        // Balanced / Quality) instead of only "closest to the render:target ratio".
        //   requestedQuality < 0  -> the JVM never set one: fall back to chooseQuality()
        //   requestedQuality >= 0 -> use it verbatim, and rebuild the feature when it changes
        // `effectiveQuality` is the ACCEPTED TIER (0..3, -1 = ratio-chosen) the live feature was
        // actually built with — after the DLAA 1:1 restriction turned tier 3 into Quality, it is
        // 2, not the requested 3. `qualityValue` is the NGX enum (DLAA == 5); never compare the
        // two directly (that mistake rebuilt the feature every frame).
        int requestedQuality = -1;
        int effectiveQuality = -1;
        // P108ac (user m08033): the DLSS *model* (NGX "render preset" hint). The JVM turns the
        // model button into one of NVSDK_NGX_DLSS_Hint_Render_Preset values (0 = Default,
        // 10 = J, 11 = K, 12 = L, 13 = M, ...) and it lands here.
        //   requestedPreset >= 0 -> set the hint for the tier being built; it is a CreateFeature
        //   input, so a change rebuilds the feature (same mechanism as the tier).
        // `effectivePreset` is the value the live feature was created with.
        int requestedPreset = 0;
        int effectivePreset = 0;
        bool presetRequestLogged = false;
        // P108aa (user m11796): true while a requested DLAA is being downgraded because
        // render != target. State-change gate for the one-shot ERROR (it used to print per frame).
        bool dlaaDowngraded = false;
        bool qualityIsExplicit = false;
        bool qualityRequestLogged = false;
        int32_t featureFlags = 0;
        // Strings for the status query and the one-time log lines.
        std::string failure;        // the stage that failed, for the status query
        std::string runtimePath;    // nvngx_dlss.dll as loaded (reported by the RTX probe)
        std::string optimalText;    // "target 1280x720, render 960x540 -> quality Balanced (...)"
        uint32_t optimalRenderW = 0;
        uint32_t optimalRenderH = 0;
        float optimalSharpness = 0.0f;
        // Per-frame input summary: log only when a value changes (no per-frame spam).
        int32_t lastRenderW = -1;
        int32_t lastRenderH = -1;
        int32_t lastTargetW = -1;
        int32_t lastTargetH = -1;
        uint32_t lastReset = 0xFFFFFFFFu;
        uint32_t lastFlags = 0xFFFFFFFFu;
        uint32_t lastExposureMode = 0xFFFFFFFFu;
        int32_t lastDepthFormat = 0x7FFFFFFF;
        float lastJitterX = 1e9f;
        float lastJitterY = 1e9f;
        float lastNear = 1e9f;
        float lastFar = 1e9f;
        // P108 blocker-B: the vendor directory the JVM announced (the same `<user.dir>
        // \dxrmc\` MCDR.dll is extracted to), whether the process DLL search path was
        // widened for it, and whether the three descriptive lines were printed yet.
        std::wstring vendorDir;
        bool searchPathSet = false;
        bool envLogged = false;
    };

    State s;

    // -----------------------------------------------------------------------
    // Runtime discovery (see the file-header note). Everything here logs through the
    // module's own dbgLog*, so a failure reads like any other native line.
    // -----------------------------------------------------------------------
    // The directory THIS module (MCDR.dll) was loaded from.
    //
    // NOT GetModuleFileNameW(nullptr): passing NULL returns the **executable**, which in a
    // Minecraft dev run is the JDK's java.exe, so every exe-relative guess landed in
    // `C:\Program Files\GraalVM\JDKs\25.1\bin`. That was the actual cause of the whole
    // `ngx=no` / ERROR_MOD_NOT_FOUND investigation: the vendor DLLs sit next to MCDR.dll in
    // `<user.dir>\dxrmc\`, and no candidate ever looked there.
    std::wstring moduleDirectoryW() {
        HMODULE self = nullptr;
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&moduleDirectoryW), &self) == FALSE ||
            self == nullptr) {
            return {};
        }
        wchar_t path[MAX_PATH * 2] = {};
        const DWORD n = ::GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
        if (n == 0 || n >= std::size(path)) {
            return {};
        }
        std::wstring w(path, n);
        const std::wstring::size_type slash = w.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            return {};
        }
        return w.substr(0, slash + 1);
    }

    // The directory the *executable* lives in (java.exe's bin during a dev run). Kept
    // separate from moduleDirectoryW so the diagnostics can show both.
    std::wstring executableDirectoryW() {
        wchar_t path[MAX_PATH * 2] = {};
        const DWORD n = ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (n == 0 || n >= std::size(path)) {
            return {};
        }
        std::wstring w(path, n);
        const std::wstring::size_type slash = w.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            return {};
        }
        return w.substr(0, slash + 1);
    }

    std::wstring currentDirectoryW() {
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD n = ::GetCurrentDirectoryW(static_cast<DWORD>(std::size(buf)), buf);
        if (n == 0 || n >= std::size(buf)) {
            return {};
        }
        return std::wstring(buf, n);
    }

    std::string narrow(const std::wstring& w) {
        if (w.empty()) {
            return {};
        }
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (need <= 0) {
            return {};
        }
        std::string out(static_cast<std::size_t>(need), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                              nullptr, nullptr);
        return out;
    }

    // Adds `dir` to the process DLL search path. SetDllDirectoryW alone would be enough
    // (it inserts one directory into the standard search order), but AddDllDirectory needs
    // the default-dirs mode to take effect, so both are issued and each result is logged.
    void widenSearchPath(const std::wstring& dir) {
        if (dir.empty()) {
            logWarn("P108: DLSS vendor directory is empty; the DLL search path is left untouched");
            return;
        }
        const BOOL setDefault = ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                                          LOAD_LIBRARY_SEARCH_USER_DIRS);
        const DWORD setDefaultErr = setDefault ? 0u : ::GetLastError();
        const BOOL setDir = ::SetDllDirectoryW(dir.c_str());
        const DWORD setDirErr = setDir ? 0u : ::GetLastError();
        const DLL_DIRECTORY_COOKIE cookie = ::AddDllDirectory(dir.c_str());
        const DWORD addErr = (cookie != nullptr) ? 0u : ::GetLastError();
        s.searchPathSet = true;
        logInfo("P108: DLSS search path widened for \"%s\": SetDefaultDllDirectories=%s(err=%lu) "
                "SetDllDirectoryW=%s(err=%lu) AddDllDirectory=%s(err=%lu)",
                narrow(dir).c_str(), setDefault ? "ok" : "failed", setDefaultErr,
                setDir ? "ok" : "failed", setDirErr, (cookie != nullptr) ? "ok" : "failed", addErr);
    }

    // One line that answers "where was it looking?" for the next failure.
    void logEnvOnce() {
        if (s.envLogged) {
            return;
        }
        s.envLogged = true;
        logInfo("P108: DLSS load environment: moduleDir=\"%s\" cwd=\"%s\" exeDir=\"%s\" vendorDir=\"%s\" "
                "searchPathSet=%d",
                narrow(moduleDirectoryW()).c_str(), narrow(currentDirectoryW()).c_str(),
                narrow(executableDirectoryW()).c_str(), narrow(s.vendorDir).c_str(),
                s.searchPathSet ? 1 : 0);
    }

    // Tries every plausible location, in order, logging each attempt with its own
    // GetLastError. Returns the module handle, or NULL after logging the whole chain.
    HMODULE loadRuntimeDll() {
        std::wstring name = L"nvngx_dlss.dll";
        const std::wstring modDir = moduleDirectoryW();
        const std::wstring cwd = currentDirectoryW();
        const std::wstring exeDir = executableDirectoryW();
        struct Candidate
        {
            const wchar_t* what;
            std::wstring path;
            bool useEx;
            DWORD flags;
        };
        std::vector<Candidate> candidates;
        // 1) bare name: the standard search order, with a widened path this is where the
        //    vendor directory is found.
        candidates.push_back({s.searchPathSet ? L"search-path(name)" : L"bare-name", name, false, 0});
        // 2) the directory the JVM announced (== where MCDR.dll itself was extracted).
        if (!s.vendorDir.empty()) {
            candidates.push_back({L"vendor-dir", s.vendorDir + L"\\" + name, false, 0});
        }
        // 3) MCDR.dll's own directory. This is the candidate that actually resolves the
        //    shipped layout: JVM extractLibrary() writes MCDR.dll AND the vendor DLLs into
        //    `<user.dir>\dxrmc\`, so the runtime always sits next to this module.
        if (!modDir.empty()) {
            candidates.push_back({L"module-dir", modDir + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }
        // 4) `<cwd>\dxrmc\` — the extracted vendor directory under the game directory, for
        //    the case where the module was loaded from somewhere else.
        if (!cwd.empty()) {
            candidates.push_back({L"cwd-dxrmc", cwd + L"\\dxrmc\\" + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }
        // 5) `<exeDir>\dxrmc\` — same idea for a non-dev deployment.
        if (!exeDir.empty()) {
            candidates.push_back({L"exeDir-dxrmc", exeDir + L"dxrmc\\" + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }
        // 6) cwd, likewise explicit.
        if (!cwd.empty()) {
            candidates.push_back({L"cwd", cwd + L"\\" + name, true,
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR});
        }

        HMODULE found = nullptr;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const Candidate& c = candidates[i];
            ::SetLastError(ERROR_SUCCESS);
            HMODULE mod = c.useEx ? ::LoadLibraryExW(c.path.c_str(), nullptr, c.flags)
                                  : ::LoadLibraryW(c.path.c_str());
            const DWORD err = (mod != nullptr) ? 0u : ::GetLastError();
            if (mod != nullptr) {
                found = mod;
                const std::wstring full = c.useEx ? c.path : std::wstring();
                if (!full.empty()) {
                    s.runtimePath = narrow(full);
                } else {
                    wchar_t p[MAX_PATH * 2] = {};
                    const DWORD n = ::GetModuleFileNameW(mod, p, static_cast<DWORD>(std::size(p)));
                    s.runtimePath = narrow(std::wstring(p, n));
                }
                logInfo("P108: nvngx runtime loaded via %s (attempt %llu) -> \"%s\"", c.what,
                        static_cast<unsigned long long>(i + 1), s.runtimePath.c_str());
                break;
            }
            logWarn("P108: nvngx runtime attempt %llu/%llu (%s) failed: \"%s\" -> err=%lu (%s)",
                    static_cast<unsigned long long>(i + 1),
                    static_cast<unsigned long long>(candidates.size()), c.what,
                    narrow(c.useEx ? c.path : std::wstring(name)).c_str(), err,
                    err == ERROR_MOD_NOT_FOUND ? "ERROR_MOD_NOT_FOUND"
                                               : (err == ERROR_FILE_NOT_FOUND ? "ERROR_FILE_NOT_FOUND"
                                                                              : "see winerror.h"));
        }
        if (found == nullptr) {
            logWarn("P108: nvngx_dlss.dll could not be loaded from any candidate; DLSS will report "
                    "itself unavailable. Note: nvngx_dlss.dll imports only system DLLs, so "
                    "ERROR_MOD_NOT_FOUND here points at the path, not at a missing dependency.");
        }
        return found;
    }

    // -----------------------------------------------------------------------
    // P108 blocker-B ROOT CAUSE: NVSDK_NGX_FeatureCommonInfo.PathListInfo.
    //
    // `nvsdk_ngx_defs.h:400-409` documents PathListInfo as "List of all paths in descending
    // order of search sequence to locate a feature dll in, other than the default path -
    // application folder"; `:519-526` repeats it for FeatureDiscoveryInfo. NGX's default is
    // the APPLICATION folder, which in a Minecraft dev run is the JDK's `bin\`, never our
    // vendor directory. With an empty path list NGX cannot find nvngx_dlss.dll and answers
    // `FAIL_InvalidParameter` (0xBAD00005) to BOTH Init_with_ProjectID and
    // GetFeatureRequirements.
    //
    // Measured outside the game (PowerShell P/Invoke, real IDXGIAdapter + D3D12 device):
    //   GetFeatureRequirements(adapter, FeatureInfo = vendor path list)
    //       -> 0x00000001 Success, FeatureSupported = 0x0 (supported), MinOSVersion "10.0.0"
    //   In-game probe with FeatureInfo = NULL reported "query failed: invalid parameter
    //       (0xFFFFFFFFBAD00005)", and Init_with_ProjectID likewise returned 0xBAD00005.
    // (Init itself is NOT reachable from PowerShell: nvngx_dlss.dll exports only
    // NVSDK_NGX_CUDA_* and NGX_SNIPPETS_*, so the D3D12 entry points must be exercised
    // in-game.)
    //
    // The buffers are owned here and live for the process lifetime because NGX keeps the
    // pointers (PathListInfo.Path is `wchar_t const* const*`).
    // -----------------------------------------------------------------------
    NVSDK_NGX_FeatureCommonInfo* gCommonInfo = nullptr;
    std::wstring* gPathListEntry = nullptr;    // the one wide string the list points at
    const wchar_t** gPathListArray = nullptr;  // the one-element array of path pointers

    // (Re)builds the common info so PathListInfo holds `dir` (empty -> Length 0).
    void rebuildCommonInfo(const std::wstring& dir) {
        if (gPathListEntry == nullptr) {
            gPathListEntry = new std::wstring();
        }
        *gPathListEntry = dir;
        if (gPathListArray == nullptr) {
            gPathListArray = new const wchar_t*[1];
        }
        gPathListArray[0] = gPathListEntry->c_str();
        if (gCommonInfo == nullptr) {
            gCommonInfo = new NVSDK_NGX_FeatureCommonInfo();
        }
        *gCommonInfo = NVSDK_NGX_FeatureCommonInfo{};
        gCommonInfo->PathListInfo.Path = gPathListArray;
        gCommonInfo->PathListInfo.Length = dir.empty() ? 0u : 1u;
        gCommonInfo->InternalData = nullptr;
        gCommonInfo->LoggingInfo = NVSDK_NGX_LoggingInfo{};
    }

    // The pointer handed to NGX and to the RTX probe's requirement query. NULL only while the
    // path list is still empty (then the probe keeps its old "inconclusive" behaviour).
    NVSDK_NGX_FeatureCommonInfo* commonInfoForNgx() {
        if (gCommonInfo == nullptr || gPathListEntry == nullptr ||
            gCommonInfo->PathListInfo.Length == 0u) {
            return nullptr;
        }
        return gCommonInfo;
    }

    // Builds the DLSS feature flags from the frame description.
    //
    // Re-derived from a WORKING integration shipping against the same 310.x runtime (ETS2/ATS
    // DLAA/DLSS injection mod, `src/dlaa.cpp`). Two of the previous choices were wrong, and both are
    // consistent with "NGX writes nothing / the world comes out black":
    //
    //   AutoExposure  **always on**. Reference: "AutoExposure spares us an exposure texture; it is
    //                 ignored for LDR input anyway." We have no GPU exposure texture and used to pass
    //                 a constant 1.0 with this flag absent — so NGX had neither a texture nor the
    //                 flag that lets it synthesise one.
    //   DepthInverted on — reverse-Z (see the file header).
    //   MVLowRes      **on when render != target**. The bit means "the motion vectors are at the
    //                 (low) render resolution", which is exactly our case: the velocity pass runs at
    //                 render res and writes render-pixel offsets. The old code left it off and even
    //                 documented the opposite, so NGX rescaled them as if they were display-sized.
    //   MVJittered    **on**. D3D12Velocity.kt builds `currViewProj` from the JITTERED projection and
    //                 then adds `PrevJitterPixels`, so both ends of every motion vector sit on the
    //                 jittered grid — the jitter delta is inside them. Declaring that (MVJittered) is
    //                 what lets NGX cancel it with `InJitterOffsetX/Y`; leaving it off made NGX treat
    //                 those deltas as real motion. The old note ("jitter is frozen to 0 anyway") no
    //                 longer holds: P110 removed the freeze, TAA and DLSS now share one Halton(2,3)
    //                 sequence, and the flag is set unconditionally — with jitter 0 it is a no-op.
    //   IsHDR         off — 8-bit UNORM target.
    //
    // Cancelling that jitter needs no extra NGX knob: `MVJittered` plus the `InJitterOffsetX/Y`
    // already passed to `EvaluateFeature` is the whole contract. (This SDK's header has no
    // "motion vector jitter cancellation" parameter key at all — an earlier note here claimed one.)
    // P108ac (user m08033): the DLSS *model* is selected through NGX's "render preset" hint, one
    // parameter key per quality tier. Only the key matching the tier the feature is built with is
    // consulted, so set exactly that one — writing all six would be noise, and a stale value on an
    // unused key could silently apply later when the tier changes without us.
    const char* presetHintKey(uint32_t qualityValue) {
        switch (qualityValue) {
            case NVSDK_NGX_PerfQuality_Value_DLAA:
                return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
            case NVSDK_NGX_PerfQuality_Value_MaxQuality:
                return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
            case NVSDK_NGX_PerfQuality_Value_Balanced:
                return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
            case NVSDK_NGX_PerfQuality_Value_MaxPerf:
                return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
            case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
                return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
            // Ultra Quality exists in this SDK's enum but has no hint key of its own, so it falls
            // through to `default` (no preset written = NGX's own choice for that tier).
            case NVSDK_NGX_PerfQuality_Value_UltraQuality:
            default:
                return nullptr;
        }
    }

    // Human-readable form of a preset value for the log lines ("Default" / "J" / "K" / "L" / "M",
    // or the raw number for the letters NVIDIA marks as reserved).
    std::string presetName(int32_t preset) {
        switch (preset) {
            case NVSDK_NGX_DLSS_Hint_Render_Preset_Default:
                return "Default (NGX picks per tier)";
            case NVSDK_NGX_DLSS_Hint_Render_Preset_E:
                return "E (deprecated, CNN)";
            case NVSDK_NGX_DLSS_Hint_Render_Preset_F:
                return "F (deprecated, CNN)";
            case NVSDK_NGX_DLSS_Hint_Render_Preset_J:
                return "J (transformer, less ghosting / more flicker)";
            case NVSDK_NGX_DLSS_Hint_Render_Preset_K:
                return "K (transformer, recommended image quality)";
            case NVSDK_NGX_DLSS_Hint_Render_Preset_L:
                return "L (transformer, Ultra Performance default)";
            case NVSDK_NGX_DLSS_Hint_Render_Preset_M:
                return "M (transformer, Performance default)";
            default:
                return std::to_string(preset);
        }
    }

    int32_t computeFeatureFlags(const mcdr_dlss_input& in) {
        // P108x (A/B experiment, parent request): AutoExposure is OFF unless asked for via
        // MCDR_DLSS_AUTOEXPOSURE=1. We declare "auto exposure" while passing no `pInExposureTexture`
        // at all, and the device is removed ~1 ms after the first evaluate that has a REAL depth
        // buffer — the earlier build, whose depth snapshot was empty, survived the same call. The
        // reference integration ships this combination, but it is D3D11 on another DLSS build;
        // 310.9.1 may not tolerate it. Default OFF so one run answers "is AutoExposure the trigger?".
        int32_t flags = 0;
        {
            char ae[8] = {};
            if (::GetEnvironmentVariableA("MCDR_DLSS_AUTOEXPOSURE", ae, sizeof(ae)) > 0 &&
                ae[0] == '1') {
                flags |= static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
            }
        }
        if (in.depthInverted != 0) {
            flags |= static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_DepthInverted);
        }
        if (in.renderWidth != in.targetWidth || in.renderHeight != in.targetHeight) {
            flags |= static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
        }
        // P110: the motion vectors already contain the sub-pixel jitter (see the note in the feature
        // flag comment block above), so tell NGX to cancel it with InJitterOffsetX/Y.
        // P134：MV 改成真实运动后不再置 MVJittered（否则 NGX 会去减一个不存在的 jitter 差）。
        return flags;
    }
    // -----------------------------------------------------------------------
    // Resource helpers (this unit owns no device state, so it creates its own scratch
    // resources directly).
    // -----------------------------------------------------------------------
    // P108: resources move through the DEVICE MODULE's tracked transition, never through a
    // barrier written here.
    //
    // History (the bug this replaces): this helper used to take an explicit `before` state and
    // emit the barrier itself. That assumed the previous pass left the resource in
    // RENDER_TARGET/DEPTH_WRITE — but `endRenderPass` (P11) explicitly returns every attachment
    // to COMMON at the end of each pass, so the assumed `before` was wrong. In release that is
    // silently tolerated; the D3D12 debug layer reports "Before state ... does not match", and
    // P11's own notes record that this class of mismatch leads to GPU state corruption and a TDR
    // freeze. The module keeps a per-resource state map (`ctx->resourceState`) and its
    // `transitionTextureTo` is idempotent, so asking it is both correct and cheaper.
    //
    // `s.transitionFn`/`s.transitionCtx` are supplied by the module per call; when absent (a
    // diagnostic path that never opened a command list) transitions are simply skipped.
    void transition(ID3D12Resource* res, D3D12_RESOURCE_STATES after) {
        if (res == nullptr) {
            return;
        }
        if (s.transitionFn == nullptr) {
            return;
        }
        s.transitionFn(s.transitionCtx, reinterpret_cast<void*>(res), static_cast<int32_t>(after));
    }

    // B4：本单元**唯一**的纹理创建入口（模块注入的 create/destroy，见 mcdr_dlss_impl.h）。
    // 成功时 *outObject / *outResource 都填好；失败返回 false，调用方按"没有私有纹理"处理。
    bool createTrackedTexture(void* ctx, UINT width, UINT height, DXGI_FORMAT format, UINT mips,
                              UINT arraySize, UINT samples, UINT flags, void** outObject,
                              ID3D12Resource** outResource) {
        if (outObject != nullptr) *outObject = nullptr;
        if (outResource != nullptr) *outResource = nullptr;
        if (outObject == nullptr || outResource == nullptr) {
            return false;
        }
        if (gCreateTexture == nullptr) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                logWarn("P108: the device module injected no texture-create entry — private DLSS "
                        "textures are unavailable (older DLL/JVM pairing); only JVM-supplied "
                        "resources are used");
            }
            return false;
        }
        DlssTextureDesc d{};
        d.width = width;
        d.height = height;
        d.dxgiFormat = static_cast<uint32_t>(format);
        d.mipLevels = (mips > 1) ? mips : 1;
        d.arraySize = (arraySize > 1) ? arraySize : 1;
        d.sampleCount = (samples > 0) ? samples : 1;
        d.flags = flags;
        void* obj = nullptr;
        void* res = nullptr;
        if (gCreateTexture(ctx, &d, &obj, &res) != 1 || obj == nullptr || res == nullptr) {
            return false;
        }
        *outObject = obj;
        *outResource = static_cast<ID3D12Resource*>(res);
        return true;
    }

    // 销毁（幂等，指针一起清空）。`ctx` 只用于从该 ctx 的跟踪表里摘键，可为 nullptr（shutdown）。
    void destroyTrackedTexture(void* ctx, void** object) {
        if (object != nullptr && *object != nullptr && gDestroyTexture != nullptr) {
            gDestroyTexture(ctx, *object);
        }
        if (object != nullptr) {
            *object = nullptr;
        }
    }

    bool createScratchBuffer(UINT64 bytes) {
        if (s.depthStencil != nullptr && s.scratchBytes >= bytes) {
            return true;  // big enough already
        }
        if (s.depthStencil != nullptr) {
            s.depthStencil->Release();
            s.depthStencil = nullptr;
        }
        s.scratchBytes = 0;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        const HRESULT hr = s.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&s.depthStencil));
        if (FAILED(hr)) {
            s.depthStencil = nullptr;
            logWarn("P108: DLSS scratch buffer creation failed (%llu bytes, hr=0x%08X); DLSS disabled",
                    static_cast<unsigned long long>(bytes), static_cast<unsigned>(hr));
            return false;
        }
        s.scratchBytes = bytes;
        return true;
    }

    // P108: the private UAV texture DLSS writes into (see the field comment on `dlssOutput`).
    // Recreated only when the target size or format changes. Returns nullptr on failure (the
    // caller then reports the reason and leaves the existing path in place).
    //
    // B4: the texture is created **by the device module** now (DlssTextureDesc, see
    // mcdr_dlss_impl.h). Consequences this function must honour:
    //   * the module owns the resource and seeds its tracked state map with COMMON, so the very
    //     first transition in this list has a real anchor instead of a COMMON guess;
    //   * it is released through the module's deferred-delete path — never `Release()` here;
    //   * this unit keeps two values only: the opaque handle (`dlssOutputObject`) and the raw
    //     pointer NGX is handed (`dlssOutput`).
    ID3D12Resource* createDlssOutput(UINT width, UINT height, DXGI_FORMAT format) {
        if (s.dlssOutput != nullptr && s.dlssOutputWidth == width && s.dlssOutputHeight == height &&
            s.dlssOutputFormat == format) {
            return s.dlssOutput;
        }
        destroyTrackedTexture(s.transitionCtx, &s.dlssOutputObject);
        s.dlssOutput = nullptr;
        s.dlssOutputWidth = 0;
        s.dlssOutputHeight = 0;
        s.dlssOutputFormat = DXGI_FORMAT_UNKNOWN;
        if (width == 0 || height == 0) {
            return nullptr;
        }
        // P108x: UAV **only**.
        //
        // This used to be `ALLOW_UNORDERED_ACCESS | ALLOW_RENDER_TARGET`. A write-side bisect then
        // showed the texture could not hold a write at all: filling it from the colour input with
        // the COPY ENGINE — same texture, same queue, no shaders, no NGX — read back all-zero both
        // immediately and on the following frame, while the JVM target beside it read back fine
        // (mean 17.6/16.2/11.3, alpha 255). Copying into a resource needs no flag, and NGX is the
        // only consumer here that needs one (`FAIL_RWFlagMissing` = "UAV missing"), so the flag set
        // itself was the problem: `UAV | RTV` is the classic case where a driver declines the
        // unordered-access path while still reporting success.
        // P108x: UAV **only** (see the note above); ALLOW_RENDER_TARGET removed.
        void* obj = nullptr;
        ID3D12Resource* res = nullptr;
        if (!createTrackedTexture(s.transitionCtx, width, height, format, 1, 1, 1,
                                  static_cast<UINT>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), &obj,
                                  &res)) {
            logWarn("P108: DLSS private UAV output creation failed (%ux%u fmt=%d) — the device "
                    "module refused or failed to create it; the existing path stays in effect",
                    width, height, static_cast<int>(format));
            return nullptr;
        }
        s.dlssOutputObject = obj;
        s.dlssOutput = res;
        s.dlssOutputWidth = width;
        s.dlssOutputHeight = height;
        s.dlssOutputFormat = format;
        logInfo("P108: DLSS private UAV output created by the device module: %ux%u fmt=%d "
                "(ALLOW_UNORDERED_ACCESS, tracked) — DLSS writes here, then the result is copied "
                "into the JVM's target",
                width, height, static_cast<int>(format));
        return s.dlssOutput;
    }

    // -----------------------------------------------------------------------
    // P108: NGX's own log lines are routed into the native log, so a failing Init explains
    // itself instead of only returning a code. Signature per nvsdk_ngx_defs.h:376-379.
    void NVSDK_CONV dlssNgxLogCallback(const char* message, NVSDK_NGX_Logging_Level loggingLevel,
                                       NVSDK_NGX_Feature sourceComponent) {
        (void)loggingLevel;
        if (message != nullptr) {
            logInfo("P108: NGX[%d] %s", static_cast<int>(sourceComponent), message);
        }
    }

    // NGX init. Runs at most once (success or failure); the caller already checked that
    // a device exists.
    // -----------------------------------------------------------------------
    // P108: the NGX application-data directory.
    //
    // NGX writes its own logs/cache there and rejects a path that does not exist with
    // `FAIL_InvalidParameter` — but the reference integration that ships DLSS against this
    // same 310.x runtime (ETS2/ATS DLAA mod, `src/dlaa.cpp`) simply passes **the directory
    // this DLL lives in**, which always exists and never needs creating. Measured order:
    //   1. this module's own directory (MCDR.dll sits in `<user.dir>\dxrmc\` next to the
    //      vendor DLLs, so this is the natural home);
    //   2. `.\ngx`, created on demand (the previous behaviour);
    //   3. `"."` as a last resort.
    std::wstring buildAppDataPath() {
        std::wstring own = moduleDirectoryW();
        while (!own.empty() && (own.back() == L'\\' || own.back() == L'/')) {
            own.pop_back();
        }
        if (!own.empty() && ::GetFileAttributesW(own.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return own;
        }
        std::wstring fallback = MCDR_NGX_APPDATA;
        const DWORD attrs = ::GetFileAttributesW(fallback.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            if (::CreateDirectoryW(fallback.c_str(), nullptr) == FALSE &&
                ::GetLastError() != ERROR_ALREADY_EXISTS) {
                logWarn("P108: could not create the NGX application-data directory \"%s\" (err=%lu); "
                        "falling back to the process working directory",
                        narrow(fallback).c_str(), ::GetLastError());
                return L".";
            }
            logInfo("P108: created the NGX application-data directory \"%s\"",
                    narrow(fallback).c_str());
        }
        return fallback;
    }

#if MCDR_DLSS_HAVE_STREAMLINE
    std::wstring dlssModuleDirectory();  // 定义在下面，auto 探测要先用到
#endif

    // P121：**默认走 Streamline**。
    //   1) `MCDR_DLSS_IMPL` 显式指定优先（`ngx` 强制回退、`streamline` 强制启用）；
    //   2) 否则 `MCDR_STREAMLINE=1` 也算启用（历史开关）；
    //   3) 否则 auto：`sl.interposer.dll` 与 MCDR.dll 同目录就用 Streamline，不在就回退 NGX
    //      —— 发行版按 NVIDIA 许可不带运行库（AGENTS.md「不可再分发」），这条保证那时退回 NGX。
    bool dlssImplStreamline() {
        // P126（用户 m14488「现在已经改到了默认 streamline」之后的实测）：以前整段被
        // `static const bool` 缓存成**一次判定**。而 `sl.interposer.dll` 是 JVM 侧启动时
        // 才解压到 `dxrmc/` 的，native 的第一次调用可能早于解压完成 —— 于是判定成 false 并
        // 永久缓存，后面即使 interposer 已经在目录里也一直走 NGX（NGX 在本机建不出 feature，
        // `0xBAD0000C out of date`，于是每帧静默 return 0，表现就是「DLSS 没出图、只剩抖动」）。
        // 现在：环境变量每次读（显式指定就该立即生效），文件探测只在**还没看到**时重试，
        // 见到之后才锁存 —— 不会再把一次过早的否定结果固定下来。
        char buf[16] = {};
        const DWORD n = ::GetEnvironmentVariableA("MCDR_DLSS_IMPL", buf, 16);
        if (n > 0) {
            return _stricmp(buf, "streamline") == 0;
        }
        char legacy[8] = {};
        if (::GetEnvironmentVariableA("MCDR_STREAMLINE", legacy, 8) == 1 && legacy[0] == '1') {
            return true;
        }
#if MCDR_DLSS_HAVE_STREAMLINE
        static bool interposerSeen = false;
        if (!interposerSeen) {
            const std::wstring dir = dlssModuleDirectory();
            if (!dir.empty()) {
                const std::wstring path = dir + L"sl.interposer.dll";
                if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    interposerSeen = true;
                }
            }
        }
        return interposerSeen;
#else
        return false;
#endif
    }



#if MCDR_DLSS_HAVE_STREAMLINE
    using PFn_SlDLSSSetOptions = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSOptions&);

    // MCDR.dll 所在目录（带结尾反斜杠）。
    std::wstring dlssModuleDirectory() {
        HMODULE self = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(&dlssModuleDirectory), &self)) {
            return {};
        }
        wchar_t path[MAX_PATH] = {};
        if (!::GetModuleFileNameW(self, path, MAX_PATH)) return {};
        std::wstring pathStr(path);
        const auto slash = pathStr.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : pathStr.substr(0, slash + 1);
    }

    // 运行时取 interposer 的 slDLSSSetOptions（不引入静态导入，缺 SL 时返回 nullptr）。
    PFn_SlDLSSSetOptions slDlssSetOptionsFn() {
        static const PFn_SlDLSSSetOptions fn = []() -> PFn_SlDLSSSetOptions {
            const std::wstring dir = dlssModuleDirectory();
            if (dir.empty()) return nullptr;
            HMODULE sl = ::LoadLibraryW((dir + L"sl.interposer.dll").c_str());
            if (sl == nullptr) return nullptr;
            using PFn_GetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);
            auto getFeatureFunction = reinterpret_cast<PFn_GetFeatureFunction>(
                ::GetProcAddress(sl, "slGetFeatureFunction"));
            if (getFeatureFunction == nullptr) return nullptr;
            void* raw = nullptr;
            if (getFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", raw) != sl::Result::eOk) {
                return nullptr;
            }
            return reinterpret_cast<PFn_SlDLSSSetOptions>(raw);
        }();
        return fn;
    }

    // 把本帧几何 + 质量档写进 SL 的 DLSS 选项；把结果写到 sl-probe.txt 自证
    // （设备创建期的 native 日志可能早于文件 sink，见 P111 教训）。
    int32_t streamlineApplyOptions(const mcdr_dlss_input& in) {
        const PFn_SlDLSSSetOptions fn = slDlssSetOptionsFn();
        if (fn == nullptr) return -1;
        sl::DLSSOptions opt{};
        switch (s.qualityValue) {
            case NVSDK_NGX_PerfQuality_Value_DLAA:       opt.mode = sl::DLSSMode::eDLAA; break;
            case NVSDK_NGX_PerfQuality_Value_MaxQuality: opt.mode = sl::DLSSMode::eMaxQuality; break;
            case NVSDK_NGX_PerfQuality_Value_Balanced:   opt.mode = sl::DLSSMode::eBalanced; break;
            default:                                     opt.mode = sl::DLSSMode::eMaxPerformance; break;
        }
        opt.outputWidth = static_cast<uint32_t>(in.targetWidth);
        opt.outputHeight = static_cast<uint32_t>(in.targetHeight);
        opt.preExposure = in.preExposure > 0.0f ? in.preExposure : 1.0f;
        opt.exposureScale = in.exposureScale > 0.0f ? in.exposureScale : 1.0f;
        opt.useAutoExposure = sl::Boolean::eFalse;
        const sl::ViewportHandle viewport(0);
        const sl::Result r = fn(viewport, opt);
        static int optionsProbeWrites = 0;
        if (++optionsProbeWrites <= 3) if (FILE* f = _wfopen((dlssModuleDirectory() + L"sl-probe.txt").c_str(), L"a, ccs=UTF-8")) {
            fwprintf(f, L"P112 dlss-options: mode=%d out=%ux%u render=%dx%d result=0x%X\n",
                     static_cast<int>(opt.mode), opt.outputWidth, opt.outputHeight,
                     static_cast<unsigned>(in.renderWidth), static_cast<unsigned>(in.renderHeight),
                     static_cast<unsigned>(r));
            fclose(f);
        }
        static int optionsLogged = 0;
        if (optionsLogged < 3) {
            ++optionsLogged;
            logWarn("P112: DLSS impl=streamline — slDLSSSetOptions result=0x%X (mode=%d, out=%ux%u, "
                "render=%dx%d)",
                static_cast<unsigned>(r), static_cast<int>(opt.mode), opt.outputWidth,
                opt.outputHeight, static_cast<unsigned>(in.renderWidth),
                static_cast<unsigned>(in.renderHeight));
        }
        return static_cast<int32_t>(r);
    }
#endif

    bool ensureInit(ID3D12Device* device) {
        if (s.initAttempted) {
            return s.initOk && s.paramsOk;
        }
        s.initAttempted = true;
        s.device = device;
        if (dlssImplStreamline()) {
            // P112：SL 拥有 NGX。这里绝不能调用 NVSDK_NGX_D3D12_Init_*（同进程双 NGX 冲突，
            // P111 实测 sl.log 里 getNGXFeatureRequirements 报 0xbad00005）。
            logInfo("P112: DLSS impl=streamline — native NGX init skipped "
                    "(Streamline owns the runtime; evaluate lands in phase 2)");
            s.failure = "streamline mode: native NGX init skipped by design";
            return false;
        }

        // P108：**固定用 SDK 常量 `NVSDK_NGX_Version_API`(0x15)，不再做版本扫描**。
        // 依据：与我们的 DLL 同代（310.x）的生产实现（ETS2/ATS DLAA mod，src/dlaa.cpp，
        // 作者注明 "verified against the public NVIDIA DLSS SDK headers + DLSS Programming
        // Guide"）在同一个 310.x 运行库上用 **0x15 成功 Init**。所以 0x15 是对的，之前
        // 0x16+ 报 out of date 正是"声明比核心新"；而 0x15 下的 FAIL_InvalidParameter
        // (0xBAD00005) 是**参数**问题，靠下面的能力诊断把原因具名化，而不是靠猜版本。
        const std::wstring appDataPath = buildAppDataPath();
        logInfo("P108: NGX Init (sdkVersion=NVSDK_NGX_Version_API=0x%X, projectId=\"%s\", "
                "engineType=%d, engineVersion=\"%s\", appDataPath=\"%s\", featureInfo=%s)",
                static_cast<unsigned>(NVSDK_NGX_Version_API), MCDR_NGX_PROJECT_ID,
                static_cast<int>(NVSDK_NGX_ENGINE_TYPE_CUSTOM), MCDR_NGX_ENGINE_VERSION,
                narrow(appDataPath).c_str(), commonInfoForNgx() != nullptr ? "pathlist" : "null");
        // 参数表逐字对齐 nvsdk_ngx.h:247；**必须写在一行**（跨行时 MSVC 报 C2065）。
        const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(MCDR_NGX_PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, MCDR_NGX_ENGINE_VERSION, appDataPath.c_str(), device, commonInfoForNgx(), NVSDK_NGX_Version_API);
        if (NVSDK_NGX_FAILED(r)) {
            s.failure = "NVSDK_NGX_D3D12_Init_with_ProjectID";
            logWarn("P108: DLSS unavailable — NVSDK_NGX_D3D12_Init_with_ProjectID failed: %s "
                    "(appDataPath=\"%s\", featureInfo=%s)",
                    ngxResultString(r).c_str(), narrow(appDataPath).c_str(),
                    commonInfoForNgx() != nullptr ? "pathlist" : "null");
            return false;
        }
        s.initOk = true;

        NVSDK_NGX_Parameter* params = nullptr;
        const NVSDK_NGX_Result pr = NVSDK_NGX_D3D12_GetCapabilityParameters(&params);
        if (NVSDK_NGX_FAILED(pr) || params == nullptr) {
            s.failure = "NVSDK_NGX_D3D12_GetCapabilityParameters";
            logWarn("P108: DLSS unavailable — GetCapabilityParameters failed: %s",
                    ngxResultString(pr).c_str());
            return false;
        }
        s.params = params;
        s.paramsOk = true;

        // P108：**能力诊断**（照 ETS2/ATS 那份实现的做法）。这四个参数是把
        // `0xBAD00005` 变成"有名有姓的原因"的关键 —— `FeatureInitResult` 常直接带出
        // 真正的失败码，`NeedsUpdatedDriver` / `MinDriverVersion*` 说明是不是驱动不匹配。
        {
            int available = -1, needsDriver = -1, initResult = 0;
            unsigned int minMajor = 0, minMinor = 0;
            const NVSDK_NGX_Result ra = NVSDK_NGX_Parameter_GetI(
                params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
            const NVSDK_NGX_Result rn = NVSDK_NGX_Parameter_GetI(
                params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
            const NVSDK_NGX_Result rmj = NVSDK_NGX_Parameter_GetUI(
                params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minMajor);
            const NVSDK_NGX_Result rmn = NVSDK_NGX_Parameter_GetUI(
                params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minMinor);
            const NVSDK_NGX_Result ri = NVSDK_NGX_Parameter_GetI(
                params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
            logInfo("P108: NGX capabilities: available=%d(%s) needsUpdatedDriver=%d(%s) "
                    "minDriverVersion=%u.%u(%s/%s) featureInitResult=0x%08X(%s)",
                    available, ngxResultString(ra).c_str(), needsDriver, ngxResultString(rn).c_str(),
                    minMajor, minMinor, ngxResultString(rmj).c_str(), ngxResultString(rmn).c_str(),
                    static_cast<unsigned>(initResult), ngxResultString(ri).c_str());
            // 这一条单独再报一次：它是"能不能用"的总答案，读到它就不用猜了。
            if (available == 0 || needsDriver == 1) {
                logWarn("P108: NGX says DLSS-SR is NOT usable on this system "
                        "(available=%d needsUpdatedDriver=%d minDriverVersion=%u.%u "
                        "featureInitResult=0x%08X)",
                        available, needsDriver, minMajor, minMinor, static_cast<unsigned>(initResult));
            }
        }


        logInfo("P108: DLSS/NGX initialised (project id \"%s\", engine version \"%s\", appdata "
                "\".\\ngx\", SDK API version 0x%08X)",
                MCDR_NGX_PROJECT_ID, MCDR_NGX_ENGINE_VERSION,
                static_cast<unsigned>(NVSDK_NGX_Version_API));
        // One-time optimal-settings probe, so the log carries the SDK's own ratio table
        // even before a world is loaded. The query for the REAL target size happens in
        // chooseQuality().
        {
            unsigned int ow = 0, oh = 0, mw = 0, mh = 0, nw = 0, nh = 0;
            float sharp = 0.0f;
            const NVSDK_NGX_Result orr = NGX_DLSS_GET_OPTIMAL_SETTINGS(
                params, 1280, 720, NVSDK_NGX_PerfQuality_Value_MaxQuality, &ow, &oh, &mw, &mh, &nw,
                &nh, &sharp);
            if (!NVSDK_NGX_FAILED(orr)) {
                logInfo("P108: DLSS optimal settings: MaxQuality 1280x720 -> optimal %ux%u "
                        "(range %ux%u .. %ux%u), sharpness %.3f",
                        ow, oh, nw, nh, mw, mh, static_cast<double>(sharp));
            } else {
                logWarn("P108: DLSS optimal-settings query failed: %s", ngxResultString(orr).c_str());
            }
        }
        return true;
    }
    // Maps a render/target ratio onto the closest NVSDK_NGX_PerfQuality_Value and reports
    // what the SDK considers optimal for that quality.
    //
    // WHY THIS EXISTS: the render scale is chosen independently of DLSS (dxrMcRenderScale,
    // 25% steps), so "which DLSS quality" is not a user setting — it is whatever quality
    // reproduces the resolution the user already picked. Choosing the closest one also
    // keeps DLSS from rescaling the input a second time.
    // P108 (user m11147): `forceQuality` >= 0 means the user picked the tier explicitly, so the
    // ratio-based choice is skipped and this call only fetches what the SDK considers optimal
    // for that tier (in particular the sharpness) — and logs the comparison table as usual.
    uint32_t chooseQuality(NVSDK_NGX_Parameter* params, uint32_t targetW, uint32_t targetH,
                           uint32_t renderW, uint32_t renderH, int forceQuality = -1) {
        static const NVSDK_NGX_PerfQuality_Value kValues[] = {
            NVSDK_NGX_PerfQuality_Value_MaxPerf,      NVSDK_NGX_PerfQuality_Value_Balanced,
            NVSDK_NGX_PerfQuality_Value_MaxQuality,   NVSDK_NGX_PerfQuality_Value_UltraPerformance,
            NVSDK_NGX_PerfQuality_Value_UltraQuality, NVSDK_NGX_PerfQuality_Value_DLAA,
        };
        static const char* kNames[] = {"MaxPerf",  "Balanced",     "MaxQuality",
                                       "UltraPerformance", "UltraQuality", "DLAA"};
        const float want =
            (targetW > 0) ? (static_cast<float>(renderW) / static_cast<float>(targetW)) : 1.0f;
        // `forceQuality` is one of kValues[]; clamp so indexing is always in range.
        const std::size_t forcedIdx =
            (forceQuality >= 0) ? static_cast<std::size_t>(forceQuality) : 0;
        const bool forced = (forceQuality >= 0) && (forcedIdx < std::size(kValues));
        uint32_t best = static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality);
        float bestErr = 1e9f;
        unsigned int bestW = renderW, bestH = renderH;
        float bestSharp = 0.0f;
        std::string table;
        for (std::size_t i = 0; i < std::size(kValues); ++i) {
            unsigned int ow = 0, oh = 0, mw = 0, mh = 0, nw = 0, nh = 0;
            float sharp = 0.0f;
            const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(
                params, targetW, targetH, kValues[i], &ow, &oh, &mw, &mh, &nw, &nh, &sharp);
            if (NVSDK_NGX_FAILED(r) || ow == 0 || oh == 0) {
                continue;
            }
            const float ratio = static_cast<float>(ow) / static_cast<float>(targetW);
            const float err = (ratio > want) ? (ratio - want) : (want - ratio);
            char row[96] = {};
            std::snprintf(row, sizeof(row), "%s=%ux%u(%.3f) ", kNames[i], ow, oh,
                          static_cast<double>(ratio));
            table += row;
            if (forced) {
                // Self-documenting: this is the tier the user asked for, not the nearest one.
                if (i == forcedIdx) {
                    best = static_cast<uint32_t>(kValues[i]);
                    bestW = ow;
                    bestH = oh;
                    bestSharp = sharp;
                    bestErr = 0.0f;
                }
                continue;
            }
            if (err < bestErr) {
                bestErr = err;
                best = static_cast<uint32_t>(kValues[i]);
                bestW = ow;
                bestH = oh;
                bestSharp = sharp;
            }
        }
        if (!table.empty()) {
            logInfo("P108: DLSS optimal render sizes at %ux%u: %s", targetW, targetH, table.c_str());
        }
        if (forced) {
            // P108 (user m11147): say plainly that this tier came from the user, not from the
            // ratio — the log must never leave the two indistinguishable.
            logInfo("P108: DLSS quality tier is EXPLICIT (user button): %s -> sdk optimal %ux%u, "
                    "sharpness %.3f (render %ux%u target %ux%u, ratio would have picked "
                    "\"nearest\")",
                    kNames[forcedIdx], bestW, bestH, static_cast<double>(bestSharp), renderW,
                    renderH, targetW, targetH);
        }
        if (bestErr > 1e8f) {
            s.optimalOk = false;
            return static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality);
        }
        s.optimalOk = true;
        s.optimalRenderW = bestW;
        s.optimalRenderH = bestH;
        s.optimalSharpness = bestSharp;
        {
            char text[192] = {};
            std::snprintf(text, sizeof(text),
                          "target %ux%u, render %ux%u -> quality %s (sdk optimal %ux%u, sharpness "
                          "%.3f)",
                          targetW, targetH, renderW, renderH, kNames[best], bestW, bestH,
                          static_cast<double>(bestSharp));
            s.optimalText = text;
        }
        return best;
    }

    void destroyFeature() {
        if (s.feature != nullptr) {
            const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_ReleaseFeature(s.feature);
            if (NVSDK_NGX_FAILED(r)) {
                logWarn("P108: NVSDK_NGX_D3D12_ReleaseFeature failed: %s",
                        ngxResultString(r).c_str());
            }
            s.feature = nullptr;
        }
        s.featureOk = false;
        s.evalOk = false;
    }

    // Creates (or recreates) the feature. Requires an OPEN command list — NGX records its
    // own commands during CreateFeature.
    bool ensureFeature(ID3D12GraphicsCommandList* cmd, const mcdr_dlss_input& in) {
        if (s.createdWarned) {
            return false;  // a previous create failed: one warning, never retried
        }
        const bool sizeChanged =
            (in.renderWidth != s.renderWidth) || (in.renderHeight != s.renderHeight) ||
            (in.targetWidth != s.targetWidth) || (in.targetHeight != s.targetHeight);
        // P108 (user m11147): a tier change must rebuild too — PerfQualityValue is a
        // CreateFeature input, so it cannot be switched by editing Evaluate parameters alone.
        //
        // P108aa (user m11796, DLAA): resolve the tier THIS frame would build — including the
        // DLAA 1:1 restriction — and compare it against the tier the live feature was actually
        // built with. The old comparison was `requestedQuality != effectiveQuality`, where
        // `effectiveQuality` held the NGX enum, so it was wrong twice:
        //   * after a DLAA downgrade (requested 3 vs effective 2) it stayed true forever and
        //     re-created the feature every frame (480 creates / 24 s, ~30-40 ms each);
        //   * even a successful DLAA build stores NVSDK_NGX_PerfQuality_Value_DLAA == 5, so
        //     3 != 5 would have caused the same per-frame rebuild.
        // `effectiveQuality` now stores the ACCEPTED TIER (0..3, -1 = ratio-chosen), so the
        // downgrade becomes this round's baseline and is not re-detected every frame.
        int resolvedTier = s.requestedQuality;  // -1 = no explicit tier: ratio-chosen
        const bool dlaaMismatch =
            (resolvedTier == 3) && (in.renderWidth != in.targetWidth ||
                                    in.renderHeight != in.targetHeight);
        if (dlaaMismatch) {
            resolvedTier = 2;
        }
        if (dlaaMismatch != s.dlaaDowngraded) {
            s.dlaaDowngraded = dlaaMismatch;
            if (dlaaMismatch) {
                // Never silently build a "fake DLAA": DLAA is only meaningful at 1:1, and asking
                // for it while the render scale is, say, 80% would make NGX upscale under a DLAA
                // label. State-change only — a per-frame ERROR is how the 480-creates bug was
                // diagnosed, so it must not survive as log spam.
                logError("P108: DLAA (tier 3) requested but render %dx%d != target %dx%d — falling "
                         "back to the Quality tier for this round (accepted as the baseline). Set "
                         "the render scale to 100%% to use DLAA.",
                         in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight);
            } else if (s.requestedQuality == 3) {
                logInfo("P108: DLAA (tier 3) accepted: render %dx%d == target %dx%d",
                        in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight);
            }
        }
        const bool qualityChanged =
            (resolvedTier >= 0) && (resolvedTier != s.effectiveQuality);
        // P108ac (user m08033): the render preset (the DLSS "model") is a CreateFeature input as
        // well — NGX bakes the network choice into the feature — so a model change has to rebuild
        // the feature exactly like a tier change. `effectivePreset` is what the live feature got.
        const bool presetChanged = (s.requestedPreset != s.effectivePreset);
        if (s.featureOk && !sizeChanged && !qualityChanged && !presetChanged) {
            return true;
        }
        if (sizeChanged || qualityChanged || presetChanged) {
            destroyFeature();
            // A new size invalidates the history; a pure tier change does not have to, but the
            // rebuilt feature starts with no history anyway, so reset in both cases.
            s.resetPending = true;
        }
        if (s.params == nullptr) {
            return false;
        }

        if (resolvedTier >= 0) {
            // Explicit tier from the JVM: Performance / Balanced / Quality, same order as the
            // Java enum. The mapping is deliberate (0->MaxPerf, 1->Balanced, 2->MaxQuality).
            // P108 (user m11796): tier 3 = DLAA. DLAA reuses this whole pipeline unchanged
            // (depth snapshot, motion vectors, output handover, copy-back); only the
            // PerfQualityValue and the size relationship differ: DLAA means "no upscaling", i.e.
            // render size == target size.
            //
            // P110b: DLAA now gets the real sub-pixel jitter — the projection UBO carries the same
            // Halton sequence TAA uses, InJitterOffsetX/Y is that offset in render pixels, and the
            // MVJittered feature flag tells NGX the motion vectors are already in jittered space.
            // The old note here said the jitter was frozen to 0 because injection "collapsed the
            // frame"; that diagnosis was built on byte-swapped matrix data (see known_bugs.md D.25)
            // and no longer applies.
            static const uint32_t kTierToNgx[4] = {
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxPerf),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_Balanced),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_DLAA),
            };
            // `resolvedTier` is already 0..3 and has the DLAA 1:1 restriction applied above;
            // a rejected DLAA was turned into Quality *before* the rebuild decision, so the
            // accepted tier is the one remembered below.
            const int tier = resolvedTier;
            s.qualityValue = kTierToNgx[tier];
            s.qualityIsExplicit = true;
            // The optimal-settings query also yields the sharpness the SDK recommends for this
            // quality/size pair; keep using it (it is what the reference integration does).
            chooseQuality(s.params, static_cast<uint32_t>(in.targetWidth),
                          static_cast<uint32_t>(in.targetHeight),
                          static_cast<uint32_t>(in.renderWidth),
                          static_cast<uint32_t>(in.renderHeight), s.qualityValue);
        } else {
            s.qualityValue = chooseQuality(s.params, static_cast<uint32_t>(in.targetWidth),
                                           static_cast<uint32_t>(in.targetHeight),
                                           static_cast<uint32_t>(in.renderWidth),
                                           static_cast<uint32_t>(in.renderHeight));
            s.qualityIsExplicit = false;
        }
        s.effectiveQuality = resolvedTier;  // accepted tier (0..3), -1 = ratio-chosen
        s.featureFlags = computeFeatureFlags(in);

        // P108: EVERY input the size can depend on must be on the parameter map *before* the
        // query. Width/Height/OutWidth/OutHeight alone are not enough — with them set the call
        // returned Success but a scratch size of 0, which the old code misread as a failure
        // (`P108: DLSS unavailable — GetScratchBufferSize failed: success (0x00000001)`).
        // The create flags and the perf-quality value are part of the same descriptor, so set
        // them here too (they are also what `NGX_D3D12_CREATE_DLSS_EXT` will pass below).
        NVSDK_NGX_Parameter_SetUI(s.params, NVSDK_NGX_Parameter_Width,
                                  static_cast<unsigned>(in.renderWidth));
        NVSDK_NGX_Parameter_SetUI(s.params, NVSDK_NGX_Parameter_Height,
                                  static_cast<unsigned>(in.renderHeight));
        NVSDK_NGX_Parameter_SetUI(s.params, NVSDK_NGX_Parameter_OutWidth,
                                  static_cast<unsigned>(in.targetWidth));
        NVSDK_NGX_Parameter_SetUI(s.params, NVSDK_NGX_Parameter_OutHeight,
                                  static_cast<unsigned>(in.targetHeight));
        NVSDK_NGX_Parameter_SetI(s.params, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                                 s.featureFlags);
        NVSDK_NGX_Parameter_SetI(s.params, NVSDK_NGX_Parameter_PerfQualityValue,
                                 static_cast<int>(s.qualityValue));
        // P108ac (user m08033): the DLSS model. 0 = let NGX pick per tier (the long-standing
        // behaviour, and what an old JVM that never calls the setter gets); otherwise a specific
        // NVSDK_NGX_DLSS_Hint_Render_Preset value. This has to happen on the same parameter map as
        // PerfQualityValue, before NGX_D3D12_CREATE_DLSS_EXT — the preset is baked into the
        // feature at creation, which is why `presetChanged` forces a rebuild above.
        if (const char* presetKey = presetHintKey(s.qualityValue)) {
            NVSDK_NGX_Parameter_SetI(s.params, presetKey, s.requestedPreset);
        }
        s.effectivePreset = s.requestedPreset;
        std::size_t scratch = 0;
        const NVSDK_NGX_Result sr =
            NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature_SuperSampling, s.params, &scratch);
        logInfo("P108: DLSS scratch buffer query: result=%s size=%llu bytes",
                ngxResultString(sr).c_str(), static_cast<unsigned long long>(scratch));
        if (NVSDK_NGX_FAILED(sr)) {
            s.createdWarned = true;
            s.failure = "NVSDK_NGX_D3D12_GetScratchBufferSize";
            logWarn("P108: DLSS unavailable — GetScratchBufferSize failed: %s",
                    ngxResultString(sr).c_str());
            return false;
        }
        // A reported size of 0 is NOT an error: modern DLSS builds answer 0 and place their
        // scratch data in internal allocations. Allocate a conservative floor anyway so a build
        // that does use the buffer cannot read past it, and say so in the log.
        if (scratch == 0) {
            logInfo("P108: NGX reports a scratch buffer size of 0 (modern DLSS allocates "
                    "internally); allocating a 64 MiB floor regardless");
            scratch = 64ull * 1024ull * 1024ull;
        }
        if (!createScratchBuffer(static_cast<UINT64>(scratch))) {
            s.createdWarned = true;
            s.failure = "scratch buffer creation";
            return false;
        }

        NVSDK_NGX_DLSS_Create_Params cp{};
        // P108 (user m11796): DLAA is defined as "no upscaling", so the feature is created at the
        // TARGET resolution. The requested-tier path already refuses DLAA when render != target
        // (ERROR + fall back to Quality), so this is a belt-and-braces pin of the equal-size
        // invariant: whatever the caller passed, a DLAA feature never upscales.
        const bool dlssTierIsDlaa =
            s.qualityValue == static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_DLAA);
        cp.Feature.InWidth = static_cast<unsigned>(
            dlssTierIsDlaa ? in.targetWidth : in.renderWidth);
        cp.Feature.InHeight = static_cast<unsigned>(
            dlssTierIsDlaa ? in.targetHeight : in.renderHeight);
        cp.Feature.InTargetWidth = static_cast<unsigned>(in.targetWidth);
        cp.Feature.InTargetHeight = static_cast<unsigned>(in.targetHeight);
        cp.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(s.qualityValue);
        cp.InFeatureCreateFlags = s.featureFlags;
        cp.InEnableOutputSubrects = false;

        NVSDK_NGX_Handle* handle = nullptr;
        const NVSDK_NGX_Result cr = NGX_D3D12_CREATE_DLSS_EXT(
            cmd, 1u /*creation node mask*/, 1u /*visibility node mask*/, &handle, s.params, &cp);
        if (NVSDK_NGX_FAILED(cr) || handle == nullptr) {
            s.createdWarned = true;
            s.failure = "NGX_D3D12_CREATE_DLSS_EXT";
            logWarn("P108: DLSS unavailable — CreateFeature failed: %s (render %dx%d, target %dx%d, "
                    "flags=0x%X, quality=%u)",
                    ngxResultString(cr).c_str(), in.renderWidth, in.renderHeight, in.targetWidth,
                    in.targetHeight, static_cast<unsigned>(s.featureFlags), s.qualityValue);
            return false;
        }
        s.feature = handle;
        s.featureOk = true;
        s.renderWidth = in.renderWidth;
        s.renderHeight = in.renderHeight;
        s.targetWidth = in.targetWidth;
        s.targetHeight = in.targetHeight;
        s.createdThisFrame = true;
        // P108: print EVERY flag bit we pass, not just three of them. The joint debugging of the
        // black-world report needed exactly this: `AutoExposure` and `MVLowRes` were invisible in
        // the old line (they were both 0), so the two wrong bits could not be seen from the log.
        const auto bit = [](int32_t flags, NVSDK_NGX_DLSS_Feature_Flags f) {
            return (flags & static_cast<int32_t>(f)) != 0 ? 1 : 0;
        };
        const char* qualityName = "unknown";
        switch (s.qualityValue) {
            case static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxPerf): qualityName = "Performance"; break;
            case static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_Balanced): qualityName = "Balanced"; break;
            case static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality): qualityName = "Quality"; break;
            case static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_DLAA): qualityName = "DLAA"; break;
            default: break;
        }
        logInfo("P108: DLSS feature created: render %dx%d -> target %dx%d, quality=%u (%s, %s%s), "
                "flags=0x%X (AutoExposure=%d DepthInverted=%d MVLowRes=%d MVJittered=%d HDR=%d), "
                "scratch=%llu bytes",
                in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight, s.qualityValue,
                qualityName,
                s.qualityIsExplicit ? "explicit tier from the JVM" : "closest to the ratio",
                s.qualityValue == static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_DLAA)
                    ? ", render == target as required for DLAA"
                    : "",
                static_cast<unsigned>(s.featureFlags),
                bit(s.featureFlags, NVSDK_NGX_DLSS_Feature_Flags_AutoExposure),
                bit(s.featureFlags, NVSDK_NGX_DLSS_Feature_Flags_DepthInverted),
                bit(s.featureFlags, NVSDK_NGX_DLSS_Feature_Flags_MVLowRes),
                bit(s.featureFlags, NVSDK_NGX_DLSS_Feature_Flags_MVJittered),
                bit(s.featureFlags, NVSDK_NGX_DLSS_Feature_Flags_IsHDR),
                static_cast<unsigned long long>(s.scratchBytes));
        // P108ac (user m08033): which model this feature was built with, and on which NGX hint
        // key. The raw number is printed too: the letters NVIDIA reserves (N/O) map to numbers
        // that have no documented name, and "did the value reach NGX at all" is the question this
        // line answers when the picture does not change.
        {
            const char* key = presetHintKey(s.qualityValue);
            logInfo("P108: DLSS render preset = %d (%s) on %s", s.effectivePreset,
                    presetName(s.effectivePreset).c_str(), key != nullptr ? key : "(unmapped tier)");
        }
        return true;
    }

    // Logs the input package ONLY when something changed (a per-frame line would drown
    // the log during world load).
    void logFrameChange(const mcdr_dlss_input& in, bool createdThisFrame) {
        const bool changed = createdThisFrame || in.renderWidth != s.lastRenderW ||
                             in.renderHeight != s.lastRenderH || in.targetWidth != s.lastTargetW ||
                             in.targetHeight != s.lastTargetH || in.reset != s.lastReset ||
                             in.depthFormatOrdinal != s.lastDepthFormat ||
                             in.exposureMode != s.lastExposureMode ||
                             in.jitterOffsetX != s.lastJitterX || in.jitterOffsetY != s.lastJitterY ||
                             in.nearZ != s.lastNear || in.farZ != s.lastFar ||
                             static_cast<uint32_t>(s.featureFlags) != s.lastFlags;
        if (!changed) {
            return;
        }
        s.lastRenderW = in.renderWidth;
        s.lastRenderH = in.renderHeight;
        s.lastTargetW = in.targetWidth;
        s.lastTargetH = in.targetHeight;
        s.lastReset = in.reset;
        s.lastDepthFormat = in.depthFormatOrdinal;
        s.lastExposureMode = in.exposureMode;
        s.lastJitterX = in.jitterOffsetX;
        s.lastJitterY = in.jitterOffsetY;
        s.lastNear = in.nearZ;
        s.lastFar = in.farZ;
        s.lastFlags = static_cast<uint32_t>(s.featureFlags);
        logInfo("P108: DLSS input: render %dx%d -> target %dx%d, depthInverted=%u near=%.3f far=%.3f "
                "depthFmt=%d, mvScale=%.2f/%.2f, exposure=%s, jitter(single-sourced from the JVM "
                "Halton by design)=%.3f/%.3f, reset=%u, flags=0x%X, quality=%u",
                in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight, in.depthInverted,
                static_cast<double>(in.nearZ), static_cast<double>(in.farZ), in.depthFormatOrdinal,
                static_cast<double>(in.mvScaleX), static_cast<double>(in.mvScaleY),
                in.exposureMode == MCDR_DLSS_EXPOSURE_AUTO ? "auto-texture" : "constant",
                static_cast<double>(in.jitterOffsetX), static_cast<double>(in.jitterOffsetY), in.reset,
                static_cast<unsigned>(s.featureFlags), s.qualityValue);
    }
}  // namespace

// P118：给设备/交换链模块用的决策出口（外部链接；实现走上面的 dlssImplStreamline）。
int32_t mcdr_dlss_streamline_requested(void) { return dlssImplStreamline() ? 1 : 0; }

// ---------------------------------------------------------------------------
// C interface for the device module. Every string crosses as a (char*, cap) buffer,
// because std::string does not survive a module boundary (the plain TU sees the global
// module's std::string, the module sees import std;'s).
// ---------------------------------------------------------------------------
extern "C" {

// P108: is `p` backed by committed, readable memory? Used to vet the four texture handles in
// mcdr_dlss_input before any vtable call (see the comment in mcdr_dlss_evaluate for the crash
// this exists to prevent). Deliberately cheap: VirtualQuery walks the VAD for the address.
static bool isMappedReadable(const void* p) {
    if (p == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(p, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if ((mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xFFu;
    return prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
           prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_EXECUTE_WRITECOPY;
}

void mcdr_dlss_install_log(void* logInfo, void* logWarn, void* logError) {
    gLogInfo = reinterpret_cast<LogFn>(logInfo);
    gLogWarn = reinterpret_cast<LogFn>(logWarn);
    gLogError = reinterpret_cast<LogFn>(logError);
}

// B4：注入模块的私有纹理创建/销毁入口（见 mcdr_dlss_impl.h 的 DlssTextureDesc）。
// 返回 1 = 两个入口都装上了；0 = 缺一个（此时本单元不会创建任何私有纹理）。
int32_t mcdr_dlss_install_texture_ops(void* createFn, void* destroyFn) {
    gCreateTexture = reinterpret_cast<CreateTextureFn>(createFn);
    gDestroyTexture = reinterpret_cast<DestroyTextureFn>(destroyFn);
    if (gCreateTexture == nullptr || gDestroyTexture == nullptr) {
        logWarn("P108: the device module injected no texture create/destroy entry (create=%p "
                "destroy=%p) — DLSS keeps using only JVM-supplied resources",
                createFn, destroyFn);
        return 0;
    }
    logInfo("B4: DLSS private-texture ops installed (create=%p destroy=%p) — the private UAV and "
            "the depth snapshot are now created by the device module, so their resource states stay "
            "inside its tracked map",
            createFn, destroyFn);
    return 1;
}

// Called by the RTX probe once it has loaded nvngx_dlss.dll — that path is the single
// most useful diagnostic when Init fails, and the probe already has it.
void mcdr_dlss_register_runtime_path(const char* path, int32_t cap) {
    if (path == nullptr || cap <= 0) {
        return;
    }
    s.runtimePath = path;
}

// P108 blocker-B: the JVM announces the directory the vendor DLLs live in (the same
// `<user.dir>\dxrmc\` MCDR.dll is extracted to). This both widens the process DLL search
// path — so the bare-name LoadLibraryW inside the probe can succeed — and gives the
// loader below a path to try. Idempotent; returns 1 when the search path was widened.
int32_t mcdr_dlss_register_vendor_dir(const char* utf8Dir, int32_t cap) {
    if (utf8Dir == nullptr || cap <= 0) {
        return 0;
    }
    const std::string dir(utf8Dir);
    if (dir.empty()) {
        return 0;
    }
    logInfo("P108: DLSS vendor directory from the mod: \"%s\"", dir.c_str());
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()),
                                          nullptr, 0);
    if (need <= 0) {
        logWarn("P108: DLSS vendor directory is not valid UTF-8; ignoring it");
        return 0;
    }
    std::wstring wide(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()), wide.data(), need);
    while (!wide.empty() && (wide.back() == L'\\' || wide.back() == L'/')) {
        wide.pop_back();
    }
    s.vendorDir = wide;
    widenSearchPath(wide);
    // P108 blocker-B root cause: tell NGX where to look for its feature DLLs. This is what
    // turns Init/GetFeatureRequirements from FAIL_InvalidParameter into Success.
    rebuildCommonInfo(wide);
    logInfo("P108: NGX feature path list set to \"%s\" (nvngx feature DLLs are looked up there "
            "instead of the application folder, which is the JVM's bin\\ in a dev run)",
            narrow(wide).c_str());
    logEnvOnce();
    return s.searchPathSet ? 1 : 0;
}

// P108: loads nvngx_dlss.dll (the probe calls this instead of doing it itself, so every
// attempt lands in the native log with its own GetLastError). Returns the module handle as
// an opaque pointer, or NULL.
//
// The handle is cached: the module is loaded once and its HMODULE is what both the probe
// and NGX need. An earlier revision tried to re-derive the handle from the recorded path
// string with LoadLibraryW and returned whatever came back — when the path was stale that
// produced a non-NULL junk handle, which made the probe treat the runtime as present and
// its own fallback never ran (observed as `ngxRuntime=no` plus GetLastError=0x7E with no
// P108 attempt lines). Never derive the handle again.
void* mcdr_dlss_load_runtime(void) {
    logEnvOnce();
    if (s.runtimeModule != nullptr) {
        return s.runtimeModule;
    }
    HMODULE mod = loadRuntimeDll();
    if (mod == nullptr) {
        return nullptr;
    }
    s.runtimeModule = reinterpret_cast<void*>(mod);
    return s.runtimeModule;
}

// Reads back the nvngx_dlss.dll path the RTX probe reported (never NULL; an empty string
// until the probe has found it). Used for the one-time P108 availability line.
const char* mcdr_dlss_runtime_path(void) { return s.runtimePath.c_str(); }

// P108: the NVSDK_NGX_FeatureCommonInfo whose PathListInfo names the vendor directory, for
// callers that must pass it to NGX themselves (the RTX probe's GetFeatureRequirements).
// Returns NULL while no vendor directory is known.
void* mcdr_dlss_ngx_feature_info(void) { return commonInfoForNgx(); }

int32_t mcdr_dlss_can_run(void* device) {
    if (device == nullptr) {
        return 0;
    }
    // P108 阻塞 B：先确保运行期 DLL 真的加载过。探测通常已经加载（probeDlss 调
    // mcdr_dlss_load_runtime），但若 DLSS 是第一次被用到、或探测那次失败过，这里再试一次；
    // 没有它，ngx 的入口会以 ERROR_MOD_NOT_FOUND 之类的方式失败在 Init 里，日志却看不出
    // 是"没加载"还是"不支持"。
    logEnvOnce();
    if (s.runtimePath.empty()) {
        if (loadRuntimeDll() == nullptr) {
            return 0;
        }
    }
#if MCDR_DLSS_HAVE_STREAMLINE
    if (dlssImplStreamline()) {
        // P112 阶段 1：SL 拥有 NGX —— 既不加载也不初始化 nvngx。对 JVM 报"可用"，
        // 让 CreateDlssFeature 被调用（那里做 slDLSSSetOptions 并把结果写进 sl-probe.txt）。
        static bool logged = false;
        if (!logged) {
            logged = true;
            logInfo("P112: DLSS impl=streamline — can_run reports available (Streamline owns NGX)");
        }
        return 1;
    }
#endif
    return ensureInit(static_cast<ID3D12Device*>(device)) ? 1 : 0;
}

// flagsOut bits, mirrored by the device module's status bits:
//   1 INIT_TRIED  2 INIT_OK  4 PARAMS_OK  8 OPTIMAL_OK  16 FEATURE_OK  32 DISABLED
void mcdr_dlss_query(char* note, int32_t noteCap, char* optimal, int32_t optimalCap, char* runtime,
                     int32_t runtimeCap, uint32_t* flagsOut) {
    if (flagsOut != nullptr) {
        *flagsOut = 0;
        if (s.initAttempted) *flagsOut |= 1u;
        if (s.initOk) *flagsOut |= 2u;
        if (s.paramsOk) *flagsOut |= 4u;
        if (s.optimalOk) *flagsOut |= 8u;
        if (s.featureOk) *flagsOut |= 16u;
        if (s.disabled) *flagsOut |= 32u;
    }
    if (note != nullptr && noteCap > 0) {
        if (s.disabled) {
            std::snprintf(note, static_cast<std::size_t>(noteCap), "DLSS disabled, %s failed",
                          s.failure.empty() ? "an earlier stage" : s.failure.c_str());
        } else if (!s.initAttempted) {
            std::snprintf(note, static_cast<std::size_t>(noteCap), "DLSS not initialised yet");
        } else if (!s.initOk) {
            std::snprintf(note, static_cast<std::size_t>(noteCap), "NGX init failed (%s)",
                          s.failure.empty() ? "unknown" : s.failure.c_str());
        } else if (!s.enabled) {
            // P108 (user m10920): the switch is its own state, so report it before "active".
            std::snprintf(note, static_cast<std::size_t>(noteCap),
                          "NGX ready, DLSS switch is OFF (featureOk=%d, %llu frames submitted so far)",
                          s.featureOk ? 1 : 0, static_cast<unsigned long long>(s.submitCount));
        } else if (!s.featureOk) {
            std::snprintf(note, static_cast<std::size_t>(noteCap),
                          "NGX ready, DLSS ON but feature not created yet (%llu evaluates so far)",
                          static_cast<unsigned long long>(s.submitCount));
        } else {
            // P108 (user m11147): report the tier and where it came from, so the UI and the log
            // can be compared without guessing whether the user picked it or MCDR derived it.
            std::snprintf(note, static_cast<std::size_t>(noteCap),
                          "DLSS active, %llu frames submitted, render %dx%d -> target %dx%d, "
                          "quality=%u (%s)",
                          static_cast<unsigned long long>(s.submitCount), s.renderWidth, s.renderHeight,
                          s.targetWidth, s.targetHeight, s.qualityValue,
                          s.qualityIsExplicit ? "explicit tier" : "closest to the ratio");
        }
    }
    if (optimal != nullptr && optimalCap > 0) {
        std::snprintf(optimal, static_cast<std::size_t>(optimalCap), "%s", s.optimalText.c_str());
    }
    if (runtime != nullptr && runtimeCap > 0) {
        std::snprintf(runtime, static_cast<std::size_t>(runtimeCap), "%s", s.runtimePath.c_str());
    }
}
void tickCallStats(const char* kind);  // P114 诊断（定义在 streamline 段）

int32_t mcdr_dlss_create(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                         void* tlCtx) {
    if (in == nullptr) {
        return -1;
    }
    if (s.disabled) {
        return -1;
    }
    if (!s.initOk || !s.paramsOk) {
#if MCDR_DLSS_HAVE_STREAMLINE
        // P112 阶段 1：streamline 模式下 ensureInit 主动跳过 NGX，因此必然走到这里；
        // 把创建/选项交给 SL 的 slDLSSSetOptions，然后返回 0（evaluate 迁到阶段 2 才出图）。
        // P112：NGX 在 streamline 模式下从未 init，这里把创建/选项交给 SL 并返回 1 ——
        // 返回 1 表示"特征已就绪"，JVM 才会继续调用 mcdr_dx12EvaluateDlss；返回 0 会被
        // 当成"特征未就绪"而跳过 evaluate（P112 实测）。
        if (dlssImplStreamline()) {
            tickCallStats("create");
            (void)streamlineApplyOptions(*in);
            return 1;
        }
#endif
        return 0;
    }
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    if (cmd == nullptr) {
        return 0;
    }
    // Install the module-provided tracked-transition callback for this call (see transition()).
    s.transitionFn = tl;
    s.transitionCtx = tlCtx;
    s.createdThisFrame = false;
    if (!ensureFeature(cmd, *in)) {
        return 0;
    }
    logFrameChange(*in, s.createdThisFrame);
    return 1;
}

// P108x: localisation helpers for the "world layer renders as a flat beige rectangle while
// RTSS's OSD (drawn into the same swapchain) looks correct" report. Two things are checked
// with numbers before anyone guesses:
//   1. the exact DXGI format / flags of both sides of the CopyResource (a UNORM vs SRGB
//      mismatch makes the copy fail *silently*, leaving the destination untouched);
//   2. the destination's pixel statistics right after the copy — a uniform value means the
//      copy did not land (the JVM's own clear colour is what remained on screen).
// Read-back cannot say whether the frame "looks right"; it only says which texture lost the
// world content. The visible verdict stays with the user.
void logResourceFormat(const char* name, ID3D12Resource* res) {
    if (res == nullptr) {
        logInfo("P108x: %s = <null>", name);
        return;
    }
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    logInfo("P108x: %s fmt=%d (SRGB=%d) %llux%u mips=%u flags=0x%X samples=%u", name,
            static_cast<int>(d.Format),
            (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
             d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                ? 1
                : 0,
            static_cast<unsigned long long>(d.Width), d.Height, d.MipLevels,
            static_cast<unsigned>(d.Flags), d.SampleDesc.Count);
}

bool p108xFlag(const char* name) {
    char buf[8] = {};
    return ::GetEnvironmentVariableA(name, buf, sizeof(buf)) > 0 && buf[0] == '1';
}

// B3：7 项 settled 读数连续请求多少帧。第 1 帧全部 "(not ready)"（那一次拷贝刚录进还没提交的
// 列表），第 2 帧起才有数字，之后每帧读的都是"上一帧 settled"的内容。有限轮数是刻意的：
// 这是诊断而不是采样器。
constexpr int kP108xDumpRounds = 8;

// P108x: read a texture back and log its pixel statistics. Localisation only — the numbers
// say whether a texture is empty/uniform/plausible; they never claim how the frame looks.
//
// B3: the readings now come from the **device module's** READBACK path (DlssReadbackFn, see
// mcdr_dlss_impl.h) instead of a command list this unit builds for itself. Why that had to
// change is recorded in dumpManyResources below.
//
// Trust rules (a diagnostic that can lie is worse than no diagnostic):
//   * the transition into COPY_SOURCE is requested from the module, which tracks per-resource
//     state (`ctx->resourceState`) and skips a transition when from == to — so the map never
//     diverges from reality (that divergence was #525/#527/#838/RemoveDevice);
//   * the copy lands in the module's READBACK buffer and the module waits on its own fence, so
//     the bytes were really produced by the GPU rather than merely submitted;
//   * bytes per pixel come from the DXGI format instead of being assumed;
//   * a reading is **one frame late** by construction (the copy is recorded into the still-open
//     list, so the data can only be mapped after that list was submitted) — a 0 return means
//     "no reading yet", which is printed as such and never as a zero-filled texture.
//
// `state` used to be part of this struct: the unit declared the texture's state and restored it
// around raw barriers. Both are gone — declaring a state here is exactly how the mismatch got
// reintroduced, and the module is the only place that knows the true tracked state.
struct P108xDumpItem {
    ID3D12Resource* res;
    const char* name;
};

UINT p108xBytesPerPixel(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT: return 8;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_FLOAT: return 4;
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_FLOAT: return 2;
        case DXGI_FORMAT_R8_UNORM: return 1;
        default: return 4;
    }
}

// B3：**全部读回走模块的路径**（这是本次迁移的正文）。
//
// 旧版本在这个函数里自建 ID3D12CommandQueue / CommandAllocator / CommandList / Fence /
// staging，并用裸 `ResourceBarrier` 把纹理切到 COPY_SOURCE 再切回调用方声明的状态。
// 那些 barrier 对设备模块的 `ctx->resourceState` **不可见**，于是映射表与现实分叉，之后
// 每一次跟踪型过渡都发出非法 barrier：
//   [D3D12 ERROR #525] Before and after states must be different
//   [D3D12 ERROR #527] Before state (...) ... specified by ...
//   [D3D12 ERROR #838] Command lists must be successfully closed before execution
//   [D3D12 ERROR #232] RemoveDevice (DXGI_ERROR_INVALID_CALL)
// 而设备正是在一次"成功"的 evaluate 之后 1 ms 被拔掉的。自建命令列表还会与模块正在录制的
// 那条列表争资源状态，所以这一整条路径都必须消失，而不是加个开关绕开。
//
// 现在本函数只做一件事：逐项把**裸资源**交给模块的 `s.readbackFn`
// （设备模块的 readbackTextureStats；指针约定见 d3d12_device.cpp 里 dlssReadbackThunk 的
// 注释），由它：
//   * 用**当前这个 ctx 的 command list** 录一次 tracking 内的 CopyTextureRegion 到模块持有
//     的 READBACK buffer（过渡全部经过 transitionTextureTo，绕不过 ctx->resourceState）；
//   * 下一次调用时用模块自己的 fence 等那次提交完成，再映射、统计、回填 DlssTexStats；
//   * 因此读数**延迟一帧**：返回 1 = 上一帧记下的那份拷贝。
//
// 返回值语义（按 mcdr_dlss_impl.h:100-111）：
//   1 → out 已填好，按旧格式打 mean/min/max；
//   0 → 这张纹理**还没有**读数（第一次请求它，或那次拷贝还录在本帧尚未提交的列表里）——
//        打 `(not ready)`。**不是失败，也不能打成全 0**：全 0 会被读成"纹理是空的"，而
//        "纹理是不是空的"正是这个诊断要回答的问题，一旦撒谎整个结论就反了；
//  -1 → 失败（创建/映射失败），打一行 `(readback failed)` 并继续下一项，绝不拉起设备。
void dumpManyResources(const P108xDumpItem* items, int count) {
    if (!p108xFlag("DXRMC_P108X_DUMP")) {
        return;
    }
    if (items == nullptr || count <= 0) {
        return;
    }
    if (s.readbackFn == nullptr || s.transitionCtx == nullptr) {
        static bool s_loggedNoReadback = false;
        if (!s_loggedNoReadback) {
            s_loggedNoReadback = true;
            logInfo("P108x: dump skipped — the device module did not hand over a readback callback "
                    "(this DLL is paired with a module older than B3)");
        }
        return;
    }
    // B3: two labels can name the SAME resource — `private-uav-from-previous-frame-settled` is
    // `ngxTarget`, which IS `s.externalOutput` whenever the JVM handed a fitting UAV texture, and
    // `input-depth-after-evaluate` is `s.depthSnapshot`, which IS the resource the
    // `depth-snapshot-settled` item names. Asking the module twice in one frame for one texture
    // answers "(not ready)" the second time (by design: that texture's copy was recorded moments
    // ago into the still-open list, so the module refuses to wait for an unsubmitted fence), which
    // would make those two labels permanently unreadable. So a resource is read ONCE per call and
    // its reading is shared by every label that names it — same texture, same numbers, stated
    // explicitly in the line so nobody mistakes it for two independent measurements.
    struct ReadOnce {
        ID3D12Resource* res;
        int32_t rc;
        DlssTexStats stats;
    };
    constexpr int kMaxReadPerCall = 8;
    ReadOnce seen[kMaxReadPerCall] = {};
    int seenCount = 0;
    for (int i = 0; i < count; ++i) {
        ID3D12Resource* res = items[i].res;
        if (res == nullptr) {
            continue;
        }
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Format == DXGI_FORMAT_UNKNOWN) {
            logInfo("P108x: dump %s skipped (fmt=%d)", items[i].name, static_cast<int>(d.Format));
            continue;
        }
        const UINT w = static_cast<UINT>(d.Width);
        const UINT h = d.Height;
        const UINT bpp = p108xBytesPerPixel(d.Format);
        int32_t rc = 0;
        DlssTexStats stats{};
        bool shared = false;
        for (int k = 0; k < seenCount; ++k) {
            if (seen[k].res == res) {
                rc = seen[k].rc;
                stats = seen[k].stats;
                shared = true;
                break;
            }
        }
        if (!shared) {
            rc = s.readbackFn(s.transitionCtx, reinterpret_cast<void*>(res), &stats);
            if (seenCount < kMaxReadPerCall) {
                seen[seenCount].res = res;
                seen[seenCount].rc = rc;
                seen[seenCount].stats = stats;
                ++seenCount;
            }
        }
        const char* sharedNote = shared ? " [same texture as an earlier item in this round]" : "";
        if (rc == 0) {
            logInfo("P108x: dump %s %ux%u fmt=%d bpp=%u --- (not ready; the module recorded this "
                    "texture's first readback on this frame and it settles on the next one)%s",
                    items[i].name, w, h, static_cast<int>(d.Format), bpp, sharedNote);
            continue;
        }
        if (rc < 0) {
            logInfo("P108x: dump %s %ux%u fmt=%d bpp=%u --- (readback failed)%s", items[i].name, w, h,
                    static_cast<int>(d.Format), bpp, sharedNote);
            continue;
        }
        logInfo("P108x: dump %s %ux%u fmt=%d bpp=%u mean=%.1f/%.1f/%.1f/%.1f "
                "min=%u/%u/%u/%u max=%u/%u/%u/%u (raw bytes)%s",
                items[i].name, w, h, static_cast<int>(d.Format), bpp, stats.mean[0], stats.mean[1],
                stats.mean[2], stats.mean[3], stats.min[0], stats.min[1], stats.min[2], stats.min[3],
                stats.max[0], stats.max[1], stats.max[2], stats.max[3], sharedNote);
    }
}

// Single-texture convenience wrapper.
void dumpResourceStats(ID3D12Resource* res, const char* name) {
    P108xDumpItem item{res, name};
    dumpManyResources(&item, 1);
}

// P108: the depth snapshot (see mcdr_dlss_impl.h for why this exists).
//
// The JVM calls this from `DeepLearningSuperSampling.captureMotionVectors()`, i.e. at the point
// in the frame where the depth buffer still holds this frame's world depth — before vanilla's
// `clearDepthTexture` (GameRenderer.render) wipes it to 0.0 (reverse-Z "far"). By the time DLSS
// is evaluated the live depth is already clear, which is why the depth-aware upscale had
// degenerated into a colour-only one.
//
// The passed-in depth resource is COPY_SOURCE only for the duration of this call: we ask the
// module for its tracked state, transition, copy, and put it back exactly as it was, so the
// JVM's state machine around the world render is untouched.
int32_t mcdr_dlss_snapshot_depth(void* commandList, void* depthResource, DlssTransitionFn tl,
                                 void* tlCtx, DlssQueryStateFn stateOf) {
    if (commandList == nullptr || depthResource == nullptr || tl == nullptr) {
        return 0;
    }
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    auto* depth = reinterpret_cast<ID3D12Resource*>(depthResource);
    if (cmd == nullptr || depth == nullptr || s.device == nullptr) {
        return 0;
    }
    const D3D12_RESOURCE_DESC dd = depth->GetDesc();
    const UINT w = static_cast<UINT>(dd.Width);
    const UINT h = dd.Height;
    if (w == 0 || h == 0 || dd.Format == DXGI_FORMAT_UNKNOWN) {
        return 0;
    }
    // P76f：设备模块现在把深度纹理建在**无类型**格式上（`R32_TYPELESS` 等），否则它无法被
    // 采样（有类型资源建别的格式的 SRV 是非法的，采样恒返回 0 —— 那是速度 pass 读到全 0
    // 深度的原因）。但 NGX 只认具体的有类型格式，所以快照这一侧仍按具体格式建；拷贝
    // 在同一类型族内的 TYPELESS ↔ typed 之间是合法的。
    const DXGI_FORMAT snapFormat = [](DXGI_FORMAT f) -> DXGI_FORMAT {
        switch (f) {
            case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
            case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_D16_UNORM;
            case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
            case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            default: return f;
        }
    }(dd.Format);
    // (Re)create the private snapshot texture when the size/format changed.
    // P108x: compare EVERYTHING a full-resource copy requires to agree, not just w/h/format. The
    // debug layer caught the old check being too loose:
    //   [D3D12 ERROR #944] CopyResource: Source and Destination texture resource must have equivalent
    //   dimensions, including width, height, depth, mip levels, and array size.
    // The copy therefore never landed and the snapshot stayed all-zero (a dead depth term for NGX).
    if (s.depthSnapshot == nullptr || s.depthSnapshotWidth != w || s.depthSnapshotHeight != h ||
        s.depthSnapshotFormat != snapFormat || s.depthSnapshotMips != dd.MipLevels ||
        s.depthSnapshotArray != dd.DepthOrArraySize ||
        s.depthSnapshotSamples != dd.SampleDesc.Count) {
        // B4: created by the device module (see DlssTextureDesc in mcdr_dlss_impl.h) and released
        // through its deferred-delete path — never Release() here. Mirror the source's
        // mip/array/sample shape so a full copy is legal at every level. Flags stay NONE: this
        // texture is only ever COPY_DEST / COPY_SOURCE / NON_PIXEL_SHADER_RESOURCE.
        destroyTrackedTexture(tlCtx, &s.depthSnapshotObject);
        s.depthSnapshot = nullptr;
        s.depthSnapshotWidth = 0;
        s.depthSnapshotHeight = 0;
        s.depthSnapshotFormat = DXGI_FORMAT_UNKNOWN;
        void* snapObj = nullptr;
        ID3D12Resource* snapRes = nullptr;
        if (!createTrackedTexture(tlCtx, w, h, snapFormat, dd.MipLevels, dd.DepthOrArraySize,
                                  dd.SampleDesc.Count, 0, &snapObj, &snapRes)) {
            s.depthSnapshot = nullptr;
            if (!s.depthSnapshotWarned) {
                s.depthSnapshotWarned = true;
                logWarn("P108: depth snapshot texture creation failed (%ux%u fmt=%d) — the device "
                        "module refused or failed to create it; DLSS falls back to the "
                        "JVM-supplied depth (which is already cleared by then, so the upscale "
                        "loses its depth term)",
                        w, h, static_cast<int>(dd.Format));
            }
            return -1;
        }
        s.depthSnapshotObject = snapObj;
        s.depthSnapshot = snapRes;
        s.depthSnapshotWidth = w;
        s.depthSnapshotHeight = h;
        s.depthSnapshotFormat = snapFormat;
        s.depthSnapshotMips = (dd.MipLevels > 1) ? dd.MipLevels : 1;
        s.depthSnapshotArray = (dd.DepthOrArraySize > 1) ? dd.DepthOrArraySize : 1;
        s.depthSnapshotSamples = dd.SampleDesc.Count;
        logInfo("P108: DLSS depth snapshot texture created by the device module: %ux%u fmt=%d "
                "(private, tracked, COPY_DEST/COPY_SOURCE)",
                w, h, static_cast<int>(dd.Format));
    }
    // Save the caller's state, copy, restore. `stateOf` may be null (older module), in which
    // case COMMON is used as the tracked anchor and restored to COMMON.
    const int32_t before = (stateOf != nullptr)
                               ? stateOf(tlCtx, depthResource)
                               : static_cast<int32_t>(D3D12_RESOURCE_STATE_COMMON);
    // P108x: the JVM depth is deliberately NOT dumped here any more.
    //
    // The helper drains the GPU queue up to the point of insertion, so it does read real bytes —
    // but at this call site the interesting content is not there yet: this frame's depth is
    // written by commands the *current, still-open* command list has not submitted, so the read
    // can only ever show the cleared value. That made the label misleading ("the source is
    // empty") when the truth was "this reading cannot see frame-in-flight content".
    //
    // The trustworthy judgement is the depth actually handed to NGX at evaluate time, which is
    // dumped as `input-depth-after-evaluate` (the snapshot when one exists). The build that moved
    // the snapshot hook to the end of the frame's main pass reports e.g.
    //   input-depth-after-evaluate 683x384 fmt=40 mean=14.6/16.4/13.5 max=255/254/255
    // i.e. real depth. Anything read at an earlier point is uninformative and is omitted.
    // P108x: `CopyTextureRegion` with an explicit box on subresource 0, NOT `CopyResource`.
    // `CopyResource` demands identical width/height/depth/mip/array (#944 above) and silently does
    // nothing when they differ; an explicit box copies the top mip of slice 0 regardless of the rest
    // of the shape, which is all DLSS samples anyway.
    // The snapshot's own state is QUERIED, never assumed — the debug layer also flagged a barrier
    // whose Before state was wrong (#527, `0x800 COPY_SOURCE` on this very texture).
    const int32_t snapBefore = (stateOf != nullptr)
                                   ? stateOf(tlCtx, reinterpret_cast<void*>(s.depthSnapshot))
                                   : static_cast<int32_t>(D3D12_RESOURCE_STATE_COMMON);
    tl(tlCtx, depthResource, static_cast<int32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE));
    tl(tlCtx, reinterpret_cast<void*>(s.depthSnapshot),
       static_cast<int32_t>(D3D12_RESOURCE_STATE_COPY_DEST));
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = depth;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = s.depthSnapshot;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        const D3D12_BOX box{0, 0, 0, w, h, 1};
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    if (!s.depthSnapshotShapeLogged) {
        // ④ release-build visibility: `CopyTextureRegion` returns void, so a shape problem would be
        // silent outside the debug layer. Print both shapes and the queried state once.
        s.depthSnapshotShapeLogged = true;
        const D3D12_RESOURCE_DESC sd = s.depthSnapshot->GetDesc();
        logInfo("P108x: depth snapshot copy: src %ux%u mips=%u array=%u samples=%u -> dst %llux%u "
                "mips=%u array=%u samples=%u (box %ux%u, subresource 0 only), before_state=%d "
                "(queried)",
                w, h, dd.MipLevels, dd.DepthOrArraySize, dd.SampleDesc.Count,
                static_cast<unsigned long long>(sd.Width), sd.Height, sd.MipLevels,
                sd.DepthOrArraySize, sd.SampleDesc.Count, w, h, snapBefore);
    }
    tl(tlCtx, reinterpret_cast<void*>(s.depthSnapshot),
       static_cast<int32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    tl(tlCtx, depthResource, before);  // hand the depth back exactly as it was found
    ++s.depthSnapshotFrame;
    if (s.depthSnapshotFrame == 1) {
        logInfo("P108: DLSS depth snapshot captured (first): %ux%u fmt=%d from resource %p; it "
                "replaces the JVM's depth at evaluate time",
                w, h, static_cast<int>(dd.Format), depthResource);
    }
    return 1;
}

// P113：最近一帧的 SL frame token（Reflex 的 slReflexSleep 要在 Present 前用它）。
sl::FrameToken* gLastFrameToken = nullptr;

    // P114 诊断：每秒统计 streamline 的 evaluate / create 调用次数。
    // 若 evaluate/s 明显大于 fps，说明每帧被调了多次（那会直接把 DLSS 的 GPU 成本翻倍）。
    void tickCallStats(const char* kind) {
        static uint32_t evalCalls = 0;
        static uint32_t createCalls = 0;
        static ULONGLONG lastTick = 0;
        (kind[0] == 'e' ? evalCalls : createCalls)++;
        const ULONGLONG now = ::GetTickCount64();
        if (lastTick == 0) { lastTick = now; return; }
        if (now - lastTick >= 1000) {
            logInfo("P112 calls/s eval=%u create=%u", evalCalls, createCalls);
            evalCalls = 0;
            createCalls = 0;
            lastTick = now;
        }
    }

    // P113/P115：Reflex 低延迟 + PCL 延迟标记 + 帧率限制器。
    // 文档：ProgrammingGuideReflex.md §4（选项）/§5（Present 前 sleep）、ProgrammingGuidePCL.md（标记）。
    using PFn_SetReflexOptions = sl::Result (*)(const sl::ReflexOptions&);
    using PFn_ReflexSleep = sl::Result (*)(const sl::FrameToken&);
    using PFn_PCLSetMarker = sl::Result (*)(sl::PCLMarker, const sl::FrameToken&);

    namespace {
        struct ReflexApi {
            PFn_SetReflexOptions setOptions = nullptr;
            PFn_ReflexSleep sleep = nullptr;
            PFn_PCLSetMarker marker = nullptr;
            sl::Result (*newFrameToken)(sl::FrameToken*&, const uint32_t*) = nullptr;
            sl::Result (*getState)(sl::ReflexState&) = nullptr;
            uint32_t frameCounter = 0;
            bool resolved = false;
            bool optionsApplied = false;
            uint32_t frameLimitUs = 0;
            uint32_t markerFailures = 0;
        };
        ReflexApi gReflex;
        bool reflexEnabled() {
            static const bool enabled = []() {
                char v[8] = {};
                const DWORD n = ::GetEnvironmentVariableA("MCDR_REFLEX", v, 8);
                return !(n == 1 && v[0] == '0');
            }();
            return enabled;
        }
        void resolveReflexApi() {
            if (gReflex.resolved) return;
            gReflex.resolved = true;
            char lim[16] = {};
            const DWORD limLen = ::GetEnvironmentVariableA("MCDR_REFLEX_FRAME_LIMIT_US", lim, 16);
            if (limLen > 0) {
                gReflex.frameLimitUs = static_cast<uint32_t>(std::strtoul(lim, nullptr, 10));
            }
            const std::wstring dir = dlssModuleDirectory();
            if (dir.empty()) return;
            HMODULE sl = ::LoadLibraryW((dir + L"sl.interposer.dll").c_str());
            if (sl == nullptr) return;
            using PFn_GetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);
            auto gff = reinterpret_cast<PFn_GetFeatureFunction>(::GetProcAddress(sl, "slGetFeatureFunction"));
            if (gff == nullptr) return;
            void* raw = nullptr;
            if (gff(sl::kFeatureReflex, "slReflexSetOptions", raw) == sl::Result::eOk) {
                gReflex.setOptions = reinterpret_cast<PFn_SetReflexOptions>(raw);
            }
            raw = nullptr;
            if (gff(sl::kFeatureReflex, "slReflexSleep", raw) == sl::Result::eOk) {
                gReflex.sleep = reinterpret_cast<PFn_ReflexSleep>(raw);
            }
            raw = nullptr;
            if (gff(sl::kFeaturePCL, "slPCLSetMarker", raw) == sl::Result::eOk) {
                gReflex.marker = reinterpret_cast<PFn_PCLSetMarker>(raw);
            }
            gReflex.newFrameToken = reinterpret_cast<decltype(gReflex.newFrameToken)>(
                ::GetProcAddress(sl, "slGetNewFrameToken"));
            raw = nullptr;
            if (gff(sl::kFeatureReflex, "slReflexGetState", raw) == sl::Result::eOk) {
                gReflex.getState = reinterpret_cast<decltype(gReflex.getState)>(raw);
            }
        }
        void applyReflexOptions() {
            if (gReflex.setOptions == nullptr || gReflex.optionsApplied) return;
            gReflex.optionsApplied = true;
            sl::ReflexOptions opt{};
            opt.mode = sl::ReflexMode::eLowLatency;
            opt.frameLimitUs = gReflex.frameLimitUs;  // 0 = 不限帧（文档：与 mode 独立）
            const sl::Result r = gReflex.setOptions(opt);
            logWarn("P113 reflex: slReflexSetOptions(eLowLatency, frameLimitUs=%u) result=0x%X",
                    gReflex.frameLimitUs, static_cast<unsigned>(r));
            if (FILE* f = _wfopen((dlssModuleDirectory() + L"sl-probe.txt").c_str(), L"a, ccs=UTF-8")) {
                fwprintf(f, L"P113 reflex: setOptions=0x%X sleep=%s marker=%s frameLimitUs=%u\n",
                         static_cast<unsigned>(r), gReflex.sleep != nullptr ? L"ok" : L"missing",
                         gReflex.marker != nullptr ? L"ok" : L"missing", gReflex.frameLimitUs);
                fclose(f);
            }
        }
        void emitMarker(sl::PCLMarker marker) {
            if (gReflex.marker == nullptr || gLastFrameToken == nullptr) return;
            const sl::Result r = gReflex.marker(marker, *gLastFrameToken);
            if (r != sl::Result::eOk && gReflex.markerFailures < 3) {
                ++gReflex.markerFailures;
                logWarn("P113 pcl: marker %d result=0x%X", static_cast<int>(marker),
                        static_cast<unsigned>(r));
            }
        }
    }  // namespace

    void mcdr_reflex_set_frame_limit(uint32_t micros) {
        resolveReflexApi();
        gReflex.frameLimitUs = micros;
        gReflex.optionsApplied = false;  // 下次 Present 前重新下发
    }

    // P117：每帧开始（世界渲染之前）—— 建本帧的 SL frame token 并发 eSimulationStart。
    // 文档 ProgrammingGuidePCL.md:144/152：simulation/submit/present 标记必须用**同一帧**的 token。
    void mcdr_reflex_frame_begin(void) {
        if (!reflexEnabled()) return;
        resolveReflexApi();
        gLastFrameToken = nullptr;
        sl::FrameToken* token = nullptr;
        uint32_t frameIndex = ++gReflex.frameCounter;
        if (gReflex.newFrameToken != nullptr &&
            gReflex.newFrameToken(token, &frameIndex) == sl::Result::eOk && token != nullptr) {
            gLastFrameToken = token;
        }
        emitMarker(sl::PCLMarker::eSimulationStart);
    }

    // P117：通用标记（eSimulationEnd / eRenderSubmitStart 等由 JVM 在正确时机调）。
    void mcdr_reflex_marker(int32_t marker) {
        if (!reflexEnabled()) return;
        resolveReflexApi();
        emitMarker(static_cast<sl::PCLMarker>(marker));
    }

    void mcdr_reflex_before_present(void) {
        if (!reflexEnabled()) return;
        resolveReflexApi();
        applyReflexOptions();
        emitMarker(sl::PCLMarker::eRenderSubmitEnd);  // 本帧所有提交已完成，接着 Present
        emitMarker(sl::PCLMarker::ePresentStart);
        if (gReflex.sleep != nullptr && gLastFrameToken != nullptr) {
            (void)gReflex.sleep(*gLastFrameToken);
        }
    }

    void mcdr_reflex_after_present(void) {
        if (!reflexEnabled()) return;
        emitMarker(sl::PCLMarker::ePresentEnd);
        gLastFrameToken = nullptr;  // 下一帧由 mcdr_reflex_frame_begin() 重新建

        // P117：每秒读一次 ReflexState，把 PCL 统计的实测延迟打进日志（证明标记链路生效）。
        static ULONGLONG lastStatsTick = 0;
        static bool statsReported = false;
        const ULONGLONG now = ::GetTickCount64();
        if (gReflex.getState == nullptr) return;
        if (lastStatsTick != 0 && now - lastStatsTick < 1000) return;
        lastStatsTick = now;
        sl::ReflexState state{};
        if (gReflex.getState(state) != sl::Result::eOk || !state.latencyReportAvailable) return;
        // 报告里有两串 frameID（一串是 QPC 量级的巨大值、一串是 1,2,3…），按 frameID 取最大
        // 会挑到无效条目；按 simStartTime 取**最新**才是本帧那条。
        const sl::ReflexReport* newest = nullptr;
        for (const sl::ReflexReport& report : state.frameReport) {
            if (report.frameID == 0 || report.simStartTime == 0 || report.presentEndTime == 0) continue;
            if (newest == nullptr || report.simStartTime > newest->simStartTime) {
                newest = &report;
            }
        }
        if (newest == nullptr) return;
        // 时间戳是 QPC tick（本机 10 MHz），不是纳秒：µs = ticks * 1e6 / 频率。
        LARGE_INTEGER freq{};
        ::QueryPerformanceFrequency(&freq);
        const unsigned long long ticksPerUs =
            freq.QuadPart > 0 ? static_cast<unsigned long long>(freq.QuadPart) / 1000000ull : 10ull;
        const auto us = [ticksPerUs](uint64_t from, uint64_t to) -> unsigned long long {
            return to > from ? (to - from) / ticksPerUs : 0ull;
        };
        const unsigned long long latency = us(newest->simStartTime, newest->presentEndTime);
        const unsigned long long simUs = us(newest->simStartTime, newest->simEndTime);
        const unsigned long long submitUs = us(newest->renderSubmitStartTime, newest->renderSubmitEndTime);
        const unsigned long long presentUs = us(newest->presentStartTime, newest->presentEndTime);
        logInfo("P113 pcl: latency=%lluus sim=%lluus submit=%lluus present=%lluus gpu=%uus active=%uus",
                latency, simUs, submitUs, presentUs,
                static_cast<unsigned>(newest->gpuFrameTimeUs),
                static_cast<unsigned>(newest->gpuActiveRenderTimeUs));
        if (!statsReported) {
            statsReported = true;
            if (FILE* f = _wfopen((dlssModuleDirectory() + L"sl-probe.txt").c_str(), L"a, ccs=UTF-8")) {
                fwprintf(f, L"P113 pcl stats: latency=%lluus gpu=%uus active=%uus\n", latency,
                         static_cast<unsigned>(newest->gpuFrameTimeUs),
                         static_cast<unsigned>(newest->gpuActiveRenderTimeUs));
                fclose(f);
            }
        }
    }

    // P112 阶段 3：每秒打一行 fps（两个实现路径共用），用于 ngx vs streamline 的同场景 A/B。
    // 计数单位是 mcdr_dlss_evaluate 的成功次数 == 每秒评价的帧数。
    void tickDlssFps(const char* impl) {
        static ULONGLONG lastTick = 0;
        static uint32_t frames = 0;
        ++frames;
        const ULONGLONG now = ::GetTickCount64();
        if (lastTick == 0) {
            lastTick = now;
            return;
        }
        const ULONGLONG elapsed = now - lastTick;
        if (elapsed >= 1000) {
            logInfo("P112 fps=%u impl=%s", static_cast<unsigned>(frames * 1000ull / elapsed), impl);
            frames = 0;
            lastTick = now;
        }
    }

#if MCDR_DLSS_HAVE_STREAMLINE
    // P112：JVM 每帧送来的相机常量（sl::Constants 的数据源）。单线程（渲染线程）写入/读取。
    mcdr_dlss_constants gCameraConstants{};
    bool gCameraConstantsValid = false;

    int32_t mcdr_dlss_set_constants(const mcdr_dlss_constants* constants) {
        if (constants == nullptr) return 0;
        gCameraConstants = *constants;
        gCameraConstantsValid = true;
        return 1;
    }

    using PFn_slGetNewFrameToken = sl::Result (*)(sl::FrameToken*&, const uint32_t*);
    using PFn_slSetTagForFrame = sl::Result (*)(const sl::FrameToken&, const sl::ViewportHandle&,
                                                const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
    using PFn_slEvaluateFeature = sl::Result (*)(sl::Feature, const sl::FrameToken&,
                                                 const sl::BaseStructure**, uint32_t,
                                                 sl::CommandBuffer*);
    // ProgrammingGuide.md §2.11.1：slSetConstants(constants, frame, viewport)
    using PFn_slSetConstants = sl::Result (*)(const sl::Constants&, const sl::FrameToken&,
                                             const sl::ViewportHandle&);

    // P112 阶段 2：Streamline 的每帧调用面（全部运行时解析，不引入静态导入）。
    struct StreamlineEvalApi {
        PFn_SlDLSSSetOptions setOptions = nullptr;
        PFn_slGetNewFrameToken newFrameToken = nullptr;
        PFn_slSetTagForFrame setTags = nullptr;
        PFn_slSetConstants setConstants = nullptr;
        PFn_slEvaluateFeature evaluate = nullptr;
    };

    const StreamlineEvalApi& streamlineEvalApi() {
        static const StreamlineEvalApi api = []() -> StreamlineEvalApi {
            StreamlineEvalApi a{};
            const std::wstring dir = dlssModuleDirectory();
            if (dir.empty()) return a;
            HMODULE sl = ::LoadLibraryW((dir + L"sl.interposer.dll").c_str());
            if (sl == nullptr) return a;
            a.newFrameToken = reinterpret_cast<PFn_slGetNewFrameToken>(
                ::GetProcAddress(sl, "slGetNewFrameToken"));
            a.setTags = reinterpret_cast<PFn_slSetTagForFrame>(
                ::GetProcAddress(sl, "slSetTagForFrame"));
            a.setConstants = reinterpret_cast<PFn_slSetConstants>(
                ::GetProcAddress(sl, "slSetConstants"));
            a.evaluate = reinterpret_cast<PFn_slEvaluateFeature>(
                ::GetProcAddress(sl, "slEvaluateFeature"));
            using PFn_GetFeatureFunction = sl::Result (*)(sl::Feature, const char*, void*&);
            auto gff = reinterpret_cast<PFn_GetFeatureFunction>(
                ::GetProcAddress(sl, "slGetFeatureFunction"));
            if (gff != nullptr) {
                void* raw = nullptr;
                if (gff(sl::kFeatureDLSS, "slDLSSSetOptions", raw) == sl::Result::eOk) {
                    a.setOptions = reinterpret_cast<PFn_SlDLSSSetOptions>(raw);
                }
            }
            return a;
        }();
        return api;
    }

    // P112：4x4 行主序矩阵工具（sl::Constants 需要 cameraViewToClip / clipToCameraView /
    // clipToPrevClip / prevClipToClip 四个矩阵，而 JVM 只送 viewProj 与 prevViewProj）。
    void mat4Multiply(const float* a, const float* b, float* out) {
        float r[16];
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += a[row * 4 + k] * b[k * 4 + col];
                }
                r[row * 4 + col] = sum;
            }
        }
        std::memcpy(out, r, sizeof(r));
    }

    // Gauss-Jordan 求逆（失败返回 false，out 保持原样）。
    bool mat4Inverse(const float* m, float* out) {
        double a[4][8] = {};
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) a[row][col] = m[row * 4 + col];
            a[row][4 + row] = 1.0;
        }
        for (int col = 0; col < 4; ++col) {
            int pivot = col;
            for (int row = col + 1; row < 4; ++row) {
                if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) pivot = row;
            }
            if (std::fabs(a[pivot][col]) < 1e-12) return false;
            if (pivot != col) {
                for (int k = 0; k < 8; ++k) std::swap(a[col][k], a[pivot][k]);
            }
            const double inv = 1.0 / a[col][col];
            for (int k = 0; k < 8; ++k) a[col][k] *= inv;
            for (int row = 0; row < 4; ++row) {
                if (row == col) continue;
                const double f = a[row][col];
                if (f == 0.0) continue;
                for (int k = 0; k < 8; ++k) a[row][k] -= f * a[col][k];
            }
        }
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) out[row * 4 + col] = static_cast<float>(a[row][4 + col]);
        }
        return true;
    }
    // SL 的 DLSS 评价：按 ProgrammingGuideDLSS.md 的顺序 —— tag 资源 → 设选项 → evaluate。
    // eDisableCLStateTracking 下资源状态由我们负责（这里沿用 NGX 路径的同一组状态）。
    int32_t streamlineEvaluate(void* commandList, const mcdr_dlss_input& in, DlssTransitionFn tl,
                               void* tlCtx) {
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
        // P124 诊断（排查完删）：每 2 秒一行，说明这次调用带着什么进来 —— 之前
        // `streamline evaluate` 探针一直没写过，说明本函数在探针之前就 return 了。
        {
            static ULONGLONG lastTick = 0;
            static uint32_t calls = 0;
            ++calls;
            const ULONGLONG now = ::GetTickCount64();
            if (lastTick == 0 || now - lastTick >= 2000) {
                lastTick = now;
                std::fprintf(stderr,
                             "[dxrmc] P124 sl-eval call#%u: cmd=%p color=%p depth=%p mv=%p out=%p "
                             "render=%ux%u target=%ux%u\n",
                             static_cast<unsigned>(calls), static_cast<void*>(cmd),
                             static_cast<void*>(reinterpret_cast<ID3D12Resource*>(in.pInColor)),
                             static_cast<void*>(reinterpret_cast<ID3D12Resource*>(in.pInDepth)),
                             static_cast<void*>(reinterpret_cast<ID3D12Resource*>(in.pInMotionVectors)),
                             static_cast<void*>(reinterpret_cast<ID3D12Resource*>(in.pInOutput)),
                             static_cast<unsigned>(in.renderWidth),
                             static_cast<unsigned>(in.renderHeight),
                             static_cast<unsigned>(in.targetWidth),
                             static_cast<unsigned>(in.targetHeight));
            }
        }
        if (cmd == nullptr) return 0;
        tickCallStats("eval");
        const StreamlineEvalApi& api = streamlineEvalApi();
        auto* color = reinterpret_cast<ID3D12Resource*>(in.pInColor);
        auto* depth = reinterpret_cast<ID3D12Resource*>(in.pInDepth);
        auto* mv = reinterpret_cast<ID3D12Resource*>(in.pInMotionVectors);
        auto* out = reinterpret_cast<ID3D12Resource*>(in.pInOutput);
        if (api.setOptions == nullptr || api.newFrameToken == nullptr ||
            api.setTags == nullptr || api.evaluate == nullptr) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                logWarn("P112: streamline evaluate unavailable — interposer exports or "
                        "slDLSSSetOptions missing");
            }
            return 0;
        }
        if (color == nullptr || depth == nullptr || mv == nullptr || out == nullptr) return 0;
        // P112：输出目标优先用 JVM 交付的 UAV 纹理（s.externalOutput，见 mcdr_dlss_set_output_texture）。
        // 直接把 GUI 目标（无 UAV 位）标成 kBufferTypeScalingOutputColor，NGX 会报
        // FAIL_RWFlagMissing(0xBAD00009)（SL 日志里是 "[sl.dlss] NGX evaluate feature failed"）。
        // P114 诊断：确认 DLSS 的输入颜色到底是谁 —— 世界层（render 尺寸）还是已经合成好的
        // GUI 目标（target 尺寸）。如果 color 是 target 尺寸，说明 GUI 也被送进 DLSS 了。
        // 只在几何/指针变化时打印，这样 GUI 打开、窗口缩放等切换都会留下记录。
        static ID3D12Resource* lastColor = nullptr;
        static ID3D12Resource* lastOut = nullptr;
        static uint32_t geomChanges = 0;
        if ((color != lastColor || out != lastOut) && geomChanges < 40) {
            ++geomChanges;
            lastColor = color;
            lastOut = out;
            const D3D12_RESOURCE_DESC cd = color->GetDesc();
            const D3D12_RESOURCE_DESC od = out->GetDesc();
            logWarn("P114 geom#%u: color=%p %ux%u fmt=%d out=%p %ux%u fmt=%d render=%dx%d target=%dx%d",
                    geomChanges, static_cast<void*>(color),
                    static_cast<unsigned>(cd.Width), static_cast<unsigned>(cd.Height),
                    static_cast<int>(cd.Format), static_cast<void*>(out),
                    static_cast<unsigned>(od.Width), static_cast<unsigned>(od.Height),
                    static_cast<int>(od.Format),
                    static_cast<unsigned>(in.renderWidth), static_cast<unsigned>(in.renderHeight),
                    static_cast<unsigned>(in.targetWidth), static_cast<unsigned>(in.targetHeight));
        }
        // P114（用户报告的 GUI 文字被当作运动物体）：DLSS 的 temporal history 以**自己的输出**
        // 为反馈源。如果直接把 GUI 目标（每帧还要被 GUI 画一遍）标成 ScalingOutputColor，
        // 历史缓冲会被 GUI 污染 —— 滑动界面时文字就被当成运动物体放大/拖出框外。
        // NGX 路径从来没这个现象，因为它写的是私有 UAV / handover 纹理再拷回目标。
        // 这里照做：优先 externalOutput（JVM 交付的 UAV），否则用模块私有的 dlssOutput，
        // 只有两者都不可用才退化成直接写 GUI 目标。
        ID3D12Resource* outResource = out;
        bool copyBack = false;
        const auto fitsTarget = [&in](ID3D12Resource* res) {
            if (res == nullptr) return false;
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            return d.Width == static_cast<UINT64>(in.targetWidth) &&
                   d.Height == static_cast<UINT>(in.targetHeight);
        };
        if (fitsTarget(s.externalOutput)) {
            outResource = s.externalOutput;
            copyBack = true;
        } else if (fitsTarget(s.dlssOutput)) {
            outResource = s.dlssOutput;
            copyBack = true;
        }
        static ID3D12Resource* lastSel = nullptr;
        if (outResource != lastSel) {
            lastSel = outResource;
            logWarn("P114 out-sel: %s externalOutput=%p dlssOutput=%p out=%p",
                    outResource == out ? "GUI-target" : "private-UAV",
                    static_cast<void*>(s.externalOutput), static_cast<void*>(s.dlssOutput),
                    static_cast<void*>(out));
        }
        s.transitionFn = tl;
        s.transitionCtx = tlCtx;
        // 文档（ProgrammingGuideDLSS.md:155）：**资源状态由 SL 管理，宿主不要手动 transition**；
        // 宿主的责任是 evaluate 之后恢复命令列表状态（:242-253，配合 eDisableCLStateTracking）。
        // P117：token 由 mcdr_reflex_frame_begin() 在帧开始建好；这里复用，保证 PCL 标记与
        // DLSS evaluate 落在**同一帧**（文档要求）。没有 frame_begin（例如 Reflex 关闭）时兜底自建。
        sl::FrameToken* token = gLastFrameToken;
        if (token == nullptr) {
            uint32_t frameIndex = static_cast<uint32_t>(s.submitCount);
            if (api.newFrameToken(token, &frameIndex) != sl::Result::eOk || token == nullptr) return 0;
        }
        const sl::ViewportHandle viewport(0);
        gLastFrameToken = token;  // P113：Reflex 在 Present 前要用它

        // 1) 相机常量（每帧、尽量早；ProgrammingGuide.md §2.11.1）。没有 JVM 送来的
        //    `mcdr_dlss_constants` 时只能跳过 —— slSetConstants 缺字段 DLSS 会拒绝该帧。
        int constResult = -1;
        if (gCameraConstantsValid) {
            sl::Constants cnst{};
            // P118 修（用户报「DLSS 静止不动也有随机拖影」）：这里有三个**传参错误**，每一个都足以
            // 让 DLSS 在完全静止时每帧取错历史：
            //   1) 布局：JVM 送的是列主序（JOML `Matrix4f.get(float[])`），而 `sl::Constants` 的
            //      `float4x4` 明确要求**行主序**（sl_consts.h:183-185）。原来直接 memcpy，等于把
            //      整组矩阵转置着交给 SL —— 静止时当前/上一帧只差一个 jitter，转置会把这点差异
            //      映射到错误的 clip 轴上，于是每个像素每帧都多出约半个像素的错误位移 ⇒ 拖影。
            //   2) `prevClipToClip` 抄成了 `inverse(prevViewProj)`，公式是
            //      `prevClipToClip = clipToPrevClip.inverse() = inverse(prevViewProj) * viewProj`
            //      （sl_consts.h:193-199），少了右乘的当前帧矩阵，它根本不是"上一帧 clip → 本帧
            //      clip"的映射。
            //   3) 矩阵求逆/相乘用的是本文件的行主序工具（mat4Multiply/mat4Inverse），所以必须先
            //      把列主序输入转成行主序再做数学，否则连 `clipToPrevClip` 都是转置的。
            const float* viewProj = gCameraConstants.viewProjection;
            const float* prevViewProj = gCameraConstants.prevViewProjection;
            float curVP[16] = {};
            float prvVP[16] = {};
            // 列主序 -> 行主序：rowMajor[r*4+c] = colMajor[c*4+r]
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    curVP[r * 4 + c] = viewProj[c * 4 + r];
                    prvVP[r * 4 + c] = prevViewProj[c * 4 + r];
                }
            }
            float invCurVP[16] = {};
            float invPrvVP[16] = {};
            const bool invOk = mat4Inverse(curVP, invCurVP);
            const bool invPrevOk = mat4Inverse(prvVP, invPrvVP);
            // 行主序之后可以直接拷进 sl::float4x4（它就是行主序的 16 个 float）。
            // P120：`cameraViewToClip` 的语义是 **view->clip**，也就是投影矩阵本身（不是 VP）。
            // 之前塞 VP，SL 的 depth->view 反投影就多带一层世界平移，深度相关的亚像素修正会随视角
            // 漂移 —— 用户看到的「静止时随机抖动」。
            const float* projColMajor = gCameraConstants.projection;
            float curProj[16] = {};
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    curProj[r * 4 + c] = projColMajor[c * 4 + r];
                }
            }
            std::memcpy(&cnst.cameraViewToClip, curProj, sizeof(curProj));
            {
                float invProj[16] = {};
                if (mat4Inverse(curProj, invProj)) {
                    std::memcpy(&cnst.clipToCameraView, invProj, sizeof(invProj));
                }
            }
            if (invOk) {
                // clipToPrevClip = clipToView * viewToViewPrev * viewToClipPrev
                //                = inverse(curVP) * prevVP
                float clipToPrev[16] = {};
                mat4Multiply(invCurVP, prvVP, clipToPrev);
                std::memcpy(&cnst.clipToPrevClip, clipToPrev, sizeof(clipToPrev));
                if (invPrevOk) {
                    // prevClipToClip 必须是上面那个的逆：inverse(prevVP) * curVP。
                    // 旧代码写的是 inverse(prevVP)，等于把"上一帧 clip"直接当成"本帧 clip"。
                    float prevClipToClip[16] = {};
                    mat4Multiply(invPrvVP, curVP, prevClipToClip);
                    std::memcpy(&cnst.prevClipToClip, prevClipToClip, sizeof(prevClipToClip));
                }
            }
            cnst.jitterOffset = {gCameraConstants.jitterOffsetX, gCameraConstants.jitterOffsetY};
            cnst.mvecScale = {gCameraConstants.mvecScaleX, gCameraConstants.mvecScaleY};
            cnst.cameraPos = {gCameraConstants.cameraPosX, gCameraConstants.cameraPosY,
                              gCameraConstants.cameraPosZ};
            cnst.cameraUp = {gCameraConstants.cameraUpX, gCameraConstants.cameraUpY,
                             gCameraConstants.cameraUpZ};
            cnst.cameraRight = {gCameraConstants.cameraRightX, gCameraConstants.cameraRightY,
                                gCameraConstants.cameraRightZ};
            cnst.cameraFwd = {gCameraConstants.cameraFwdX, gCameraConstants.cameraFwdY,
                              gCameraConstants.cameraFwdZ};
            cnst.cameraNear = gCameraConstants.cameraNear;
            cnst.cameraFar = gCameraConstants.cameraFar;
            cnst.cameraFOV = gCameraConstants.cameraFovRadians;
            cnst.cameraAspectRatio = gCameraConstants.cameraAspectRatio;
            cnst.depthInverted = gCameraConstants.depthInverted ? sl::Boolean::eTrue
                                                                : sl::Boolean::eFalse;
            cnst.cameraMotionIncluded = gCameraConstants.cameraMotionIncluded
                                            ? sl::Boolean::eTrue
                                            : sl::Boolean::eFalse;
            cnst.motionVectors3D =
                gCameraConstants.motionVectors3D ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            cnst.motionVectorsJittered = gCameraConstants.motionVectorsJittered
                                             ? sl::Boolean::eTrue
                                             : sl::Boolean::eFalse;
            cnst.reset = gCameraConstants.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            // 注意：本版 SDK 的 sl::Constants 没有 renderingGameFrames 字段（文档草稿里有、
            // sl_consts.h:182 的实际结构没有），所以那一位只能留在 JVM/AI 侧不用。
            cnst.motionVectorsDilated = sl::Boolean::eFalse;
            cnst.orthographicProjection = sl::Boolean::eFalse;
            constResult = static_cast<int>(api.setConstants(cnst, *token, viewport));
        }

        // 2) 资源 tag（lifecycle 按文档 :143-147：输入/输出 eOnlyValidNow，深度/MV eValidUntilPresent）
        // 只保留输出这一个 barrier：文档 :155 说资源状态由 SL 管理，输入侧的 transition 没必要，
        // 还会和 SMAA/GUI 的既有状态重复切；tag 里仍带上资源当前应有的 state（D3D12 下必填）。
        transition(outResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        sl::Resource colorRes(sl::ResourceType::eTex2d, color,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource depthRes(sl::ResourceType::eTex2d, depth,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource mvRes(sl::ResourceType::eTex2d, mv,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource outRes(sl::ResourceType::eTex2d, outResource,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        sl::ResourceTag tags[4] = {
            sl::ResourceTag(&colorRes, sl::kBufferTypeScalingInputColor,
                            sl::ResourceLifecycle::eOnlyValidNow),
            sl::ResourceTag(&depthRes, sl::kBufferTypeDepth,
                            sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(&mvRes, sl::kBufferTypeMotionVectors,
                            sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(&outRes, sl::kBufferTypeScalingOutputColor,
                            sl::ResourceLifecycle::eOnlyValidNow),
        };
        const sl::Result tagResult = api.setTags(*token, viewport, tags, 4, cmd);

        // 3) 选项（:170-186：各档 preset + 模式 + 输出尺寸 + 非 HDR/非自动曝光）
        sl::DLSSOptions opt{};
        opt.dlaaPreset = sl::DLSSPreset::ePresetK;
        opt.qualityPreset = sl::DLSSPreset::ePresetK;
        opt.balancedPreset = sl::DLSSPreset::ePresetK;
        opt.performancePreset = sl::DLSSPreset::ePresetM;
        opt.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
        switch (s.qualityValue) {
            case NVSDK_NGX_PerfQuality_Value_DLAA:       opt.mode = sl::DLSSMode::eDLAA; break;
            case NVSDK_NGX_PerfQuality_Value_MaxQuality: opt.mode = sl::DLSSMode::eMaxQuality; break;
            case NVSDK_NGX_PerfQuality_Value_Balanced:   opt.mode = sl::DLSSMode::eBalanced; break;
            default:                                     opt.mode = sl::DLSSMode::eMaxPerformance; break;
        }
        opt.outputWidth = static_cast<uint32_t>(in.targetWidth);
        opt.outputHeight = static_cast<uint32_t>(in.targetHeight);
        opt.preExposure = in.preExposure > 0.0f ? in.preExposure : 1.0f;
        opt.exposureScale = in.exposureScale > 0.0f ? in.exposureScale : 1.0f;
        opt.useAutoExposure = sl::Boolean::eFalse;
        opt.colorBuffersHDR = sl::Boolean::eFalse;
        opt.alphaUpscalingEnabled = sl::Boolean::eFalse;
        const sl::Result optResult = api.setOptions(viewport, opt);

        // 4) evaluate（文档 :235：inputs 只需要 viewport；资源已经在 tag 里给过）
        const sl::BaseStructure* evalInputs[1] = {&viewport};
        const sl::Result evalResult = api.evaluate(sl::kFeatureDLSS, *token, evalInputs, 1, cmd);
        ++s.submitCount;

        // 4.5) handover：只有当 SL 写的那张纹理不是 GUI 目标时才需要拷回（同尺寸、同格式）。
        // P115：`outResource == out` 说明两者已经是同一张纹理，这一拷贝可以直接省掉。
        if (copyBack && outResource != out && evalResult == sl::Result::eOk) {
            transition(outResource, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(out, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(out, outResource);
        }
        // 5) 宿主负责恢复（:242-253 + eDisableCLStateTracking）：只恢复输出到 RT。
        // 输入侧我们不再改状态，所以也没有要恢复的 barrier。
        transition(out, D3D12_RESOURCE_STATE_RENDER_TARGET);

        static int loggedRuns = 0;
        if (loggedRuns < 3) {
            ++loggedRuns;
            logWarn("P112: streamline evaluate tags=0x%X options=0x%X evaluate=0x%X "
                    "(mode=%d, out=%ux%u)",
                    static_cast<unsigned>(tagResult), static_cast<unsigned>(optResult),
                    static_cast<unsigned>(evalResult), static_cast<int>(opt.mode),
                    opt.outputWidth, opt.outputHeight);
            if (FILE* f = _wfopen((dlssModuleDirectory() + L"sl-probe.txt").c_str(),
                                  L"a, ccs=UTF-8")) {
                fwprintf(f, L"streamline evaluate: constants=%d tags=0x%X options=0x%X evaluate=0x%X\n",
                         constResult, static_cast<unsigned>(tagResult),
                         static_cast<unsigned>(optResult), static_cast<unsigned>(evalResult));
                fclose(f);
            }
        }
        if (evalResult == sl::Result::eOk) {
            tickDlssFps("streamline");
            return 1;
        }
        return 0;
    }
#endif

int32_t mcdr_dlss_evaluate(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                           void* tlCtx, DlssReadbackFn rb) {
    if (in == nullptr) {
        return -1;
    }
    // P128 诊断（排查完删）：每 2 秒说明这次调用走哪条分支、被哪个闸门挡住。
    {
        static ULONGLONG lastTick = 0;
        static uint32_t calls = 0;
        ++calls;
        const ULONGLONG now = ::GetTickCount64();
        if (lastTick == 0 || now - lastTick >= 2000) {
            lastTick = now;
            const bool sl = dlssImplStreamline();
            // P133：把"native 这一帧真正收到的 jitter"与 token 指针一起打出来。
            // 符号 A/B 得到"一样抖"说明抖动不随所报 jitter 的符号变化 —— 那要么是
            // 常量没被逐帧接纳（token 陈旧），要么是收到的 jitter 根本不随帧变化。
            std::fprintf(stderr,
                         "[dxrmc] P128 eval call#%u: impl=%s disabled=%d featureOk=%d params=%p "
                         "cmd=%p render=%ux%u target=%ux%u recvJitter=(%.4f, %.4f) token=%p\n",
                         static_cast<unsigned>(calls), sl ? "streamline" : "ngx",
                         s.disabled ? 1 : 0, s.featureOk ? 1 : 0,
                         static_cast<void*>(s.params), static_cast<void*>(commandList),
                         static_cast<unsigned>(in->renderWidth),
                         static_cast<unsigned>(in->renderHeight),
                         static_cast<unsigned>(in->targetWidth),
                         static_cast<unsigned>(in->targetHeight),
                         static_cast<double>(gCameraConstants.jitterOffsetX),
                         static_cast<double>(gCameraConstants.jitterOffsetY),
                         static_cast<void*>(gLastFrameToken));
        }
    }
#if MCDR_DLSS_HAVE_STREAMLINE
    // P127（p93 实测：native 已自报 impl=streamline，但 streamlineEvaluate 一次都没被调用）：
    // `s.disabled` 是 **NGX 路径**自己的失败状态（建不出 feature / MV 中间纹理失败时置位）。
    // 它原来排在这个分支之前，于是启动阶段只要 NGX 探测失败，Streamline 就被永久挡在门外，
    // native 每帧静默 return -1，JVM 只看到 "failed to evaluate 30 frames in a row"。
    // Streamline 模式下 NGX 的状态与它无关，所以必须先判 SL。
    if (dlssImplStreamline()) {
        // P112 阶段 2：streamline 模式下 NGX 从未 init/创建，这里直接走 SL 的
        // tag → options → evaluate 链（资源状态由 streamlineEvaluate 内部负责）。
        return streamlineEvaluate(commandList, *in, tl, tlCtx);
    }
#endif
    if (s.disabled) {
        return -1;
    }
    if (!s.featureOk || s.params == nullptr) {
        return 0;
    }
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    if (cmd == nullptr) {
        return 0;
    }
    // Install the module-provided tracked-transition callback for this call (see transition()).
    s.transitionFn = tl;
    s.transitionCtx = tlCtx;
    s.readbackFn = rb;  // B3 会用它；本轮为 nullptr 时仅保存，不做任何读回。
    auto* color = reinterpret_cast<ID3D12Resource*>(in->pInColor);
    auto* depth = reinterpret_cast<ID3D12Resource*>(in->pInDepth);
    auto* mv = reinterpret_cast<ID3D12Resource*>(in->pInMotionVectors);
    auto* out = reinterpret_cast<ID3D12Resource*>(in->pInOutput);
    if (color == nullptr || depth == nullptr || mv == nullptr || out == nullptr) {
        return 0;
    }
    // P108 (2026-10-16 crash, hs_err_pid52140): a NON-NULL but invalid handle passed the
    // `== nullptr` check above and faulted on the very first vtable call —
    // `EXCEPTION_ACCESS_VIOLATION reading 0x50`, native frames
    // `mcdr_dlss_evaluate+0xba (d3d12_dlss.cpp:1125)` -> `applyDlssToTexture+0x31d` ->
    // `mcdr_dx12EvaluateDlss+0x13`, i.e. the first `mv->GetDesc()`. Reject a dangling or
    // truncated handle HERE instead of dereferencing it: a garbage pointer must degrade to
    // "DLSS skipped this frame", never to a process kill. Reported once, with raw values.
    if (!isMappedReadable(color) || !isMappedReadable(depth) || !isMappedReadable(mv) ||
        !isMappedReadable(out)) {
        if (!s.evalFailWarned) {
            s.evalFailWarned = true;
            s.failure = "invalid texture handle";
            logError("P108: DLSS skipped — a texture handle in mcdr_dlss_input is not a readable "
                     "pointer (color=%p depth=%p mv=%p out=%p). That is a data-plane bug (the JVM "
                     "handed over a stale/invalid handle), not a DLSS failure; DLSS stays enabled "
                     "so the next frame can retry with a valid set.",
                     static_cast<const void*>(color), static_cast<const void*>(depth),
                     static_cast<const void*>(mv), static_cast<const void*>(out));
        }
        return 0;
    }

    // Motion vectors must be in a format DLSS can bind. The velocity pass writes
    // RG16_FLOAT, which is exactly what NGX wants; anything else is copied into an
    // intermediate of the same footprint first.
    ID3D12Resource* mvForDlss = mv;
    const bool mvIsRg16 = (mv->GetDesc().Format == DXGI_FORMAT_R16G16_FLOAT);
    if (!mvIsRg16) {
        const D3D12_RESOURCE_DESC md = mv->GetDesc();
        const bool reuse = (s.mvBuffer != nullptr) && (s.mvBuffer->GetDesc().Width == md.Width) &&
                           (s.mvBuffer->GetDesc().Height == md.Height);
        if (!reuse) {
            // B4: created by the device module (verified/tracked there), released through its
            // deferred-delete path — no CreateCommittedResource in this unit.
            destroyTrackedTexture(s.transitionCtx, &s.mvBufferObject);
            s.mvBuffer = nullptr;
            void* mvObj = nullptr;
            ID3D12Resource* mvRes = nullptr;
            if (!createTrackedTexture(s.transitionCtx, static_cast<UINT>(md.Width), md.Height,
                                      md.Format, md.MipLevels, md.DepthOrArraySize,
                                      md.SampleDesc.Count, 0, &mvObj, &mvRes)) {
                logError("P108: motion-vector intermediate creation failed (fmt=%d); DLSS disabled",
                         static_cast<int>(md.Format));
                s.disabled = true;
                return -1;
            }
            s.mvBufferObject = mvObj;
            s.mvBuffer = mvRes;
            s.mvCopyResourceState = D3D12_RESOURCE_STATE_COMMON;
        }
        mvForDlss = s.mvBuffer;
        transition(mv, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(mvForDlss, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(mvForDlss, mv);
        transition(mvForDlss, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        s.mvCopyResourceState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    // P108: prefer the depth snapshot taken before vanilla cleared the depth buffer. This is
    // what keeps the upscale depth-aware instead of colour-only; without it the depth term is
    // a constant "far" plane. Falls back to the JVM-supplied depth when no snapshot exists.
    ID3D12Resource* depthForDlss = depth;
    if (s.depthSnapshot != nullptr) {
        depthForDlss = s.depthSnapshot;
        if (!s.depthSnapshotUsedLogged) {
            s.depthSnapshotUsedLogged = true;
            logInfo("P108: DLSS depth snapshot in use (%ux%u fmt=%d) — the JVM-supplied depth is "
                    "not passed to NGX",
                    s.depthSnapshotWidth, s.depthSnapshotHeight,
                    static_cast<int>(s.depthSnapshotFormat));
        }
    }

    // All four inputs go to PIXEL_SHADER_RESOURCE (NGX samples them) and the output to
    // RENDER_TARGET (NGX writes it). Done before every Evaluate so the states hold no
    // matter which pass the JVM hands them over from. `depth` is only transitioned when it is
    // the resource actually handed to NGX (the snapshot already sits in PIXEL_SHADER_RESOURCE).
    // P108x ROOT-CAUSE FIX: NGX evaluates DLSS with a COMPUTE dispatch (its output is a UAV), and a
    // texture sitting in PIXEL_SHADER_RESOURCE is not readable from a compute shader. That is an
    // INVALID CALL — and indeed `logDeviceStatus("dlss-evaluate")` reports
    // `GetDeviceRemovedReason=HRESULT 0x887A0001` (DXGI_ERROR_INVALID_CALL) one millisecond after a
    // "successful" evaluate. All three inputs therefore go to NON_PIXEL_SHADER_RESOURCE (the state
    // compute dispatches read from).
    transition(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (depthForDlss == depth) {
        transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    transition(mvForDlss, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // P108: NGX's output must be a UAV resource. The JVM's target is created with
    // ALLOW_RENDER_TARGET only, which NGX rejects with `FAIL_RWFlagMissing` (0xBAD00009), so DLSS
    // has to write into something else.
    //
    // HISTORY — why the JVM now supplies that something (user m11147 follow-up, control-group
    // evidence): our own `CreateCommittedResource` texture turned out to be unwritable. With the
    // IDENTICAL source, call site and command list, copying the colour input into the
    // module-created target read back `mean=17.8/16.5/11.6/255.0`, while the same copy into this
    // unit's private texture read `mean=0.0/0.0/0.0/0.0` — all zero, alpha included — both
    // immediately and on the following frame. So the ad-hoc creation path produces a resource the
    // GPU will not write; the module's `createTexture` path does not.
    //
    // Therefore: if the JVM hands us a UAV texture it created through the module (see
    // `mcdr_dx12SetDlssOutput`), use it and never build a private one. The private texture stays
    // only as a fallback for older JVM builds / older DLL pairings.
    ID3D12Resource* ngxTarget = out;
    // Which of the three destinations the evaluate actually uses. Kept explicit because the old
    // log collapsed "the JVM's UAV texture" and "our private fallback" into one misleading string
    // (both satisfy `ngxTarget != out`), which sent a joint debugging run down the wrong path.
    const char* destKind = "the JVM's target directly";
    if (s.externalOutput != nullptr) {
        const D3D12_RESOURCE_DESC ed = s.externalOutput->GetDesc();
        const bool fits = (ed.Width == static_cast<UINT64>(in->targetWidth)) &&
                          (ed.Height == static_cast<UINT>(in->targetHeight)) &&
                          (ed.Format == out->GetDesc().Format);
        // P108x: the decision inputs, on the record. A joint run showed `override accepted` and then
        // an evaluate that still chose the private fallback with no `cleared`/`does not fit` line —
        // which is only possible if the state went null silently. Printing the inputs (and the
        // state's own address) makes "empty" vs "mismatched" vs "two state copies" distinguishable
        // on sight instead of inferred.
        logInfo("P108x: DLSS output override state: externalOutput=%p fits=%d (need %dx%d fmt=%d, "
                "have %llux%u fmt=%d), state=%p",
                static_cast<void*>(s.externalOutput), fits ? 1 : 0, in->targetWidth, in->targetHeight,
                static_cast<int>(out->GetDesc().Format), static_cast<unsigned long long>(ed.Width),
                ed.Height, static_cast<int>(ed.Format), static_cast<const void*>(&s));
        if (fits) {
            ngxTarget = s.externalOutput;
            destKind = "the JVM-supplied UAV texture";
            transition(out, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(ngxTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } else {
            // Never silent: a mismatch means the copy back would be nonsense, so say so and fall
            // back to the private texture (the old, still-broken-on-this-machine path).
            logError("P108: the JVM-supplied DLSS output texture does not fit the target — "
                     "got %llux%u fmt=%d, need %dx%d fmt=%d; falling back to the private UAV "
                     "texture (which this machine cannot write, so DLSS output will be empty)",
                     static_cast<unsigned long long>(ed.Width), ed.Height,
                     static_cast<int>(ed.Format), in->targetWidth, in->targetHeight,
                     static_cast<int>(out->GetDesc().Format));
        }
    }
    if (ngxTarget == out) {
        ID3D12Resource* priv = createDlssOutput(static_cast<UINT>(in->targetWidth),
                                                static_cast<UINT>(in->targetHeight),
                                                out->GetDesc().Format);
        if (priv != nullptr) {
            ngxTarget = priv;
            destKind = "our private UAV texture (the JVM supplied none)";
            // `out` only has to be a copy destination until the copy below.
            transition(out, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(priv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    // Report where the evaluate writes, so a failing run says exactly what was tried.
    if (!s.evalDestinationLogged) {
        s.evalDestinationLogged = true;
        logInfo("P108: DLSS Evaluate destination: %s (target %ux%u fmt=%d%s)", destKind,
                static_cast<unsigned>(in->targetWidth), static_cast<unsigned>(in->targetHeight),
                static_cast<int>(ngxTarget->GetDesc().Format),
                ngxTarget != out ? "; result copied into the JVM target afterwards" : "");
    }

    NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = color;
    ep.Feature.pInOutput = ngxTarget;
    // P108: 0, not the SDK's "optimal" sharpness. The reference integration sets 0 with the note
    // "sharpening is deprecated in DLSS"; the value we used to pass (0.35 here) is a leftover from
    // the optimal-settings query and is not what a working integration asks for.
    ep.Feature.InSharpness = 0.0f;
    ep.pInDepth = depthForDlss;
    // P108x: state exactly WHICH resource NGX gets as depth, in which **DXGI** format. The frame
    // log's `depthFmt=NN` is the JVM's own format ORDINAL (`mcdr_dlss_input.depthFormatOrdinal`),
    // not a DXGI_FORMAT value, so it cannot be read as "51 = R8G8_SNORM" — this line is the
    // authoritative one. A typeless/DSV view or a format NGX does not accept would explain an
    // evaluate that reports success while producing nothing.
    if (!s.depthInputLogged) {
        s.depthInputLogged = true;
        const D3D12_RESOURCE_DESC dd = depthForDlss->GetDesc();
        const char* dxgiName = "?";
        switch (dd.Format) {
            case DXGI_FORMAT_D32_FLOAT: dxgiName = "D32_FLOAT"; break;
            case DXGI_FORMAT_R32_FLOAT: dxgiName = "R32_FLOAT"; break;
            case DXGI_FORMAT_R32_TYPELESS: dxgiName = "R32_TYPELESS"; break;
            case DXGI_FORMAT_D24_UNORM_S8_UINT: dxgiName = "D24_UNORM_S8_UINT"; break;
            case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: dxgiName = "R24_UNORM_X8_TYPELESS"; break;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: dxgiName = "D32_FLOAT_S8X24_UINT"; break;
            case DXGI_FORMAT_D16_UNORM: dxgiName = "D16_UNORM"; break;
            case DXGI_FORMAT_R16_UNORM: dxgiName = "R16_UNORM"; break;
            default: break;
        }
        logInfo("P108x: DLSS depth input: resource=%p GetDesc().Format=%d (%s) %llux%u, "
                "snapshot_used=%d, ordinal_from_jvm=%d",
                static_cast<void*>(depthForDlss), static_cast<int>(dd.Format), dxgiName,
                static_cast<unsigned long long>(dd.Width), dd.Height,
                (depthForDlss == s.depthSnapshot) ? 1 : 0, in->depthFormatOrdinal);
    }
    ep.pInMotionVectors = mvForDlss;
    // P76f 诊断：把 NGX 真正拿到的那张运动矢量纹理读回来（`DXRMC_P108X_DUMP_MV=1`）。
    //
    // 为什么需要它：用户报告"整片地面只在相机移动时拖影"，而 mv 的每一条上游（深度快照、相机
    // 坐标、逐物体行、平移视差项）在日志里都已经 `ok`。再往下就只能靠"mv 里到底有没有值"这一条
    // 硬事实来二分：全 0 ⇒ 速度 pass 的写入/状态/深度链路有问题；有值 ⇒ 问题在 NGX 消费侧。
    //
    // 采样时机：相机静止时 mv **本来就该** 全 0，所以不能用"开局 8 帧"那套；这里每 300 次
    // evaluate 取一次，覆盖到用户真正走动的时间段。读回本身延迟一帧（模块的 fence），无害。
    if (p108xFlag("DXRMC_P108X_DUMP_MV") && (s.submitCount % 60u) == 0u) {
        dumpResourceStats(mvForDlss, "input-motion-vectors");
    }
    ep.InJitterOffsetX = in->jitterOffsetX;
    ep.InJitterOffsetY = in->jitterOffsetY;
    ep.InRenderSubrectDimensions.Width = static_cast<unsigned>(in->renderWidth);
    ep.InRenderSubrectDimensions.Height = static_cast<unsigned>(in->renderHeight);
    ep.InReset = (in->reset != 0 || s.resetPending) ? 1 : 0;
    ep.InMVScaleX = in->mvScaleX;
    ep.InMVScaleY = in->mvScaleY;
    ep.InPreExposure = (in->preExposure != 0.0f) ? in->preExposure : 1.0f;
    ep.InExposureScale = (in->exposureMode == MCDR_DLSS_EXPOSURE_NONE && in->exposureScale != 0.0f)
                             ? in->exposureScale
                             : 1.0f;
    // No transparency mask, no auto-exposure texture, no subrects: the data plane produces
    // none of them yet (see the ABI header's list of what MCDR does not have).

        // P108x: one-shot WRITE-side bisect. The private UAV reads all-zero after the evaluate even
        // though the read-back waits for the queue to drain (the helper signals a fence value one past
        // its own copy), so "the GPU had not run yet" is ruled out. That leaves two possibilities:
        //   (a) the texture cannot carry the write at all — then this pre-evaluate fill, which lands
        //       in the same texture through the same queue, will read back zero too;
        //   (b) DLSS really is not writing — then this fill reads back NON-zero and the post-evaluate
        //       reading of the same texture stays zero.
        // Filling `ngxTarget` with a copy of the colour input is deliberate: it uses the identical
        // resource the evaluate is about to be handed, and the copy engine instead of a shader.
        // P108x: the write-side bisect + CONTROL copies are DISABLED by default. They copy into the
    // privately created textures from inside the evaluate, and that is the same family of copy that
    // produced #944/#525/#527 — it is a prime suspect for the later DXGI_ERROR_DEVICE_HUNG
    // (0x887A0006, a TDR at present time). Re-enable explicitly with DXRMC_P108X_TESTFILL=1 when the
    // bisect is needed again.
    if (p108xFlag("DXRMC_P108X_TESTFILL") && !s.preEvalFillDone && ngxTarget == s.dlssOutput) {
            s.preEvalFillDone = true;
            transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(ngxTarget, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(ngxTarget, color);
            transition(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(ngxTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const P108xDumpItem pre[] = {
                {ngxTarget, "private-uav-after-testfill"},
            };
            logInfo("P108x: write-side bisect — private UAV filled from the colour input before "
                    "the evaluate; if the line below is non-zero the texture is writable and DLSS "
                    "is the one not writing");
            dumpManyResources(pre, 1);
        // P108x CONTROL: identical source, identical call site, but the destination is the
        // JVM/module-created target instead of a texture we created ourselves. If this reads
        // non-zero while the private texture reads zero, the asymmetry lives in our own texture.
        if (!s.controlFillDone) {
            s.controlFillDone = true;
            transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(out, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(out, color);
            transition(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            const P108xDumpItem ctl[] = {
                {out, "control-module-created-target-filled-from-colour"},
            };
            logInfo("P108x: CONTROL — the same colour input copied into the MODULE-created target "
                    "at the same point; if this is non-zero the copy path works and the private "
                    "texture is the odd one out");
            dumpManyResources(ctl, 1);
        }
    }

    const NVSDK_NGX_Result er = NGX_D3D12_EVALUATE_DLSS_EXT(cmd, s.feature, s.params, &ep);
    if (NVSDK_NGX_FAILED(er)) {
        if (!s.evalFailWarned) {
            s.evalFailWarned = true;
            s.failure = "NGX_D3D12_EVALUATE_DLSS_EXT";
            logError("P108: DLSS Evaluate failed: %s; DLSS disabled for this process "
                     "(the existing linear-blit path stays in effect). Resources that had UAV "
                     "this attempt: %s. Inputs: color=%p depth=%p mv=%p out=%p",
                     ngxResultString(er).c_str(),
                     ngxTarget == out ? "none (JVM target used directly)"
                                      : "the private output texture only",
                     static_cast<const void*>(color), static_cast<const void*>(depth),
                     static_cast<const void*>(mvForDlss), static_cast<const void*>(out));
        }
        s.disabled = true;
        return -1;
    }
    // P108: hand the result back. DLSS wrote into our UAV texture, so copy it into the JVM's
    // target and leave that target in RENDER_TARGET — the GUI render pass attaches it right
    // after this returns (the contract this entry point has always had).
    if (ngxTarget != out) {
        if (p108xFlag("DXRMC_P108X_SKIP_COPY")) {
            if (!s.evalDestinationLogged) {
                s.evalDestinationLogged = true;
                logInfo("P108x: DXRMC_P108X_SKIP_COPY=1 — the DLSS result is deliberately NOT "
                        "copied into the JVM target (bisect: is the copy the problem?)");
            }
        } else if (p108xFlag("DXRMC_P108X_COPY_COLOR")) {
            // Keep the evaluate running (so feature/sizes stay warm) but put the *input*
            // colour into the target instead of the DLSS result. If the screen then shows the
            // world in the top-left 640x360, the copy-back machinery lands and the problem is
            // the DLSS output itself; if the screen stays beige, the copy-back never lands.
            transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION cs{};
            cs.pResource = color;
            cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cs.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION cd{};
            cd.pResource = out;
            cd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cd.SubresourceIndex = 0;
            const D3D12_BOX box{0, 0, 0, static_cast<UINT>(in->renderWidth),
                                static_cast<UINT>(in->renderHeight), 1};
            cmd->CopyTextureRegion(&cd, 0, 0, 0, &cs, &box);
            transition(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(out, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (!s.evalDestinationLogged) {
                s.evalDestinationLogged = true;
                logInfo("P108x: DXRMC_P108X_COPY_COLOR=1 — the *input colour* is copied into the "
                        "JVM target instead of the DLSS result (does the copy-back land?)");
            }
        } else {
            transition(ngxTarget, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyResource(out, ngxTarget);
            transition(ngxTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(out, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        // P108x: one-shot localisation. Formats first (a UNORM/SRGB mismatch makes
        // CopyResource fail SILENTLY and leaves `out` showing whatever was there before —
        // which is exactly the "flat beige world area" symptom), then the destination's
        // pixel statistics. Numbers localise; they never claim how the frame looks.
        if (!s.diagDone) {
            s.diagDone = true;
            logResourceFormat("JVM target (pInOutput)", out);
            logResourceFormat("private UAV texture", ngxTarget);
            logResourceFormat("input color (pInColor)", color);
            logResourceFormat("input depth (pInDepth)", depth);
            logResourceFormat("input motion (pInMotionVectors)", mvForDlss);
            logInfo("P108x: DXRMC_P108X_SKIP_COPY=%d DXRMC_P108X_DUMP=%d",
                    p108xFlag("DXRMC_P108X_SKIP_COPY") ? 1 : 0,
                    p108xFlag("DXRMC_P108X_DUMP") ? 1 : 0);
        }
        // B3: the 7 settled readings. They used to be requested ONCE (guarded by `s.diagDone`)
        // through a queue this unit built for itself. Both halves of that are gone: the readings
        // now come from the module's readback path, which is **one frame late by construction**
        // (the copy is recorded into the still-open list, so its bytes can only be mapped after
        // that list was submitted), and a single round would therefore only ever print
        // "(not ready)". So the same 7 labels are requested for a bounded number of frames
        // (kP108xDumpRounds): round 1 reports "(not ready)" for each texture, round 2 is the
        // first with real numbers, and every later round reads the *previous* frame's settled
        // content — exactly what the label names (`…-settled`, `…-from-previous-frame-settled`)
        // have always claimed. Bounded, not per-frame: this is a diagnostic, not a sampler.
        if (s.dumpRounds < kP108xDumpRounds) {
            ++s.dumpRounds;
            const P108xDumpItem batch[] = {
                {out, "jvm-target-from-previous-frame-settled"},
                {color, "input-color-after-evaluate"},
                {ngxTarget, "private-uav-from-previous-frame-settled"},
                {depthForDlss, "input-depth-after-evaluate"},
                // P108x (parent's item 2): the ACTUAL NGX output, when the JVM supplied it.
                // Read at the settle point — i.e. on a later frame, after the command list that
                // produced it was submitted and fenced — so "NGX wrote / did not write" cannot be
                // explained away as "the GPU had not run yet". This is the one number that settles
                // the black-world question.
                {s.externalOutput, "dlss-output-texture-settled"},
                // P108x (parent's item 2): the snapshot is ALSO one of this unit's own
                // `CreateCommittedResource` textures, i.e. the same class that provably cannot hold a
                // write. Reading it next to the JVM's main depth separates "the snapshot copy never
                // landed" from "the world depth really was empty".
                {s.depthSnapshot, "depth-snapshot-settled"},
                {depth, "jvm-main-depth-settled"},
            };
            dumpManyResources(batch, 7);
        }
        // P108x: same copy-back as above, but placed AFTER the evaluate (i.e. at the very end
        // of our work for the frame). Lets a run compare "what pInColor held when DLSS was
        // asked to upscale it" with the same texture later in the frame, which distinguishes
        // "the hook fires before the world for this frame is drawn" from "the world layer is
        // not in this texture at all".
        if (p108xFlag("DXRMC_P108X_COPY_COLOR_AFTER")) {
            transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION cs{};
            cs.pResource = color;
            cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cs.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION cd{};
            cd.pResource = out;
            cd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cd.SubresourceIndex = 0;
            const D3D12_BOX box{0, 0, 0, static_cast<UINT>(in->renderWidth),
                                static_cast<UINT>(in->renderHeight), 1};
            cmd->CopyTextureRegion(&cd, 0, 0, 0, &cs, &box);
            transition(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(out, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (!s.evalDestinationLogged) {
                s.evalDestinationLogged = true;
                logInfo("P108x: DXRMC_P108X_COPY_COLOR_AFTER=1 — input colour copied into the "
                        "target at the END of our work in the frame");
            }
        }
    } else {
        transition(out, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    s.resetPending = false;

    ++s.frameIndex;
    s.prevPrivateOutput = (ngxTarget != out) ? ngxTarget : nullptr;
    s.prevPrivateFrame = s.frameIndex;
    ++s.submitCount;
    if (!s.evalOk) {
        s.evalOk = true;
        logInfo("P108: DLSS Evaluate succeeded on the first frame: render %dx%d -> target %dx%d "
                "(mv=%s)",
                in->renderWidth, in->renderHeight, in->targetWidth, in->targetHeight,
                mvIsRg16 ? "RG16 straight through" : "copied into an RG16 intermediate");
    }
    tickDlssFps("ngx");
    return 1;
    }

    // Called from destroyDevice() BEFORE the D3D12 device goes away: NGX must release its
    // feature and shut down while the device is still alive.
    // P108 (user m11147): record the tier the user picked. Called by the device module whenever
    // `setDlssQuality` observes a change. A value >= 0 makes `ensureFeature` rebuild the feature on
    // the next frame (PerfQualityValue is a CreateFeature input); -1 restores the "closest to the
    // render:target ratio" behaviour for old DLL/old JVM combinations.
    int32_t mcdr_dlss_request_quality(int32_t tier) {
    const int normalised = (tier < 0) ? -1 : (tier > 3 ? 3 : tier);
    if (normalised != s.requestedQuality) {
        s.requestedQuality = normalised;
        // Allow a rebuild even if a previous create failed, so switching tiers can recover.
        s.createdWarned = false;
        if (!s.qualityRequestLogged) {
            s.qualityRequestLogged = true;
            logInfo("P108: DLSS quality tier request: %d (%s) — the feature will be rebuilt",
                    normalised,
                    normalised < 0 ? "unset -> closest to the render:target ratio"
                                   : (normalised == 0 ? "Performance"
                                                      : (normalised == 1 ? "Balanced"
                                                                         : (normalised == 2
                                                                                ? "Quality"
                                                                                : "DLAA"))));
        }
    }
    return normalised;
    }

    // P108ac (user m08033): the DLSS *model* (NGX render preset). The user's report was that the
    // remaining noise/flicker is inherent to DLSS and that the usual remedy is swapping the model,
    // so this exposes exactly that knob instead of guessing a better default ourselves.
    //
    // `preset` is a NVSDK_NGX_DLSS_Hint_Render_Preset value: 0 = Default (NGX decides per tier),
    // 5 = E and 6 = F (deprecated CNN presets), 10 = J, 11 = K, 12 = L, 13 = M (transformer).
    // Out-of-range values are rejected with -1 and change nothing. A change makes `ensureFeature`
    // rebuild the feature on the next frame, because the preset is a CreateFeature input.
    int32_t mcdr_dlss_request_preset(int32_t preset) {
        if (preset < 0 || preset > NVSDK_NGX_DLSS_Hint_Render_Preset_O) {
            logWarn("P108: DLSS render preset %d is out of range (valid: 0=Default, 5=E, 6=F, "
                    "10=J, 11=K, 12=L, 13=M) — ignored",
                    preset);
            return -1;
        }
        if (preset != s.requestedPreset) {
            s.requestedPreset = preset;
            // Same recovery rule as the tier: a failed create must be retryable by changing the
            // model, otherwise a first-frame failure locks the button out for the whole session.
            s.createdWarned = false;
            if (!s.presetRequestLogged) {
                s.presetRequestLogged = true;
                logInfo("P108: DLSS render preset request: %d (%s) — the feature will be rebuilt",
                        preset, presetName(preset).c_str());
            }
        }
        return preset;
    }

    // P108 (user m11147 follow-up): the JVM hands us the UAV texture it created through the module's
// `createTexture` path, to be used as NGX's output instead of this unit's private texture.
//
// The module does the unwrapping (it is the side that knows `Dx12Object`'s layout) and passes the
// raw `ID3D12Resource*` as an opaque pointer — never reinterpret a JVM handle here, that mistake
// caused the `mv->GetDesc()` null-vtable crash recorded in mcdr_dlss_evaluate.
//
// `resource == nullptr` clears the override and restores the private fallback. The resource is NOT
// owned by this unit; the module keeps the Dx12Object alive. Returns 1=accepted 0=cleared.
int32_t mcdr_dlss_set_output_texture(void* resource) {
    auto* res = static_cast<ID3D12Resource*>(resource);
    if (res == s.externalOutput) {
        return res != nullptr ? 1 : 0;
    }
    s.externalOutput = res;
    if (res != nullptr) {
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        logInfo("P108: DLSS output texture supplied by the JVM: %llux%u fmt=%d flags=0x%X — NGX "
                "will write here instead of this unit's private UAV texture",
                static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(d.Format),
                static_cast<unsigned>(d.Flags));
        // A new destination invalidates the previous frame's assumption about where the result
        // went, so let the next evaluate re-log its destination.
        s.evalDestinationLogged = false;
    } else {
        logInfo("P108: DLSS output texture override cleared; back to the private UAV texture");
    }
    return res != nullptr ? 1 : 0;
}

// P108 (user m10920): the DLSS on/off switch. Independent of the anti-aliasing mode, and
    // deliberately does NOT touch the loaded NGX runtime / feature handle — flipping the switch
    // off and on again must not throw away a successful Init/CreateFeature. The real gate is
    // the device module's applyDlssToTexture (gCtx.dlssEnabled); this mirror feeds the status
    // query and the log.
    int32_t mcdr_dlss_set_enabled(int32_t enabled) {
    const bool want = enabled != 0;
    const bool changed = (want != s.enabled);
    s.enabled = want;
    if (changed) {
        logInfo("P108: DLSS switch %s (independent of anti-aliasing; NGX state is kept: "
                "initOk=%d featureOk=%d)",
                want ? "ON" : "OFF", s.initOk ? 1 : 0, s.featureOk ? 1 : 0);
    }
    return want ? 1 : 0;
    }

    void mcdr_dlss_shutdown(void) {
    destroyFeature();
    // B4: the three private textures are module-owned now. Hand them back **before** the device
    // goes away (ctx = nullptr: no command list to touch, just the deferred delete). They used to
    // be leaked here (`s = State{}` dropped the pointers) or Release()d inline.
    destroyTrackedTexture(nullptr, &s.mvBufferObject);
    s.mvBuffer = nullptr;
    destroyTrackedTexture(nullptr, &s.dlssOutputObject);
    s.dlssOutput = nullptr;
    destroyTrackedTexture(nullptr, &s.depthSnapshotObject);
    s.depthSnapshot = nullptr;
    if (s.depthStencil != nullptr) {
        s.depthStencil->Release();
        s.depthStencil = nullptr;
    }
    if (s.initOk) {
        // Shutdown1(device) 是现行入口；NVSDK_NGX_D3D12_Shutdown() 只在定义了
        // NGX_ENABLE_DEPRECATED_SHUTDOWN 时才声明，本项目没有定义它（编译期就报 C3861）。
        const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Shutdown1(s.device);
        if (NVSDK_NGX_FAILED(r)) {
            logWarn("P108: NVSDK_NGX_D3D12_Shutdown1 failed: %s", ngxResultString(r).c_str());
        }
        s = State{};
    }


}  // mcdr_dlss_shutdown
    }  // extern "C"
