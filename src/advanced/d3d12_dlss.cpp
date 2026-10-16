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
        UINT dlssOutputWidth = 0;
        UINT dlssOutputHeight = 0;
        DXGI_FORMAT dlssOutputFormat = DXGI_FORMAT_UNKNOWN;
        // P108x: one-shot diagnostic state (temporary — used to localise the beige-frame
        // report; the numbers localise, they never claim how the frame looks).
        bool diagDone = false;
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
        void* transitionCtx = nullptr;
        // P108: the private D32 depth snapshot (see mcdr_dlss_snapshot_depth). When present it
        // replaces the JVM's `pInDepth`, which by the time DLSS runs has already been cleared.
        ID3D12Resource* depthSnapshot = nullptr;
        UINT depthSnapshotWidth = 0;
        UINT depthSnapshotHeight = 0;
        DXGI_FORMAT depthSnapshotFormat = DXGI_FORMAT_UNKNOWN;
        UINT64 depthSnapshotFrame = 0;   // frame counter at the last snapshot
        bool depthSnapshotWarned = false;
        bool depthSnapshotUsedLogged = false;

        ID3D12Resource* mvBuffer = nullptr;      // intermediate for a non-RG16 MV input
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
        // `effectiveQuality` is what the live feature was actually built with, so a tier change
        // is detected by comparing against it rather than against `requestedQuality`.
        int requestedQuality = -1;
        int effectiveQuality = -1;
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

    // Builds the DLSS feature flags from the frame description. Each bit is justified:
    //   MVLowRes      0  — the velocity pass runs at the full render resolution
    //   MVJittered    1  — D3D12Velocity.kt projects with the jittered matrix
    //   DepthInverted 1  — reverse-Z (see the file header)
    //   IsHDR         0  — the main render target is an 8-bit UNORM/SRGB target
    //   AutoExposure  0  — no GPU exposure texture exists (see the ABI header)
    // motionVectorJitterCancellation is NOT a create flag: it is set through the
    // parameter map at evaluate time, because NVSDK_NGX_DLSS_Create_Params has no field
    // for it.
    int32_t computeFeatureFlags(const mcdr_dlss_input& in) {
        int32_t flags = 0;
        if (in.depthInverted != 0) {
            flags |= static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_DepthInverted);
        }
        flags |= static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVJittered);
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
    ID3D12Resource* createDlssOutput(UINT width, UINT height, DXGI_FORMAT format) {
        if (s.dlssOutput != nullptr && s.dlssOutputWidth == width && s.dlssOutputHeight == height &&
            s.dlssOutputFormat == format) {
            return s.dlssOutput;
        }
        if (s.dlssOutput != nullptr) {
            s.dlssOutput->Release();
            s.dlssOutput = nullptr;
        }
        s.dlssOutputWidth = 0;
        s.dlssOutputHeight = 0;
        s.dlssOutputFormat = DXGI_FORMAT_UNKNOWN;
        if (width == 0 || height == 0) {
            return nullptr;
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
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
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const HRESULT hr = s.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&s.dlssOutput));
        if (FAILED(hr)) {
            s.dlssOutput = nullptr;
            logWarn("P108: DLSS private UAV output creation failed (%ux%u fmt=%d, hr=0x%08X); the "
                    "existing path stays in effect",
                    width, height, static_cast<int>(format), static_cast<unsigned>(hr));
            return nullptr;
        }
        s.dlssOutputWidth = width;
        s.dlssOutputHeight = height;
        s.dlssOutputFormat = format;
        logInfo("P108: DLSS private UAV output created: %ux%u fmt=%d "
                "(ALLOW_UNORDERED_ACCESS|ALLOW_RENDER_TARGET) — DLSS writes here, then the result "
                "is copied into the JVM's target",
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

    bool ensureInit(ID3D12Device* device) {
        if (s.initAttempted) {
            return s.initOk && s.paramsOk;
        }
        s.initAttempted = true;
        s.device = device;

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
        const bool qualityChanged =
            (s.requestedQuality >= 0) && (s.requestedQuality != s.effectiveQuality);
        if (s.featureOk && !sizeChanged && !qualityChanged) {
            return true;
        }
        if (sizeChanged || qualityChanged) {
            destroyFeature();
            // A new size invalidates the history; a pure tier change does not have to, but the
            // rebuilt feature starts with no history anyway, so reset in both cases.
            s.resetPending = true;
        }
        if (s.params == nullptr) {
            return false;
        }

        if (s.requestedQuality >= 0) {
            // Explicit tier from the JVM: Performance / Balanced / Quality, same order as the
            // Java enum. The mapping is deliberate (0->MaxPerf, 1->Balanced, 2->MaxQuality).
            static const uint32_t kTierToNgx[3] = {
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxPerf),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_Balanced),
                static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_MaxQuality),
            };
            const int tier = (s.requestedQuality <= 2) ? s.requestedQuality : 2;
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
        s.effectiveQuality = static_cast<int>(s.qualityValue);
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
        cp.Feature.InWidth = static_cast<unsigned>(in.renderWidth);
        cp.Feature.InHeight = static_cast<unsigned>(in.renderHeight);
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
        logInfo("P108: DLSS feature created: render %dx%d -> target %dx%d, quality=%u (%s), "
                "flags=0x%X (DepthInverted=%d MVJittered=%d HDR=%d), scratch=%llu bytes",
                in.renderWidth, in.renderHeight, in.targetWidth, in.targetHeight, s.qualityValue,
                s.qualityIsExplicit ? "explicit tier from the JVM" : "closest to the ratio",
                static_cast<unsigned>(s.featureFlags),
                (s.featureFlags &
                 static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_DepthInverted)) != 0
                    ? 1
                    : 0,
                (s.featureFlags & static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVJittered)) != 0
                    ? 1
                    : 0,
                (s.featureFlags & static_cast<int32_t>(NVSDK_NGX_DLSS_Feature_Flags_IsHDR)) != 0 ? 1
                                                                                                : 0,
                static_cast<unsigned long long>(s.scratchBytes));
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
int32_t mcdr_dlss_create(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                         void* tlCtx) {
    if (in == nullptr) {
        return -1;
    }
    if (s.disabled) {
        return -1;
    }
    if (!s.initOk || !s.paramsOk) {
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

// P108x: read a texture back and log its pixel statistics. Localisation only — the numbers
// say whether a texture is empty/uniform/plausible; they never claim how the frame looks.
// Uses its own short-lived DIRECT queue so it does not depend on module state.
// P108x: dump one or more textures in a SINGLE command list + queue + fence.
//
// The previous version closed its command list after the first texture, so a caller asking for
// several readings only ever got the first one — which is exactly why some of the numbers looked
// suspicious. It also assumed 4 bytes per pixel and always emitted raw barriers.
//
// Trust rules (a diagnostic that can lie is worse than no diagnostic):
//   * each read is preceded by an explicit transition FROM the state the caller declares the
//     texture to be in, and that state is restored afterwards;
//   * the copy lands in a READBACK buffer, the queue is signalled and the CPU waits on the fence
//     before mapping, so the GPU has definitely finished writing;
//   * bytes per pixel come from the DXGI format instead of being assumed.
struct P108xDumpItem {
    ID3D12Resource* res;
    const char* name;
    D3D12_RESOURCE_STATES state;
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

void dumpManyResources(const P108xDumpItem* items, int count) {
    if (items == nullptr || count <= 0 || s.device == nullptr) {
        return;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    const bool ok =
        SUCCEEDED(s.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) &&
        SUCCEEDED(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                              IID_PPV_ARGS(&list))) &&
        SUCCEEDED(s.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    if (!ok) {
        logInfo("P108x: dump batch skipped (could not create its own queue/list)");
        if (list != nullptr) list->Release();
        if (alloc != nullptr) alloc->Release();
        if (q != nullptr) q->Release();
        if (fence != nullptr) fence->Release();
        return;
    }
    constexpr int kMaxDumps = 8;
    ID3D12Resource* stagings[kMaxDumps] = {};
    const P108xDumpItem* done[kMaxDumps] = {};
    UINT widths[kMaxDumps] = {};
    UINT heights[kMaxDumps] = {};
    UINT pitches[kMaxDumps] = {};
    UINT bpps[kMaxDumps] = {};
    int staged = 0;
    for (int i = 0; i < count && staged < kMaxDumps; ++i) {
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
        const UINT rowPitch = w * bpp;
        D3D12_HEAP_PROPERTIES rheap{};
        rheap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bdesc{};
        bdesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bdesc.Width = static_cast<UINT64>(rowPitch) * h;
        bdesc.Height = 1;
        bdesc.DepthOrArraySize = 1;
        bdesc.MipLevels = 1;
        bdesc.Format = DXGI_FORMAT_UNKNOWN;
        bdesc.SampleDesc.Count = 1;
        bdesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bdesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* staging = nullptr;
        if (FAILED(s.device->CreateCommittedResource(&rheap, D3D12_HEAP_FLAG_NONE, &bdesc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&staging)))) {
            logInfo("P108x: dump %s skipped (staging alloc failed)", items[i].name);
            continue;
        }
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = staging;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = d.Format;
        dst.PlacedFootprint.Footprint.Width = w;
        dst.PlacedFootprint.Footprint.Height = h;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
        D3D12_RESOURCE_BARRIER rb{};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = res;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        rb.Transition.StateBefore = items[i].state;
        rb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &rb);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER rb2 = rb;
        rb2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        rb2.Transition.StateAfter = items[i].state;
        list->ResourceBarrier(1, &rb2);
        stagings[staged] = staging;
        done[staged] = &items[i];
        widths[staged] = w;
        heights[staged] = h;
        pitches[staged] = rowPitch;
        bpps[staged] = bpp;
        ++staged;
    }
    list->Close();
    ID3D12CommandList* lists[] = {list};
    q->ExecuteCommandLists(1, lists);
    // P108x: signal **2**, not 1. Fence values are monotonic per queue, so waiting on a value one
    // PAST our copy proves that every command submitted to this queue before the signal — our own
    // copy included — has *executed*. Signalling 1 and waiting for 1 would only prove it was
    // *submitted*, and a mapped buffer could still hold stale bytes. This is what makes the
    // reading trustworthy for resources the frame is still producing (the "in-flight" concern).
    q->Signal(fence, 2);
    const HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev != nullptr) {
        if (fence->GetCompletedValue() < 2) {
            fence->SetEventOnCompletion(2, ev);
            ::WaitForSingleObject(ev, 4000);
        }
        ::CloseHandle(ev);
    }
    for (int k = 0; k < staged; ++k) {
        ID3D12Resource* staging = stagings[k];
        const D3D12_RESOURCE_DESC d = done[k]->res->GetDesc();
        const UINT w = widths[k];
        const UINT h = heights[k];
        const UINT rowPitch = pitches[k];
        const UINT bpp = bpps[k];
        const UINT64 total = static_cast<UINT64>(rowPitch) * h;
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
        if (SUCCEEDED(staging->Map(0, &range, &mapped)) && mapped != nullptr) {
            const auto* px = static_cast<const unsigned char*>(mapped);
            double sum[4] = {0, 0, 0, 0};
            unsigned lo[4] = {255, 255, 255, 255};
            unsigned hi[4] = {0, 0, 0, 0};
            const UINT sx = (w > 64) ? w / 64 : 1;
            const UINT sy = (h > 64) ? h / 64 : 1;
            UINT64 n = 0;
            for (UINT y = 0; y < h; y += sy) {
                for (UINT x = 0; x < w; x += sx) {
                    const unsigned char* p = px + static_cast<UINT64>(y) * rowPitch + x * bpp;
                    for (UINT c = 0; c < bpp && c < 4; ++c) {
                        sum[c] += p[c];
                        lo[c] = (p[c] < lo[c]) ? p[c] : lo[c];
                        hi[c] = (p[c] > hi[c]) ? p[c] : hi[c];
                    }
                    ++n;
                }
            }
            if (n > 0) {
                logInfo("P108x: dump %s %ux%u fmt=%d bpp=%u mean=%.1f/%.1f/%.1f/%.1f "
                        "min=%u/%u/%u/%u max=%u/%u/%u/%u (raw bytes)",
                        done[k]->name, w, h, static_cast<int>(d.Format), bpp, sum[0] / n,
                        sum[1] / n, sum[2] / n, sum[3] / n, lo[0], lo[1], lo[2], lo[3], hi[0],
                        hi[1], hi[2], hi[3]);
            }
            staging->Unmap(0, nullptr);
        }
        staging->Release();
    }
    list->Release();
    alloc->Release();
    q->Release();
    fence->Release();
}

// Single-texture convenience wrapper.
void dumpResourceStats(ID3D12Resource* res, const char* name,
                       D3D12_RESOURCE_STATES knownState = D3D12_RESOURCE_STATE_COPY_DEST) {
    P108xDumpItem item{res, name, knownState};
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
    // (Re)create the private snapshot texture when the size/format changed.
    if (s.depthSnapshot == nullptr || s.depthSnapshotWidth != w || s.depthSnapshotHeight != h ||
        s.depthSnapshotFormat != dd.Format) {
        if (s.depthSnapshot != nullptr) {
            s.depthSnapshot->Release();
            s.depthSnapshot = nullptr;
        }
        s.depthSnapshotWidth = 0;
        s.depthSnapshotHeight = 0;
        s.depthSnapshotFormat = DXGI_FORMAT_UNKNOWN;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = dd.Format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;  // only ever COPY_DEST / COPY_SOURCE
        const HRESULT hr = s.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&s.depthSnapshot));
        if (FAILED(hr)) {
            s.depthSnapshot = nullptr;
            if (!s.depthSnapshotWarned) {
                s.depthSnapshotWarned = true;
                logWarn("P108: depth snapshot texture creation failed (%ux%u fmt=%d, hr=0x%08X); "
                        "DLSS falls back to the JVM-supplied depth (which is already cleared by "
                        "then, so the upscale loses its depth term)",
                        w, h, static_cast<int>(dd.Format), static_cast<unsigned>(hr));
            }
            return -1;
        }
        s.depthSnapshotWidth = w;
        s.depthSnapshotHeight = h;
        s.depthSnapshotFormat = dd.Format;
        logInfo("P108: DLSS depth snapshot texture created: %ux%u fmt=%d (private, COPY_DEST)",
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
    tl(tlCtx, depthResource, static_cast<int32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE));
    tl(tlCtx, reinterpret_cast<void*>(s.depthSnapshot),
       static_cast<int32_t>(D3D12_RESOURCE_STATE_COPY_DEST));
    cmd->CopyResource(s.depthSnapshot, depth);
    tl(tlCtx, reinterpret_cast<void*>(s.depthSnapshot),
       static_cast<int32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE));
    tl(tlCtx, depthResource, before);  // hand the depth back exactly as it was found
    ++s.depthSnapshotFrame;
    if (s.depthSnapshotFrame == 1) {
        logInfo("P108: DLSS depth snapshot captured (first): %ux%u fmt=%d from resource %p; it "
                "replaces the JVM's depth at evaluate time",
                w, h, static_cast<int>(dd.Format), depthResource);
    }
    return 1;
}

