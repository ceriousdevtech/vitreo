// SPDX-License-Identifier: LGPL-2.1-only
//
// Device entry points, per-entry device locking, format support, feature levels and the CreateDevice parameter rules.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

namespace doubles {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../oracle_data/d3d11_doubles_shaders.h"
#pragma clang diagnostic pop
}  // namespace doubles

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest, CreatesNativeWidthDeviceAndImmediateContext) {
    create_default_device();

    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    ASSERT_NE(device, 0u);
    ASSERT_NE(context, 0u);
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(),
              d3d11::FEATURE_LEVEL_11_0);

    const GuestAddr device_vtable = memory.read_u64(device).value();
    const GuestAddr context_vtable = memory.read_u64(context).value();
    EXPECT_NE(memory.read_u64(device_vtable + 37 * 8).value(), 0u);
    EXPECT_NE(memory.read_u64(device_vtable + 40 * 8).value(), 0u);
    EXPECT_NE(memory.read_u64(context_vtable + 3 * 8).value(), 0u);
    EXPECT_NE(memory.read_u64(context_vtable + 112 * 8).value(), 0u);

    cpu.gpr.rcx = device;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 37);
    EXPECT_EQ(cpu.gpr.rax, d3d11::FEATURE_LEVEL_11_0);
}

TEST_F(D3D11ManagerTest, NullClearsUnavailableShaderStages) {
    create_default_device();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // GS/HS/DS SetShaderResources(StartSlot, NumViews, nullptr) unbinds the
    // requested range. SetShader(nullptr, nullptr, 0) disables HS/DS.
    for (const usize slot : {31u, 59u, 63u}) {
        set_arg(0, context);
        set_arg(1, 0);
        set_arg(2, 8);
        set_arg(3, 0);
        cpu.gpr.rax = 0xFFFFFFFFFFFFFFFFull;
        d3d11.impl_ContextMethod(marshaler, slot);
        EXPECT_EQ(cpu.gpr.rax, 0u) << "slot " << slot;
    }
    for (const usize slot : {60u, 64u}) {
        set_arg(0, context);
        set_arg(1, 0);
        set_arg(2, 0);
        set_arg(3, 0);
        cpu.gpr.rax = 0xFFFFFFFFFFFFFFFFull;
        d3d11.impl_ContextMethod(marshaler, slot);
        EXPECT_EQ(cpu.gpr.rax, 0u) << "slot " << slot;
    }
}

// Learn ID3D11Device::CheckFormatSupport: S_OK when successful, E_INVALIDARG
// when Format is NULL (DXGI_FORMAT_UNKNOWN), E_FAIL when the described format
// does not exist. A real DXGI format the device cannot use is not an error —
// it reports S_OK with an empty support mask. Returning E_INVALIDARG for every
// format Vitreo does not implement made Chrome's DirectComposition probe treat
// NV12/YUY2/P010 as a hard failure and disable the path that delivers renderer
// page content to the compositor.
TEST_F(D3D11ManagerTest, CheckFormatSupportSeparatesUnsupportedFromInvalid) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr SUPPORT_OUT = INTERFACE_OUT;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const auto check = [&](u32 format) {
        ASSERT_TRUE(memory.write_u32(SUPPORT_OUT, 0xDEADBEEFu).is_ok());
        cpu.gpr.rcx = device;
        cpu.gpr.rdx = format;
        cpu.gpr.r8 = SUPPORT_OUT;
        d3d11.impl_DeviceMethod(marshaler, 29);
    };

    // A format the backend implements: S_OK and a non-empty mask.
    check(87);  // DXGI_FORMAT_B8G8R8A8_UNORM
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u32(SUPPORT_OUT).value(), 0u);

    // The packed video formats are implemented through their element views:
    // the whole Windows row (d3d11_video_formats_probe [caps], "cfs=").
    const std::pair<u32, u32> packed[] = {
        {107u, 0x02820320u},  // YUY2
        {100u, 0x0282f320u},  // AYUV
        {108u, 0x02820320u},  // Y210
    };
    for (const auto& [format, mask] : packed) {
        check(format);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "format " << format;
        EXPECT_EQ(memory.read_u32(SUPPORT_OUT).value(), mask)
            << "format " << format;
    }
    // NV12 and P010 are implemented (planar Texture2Ds): the Windows row
    // (d3d11_nv12_probe "[NV12] CheckFormatSupport ... 0xc282c320", "[P010]
    // ... 0x0282c320") less the video-engine bits nothing here serves.
    check(103u);
    EXPECT_EQ(memory.read_u32(SUPPORT_OUT).value(), 0x0282c320u);
    check(104u);
    EXPECT_EQ(memory.read_u32(SUPPORT_OUT).value(), 0x0282c320u);

    // Corrected by Windows (d3d11_a01_device_caps_probe64.expected,
    // "fmt   0: cfs=00000000/00020001"): DXGI_FORMAT_UNKNOWN is not an
    // argument error but the format of structured and raw buffers, S_OK with
    // BUFFER | CPU_LOCKABLE.
    check(0);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SUPPORT_OUT).value(), 0x00020001u);

    // Corrected by Windows: values the runtime has no entry for -- the
    // unassigned 116..129 and assigned-but-unsupported R1_UNORM, NV11,
    // P208 -- are E_FAIL, not S_OK with 0.
    for (const u32 format : {66u, 110u, 116u, 129u, 130u, 191u}) {
        check(format);
        EXPECT_EQ(cpu.gpr.rax, d3d11::E_FAIL) << "format " << format;
    }

    // A value that names no DXGI format at all is the E_FAIL case.
    check(4242);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_FAIL);

    // A null out-pointer stays a genuine bad argument.
    cpu.gpr.rcx = device;
    cpu.gpr.rdx = 87;
    cpu.gpr.r8 = 0;
    d3d11.impl_DeviceMethod(marshaler, 29);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
}

