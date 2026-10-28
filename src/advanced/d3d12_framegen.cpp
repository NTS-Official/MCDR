// d3d12_framegen.cpp — P293：帧生成（FG）Provider 接口层的实现单元（模块 mcdr.d3d12.device）
//
// 三块内容，与 `d3d12_sr.cpp`（P272）一一对应：
//   1. 能力表（`fillDlssGCaps`）—— 「这家后端能不能用、为什么不能用」的唯一事实来源；
//   2. `DlssGFrameGenProvider` —— DLSS-G 的实现。**P293 是个薄包装**：真正的
//      `slDLSSGSetOptions` 仍然在 `d3d12_dlss.cpp` 的 streamlineEvaluate 里下发，这里只把
//      既有 C 入口（`mcdr_dlss_set_fg_mode` / `mcdr_dlss_fg_presented`）接到接口上。
//      为什么先包装而不是搬：P293/P295 只新增抽象，行为等价性优先 —— 搬实现要同时动
//      渲染线程的 tag 提交顺序，那属于 P296；
//   3. 注册表 —— 期望后端（picked）/ 生效后端（applied）/ 链归属（chainOwner）/ 一行状态文本。
//
// 为什么放在本模块而不是再切一个模块（D15）：provider 要在 Present 两侧插钩子、要能读
// 交换链状态、要解包 `Dx12Object`，这三件事只有 mcdr.d3d12.device 能做 —— 与 P272 把
// `IUpscaleProvider` 留在本模块是同一条理由（`mcdr_device.cppm:1105-1107`）。

module;

#include "_d3d12_module_prelude.h"

// mcdr_fg_caps / mcdr_fg_dispatch / MCDR_FG_* 都在这里。必须放在全局模块片段：模块
// purview 内文本包含头文件会触发 C5244，而本项目 /WX 直接失败。与 d3d12_sr.cpp:20-24
// 同理，**.cppm 全局模块片段里的声明不会传给实现单元**，每个实现单元要自己再 include。
#include "mcdr_abi.h"

// FG 的两个既有 C 入口（`mcdr_dlss_set_fg_mode` / `mcdr_dlss_fg_presented`）与 Reflex 的
// 可用性查询，都在这份头里。P293 不搬它们，只调它们。
#include "mcdr_dlss_impl.h"

module mcdr.d3d12.device;

namespace d3d12_mc {

namespace {

// 把一句话写进固定长度的 note 缓冲（NUL 结尾、超长截断）。不用 snprintf：这里只需要
// 逐字节拷贝，少一个格式化依赖，也不会因为 /analyze 的格式检查多出警告。
void writeNote(char* dst, size_t cap, const char* text) {
    if (dst == nullptr || cap == 0) {
        return;
    }
    size_t i = 0;
    if (text != nullptr) {
        for (; i + 1 < cap && text[i] != '\0'; ++i) {
            dst[i] = text[i];
        }
    }
    dst[i] = '\0';
}

// 注册表状态。期望（picked）与生效（applied）分开，热切换在下一帧帧首完成（P280 同构）。
//
// P296 起这个结构体跨线程：`fgSetMode` 由 JVM 的设置线程调（改一下界面滑条就调），
// `fgBeforePresent`/`fgAfterPresent`/`fgChainRebuildPending` 由渲染线程/主线程每帧调。
// 里面既有 `std::unique_ptr` 又有 `std::string`，裸读裸写就是数据竞争（provider 可能在
// 渲染线程正调 `beforePresent()` 时被设置线程 reset 掉）。所以**每个入口都拿同一把锁**，
// 包括每帧那两条 —— 无竞争时 mutex 约 20ns，相对一次 Present（数百 µs）可忽略。
// 后端还没建成时 `fgBeforePresent` 的重试间隔（帧）。60 fps 下 30 帧约 0.5 s，
// 与 `refreshFgApi()` 对 SL 入口的 500 ms 重试节奏一致。
constexpr uint64_t ENSURE_RETRY_FRAMES = 30;

// 「期望开、可后端一直建不出来」时给一次 warn 的宽限期（帧）。300 帧在 60 fps 下约 5 s ——
// 启动顺序造成的短暂不可用会在这之前自愈，过了这个点还建不出来就是真的不行了。
constexpr uint64_t USABLE_GRACE_FRAMES = 300;

// P300：能力位从「可用」掉到「不可用」连续多少帧才回滚。60 帧在 60 fps 下约 1 s ——
// 一次瞬时抖动（运行库重新解析、SR 后端切换的中间态）不该把后端拆掉。
constexpr uint64_t ROLLBACK_FRAMES = 60;

struct FgRegistry {
    std::mutex mutex;
    std::unique_ptr<IFrameGenProvider> provider;
    int32_t picked = MCDR_FG_BACKEND_NONE;
    // P305ab：自测钩子显式指定过后端时为真，gSetMode 就不再让门面的推测覆盖 picked。
    bool forcedPick = false;
    int32_t applied = MCDR_FG_BACKEND_NONE;
    // P253 之后链的归属是「建链那一刻定的」，provider 改不动它 —— 所以单独存一份，
    // 由 `d3d12_surface.cpp:91` 在工厂升级成功后写进来。
    int32_t chainOwner = MCDR_FG_CHAIN_APP_NATIVE;
    int32_t lastResult = 0;
    std::string lastReason;
    mcdr_fg_constants constants{};
    bool hasConstants = false;
    // P305c：UI 纹理（GUI 合成目标）的 `Dx12Object*`，nullptr = 没有。
    void* uiTexture = nullptr;
    // P306ac：hudless 呈现模式（1 = present 的世界层不含 UI，UI 由 provider 合成）。
    // 与 `uiTexture` 同一条纪律：期望值，provider 建成后回放。
    int32_t uiComposition = 0;
    // P296（D13）：期望的 mode 与「链是在哪个 mode 下建成的」分开记。两者跨过 0 边界
    // （开 → 关 / 关 → 开）时挂一次重建请求，由帧首的 acquireSurface 消费。
    int32_t desiredMode = 0;
    int32_t desiredFrames = 1;
    int32_t chainMode = 0;
    bool rebuildPending = false;
    int32_t rebuilds = 0;
    // P297：三轨帧号。**谁维护、什么时候复位**是契约的一部分，别随手清零 ——
    // FSR-FG / XeSS-FG 都要求交给它们的 present id **严格单调 +1**，一个回绕或跳号
    // 会让它们静默丢帧（或直接拒绝）。
    //   appFrameId     渲染线程每调一次 `fgBeforePresent` +1（= 每次尝试 Present 一帧，
    //                  Present 因为没 blit 被跳过时不会走到这里，也就不会 +1）；
    //   backendFrameId provider 真正向后端提交了一帧时由 provider 调 `fgNoteBackendFrame`
    //                  写进来（DLSS-G 不提交 —— SL 自己管帧 token，所以它恒为 0）；
    //   presentId      每次 `Present` 返回之后 +1，**从 0 开始、跨 Resize 与切后端都不复位**。
    uint64_t appFrameId = 0;
    uint64_t backendFrameId = 0;
    uint64_t presentId = 0;
    // 每 600 帧打一行，验收 A7 就靠它（600 帧严格 +1）。
    uint64_t lastTrackLog = 0;
    // P296 实测缺口（2026-10-27 13:06 冒烟）：启动早期 `mcdr_reflex_available()` 还是 0
    // （slInit / 设备创建晚于 `Options.load` 里的 push），当时建不出后端，而 JVM 只在设置
    // 变更时 push 一次 —— 用户存下来的"帧生成开"会静默失效一整局。所以这里记一个重试时刻，
    // 由 `fgBeforePresent` 每 ENSURE_RETRY_FRAMES 帧再试一次（30 帧 ≈ 60 fps 下 0.5 s）。
    uint64_t ensureTryAt = 0;
    // 「期望开但一直建不出来」只 warn 一次（见 fgAfterPresent）。后端建成后清零，
    // 这样将来热切换失败一次仍然能再报警。
    bool warnedNotUsable = false;
    // P300：后端已建成、但能力位后来掉成不可用的连续帧数（见 fgAfterPresent 的回滚段）。
    uint64_t unavailableFrames = 0;
};

FgRegistry& registry() {
    static FgRegistry s;
    return s;
}

// P304：链归属的一个**无锁快照**。
//
// 为什么需要第二份：`fillCaps` 会被 `fgAfterPresent` 在**持有注册表锁**的情况下调用
// （见那里的 `s.provider->fillCaps(live)`），而包装型后端（FSR-FG / XeSS-FG）判
// `chainCompatible` 时必须知道链是哪一种。若在 `fillCaps` 里调 `fgChainOwner()`，
// `std::mutex` 不可重入 → 每帧死锁一次。所以写入口（`fgNoteChainOwner` /
// `fgNoteChainRebuildDone`）同时更新这个原子量，读入口（`fgChainOwnerFast`）只读它。
//
// 它不是"第二个真相"：注册表里的 `chainOwner` 才是权威，这个原子量是它的镜像，
// 且只在两个写点上更新（都在已持锁的临界区里写）。
std::atomic<int32_t> gChainOwnerSnapshot{MCDR_FG_CHAIN_APP_NATIVE};

bool validBackendId(int32_t backendId) {
    return backendId == MCDR_FG_BACKEND_NONE || backendId == MCDR_FG_BACKEND_DLSSG ||
           backendId == MCDR_FG_BACKEND_FSRFG || backendId == MCDR_FG_BACKEND_XESSFG;
}

// P300：状态**变化**打一行，周期性快照仍由 `[FG]` 那条负责。
//
// 为什么还要这一行：周期快照（600 帧一次）能说明"现在是什么"，说不清"什么时候变的、变了几次"。
// 验收 A6/A11 恰恰要看顺序（先 on 后 off、先回滚后重建），而两次快照之间发生的事在快照里只剩
// 差值。P296b 那个"每帧都在下发"就是靠数下发次数才发现的 —— 那一类问题需要有序事实。
//
// 只在变化时打，所以常态下一局只有几行（用户开关一次、回滚一次各一行），不占日志。
void logStateChange(const FgRegistry& s, const char* what, int32_t before, int32_t after) {
    if (before == after) {
        return;
    }
    dbgLogInfo("[FG] %s %d -> %d at present=%llu (picked=%d applied=%d chain=%d rebuilds=%d)",
               what, before, after, static_cast<unsigned long long>(s.presentId), s.picked, s.applied,
               s.chainMode, s.rebuilds);
}

// ---- DLSS-G 的 provider ----
//
// 行为与 P213 的直连路径**等价**，只是把「什么时候把值交下去」从 setMode 挪到了
// beforePresent：P296 起 setMode 只记录（它可能被 JVM 的设置线程调），真正的
// `mcdr_dlss_set_fg_mode` 在 Present 之前、且只在值变化时调一次。
class DlssGFrameGenProvider final : public IFrameGenProvider {
public:
    FgBackendId backend() const override { return FgBackendId::DlssG; }

