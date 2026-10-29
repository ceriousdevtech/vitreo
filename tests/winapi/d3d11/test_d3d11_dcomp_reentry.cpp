// SPDX-License-Identifier: LGPL-2.1-only
//
// Guest-interface re-entry from setters and the DirectComposition surface.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest, SetterReleasingAViewsGuestInterfaceReentersAndResumes) {
    // A setter that unbinds a view the application has already released
    // destroys it, and the view releases the interface the application gave
    // it (Learn, SetPrivateDataInterface): a guest Release, run from inside
    // the setter, whose code calls back into the context. Through the
    // direct entries the guest's crossings now reach: the device lock must
    // be free for the callback, the re-entrant call must land, and the
    // setter must resume with its lock and its records consistent.
    com_direct::force_for_test(com_direct::Force::On);
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto exports = create_d3d11_thunks(d3d11);
    const auto entry = [&](const std::string& name) {
        for (const ExportEntry& candidate : exports.entries) {
            if (candidate.name == name) {
                EXPECT_NE(candidate.direct.fn, nullptr) << name;
                return candidate.direct;
            }
        }
        ADD_FAILURE() << name;
        return DirectHandler{};
    };
    const auto call = [](const DirectHandler& direct, Marshaler& m) {
        direct.fn(direct.context, m, direct.slot);
    };
    const DirectHandler create_texture = entry(d3d11_device_method_export_name(5));
    const DirectHandler create_srv = entry(d3d11_device_method_export_name(7));
    const DirectHandler set_interface =
        entry(d3d11_shader_resource_view_method_export_name(6));
    const DirectHandler release_srv =
        entry(d3d11_shader_resource_view_method_export_name(2));
    const DirectHandler ps_set_srvs = entry(d3d11_context_method_export_name(8));
    const DirectHandler set_topology = entry(d3d11_context_method_export_name(24));
    const DirectHandler get_topology = entry(d3d11_context_method_export_name(83));

    constexpr GuestAddr TEX_OUT = DATA_BASE + 0x900;
    constexpr GuestAddr SRV_OUT = DATA_BASE + 0x908;
    constexpr GuestAddr BINDINGS = DATA_BASE + 0x910;
    constexpr GuestAddr TOPOLOGY_OUT = DATA_BASE + 0x918;
    constexpr GuestAddr GUID_A = DATA_BASE + 0x920;
    constexpr GuestAddr GUEST_IFACE = DATA_BASE + 0x940;
    constexpr GuestAddr GUEST_VTABLE = DATA_BASE + 0x960;
    constexpr GuestAddr GUEST_ADDREF = 0x72000010;
    constexpr GuestAddr GUEST_RELEASE = 0x72000020;
    ASSERT_TRUE(memory.write_u64(GUEST_VTABLE + 1 * 8, GUEST_ADDREF).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_VTABLE + 2 * 8, GUEST_RELEASE).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_IFACE, GUEST_VTABLE).is_ok());
    const std::array<u8, 16> guid{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                  0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE,
                                  0xF0, 0x01};
    ASSERT_TRUE(memory.write_bytes(
        GUID_A, reinterpret_cast<const Byte*>(guid.data()), guid.size()).is_ok());

    std::vector<GuestAddr> released;
    bool lock_free_in_callback = false;
    d3d11.set_guest_function_caller(
        [&](GuestAddr function, const u64* args, usize arg_count) -> u64 {
            if (function != GUEST_RELEASE) return 1;  // AddRef
            released.push_back(arg_count != 0 ? args[0] : 0);
            std::thread probe([&] {
                lock_free_in_callback = dxgi.api_mutex().try_lock();
                if (lock_free_in_callback) dxgi.api_mutex().unlock();
            });
            probe.join();
            // The guest destructor calls back into the context on this
            // thread, through the direct entry its crossing reaches.
            cpu::CpuState guest_cpu = cpu;
            guest_cpu.gpr.rcx = context;
            guest_cpu.gpr.rdx = 5;  // D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
            Marshaler guest_marshaler(memory);
            guest_marshaler.attach_cpu(guest_cpu);
            call(set_topology, guest_marshaler);
            return 0;
        });

    const std::array<u32, 11> desc{16, 16, 1, 1, 28, 1, 0, 0, 0x8, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEX_OUT);
    call(create_texture, marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, device);
    set_arg(1, memory.read_u64(TEX_OUT).value());
    set_arg(2, 0);
    set_arg(3, SRV_OUT);
    call(create_srv, marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(SRV_OUT).value();
    set_arg(0, view);
    set_arg(1, GUID_A);
    set_arg(2, GUEST_IFACE);
    call(set_interface, marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);

    ASSERT_TRUE(memory.write_u64(BINDINGS, view).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    call(ps_set_srvs, marshaler);
    set_arg(0, view);
    call(release_srv, marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u) << "the application's last reference";
    EXPECT_TRUE(released.empty()) << "the binding keeps the view alive";

    // Unbind: the view dies inside this setter.
    ASSERT_TRUE(memory.write_u64(BINDINGS, 0).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    call(ps_set_srvs, marshaler);
    ASSERT_EQ(released.size(), 1u);
    EXPECT_EQ(released[0], GUEST_IFACE);
    EXPECT_TRUE(lock_free_in_callback)
        << "device lock held across the guest COM callback";

    // The setter resumed and left the lock wholly released...
    bool lock_free_after = false;
    std::thread after([&] {
        lock_free_after = dxgi.api_mutex().try_lock();
        if (lock_free_after) dxgi.api_mutex().unlock();
    });
    after.join();
    EXPECT_TRUE(lock_free_after);
    // ...and the context answers with the re-entrant call's state.
    ASSERT_TRUE(memory.write_u32(TOPOLOGY_OUT, 0xFFFFu).is_ok());
    set_arg(0, context);
    set_arg(1, TOPOLOGY_OUT);
    call(get_topology, marshaler);
    EXPECT_EQ(memory.read_u32(TOPOLOGY_OUT).value(), 5u);
    com_direct::force_for_test(com_direct::Force::None);
}

TEST_F(D3D11ManagerTest,
       DirectCompositionAppliesTransformClipAndPresentsDeferredSwapChain) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    ASSERT_NE(device, 0u);

    constexpr GuestAddr desc1 = DATA_BASE + 0x800;
    constexpr GuestAddr composition_swap_out = DATA_BASE + 0x840;
    ASSERT_TRUE(memory.write_u32(desc1 + 0, 640).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 4, 480).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 8, 87).is_ok()); // B8G8R8A8
    ASSERT_TRUE(memory.write_u32(desc1 + 12, 0).is_ok()); // stereo
    ASSERT_TRUE(memory.write_u32(desc1 + 16, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 24, 0x20).is_ok()); // RT output
    ASSERT_TRUE(memory.write_u32(desc1 + 28, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 32, 0).is_ok()); // stretch
    ASSERT_TRUE(memory.write_u32(desc1 + 36, 3).is_ok()); // flip sequential
    ASSERT_TRUE(memory.write_u32(desc1 + 40, 1).is_ok()); // premultiplied
    ASSERT_TRUE(memory.write_u32(desc1 + 44, 0).is_ok());

    set_arg(0, 0xFACADE);
    set_arg(1, device);
    set_arg(2, desc1);
    set_arg(3, 0);
    set_arg(4, composition_swap_out);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_CreateSwapChainForComposition(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr swap_chain =
        memory.read_u64(composition_swap_out).value();
    ASSERT_TRUE(d3d11.is_swap_chain_object(swap_chain));
    EXPECT_EQ(backend.state.swap_chain_creations, 0u);
    EXPECT_TRUE(backend.state.last_texture_desc.shareable);

    // A composition swap chain is windowless by construction. Present1 is
    // valid before a visual transaction binds it; the frame is retained and
    // flushed when Commit supplies the composition target.
    set_arg(0, swap_chain);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_SwapChainMethod(marshaler, 22);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(backend.state.swap_chain_creations, 0u);
    EXPECT_EQ(backend.state.presents, 0u);

    DcompManager dcomp_manager{memory, true, d3d11};
    DcompManager::MethodThunks dcomp_thunks{};
    GuestAddr thunk = 0x71000000;
    const auto fill = [&](auto& methods) {
        for (GuestAddr& method : methods) {
            method = thunk;
            thunk += 0x10;
        }
    };
    fill(dcomp_thunks.device);
    fill(dcomp_thunks.device2);
    fill(dcomp_thunks.desktop_device);
    fill(dcomp_thunks.device3);
    fill(dcomp_thunks.target);
    fill(dcomp_thunks.visual2);
    fill(dcomp_thunks.translate_transform);
    fill(dcomp_thunks.scale_transform);
    fill(dcomp_thunks.rotate_transform);
    fill(dcomp_thunks.skew_transform);
    fill(dcomp_thunks.matrix_transform);
    fill(dcomp_thunks.rectangle_clip);
    dcomp_manager.set_method_thunks(dcomp_thunks);
    dcomp_manager.set_window_ownership_resolver(
        [](u64 hwnd) {
            return hwnd == 0xCAFE
                ? DcompManager::WindowOwnership::CurrentProcess
                : DcompManager::WindowOwnership::Invalid;
        });

    constexpr GuestAddr dcomp_iid = DATA_BASE + 0x880;
    constexpr GuestAddr dcomp_device_out = DATA_BASE + 0x8A0;
    constexpr GuestAddr visual_out = DATA_BASE + 0x8A8;
    constexpr GuestAddr target_out = DATA_BASE + 0x8B0;
    constexpr GuestAddr transform_out = DATA_BASE + 0x8B8;
    constexpr GuestAddr clip_out = DATA_BASE + 0x8C0;
    constexpr GuestAddr scale_out = DATA_BASE + 0x8C8;
    constexpr GuestAddr group_members = DATA_BASE + 0x8D0;
    constexpr GuestAddr group_out = DATA_BASE + 0x8E0;
    ASSERT_TRUE(memory.write_bytes(
        dcomp_iid,
        reinterpret_cast<const Byte*>(&IID_IDCOMPOSITIONDESKTOPDEVICE),
        sizeof(IID_IDCOMPOSITIONDESKTOPDEVICE)).is_ok());
    set_arg(0, device);
    set_arg(1, dcomp_iid);
    set_arg(2, dcomp_device_out);
    dcomp_manager.impl_DCompositionCreateDevice3(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    const GuestAddr composition_device =
        memory.read_u64(dcomp_device_out).value();
    ASSERT_NE(composition_device, 0u);
    const GuestAddr device_vtable =
        memory.read_u64(composition_device).value();
    EXPECT_NE(memory.read_u64(device_vtable + 24 * 8).value(), 0u);

    set_arg(0, composition_device);
    set_arg(1, visual_out);
    dcomp_manager.impl_DeviceMethod(
        marshaler, DcompManager::DeviceInterface::Desktop, 6);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    const GuestAddr visual = memory.read_u64(visual_out).value();

    // IDCompositionDevice2::CreateTranslateTransform (slot 10), followed by
    // its static OffsetX/OffsetY setters and Visual::SetTransform(object).
    set_arg(0, composition_device);
    set_arg(1, transform_out);
    dcomp_manager.impl_DeviceMethod(
        marshaler, DcompManager::DeviceInterface::Desktop, 10);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    const GuestAddr transform = memory.read_u64(transform_out).value();
    ASSERT_NE(transform, 0u);
    set_arg(0, transform);
    cpu.sse.xmm[1].f32_[0] = 25.0f;
    dcomp_manager.impl_TransformMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    cpu.sse.xmm[1].f32_[0] = 30.0f;
    dcomp_manager.impl_TransformMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    set_arg(0, composition_device);
    set_arg(1, scale_out);
    dcomp_manager.impl_DeviceMethod(
        marshaler, DcompManager::DeviceInterface::Desktop, 11);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    const GuestAddr scale = memory.read_u64(scale_out).value();
    set_arg(0, scale);
    cpu.sse.xmm[1].f32_[0] = 2.0f;
    dcomp_manager.impl_TransformMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    cpu.sse.xmm[1].f32_[0] = 3.0f;
    dcomp_manager.impl_TransformMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);

    ASSERT_TRUE(memory.write_u64(group_members, transform).is_ok());
    ASSERT_TRUE(memory.write_u64(group_members + 8, scale).is_ok());
    set_arg(0, composition_device);
    set_arg(1, group_members);
    set_arg(2, 2);
    set_arg(3, group_out);
    dcomp_manager.impl_DeviceMethod(
        marshaler, DcompManager::DeviceInterface::Desktop, 15);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    const GuestAddr group = memory.read_u64(group_out).value();
    ASSERT_NE(group, 0u);
    set_arg(0, visual);
    set_arg(1, group);
    dcomp_manager.impl_VisualMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);

    // dcomp.h publishes the animation overload first and the scalar overload
    // second. These calls exercise the guest-visible scalar slots.
    set_arg(0, visual);
    cpu.sse.xmm[1].f32_[0] = 0.0f;
    dcomp_manager.impl_VisualMethod(marshaler, 4);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    dcomp_manager.impl_VisualMethod(marshaler, 6);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);

    // The remaining typed 2D factories are real objects too, with distinct
    // complete vtables even though this tree only needs translate + scale.
    for (usize index = 0; index < 3; ++index) {
        const GuestAddr output = DATA_BASE + 0x900 + index * 8;
        set_arg(0, composition_device);
        set_arg(1, output);
        dcomp_manager.impl_DeviceMethod(
            marshaler, DcompManager::DeviceInterface::Desktop, 12 + index);
        ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
        EXPECT_NE(memory.read_u64(output).value(), 0u);
    }

    // IDCompositionDevice2::CreateRectangleClip (slot 22). The rectangle
    // property methods occupy alternating static/animation slots.
    set_arg(0, composition_device);
    set_arg(1, clip_out);
    dcomp_manager.impl_DeviceMethod(
        marshaler, DcompManager::DeviceInterface::Desktop, 22);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    const GuestAddr clip = memory.read_u64(clip_out).value();
    ASSERT_NE(clip, 0u);
    const std::array<float, 4> clip_values{10.0f, 20.0f, 310.0f, 220.0f};
    for (usize index = 0; index < clip_values.size(); ++index) {
        set_arg(0, clip);
        cpu.sse.xmm[1].f32_[0] = clip_values[index];
        dcomp_manager.impl_RectangleClipMethod(marshaler, 3 + index * 2);
        ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    }
    set_arg(0, visual);
    set_arg(1, clip);
    dcomp_manager.impl_VisualMethod(marshaler, 13);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);

    set_arg(0, visual);
    set_arg(1, swap_chain);
    dcomp_manager.impl_VisualMethod(marshaler, 15);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);

    set_arg(0, composition_device);
    set_arg(1, 0xCAFE);
    set_arg(2, 1);
    set_arg(3, target_out);
    dcomp_manager.impl_DeviceMethod(
        marshaler, DcompManager::DeviceInterface::Desktop, 24);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    const GuestAddr target = memory.read_u64(target_out).value();

    set_arg(0, target);
    set_arg(1, visual);
    dcomp_manager.impl_TargetMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);

    set_arg(0, composition_device);
    dcomp_manager.impl_DeviceMethod(
        marshaler, DcompManager::DeviceInterface::Desktop, 3);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    EXPECT_EQ(backend.state.swap_chain_creations, 1u);
    EXPECT_EQ(backend.state.presents, 1u);
    ASSERT_EQ(backend.state.composition_layers.size(), 1u);
    const auto& layer = backend.state.composition_layers.front();
    // Transform groups multiply in array order: translate(25,30) * scale(2,3).
    EXPECT_FLOAT_EQ(layer.transform[0], 2.0f);
    EXPECT_FLOAT_EQ(layer.transform[3], 3.0f);
    EXPECT_FLOAT_EQ(layer.transform[4], 50.0f);
    EXPECT_FLOAT_EQ(layer.transform[5], 90.0f);
    EXPECT_TRUE(layer.has_clip_rect);
    EXPECT_TRUE(layer.above_window_children);
    EXPECT_EQ(layer.clip_rect, clip_values);

    set_arg(0, swap_chain);
    set_arg(1, 1);
    set_arg(2, 0);
    d3d11.impl_SwapChainMethod(marshaler, 8);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(backend.state.presents, 2u);
}