TEST_F(D3D11ManagerTest, CheckFormatSupportAdvertisesEveryNativeIaFormat) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr SUPPORT_OUT = DATA_BASE + 0x278;

    for (const auto& format : d3d11::kVertexFormats) {
        ASSERT_TRUE(memory.write_u32(SUPPORT_OUT, 0).is_ok());
        set_arg(0, device);
        set_arg(1, format.dxgi_format);
        set_arg(2, SUPPORT_OUT);
        d3d11.impl_DeviceMethod(marshaler, 29);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK) << format.dxgi_format;
        EXPECT_EQ(memory.read_u32(SUPPORT_OUT).value() & 0x3u, 0x3u)
            << format.dxgi_format;
    }
}

// Measured on Windows (tools/oracle_probe/d3d11_video_formats_probe
// [video], x86 = x64, with and without D3D11_CREATE_DEVICE_VIDEO_SUPPORT):
// the device answers no ID3D11VideoDevice (E_NOINTERFACE), while the
// immediate context answers ID3D11VideoContext through ID3D11VideoContext3 as
// one COM object with it. This runtime used to answer ID3D11VideoDevice with
// no decoder profiles and E_NOTIMPL processors -- a video device Windows'
// own device does not have, which sends Media Foundation down a video path
// that then fails instead of falling back.
TEST_F(D3D11ManagerTest, VideoInterfacesMatchWindowsNoVideoDeviceButAContext) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto query = [&](GuestAddr self, const GuidBytes& iid,
                           void (D3D11Manager::*method)(Marshaler&, usize)) {
        EXPECT_TRUE(memory.write_bytes(
            IID_ADDRESS, reinterpret_cast<const Byte*>(iid.data()),
            iid.size()).is_ok());
        EXPECT_TRUE(memory.write_u64(INTERFACE_OUT, 0xA5A5).is_ok());
        set_arg(0, self);
        set_arg(1, IID_ADDRESS);
        set_arg(2, INTERFACE_OUT);
        (d3d11.*method)(marshaler, 0);
        return std::pair{static_cast<u32>(cpu.gpr.rax),
                         memory.read_u64(INTERFACE_OUT).value()};
    };
    GuidBytes video_device{};
    std::memcpy(video_device.data(), &IID_ID3D11VIDEODEVICE,
                video_device.size());
    const auto [device_hr, device_out] =
        query(device, video_device, &D3D11Manager::impl_DeviceMethod);
    EXPECT_EQ(device_hr, d3d11::E_NOINTERFACE);
    EXPECT_EQ(device_out, 0u);

    const auto [video_hr, video] = query(
        context, winapi::IID_ID3D11VIDEOCONTEXT,
        &D3D11Manager::impl_ContextMethod);
    ASSERT_EQ(video_hr, d3d11::S_OK);
    ASSERT_NE(video, 0u);
    EXPECT_NE(video, context);
    for (const GuidBytes* iid : {&winapi::IID_ID3D11VIDEOCONTEXT1,
                                 &winapi::IID_ID3D11VIDEOCONTEXT2,
                                 &winapi::IID_ID3D11VIDEOCONTEXT3}) {
        const auto [hr, out] =
            query(context, *iid, &D3D11Manager::impl_ContextMethod);
        EXPECT_EQ(hr, d3d11::S_OK);
        EXPECT_EQ(out, video);
    }
    // The device does not answer the context's facet.
    EXPECT_EQ(query(device, winapi::IID_ID3D11VIDEOCONTEXT,
                    &D3D11Manager::impl_DeviceMethod).first,
              d3d11::E_NOINTERFACE);
    // One COM object with the context: QI back to ID3D11DeviceContext and
    // IUnknown give the context (measured "VideoContext QI Context same=1").
    EXPECT_EQ(query(video, winapi::IID_ID3D11DEVICECONTEXT,
                    &D3D11Manager::impl_VideoContextMethod).second, context);
    EXPECT_EQ(query(video, winapi::IID_IUNKNOWN,
                    &D3D11Manager::impl_VideoContextMethod).second, context);
    // The guest-visible vtable runs through ID3D11VideoContext3's last slot.
    const GuestAddr vtable = memory.read_u64(video).value();
    for (usize slot = 0; slot < d3d11::VIDEO_CONTEXT_METHOD_COUNT; ++slot) {
        EXPECT_NE(memory.read_u64(vtable + slot * 8).value(), 0u) << slot;
    }
    // ID3D11DeviceChild::GetDevice returns the device (measured same=1).
    set_arg(0, video);
    set_arg(1, INTERFACE_OUT);
    d3d11.impl_VideoContextMethod(marshaler, 3);
    EXPECT_EQ(memory.read_u64(INTERFACE_OUT).value(), device);
    // No decoder or processor can exist without an ID3D11VideoDevice: an
    // HRESULT method fails, a void one leaves its outputs alone.
    set_arg(0, video);
    set_arg(1, 0);  // pVideoProcessor
    set_arg(2, 0);
    set_arg(3, 0);
    set_arg(4, 0);
    d3d11.impl_VideoContextMethod(marshaler, 53);  // VideoProcessorBlt
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::E_INVALIDARG);
    constexpr GuestAddr enabled_out = DATA_BASE + 0x920;
    ASSERT_TRUE(memory.write_u32(enabled_out, 0x5A5A5A5Au).is_ok());
    set_arg(0, video);
    set_arg(1, 0);
    set_arg(2, enabled_out);
    set_arg(3, 0);
    d3d11.impl_VideoContextMethod(marshaler, 20);  // GetOutputTargetRect
    EXPECT_EQ(memory.read_u32(enabled_out).value(), 0x5A5A5A5Au);

    // x86 stdcall cleanup from the SDK signatures (audit_d3d11_vtables.py):
    // VideoProcessorBlt pops 24 bytes, DecoderBeginFrame1 32.
    auto exports = create_d3d11_thunks(d3d11);
    const auto stdcall_bytes = [&](usize slot) {
        const auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [slot](const ExportEntry& candidate) {
                return candidate.name ==
                       d3d11_video_context_method_export_name(slot);
            });
        return entry == exports.entries.end() ? 0u : entry->stdcall_bytes;
    };
    EXPECT_EQ(stdcall_bytes(53), 24u);
    EXPECT_EQ(stdcall_bytes(83), 32u);
    EXPECT_EQ(stdcall_bytes(84), 16u);
}