    void fillCaps(mcdr_fg_caps& out) const override { fillDlssGCaps(out); }

    bool setMode(const mcdr_fg_dispatch& desc) override {
        // 只记录。P296：下发搬到 beforePresent —— 官方 ProgrammingGuideDLSS_G §6 明确
        // 「选项在**下一次 Present** 生效」，所以在 Present 之前写下去是**保守合法**的：
        // 这一帧的标签/选项已经由渲染线程的 `streamlineEvaluate()` 采集完毕，写下的值由
        // 下一帧的 evaluate 读走（P213 的两个原子量），生效点因此一定不早于「下一次
        // Present」。跨线程调 setMode 也不会落到不确定的那次 Present 上。
        const int32_t frames = desc.numFrames < 1 ? 1 : desc.numFrames;
        desiredMode_ = desc.mode;
        desiredFrames_ = frames;
        return true;
    }

    void beforePresent() override {
        // 注意：这里调的是 DLSS-G 实现单元的「状态写入」入口，不是 `slDLSSGSetOptions`
        // 本身 —— 那条调用留在 `streamlineEvaluate()` 里（P213 的既有顺序，tag 与选项都
        // 用同一个 frame token）。本函数只负责「把期望值交下去」这一步的时机。
        if (!applied_ || appliedMode_ != desiredMode_ || appliedFrames_ != desiredFrames_) {
            mcdr_dlss_set_fg_mode(desiredMode_, desiredFrames_);
            appliedMode_ = desiredMode_;
            appliedFrames_ = desiredFrames_;
            applied_ = true;
        }
    }

    void afterPresent() override {
        // P296 起这里仍然没有必须做的事：插帧发生在 SL 的 Present 内部，我们这一侧
        // 只需要在下一帧读 `presented()`。留空是**有意**的，不是还没写。
    }

    int32_t presented() const override { return mcdr_dlss_fg_presented(); }

    void onResize(int32_t displayWidth, int32_t displayHeight) override {
        // SL 的呈现路径自己处理尺寸（它拿得到 swapchain 的 desc），这里只记下来备诊断。
        static_cast<void>(displayWidth);
        static_cast<void>(displayHeight);
    }

