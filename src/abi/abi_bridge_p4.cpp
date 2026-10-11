// abi_bridge_p4.cpp — graphics pipeline creation/destruction.
//
// Replaces jni_bridge_p4.cpp (172 lines). That file's entire body was a
// hand-written little-endian unpacker for the packed descriptor ByteBuffer the
// Java side built with putInt/putU8. Because the blob carried no field
// identifiers, every offset was a magic number on both sides and a single
// mis-ordered field silently produced a wrong pipeline; the semantic names even
// had to travel in a separate trailing section and were re-associated with
// their input elements by array index.
//
// Here the descriptor is a real struct (mcdr_pipeline_desc) whose layout the
// Java side mirrors with MemoryLayout.structLayout and whose offsets are
// verified at load time by mcdrABILayout(). There is nothing left to unpack.

#include "abi_common.h"

#include "../dx12/dx12_device.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

// Bounds the old unpacker enforced inline. Kept as named constants because they
// are the contract with createGraphicsPipeline(), not incidental limits.
constexpr int32_t kMaxColorTargets = 8;
constexpr int32_t kMaxInputElements = 64;
constexpr int32_t kMaxPipelineBindings = 64;

constexpr const char* kCreate = "mcdr_dx12CreateGraphicsPipeline";

using PipelinePtr = d3d12_mc::Dx12Pipeline;

}  // namespace

extern "C" {

PipelinePtr* MCDR_CALL mcdr_dx12CreateGraphicsPipeline(const mcdr_pipeline_desc* desc) {
    using mcdr::abi::failWithPtr;
    using mcdr::abi::setLastErrorf;

    if (!desc) {
        return failWithPtr<PipelinePtr>(kCreate, "descriptor is null");
    }

    d3d12_mc::PipelineDesc d;

    // --- shader bytecode ---------------------------------------------------
    // The old path went through NewDirectByteBuffer + GetDirectBufferAddress,
    // which rejected heap buffers with a bare stderr line. A null/length
    // mismatch is now an explicit, reportable error.
    if (desc->vsLen < 0 || (desc->vsLen > 0 && desc->vsData == nullptr)) {
        return failWithPtr<PipelinePtr>(kCreate, "vsData/vsLen mismatch");
    }
    if (desc->psLen < 0 || (desc->psLen > 0 && desc->psData == nullptr)) {
        return failWithPtr<PipelinePtr>(kCreate, "psData/psLen mismatch");
    }
    if (desc->vsLen > 0) {
        const auto* first = static_cast<const uint8_t*>(desc->vsData);
        d.vsBytes.assign(first, first + desc->vsLen);
    }
    if (desc->psLen > 0) {
        const auto* first = static_cast<const uint8_t*>(desc->psData);
        d.psBytes.assign(first, first + desc->psLen);
    }

    // --- colour targets ----------------------------------------------------
    if (desc->colorCount < 0 || desc->colorCount > kMaxColorTargets) {
        setLastErrorf(kCreate, "bad colorCount %d", static_cast<int>(desc->colorCount));
        return nullptr;
    }
    if (desc->colorCount > 0 && desc->colorTargets == nullptr) {
        return failWithPtr<PipelinePtr>(kCreate, "colorTargets is null");
    }
    d.colorCount = desc->colorCount;
    d.colorTargets.resize(static_cast<size_t>(desc->colorCount));
    for (int32_t i = 0; i < desc->colorCount; ++i) {
        const mcdr_color_target_desc& src = desc->colorTargets[i];
        d3d12_mc::PipelineDesc::ColorTarget& dst = d.colorTargets[static_cast<size_t>(i)];
        dst.format = src.format;
        dst.writeMask = src.writeMask;
        dst.blendEnabled = src.blendEnabled != 0;
        dst.srcColor = src.srcColor;
        dst.dstColor = src.dstColor;
        dst.colorOp = src.colorOp;
        dst.srcAlpha = src.srcAlpha;
        dst.dstAlpha = src.dstAlpha;
        dst.alphaOp = src.alphaOp;
    }

    // --- depth state -------------------------------------------------------
    d.hasDepth = desc->hasDepth != 0;
    if (d.hasDepth) {
        d.depthFormat = desc->depth.depthFormat;
        d.depthWrite = desc->depth.depthWrite != 0;
        d.depthCompareOp = desc->depth.depthCompareOp;
    }

    d.topology = desc->topology;
    d.cullEnabled = desc->cullEnabled != 0;
    d.polygonMode = desc->polygonMode;

    // --- input elements ----------------------------------------------------
    if (desc->inputElementCount < 0 || desc->inputElementCount > kMaxInputElements) {
        setLastErrorf(kCreate, "bad inputElementCount %d", static_cast<int>(desc->inputElementCount));
        return nullptr;
    }
    if (desc->inputElementCount > 0 && desc->inputElements == nullptr) {
        return failWithPtr<PipelinePtr>(kCreate, "inputElements is null");
    }
    d.inputElements.resize(static_cast<size_t>(desc->inputElementCount));
    for (int32_t i = 0; i < desc->inputElementCount; ++i) {
        const mcdr_input_element_desc& src = desc->inputElements[i];
        d3d12_mc::PipelineDesc::InputElement& dst = d.inputElements[static_cast<size_t>(i)];
        dst.location = src.location;
        dst.binding = src.binding;
        dst.format = src.format;
        dst.offset = src.offset;
        dst.stride = src.stride;
        dst.stepRate = src.stepRate;
        // NULL or "" keeps std::string empty, which createGraphicsPipeline()
        // already understands as "fall back to TEXCOORD". This is the field the
        // packed Java blob could not express at all: it had to ship the names in
        // a separate trailing section and hope the ordering matched.
        if (src.semanticName != nullptr && src.semanticName[0] != '\0') {
            dst.semanticName = src.semanticName;
        }
    }

    // --- root bindings -----------------------------------------------------
    if (desc->bindingCount < 0 || desc->bindingCount > kMaxPipelineBindings) {
        setLastErrorf(kCreate, "bad bindingCount %d", static_cast<int>(desc->bindingCount));
        return nullptr;
    }
    if (desc->bindingCount > 0 && desc->bindings == nullptr) {
        return failWithPtr<PipelinePtr>(kCreate, "bindings is null");
    }
    d.bindings.resize(static_cast<size_t>(desc->bindingCount));
    for (int32_t i = 0; i < desc->bindingCount; ++i) {
        d.bindings[static_cast<size_t>(i)].type = desc->bindings[i].type;
        d.bindings[static_cast<size_t>(i)].reg = desc->bindings[i].reg;
    }

    std::string err;
    PipelinePtr* pipeline = d3d12_mc::createGraphicsPipeline(d, err);
    if (pipeline == nullptr) {
        // The old bridge printed err to stderr and returned 0, so Java only ever
        // saw "pipeline handle is 0". The message now survives the boundary.
        return failWithPtr<PipelinePtr>(kCreate, err);
    }
    return pipeline;
}

void MCDR_CALL mcdr_dx12DestroyPipeline(PipelinePtr* pipeline) { d3d12_mc::destroyPipeline(pipeline); }

}  // extern "C"
