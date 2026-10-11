// mcdr_abi.h — public C ABI of the MCDR native render library.
//
// This header is the *only* contract between the native library and the JVM
// side. It replaces the former JNI bridge (6 files, 2158 lines, 115
// `Java_com_xgdt_dx12_dx12_Dx12Native_*` exports) with a flat C ABI that the
// Java side reaches through the FFM API (java.lang.foreign).
//
// Design rules (see 26.2-rt-experiment-design.md §5.2/§5.3):
//   * C linkage only (`extern "C"`), no name mangling, no generated headers.
//   * Every cross-language aggregate is ONE struct definition, mirrored 1:1 by
//     a `MemoryLayout.structLayout` on the Java side. No hand-rolled byte
//     offsets, no packed blobs, no byte-order ambiguity.
//   * Handles are opaque pointers to native objects — never integers whose
//     meaning the Java side has to guess.
//   * Strings are caller-allocated UTF-8 buffers; the callee returns the
//     required byte count (including the NUL) so the caller can grow and retry.
//   * No exceptions cross the boundary. Boolean entry points return int32_t
//     1/0. mcdrLastError() carries the message.
//   * Array parameters are (pointer, count) pairs, never JNI arrays.
//   * One single upcall stub type (mcdr_fence_callback_fn) carries an integer
//     user handle, so the native layer never holds a JVM object reference.
//
// Windows x64 mapping note for the Java mirror: the MSVC `long` is 32-bit, so
// every parameter written here as int32_t must be JAVA_INT on the Java side
// (only int64_t/size_t/pointers are JAVA_LONG/JAVA_ADDRESS).

#ifndef MCDR_ABI_H
#define MCDR_ABI_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(MCDR_EXPORTS)
#define MCDR_API __declspec(dllexport)
#else
#define MCDR_API __declspec(dllimport)
#endif
#define MCDR_CALL __cdecl
#else
#define MCDR_API __attribute__((visibility("default")))
#define MCDR_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever ANY struct layout or signature below changes. The Java side
// asserts equality at load time and aborts backend creation on mismatch.
#define MCDR_ABI_VERSION 1u

// The same value at runtime, so a stale Java-side constant cannot be optimised
// away and a mismatched pair fails at load time rather than at first draw.
MCDR_API uint32_t MCDR_CALL mcdrABIVersion(void);

// ---------------------------------------------------------------------------
// Opaque handles
//
// Each typedef forward-declares the real C++ class; the native side
// reinterpret_casts between them. The Java side only ever sees a MemorySegment
// address and must never invent one.
// Real class names, not invented ones: query the core headers, do not guess.
// Each handle names the real C++ class so the native side can reinterpret_cast
// with no intermediate wrapper object and no runtime cost. In C++ the tag is
// declared with the SAME class-key the definition uses, because MSVC's C4099
// ("type name first seen using 'class' now seen using 'struct'") becomes a
// hard error under this project's TreatWarningAsError=true.
// C++ forward declarations MUST live inside namespace d3d12_mc. A global-scope
// `struct Dx12Object;` is a *different type* from `d3d12_mc::Dx12Object`, so the
// typedef below would silently name an incomplete global type and every adapter
// call site would fail with C2664 "cannot convert argument 1".
#ifdef __cplusplus
namespace d3d12_mc {
class PartitionedDescriptorAllocator;
class BundleRecorderPool;
class MainCommandExecutor;
class AsyncFenceManager;
struct Dx12Object;
struct CommandContext;
struct Dx12Pipeline;
struct Dx12Surface;
struct QueryPool;
}  // namespace d3d12_mc