TEST_F(D3D11ManagerTest,
       DirectCompositionCommitAppliesAncestorClipToChildContent) {
    // Learn SetClip: the clip restricts this visual and its subtree. CEF
    // puts the swap chain on a child visual under a clipped parent; Commit
    // must bind that tree instead of returning E_NOTIMPL.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    ASSERT_NE(device, 0u);

    constexpr GuestAddr desc1 = DATA_BASE + 0x800;
    constexpr GuestAddr composition_swap_out = DATA_BASE + 0x840;
    ASSERT_TRUE(memory.write_u32(desc1 + 0, 700).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 4, 330).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 8, 87).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 16, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 24, 0x20).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 28, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 32, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 36, 3).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 40, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(desc1 + 44, 0).is_ok());

    set_arg(0, 0xFACADE);
    set_arg(1, device);
    set_arg(2, desc1);
    set_arg(3, 0);
    set_arg(4, composition_swap_out);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_CreateSwapChainForComposition(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr swap_chain =
        memory.read_u64(composition_swap_out).value();
    ASSERT_NE(swap_chain, 0u);

    set_arg(0, swap_chain);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_SwapChainMethod(marshaler, 22);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);

    DcompManager dcomp_manager{memory, true, d3d11};
    DcompManager::MethodThunks dcomp_thunks{};
    GuestAddr thunk = 0x71000000;
    const auto fill = [&](auto& methods) {
        for (GuestAddr& method : methods) {
            method = thunk;
            thunk += 0x10;
        }
    };
    fill(dcomp_thunks.device);
    fill(dcomp_thunks.device2);
    fill(dcomp_thunks.desktop_device);
    fill(dcomp_thunks.device3);
    fill(dcomp_thunks.target);
    fill(dcomp_thunks.visual2);
    fill(dcomp_thunks.translate_transform);
    fill(dcomp_thunks.scale_transform);
    fill(dcomp_thunks.rotate_transform);
    fill(dcomp_thunks.skew_transform);
    fill(dcomp_thunks.matrix_transform);
    fill(dcomp_thunks.rectangle_clip);
    dcomp_manager.set_method_thunks(dcomp_thunks);
    dcomp_manager.set_window_ownership_resolver(
        [](u64 hwnd) {
            return hwnd == 0xCAFE
                ? DcompManager::WindowOwnership::CurrentProcess
                : DcompManager::WindowOwnership::Invalid;
        });

    constexpr GuestAddr dcomp_iid = DATA_BASE + 0x880;
    constexpr GuestAddr dcomp_device_out = DATA_BASE + 0x8A0;
    constexpr GuestAddr parent_out = DATA_BASE + 0x8A8;
    constexpr GuestAddr child_out = DATA_BASE + 0x8B0;
    constexpr GuestAddr target_out = DATA_BASE + 0x8B8;
    constexpr GuestAddr clip_out = DATA_BASE + 0x8C0;
    ASSERT_TRUE(memory.write_bytes(
        dcomp_iid,
        reinterpret_cast<const Byte*>(&IID_IDCOMPOSITIONDESKTOPDEVICE),
        sizeof(IID_IDCOMPOSITIONDESKTOPDEVICE)).is_ok());
    set_arg(0, device);
    set_arg(1, dcomp_iid);
    set_arg(2, dcomp_device_out);
    dcomp_manager.impl_DCompositionCreateDevice3(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    const GuestAddr composition_device =
        memory.read_u64(dcomp_device_out).value();
    ASSERT_NE(composition_device, 0u);

    set_arg(0, composition_device);
    set_arg(1, parent_out);
    dcomp_manager.impl_DeviceMethod(
        marshaler, DcompManager::DeviceInterface::Desktop, 6);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    const GuestAddr parent = memory.read_u64(parent_out).value();

    set_arg(0, composition_device);
    set_arg(1, child_out);
    dcomp_manager.impl_DeviceMethod(
        marshaler, DcompManager::DeviceInterface::Desktop, 6);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    const GuestAddr child = memory.read_u64(child_out).value();

    set_arg(0, composition_device);
    set_arg(1, clip_out);
    dcomp_manager.impl_DeviceMethod(
        marshaler, DcompManager::DeviceInterface::Desktop, 22);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    const GuestAddr clip = memory.read_u64(clip_out).value();
    const std::array<float, 4> clip_values{0.0f, 0.0f, 700.0f, 330.0f};
    for (usize index = 0; index < clip_values.size(); ++index) {
        set_arg(0, clip);
        cpu.sse.xmm[1].f32_[0] = clip_values[index];
        dcomp_manager.impl_RectangleClipMethod(marshaler, 3 + index * 2);
        ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    }
    set_arg(0, parent);
    set_arg(1, clip);
    dcomp_manager.impl_VisualMethod(marshaler, 13);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);

    set_arg(0, child);
    set_arg(1, swap_chain);
    dcomp_manager.impl_VisualMethod(marshaler, 15);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);

    set_arg(0, parent);
    set_arg(1, child);
    set_arg(2, 1);
    set_arg(3, 0);
    dcomp_manager.impl_VisualMethod(marshaler, 16);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);

    set_arg(0, composition_device);
    set_arg(1, 0xCAFE);
    set_arg(2, 1);
    set_arg(3, target_out);
    dcomp_manager.impl_DeviceMethod(
        marshaler, DcompManager::DeviceInterface::Desktop, 24);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    const GuestAddr target = memory.read_u64(target_out).value();

    set_arg(0, target);
    set_arg(1, parent);
    dcomp_manager.impl_TargetMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);

    set_arg(0, composition_device);
    dcomp_manager.impl_DeviceMethod(
        marshaler, DcompManager::DeviceInterface::Desktop, 3);
    ASSERT_EQ(cpu.gpr.rax, dcomp::S_OK);
    EXPECT_EQ(backend.state.swap_chain_creations, 1u);
    EXPECT_EQ(backend.state.presents, 1u);
    ASSERT_EQ(backend.state.composition_layers.size(), 1u);
    const auto& layer = backend.state.composition_layers.front();
    EXPECT_TRUE(layer.has_clip_rect);
    EXPECT_EQ(layer.clip_rect, clip_values);
}