    void shutdown() override {
        // P213e：**必须显式下发 eOff**，否则 SL 还在插帧，界面却已经说关了。
        // P213c：`mcdr_dlss_set_fg_mode(0, …)` 内部会把 presented 复位成 1。
        mcdr_dlss_set_fg_mode(0, desiredFrames_ < 1 ? 1 : desiredFrames_);
        desiredMode_ = 0;
        appliedMode_ = 0;
        applied_ = false;
    }

private:
    // 期望值（setMode 写）与已下发值（beforePresent 写）分开，差一帧也看得出来。
    int32_t desiredMode_ = 0;
    int32_t desiredFrames_ = 1;
    int32_t appliedMode_ = 0;
    int32_t appliedFrames_ = 1;
    bool applied_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// 能力表
// ---------------------------------------------------------------------------

void fillDlssGCaps(mcdr_fg_caps& out) {
    out.backend = MCDR_FG_BACKEND_DLSSG;

    const bool dlssReady = mcdr_dlss_available() != 0;
    const bool streamline = mcdr_dlss_streamline_requested() != 0;
    const bool reflexReady = mcdr_reflex_available() != 0;
    // P300（补 P281 的已知缺口）：DLSS-G 的 HUDless 与 ScalingOutputColor 都来自 SL 的超分输出，
    // 所以超分一旦被 pick 成 FSR/XeSS，插帧就没有输入可拿，SL 会静默不插。界面挡住了这条，
    // 配置文件挡不住（`[sr] backend` 与 `[dlss] framegen` 是两个独立键）—— 所以在这里判。
    // 判据用「用户选的那家」优先，`active` 只作为没选过时的兜底：
    // 比例 100% 时 FSR / XeSS 根本不跑（日志 `xess did not run this frame`），但 DLSS 也已经
    // stand down（`P108: standing down: the SR backend in options.txt is XESS`）—— 按 active
    // 判会把「选了 XESS 但本帧没跑」当成「没超分」然后放行 DLSS-G，那是假的。
    const int32_t srSelected = srSelectedBackend();
    const int32_t srActive = srActiveBackend();
    const int32_t srBackend = srSelected != MCDR_SR_BACKEND_NONE ? srSelected : srActive;
    const bool srIsDlss = srBackend == MCDR_SR_BACKEND_NONE || srBackend == MCDR_SR_BACKEND_DLSS;

    out.runtimeLoaded = dlssReady ? 1 : 0;
    out.deviceSupported = streamline ? 1 : 0;
    // P253 之后「实现是 STREAMLINE ⇒ 建链时一律升级代理工厂」，所以链兼容性不依赖 FG 开关。
    out.chainCompatible = streamline ? 1 : 0;
    // 帧生成依赖 Reflex：SDK 会因为 Reflex 不是 On 而静默禁用插帧（P213）。
    out.needsReflex = 1;
    // DLSS-G 的 HUDless 复用 SR 的输出（P213 用同一次 slSetTagForFrame 提交），
    // 不需要调用方再喂一份；UI 也由 SL 在 Present 路径上合成。
    out.needsUiResource = 0;
    out.needsHudless = 0;
    // SL 自己管被 tag 的资源状态（官方 ProgrammingGuideDLSS_G §5.2），我们不替它转。
    out.transitionsInputs = 0;
    // P225：自己算帧数并 clamp 到 1..5（slDLSSGGetState 的 numFramesToGenerateMax）。
    out.maxGeneratedFrames = 5;
    // chainOwner 由 `fgFillCaps` 在外面填（这里读注册表会和它的锁互相嵌套）。
    out.reserved = 0;
    // P298：DLSS-G 的低延迟方案就是 Reflex（而且不是"可选"—— FG 依赖它，见 needsReflex）。
    out.latencyProvider = MCDR_FG_LATENCY_REFLEX;

    const bool available = dlssReady && streamline && reflexReady && srIsDlss;
    out.available = available ? 1 : 0;

    if (!dlssReady) {
        writeNote(out.note, sizeof(out.note), "DLSS runtime not loaded");
    } else if (!streamline) {
        writeNote(out.note, sizeof(out.note), "not on the Streamline path");
    } else if (!reflexReady) {
        // P298：必须说清"是 Reflex 挡住了"，不能只说 unavailable —— 否则人只会去查 FG 本身。
        // （note 只有 28 B，这句话必须压到 27 字符以内。）
        writeNote(out.note, sizeof(out.note), "needs Reflex, unavailable");
    } else if (!srIsDlss) {
        // 同理：说清"是超分后端不对"，否则人会去查 DLSS-G 本身。
        writeNote(out.note, sizeof(out.note), "needs SR=DLSS");
    } else if (!available) {
        writeNote(out.note, sizeof(out.note), "unavailable");
    } else {
        writeNote(out.note, sizeof(out.note), "DLSS-G ready");
    }
}

std::unique_ptr<IFrameGenProvider> makeDlssGProvider() {
    return std::make_unique<DlssGFrameGenProvider>();
}

// ---------------------------------------------------------------------------
// 注册表
// ---------------------------------------------------------------------------

int32_t fgPickBackend(int32_t backendId) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (!validBackendId(backendId)) {
        s.lastReason = "unknown frame generation backend id";
        return 0;
    }
    // 纯记录：不建、不销毁。真要换后端时，下一次 fgEnsureProvider 会按 picked 重建。
    // P306w：链已经被某一家的逻辑接管时不允许改 `picked` —— 代理链的上下文绑在旧 provider
    // 身上，`picked` 一改，后面所有"按 picked 判断"的地方（重建策略、caps、状态文本）与真实
    // 链归属就对不上了；自测里"启动时是 FSR、tick 20 又 pick XeSS"正是这种状态，实测会在
    // 几秒后崩在 SDK 的线程里。产品语义：一次会话只用一个接管链的后端，换后端要重启。
    if (s.provider != nullptr && s.applied != backendId &&
        (s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP || s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP)) {
        s.lastReason = "restart to switch FG";
        return 0;
    }
    s.picked = backendId;
    s.forcedPick = true;
    return 1;
}

void fgFillCaps(int32_t backendId, mcdr_fg_caps& out) {
    // 清零 + 填 backend 由这里负责，实现只写自己的字段（与 srFillCaps 的分工一致）。
    out = mcdr_fg_caps{};
    out.backend = backendId;
    switch (backendId) {
        case MCDR_FG_BACKEND_DLSSG:
            fillDlssGCaps(out);
            break;
        case MCDR_FG_BACKEND_FSRFG:
            // P304：实现挪进 `src\advanced\d3d12_fsffg.cpp`（FFX 的类型只在那一个单元里）。
            // P298 那条「架构上有 Anti-Lag 2、运行时没有 DLL」的结论保留在那份实现里。
            fillFsrFgCaps(out);
            break;
        case MCDR_FG_BACKEND_XESSFG:
            // P304：实现挪进 `src\advanced\d3d12_xessfg.cpp`（XeSS-FG 的类型只在那一个单元里）。
            fillXessFgCaps(out);
            break;
        default:
            out.latencyProvider = MCDR_FG_LATENCY_NONE;
            writeNote(out.note, sizeof(out.note), "no frame generation backend");
            break;
    }
    // 链的归属是全局状态、不是某个后端的能力，所以统一在这里补。
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    out.chainOwner = s.chainOwner;
    // 期望 ≠ 生效时用 lastReason **替换** note：否则用户在视频设置里点了开关、实际什么都没发生，
    // 而 UI 只显示一句静态的 "ready"。note 只有 28 字节，所以理由串都写成短句。
    //
    // P306p：包装型后端还要多看一眼链 —— provider 建成（applied==picked）不等于链已经包好。
    // 重建失败（典型是独占全屏）时 provider 在、链没换，`picked==applied` 会让这里把失败理由
    // 咽掉，UI 继续显示 "ready"，用户点了开关却没有任何插帧。所以把"链家族是否已满足"也算进来。
    const bool wrapPicked = s.picked == MCDR_FG_BACKEND_FSRFG || s.picked == MCDR_FG_BACKEND_XESSFG;
    const bool chainSatisfied =
        !wrapPicked ||
        (s.picked == MCDR_FG_BACKEND_FSRFG && s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP) ||
        (s.picked == MCDR_FG_BACKEND_XESSFG && s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP);
    if (s.picked != MCDR_FG_BACKEND_NONE && !s.lastReason.empty() &&
        (!chainSatisfied || s.applied != s.picked)) {
        writeNote(out.note, sizeof(out.note), s.lastReason.c_str());
    }
}

// 调用方必须已持锁（`fgEnsureProvider` 与 `fgSetMode` 都会走到这里，别在这里再锁一次，
// std::mutex 不可重入）。
// P306m：注册表里的 uiTexture 存的是 Dx12Object* 句柄，provider 只认 ID3D12Resource*，
// 解包函数定义在文件后部（fgSetUiTexture 旁），这里先声明。
ID3D12Resource* unwrapDx12Resource(void* object);
int32_t ensureProviderLocked(FgRegistry& s) {
    const int32_t target = s.picked;
    if (target == MCDR_FG_BACKEND_NONE) {
        s.lastReason = "no frame generation backend picked";
        return 0;
    }
    if (s.provider != nullptr && s.applied == target) {
        return 1;
    }
    // 链已经被另一家接管时**不允许热切换**。代理链的引用在 surface 手里：旧 provider 一销毁，
    // 到下一帧 acquire 之前那一次 Present 就会走一条已经没有上下文的链（实测 FFX 侧返回
    // `FFX_API_RETURN_ERROR_RUNTIME_ERROR` 0x3，或直接崩在 SDK 里）。
    // 产品语义因此定为：一次会话只用一个"接管交换链"的后端，换后端要重启游戏。
    // 下面那段先 unwrap 再 shutdown 的代码仍然保留 —— 它服务于"同一后端重建链"（resize）路径。
    if (s.provider != nullptr && s.applied != target &&
        (s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP || s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP)) {
        s.lastReason = "restart to switch FG";
        return 0;
    }
    // 换后端：先让旧 provider 自己收尾（它会显式下发 eOff）。**不等 GPU**：DLSS-G 的
    // shutdown 只写 atomic 与 SL 选项，不销毁设备资源；真要等的时候由
    // fgReleaseProviderWithWait 负责（P296 接 Present 之后才有意义）。
    if (s.provider != nullptr) {
        // P306：换一家"接管交换链"的后端之前必须先解包。旧 provider 的 SDK context
        // 绑在代理链上，不解包就 shutdown，代理链会留在 `IDXGISwapChain` 里；
        // 新后端再包一次 = 双重包装，实测 FFX 直接返回
        // `FFX_API_RETURN_ERROR_RUNTIME_ERROR` (0x3)。
        s.provider->unwrapSwapChain();
        s.provider->shutdown();
        s.provider.reset();
        s.applied = MCDR_FG_BACKEND_NONE;
        // 链的归属已经变了：让 surface 在下一帧首重建回应用原生链，新后端在那里再包。
        s.rebuildPending = true;
        s.chainMode = 0;
    }

    std::unique_ptr<IFrameGenProvider> made;
    switch (target) {
        case MCDR_FG_BACKEND_DLSSG:
            made = makeDlssGProvider();
            break;
        case MCDR_FG_BACKEND_FSRFG:
            made = makeFsrFgProvider();
            break;
        case MCDR_FG_BACKEND_XESSFG:
            made = makeXessFgProvider();
            break;
        default:
            s.lastReason = "unknown frame generation backend";
            return 0;
    }
    if (made == nullptr) {
        s.lastReason = "frame generation backend factory returned null";
        return 0;
    }

    // 先问一次能力：不可用就别把它当成生效后端（否则 UI 会以为已经开着）。
    mcdr_fg_caps caps = mcdr_fg_caps{};
    made->fillCaps(caps);
    if (caps.available == 0) {
        // P306：自测/诊断用 `fg-backend-*` 强制选定时允许越过 available —— 否则实验性
        // 后端连 provider 都建不出来，wrap 永远不触发。产品路径（非强制）仍然拒绝，
        // 这样配置/UI 里选不到未完成的后端，也就不会把玩家带进一个会 abort 的路径。
        //
        // P306r：拒绝必须留痕。以前这里只写 `lastReason`，用户报"开了没生效"时日志里
        // 一行都没有，只能靠猜。同一条原因只打一次（重试是每 30 帧一次）。
        if (!s.forcedPick) {
            if (s.lastReason != caps.note) {
                dbgLogWarn("provider %d unavailable: %s", target, caps.note);
            }
            s.lastReason = caps.note;
            return 0;
        }
        s.lastReason = caps.note;
    }

    s.applied = target;
    s.provider = std::move(made);
    // P305al：JVM 在启动早期就把 UI 纹理与帧常量推过来了（那时还没有 provider，注册表
    // 只把它们存进 `uiTexture` / `constants`）。新建的 provider 是白纸，必须回放一次，
    // 否则 UI 合成永远拿不到纹理（实测 breadcrumb 里 `ui=null`），tag 出去的 HUDless
    // 帧也就没人合成 HUD。
    if (s.hasConstants) {
        s.provider->setFrameConstants(s.constants);
    }
    if (s.uiTexture != nullptr) {
        // P306m：`s.uiTexture` 存的是 `Dx12Object*` 句柄，provider 要资源指针。
        s.provider->setUiTexture(unwrapDx12Resource(s.uiTexture));
    }
    // P306ac：hudless 呈现模式也是"期望状态"，新建的 provider 默认是关的，必须回放。
    // 顺序在 `setUiTexture` 之后：先有纹理、再开模式，provider 才可能在同一帧里注册 UI。
    if (s.uiComposition != 0) {
        s.provider->setUiComposition(s.uiComposition);
    }
    // P306u：provider 可能是**事后**才建出来的 —— 开机那一帧帧生成运行库还没装载、或链重建
    // 那一帧才第一次真的需要它。注册表里记着用户的期望状态，这里必须补一次 `setMode`：
    // provider 是白纸，默认就是"关"，不补的话会一直停在关着的状态。
    //
    // 实测症状（2026-10-28 14:08，用户实机日志）：`app_frames=600 swapchain_presents=0
    // ratio=0.00 enabled=0 prepared=0 frame_id=1` —— 帧号停在 wrap 时那一次 Configure(off)
    // 的 1，说明 Configure(enabled) 从未下发；FFX 的 `interpolationEnabled` 也就一直是
    // false，`Present` 里根本不会走插帧分支（`FrameInterpolationSwapchainDX12.cpp:2169`）。
    if (s.desiredMode != 0) {
        mcdr_fg_dispatch replay{};
        replay.backend = target;
        replay.mode = s.desiredMode;
        replay.numFrames = s.desiredFrames;
        replay.reset = 1;
        s.provider->setMode(replay);
    }
    // 建成之后「建不出来」那条 warn 的资格要还回去，否则一次迟到的建成会让后续真故障静默。
    s.warnedNotUsable = false;
    return 1;
}

int32_t fgEnsureProvider(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    return ensureProviderLocked(s);
}

int32_t fgSetMode(const mcdr_fg_dispatch& desc) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (desc.backend != MCDR_FG_BACKEND_NONE) {
        if (!validBackendId(desc.backend)) {
            return -1;
        }
        // P304：picked 真的换了后端时，旧 provider 必须换掉 —— DLSS-G 走 SL 代理链、
        // FSR-FG / XeSS-FG 走应用原生链并被 SDK 包装，两家的资源契约与链要求都不同。
        // 这里只拆旧的、不建新的：新的由下面 `ensureProviderLocked` 建，链重建统一在帧首做。
        if (s.provider != nullptr && s.applied != desc.backend) {
            s.provider->shutdown();
            s.provider.reset();
            s.applied = MCDR_FG_BACKEND_NONE;
            s.ensureTryAt = 0;
            s.rebuildPending = true;
            s.lastReason.clear();
        }
        // P305ab：显式指定优先。门面在 XeSS-FG/FSR-FG 的 available=0 时会把 backend 算成 DLSS-G，
        // 若让它无条件覆盖 picked，自测钩子强制选中的后端在 FG 打开的瞬间就被抹掉（P305aa）。
        if (!s.forcedPick) {
            s.picked = desc.backend;
        } else if (s.picked != desc.backend) {
            s.lastReason = "picked backend is forced; ignoring the facade's suggestion";
        }
    }
    if (desc.mode < 0 || desc.mode > 2) {
        return -1;
    }
    // 先把「用户要什么」记下来，再去试图满足它。P296 实测（2026-10-27 13:06）：启动早期
    // Reflex 还没解析出来、能力位是 0，如果这里直接失败并把期望值丢掉，整局都不会有人重试，
    // 用户存的「帧生成开」就等于没生效（日志里只有一行 warn，界面还把 FG 显示成开）。
    const int32_t beforeMode = s.desiredMode;
    const int32_t beforeChainMode = s.chainMode;
    s.desiredMode = desc.mode;
    s.desiredFrames = desc.numFrames < 1 ? 1 : desc.numFrames;
    logStateChange(s, "desiredMode", beforeMode, s.desiredMode);
    // mode=0 是"关"：即使现在没有生效后端也必须成功，否则界面关不掉一个半建成的后端。
    if (desc.mode == 0 && s.provider == nullptr) {
        s.lastResult = 0;
        // 从没建过后端就等于"链一直按关的状态建"，不存在要重建的差异。
        s.chainMode = 0;
        s.rebuildPending = false;
        logStateChange(s, "chainMode", beforeChainMode, s.chainMode);
        return 1;
    }
    if (s.provider == nullptr && ensureProviderLocked(s) == 0) {
        s.lastResult = 0;
        // 不挂重建请求：后端都还没建成，没有链要改。`fgBeforePresent` 会按 ensureTryAt 重试，
        // 成功的那一刻再挂重建（那时才真的要从"关的链"换成"开的链"）。
        s.ensureTryAt = s.appFrameId + ENSURE_RETRY_FRAMES;
        return 0;
    }
    const bool ok = s.provider->setMode(desc);
    s.lastResult = ok ? 1 : 0;
    if (!ok) {
        return s.lastResult;
    }
    // P305af：链**只在家族不符时**重建，开关本身不再是重建理由。
    //
    // 旧规则是「期望开/关 != 链建成时的开/关 ⇒ 重建」，对 DLSS-G（走 SL 代理工厂、链本身
    // 不换）只是多一次开销，对 XeSS-FG / FSR-FG 却是每次切开关都 destroy + create 一条
    // HWND 链。P305ae 实测（2026-10-27 21:12）：连续 4 次重建都成功，第 5 次
    // `CreateSwapChainForHwnd` 直接 0x887A0001（DXGI_ERROR_INVALID_CALL）——同一条 HWND
    // 上的旧链销毁是异步的，短时间内反复易主就踩到；用户真实操作里"关掉再开"就会撞上。
    //
    // 包装链在关掉帧生成之后**仍然是合法链**：SDK 支持整局保持包装，只把插帧关掉
    // （`SetEnabled(0)`，见 `xessfg` 的 provider），ResizeBuffers 也由代理链自己转给
    // 底层链。所以唯一必须重建的情形是：需要一个包装链而现在不是（首次打开），或者
    // 现在包着 A 家而后端换成了 B 家 / 不再需要包装（切 DLSS-G、切回应用原生链）。
    const bool wantOn = s.desiredMode != 0;
    const bool wantsWrap = s.picked == MCDR_FG_BACKEND_FSRFG || s.picked == MCDR_FG_BACKEND_XESSFG;
    const bool chainIsWrap = s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP ||
                             s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP;
    const bool chainFamilyMatches =
        (s.picked == MCDR_FG_BACKEND_FSRFG && s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP) ||
        (s.picked == MCDR_FG_BACKEND_XESSFG && s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP);
    const bool needWrap = wantOn && wantsWrap && (!chainIsWrap || !chainFamilyMatches);
    // 关掉帧生成时**不**解包（见上）；只有后端已经不是包装家族了才解包，否则包着的链会
    // 挡住 DLSS-G 的 SL 代理链 / 用户想要的"关到最原始状态"。
    const bool needUnwrap = chainIsWrap && !wantsWrap;
    if (needWrap || needUnwrap) {
        s.rebuildPending = true;
        s.lastReason.clear();
    }
    return s.lastResult;
}

bool fgChainRebuildPending(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.rebuildPending;
}

void fgNoteChainRebuildDone(int32_t ownerAfterRebuild, int32_t ok, const char* reason) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // 无论成功失败都撤销请求：失败时重挂请求会在每帧 acquire 里反复尝试同一次注定失败
    // 的重建（例如独占全屏），那是死循环；等下次用户改开关自然会再挂一次。
    s.rebuildPending = false;
    if (ok == 0) {
        if (reason != nullptr && reason[0] != '\0') {
            s.lastReason = reason;
        }
        return;
    }
    const int32_t beforeChainMode = s.chainMode;
    s.chainMode = s.desiredMode;
    ++s.rebuilds;
    if (ownerAfterRebuild >= MCDR_FG_CHAIN_APP_NATIVE && ownerAfterRebuild <= MCDR_FG_CHAIN_XESS_WRAP) {
        s.chainOwner = ownerAfterRebuild;
        gChainOwnerSnapshot.store(ownerAfterRebuild, std::memory_order_relaxed);
    }
    logStateChange(s, "chainMode", beforeChainMode, s.chainMode);
}