typedef d3d12_mc::Dx12Object mcdr_object;       // texture / buffer / sampler / texture view
typedef d3d12_mc::CommandContext mcdr_command_ctx;  // one command encoder (3-deep list ring)
typedef d3d12_mc::Dx12Pipeline mcdr_pipeline;
typedef d3d12_mc::Dx12Surface mcdr_surface;
typedef d3d12_mc::QueryPool mcdr_query_pool;
typedef d3d12_mc::PartitionedDescriptorAllocator mcdr_desc_alloc;
typedef d3d12_mc::BundleRecorderPool mcdr_bundle_pool;
typedef d3d12_mc::MainCommandExecutor mcdr_executor;
typedef d3d12_mc::AsyncFenceManager mcdr_fence_mgr;
#else
typedef struct mcdr_object mcdr_object;
typedef struct mcdr_command_ctx mcdr_command_ctx;
typedef struct mcdr_pipeline mcdr_pipeline;
typedef struct mcdr_surface mcdr_surface;
typedef struct mcdr_query_pool mcdr_query_pool;
typedef struct mcdr_desc_alloc mcdr_desc_alloc;
typedef struct mcdr_bundle_pool mcdr_bundle_pool;
typedef struct mcdr_executor mcdr_executor;
typedef struct mcdr_fence_mgr mcdr_fence_mgr;
#endif

// ---------------------------------------------------------------------------
// Cross-language aggregates
//
// Every struct below has an exact Java mirror built from MemoryLayout. Offsets
// and sizes are asserted at runtime by mcdrABILayout() — never by hand.
// ---------------------------------------------------------------------------

// One pipeline binding slot, shared by mcdr_dx12PushDescriptors and the bundle
// recorder. Replaces bType/bBuf/bOff/bLen/bView.
typedef struct mcdr_binding_slot {
    int32_t type;          // 0 = CBV, 1 = SRV (texture view), 2 = SRV (texel buffer)
    int32_t texelFormat;   // type 2 only: GpuFormat ordinal
    mcdr_object* buffer;        // type 0/2 only
    mcdr_object* view;          // type 1 only
    int64_t offset;        // type 0/2 only
    int64_t length;        // type 0/2 only
} mcdr_binding_slot;

// One draw's index/vertex geometry, used by the bundle recorder. Replaces
// idxBuf/idxType/idxCount/firstIdx/baseVert/vbSlot/vbBuf/vbOff/vbStride.
typedef struct mcdr_partition_draw {
    mcdr_object* indexBuffer;
    mcdr_object* vertexBuffer;
    int64_t vertexOffset;
    int32_t indexType;     // 0 = SHORT (R16_UINT), 1 = INT (R32_UINT)
    int32_t indexCount;
    int32_t firstIndex;
    int32_t baseVertex;
    int32_t vertexSlot;
    int32_t vertexStride;
    int32_t _pad0;
} mcdr_partition_draw;

// One colour target of a graphics pipeline. Replaces 8 bytes of the old packed
// descriptor blob. format == -1 means "unused slot" (DXGI_FORMAT_UNKNOWN).
typedef struct mcdr_color_target_desc {
    int32_t format;             // GpuFormat ordinal, or -1
    uint8_t writeMask;
    uint8_t blendEnabled;
    uint8_t srcColor;           // BlendFactor ordinal
    uint8_t dstColor;           // BlendFactor ordinal
    uint8_t colorOp;            // BlendOp ordinal
    uint8_t srcAlpha;           // BlendFactor ordinal
    uint8_t dstAlpha;           // BlendFactor ordinal
    uint8_t alphaOp;            // BlendOp ordinal
} mcdr_color_target_desc;

typedef struct mcdr_depth_state_desc {
    int32_t depthFormat;        // GpuFormat ordinal
    uint8_t depthWrite;
    uint8_t depthCompareOp;     // CompareOp ordinal
} mcdr_depth_state_desc;

// HLSL input element. `semanticName` is a NUL-terminated ASCII string. The old
// bridge synthesised it (empty -> "TEXCOORD"), which the packed blob could not
// express; here it crosses explicitly.
typedef struct mcdr_input_element_desc {
    int32_t location;
    int32_t binding;
    int32_t format;         // GpuFormat ordinal
    int32_t offset;
    int32_t stride;
    int32_t stepRate;       // 0 = per-vertex, 1 = per-instance
    int32_t _pad0;
    const char* semanticName;   // NUL-terminated; NULL or "" -> "TEXCOORD"
} mcdr_input_element_desc;

typedef struct mcdr_pipeline_binding_desc {
    uint8_t type;               // 0 = CBV, 1 = SRV + static sampler, 2 = SRV
    uint8_t reg;
} mcdr_pipeline_binding_desc;

