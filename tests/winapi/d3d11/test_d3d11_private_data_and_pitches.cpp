// SPDX-License-Identifier: LGPL-2.1-only
//
// Private data, unserved shader stages, staging pitches, Texture1D and the event/timestamp queries.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// An APPEND or COUNTER buffer UAV carries a hidden counter, and
// CopyStructureCount copies it -- four bytes, on the GPU timeline, at the
// destination offset the guest named.
//
// The counter has to be real GPU storage rather than a number in the view
// record: a shader moves it, and a copy that read a host-side integer would
// report whatever the CPU last knew rather than what the GPU produced. The
// backend records the source buffer's size, which is how this tells the
// four-byte counter from the 32-byte resource it counts.
// Predication: CreatePredicate, SetPredication/GetPredication round trip, and
// the rules Learn attaches to them. What predication suppresses is
// PredicationSkipsTheListedOperationsWhenDataEqualsValue.
// Texture1D: created, described, viewed and mapped.
//
// It is backed by a height-1 2D texture rather than a Metal 1D texture, because
// MTLTextureType1D supports neither mipmaps nor use as a render target while a
// D3D11 Texture1D supports both. That is a host detail the guest must never see:
// GetType says TEXTURE1D, GetDesc writes the 1D layout, and only a 1D view is
// accepted.
// Staging texture Map layouts: compressed (block rows) and depth.
//
// The row pitch is the whole contract here. A guest walks the mapped bytes with
// it, so a pitch that is off by a factor of four -- which is what a texel-row
// pitch on a BC format is -- reads as corrupted texture data with nothing
// pointing at the pitch.
// SetPrivateDataInterface on the device and on a device child.
//
// The point is the reference, and the hardest part is the teardown: the
// interfaces are kept in one manager-level map so that a single free_object hook
// covers all eighteen release_* paths. One that forgot would leak a guest
// reference silently -- no crash, no failing test, just an object the guest can
// never free -- so this test destroys a child and asserts the Release arrives.
TEST_F(D3D11ManagerTest, PrivateDataInterfaceIsRefCountedThroughTheGuestVtable) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr SCRATCH = GPU_BUFFER_DESC + 0x800;
    constexpr GuestAddr GUID_A = SCRATCH + 0x00;
    constexpr GuestAddr GUEST_VTABLE = SCRATCH + 0x40;
    constexpr GuestAddr GUEST_IFACE = SCRATCH + 0xC0;
    constexpr GuestAddr GUEST_IFACE2 = SCRATCH + 0xD0;
    constexpr GuestAddr SIZE_OUT = SCRATCH + 0x100;
    constexpr GuestAddr DATA_OUT = SCRATCH + 0x110;
    constexpr GuestAddr BUF_DESC = SCRATCH + 0x140;
    constexpr GuestAddr BUF_OUT = SCRATCH + 0x180;
    constexpr GuestAddr GUEST_ADDREF = 0x72000010;
    constexpr GuestAddr GUEST_RELEASE = 0x72000020;

    ASSERT_TRUE(memory.write_u64(GUEST_VTABLE + 1 * 8, GUEST_ADDREF).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_VTABLE + 2 * 8, GUEST_RELEASE).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_IFACE, GUEST_VTABLE).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_IFACE2, GUEST_VTABLE).is_ok());
    const std::array<u8, 16> guid{
        0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
        0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90};
    ASSERT_TRUE(memory.write_bytes(
        GUID_A, reinterpret_cast<const Byte*>(guid.data()),
        guid.size()).is_ok());

    const auto exports = create_d3d11_thunks(d3d11);
    const auto invoke = [&](const std::string& name, Marshaler& m) {
        for (const auto& entry : exports.entries) {
            if (entry.name == name) { call_export(entry, m); return; }
        }
        ADD_FAILURE() << name;
    };
    std::vector<std::pair<GuestAddr, GuestAddr>> calls;
    d3d11.set_guest_function_caller(
        [this, &calls](GuestAddr function, const u64* args, usize arg_count) -> u64 {
            EXPECT_EQ(arg_count, 1u);
            std::thread probe([&] {
                const bool released = dxgi.api_mutex().try_lock();
                EXPECT_TRUE(released) << "shared graphics lock held across guest COM callback";
                if (released) dxgi.api_mutex().unlock();
            });
            probe.join();
            calls.emplace_back(function,
                               static_cast<GuestAddr>(arg_count ? args[0] : 0));
            return 1;
        });
    const auto count_of = [this, &calls](GuestAddr function, GuestAddr self) {
        usize total = 0;
        for (const auto& [fn, arg] : calls) {
            if (fn == function && arg == self) ++total;
        }
        return total;
    };

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto set_interface = [&](usize dispatcher_is_device, GuestAddr object,
                                   GuestAddr interface_ptr) {
        set_arg(0, object);
        set_arg(1, GUID_A);
        set_arg(2, interface_ptr);
        if (dispatcher_is_device != 0) {
            invoke(d3d11_device_method_export_name(36), marshaler);
        } else {
            invoke(d3d11_buffer_method_export_name(6), marshaler);
        }
        return static_cast<u32>(cpu.gpr.rax);
    };

    // --- the device -----------------------------------------------------
    calls.clear();
    EXPECT_EQ(set_interface(1, device, GUEST_IFACE), d3d11::S_OK);
    EXPECT_EQ(count_of(GUEST_ADDREF, GUEST_IFACE), 1u);

    // GetPrivateData returns the interface with its own reference, and reports a
    // required size of one pointer rather than a byte count.
    ASSERT_TRUE(memory.write_u32(SIZE_OUT, 8).is_ok());
    ASSERT_TRUE(memory.write_u64(DATA_OUT, 0).is_ok());
    calls.clear();
    set_arg(0, device);
    set_arg(1, GUID_A);
    set_arg(2, SIZE_OUT);
    set_arg(3, DATA_OUT);
    invoke(d3d11_device_method_export_name(34), marshaler);  // GetPrivateData
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(DATA_OUT).value(), GUEST_IFACE);
    EXPECT_EQ(memory.read_u32(SIZE_OUT).value(), 8u);
    EXPECT_EQ(count_of(GUEST_ADDREF, GUEST_IFACE), 1u)
        << "GetPrivateData gives the caller its own reference";

    // Replacing releases the old one exactly once.
    calls.clear();
    EXPECT_EQ(set_interface(1, device, GUEST_IFACE2), d3d11::S_OK);
    EXPECT_EQ(count_of(GUEST_ADDREF, GUEST_IFACE2), 1u);
    EXPECT_EQ(count_of(GUEST_RELEASE, GUEST_IFACE), 1u);

    // Learn: private data and private interfaces share one GUID namespace, so
    // SetPrivateData over the same GUID releases the interface.
    calls.clear();
    ASSERT_TRUE(memory.write_u32(DATA_OUT, 0x1234u).is_ok());
    set_arg(0, device);
    set_arg(1, GUID_A);
    set_arg(2, 4);
    set_arg(3, DATA_OUT);
    invoke(d3d11_device_method_export_name(35), marshaler);  // SetPrivateData
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(count_of(GUEST_RELEASE, GUEST_IFACE2), 1u)
        << "bytes under a GUID that held an interface must release it";
    // ...and the GUID now reads back as bytes, not as an interface.
    ASSERT_TRUE(memory.write_u32(SIZE_OUT, 4).is_ok());
    set_arg(0, device);
    set_arg(1, GUID_A);
    set_arg(2, SIZE_OUT);
    set_arg(3, DATA_OUT);
    invoke(d3d11_device_method_export_name(34), marshaler);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SIZE_OUT).value(), 4u);

    // --- a device child, and its destruction -----------------------------
    const std::array<u32, 6> desc{64, 0, 0x8, 0, 0, 0};  // DEFAULT, bind SRV
    ASSERT_TRUE(memory.write_bytes(
        BUF_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, BUF_DESC);
    set_arg(2, 0);
    set_arg(3, BUF_OUT);
    invoke(d3d11_device_method_export_name(3), marshaler);  // CreateBuffer
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(BUF_OUT).value();
    ASSERT_NE(buffer, 0u);

    calls.clear();
    EXPECT_EQ(set_interface(0, buffer, GUEST_IFACE), d3d11::S_OK)
        << "every ID3D11DeviceChild shares the helper";
    EXPECT_EQ(count_of(GUEST_ADDREF, GUEST_IFACE), 1u);

    // The release path has to drop it. This is the assertion the single
    // free_object hook exists for: eighteen release_* paths, and a leak here
    // would be invisible.
    calls.clear();
    set_arg(0, buffer);
    invoke(d3d11_buffer_method_export_name(2), marshaler);  // Release
    EXPECT_EQ(cpu.gpr.rax, 0u) << "last reference";
    EXPECT_EQ(count_of(GUEST_RELEASE, GUEST_IFACE), 1u)
        << "an interface still held when the object dies must be released, or "
           "the guest can never free it";
}