void fgAbortWrapAfterFailure(const char* reason) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // P306af 实测（2026-10-28 19:51）：从 FSR 包链热切到 XeSS 时，`InitFromSwapChain` 会在
    // 同一个 HWND 上建它自己的链，而旧链刚被放开、DWM 还没交还 HWND，于是
    // `sdk[3] Failed to create swap chain: 0x887a0001`。wrap 失败之后应用链已经没了
    // （`wrapChainForFrameGen` 里 Detach 过），如果上层只是 return 错误，surface 就再也
    // present 不出去 —— 用户看到"游戏还在跑、画面不动"。
    //
    // 这里把状态退回"没有帧生成"：期望清零、链归属记回原生、provider 拆掉，再把原因写进
    // `lastReason`（界面读它）。调用方随后按"不需要包装"重建一条纯应用链。
    if (s.provider != nullptr) {
        s.provider->shutdown();
        s.provider.reset();
    }
    s.applied = MCDR_FG_BACKEND_NONE;
    s.picked = MCDR_FG_BACKEND_NONE;
    s.chainOwner = MCDR_FG_CHAIN_APP_NATIVE;
    gChainOwnerSnapshot.store(MCDR_FG_CHAIN_APP_NATIVE, std::memory_order_relaxed);
    const int32_t beforeMode = s.desiredMode;
    s.desiredMode = 0;
    s.chainMode = 0;
    s.rebuildPending = false;
    s.lastResult = 0;
    if (reason != nullptr && reason[0] != '\0') {
        s.lastReason = reason;
    }
    logStateChange(s, "desiredMode", beforeMode, s.desiredMode);
    dbgLogWarn("%s", reason == nullptr ? "frame generation could not take the swap chain" : reason);
}