TEST_F(D3D11ManagerTest, EveryGuestEntryHoldsTheDeviceLock) {
    EXPECT_EQ(&d3d11.api_mutex(), &dxgi.api_mutex());
    // Learn: ID3D11Device methods are free-threaded; the runtime serializes
    // them (and the immediate context) with a device lock unless the device
    // is D3D11_CREATE_DEVICE_SINGLETHREADED. UE creates textures on worker
    // threads while its render thread drives the context; with no lock a
    // CreateTexture2D rehashed a map under a concurrent lookup and crashed the
    // host. The unit tests above call impl_* directly and never see the lock,
    // so this goes through the export table the guest actually calls.
    auto exports = create_d3d11_thunks(d3d11);
    const auto find = [&](const std::string& name) {
        const auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& e) { return e.name == name; });
        EXPECT_NE(it, exports.entries.end()) << name;
        return it;
    };
    for (const std::string name : {d3d11_device_method_export_name(5),
                                   d3d11_context_method_export_name(112)}) {
        const auto entry = find(name);
        if (entry == exports.entries.end()) continue;
        std::atomic<bool> returned{false};
        std::unique_lock held(d3d11.api_mutex());
        std::thread caller([&] {
            cpu::CpuState thread_cpu = cpu;
            thread_cpu.gpr.rcx = 0;  // no such interface: returns at once
            Marshaler thread_marshaler(memory);
            thread_marshaler.attach_cpu(thread_cpu);
            call_export(*entry, thread_marshaler);
            returned = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_FALSE(returned.load())
            << name << " ran while another thread held the device lock";
        held.unlock();
        caller.join();
        EXPECT_TRUE(returned.load()) << name;
    }
}

TEST_F(D3D11ManagerTest, EveryComMethodDirectEntryHoldsTheDeviceLock) {
    // The direct crossing (WP-R1, cli::run_direct_thunk) calls an export's
    // direct entry instead of its handler: the entry must take the same
    // device lock. Each interface table has its own entry function, so one
    // export of every table (and every DXGI one) covers them all.
    const auto exports = create_d3d11_thunks(d3d11);
    const auto dxgi_exports = create_dxgi_thunks(dxgi);
    std::vector<const ExportEntry*> samples;
    usize with_direct = 0;
    for (const DllExports* list : {&exports, &dxgi_exports}) {
        void (*last)(void*, Marshaler&, u32) = nullptr;
        for (const ExportEntry& entry : list->entries) {
            if (entry.direct.fn == nullptr) continue;
            ++with_direct;
            // register_dll derives the handler (ExportEntry::direct).
            EXPECT_FALSE(static_cast<bool>(entry.handler)) << entry.name;
            if (entry.direct.fn != last) {
                samples.push_back(&entry);
                last = entry.direct.fn;
            }
        }
    }
    // Every D3D11 COM method table, the IDXGIOutput table and the DXGI
    // factory/adapter methods.
    EXPECT_GE(with_direct, 650u);
    EXPECT_GE(samples.size(), 34u);
    for (const ExportEntry* entry : samples) {
        std::atomic<bool> returned{false};
        std::unique_lock held(d3d11.api_mutex());
        std::thread caller([&] {
            cpu::CpuState thread_cpu = cpu;
            thread_cpu.gpr.rcx = 0;  // no such interface: returns at once
            Marshaler thread_marshaler(memory);
            thread_marshaler.attach_cpu(thread_cpu);
            entry->direct.fn(entry->direct.context, thread_marshaler,
                             entry->direct.slot);
            returned = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_FALSE(returned.load())
            << entry->name << " ran while another thread held the device lock";
        held.unlock();
        caller.join();
        EXPECT_TRUE(returned.load()) << entry->name;
    }
}

TEST_F(D3D11ManagerTest, DirectTableCarriesEveryExportsStdcallBytes) {
    // The direct crossing pops what the export's stdcall table says (the
    // SDK-signature audit, audit_d3d11_vtables.py): the registry must hand
    // it the same count the full path's finish_thunk_ret reads.
    ThunkRegistry registry(0x70000000);
    ASSERT_TRUE(registry.register_dll("d3d11.dll", create_d3d11_thunks(d3d11))
                    .is_ok());
    ASSERT_TRUE(registry.register_dll("dxgi.dll", create_dxgi_thunks(dxgi))
                    .is_ok());
    usize checked = 0;
    for (const char* dll : {"d3d11.dll", "dxgi.dll"}) {
        const DllExports exports = std::string(dll) == "d3d11.dll"
                                       ? create_d3d11_thunks(d3d11)
                                       : create_dxgi_thunks(dxgi);
        for (const ExportEntry& entry : exports.entries) {
            if (entry.name.empty()) continue;
            const auto thunk = registry.get_thunk_address(dll, entry.name);
            ASSERT_TRUE(thunk.is_ok()) << entry.name;
            const GuestAddr gateway =
                thunk.value() + ThunkRegistry::STUB_SIZE;
            const DirectThunk* direct = registry.direct_thunk(gateway);
            if (entry.direct.fn == nullptr) {
                EXPECT_EQ(direct, nullptr) << entry.name;
                continue;
            }
            ASSERT_NE(direct, nullptr) << entry.name;
            EXPECT_EQ(direct->stdcall_bytes,
                      registry.resolve_stdcall_bytes(gateway))
                << entry.name;
            EXPECT_EQ(direct->stdcall_bytes, entry.stdcall_bytes) << entry.name;
            ++checked;
        }
    }
    EXPECT_GE(checked, 650u);
}

TEST_F(D3D11ManagerTest, ContextLookupMemoFollowsEveryInvalidatingEvent) {
    // impl_ContextMethod memoizes interface -> device, context state and
    // deferred? across calls (WP-R1 C4). Each event that can change one of
    // those answers, or free a record, must move the generation, and a call
    // after it must see the new truth, not the memo.
    com_direct::force_for_test(com_direct::Force::On);
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr immediate = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr CTX_OUT = DATA_BASE + 0x1000;
    constexpr GuestAddr TOPOLOGY_OUT = DATA_BASE + 0x1008;
    constexpr GuestAddr LIST_OUT = DATA_BASE + 0x1010;
    constexpr GuestAddr BUF_DESC = DATA_BASE + 0x1020;
    constexpr GuestAddr BUF_OUT = DATA_BASE + 0x1060;
    const auto set_topology = [&](GuestAddr context, u32 topology) {
        set_arg(0, context);
        set_arg(1, topology);
        d3d11.impl_ContextMethod(marshaler, 24);  // IASetPrimitiveTopology
        return static_cast<u32>(cpu.gpr.rax);
    };
    const auto topology_of = [&](GuestAddr context) {
        EXPECT_TRUE(memory.write_u32(TOPOLOGY_OUT, 0xFFFFu).is_ok());
        set_arg(0, context);
        set_arg(1, TOPOLOGY_OUT);
        d3d11.impl_ContextMethod(marshaler, 83);  // IAGetPrimitiveTopology
        return memory.read_u32(TOPOLOGY_OUT).value();
    };
    const auto moved = [&](u64 before) {
        return d3d11.context_lookup_generation() > before;
    };
    // The memo answers like the lookups it replaces, call after call.
    set_topology(immediate, 4);
    EXPECT_EQ(topology_of(immediate), 4u);
    set_topology(immediate, 5);
    EXPECT_EQ(topology_of(immediate), 5u);

    // (1) An insert into m_deferred_contexts: CreateDeferredContext.
    u64 generation = d3d11.context_lookup_generation();
    set_arg(0, device);
    set_arg(1, 0);
    set_arg(2, CTX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 27);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_TRUE(moved(generation));
    const GuestAddr deferred = memory.read_u64(CTX_OUT).value();
    set_topology(deferred, 1);
    EXPECT_EQ(topology_of(deferred), 1u);
    EXPECT_EQ(topology_of(immediate), 5u) << "two contexts, two records";

    // (2) release_bindings' detached-state erase: ExecuteCommandList with
    // RestoreContextState FALSE.
    set_arg(0, deferred);
    set_arg(1, 0);
    set_arg(2, LIST_OUT);
    d3d11.impl_ContextMethod(marshaler, 114);  // FinishCommandList
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr list = memory.read_u64(LIST_OUT).value();
    generation = d3d11.context_lookup_generation();
    set_arg(0, immediate);
    set_arg(1, list);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 58);  // ExecuteCommandList
    EXPECT_TRUE(moved(generation));
    // Learn (ExecuteCommandList): FALSE leaves the immediate context in the
    // default state afterwards.
    EXPECT_EQ(topology_of(immediate), 0u);

    // (3) Recycling an object address (free_object): a buffer's last Release.
    const std::array<u32, 6> desc{256, 0, 0x4, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        BUF_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, BUF_DESC);
    set_arg(2, 0);
    set_arg(3, BUF_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    generation = d3d11.context_lookup_generation();
    set_arg(0, memory.read_u64(BUF_OUT).value());
    d3d11.impl_BufferMethod(marshaler, 2);
    EXPECT_TRUE(moved(generation));

    // (4) The erases of a deferred context's last Release: the memo must
    // not keep answering for the dead interface.
    set_topology(deferred, 2);  // memoized again
    set_arg(0, list);
    d3d11.impl_CommandListMethod(marshaler, 2);
    generation = d3d11.context_lookup_generation();
    set_arg(0, deferred);
    d3d11.impl_ContextMethod(marshaler, 2);  // Release (last)
    EXPECT_TRUE(moved(generation));
    EXPECT_EQ(set_topology(deferred, 3), d3d11::E_INVALIDARG)
        << "a released context is no context";

    // (5) Device teardown erases the device, its interfaces and the
    // immediate context's state.
    EXPECT_EQ(topology_of(immediate), 0u);  // memoized again
    set_arg(0, immediate);
    d3d11.impl_ContextMethod(marshaler, 2);
    generation = d3d11.context_lookup_generation();
    set_arg(0, device);
    d3d11.impl_DeviceMethod(marshaler, 2);
    EXPECT_TRUE(moved(generation));
    EXPECT_EQ(set_topology(immediate, 4), d3d11::E_INVALIDARG)
        << "the immediate context of a destroyed device";
    com_direct::force_for_test(com_direct::Force::None);
}

TEST_F(D3D11ManagerTest, SelectsFirstSupportedRequestedFeatureLevel) {
    ASSERT_TRUE(memory.write_u32(LEVELS, d3d11::FEATURE_LEVEL_11_1).is_ok());
    ASSERT_TRUE(memory.write_u32(
        LEVELS + 4, d3d11::FEATURE_LEVEL_10_1).is_ok());
    prepare_create();
    set_arg(4, LEVELS);
    set_arg(5, 2);

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(),
              d3d11::FEATURE_LEVEL_10_1);
}

// D3D11_FEATURE_DOUBLES and D3D11_OPTIONS.ExtendedDoublesShaderInstructions
// follow the feature level: a01 "FLb000 feature 1 ... data=1" and "FLa100 /
// FLa000 feature 1 ... data=0", "FLa100 feature 5 ...=1,0,1,1,1,1,1,1,1,1,
// 1,0,0,1" (member 12 FALSE). Doubles are shader model 5 only.
TEST_F(D3D11ManagerTest, DoublesAreReportedFromFeatureLevel11_0) {
    const auto caps_at = [&](u32 level, u32& doubles, u32& extended) {
        ASSERT_TRUE(memory.write_u32(LEVELS, level).is_ok());
        prepare_create();
        set_arg(4, LEVELS);
        set_arg(5, 1);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_D3D11CreateDevice(marshaler);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
        constexpr GuestAddr DATA = DATA_BASE + 0x800;
        ASSERT_TRUE(memory.write_u32(DATA, 0xCDCDCDCDu).is_ok());
        set_arg(0, device);
        set_arg(1, 1);  // D3D11_FEATURE_DOUBLES
        set_arg(2, DATA);
        set_arg(3, 4);
        d3d11.impl_DeviceMethod(marshaler, 33);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        doubles = memory.read_u32(DATA).value();
        set_arg(0, device);
        set_arg(1, 5);  // D3D11_FEATURE_D3D11_OPTIONS
        set_arg(2, DATA);
        set_arg(3, 56);
        d3d11.impl_DeviceMethod(marshaler, 33);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        extended = memory.read_u32(DATA + 12 * 4).value();
    };
    u32 doubles = 0, extended = 0;
    caps_at(d3d11::FEATURE_LEVEL_11_0, doubles, extended);
    EXPECT_EQ(doubles, 1u);
    EXPECT_EQ(extended, 1u);
    caps_at(d3d11::FEATURE_LEVEL_10_1, doubles, extended);
    EXPECT_EQ(doubles, 0u);
    EXPECT_EQ(extended, 0u);
    caps_at(d3d11::FEATURE_LEVEL_10_0, doubles, extended);
    EXPECT_EQ(doubles, 0u);
    EXPECT_EQ(extended, 0u);
}

// Chromium's GPU process asks ANGLE for every level from 12_1 down. Only a
// pre-Windows 10 runtime rejects an array containing 12_x outright; the
// Direct3D 11.3 runtime skips the levels the device cannot reach.
TEST_F(D3D11ManagerTest, AcceptsFeatureLevelsAboveTheSupportedContract) {
    const u32 requested[] = {
        d3d11::FEATURE_LEVEL_12_1, d3d11::FEATURE_LEVEL_12_0,
        d3d11::FEATURE_LEVEL_11_1, d3d11::FEATURE_LEVEL_11_0,
        d3d11::FEATURE_LEVEL_10_1, d3d11::FEATURE_LEVEL_10_0,
        d3d11::FEATURE_LEVEL_9_3,  d3d11::FEATURE_LEVEL_9_2,
        d3d11::FEATURE_LEVEL_9_1,
    };
    for (u32 index = 0; index < std::size(requested); ++index) {
        ASSERT_TRUE(memory.write_u32(
            LEVELS + index * 4, requested[index]).is_ok());
    }
    prepare_create();
    set_arg(4, LEVELS);
    set_arg(5, static_cast<u64>(std::size(requested)));

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(),
              d3d11::FEATURE_LEVEL_11_0);
    EXPECT_NE(memory.read_u64(DEVICE_OUT).value(), 0u);
}

TEST_F(D3D11ManagerTest, RejectsUnknownFeatureLevelValues) {
    ASSERT_TRUE(memory.write_u32(LEVELS, 0x1234u).is_ok());
    prepare_create();
    set_arg(4, LEVELS);
    set_arg(5, 1);

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
}

TEST_F(D3D11ManagerTest, RejectsHardwareDriverTypeWithExplicitAdapter) {
    const GuestAddr adapter = dxgi.create_default_adapter();
    ASSERT_NE(adapter, 0u);
    prepare_create(adapter, d3d11::DRIVER_TYPE_HARDWARE);

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(DEVICE_OUT).value(), 0u);
    dxgi.release_adapter(adapter);
}

