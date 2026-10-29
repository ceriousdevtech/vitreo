// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: dxgi.dll, d3d10 / d3d11 / d3d12 and the dxcore entry points

#include "dll_registry_internal.hpp"

#include "winapi/graphics/d3dkmt/d3dkmt_dxcore.hpp"

namespace vitreo::cli {

void register_dxgi_d3d_dlls(RuntimeContext& ctx) {
    auto& reg = ctx.thunk_registry;

    {
        // dxgi.dll — DXGI 1.1 factory and Metal adapter enumeration.
        (void)reg.register_dll(
            "dxgi.dll", winapi::create_dxgi_thunks(ctx.dxgi_mgr));
        const auto dxgi_thunk = [&](const char* name) {
            const auto address = reg.get_thunk_address("dxgi.dll", name);
            return address.is_ok() ? address.value() : GuestAddr{0};
        };
        winapi::DxgiManager::MethodThunks dxgi_methods{
            .query_interface = dxgi_thunk("_DXGI_QueryInterface"),
            .add_ref = dxgi_thunk("_DXGI_AddRef"),
            .release = dxgi_thunk("_DXGI_Release"),
            .set_private_data = dxgi_thunk("_DXGI_SetPrivateData"),
            .set_private_data_interface =
                dxgi_thunk("_DXGI_SetPrivateDataInterface"),
            .get_private_data = dxgi_thunk("_DXGI_GetPrivateData"),
            .get_parent = dxgi_thunk("_DXGI_GetParent"),
            .enum_adapters = dxgi_thunk("_DXGI_EnumAdapters"),
            .make_window_association =
                dxgi_thunk("_DXGI_MakeWindowAssociation"),
            .get_window_association =
                dxgi_thunk("_DXGI_GetWindowAssociation"),
            .create_swap_chain = dxgi_thunk("_DXGI_CreateSwapChain"),
            .create_software_adapter =
                dxgi_thunk("_DXGI_CreateSoftwareAdapter"),
            .enum_adapters1 = dxgi_thunk("_DXGI_EnumAdapters1"),
            .is_current = dxgi_thunk("_DXGI_IsCurrent"),
            .is_windowed_stereo_enabled =
                dxgi_thunk("_DXGI_IsWindowedStereoEnabled"),
            .create_swap_chain_for_hwnd =
                dxgi_thunk("_DXGI_CreateSwapChainForHwnd"),
            .create_swap_chain_for_core_window =
                dxgi_thunk("_DXGI_CreateSwapChainForCoreWindow"),
            .create_swap_chain_for_composition =
                dxgi_thunk("_DXGI_CreateSwapChainForComposition"),
            .get_shared_resource_adapter_luid =
                dxgi_thunk("_DXGI_GetSharedResourceAdapterLuid"),
            .register_stereo_status_window =
                dxgi_thunk("_DXGI_RegisterStereoStatusWindow"),
            .register_stereo_status_event =
                dxgi_thunk("_DXGI_RegisterStereoStatusEvent"),
            .unregister_stereo_status =
                dxgi_thunk("_DXGI_UnregisterStereoStatus"),
            .register_occlusion_status_window =
                dxgi_thunk("_DXGI_RegisterOcclusionStatusWindow"),
            .register_occlusion_status_event =
                dxgi_thunk("_DXGI_RegisterOcclusionStatusEvent"),
            .get_creation_flags = dxgi_thunk("_DXGI_GetCreationFlags"),
            .enum_adapter_by_luid = dxgi_thunk("_DXGI_EnumAdapterByLuid"),
            .enum_warp_adapter = dxgi_thunk("_DXGI_EnumWarpAdapter"),
            .factory_check_feature_support =
                dxgi_thunk("_DXGI_FactoryCheckFeatureSupport"),
            .enum_adapter_by_gpu_preference =
                dxgi_thunk("_DXGI_EnumAdapterByGpuPreference"),
            .register_adapters_changed_event =
                dxgi_thunk("_DXGI_RegisterAdaptersChangedEvent"),
            .unregister_adapters_changed_event =
                dxgi_thunk("_DXGI_UnregisterAdaptersChangedEvent"),
            .unregister_occlusion_status =
                dxgi_thunk("_DXGI_UnregisterOcclusionStatus"),
            .enum_outputs = dxgi_thunk("_DXGI_EnumOutputs"),
            .get_desc = dxgi_thunk("_DXGI_GetDesc"),
            .check_interface_support =
                dxgi_thunk("_DXGI_CheckInterfaceSupport"),
            .get_desc1 = dxgi_thunk("_DXGI_GetDesc1"),
            .get_desc2 = dxgi_thunk("_DXGI_GetDesc2"),
            .register_hwcp_teardown_event = dxgi_thunk(
                "_DXGI_RegisterHardwareContentProtectionTeardownStatusEvent"),
            .unregister_hwcp_teardown = dxgi_thunk(
                "_DXGI_UnregisterHardwareContentProtectionTeardownStatus"),
            .query_video_memory_info =
                dxgi_thunk("_DXGI_QueryVideoMemoryInfo"),
            .set_video_memory_reservation =
                dxgi_thunk("_DXGI_SetVideoMemoryReservation"),
            .register_budget_change_event = dxgi_thunk(
                "_DXGI_RegisterVideoMemoryBudgetChangeNotificationEvent"),
            .unregister_budget_change = dxgi_thunk(
                "_DXGI_UnregisterVideoMemoryBudgetChangeNotification"),
            .get_desc3 = dxgi_thunk("_DXGI_GetDesc3"),
        };
        for (usize slot = 0;
             slot < winapi::DxgiManager::OUTPUT_METHOD_COUNT; ++slot) {
            dxgi_methods.output[slot] = dxgi_thunk(
                winapi::dxgi_output_method_export_name(slot).c_str());
        }
        ctx.dxgi_mgr.set_method_thunks(std::move(dxgi_methods));

        // d3d11.dll — D3D11 device/context foundation over the same shared
        // Metal backend used by Vulkan and the other graphics frontends.
        (void)reg.register_dll(
            "d3d11.dll", winapi::create_d3d11_thunks(ctx.d3d11_mgr));
        ctx.d3d11_mgr.set_d3d12_device_query([&ctx](GuestAddr device) {
            return ctx.d3d12_mgr.is_device(device);
        });
        winapi::D3D11Manager::MethodThunks d3d11_methods;
        for (usize slot = 0;
             slot < winapi::d3d11::DEVICE_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_device_method_export_name(slot));
            d3d11_methods.device[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::CONTEXT_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_context_method_export_name(slot));
            d3d11_methods.context[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::DXGI_DEVICE_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_dxgi_device_method_export_name(slot));
            d3d11_methods.dxgi_device[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::BUFFER_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_buffer_method_export_name(slot));
            d3d11_methods.buffer[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::TEXTURE_2D_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_texture_2d_method_export_name(slot));
            d3d11_methods.texture_2d[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::DXGI_RESOURCE_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_dxgi_resource_method_export_name(slot));
            d3d11_methods.dxgi_resource[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::KEYED_MUTEX_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_keyed_mutex_method_export_name(slot));
            d3d11_methods.keyed_mutex[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot <
                 winapi::d3d11::SHADER_RESOURCE_VIEW_METHOD_COUNT;
             ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_shader_resource_view_method_export_name(
                    slot));
            d3d11_methods.shader_resource_view[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::UNORDERED_ACCESS_VIEW_METHOD_COUNT;
             ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_unordered_access_view_method_export_name(slot));
            d3d11_methods.unordered_access_view[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::RENDER_TARGET_VIEW_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_render_target_view_method_export_name(slot));
            d3d11_methods.render_target_view[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::DEPTH_STENCIL_VIEW_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_depth_stencil_view_method_export_name(slot));
            d3d11_methods.depth_stencil_view[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::QUERY_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_query_method_export_name(slot));
            d3d11_methods.query[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::FENCE_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_fence_method_export_name(slot));
            d3d11_methods.fence[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::COMMAND_LIST_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_command_list_method_export_name(slot));
            d3d11_methods.command_list[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::SWAP_CHAIN_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_swap_chain_method_export_name(slot));
            d3d11_methods.swap_chain[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::SWAP_CHAIN_MEDIA_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_swap_chain_media_method_export_name(slot));
            d3d11_methods.swap_chain_media[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::MULTITHREAD_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_multithread_method_export_name(slot));
            d3d11_methods.multithread[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::CLASS_LINKAGE_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_class_linkage_method_export_name(slot));
            d3d11_methods.class_linkage[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::CLASS_INSTANCE_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_class_instance_method_export_name(slot));
            d3d11_methods.class_instance[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::ANNOTATION_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_annotation_method_export_name(slot));
            d3d11_methods.annotation[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::VERTEX_SHADER_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_vertex_shader_method_export_name(slot));
            d3d11_methods.vertex_shader[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::PIXEL_SHADER_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_pixel_shader_method_export_name(slot));
            d3d11_methods.pixel_shader[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::COMPUTE_SHADER_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_compute_shader_method_export_name(slot));
            d3d11_methods.compute_shader[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::GEOMETRY_SHADER_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_geometry_shader_method_export_name(slot));
            d3d11_methods.geometry_shader[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::HULL_SHADER_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll", winapi::d3d11_hull_shader_method_export_name(slot));
            d3d11_methods.hull_shader[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::DOMAIN_SHADER_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_domain_shader_method_export_name(slot));
            d3d11_methods.domain_shader[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::DXGI_SURFACE_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll", winapi::d3d11_dxgi_surface_method_export_name(slot));
            d3d11_methods.dxgi_surface[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::DEVICE_CONTEXT_STATE_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_device_context_state_method_export_name(slot));
            d3d11_methods.device_context_state[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::INPUT_LAYOUT_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_input_layout_method_export_name(slot));
            d3d11_methods.input_layout[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::BLEND_STATE_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_blend_state_method_export_name(slot));
            d3d11_methods.blend_state[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::RASTERIZER_STATE_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_rasterizer_state_method_export_name(slot));
            d3d11_methods.rasterizer_state[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::DEPTH_STENCIL_STATE_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_depth_stencil_state_method_export_name(slot));
            d3d11_methods.depth_stencil_state[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::SAMPLER_STATE_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_sampler_state_method_export_name(slot));
            d3d11_methods.sampler_state[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (usize slot = 0;
             slot < winapi::d3d11::VIDEO_CONTEXT_METHOD_COUNT; ++slot) {
            const auto address = reg.get_thunk_address(
                "d3d11.dll",
                winapi::d3d11_video_context_method_export_name(slot));
            d3d11_methods.video_context[slot] =
                address.is_ok() ? address.value() : GuestAddr{0};
        }
        for (const winapi::D3D10VtableSpec& spec :
             winapi::d3d10_vtable_specs()) {
            GuestAddr* thunks = spec.thunks(d3d11_methods);
            for (usize slot = 0; slot < spec.slots; ++slot) {
                const auto address = reg.get_thunk_address(
                    "d3d11.dll", winapi::d3d10_method_export_name(
                                     spec.interface_name, slot));
                thunks[slot] = address.is_ok() ? address.value() : GuestAddr{0};
            }
        }
        ctx.d3d11_mgr.set_method_thunks(std::move(d3d11_methods));

        // -- d3d10.dll / d3d10_1.dll / d3d10core.dll / d3d10_1core.dll --
        //
        // Direct3D 10 is layered on D3D11Manager (winapi/graphics/d3d10/):
        // the module entry points take its device lock, and the D3D10 facet
        // vtables' hidden thunks live in d3d11.dll's table above. Each module
        // carries its own export names and ordinals (dumpbin on Windows):
        // d3d10.dll has D3D10CreateDevice(AndSwapChain), d3d10_1.dll the `1`
        // pair, and the two core modules are the thunk DLLs Windows ships.
        (void)reg.register_dll("d3d10.dll",
                               winapi::create_d3d10_thunks(ctx.d3d11_mgr));
        (void)reg.register_dll("d3d10_1.dll",
                               winapi::create_d3d10_1_thunks(ctx.d3d11_mgr));
        (void)reg.register_dll("d3d10core.dll",
                               winapi::create_d3d10core_thunks(false));
        (void)reg.register_dll("d3d10_1core.dll",
                               winapi::create_d3d10core_thunks(true));

        // -- d3d12.dll --
        //
        // Registered unconditionally so a guest's imports resolve and its calls
        // reach a real implementation; whether a DEVICE is served is a separate
        // decision, made in D3D12CreateDevice. See d3d12::d3d12_enabled().
        {
            (void)reg.register_dll(
                "d3d12.dll", winapi::create_d3d12_thunks(ctx.d3d12_mgr));
            winapi::D3D12Manager::MethodThunks d3d12_methods{};
            const auto load_d3d12_vtable = [&reg](const char* interface_name,
                                                  GuestAddr* thunks,
                                                  usize count) {
                for (usize slot = 0; slot < count; ++slot) {
                    const auto address = reg.get_thunk_address(
                        "d3d12.dll",
                        winapi::d3d12_method_export_name(interface_name, slot));
                    thunks[slot] = address.is_ok() ? address.value()
                                                   : GuestAddr{0};
                }
            };
            // Every interface in the generated table (d3d12_vtables.hpp).
            for (usize table = 0; table < winapi::d3d12::INTERFACE_TABLE_COUNT;
                 ++table) {
                const auto& row = winapi::d3d12::INTERFACE_TABLES[table];
                d3d12_methods.tables[table].resize(row.method_count);
                load_d3d12_vtable(row.export_name,
                                  d3d12_methods.tables[table].data(),
                                  row.method_count);
            }
            load_d3d12_vtable("Blob", d3d12_methods.blob.data(),
                              d3d12_methods.blob.size());
            load_d3d12_vtable(
                "RootSignatureDeserializer",
                d3d12_methods.root_signature_deserializer.data(),
                d3d12_methods.root_signature_deserializer.size());
            load_d3d12_vtable(
                "VersionedRootSignatureDeserializer",
                d3d12_methods.versioned_root_signature_deserializer.data(),
                d3d12_methods.versioned_root_signature_deserializer.size());
            ctx.d3d12_mgr.set_method_thunks(std::move(d3d12_methods));
        }

        // dxcore.dll
        winapi::DllExports dxcore;
        dxcore.entries.push_back({"DXCoreCreateAdapterFactory", 0,
            [](winapi::Marshaler& m) {
                (void)m.arg_ptr(0);
                (void)m.arg_ptr(1);
                m.set_return(0x80004002u); // E_NOINTERFACE
            }, 8});
        (void)reg.register_dll("dxcore.dll", std::move(dxcore));

        // ext-ms-win-dx-d3dkmt-dxcore-l1-1-{0,1,3,4,5}.dll. D3D12Core
        // imports these by ordinal. Identity from dumpbin gdi32 forwards on
        // Windows; bodies STATUS_NOT_IMPLEMENTED (no WDDM kernel).
        for (const char* kmt_dll : {
                 "ext-ms-win-dx-d3dkmt-dxcore-l1-1-0.dll",
                 "ext-ms-win-dx-d3dkmt-dxcore-l1-1-1.dll",
                 "ext-ms-win-dx-d3dkmt-dxcore-l1-1-3.dll",
                 "ext-ms-win-dx-d3dkmt-dxcore-l1-1-4.dll",
                 "ext-ms-win-dx-d3dkmt-dxcore-l1-1-5.dll"}) {
            auto kmt = winapi::create_d3dkmt_dxcore_thunks(kmt_dll);
            (void)reg.register_dll(kmt_dll, std::move(kmt));
        }
    }
}

} // namespace vitreo::cli