void fgFrameStart(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // P305ah：帧首钩子（XeSS-FG 的 XeLL marker 需要它）。不加锁做别的事：这里只在
    // `acquireSurface` 里被调一次，provider 的 frameStart() 只推进一个帧号 + 发一条
    // marker，都是几十纳秒级的操作。
    if (s.provider != nullptr) {
        s.provider->frameStart();
    }
}

void fgBeforePresent(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // P297：appFrameId 在**进 provider 之前**加，这样 provider 在 beforePresent 里看到的
    // 就是本帧的号（顺序反了会差一帧，将来 FSR/XeSS-FG 对不齐 present id 时很难查）。
    ++s.appFrameId;
    // P296 实测缺口（2026-10-27 13:06）：期望是"开"但后端还没建成（启动早期 Reflex 尚未解析、
    // 或运行库比插件晚加载）时，按 ENSURE_RETRY_FRAMES 的节奏重试。成功后立刻把期望的 mode 下发，
    // 并挂一次链重建 —— 因为现在的链是"按关的状态"建的。
    if (s.provider == nullptr && s.desiredMode != 0 && s.appFrameId >= s.ensureTryAt) {
        s.ensureTryAt = s.appFrameId + ENSURE_RETRY_FRAMES;
        if (ensureProviderLocked(s) == 1) {
            mcdr_fg_dispatch d{};
            d.backend = s.picked;
            d.mode = s.desiredMode;
            d.numFrames = s.desiredFrames;
            (void)s.provider->setMode(d);
            // P305af：只在"确实还缺包装链"时挂重建（策略见 fgSetMode 末尾）。
            const bool wantsWrap = s.picked == MCDR_FG_BACKEND_FSRFG || s.picked == MCDR_FG_BACKEND_XESSFG;
            const bool chainIsWrap = s.chainOwner == MCDR_FG_CHAIN_FSR_WRAP ||
                                     s.chainOwner == MCDR_FG_CHAIN_XESS_WRAP;
            if (wantsWrap && !chainIsWrap) {
                s.rebuildPending = true;
            }
            dbgLogInfo("frame generation became usable lazily: backend=%d mode=%d frames=%d "
                       "(provider created on the render thread, chain rebuild queued)",
                       s.picked, s.desiredMode, s.desiredFrames);
        }
    }
    if (s.provider != nullptr) {
        s.provider->beforePresent();
    }
}