TEST_F(D3D11ManagerTest, ExposesDxgiDeviceAndReturnsTheBackingAdapter) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_IDXGIDEVICE),
        sizeof(IID_IDXGIDEVICE)).is_ok());

    cpu.gpr.rcx = device;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = INTERFACE_OUT;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr dxgi_device = memory.read_u64(INTERFACE_OUT).value();
    ASSERT_NE(dxgi_device, 0u);

    cpu.gpr.rcx = dxgi_device;
    cpu.gpr.rdx = ADAPTER_OUT;
    d3d11.impl_DxgiDeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr adapter = memory.read_u64(ADAPTER_OUT).value();
    EXPECT_TRUE(dxgi.is_adapter_object(adapter));
    dxgi.release_adapter(adapter);
}

// Corrected by Windows (d3d11_a01_device_caps_probe "out fl" / "out none"):
// with ppDevice and ppImmediateContext both NULL the call validates only and
// returns S_FALSE, not S_OK, after writing *pFeatureLevel.
TEST_F(D3D11ManagerTest, FeatureProbeDoesNotCreateGuestComObjects) {
    prepare_create();
    set_arg(7, 0);
    set_arg(9, 0);

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);  // S_FALSE
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(),
              d3d11::FEATURE_LEVEL_11_0);
}

