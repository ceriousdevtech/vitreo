// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#import <Metal/Metal.h>

#include <cstdio>
#include <cstdlib>

namespace vitreo::graphics {

/// Opt-in shader instrumentation without enabling the global Metal API
/// validation layer. Apple MTL*PipelineDescriptor.shaderValidation, macOS 15+.
/// Requires MTL_SHADER_VALIDATION=1 before Metal initialization; the descriptor
/// selects instrumentation but cannot enable the validation engine itself.
/// Applies to guest render, compute and mesh pipeline creation only.
inline bool metal_shader_validation_requested() {
    static const bool requested = [] {
        const char* value = std::getenv("VITREO_METAL_SHADER_VALIDATION");
        if (value == nullptr || value[0] == '\0' || value[0] == '0') return false;
        if (@available(macOS 15.0, *)) {
            std::fprintf(stderr, "[metal] per-pipeline shader validation requested\n");
            return true;
        }
        std::fprintf(stderr, "[metal] per-pipeline shader validation unavailable: macOS 15 required\n");
        return false;
    }();
    return requested;
}

template <class Descriptor>
inline void configure_metal_shader_validation(Descriptor* descriptor) {
    if (!metal_shader_validation_requested()) return;
    if (@available(macOS 15.0, *)) descriptor.shaderValidation = MTLShaderValidationEnabled;
}

template <class State>
inline void report_metal_shader_validation(State state) {
    const bool requested = metal_shader_validation_requested();
    const char* global = std::getenv("MTL_SHADER_VALIDATION");
    if (!requested && (global == nullptr || global[0] != '1')) return;
    if (@available(macOS 15.0, *)) {
        const char* label = state.label != nil ? state.label.UTF8String : "unnamed pipeline";
        if (state.shaderValidation == MTLShaderValidationEnabled) {
            std::fprintf(stderr, "[metal] shader validation active: %s\n", label);
        } else if (requested) {
            std::fprintf(stderr, "[metal] shader validation NOT active: %s\n", label);
        }
    }
}

} // namespace vitreo::graphics
