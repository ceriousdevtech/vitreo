// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_context_annotations.cpp
/// @brief Debug markers: ID3DUserDefinedAnnotation and
///        ID3D11DeviceContext2.
///
/// annotation_marker reads a marker's label and either records it into a
/// deferred context's command list or hands it to emit_debug_marker,
/// which puts it on the GPU command stream as a debug group or
/// signpost.
///
/// Split out of d3d11_device_ext.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi {

void D3D11Manager::annotation_marker(GuestAddr context, DebugMarkerKind kind,
                                     GuestAddr label, const i32* data) {
    // D3D11_FEATURE_DATA_MARKER_SUPPORT.Profile: the markers reach the GPU
    // command stream, where Metal GPU capture and profiling show them as
    // debug groups and signposts. What the application observes stays WARP's
    // with no capture tool attached (measured, d3d11_capsmall_probe
    // "marker"): BeginEvent / EndEvent return -1, GetStatus and
    // IsAnnotationEnabled FALSE, an unbalanced EndEvent is harmless.
    if (!m_backend.supports_debug_markers()) return;
    std::string text;
    if (kind != DebugMarkerKind::Pop) {
        if (label != 0) {
            Marshaler reader(m_memory);
            auto read = reader.read_wide_string(label, 1024);
            if (read.is_ok()) text = std::move(read.value());
        }
        if (data != nullptr) {
            if (!text.empty()) text += ' ';
            text += "(" + std::to_string(*data) + ")";
        }
    }
    if (is_deferred_context(context)) {
        // In command order: a deferred context's markers land where its
        // command list executes.
        auto deferred = m_deferred_contexts.find(context);
        if (deferred == m_deferred_contexts.end() ||
            !deferred->second.recording) {
            return;
        }
        DeferredRecording& recording = *deferred->second.recording;
        RecordedCall call{};
        call.slot = kDebugMarker;
        call.arg_count = 3;
        call.args[0] = static_cast<u64>(kind);
        if (!text.empty()) {
            const GuestAddr copy = arena_allocate(recording.arena, text.size());
            if (copy == 0 ||
                m_memory.write_bytes(copy,
                                     reinterpret_cast<const Byte*>(text.data()),
                                     text.size()).is_err()) {
                return;
            }
            call.args[1] = copy;
            call.args[2] = text.size();
        }
        recording.calls.push_back(std::move(call));
        return;
    }
    emit_debug_marker(kind, text);
}

void D3D11Manager::emit_debug_marker(DebugMarkerKind kind,
                                     const std::string& label) {
    switch (kind) {
    case DebugMarkerKind::Push:
        m_backend.push_debug_group(label);
        break;
    case DebugMarkerKind::Pop:
        m_backend.pop_debug_group();
        break;
    case DebugMarkerKind::Signpost:
        m_backend.insert_debug_signpost(label);
        break;
    }
}

}  // namespace vitreo::winapi