void fgAfterPresent(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // P297：只有真的 Present 过才进这里（`presentSurface` 在没 blit 时提前 return），
    // 所以 presentId 天然等于「真正提交的帧数」，FSR/XeSS-FG 要的就是这个语义。
    ++s.presentId;
    if (s.provider != nullptr) {
        s.provider->afterPresent();
    }
    // P300：能力位从 1 掉到 0 时回滚。典型触发是用户把 SR 后端从 DLSS 换成 FSR/XeSS ——
    // DLSS-G 的 `ScalingOutputColor` 只能来自 SL 的 SR 输出，换掉之后这个 provider 再也拿不到
    // 合法输入（P281 记的互斥只在界面上挡住了，配置文件挡不住）。回滚 = 关掉它、清掉期望值，
    // 并挂一次链重建回到「关」的链；不这么做就是让一个注定失败的 provider 每帧继续折腾。
    // 连续 ROLLBACK_FRAMES 帧都不行才动手；`fillCaps` 每帧问一次是可以的（这一层只读几个原子量
    // 与运行库句柄），将来某家的 caps 查询变贵就该在它自己的 `fillCaps` 里缓存，而不是在这里省。
    if (s.provider != nullptr && s.desiredMode != 0) {
        mcdr_fg_caps live{};
        s.provider->fillCaps(live);
        if (live.available != 0) {
            s.unavailableFrames = 0;
        } else if (++s.unavailableFrames >= ROLLBACK_FRAMES) {
            const std::string why = live.note[0] != '\0' ? std::string(live.note)
                                                         : std::string("backend reports unavailable");
            s.provider->shutdown();
            s.provider.reset();
            s.applied = MCDR_FG_BACKEND_NONE;
            s.desiredMode = 0;
            s.desiredFrames = 1;
            s.lastReason = why;
            s.unavailableFrames = 0;
            // 这条 warn 已经把原因说清楚了，别让 300 帧后那条「建不出来」再重复报一次。
            s.warnedNotUsable = true;
            if (s.chainMode != 0) {
                s.rebuildPending = true;
            }
            dbgLogWarn("frame generation rolled back after %llu frames unavailable (%s); "
                       "chain rebuild queued so the swapchain returns to the native path",
                       static_cast<unsigned long long>(ROLLBACK_FRAMES), why.c_str());
        }
    }
    if (s.presentId - s.lastTrackLog >= 600) {
        s.lastTrackLog = s.presentId;
        dbgLogInfo("fg tracks: app=%llu backend=%llu present=%llu chainOwner=%d mode=%d/%d",
                   static_cast<unsigned long long>(s.appFrameId),
                   static_cast<unsigned long long>(s.backendFrameId),
                   static_cast<unsigned long long>(s.presentId), s.chainOwner, s.desiredMode, s.chainMode);
        // §3.8 的一半：能力位与生效状态一行打全，验收时只看这行就够判断"为什么没插帧"。
        // 为什么没有 cost（CPU 耗时）：这两点在 Present 两侧，中间的 `Present()` 会等 vsync，
        // 测出来的差值是被呈现节流主导的，不是插帧开销。要测真开销得走 GPU timestamp（未做）。
        mcdr_fg_caps caps{};
        if (s.provider != nullptr) {
            s.provider->fillCaps(caps);
        } else if (s.picked == MCDR_FG_BACKEND_FSRFG) {
            fillFsrFgCaps(caps);
        } else if (s.picked == MCDR_FG_BACKEND_XESSFG) {
            fillXessFgCaps(caps);
        } else {
            // P296 冒烟实测发现的缺口（2026-10-27 13:00）：帧生成关着的会话里 provider 从来没建过，
            // 于是这一行把 caps 全打成 0（`backend=0 available=0 maxGen=0`），恰好是最需要它说人话的时候
            // 什么都看不出来。改成"没 provider 也回答如果现在开会是什么结果" —— 这几个 fillXxxCaps 都是
            // 自由函数、不碰注册表、不持锁（本函数已持 s.mutex，调 `fgFillCaps` 会死锁）。
            fillDlssGCaps(caps);
        }
        caps.chainOwner = s.chainOwner;
        dbgLogInfo("[FG] backend=%d available=%d runtime=%d device=%d chainCompat=%d needsReflex=%d "
                   "maxGen=%d latency=%d transitions=%d chain=%d rebuilds=%d active=%d note=\"%s\"",
                   caps.backend, caps.available, caps.runtimeLoaded, caps.deviceSupported,
                   caps.chainCompatible, caps.needsReflex, caps.maxGeneratedFrames,
                   caps.latencyProvider, caps.transitionsInputs, s.chainOwner, s.rebuilds,
                   s.provider != nullptr ? 1 : 0, caps.note);
    }
    // P296 实测缺口（2026-10-27 13:06）：期望是"开"、可后端一直建不出来时，用户看到的只是
    // "没有插帧" —— 给它一次说人话的机会。等 300 帧（60 fps 下约 5 s）再说，是因为启动顺序
    // 造成的短暂不可用（Reflex 还没解析、运行库晚于插件加载）是**正常现象**，`fgBeforePresent`
    // 会每 ENSURE_RETRY_FRAMES 重试一次，这时候报警只会制造假故障（JVM 侧原来那条 warn 就是
    // 这么来的）。只在第一次给一个 warn，之后仍由 `[FG]` 行的 available/active/note 持续暴露。
    if (s.desiredMode != 0 && s.provider == nullptr && !s.warnedNotUsable && s.presentId > USABLE_GRACE_FRAMES) {
        s.warnedNotUsable = true;
        mcdr_fg_caps caps{};
        if (s.picked == MCDR_FG_BACKEND_FSRFG) {
            fillFsrFgCaps(caps);
        } else if (s.picked == MCDR_FG_BACKEND_XESSFG) {
            fillXessFgCaps(caps);
        } else {
            fillDlssGCaps(caps);
        }
        dbgLogWarn("frame generation requested (mode=%d frames=%d) but no provider after %llu frames: %s",
                   s.desiredMode, s.desiredFrames, static_cast<unsigned long long>(s.presentId),
                   s.lastReason.empty() ? caps.note : s.lastReason.c_str());
    }
}

void fgNoteBackendFrame(uint64_t backendFrameId) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.backendFrameId = backendFrameId;
}