// Full graphics-pipeline description. Replaces the LITTLE_ENDIAN packed
// ByteBuffer that dx12CreateGraphicsPipeline() unpacked by hand.
// `vsData`/`psData` point at DXBC bytecode (not NUL-terminated).
typedef struct mcdr_pipeline_desc {
    const void* vsData;
    int32_t vsLen;
    int32_t _pad0;
    const void* psData;
    int32_t psLen;
    int32_t colorCount;
    const mcdr_color_target_desc* colorTargets;
    int32_t hasDepth;
    int32_t _pad1;
    mcdr_depth_state_desc depth;
    int32_t topology;       // PrimitiveTopology ordinal
    int32_t cullEnabled;
    int32_t polygonMode;    // PolygonMode ordinal
    int32_t inputElementCount;
    const mcdr_input_element_desc* inputElements;
    int32_t bindingCount;
    int32_t _pad2;
    const mcdr_pipeline_binding_desc* bindings;
} mcdr_pipeline_desc;

// ---------------------------------------------------------------------------
// ABI self-check
//
// The native side publishes one entry per mirrored struct field. The Java side
// walks the same list from its own MemoryLayouts and fails fast on the first
// mismatch. This is the check JNI never had: under JNI a package rename or an
// added struct field silently corrupted memory at runtime.
// ---------------------------------------------------------------------------
typedef struct mcdr_abi_field {
    const char* name;   // "mcdr_binding_slot.buffer"
    uint64_t offset;
    uint64_t size;
} mcdr_abi_field;

MCDR_API void MCDR_CALL mcdrABILayout(const mcdr_abi_field** fields, int32_t* count);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// Copies the calling thread's last error message into `buf` as UTF-8 and
// returns the required byte count including the NUL terminator, or 0 when no
// error is pending. buf == NULL / cap == 0 performs a size query only.
MCDR_API int32_t MCDR_CALL mcdrLastError(char* buf, int32_t cap);

// Verbosity for the native log, mirroring DX12_LOG_VERBOSE.
MCDR_API void MCDR_CALL mcdrSetLogVerbose(int32_t enabled);

// ---------------------------------------------------------------------------
// Device and resources  (was jni_bridge.cpp — 13 exports)
// ---------------------------------------------------------------------------

// Writes a human-readable device/adapter summary into `buf`.
MCDR_API int32_t MCDR_CALL mcdr_dx12CreateDevice(char* buf, int32_t cap);

// P31：释放设备上下文中的全部 D3D12/DXGI 对象（device / queue / 描述符堆 /
// DXGI adapter）。**必须在进程退出前调用**：这些 ComPtr 若留到
// DLL_PROCESS_DETACH 才析构，Release() 会在 dxgi.dll 已经 detach 之后执行，
// 产生原生异常并以异常码 2170 作为进程退出码。幂等，重复调用安全。
MCDR_API void MCDR_CALL mcdr_dx12DestroyDevice(void);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateTexture(int32_t usage, int32_t format, int32_t width,
                                                       int32_t height, int32_t depthOrLayers,
                                                       int32_t mipLevels);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateBuffer(int32_t usage, int64_t size);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateSampler(int32_t addressU, int32_t addressV, int32_t minFilter,
                                                       int32_t magFilter, int32_t maxAnisotropy,
                                                       float maxLod);

MCDR_API mcdr_object* MCDR_CALL mcdr_dx12CreateTextureView(mcdr_object* texture, int32_t baseMipLevel,
                                                           int32_t mipLevels);

MCDR_API void MCDR_CALL mcdr_dx12DestroyResource(mcdr_object* object);

// Returns the CPU-visible base address of a mapped buffer, or NULL on failure.
// `outLength` receives the number of bytes actually mapped.
MCDR_API void* MCDR_CALL mcdr_dx12MapBuffer(mcdr_object* buffer, int64_t offset, int64_t length, int32_t read,
                                            int32_t write, int64_t* outLength);

MCDR_API void MCDR_CALL mcdr_dx12UnmapBuffer(mcdr_object* buffer);