// Driver types, Software-module rules, creation flags and feature-level
// arrays. Every expected HRESULT is a d3d11_a01_device_caps_probe64.expected
// line (Windows); WARP and REFERENCE are the documented refusals of a
// system with neither (no software rasterizer, no Graphics Tools).
TEST_F(D3D11ManagerTest, CreateDeviceParameterRulesMatchWindows) {
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto create = [&](u32 driver, GuestAddr software, u32 flags,
                            GuestAddr levels, u32 count) -> u32 {
        prepare_create(0, driver);
        set_arg(2, software);
        set_arg(3, flags);
        set_arg(4, levels);
        set_arg(5, count);
        d3d11.impl_D3D11CreateDevice(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    // "drv NULL(3)": a device without rendering guarantees; this one renders.
    EXPECT_EQ(create(d3d11::DRIVER_TYPE_NULL, 0, 0, 0, 0), d3d11::S_OK);
    EXPECT_NE(memory.read_u64(DEVICE_OUT).value(), 0u);
    // "drv SOFTWARE(4) sw=NULL" and "drv HARDWARE sw=exe", "drv WARP sw=exe".
    EXPECT_EQ(create(d3d11::DRIVER_TYPE_SOFTWARE, 0, 0, 0, 0),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(create(d3d11::DRIVER_TYPE_HARDWARE, 0x400000, 0, 0, 0),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(create(d3d11::DRIVER_TYPE_WARP, 0x400000, 0, 0, 0),
              d3d11::E_INVALIDARG);
    // "drv SOFTWARE(4) sw=exe": a module with no OpenAdapter10 export. The
    // test memory has no PE image at all, which reads as the same thing.
    EXPECT_EQ(create(d3d11::DRIVER_TYPE_SOFTWARE, 0x400000, 0, 0, 0),
              d3d11::HRESULT_PROC_NOT_FOUND);
    EXPECT_EQ(create(6, 0, 0, 0, 0), d3d11::E_INVALIDARG);

    // "flags 0x4/0x10/0x40/0x200/0x2000..": SDK layers missing.
    for (u32 bit : {0x2u, 0x4u, 0x10u, 0x40u, 0x200u, 0x400u, 0x2000u,
                    0x4000000u, 0x10000000u, 0x20000000u}) {
        EXPECT_EQ(create(d3d11::DRIVER_TYPE_HARDWARE, 0, bit, 0, 0),
                  d3d11::DXGI_ERROR_SDK_COMPONENT_MISSING)
            << std::hex << bit;
    }
    // "flags 0x1000/0x8000000/0x40000000/0x80000000": accepted, dropped.
    for (u32 bit : {0x1000u, 0x8000000u, 0x40000000u, 0x80000000u}) {
        ASSERT_EQ(create(d3d11::DRIVER_TYPE_HARDWARE, 0, bit | 0x821u, 0, 0),
                  d3d11::S_OK) << std::hex << bit;
        cpu.gpr.rcx = memory.read_u64(DEVICE_OUT).value();
        d3d11.impl_DeviceMethod(marshaler, 38);  // GetCreationFlags
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x821u);
    }

    // Feature-level arrays.
    const auto write_levels = [&](std::initializer_list<u32> values) {
        u32 index = 0;
        for (u32 value : values) {
            ASSERT_TRUE(memory.write_u32(LEVELS + index++ * 4, value).is_ok());
        }
    };
    write_levels({d3d11::FEATURE_LEVEL_11_0, 0xffffu});
    EXPECT_EQ(create(d3d11::DRIVER_TYPE_HARDWARE, 0, 0, LEVELS, 2),
              d3d11::E_INVALIDARG);  // "levels {11_0,0xffff}"
    write_levels({d3d11::FEATURE_LEVEL_11_0, 0xc300u});
    EXPECT_EQ(create(d3d11::DRIVER_TYPE_HARDWARE, 0, 0, LEVELS, 2),
              d3d11::E_INVALIDARG);  // "levels {11_0,0xc300}"
    EXPECT_EQ(create(d3d11::DRIVER_TYPE_HARDWARE, 0, 0, LEVELS, 0),
              d3d11::DXGI_ERROR_UNSUPPORTED);  // "levels {11_0} count 0"
    for (u32 index = 0; index < 40; ++index) {
        ASSERT_TRUE(memory.write_u32(
            LEVELS + index * 4, d3d11::FEATURE_LEVEL_11_0).is_ok());
    }
    EXPECT_EQ(create(d3d11::DRIVER_TYPE_HARDWARE, 0, 0, LEVELS, 40),
              d3d11::S_OK);  // "levels 40 x 11_0"
    write_levels({d3d11::FEATURE_LEVEL_9_1, d3d11::FEATURE_LEVEL_12_2});
    ASSERT_EQ(create(d3d11::DRIVER_TYPE_HARDWARE, 0, 0, LEVELS, 2),
              d3d11::S_OK);  // "levels {9_1,12_2}"
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(), d3d11::FEATURE_LEVEL_9_1);
}

// d3d11_a01_device_caps_probe_swap.expected: a swap chain is made only when
// both pSwapChainDesc and ppSwapChain are given; otherwise the device alone.
TEST_F(D3D11ManagerTest, CreateDeviceAndSwapChainWithoutSwapChainArguments) {
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto call = [&](GuestAddr desc, GuestAddr swap_out,
                          GuestAddr device_out, GuestAddr context_out) {
        for (usize index = 0; index < 12; ++index) set_arg(index, 0);
        set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
        set_arg(6, d3d11::SDK_VERSION);
        set_arg(7, desc);
        set_arg(8, swap_out);
        set_arg(9, device_out);
        set_arg(10, LEVEL_OUT);
        set_arg(11, context_out);
        ASSERT_TRUE(memory.write_u64(SWAP_OUT, 0x5a5a5a5a).is_ok());
        d3d11.impl_D3D11CreateDeviceAndSwapChain(marshaler);
    };
    call(0, 0, DEVICE_OUT, CONTEXT_OUT);  // "sc desc NULL ppSwap NULL"
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(DEVICE_OUT).value(), 0u);
    EXPECT_NE(memory.read_u64(CONTEXT_OUT).value(), 0u);
    call(0, SWAP_OUT, DEVICE_OUT, CONTEXT_OUT);  // "sc desc NULL ppSwap set"
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(SWAP_OUT).value(), 0u);
    EXPECT_NE(memory.read_u64(DEVICE_OUT).value(), 0u);
    call(0, 0, 0, 0);  // "sc desc NULL, only fl"
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(), d3d11::FEATURE_LEVEL_11_0);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
