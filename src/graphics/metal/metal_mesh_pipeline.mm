// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_mesh_pipeline.mm
/// @brief MetalMeshPipeline and MetalDevice::create_mesh_pipeline (WS5).

#import <Metal/Metal.h>

#include "graphics/metal/metal_mesh_pipeline.hpp"
#include "graphics/metal/metal_backend_enums.hpp"
#include "graphics/metal/metal_backend_pipeline.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_shader_validation.hpp"
#include "graphics/contracts/format_converter.hpp"

#include <algorithm>
#include <cstdio>

namespace vitreo::graphics {
namespace {

MTLColorWriteMask metal_color_write_mask(unsigned value) {
    MTLColorWriteMask mask = MTLColorWriteMaskNone;
    if ((value & 0x1) != 0) mask |= MTLColorWriteMaskRed;
    if ((value & 0x2) != 0) mask |= MTLColorWriteMaskGreen;
    if ((value & 0x4) != 0) mask |= MTLColorWriteMaskBlue;
    if ((value & 0x8) != 0) mask |= MTLColorWriteMaskAlpha;
    return mask;
}

/// The colour attachments, blend state, write masks, depth/stencil formats,
/// alpha-to-coverage and sample count of a render pipeline, exactly as
/// MetalBackendPipeline maps them (its twin block in
/// metal_backend_pipeline.mm, which this workstream may not edit;
/// integration follow-up: share one function). The mesh path is drawn into
/// the same render pass as the guest's vertex pipelines, so the two must
/// agree attachment for attachment.
void apply_render_targets(MTLMeshRenderPipelineDescriptor* pipeline,
                          const BackendPipelineDesc& desc) {
    const PixelFormat color0_format = desc.color_formats[0] != PixelFormat::Unknown
                                          ? desc.color_formats[0]
                                          : desc.color_format;
    for (unsigned slot = 0; slot < BACKEND_MAX_RENDER_TARGETS; ++slot) {
        const PixelFormat format = slot == 0 ? color0_format : desc.color_formats[slot];
        const FormatInfo attachment = format_table(format);
        if (attachment.mtl_pixel_format == 0) continue;
        auto* target = pipeline.colorAttachments[slot];
        target.pixelFormat = static_cast<MTLPixelFormat>(attachment.mtl_pixel_format);
        const bool independent = desc.independent_blend_enabled;
        // Integer formats are not blendable (D3D11.3 17.1); Metal aborts in
        // validation instead of ignoring BlendEnable.
        const bool blendable = !is_integer_format(format);
        target.blendingEnabled =
            (blendable && (independent ? desc.blend_enabled_targets[slot] : desc.blend_enabled))
                ? YES : NO;
        target.sourceRGBBlendFactor = metal_blend_factor(
            independent ? desc.source_blends[slot] : desc.source_blend);
        target.destinationRGBBlendFactor = metal_blend_factor(
            independent ? desc.destination_blends[slot] : desc.destination_blend);
        target.rgbBlendOperation = metal_blend_operation(
            independent ? desc.blend_operations[slot] : desc.blend_operation);
        target.sourceAlphaBlendFactor = metal_blend_factor(
            independent ? desc.source_blends_alpha[slot] : desc.source_blend_alpha);
        target.destinationAlphaBlendFactor = metal_blend_factor(
            independent ? desc.destination_blends_alpha[slot] : desc.destination_blend_alpha);
        target.alphaBlendOperation = metal_blend_operation(
            independent ? desc.blend_operations_alpha[slot] : desc.blend_operation_alpha);
        target.writeMask = metal_color_write_mask(
            independent ? desc.color_write_masks[slot] : desc.color_write_mask);
    }
    if (desc.depth_attachment) {
        const FormatInfo depth = format_table(desc.depth_format);
        const MTLPixelFormat depth_format =
            depth.is_depth && depth.mtl_pixel_format != 0
                ? static_cast<MTLPixelFormat>(depth.mtl_pixel_format)
                : MTLPixelFormatDepth32Float_Stencil8;
        pipeline.depthAttachmentPixelFormat = depth_format;
        if (depth_format == MTLPixelFormatDepth32Float_Stencil8 ||
            depth_format == MTLPixelFormatDepth24Unorm_Stencil8) {
            pipeline.stencilAttachmentPixelFormat = depth_format;
        }
    }
    if (desc.alpha_to_coverage) pipeline.alphaToCoverageEnabled = YES;
    if (desc.sample_count > 1) pipeline.rasterSampleCount = desc.sample_count;
}

}  // namespace

MetalMeshPipeline::MetalMeshPipeline(void* device, const BackendMeshPipelineDesc& desc) {
    @autoreleasepool {
        id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
        const BackendPipelineDesc& render = desc.render;
        // Read once (metal_minmax_rebuild.hpp): built without the MIN/MAX
        // reduction, the state is rebuilt with it later.
        const unsigned compiled_features = enabled_sampler_features();
        const unsigned source_features = sampler_features_in_msl(render.vertex_shader_msl) |
            sampler_features_in_msl(render.fragment_shader_msl) |
            sampler_features_in_msl(desc.object_shader_msl);
        if (mtl_device == nil || ![mtl_device supportsFamily:MTLGPUFamilyApple7]) {
            m_failure = "mesh shading needs MTLGPUFamilyApple7";
            return;
        }
        if (!render.vertex_attributes.empty() || render.tessellation ||
            !render.rasterization_enabled || desc.threads_per_mesh_threadgroup == 0 ||
            desc.threads_per_object_threadgroup == 0) {
            m_failure = "not a mesh pipeline description";
            return;
        }
        // Named like every other library (VITREO_DUMP_MSL_DIR writes them).
        (void)shader_tag("mesh", render.vertex_shader_msl);
        (void)shader_tag("object", desc.object_shader_msl);
        (void)shader_tag("fragment", render.fragment_shader_msl);
        NSError* error = nil;
        id<MTLLibrary> mesh_lib = [mtl_device
            newLibraryWithSource:metal_shader_source(render.vertex_shader_msl, "")
                         options:metal_compile_options(render.vertex_math_mode,
                                                       render.preserve_invariance)
                           error:&error];
        if (mesh_lib == nil) {
            m_failure = error ? error.localizedDescription.UTF8String : "mesh library";
            return;
        }
        id<MTLFunction> object_fn = nil;
        if (!desc.object_shader_msl.empty()) {
            id<MTLLibrary> object_lib = [mtl_device
                newLibraryWithSource:metal_shader_source(desc.object_shader_msl, "")
                             options:metal_compile_options(desc.object_math_mode, false)
                               error:&error];
            if (object_lib == nil) {
                m_failure = error ? error.localizedDescription.UTF8String : "object library";
                return;
            }
            object_fn = [object_lib newFunctionWithName:@(desc.object_entry_point.c_str())];
        } else if (!desc.object_entry_point.empty()) {
            object_fn = [mesh_lib newFunctionWithName:@(desc.object_entry_point.c_str())];
        }
        id<MTLFunction> mesh_fn = [mesh_lib newFunctionWithName:@(desc.mesh_entry_point.c_str())];
        if ((object_fn == nil && !desc.object_entry_point.empty()) || mesh_fn == nil) {
            m_failure = "no object or mesh function named " + desc.object_entry_point +
                        " / " + desc.mesh_entry_point;
            return;
        }
        id<MTLFunction> fragment_fn = nil;
        if (!render.fragment_shader_msl.empty()) {
            id<MTLLibrary> frag_lib = [mtl_device
                newLibraryWithSource:metal_shader_source(render.fragment_shader_msl, "")
                             options:metal_compile_options(render.fragment_math_mode, false)
                               error:&error];
            fragment_fn = frag_lib ? [frag_lib newFunctionWithName:
                                                    @(render.fragment_entry_point.c_str())]
                                   : nil;
            if (fragment_fn == nil) {
                m_failure = error ? error.localizedDescription.UTF8String : "fragment function";
                return;
            }
        }
        const bool no_fragment = fragment_fn == nil;
        if (no_fragment) {
            // A mesh pipeline must have a fragment function (Metal validation);
            // with no pixel shader nothing writes a colour target (the masks
            // below), and depth / stencil still run.
            id<MTLLibrary> null_lib = [mtl_device
                newLibraryWithSource:@"fragment void vitreo_null_fragment() {}"
                             options:nil
                               error:&error];
            fragment_fn = [null_lib newFunctionWithName:@"vitreo_null_fragment"];
            if (fragment_fn == nil) {
                m_failure = "the empty fragment function";
                return;
            }
        }
        MTLMeshRenderPipelineDescriptor* pipeline = [MTLMeshRenderPipelineDescriptor new];
        configure_metal_shader_validation(pipeline);
        pipeline.objectFunction = object_fn;
        pipeline.meshFunction = mesh_fn;
        pipeline.fragmentFunction = fragment_fn;
        if (object_fn != nil) pipeline.payloadMemoryLength = desc.payload_bytes;
        if (object_fn != nil) pipeline.maxTotalThreadsPerObjectThreadgroup = desc.threads_per_object_threadgroup;
        pipeline.maxTotalThreadsPerMeshThreadgroup = desc.threads_per_mesh_threadgroup;
        apply_render_targets(pipeline, render);
        if (no_fragment) {
            for (unsigned slot = 0; slot < BACKEND_MAX_RENDER_TARGETS; ++slot) {
                pipeline.colorAttachments[slot].writeMask = MTLColorWriteMaskNone;
            }
        }
        pipeline.label = desc.object_shader_msl.empty() && !desc.object_entry_point.empty()
                             ? @"vitreo-gs-mesh" : @"vitreo-d3d12-mesh";
        id<MTLRenderPipelineState> state =
            [mtl_device newRenderPipelineStateWithMeshDescriptor:pipeline
                                                         options:MTLPipelineOptionNone
                                                      reflection:nil
                                                           error:&error];
        if (state == nil) {
            m_failure = error ? error.localizedDescription.UTF8String : "mesh pipeline";
            return;
        }
        m_object_threads = desc.threads_per_object_threadgroup;
        m_mesh_threads = desc.threads_per_mesh_threadgroup;
        report_metal_shader_validation(state);
        m_state = (__bridge_retained void*)state;
        if ((source_features & ~compiled_features) != 0) {
            BackendMeshPipelineDesc kept = desc;
            kept.render.vertex_shader_msl.clear();
            kept.render.fragment_shader_msl.clear();
            kept.object_shader_msl.clear();
            auto mesh = intern_msl_source(render.vertex_shader_msl);
            auto fragment = intern_msl_source(render.fragment_shader_msl);
            auto object = intern_msl_source(desc.object_shader_msl);
            m_sampler_features = std::make_unique<SamplerFeatureRebuild>(
                source_features, compiled_features, [device, kept = std::move(kept), mesh, fragment, object]() mutable -> void* {
                    kept.render.vertex_shader_msl = *mesh;
                    kept.render.fragment_shader_msl = *fragment;
                    kept.object_shader_msl = *object;
                    MetalMeshPipeline fresh(device, kept);
                    return fresh.m_state.exchange(nullptr);
                });
        }
    }
}

MetalMeshPipeline::~MetalMeshPipeline() {
    if (void* state = m_state.load(); state != nullptr) CFRelease(state);
}

std::unique_ptr<BackendPipeline> MetalDevice::create_mesh_pipeline(
    const BackendMeshPipelineDesc& desc) {
    if (!available() || !m_mesh_shaders) return nullptr;
    auto pipeline = std::make_unique<MetalMeshPipeline>(m_device, desc);
    if (!pipeline->valid()) {
        std::fprintf(stderr, "[metal] mesh pipeline creation failed: %s\n",
                     pipeline->failure().c_str());
        last_pipeline_failure() = "Metal mesh pipeline: " + pipeline->failure();
    }
    // Returned even when invalid, so the caller can read failure().
    return pipeline;
}

}  // namespace vitreo::graphics