TEST_F(D3D11ManagerTest,
       DirectCompositionPublishesCompleteAbiSafeFoundationalSurface) {
    DcompManager manager{memory, true, d3d11};
    auto exports = create_dcomp_thunks(manager);
    const auto find = [&](const std::string& name) -> const ExportEntry* {
        const auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& candidate) {
                return candidate.name == name;
            });
        return entry == exports.entries.end() ? nullptr : &*entry;
    };
    ASSERT_NE(find("DCompositionCreateDevice"), nullptr);
    ASSERT_NE(find("DCompositionCreateDevice2"), nullptr);
    ASSERT_NE(find("DCompositionCreateDevice3"), nullptr);
    ASSERT_NE(find("DCompositionGetTargetStatistics"), nullptr);
    EXPECT_EQ(find("DCompositionCreateDevice3")->stdcall_bytes, 12u);
    EXPECT_EQ(find(dcomp_desktop_method_export_name(24))->stdcall_bytes,
              16u);
    EXPECT_EQ(find(dcomp_visual_method_export_name(16))->stdcall_bytes,
              16u);
    EXPECT_EQ(find(dcomp_visual_method_export_name(18))->stdcall_bytes,
              4u);
    EXPECT_EQ(find(dcomp_matrix_transform_method_export_name(4))->stdcall_bytes,
              16u);
    EXPECT_EQ(find(dcomp_rectangle_clip_method_export_name(26))->stdcall_bytes,
              8u);
    EXPECT_GE(exports.entries.size(), 210u);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