int32_t fgPresented(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.provider != nullptr ? s.provider->presented() : 1;
}

void fgOnResize(int32_t displayWidth, int32_t displayHeight) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.provider != nullptr) {
        s.provider->onResize(displayWidth, displayHeight);
    }
}

void fgReleaseProviderWithWait(CommandContext* ctx) {
    // P293 的 DLSS-G provider 不持有设备资源，所以这里没有"等 GPU 追平"的必要；
    // 语义上仍然提供这个入口，等 P296 把插帧资源搬进 provider 之后在这里加
    // waitForFenceValue(ctx, ctx->fenceValue, 5s)（P280 的教训：**只等已提交的值，不等 +1**）。
    static_cast<void>(ctx);
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.provider != nullptr) {
        // P306y：必须先让 provider 自己解包（包装型后端在 unwrap 里销毁自己的 SDK context），
        // 再 shutdown。只调 shutdown 会把 xefg/ffx 的 context 泄漏在进程里 —— 下一个 provider
        // 再 `CreateContext` 时 SDK 侧还留着上一条链的 latency-reduction/swapchain 状态，
        // 表现为 `SetEnabled(1)` 返回成功、`GetLastPresentStatus` 却一直报 disabled（实测 1:1 出帧）。
        s.provider->unwrapSwapChain();
        s.provider->shutdown();
        s.provider.reset();
    }
    s.applied = MCDR_FG_BACKEND_NONE;
    // P306x：**不要清 `picked` / `desiredMode`**。这个入口的语义是"surface/设备要重建了"，
    // 不是"用户把帧生成关掉了"。以前在这里把 picked 清成 NONE，后果是：窗口重建（启动早期
    // 常见）之后 FG 永久消失，而界面上还显示着开 —— 实测日志里启动时 XeSS 已经包上了链，
    // 第二次 `configureSurface` 之后 provider 被 release，picked 变 NONE，新链再也没被包过，
    // 而 `chainOwner` 还停在 XESS_WRAP，于是 `[FG] ... chainOwner=3` 看着像好的、实际 1:1 出帧。
    // 保留意图 + 把链归属复位成"应用原生"，下一帧 `configureSurface` 会按 picked 重新建、重新包。
    s.chainMode = 0;
    s.chainOwner = MCDR_FG_CHAIN_APP_NATIVE;
    gChainOwnerSnapshot.store(MCDR_FG_CHAIN_APP_NATIVE, std::memory_order_relaxed);
    s.rebuildPending = s.desiredMode != 0 &&
                       (s.picked == MCDR_FG_BACKEND_FSRFG || s.picked == MCDR_FG_BACKEND_XESSFG);
}

void fgNoteChainOwner(int32_t owner) {
    if (owner < MCDR_FG_CHAIN_APP_NATIVE || owner > MCDR_FG_CHAIN_XESS_WRAP) {
        return;
    }
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.chainOwner = owner;
    gChainOwnerSnapshot.store(owner, std::memory_order_relaxed);
}

int32_t fgChainOwner(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.chainOwner;
}

int32_t fgChainOwnerFast(void) {
    // 只读原子镜像：调用点可能在已持锁的临界区里（见 gChainOwnerSnapshot 的注释）。
    return gChainOwnerSnapshot.load(std::memory_order_relaxed);
}

int32_t fgPickedBackend(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.picked;
}

bool fgWantsAppNativeChain(void) {
    // 包装型后端（FSR-FG / XeSS-FG）只能吃应用原生链：SL 的代理工厂把链的 `Present` 槽
    // 留给自己，SDK 再包装一层就会与 SL 的钩子互相顶掉（两种集成方式互斥，见
    // `d3d12_surface.cpp:72-78`）。所以 pick 了这两家就要求 factory 不升级。
    const int32_t picked = fgPickedBackend();
    return picked == MCDR_FG_BACKEND_FSRFG || picked == MCDR_FG_BACKEND_XESSFG;
}

std::string fgStatusText(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    const char* picked = "none";
    switch (s.picked) {
        case MCDR_FG_BACKEND_DLSSG:
            picked = "dlssg";
            break;
        case MCDR_FG_BACKEND_FSRFG:
            picked = "fsr-fg";
            break;
        case MCDR_FG_BACKEND_XESSFG:
            picked = "xess-fg";
            break;
        default:
            break;
    }
    const char* applied = "none";
    switch (s.applied) {
        case MCDR_FG_BACKEND_DLSSG:
            applied = "dlssg";
            break;
        case MCDR_FG_BACKEND_FSRFG:
            applied = "fsr-fg";
            break;
        case MCDR_FG_BACKEND_XESSFG:
            applied = "xess-fg";
            break;
        default:
            break;
    }
    // P304：链的归属现在有四种，"sl-proxy / app-native" 二分会把包装型后端说成 app-native
    // 而掩盖"谁在包装这条链"这个事实（重建时要先 unwrap）。
    const char* chain = "app-native";
    switch (s.chainOwner) {
        case MCDR_FG_CHAIN_SL_PROXY:
            chain = "sl-proxy";
            break;
        case MCDR_FG_CHAIN_FSR_WRAP:
            chain = "fsr-wrap";
            break;
        case MCDR_FG_CHAIN_XESS_WRAP:
            chain = "xess-wrap";
            break;
        default:
            break;
    }

    std::string text = "[fg] picked=";
    text += picked;
    text += " applied=";
    text += applied;
    text += " chain=";
    text += chain;
    // P296：期望 / 链建成时的 mode / 重建次数 —— 「开关到底生效了没有」只能看这三个数。
    text += " mode=";
    text += std::to_string(s.desiredMode);
    text += "/";
    text += std::to_string(s.chainMode);
    text += " rebuilds=";
    text += std::to_string(s.rebuilds);
    if (s.rebuildPending) {
        text += " rebuildPending=1";
    }
    // P297：三轨帧号。验收时看的就是这三个数是否严格 +1。
    text += " tracks=";
    text += std::to_string(s.appFrameId);
    text += "/";
    text += std::to_string(s.backendFrameId);
    text += "/";
    text += std::to_string(s.presentId);
    // P306s：provider 自报的“每次 Present 实际呈现了几帧”。1 = 没插帧（链路可能接上了但
    // 生成没发生），2 = 2x。这是唯一能区分“接上了”与“真在插帧”的数字，设置页直接可见，
    // 比 F3 的 `presented`（历史上只看 DLSS-G 计数器）可靠。
    if (s.provider != nullptr) {
        text += " presented=";
        text += std::to_string(s.provider->presented());
    }
    // P298：低延迟方案。括号里那句是**现实**，别删 —— 免得有人看到 antilag2/xell 就以为已经支持。
    text += " latency=";
    switch (s.picked) {
        case MCDR_FG_BACKEND_DLSSG:
            text += "reflex";
            break;
        case MCDR_FG_BACKEND_FSRFG:
            text += "antilag2(no-dll)";
            break;
        case MCDR_FG_BACKEND_XESSFG:
            // P305ah 起 XeLL 的 marker/sleep 已接（SetLatencyReduction + 帧号对齐 presentId），
            // 不再是“没接”。这句曾写 xell(not-wired)，是旧状态，别改回去。
            text += "xell";
            break;
        default:
            text += "none";
            break;
    }
    if (!s.lastReason.empty()) {
        text += " reason=";
        text += s.lastReason;
    }
    return text;
}