int32_t mcdr_dlss_evaluate(void* commandList, const mcdr_dlss_input* in, DlssTransitionFn tl,
                           void* tlCtx) {
    if (in == nullptr) {
        return -1;
    }
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
            if (s.mvBuffer != nullptr) {
                s.mvBuffer->Release();
                s.mvBuffer = nullptr;
            }
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc = md;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&s.mvBuffer)))) {
                s.mvBuffer = nullptr;
                logError("P108: motion-vector intermediate creation failed (fmt=%d); DLSS disabled",
                         static_cast<int>(md.Format));
                s.disabled = true;
                return -1;
            }
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
    transition(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (depthForDlss == depth) {
        transition(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    transition(mvForDlss, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // P108: NGX's output must be a UAV resource. The JVM's target is created with
    // ALLOW_RENDER_TARGET only, which NGX rejects with FAIL_RWFlagMissing (0xBAD00009), so
    // DLSS renders into our own UAV texture and we copy the result into `out` right after.
    ID3D12Resource* ngxTarget = out;
    const bool privateOutput = true;
    {
        ID3D12Resource* priv = createDlssOutput(static_cast<UINT>(in->targetWidth),
                                                static_cast<UINT>(in->targetHeight),
                                                out->GetDesc().Format);
        if (priv != nullptr) {
            ngxTarget = priv;
            // `out` only has to be a copy destination until the copy below.
            transition(out, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(priv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    // Report where the evaluate writes, so a failing run says exactly what was tried.
    if (!s.evalDestinationLogged) {
        s.evalDestinationLogged = true;
        logInfo("P108: DLSS Evaluate destination: %s (target %ux%u fmt=%d%s)",
                ngxTarget == out ? "the JVM's target directly" : "private UAV texture",
                static_cast<unsigned>(in->targetWidth), static_cast<unsigned>(in->targetHeight),
                static_cast<int>(ngxTarget->GetDesc().Format),
                privateOutput && ngxTarget != out ? "; result copied into the JVM target afterwards"
                                                  : "");
    }

    NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = color;
    ep.Feature.pInOutput = ngxTarget;
    ep.Feature.InSharpness = s.optimalSharpness;
    ep.pInDepth = depthForDlss;
    ep.pInMotionVectors = mvForDlss;
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
    if (!s.preEvalFillDone && ngxTarget == s.dlssOutput) {
        s.preEvalFillDone = true;
        transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(ngxTarget, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(ngxTarget, color);
        transition(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(ngxTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (p108xFlag("DXRMC_P108X_DUMP")) {
            const P108xDumpItem pre[] = {
                {ngxTarget, "private-uav-after-testfill", D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
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
                {out, "control-module-created-target-filled-from-colour",
                 D3D12_RESOURCE_STATE_COPY_DEST},
            };
            logInfo("P108x: CONTROL — the same colour input copied into the MODULE-created target "
                    "at the same point; if this is non-zero the copy path works and the private "
                    "texture is the odd one out");
            dumpManyResources(ctl, 1);
        }
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
            // P108x: dump the colour input AFTER the evaluate. `color` is back in
            // PIXEL_SHADER_RESOURCE at this point (the evaluate restores it), which is what the
            // helper assumes, and the numbers answer the only question that matters here:
            // does the texture the JVM calls "the world layer" actually contain a world?
            if (p108xFlag("DXRMC_P108X_DUMP")) {
                // SELF-TEST of the helper (parent's item 1): dump `out` immediately after the
                // CopyResource put the final frame into it. If this line is plausible while the
                // private-UAV line below is 0, the helper is fine and the UAV really is empty.
                const P108xDumpItem batch[] = {
                    {out, "jvm-target-from-previous-frame-settled", D3D12_RESOURCE_STATE_COPY_DEST},
                    {color, "input-color-after-evaluate",
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
                    {ngxTarget, "private-uav-from-previous-frame-settled",
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
                    {depthForDlss, "input-depth-after-evaluate",
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
                };
                dumpManyResources(batch, 4);
            }
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
    return 1;
}

// Called from destroyDevice() BEFORE the D3D12 device goes away: NGX must release its
// feature and shut down while the device is still alive.
// P108 (user m11147): record the tier the user picked. Called by the device module whenever
// `setDlssQuality` observes a change. A value >= 0 makes `ensureFeature` rebuild the feature on
// the next frame (PerfQualityValue is a CreateFeature input); -1 restores the "closest to the
// render:target ratio" behaviour for old DLL/old JVM combinations.
int32_t mcdr_dlss_request_quality(int32_t tier) {
    const int normalised = (tier < 0) ? -1 : (tier > 2 ? 2 : tier);
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
                                                      : (normalised == 1 ? "Balanced" : "Quality")));
        }
    }
    return normalised;
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
    if (s.mvBuffer != nullptr) {
        s.mvBuffer->Release();
        s.mvBuffer = nullptr;
    }
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
    }
    s = State{};
}

}  // extern "C"