// Writes a JSON array describing every adapter into `buf`.
MCDR_API int32_t MCDR_CALL mcdr_dx12EnumerateAdapters(char* buf, int32_t cap);

// Writes a flat, single-line JSON object describing the adapter that the ACTIVE
// device is bound to. Every key is always present:
//   name / vendor / vendor_id / device_id / subsys_id / revision /
//   feature_level / feature_level_value / driver_version / driver_description /
//   device_type / vram_bytes / shared_bytes / dedicated_system_bytes / luid /
//   is_hardware / is_detachable / dxcore
// `vendor` is mapped through the same vendor table vanilla uses in
// VulkanPhysicalDevice.java. `driver_version`, `driver_description`,
// `is_hardware` and `is_detachable` come from DXCore, which is loaded at run
// time (this DLL has no link-time dependency on dxcore.dll); when DXCore is
// unavailable those fields fall back to ""/false and `dxcore` is false.
//
// Same buffer convention as mcdr_dx12EnumerateAdapters: buf == nullptr or
// cap <= 0 returns the required byte count INCLUDING the terminating NUL;
// otherwise a NUL-terminated UTF-8 string is written and the number of bytes
// written EXCLUDING the NUL is returned. Returns 0 when there is no active
// device (not created yet, or already destroyed).
MCDR_API int32_t MCDR_CALL mcdr_dx12GetAdapterInfo(char* buf, int32_t cap);

MCDR_API uint64_t MCDR_CALL mcdr_dx12GetQueueHandle(void);
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetDeviceHandle(void);

MCDR_API uint64_t MCDR_CALL mcdr_dx12CreateHiddenWindow(int32_t width, int32_t height);
MCDR_API void MCDR_CALL mcdr_dx12DestroyHiddenWindow(uint64_t hwnd);

// ---------------------------------------------------------------------------
// Command layer  (was jni_bridge_p3.cpp — 36 exports, +1 for MC 26.2 clearStencilTexture)
// ---------------------------------------------------------------------------

MCDR_API int64_t MCDR_CALL mcdr_dx12GetTimestampFrequency(void);