// Without the host->guest call wired, the D3D11 side refuses too.
TEST_F(D3D11ManagerTest, PrivateDataInterfaceRefusesWithoutAGuestCaller) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr GUID_B = GPU_BUFFER_DESC + 0x900;
    const std::array<u8, 16> guid{
        0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
        0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00};
    ASSERT_TRUE(memory.write_bytes(
        GUID_B, reinterpret_cast<const Byte*>(guid.data()),
        guid.size()).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, device);
    set_arg(1, GUID_B);
    set_arg(2, GPU_BUFFER_DESC + 0x9C0);
    d3d11.impl_DeviceMethod(marshaler, 36);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_NOTIMPL)
        << "holding a reference it never took is worse than refusing";
}

// The stages Metal does not have must refuse without leaving a pointer behind.
//
// Geometry, hull and domain shaders are emulated now; what remains is the
// out-parameter rule. A COM out-parameter has to be written even on failure: a guest that
// poisons ppGeometryShader and checks the pointer rather than the HRESULT would
// otherwise Release stack garbage. Same defect class the IDXGIFactory2 stubs had.
TEST_F(D3D11ManagerTest, UnservedShaderStagesRefuseAndNullTheirOutParameters) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    ASSERT_NE(context, 0u);
    constexpr GuestAddr OUT = GPU_BUFFER_DESC + 0xA00;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    struct Case { usize slot; usize out_arg; const char* name; };
    const Case cases[] = {
        // CreateGeometryShader (13) is served since 11.16 -- with a NULL
        // bytecode it is E_INVALIDARG now, and D3D11GeometryTest covers it.
        // CreateGeometryShaderWithStreamOutput (14) is served now, and its
        // out-parameter is arg(9) -- this row used to say 8, the same wrong
        // index the implementation used, so both agreed and neither was right.
        {16, 4, "CreateHullShader"},
        {17, 4, "CreateDomainShader"},
    };
    for (const Case& test_case : cases) {
        ASSERT_TRUE(memory.write_u64(OUT, 0xA5A5A5A5ull).is_ok());
        cpu.gpr.rsp = STACK_BASE + 0x800;
        for (usize index = 0; index <= test_case.out_arg; ++index) {
            set_arg(index, index == test_case.out_arg ? OUT : 0);
        }
        set_arg(0, device);
        d3d11.impl_DeviceMethod(marshaler, test_case.slot);
        // Hull and domain shaders are served now (11.17, the software
        // tessellator included), so NULL bytecode is the caller's error:
        // Windows returns E_INVALIDARG and NULLs the out-parameter
        // (measured on Windows: CreateHullShader / CreateDomainShader
        // (NULL, 0) hr=0x80070057 out=NULL). This expected E_NOTIMPL while
        // the stages were refused.
        EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG)
            << test_case.name << " with NULL bytecode";
        EXPECT_EQ(memory.read_u64(OUT).value(), 0u)
            << test_case.name << " left its out-parameter unwritten; a guest "
            << "checking the pointer would Release stack garbage";
    }

    // SOSetTargets and DrawAuto are void, so they cannot refuse at all. What
    // they must not do is look like they worked: binding stream-out targets that
    // capture nothing, then reading the buffer back, is a silent wrong result.
    // Both are reachable and both return without touching anything.
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, OUT);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 37);  // SOSetTargets
    EXPECT_EQ(cpu.gpr.rax, 0u) << "void";
    set_arg(0, context);
    d3d11.impl_ContextMethod(marshaler, 38);  // DrawAuto
    EXPECT_EQ(cpu.gpr.rax, 0u) << "void";

    // GSSetShader(NULL) is the documented way to disable the stage and must be
    // accepted cleanly, since that is what a guest does between passes.
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 23);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(D3D11ManagerTest, StagingMapReportsBlockAndDepthRowPitches) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    ASSERT_NE(context, 0u);
    constexpr GuestAddr SCRATCH = GPU_BUFFER_DESC + 0x700;
    constexpr GuestAddr DESC = SCRATCH + 0x00;
    constexpr GuestAddr OUT = SCRATCH + 0x40;
    constexpr GuestAddr MAPPED = SCRATCH + 0x60;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // D3D11_TEXTURE2D_DESC { Width, Height, MipLevels, ArraySize, Format,
    // SampleCount, SampleQuality, Usage, BindFlags, CPUAccessFlags, MiscFlags }.
    const auto create = [&](u32 width, u32 height, u32 fmt, u32 usage,
                            u32 bind, u32 cpu_access) {
        const std::array<u32, 11> d{
            width, height, 1, 1, fmt, 1, 0, usage, bind, cpu_access, 0};
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(d.data()),
            sizeof(d)).is_ok());
        set_arg(0, device);
        set_arg(1, DESC);
        set_arg(2, 0);
        set_arg(3, OUT);
        d3d11.impl_DeviceMethod(marshaler, 5);
        return cpu.gpr.rax == d3d11::S_OK ? memory.read_u64(OUT).value()
                                          : GuestAddr{0};
    };
    // D3D11_USAGE_STAGING=3, CPU_ACCESS_READ|WRITE=0x30000.
    const auto create_staging = [&](u32 width, u32 height, u32 fmt) {
        return create(width, height, fmt, 3, 0, 0x30000);
    };
    // D3D11_MAP_READ=1.
    const auto map_row_pitch = [&](GuestAddr texture) -> u32 {
        EXPECT_TRUE(memory.write_u32(MAPPED + 8, 0xEEEEEEEEu).is_ok());
        set_arg(0, context);
        set_arg(1, texture);
        set_arg(2, 0);
        set_arg(3, 1);
        set_arg(4, 0);
        set_arg(5, MAPPED);
        d3d11.impl_ContextMethod(marshaler, 14);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u32(MAPPED + 8).value();
    };

    // A block-compressed staging texture's pitch is one BLOCK row: for a 16-wide
    // BC1 surface that is ceil(16/4) * 8 = 32 bytes, not 16 * 4.
    const GuestAddr bc1 = create_staging(16, 16, 71);  // BC1_UNORM
    ASSERT_NE(bc1, 0u);
    EXPECT_EQ(map_row_pitch(bc1), 32u) << "BC1: four blocks of eight bytes";
    const GuestAddr bc3 = create_staging(16, 16, 77);  // BC3_UNORM
    ASSERT_NE(bc3, 0u);
    EXPECT_EQ(map_row_pitch(bc3), 64u) << "BC3: four blocks of sixteen bytes";

    // Depth. Learn: a staging texture may carry a depth format -- that is how a
    // guest reads a depth buffer back.
    const GuestAddr d32 = create_staging(16, 16, 40);  // D32_FLOAT
    ASSERT_NE(d32, 0u);
    EXPECT_EQ(map_row_pitch(d32), 64u);
    const GuestAddr d16 = create_staging(16, 16, 55);  // D16_UNORM
    ASSERT_NE(d16, 0u);
    EXPECT_EQ(map_row_pitch(d16), 32u);

    // Packed depth+stencil, with the pitches measured on Windows: a 16-wide
    // D24_UNORM_S8_UINT staging surface reports 64 bytes (four per pixel, both
    // planes packed) and D32_FLOAT_S8X24_UINT reports 128 (eight). The host maps
    // both onto Metal's Depth32Float_Stencil8, whose separate planes
    // format_table reports as five bytes, so using the host number here would
    // hand back a stride Windows never reports.
    const GuestAddr d24s8 = create_staging(16, 16, 45);
    ASSERT_NE(d24s8, 0u) << "Windows accepts this; an earlier refusal here was "
                            "under-permissive";
    EXPECT_EQ(map_row_pitch(d24s8), 64u);
    const GuestAddr d32s8 = create_staging(16, 16, 20);
    ASSERT_NE(d32s8, 0u);
    EXPECT_EQ(map_row_pitch(d32s8), 128u);

    // End to end: CopyResource from a depth resource into the staging one, then
    // Map, must hand back the depth bytes. A create-and-Map that reported the
    // right pitch over a buffer nothing filled would pass every assertion above.
    const GuestAddr depth =
        create(4, 4, 40, 0, D3D11_BIND_DEPTH_STENCIL, 0);  // DEFAULT + DSV
    ASSERT_NE(depth, 0u);
    const GuestAddr depth_staging = create_staging(4, 4, 40);
    ASSERT_NE(depth_staging, 0u);
    backend.state.native_texture_readback.resize(4 * 4 * sizeof(float));
    for (u32 index = 0; index < 16; ++index) {
        const float value = static_cast<float>(index) / 16.0f;
        std::memcpy(backend.state.native_texture_readback.data() +
                        index * sizeof(float), &value, sizeof(float));
    }
    backend.state.texture_native_readbacks = 0;
    set_arg(0, context);
    set_arg(1, depth_staging);
    set_arg(2, depth);
    d3d11.impl_ContextMethod(marshaler, 47);  // CopyResource
    EXPECT_GT(backend.state.texture_native_readbacks, 0u)
        << "the copy must read the depth resource back, not leave the staging "
           "shadow as it was";

    ASSERT_TRUE(memory.write_u64(MAPPED, 0).is_ok());
    set_arg(0, context);
    set_arg(1, depth_staging);
    set_arg(2, 0);
    set_arg(3, 1);
    set_arg(4, 0);
    set_arg(5, MAPPED);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr data = memory.read_u64(MAPPED).value();
    ASSERT_NE(data, 0u);
    const u32 pitch = memory.read_u32(MAPPED + 8).value();
    EXPECT_EQ(pitch, 16u) << "four D32_FLOAT texels";
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            const auto bits =
                memory.read_u32(data + row * pitch + column * 4);
            ASSERT_TRUE(bits.is_ok());
            float value = 0.0f;
            const u32 raw = bits.value();
            std::memcpy(&value, &raw, sizeof(value));
            EXPECT_FLOAT_EQ(value, static_cast<float>(row * 4 + column) / 16.0f)
                << "depth texel (" << column << ", " << row << ")";
        }
    }
}