int32_t fgStatePacked(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // 位布局是契约，改它必须同步 `include\mcdr_abi.h` 的声明注释与 `MCDR_ABI.kt` 的读取方式。
    const int32_t desired = s.desiredMode & 0xFF;
    const int32_t chain = s.chainMode & 0xFF;
    const int32_t active = s.provider != nullptr ? 1 : 0;
    const int32_t rebuilds = s.rebuilds > 255 ? 255 : (s.rebuilds < 0 ? 0 : s.rebuilds);
    return desired | (chain << 8) | (active << 16) | (rebuilds << 24);
}

// P305：把新建好的链交给包装型 provider。**只在 picked 是包装型后端时**动手 —— 否则返回 0，
// 调用方继续用原链（这就是"零行为变化"的保证：DLSS-G 路径永远走不到这里）。
int32_t fgWrapPresentChain(void* device, void* queue, void* appChain, int32_t width, int32_t height, int32_t format,
                           void** outPresentChain) {
    if (outPresentChain != nullptr) {
        *outPresentChain = nullptr;
    }
    if (device == nullptr || queue == nullptr || appChain == nullptr || outPresentChain == nullptr) {
        return 0;
    }
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    // P306：FSR-FG 与 XeSS-FG 都是"接管应用原生链"的后端，两家都走这里；注册表的
    // "同一时刻只有一个 provider"约束保证它们不会同时抢同一条链。
    if (s.picked != MCDR_FG_BACKEND_XESSFG && s.picked != MCDR_FG_BACKEND_FSRFG) {
        return 0;
    }
    // P306：FG 关闭时**不要**接管交换链。包装链在关闭态没有任何收益，但会让"换一家
    // 后端"永远失败 —— 旧 provider 的代理链还挂在 `s->swapChain` 上，新后端再包一次
    // 就是双重包装（实测 `CreateContext(swap) rc=0x3`，随后整条 FG 静默失效）。
    // 打开 FG 的那一帧 `fgSetMode` 会置 `rebuildPending`，链在那里重建并包装。
    if (s.desiredMode == 0) {
        return 0;
    }
    if (ensureProviderLocked(s) != 1 || s.provider == nullptr) {
        return 0;
    }
    return s.provider->wrapSwapChain(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue),
                                     static_cast<IDXGISwapChain*>(appChain), width, height,
                                     static_cast<DXGI_FORMAT>(format),
                                     reinterpret_cast<IDXGISwapChain**>(outPresentChain));
}

// 链要销毁/重建之前必须先解包：provider 的 context 绑在那条链上（`InitFromSwapChain` 的行头）。
void fgUnwrapPresentChain(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.provider != nullptr) {
        s.provider->unwrapSwapChain();
    }
}
// P305b：每帧一次，把 tag 机会交给 provider。现在 dispatch 里还没有资源句柄（P305c 由 JVM 灌），
// 所以包装型 provider 只会在这里记一行"到点了但没东西可 tag"，**不做任何 SDK 调用** ——
// 这正是 kTaggingWired 仍然是 false 的原因。
// P305c：登记 UI 纹理。注册表持锁保存并同步给 provider —— provider 在 tag 时读它，不回头调注册表。
void fgSetFrameConstants(const mcdr_fg_constants* constants)
{
    if (constants == nullptr)
    {
        return;
    }
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.constants = *constants;
    s.hasConstants = true;
    if (s.provider != nullptr)
    {
        s.provider->setFrameConstants(*constants);
    }
}

ID3D12Resource* unwrapDx12Resource(void* object) {
    // P306m：JVM 交过来的是 `Dx12Object*`（`D3D12GpuTexture.handle()` 的值），不是
    // `ID3D12Resource*`。之前把包装对象直接当资源传给 `ffxApiGetResourceDX12`，它读
    // `GetDesc()` 时崩在 0x50（hs_err 指向 `ffxApiGetResourceDX12+0x4f` → `applyFrameLocked`）。
    // 句柄本身仍然按 `void*` 存/回放：包装对象的生命周期由 JVM 侧持有，我们只借用。
    if (object == nullptr) {
        return nullptr;
    }
    auto* wrapper = static_cast<Dx12Object*>(object);
    return wrapper != nullptr ? wrapper->resource.Get() : nullptr;
}

void fgSetUiTexture(void* object) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.uiTexture = object;
    if (s.provider != nullptr) {
        s.provider->setUiTexture(unwrapDx12Resource(object));
    }
}

// P306ac：hudless 呈现开关。与 `uiTexture` 一样属于"期望状态"：provider 可能还没建出来
// （启动早期 / 链重建那一帧），所以先记在注册表里，建出来时由 `ensureProviderLocked` 回放。
void fgSetUiComposition(int32_t enabled) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    const int32_t value = enabled != 0 ? 1 : 0;
    if (s.uiComposition == value) {
        return;
    }
    s.uiComposition = value;
    if (s.provider != nullptr) {
        s.provider->setUiComposition(value);
    }
    dbgLogWarn("fg: hudless ui composition %s (backend=%d)", value != 0 ? "on" : "off", s.applied);
}

int32_t fgUiCompositionReady(void) {
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.provider == nullptr) {
        return 0;
    }
    // provider 自己回答"链已包装 + UI 纹理已交过来"。**不看** `s.uiComposition`：JVM 正是先用这个
    // 查询决定要不要打开 hudless（P306ac）—— 要求"已打开"会让它永远打不开。
    // 拿不准就是 0 —— 调用方据此继续走"自己把 UI 合成进 backbuffer"的旧路径，UI 不会整帧消失。
    return s.provider->uiCompositionReady() ? 1 : 0;
}

FgFrameInputs fgMakeFrameInputs(CommandContext* ctx, const mcdr_sr_dispatch& sr) {
    FgFrameInputs in{};
    if (ctx != nullptr) {
        in.cmdList = ctx->commandList;
    }
    // P306v：SR 通用路径给的是 `Dx12Object*`（JVM 的 `D3D12GpuTexture.handle()`），不是
    // `ID3D12Resource*` —— 三个指针必须先解包，否则 provider 里 `GetDesc()` 会 AV
    // （实测 `P306 fsr-fg: frame inputs were released under us ... bad=<color>`：FSR 超分
    // 路径下 FG 一帧输入都拿不到，插帧永远不启动）。DLSS 那条路径走的是
    // `mcdr_fgTagDlssInputs`，它给的是裸 `ID3D12Resource*`，不经过这里。
    in.pColorOutput = unwrapDx12Resource(sr.pColorOutput);
    in.pDepth = unwrapDx12Resource(sr.pDepth);
    in.pMotionVectors = unwrapDx12Resource(sr.pMotionVectors);
    in.renderWidth = sr.renderWidth;
    in.renderHeight = sr.renderHeight;
    in.displayWidth = sr.displayWidth;
    in.displayHeight = sr.displayHeight;
    in.cameraNear = sr.cameraNear;
    in.cameraFar = sr.cameraFar;
    in.cameraVerticalFov = sr.cameraVerticalFov;
    in.frameTimeDeltaMs = sr.frameTimeDeltaMs;
    in.viewSpaceToMeters = sr.viewSpaceToMetersFactor;
    // SR 通用路径的契约（P273）：返回时输出已回到 RENDER_TARGET，深度是只读输入，MV 是 SRV。
    in.colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    in.depthState = D3D12_RESOURCE_STATE_DEPTH_READ;
    in.motionVectorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return in;
}

void fgTagFrameInputs(const FgFrameInputs& inputs) {
    if (inputs.cmdList == nullptr) {
        return;
    }
    FgRegistry& s = registry();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.provider == nullptr) {
        return;
    }
    s.provider->tagFrameInputs(inputs);
}
}  // namespace d3d12_mc