MCDR_API mcdr_command_ctx* MCDR_CALL mcdr_dx12CreateCommandEncoder(void);
MCDR_API void MCDR_CALL mcdr_dx12DestroyCommandEncoder(mcdr_command_ctx* ctx);
MCDR_API void MCDR_CALL mcdr_dx12BeginCommandList(mcdr_command_ctx* ctx);
MCDR_API void MCDR_CALL mcdr_dx12BeginCommandListWithWait(mcdr_command_ctx* ctx, uint64_t waitForValue);
MCDR_API void MCDR_CALL mcdr_dx12EndCommandList(mcdr_command_ctx* ctx);
MCDR_API uint64_t MCDR_CALL mcdr_dx12Submit(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12WaitForFence(mcdr_command_ctx* ctx, uint64_t value, int64_t timeoutNs);
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetFenceValue(mcdr_command_ctx* ctx);
MCDR_API int64_t MCDR_CALL mcdr_dx12GetTimestampNow(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12IsListOpen(mcdr_command_ctx* ctx);

MCDR_API void MCDR_CALL mcdr_dx12CopyBuffer(mcdr_command_ctx* ctx, mcdr_object* src, int64_t srcOffset,
                                            mcdr_object* dst, int64_t dstOffset, int64_t size);

MCDR_API void MCDR_CALL mcdr_dx12WriteToTexture(mcdr_command_ctx* ctx, mcdr_object* stagingBuffer,
                                                int64_t stagingOffset, int32_t width, int32_t height,
                                                mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                int32_t dstX, int32_t dstY);

MCDR_API void MCDR_CALL mcdr_dx12CopyBufferToTexture(mcdr_command_ctx* ctx, mcdr_object* srcBuffer,
                                                     int64_t srcOffset, int32_t srcWidth, int32_t srcHeight,
                                                     mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                     int32_t dstX, int32_t dstY, int32_t width,
                                                     int32_t height);

MCDR_API void MCDR_CALL mcdr_dx12CopyTextureToBuffer(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
                                                     int32_t mip, int32_t layer, int32_t srcX, int32_t srcY,
                                                     int32_t width, int32_t height, mcdr_object* dstBuffer,
                                                     int64_t dstOffset);

MCDR_API void MCDR_CALL mcdr_dx12CopyTextureToTexture(mcdr_command_ctx* ctx, mcdr_object* srcTexture,
                                                      mcdr_object* dstTexture, int32_t mip, int32_t layer,
                                                      int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY,
                                                      int32_t width, int32_t height);

MCDR_API void MCDR_CALL mcdr_dx12ClearColorTexture(mcdr_command_ctx* ctx, mcdr_object* texture, float r,
                                                   float g, float b, float a);
MCDR_API void MCDR_CALL mcdr_dx12ClearDepthTexture(mcdr_command_ctx* ctx, mcdr_object* texture, double depth);
MCDR_API void MCDR_CALL mcdr_dx12ClearColorTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture, float r,
                                                         float g, float b, float a, int32_t x, int32_t y,
                                                         int32_t width, int32_t height);
MCDR_API void MCDR_CALL mcdr_dx12ClearDepthTextureRegion(mcdr_command_ctx* ctx, mcdr_object* texture,
                                                         double depth, int32_t x, int32_t y, int32_t width,
                                                         int32_t height);

// MC 26.2 addition: CommandEncoderBackend::clearStencilTexture(GpuTexture, int).
// Clears only the stencil plane to `value`; the depth plane is untouched.
MCDR_API int32_t MCDR_CALL mcdr_dx12ClearStencilTexture(mcdr_command_ctx* ctx, mcdr_object* texture,
                                                        int32_t value);

// `colorTextures` may contain NULL entries (unused attachments); they are
// skipped. `colorMips` is the per-attachment RTV mip slice. `colorClearFlags`
// is 0 = load, 1 = clear. `clearColors` holds 4 floats per attachment.
MCDR_API void MCDR_CALL mcdr_dx12BeginRenderPass(mcdr_command_ctx* ctx, mcdr_object* const* colorTextures,
                                                 int32_t colorCount, const int32_t* colorMips,
                                                 const int32_t* colorClearFlags, const float* clearColors,
                                                 mcdr_object* depthTexture, int32_t depthMip,
                                                 int32_t depthClearFlag, double depthClearValue, int32_t x,
                                                 int32_t y, int32_t width, int32_t height);

MCDR_API void MCDR_CALL mcdr_dx12EndRenderPass(mcdr_command_ctx* ctx);
MCDR_API mcdr_object* MCDR_CALL mcdr_dx12GetActiveColorTexture(mcdr_command_ctx* ctx);

MCDR_API mcdr_query_pool* MCDR_CALL mcdr_dx12CreateQueryPool(int32_t size);
MCDR_API void MCDR_CALL mcdr_dx12DestroyQueryPool(mcdr_query_pool* pool);
MCDR_API void MCDR_CALL mcdr_dx12WriteTimestamp(mcdr_command_ctx* ctx, mcdr_query_pool* pool, int32_t index);
MCDR_API uint64_t MCDR_CALL mcdr_dx12ReadQueryValue(mcdr_query_pool* pool, int32_t index);
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadQueryValues(mcdr_query_pool* pool, int32_t start, int32_t count,
                                                    int64_t* out);

// Async renderer control channel.
MCDR_API void MCDR_CALL mcdr_dx12InitAsyncRenderer(int32_t workerCount);
MCDR_API void MCDR_CALL mcdr_dx12DestroyAsyncRenderer(void);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderBeginFrame(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitComplete(mcdr_command_ctx* ctx, int64_t timeoutMs);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderIsRecordingReady(mcdr_command_ctx* ctx);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncRenderWaitRecordingReady(mcdr_command_ctx* ctx, int64_t timeoutMs);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncSendCommandsReady(mcdr_command_ctx* ctx);
MCDR_API void MCDR_CALL mcdr_dx12ClearAsyncRenderCtx(void);

// ---------------------------------------------------------------------------
// Pipeline  (was jni_bridge_p4.cpp — 2 exports)
// ---------------------------------------------------------------------------

MCDR_API mcdr_pipeline* MCDR_CALL mcdr_dx12CreateGraphicsPipeline(const mcdr_pipeline_desc* desc);
MCDR_API void MCDR_CALL mcdr_dx12DestroyPipeline(mcdr_pipeline* pipeline);

// ---------------------------------------------------------------------------
// Surface  (was jni_bridge_p5.cpp — 13 exports)
// ---------------------------------------------------------------------------

MCDR_API mcdr_surface* MCDR_CALL mcdr_dx12CreateSurface(uint64_t hwnd);

// Writes the supported present-mode ordinals into `out` (up to `cap`) and
// returns the total count, so a caller can size the buffer first.
MCDR_API int32_t MCDR_CALL mcdr_dx12SurfacePresentModes(int32_t* out, int32_t cap);

MCDR_API int32_t MCDR_CALL mcdr_dx12ConfigureSurface(mcdr_surface* surface, int32_t width, int32_t height,
                                                     int32_t presentMode);
MCDR_API int32_t MCDR_CALL mcdr_dx12AcquireSurface(mcdr_surface* surface);
MCDR_API void MCDR_CALL mcdr_dx12BlitSurface(mcdr_command_ctx* ctx, mcdr_surface* surface,
                                             mcdr_object* texture);
MCDR_API void MCDR_CALL mcdr_dx12PresentSurface(mcdr_surface* surface);
// Returns the raw ID3D12Resource* of the currently acquired back buffer as an
// opaque integer (NOT a mcdr_object* -- the Java side only forwards it).
MCDR_API uint64_t MCDR_CALL mcdr_dx12GetBackBufferHandle(mcdr_surface* surface);
MCDR_API int32_t MCDR_CALL mcdr_dx12IsSurfaceSuboptimal(mcdr_surface* surface);
MCDR_API void MCDR_CALL mcdr_dx12DestroySurface(mcdr_surface* surface);
MCDR_API mcdr_surface* MCDR_CALL mcdr_dx12GetActiveSurfaceHandle(void);

// Both readback entry points write 36 int32s (9 pixels x RGBA) into `out` and
// return 1 on success, 0 on failure.
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadbackSurfacePixels(mcdr_surface* surface, int32_t* out);
MCDR_API int32_t MCDR_CALL mcdr_dx12ReadbackTexturePixels(mcdr_object* texture, int32_t* out);

MCDR_API void MCDR_CALL mcdr_dx12DumpTextureToFile(mcdr_object* texture, const char* tag);

// ---------------------------------------------------------------------------
// Draw  (was jni_bridge_p6.cpp — 9 exports, +1 for MC 26.2 setViewport)
// ---------------------------------------------------------------------------

MCDR_API int32_t MCDR_CALL mcdr_dx12SetPipeline(mcdr_command_ctx* ctx, mcdr_pipeline* pipeline,
                                                int32_t hasDepth);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetScissor(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                               int32_t height);
// MC 26.2 addition: RenderPassBackend::setViewport(int,int,int,int). Previously
// the viewport was only ever set inside mcdr_dx12BeginRenderPass.
MCDR_API int32_t MCDR_CALL mcdr_dx12SetViewport(mcdr_command_ctx* ctx, int32_t x, int32_t y, int32_t width,
                                                int32_t height);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetVertexBuffer(mcdr_command_ctx* ctx, int32_t slot, mcdr_object* buffer,
                                                    int64_t offset, int32_t stride);
MCDR_API int32_t MCDR_CALL mcdr_dx12SetIndexBuffer(mcdr_command_ctx* ctx, mcdr_object* buffer,
                                                   int32_t indexType);

// `bindings` holds `count` entries, one per pipeline binding in declaration
// order. Replaces the six parallel arrays of the JNI version.
MCDR_API int32_t MCDR_CALL mcdr_dx12PushDescriptors(mcdr_command_ctx* ctx, const mcdr_binding_slot* bindings,
                                                    int32_t count);

MCDR_API int32_t MCDR_CALL mcdr_dx12DrawIndexed(mcdr_command_ctx* ctx, int32_t indexCount,
                                                int32_t instanceCount, int32_t firstIndex, int32_t baseVertex,
                                                int32_t firstInstance);
MCDR_API int32_t MCDR_CALL mcdr_dx12Draw(mcdr_command_ctx* ctx, int32_t vertexCount, int32_t instanceCount,
                                         int32_t firstVertex, int32_t firstInstance);
MCDR_API int32_t MCDR_CALL mcdr_dx12DrawIndexedIndirect(mcdr_command_ctx* ctx, mcdr_object* commands,
                                                        int64_t offset, int32_t drawCount);
MCDR_API int32_t MCDR_CALL mcdr_dx12DrawIndirect(mcdr_command_ctx* ctx, mcdr_object* commands, int64_t offset,
                                                 int32_t drawCount);

// ---------------------------------------------------------------------------
// Async: partitioned descriptor allocator
// (was jni_bridge_p33.cpp — 42 exports across the four families below)
// ---------------------------------------------------------------------------

MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncCurrentFrameSlot(mcdr_command_ctx* ctx);
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncCurrentFrameValue(mcdr_command_ctx* ctx);

MCDR_API mcdr_desc_alloc* MCDR_CALL mcdr_dx12AsyncDescriptorCreate(int32_t workerCount);
MCDR_API void MCDR_CALL mcdr_dx12AsyncDescriptorDestroy(mcdr_desc_alloc* alloc);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncDescriptorRegionBase(mcdr_desc_alloc* alloc, int32_t frameSlot,
                                                              int32_t worker);
// Returns the base descriptor slot, or -1 when the region is exhausted.
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncDescriptorAllocate(mcdr_desc_alloc* alloc, int32_t frameSlot,
                                                            int32_t worker, int32_t count);
MCDR_API void MCDR_CALL mcdr_dx12AsyncDescriptorResetFrame(mcdr_desc_alloc* alloc, int32_t frameSlot);
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncDescriptorGpuHandle(mcdr_desc_alloc* alloc, int32_t slot);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncDescriptorWriteCBV(mcdr_desc_alloc* alloc, int32_t slot,
                                                            mcdr_object* buffer, int64_t offset,
                                                            int64_t size);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncDescriptorWriteSRV(mcdr_desc_alloc* alloc, int32_t slot,
                                                            mcdr_object* view);

// ---------------------------------------------------------------------------
// Async: bundle recorder pool
// ---------------------------------------------------------------------------

MCDR_API mcdr_bundle_pool* MCDR_CALL mcdr_dx12AsyncBundlePoolCreate(int32_t workerCount);
MCDR_API void MCDR_CALL mcdr_dx12AsyncBundlePoolDestroy(mcdr_bundle_pool* pool);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleBegin(mcdr_bundle_pool* pool, int32_t worker,
                                                     int32_t frameSlot, uint64_t frameValue);
// Returns the recorded ID3D12GraphicsCommandList* as an opaque integer, 0 on failure.
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncBundleEnd(mcdr_bundle_pool* pool, int32_t worker);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleSetPipelineState(mcdr_bundle_pool* pool, int32_t worker,
                                                                mcdr_pipeline* pipeline, int32_t hasDepth);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleSetDescriptorTable(mcdr_bundle_pool* pool, int32_t worker,
                                                                  int32_t slot, uint64_t gpuHandle);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleSetVertexBuffer(mcdr_bundle_pool* pool, int32_t worker,
                                                               int32_t slot, mcdr_object* buffer,
                                                               int64_t offset, int32_t stride);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleSetIndexBuffer(mcdr_bundle_pool* pool, int32_t worker,
                                                              mcdr_object* buffer, int32_t indexType);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleDrawIndexed(mcdr_bundle_pool* pool, int32_t worker,
                                                           int32_t indexCount, int32_t instanceCount,
                                                           int32_t firstIndex, int32_t baseVertex,
                                                           int32_t firstInstance);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleDraw(mcdr_bundle_pool* pool, int32_t worker,
                                                    int32_t vertexCount, int32_t instanceCount,
                                                    int32_t firstVertex, int32_t firstInstance);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleDrawIndexedIndirect(mcdr_bundle_pool* pool, int32_t worker,
                                                                   mcdr_object* commands, int64_t offset,
                                                                   int32_t drawCount);

// The hot path (P57/P63/P64): one call records a whole worker partition.
//
//   `bindings` — when `sharedBindings` is non-zero, `bindingCount` entries
//                describing draw 0's block, reused for every draw. Otherwise
//                `drawCount * bindingCount` entries indexed as
//                bindings[draw * bindingCount + slot].
//   `draws`    — `drawCount` entries for the partition, already advanced to
//                the partition's first draw.
//
// Semantics match the per-call sequence exactly: any invalid argument returns
// 0 and the Java side discards the bundle and re-records serially.
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncBundleRecordPartition(mcdr_desc_alloc* alloc, mcdr_bundle_pool* pool,
                                                               int32_t worker, int32_t baseSlot,
                                                               int32_t bindingCount, int32_t sharedBindings,
                                                               const mcdr_binding_slot* bindings,
                                                               const mcdr_partition_draw* draws,
                                                               int32_t drawCount);

MCDR_API int32_t MCDR_CALL mcdr_dx12ExecuteBundle(mcdr_command_ctx* ctx, uint64_t bundle);

// Barrier pre-passes: a bundle may not contain barriers, so every CBV buffer,
// index buffer and texture view is transitioned on the main list first.
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncPrepareCBVBuffers(mcdr_command_ctx* ctx, mcdr_object* const* buffers,
                                                           int32_t count);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncPrepareIndexBuffers(mcdr_command_ctx* ctx,
                                                             mcdr_object* const* buffers, int32_t count);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncPrepareTextureViews(mcdr_command_ctx* ctx, mcdr_object* const* views,
                                                             int32_t count);

// ---------------------------------------------------------------------------
// Async: executor
// ---------------------------------------------------------------------------

MCDR_API mcdr_executor* MCDR_CALL mcdr_dx12AsyncExecutorCreate(void);
MCDR_API void MCDR_CALL mcdr_dx12AsyncExecutorDestroy(mcdr_executor* executor);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorTryBeginFrame(mcdr_executor* executor);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorAddBundle(mcdr_executor* executor, uint64_t bundle);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorAddTransition(mcdr_executor* executor, mcdr_object* object,
                                                               int32_t stateBefore, int32_t stateAfter);
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncExecutorEndFrame(mcdr_executor* executor);
MCDR_API uint64_t MCDR_CALL mcdr_dx12AsyncExecutorLastFenceValue(mcdr_executor* executor);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorIsFrameInProgress(mcdr_executor* executor);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncExecutorWaitFrame(mcdr_executor* executor, uint64_t fenceValue,
                                                           int64_t timeoutMs);

// ---------------------------------------------------------------------------
// Async: fence manager
//
// The only place the native layer calls back into the JVM. `cb` is a single
// FFM upcall stub created once for the whole library; `userHandle` identifies
// the Java-side registration, so no JVM object reference is ever retained here.
// The callback is invoked OUTSIDE the registry lock, so it may re-register or
// unregister. `timeoutMs == 0` means wait forever.
// ---------------------------------------------------------------------------

typedef void(MCDR_CALL* mcdr_fence_callback_fn)(int64_t userHandle, uint64_t value, int32_t completed);

MCDR_API mcdr_fence_mgr* MCDR_CALL mcdr_dx12AsyncFenceCreate(void);
MCDR_API void MCDR_CALL mcdr_dx12AsyncFenceDestroy(mcdr_fence_mgr* manager);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncFenceSignal(mcdr_fence_mgr* manager, uint64_t value);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncFenceWait(mcdr_fence_mgr* manager, uint64_t value,
                                                   int64_t timeoutMs);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncFenceIsComplete(mcdr_fence_mgr* manager, uint64_t value);
MCDR_API int32_t MCDR_CALL mcdr_dx12AsyncFenceRegisterCallback(mcdr_fence_mgr* manager, uint64_t value,
                                                               mcdr_fence_callback_fn cb, int64_t userHandle,
                                                               int64_t timeoutMs);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MCDR_ABI_H