TEST_F(D3D11ManagerTest, Texture1DIsCreatedDescribedAndViewedAsOneDimensional) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr SCRATCH = GPU_BUFFER_DESC + 0x600;
    constexpr GuestAddr DESC = SCRATCH + 0x00;
    constexpr GuestAddr OUT = SCRATCH + 0x40;
    constexpr GuestAddr READBACK = SCRATCH + 0x60;
    constexpr GuestAddr SRV_D = SCRATCH + 0xA0;
    constexpr GuestAddr SRV_O = SCRATCH + 0xE0;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // D3D11_TEXTURE1D_DESC { Width, MipLevels, ArraySize, Format, Usage,
    // BindFlags, CPUAccessFlags, MiscFlags }.
    const auto create_1d = [&](const std::array<u32, 8>& desc, GuestAddr out) {
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, DESC);
        set_arg(2, 0);
        set_arg(3, out);
        d3d11.impl_DeviceMethod(marshaler, 4);
        return cpu.gpr.rax == d3d11::S_OK && out != 0
                   ? memory.read_u64(out).value() : GuestAddr{0};
    };

    // 256 texels, 1 mip, 1 slice, R8G8B8A8_UNORM(28), DEFAULT, bind SRV(0x8).
    const std::array<u32, 8> base{256, 1, 1, 28, 0, 0x8, 0, 0};
    const GuestAddr texture = create_1d(base, OUT);
    ASSERT_NE(texture, 0u);

    // A zero width has no texels to address.
    std::array<u32, 8> bad = base;
    bad[0] = 0;
    EXPECT_EQ(create_1d(bad, OUT), 0u);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    // D3D11_RESOURCE_MISC_TEXTURECUBE names six 2D faces; a 1D texture has none.
    bad = base;
    bad[7] = 0x4;
    EXPECT_EQ(create_1d(bad, OUT), 0u);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    // A null out-parameter validates the description without creating anything.
    (void)create_1d(base, 0);
    EXPECT_EQ(cpu.gpr.rax, 1u) << "S_FALSE for the validation-only form";

    // GetType must say TEXTURE1D. Reporting TEXTURE2D would send a guest that
    // switches on the dimension down the wrong cast.
    set_arg(0, texture);
    set_arg(1, READBACK);
    d3d11.impl_Texture2DMethod(marshaler, 7);
    EXPECT_EQ(memory.read_u32(READBACK).value(), 2u)
        << "D3D11_RESOURCE_DIMENSION_TEXTURE1D";

    // GetDesc writes the 1D layout: no Height, no SampleDesc. A guest reading
    // the 2D layout back would take MipLevels out of the Height field.
    for (u32 word = 0; word < 8; ++word) {
        ASSERT_TRUE(memory.write_u32(READBACK + word * 4, 0xEEEEEEEEu).is_ok());
    }
    set_arg(0, texture);
    set_arg(1, READBACK);
    d3d11.impl_Texture2DMethod(marshaler, 10);
    EXPECT_EQ(memory.read_u32(READBACK + 0).value(), 256u) << "Width";
    EXPECT_EQ(memory.read_u32(READBACK + 4).value(), 1u) << "MipLevels";
    EXPECT_EQ(memory.read_u32(READBACK + 8).value(), 1u) << "ArraySize";
    EXPECT_EQ(memory.read_u32(READBACK + 12).value(), 28u) << "Format";
    EXPECT_EQ(memory.read_u32(READBACK + 20).value(), 0x8u) << "BindFlags";

    // Views. D3D11_SHADER_RESOURCE_VIEW_DESC { Format, ViewDimension, union }.
    const auto create_srv = [&](u32 dimension) {
        ASSERT_TRUE(memory.write_u32(SRV_D, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(SRV_D + 4, dimension).is_ok());
        ASSERT_TRUE(memory.write_u32(SRV_D + 8, 0).is_ok());   // MostDetailedMip
        ASSERT_TRUE(memory.write_u32(SRV_D + 12, 1).is_ok());  // MipLevels
        set_arg(0, device);
        set_arg(1, texture);
        set_arg(2, SRV_D);
        set_arg(3, SRV_O);
        d3d11.impl_DeviceMethod(marshaler, 7);
    };
    create_srv(2);  // D3D11_SRV_DIMENSION_TEXTURE1D
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(SRV_O).value(), 0u);
    // A 2D view of a 1D resource must be refused. The backing texture really is
    // 2D, so accepting it would "work" here and bind to a Texture2D register
    // that cannot address the resource the guest asked for.
    create_srv(4);  // D3D11_SRV_DIMENSION_TEXTURE2D
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    create_srv(8);  // TEXTURE3D
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    // ...and symmetrically, a 1D view of a 2D resource.
    const std::array<u32, 11> desc_2d{
        16, 16, 1, 1, 28, 1, 0, 0, 0x8, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        DESC, reinterpret_cast<const Byte*>(desc_2d.data()),
        sizeof(desc_2d)).is_ok());
    set_arg(0, device);
    set_arg(1, DESC);
    set_arg(2, 0);
    set_arg(3, OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture_2d = memory.read_u64(OUT).value();
    ASSERT_NE(texture_2d, 0u);
    ASSERT_TRUE(memory.write_u32(SRV_D, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SRV_D + 4, 2).is_ok());  // TEXTURE1D
    ASSERT_TRUE(memory.write_u32(SRV_D + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SRV_D + 12, 1).is_ok());
    set_arg(0, device);
    set_arg(1, texture_2d);
    set_arg(2, SRV_D);
    set_arg(3, SRV_O);
    d3d11.impl_DeviceMethod(marshaler, 7);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG)
        << "a 1D view names a 1D resource";

    // RTV and UAV take the same dimension rule as the SRV, in both directions.
    // A 2D view of a 1D resource must fail -- and the 2D case must keep working,
    // which is the assertion that catches a switch-grouping mistake: inserting
    // the 1D cases between TEXTURE2D and TEXTURE2DARRAY made a 2D view require a
    // 1D resource, exactly backwards, and nothing else here noticed.
    constexpr GuestAddr RT_DESC = SRV_D + 0x40;
    constexpr GuestAddr RT_OUT = SRV_O + 0x20;
    const std::array<u32, 8> rt_desc_1d{28, 30, 1, 1, 28, 0, 0x20, 0};
    const auto make_rt_1d = [&](u32 fmt, u32 bind) {
        const std::array<u32, 8> d{256, 1, 1, fmt, 0, bind, 0, 0};
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(d.data()), sizeof(d)).is_ok());
        set_arg(0, device);
        set_arg(1, DESC);
        set_arg(2, 0);
        set_arg(3, OUT);
        d3d11.impl_DeviceMethod(marshaler, 4);
        return cpu.gpr.rax == d3d11::S_OK ? memory.read_u64(OUT).value()
                                          : GuestAddr{0};
    };
    (void)rt_desc_1d;
    // D3D11_BIND_RENDER_TARGET = 0x20.
    const GuestAddr rt_1d = make_rt_1d(28, 0x20);
    ASSERT_NE(rt_1d, 0u);
    const auto create_rtv = [&](GuestAddr resource, u32 dimension) {
        ASSERT_TRUE(memory.write_u32(RT_DESC, 0).is_ok());        // Format
        ASSERT_TRUE(memory.write_u32(RT_DESC + 4, dimension).is_ok());
        ASSERT_TRUE(memory.write_u32(RT_DESC + 8, 0).is_ok());    // MipSlice
        ASSERT_TRUE(memory.write_u32(RT_DESC + 12, 0).is_ok());
        set_arg(0, device);
        set_arg(1, resource);
        set_arg(2, RT_DESC);
        set_arg(3, RT_OUT);
        d3d11.impl_DeviceMethod(marshaler, 9);  // CreateRenderTargetView
    };
    create_rtv(rt_1d, 2);  // D3D11_RTV_DIMENSION_TEXTURE1D
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "a 1D RTV of a 1D resource";
    create_rtv(rt_1d, 4);  // TEXTURE2D
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "2D RTV of a 1D resource";
    // The same 2D dimension must still work on a 2D resource.
    const std::array<u32, 11> rt2d{
        16, 16, 1, 1, 28, 1, 0, 0, 0x20, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        DESC, reinterpret_cast<const Byte*>(rt2d.data()),
        sizeof(rt2d)).is_ok());
    set_arg(0, device);
    set_arg(1, DESC);
    set_arg(2, 0);
    set_arg(3, OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr rt_2d = memory.read_u64(OUT).value();
    ASSERT_NE(rt_2d, 0u);
    create_rtv(rt_2d, 4);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK)
        << "a 2D RTV of a 2D resource must keep working";
    create_rtv(rt_2d, 2);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "1D RTV of a 2D resource";

    // UAV, same rule. D3D11_BIND_UNORDERED_ACCESS = 0x80.
    const GuestAddr uav_1d = make_rt_1d(28, 0x80);
    ASSERT_NE(uav_1d, 0u);
    const auto create_uav_1d = [&](GuestAddr resource, u32 dimension) {
        ASSERT_TRUE(memory.write_u32(RT_DESC, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(RT_DESC + 4, dimension).is_ok());
        ASSERT_TRUE(memory.write_u32(RT_DESC + 8, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(RT_DESC + 12, 0).is_ok());
        set_arg(0, device);
        set_arg(1, resource);
        set_arg(2, RT_DESC);
        set_arg(3, RT_OUT);
        d3d11.impl_DeviceMethod(marshaler, 8);  // CreateUnorderedAccessView
    };
    create_uav_1d(uav_1d, 2);  // D3D11_UAV_DIMENSION_TEXTURE1D
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "a 1D UAV of a 1D resource";
    create_uav_1d(uav_1d, 4);  // TEXTURE2D
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "2D UAV of a 1D resource";

    // D3D11_UAV_DIMENSION_TEXTURE2DARRAY is 5. A cubemap has no UAV
    // dimension; its faces are a 2D array. Width, Height, Mips, Array,
    // Format, Samples, Quality, Usage, Bind(UAV), CPU, Misc(TEXTURECUBE).
    const std::array<u32, 11> cube{
        128, 128, 1, 6, 28, 1, 0, 0, 0x80, 0, 0x4};
    ASSERT_TRUE(memory.write_bytes(
        DESC, reinterpret_cast<const Byte*>(cube.data()),
        sizeof(cube)).is_ok());
    set_arg(0, device);
    set_arg(1, DESC);
    set_arg(2, 0);
    set_arg(3, OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr cube_tex = memory.read_u64(OUT).value();
    ASSERT_NE(cube_tex, 0u);
    ASSERT_TRUE(memory.write_u32(RT_DESC, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(RT_DESC + 4, 5).is_ok());
    ASSERT_TRUE(memory.write_u32(RT_DESC + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(RT_DESC + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(RT_DESC + 16, 6).is_ok());
    set_arg(0, device);
    set_arg(1, cube_tex);
    set_arg(2, RT_DESC);
    set_arg(3, RT_OUT);
    d3d11.impl_DeviceMethod(marshaler, 8);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK)
        << "a 2D-array UAV over a cubemap's six faces";

    // An array Texture1D takes the array view dimension by default.
    const std::array<u32, 8> array_desc{64, 1, 4, 28, 0, 0x8, 0, 0};
    const GuestAddr array_texture = create_1d(array_desc, OUT);
    ASSERT_NE(array_texture, 0u);
    set_arg(0, device);
    set_arg(1, array_texture);
    set_arg(2, 0);  // null desc: the default view covers the whole resource
    set_arg(3, SRV_O);
    d3d11.impl_DeviceMethod(marshaler, 7);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
}

TEST_F(D3D11ManagerTest, EventAndTimestampQueriesAnswerFromTheGpuPoint) {
    // Learn (D3D11_QUERY, ID3D11DeviceContext::GetData): an EVENT reports
    // TRUE once the GPU has finished the commands before its End; a TIMESTAMP
    // is the GPU's counter when it reaches the End, in TIMESTAMP_DISJOINT
    // Frequency units; until then GetData is S_FALSE. Without
    // D3D11_ASYNC_GETDATA_DONOTFLUSH (0x1) GetData flushes the command buffer.
    // Answering from the host clock at End timed command RECORDING, which
    // gave UE's GPU benchmark a ~100x wrong result.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    ASSERT_NE(context, 0u);
    constexpr GuestAddr SCRATCH = GPU_BUFFER_DESC + 0x500;
    constexpr GuestAddr DESC = SCRATCH + 0x00;
    constexpr GuestAddr OUT = SCRATCH + 0x20;
    constexpr GuestAddr DATA = SCRATCH + 0x60;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const auto create_query = [&](u32 type) {
        EXPECT_TRUE(memory.write_u32(DESC, type).is_ok());
        EXPECT_TRUE(memory.write_u32(DESC + 4, 0).is_ok());
        set_arg(0, device);
        set_arg(1, DESC);
        set_arg(2, OUT);
        d3d11.impl_DeviceMethod(marshaler, 24);  // CreateQuery
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(OUT).value();
    };
    const auto call = [&](u32 slot, GuestAddr query) {
        set_arg(0, context);
        set_arg(1, query);
        d3d11.impl_ContextMethod(marshaler, slot);
    };
    const auto get_data = [&](GuestAddr query, u32 size, u32 flags) {
        EXPECT_TRUE(memory.write_u64(DATA, 0xEEEEEEEEEEEEEEEEull).is_ok());
        EXPECT_TRUE(memory.write_u64(DATA + 8, 0xEEEEEEEEEEEEEEEEull).is_ok());
        set_arg(0, context);
        set_arg(1, query);
        set_arg(2, DATA);
        set_arg(3, size);
        set_arg(4, flags);
        d3d11.impl_ContextMethod(marshaler, 29);  // GetData
        return static_cast<u32>(cpu.gpr.rax);
    };

    const GuestAddr disjoint = create_query(D3D11_QUERY_TIMESTAMP_DISJOINT);
    const GuestAddr stamp = create_query(D3D11_QUERY_TIMESTAMP);
    const GuestAddr event = create_query(D3D11_QUERY_EVENT);
    ASSERT_TRUE(disjoint != 0 && stamp != 0 && event != 0);

    call(27, disjoint);  // Begin
    call(28, stamp);     // End
    call(28, event);
    call(28, disjoint);
    ASSERT_EQ(backend.state.gpu_points.size(), 3u)
        << "each End records its own point in the command stream";
    TestGpuPoint& stamp_point = *backend.state.gpu_points[0];
    TestGpuPoint& event_point = *backend.state.gpu_points[1];
    TestGpuPoint& disjoint_point = *backend.state.gpu_points[2];

    // Not reached yet: S_FALSE, and a flushing poll flushes.
    const unsigned flushes = backend.state.flushes;
    EXPECT_EQ(get_data(event, 4, 0), 1u);
    EXPECT_EQ(backend.state.flushes, flushes + 1);
    EXPECT_EQ(memory.read_u32(DATA).value(), 0xEEEEEEEEu)
        << "nothing is written while the query is not ready";
    EXPECT_EQ(get_data(event, 4, /*DONOTFLUSH=*/1), 1u);
    EXPECT_EQ(backend.state.flushes, flushes + 1) << "DONOTFLUSH must not flush";
    EXPECT_EQ(get_data(stamp, 8, 1), 1u);
    EXPECT_EQ(get_data(disjoint, 16, 1), 1u);

    // The GPU reaches the End points.
    stamp_point.is_ready = true;
    stamp_point.ns = 123456789012ull;
    event_point.is_ready = true;
    disjoint_point.is_ready = true;
    EXPECT_EQ(get_data(stamp, 8, 1), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(DATA).value(), 123456789012ull)
        << "a timestamp is the GPU time of its point, not the time of End";
    EXPECT_EQ(get_data(event, 4, 1), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(DATA).value(), 1u);
    set_arg(0, context);
    set_arg(1, event);
    set_arg(2, 0);
    set_arg(3, 0);
    set_arg(4, 1);
    d3d11.impl_ContextMethod(marshaler, 29);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "status poll: pData NULL, size 0";
    EXPECT_EQ(get_data(disjoint, 16, 1), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(DATA).value(), 1000000000ull);
    EXPECT_EQ(memory.read_u32(DATA + 8).value(), 0u) << "Disjoint FALSE";

    // A second End re-arms the query at a new point.
    call(28, event);
    ASSERT_EQ(backend.state.gpu_points.size(), 4u);
    EXPECT_EQ(get_data(event, 4, 1), 1u);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
