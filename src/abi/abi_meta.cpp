// abi_meta.cpp — the ABI's own surface: version, layout self-check, error
// reporting and log verbosity.
//
// The layout self-check is the piece JNI never had. Under JNI every one of the
// 115 entry points took `jlong` handles and hand-packed byte buffers, so a
// renamed package, a reordered struct field or a changed enum ordinal produced
// silent memory corruption at runtime with no compile-time or load-time signal
// (the repo already shipped a stale `com_dx12_dx12` symbol prefix once). Here
// the native side publishes one (name, offset, size) triple per mirrored field
// and the Java side compares it against its own MemoryLayouts before any other
// entry point is called.

#include "abi_common.h"
#include "../dx12/dx12_device.h"   // d3d12_mc::setLogLevel

import std;

namespace mcdr::abi {

namespace {

// (name, offset, size) for one mirrored field. sizeof() is applied to an
// unevaluated null dereference because a non-static data member cannot be named
// in a sizeof expression directly.
#define MCDR_FIELD(T, F)                               \
    {#T "." #F, static_cast<uint64_t>(offsetof(T, F)), \
     static_cast<uint64_t>(sizeof((static_cast<T*>(nullptr))->F))}

const std::vector<mcdr_abi_field>& layoutTable() {
    // Function-local static: initialised once, thread-safe under C++11 and
    // later, and never touched again after the first call.
    static const std::vector<mcdr_abi_field> table = {
        MCDR_FIELD(mcdr_binding_slot, type),
        MCDR_FIELD(mcdr_binding_slot, texelFormat),
        MCDR_FIELD(mcdr_binding_slot, buffer),
        MCDR_FIELD(mcdr_binding_slot, view),
        MCDR_FIELD(mcdr_binding_slot, offset),
        MCDR_FIELD(mcdr_binding_slot, length),

        MCDR_FIELD(mcdr_partition_draw, indexBuffer),
        MCDR_FIELD(mcdr_partition_draw, vertexBuffer),
        MCDR_FIELD(mcdr_partition_draw, vertexOffset),
        MCDR_FIELD(mcdr_partition_draw, indexType),
        MCDR_FIELD(mcdr_partition_draw, indexCount),
        MCDR_FIELD(mcdr_partition_draw, firstIndex),
        MCDR_FIELD(mcdr_partition_draw, baseVertex),
        MCDR_FIELD(mcdr_partition_draw, vertexSlot),
        MCDR_FIELD(mcdr_partition_draw, vertexStride),
        MCDR_FIELD(mcdr_partition_draw, _pad0),

        MCDR_FIELD(mcdr_color_target_desc, format),
        MCDR_FIELD(mcdr_color_target_desc, writeMask),
        MCDR_FIELD(mcdr_color_target_desc, blendEnabled),
        MCDR_FIELD(mcdr_color_target_desc, srcColor),
        MCDR_FIELD(mcdr_color_target_desc, dstColor),
        MCDR_FIELD(mcdr_color_target_desc, colorOp),
        MCDR_FIELD(mcdr_color_target_desc, srcAlpha),
        MCDR_FIELD(mcdr_color_target_desc, dstAlpha),
        MCDR_FIELD(mcdr_color_target_desc, alphaOp),

        MCDR_FIELD(mcdr_depth_state_desc, depthFormat),
        MCDR_FIELD(mcdr_depth_state_desc, depthWrite),
        MCDR_FIELD(mcdr_depth_state_desc, depthCompareOp),

        MCDR_FIELD(mcdr_input_element_desc, location),
        MCDR_FIELD(mcdr_input_element_desc, binding),
        MCDR_FIELD(mcdr_input_element_desc, format),
        MCDR_FIELD(mcdr_input_element_desc, offset),
        MCDR_FIELD(mcdr_input_element_desc, stride),
        MCDR_FIELD(mcdr_input_element_desc, stepRate),
        MCDR_FIELD(mcdr_input_element_desc, _pad0),
        MCDR_FIELD(mcdr_input_element_desc, semanticName),

        MCDR_FIELD(mcdr_pipeline_binding_desc, type),
        MCDR_FIELD(mcdr_pipeline_binding_desc, reg),

        MCDR_FIELD(mcdr_pipeline_desc, vsData),
        MCDR_FIELD(mcdr_pipeline_desc, vsLen),
        MCDR_FIELD(mcdr_pipeline_desc, _pad0),
        MCDR_FIELD(mcdr_pipeline_desc, psData),
        MCDR_FIELD(mcdr_pipeline_desc, psLen),
        MCDR_FIELD(mcdr_pipeline_desc, colorCount),
        MCDR_FIELD(mcdr_pipeline_desc, colorTargets),
        MCDR_FIELD(mcdr_pipeline_desc, hasDepth),
        MCDR_FIELD(mcdr_pipeline_desc, _pad1),
        MCDR_FIELD(mcdr_pipeline_desc, depth),
        MCDR_FIELD(mcdr_pipeline_desc, topology),
        MCDR_FIELD(mcdr_pipeline_desc, cullEnabled),
        MCDR_FIELD(mcdr_pipeline_desc, polygonMode),
        MCDR_FIELD(mcdr_pipeline_desc, inputElementCount),
        MCDR_FIELD(mcdr_pipeline_desc, inputElements),
        MCDR_FIELD(mcdr_pipeline_desc, bindingCount),
        MCDR_FIELD(mcdr_pipeline_desc, _pad2),
        MCDR_FIELD(mcdr_pipeline_desc, bindings),
    };
    return table;
}

#undef MCDR_FIELD

}  // namespace

}  // namespace mcdr::abi

extern "C" {

uint32_t MCDR_CALL mcdrABIVersion(void) { return MCDR_ABI_VERSION; }

void MCDR_CALL mcdrABILayout(const mcdr_abi_field** fields, int32_t* count) {
    const std::vector<mcdr_abi_field>& table = mcdr::abi::layoutTable();
    if (fields) {
        *fields = table.data();
    }
    if (count) {
        *count = static_cast<int32_t>(table.size());
    }
}

int32_t MCDR_CALL mcdrLastError(char* buf, int32_t cap) {
    const std::string message = mcdr::abi::lastErrorText();
    if (message.empty()) {
        return 0;
    }
    return mcdr::abi::writeString(message, buf, cap);
}

void MCDR_CALL mcdrSetLogVerbose(int32_t enabled) {
    // Mirrors the old getenv("DX12_LOG_VERBOSE") read, which JNI performed
    // inside dx12CreateDevice(). The decision now belongs to the Java side so
    // the native layer no longer reaches into the process environment.
    d3d12_mc::setLogLevel(enabled ? 3 : 1);
}

}  // extern "C"
