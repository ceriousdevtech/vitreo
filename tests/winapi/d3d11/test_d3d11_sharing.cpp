// SPDX-License-Identifier: LGPL-2.1-only
//
// Shared resources, NT handles, names, abandoned keyed mutex and the cross-process keyed mutex.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_sharing.hpp"
#include "d3d11_test_fixture_x86.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11SharingTest, GuaranteedFormatsOpenWithTheCreatorsDescription) {
    // [formats] 16x8, 1 mip, 1 slice, DEFAULT, SRV|RTV: each of the D3D11.1
    // guaranteed formats shares with SH, KM, SHNT and KMNT; the opening's
    // description is the creator's, misc flags included ("od=...x0x2",
    // "x0x100", "x0x802", "x0x900"), and only a KEYEDMUTEX resource answers
    // IDXGIKeyedMutex ("km=1").
    const u32 formats[] = {28, 29, 87, 91, 88, 93, 24, 10};
    const u32 miscs[] = {0x2, 0x100, 0x802, 0x900};
    for (const u32 format : formats) {
        for (const u32 misc : miscs) {
            SCOPED_TRACE(testing::Message() << "format " << format << " misc 0x"
                                            << std::hex << misc);
            const GuestAddr texture = make(format, misc);
            ASSERT_EQ(last_hr, d3d11::S_OK);
            const bool nt = (misc & 0x800u) != 0;
            u64 handle = 0;
            ASSERT_EQ(nt ? create_nt(texture, 0x80000001u, handle)
                         : get_kmt(texture, handle),
                      d3d11::S_OK);
            ASSERT_NE(handle, 0u);
            GuestAddr opened = 0;
            ASSERT_EQ(open(nt ? 48 : 28, handle, winapi::IID_ID3D11TEXTURE2D,
                           opened),
                      d3d11::S_OK);
            ASSERT_NE(opened, 0u);
            EXPECT_NE(opened, texture);
            const std::array<u32, 11> expected{16, 8, 1, 1, format, 1, 0, 0,
                                               0x28, 0, misc};
            EXPECT_EQ(desc_of(opened), expected);
            EXPECT_EQ(answers(opened, winapi::IID_IDXGIKEYEDMUTEX),
                      (misc & 0x100u) != 0);
            if (nt) (void)handles.close(static_cast<Handle>(handle));
            release_texture(opened);
            release_texture(texture);
        }
    }
}

TEST_F(D3D11SharingTest, ShareableFormatsAndShapesAreConsistent) {
    // [caps] SHAREABLE: every guaranteed and ExtendedResourceSharing
    // (non-video) format is SHAREABLE on Windows, and each is one this
    // device has shared storage for.
    const u32 shareable[] = {28, 29, 87, 91, 88, 93, 24, 10, 1, 2, 3, 4, 9,
                             11, 12, 13, 14, 25, 27, 30, 31, 32, 90, 92, 39,
                             41, 42, 43, 53, 54, 56, 57, 58, 59, 60, 61, 62,
                             63, 64, 65, 26, 49, 34};
    constexpr GuestAddr FORMAT2 = DATA_BASE + 0x1B00;
    const auto support2 = [&](u32 format) {
        EXPECT_TRUE(memory.write_u32(FORMAT2, format).is_ok());
        EXPECT_TRUE(memory.write_u32(FORMAT2 + 4, 0).is_ok());
        (void)dispatch_device(device, 33, {3, FORMAT2, 8});
        return memory.read_u32(FORMAT2 + 4).value();
    };
    for (const u32 format : shareable) {
        SCOPED_TRACE(format);
        EXPECT_NE(support2(format) & 0x400u, 0u);
        // With SHARED_NTHANDLE the runtime validates the format: it creates.
        (void)make(format, 0x802);
        EXPECT_EQ(last_hr, d3d11::S_OK);
    }
    // A backend without extended sharing, unlike WARP (SHAREABLE 1 there),
    // has no shared form for a block-compressed or depth format: SHAREABLE
    // is clear, and an NT-handle resource of one is refused where its
    // creation is validated.
    for (const u32 format : {71u, 40u}) {
        SCOPED_TRACE(format);
        EXPECT_EQ(support2(format) & 0x400u, 0u);
        (void)make(format, 0x802, format == 40u ? 0x40u : 0x8u);
        EXPECT_EQ(last_hr, d3d11::E_INVALIDARG);
        // Without NTHANDLE the runtime does not validate (Learn): created,
        // and the missing shared form fails where the handle is asked for.
        const GuestAddr texture =
            make(format, 0x2, format == 40u ? 0x40u : 0x8u);
        ASSERT_EQ(last_hr, d3d11::S_OK);
        u64 handle = 0;
        EXPECT_EQ(get_kmt(texture, handle), kIC);
        EXPECT_EQ(handle, 0u);
        release_texture(texture);
    }
    // Without ExtendedResourceSharing a shared Texture2D is the D3D11.1
    // guaranteed single image: mips / arrays / MSAA are created unshared
    // (no handle) and refused with NTHANDLE. WARP ([shapes]) -- and the Metal
    // backend -- share them.
    struct Shape {
        u32 mips, array, samples;
    };
    for (const Shape shape : {Shape{2, 1, 1}, Shape{1, 2, 1}, Shape{1, 1, 4}}) {
        SCOPED_TRACE(testing::Message() << shape.mips << "/" << shape.array
                                        << "/" << shape.samples);
        const GuestAddr texture =
            make(87, 0x2, 0x28, shape.mips, shape.array, shape.samples);
        ASSERT_EQ(last_hr, d3d11::S_OK);
        u64 handle = 0;
        EXPECT_EQ(get_kmt(texture, handle), kIC);
        release_texture(texture);
        (void)make(87, 0x802, 0x28, shape.mips, shape.array, shape.samples);
        EXPECT_EQ(last_hr, d3d11::E_INVALIDARG);
    }
    // [shapes] "RESOURCE_CLAMP SH create=0x80070057", "... SHNT".
    (void)make(87, 0x82);
    EXPECT_EQ(last_hr, d3d11::E_INVALIDARG);
    (void)make(87, 0x882);
    EXPECT_EQ(last_hr, d3d11::E_INVALIDARG);
    // [shapes] single-image variants share: bind none / RTV / SRV, UAV,
    // GDI_COMPATIBLE (x0x202 / x0xa02).
    for (const u32 bind : {0x0u, 0x20u, 0x8u}) {
        const GuestAddr texture = make(87, 0x802, bind);
        ASSERT_EQ(last_hr, d3d11::S_OK);
        release_texture(texture);
    }
    const GuestAddr uav = make(28, 0x802, 0xA8);
    EXPECT_EQ(last_hr, d3d11::S_OK);
    release_texture(uav);
    const GuestAddr gdi = make(87, 0xA02, 0x20);
    EXPECT_EQ(last_hr, d3d11::S_OK);
    u64 gdi_handle = 0;
    ASSERT_EQ(create_nt(gdi, 0x80000001u, gdi_handle), d3d11::S_OK);
    GuestAddr gdi_opened = 0;
    ASSERT_EQ(open(48, gdi_handle, winapi::IID_ID3D11TEXTURE2D, gdi_opened),
              d3d11::S_OK);
    EXPECT_EQ(desc_of(gdi_opened)[10], 0xA02u);
    release_texture(gdi_opened);
    release_texture(gdi);
}

TEST_F(D3D11SharingTest, OpenSharedResourceAnswersInterfacesAndValidates) {
    const GuestAddr kt = make(87, 0x2);
    const GuestAddr nt = make(87, 0x802);
    u64 kh = 0;
    u64 nh = 0;
    ASSERT_EQ(get_kmt(kt, kh), d3d11::S_OK);
    ASSERT_EQ(create_nt(nt, 0x80000001u, nh), d3d11::S_OK);
    // "GetSharedHandle again same value same=1".
    u64 again = 0;
    EXPECT_EQ(get_kmt(kt, again), d3d11::S_OK);
    EXPECT_EQ(again, kh);
    // "OpenSharedResource(kmt, <iid>)": the opening answers QueryInterface.
    const struct {
        const GuidBytes* iid;
        u32 hr;
    } rows[] = {
        {&winapi::IID_IUNKNOWN, d3d11::S_OK},
        {&winapi::IID_ID3D11RESOURCE, d3d11::S_OK},
        {&winapi::IID_ID3D11TEXTURE2D, d3d11::S_OK},
        {&winapi::IID_ID3D11TEXTURE2D1, d3d11::S_OK},
        {&winapi::IID_IDXGIRESOURCE, d3d11::S_OK},
        {&winapi::IID_IDXGIRESOURCE1, d3d11::S_OK},
        {&winapi::IID_IDXGISURFACE, d3d11::S_OK},
        {&winapi::IID_IDXGIKEYEDMUTEX, d3d11::E_NOINTERFACE},
        {&winapi::IID_ID3D11TEXTURE1D, d3d11::E_NOINTERFACE},
        {&winapi::IID_ID3D11BUFFER, d3d11::E_NOINTERFACE},
    };
    for (const auto& row : rows) {
        GuestAddr out = 0;
        EXPECT_EQ(open(28, kh, *row.iid, out), row.hr);
        if (row.hr == d3d11::S_OK) {
            EXPECT_NE(out, 0u);
            // Every opening is a new object ("again same=0").
            EXPECT_NE(out, kt);
            set_arg(0, out);
            Marshaler marshaler(memory);
            marshaler.attach_cpu(cpu);
            d3d11.impl_Texture2DMethod(marshaler, 2);  // Release
        } else {
            EXPECT_EQ(out, 0u) << "out-parameter NULLed on failure";
        }
    }
    GuestAddr out = 0;
    // "ppResource NULL hr=0x00000001" (both kinds, and whatever the IID).
    EXPECT_EQ(open(28, kh, winapi::IID_ID3D11TEXTURE2D, out, false),
              kShareSFalse);
    EXPECT_EQ(open(48, nh, winapi::IID_ID3D11TEXTURE2D, out, false),
              kShareSFalse);
    EXPECT_EQ(open(28, kh, winapi::IID_IDXGIKEYEDMUTEX, out, false),
              kShareSFalse);
    // "OpenSharedResource(nt)" / "OpenSharedResource1(kmt)" / NULL: the
    // handle kinds do not cross.
    EXPECT_EQ(open(28, nh, winapi::IID_ID3D11TEXTURE2D, out),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(out, 0u);
    EXPECT_EQ(open(48, kh, winapi::IID_ID3D11TEXTURE2D, out),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(open(28, 0, winapi::IID_ID3D11TEXTURE2D, out),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(open(48, 0, winapi::IID_ID3D11TEXTURE2D, out),
              d3d11::E_INVALIDARG);
    // "opened GetSharedHandle same=1" / "opened CreateSharedHandle
    // 0x80070057"; "NT-opened GetSharedHandle 0x80070057" / "NT-opened
    // CreateSharedHandle 0x00000000".
    GuestAddr k_opened = 0;
    ASSERT_EQ(open(28, kh, winapi::IID_ID3D11TEXTURE2D, k_opened),
              d3d11::S_OK);
    u64 value = 0;
    EXPECT_EQ(get_kmt(k_opened, value), d3d11::S_OK);
    EXPECT_EQ(value, kh);
    EXPECT_EQ(create_nt(k_opened, 0x80000001u, value), d3d11::E_INVALIDARG);
    GuestAddr n_opened = 0;
    ASSERT_EQ(open(48, nh, winapi::IID_ID3D11TEXTURE2D, n_opened),
              d3d11::S_OK);
    EXPECT_EQ(get_kmt(n_opened, value), d3d11::E_INVALIDARG);
    EXPECT_EQ(create_nt(n_opened, 0x80000001u, value), d3d11::S_OK);
    (void)handles.close(static_cast<Handle>(value));
    // "OpenSharedResource(kmt) on creating device ... same=0".
    write_iid(winapi::IID_ID3D11TEXTURE2D);
    ASSERT_EQ(dispatch_device(device, 28, {kh, IID, RESOURCE_OUT}),
              d3d11::S_OK);
    const GuestAddr self_opened = memory.read_u64(RESOURCE_OUT).value();
    EXPECT_NE(self_opened, kt);
    release_texture(self_opened);
    release_texture(k_opened);
    release_texture(n_opened);
    release_texture(kt);
    release_texture(nt);
}

TEST_F(D3D11SharingTest, NtHandlesAreKernelHandles) {
    const GuestAddr nt = make(87, 0x802);
    const GuestAddr kt = make(87, 0x2);
    u64 kh = 0;
    ASSERT_EQ(get_kmt(kt, kh), d3d11::S_OK);
    // "GetHandleInformation(kmt) ok=0 gle=6": a KMT value is no kernel
    // handle.
    EXPECT_FALSE(handles.is_valid(static_cast<Handle>(kh)));
    // "CreateSharedHandle pHandle NULL hr=0x887a0001".
    EXPECT_EQ(dispatch_resource(nt, 13, {0, 0x80000001u, 0, 0}), kIC);
    u64 nh = 0;
    ASSERT_EQ(create_nt(nt, 0x80000001u, nh), d3d11::S_OK);
    // "GetHandleInformation(nt) ok=1 flags=0x0".
    ASSERT_TRUE(handles.is_valid(static_cast<Handle>(nh)));
    EXPECT_EQ(handles.get_type(static_cast<Handle>(nh)),
              HandleType::SharedResource);
    EXPECT_EQ(handles.get_flags(static_cast<Handle>(nh)), 0u);
    // "DuplicateHandle(nt) same process ok=1", "OpenSharedResource1
    // (duplicate) 0x00000000", "CloseHandle(duplicate) ok=1", again ok=0.
    const Handle dup = duplicate_shared_resource_handle(
        handles, static_cast<Handle>(nh), handles.get_access(
            static_cast<Handle>(nh)), false);
    ASSERT_NE(dup, NULL_HANDLE);
    GuestAddr opened = 0;
    ASSERT_EQ(open(48, dup, winapi::IID_ID3D11TEXTURE2D, opened), d3d11::S_OK);
    release_texture(opened);
    EXPECT_TRUE(handles.close(dup));
    EXPECT_FALSE(handles.close(dup));
    EXPECT_EQ(open(48, dup, winapi::IID_ID3D11TEXTURE2D, opened),
              d3d11::E_INVALIDARG);
    // "CreateSharedHandle access ...": every access creates; the opening
    // needs read access (E_ACCESSDENIED otherwise).
    const struct {
        u32 access;
        u32 open_hr;
    } rows[] = {
        {0x0u, kShareAccessDenied},
        {0x80000000u, d3d11::S_OK},         // DXGI_SHARED_RESOURCE_READ
        {0x1u, kShareAccessDenied},         // DXGI_SHARED_RESOURCE_WRITE
        {0x10000000u, d3d11::S_OK},         // GENERIC_ALL
        {0xC0000000u, d3d11::S_OK},         // GENERIC_READ|GENERIC_WRITE
        {0x2u, kShareAccessDenied},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.access);
        u64 handle = 0;
        ASSERT_EQ(create_nt(nt, row.access, handle), d3d11::S_OK);
        EXPECT_EQ(open(48, handle, winapi::IID_ID3D11TEXTURE2D, opened),
                  row.open_hr);
        if (row.open_hr == d3d11::S_OK) release_texture(opened);
        EXPECT_TRUE(handles.close(static_cast<Handle>(handle)));
    }
    // "CreateSharedHandle inheritable inherit=1": SECURITY_ATTRIBUTES
    // {nLength, lpSecurityDescriptor, bInheritHandle} (x64: 16).
    ASSERT_TRUE(memory.write_u32(ATTRIBUTES, 24).is_ok());
    ASSERT_TRUE(memory.write_u64(ATTRIBUTES + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(ATTRIBUTES + 16, 1).is_ok());
    u64 inherit = 0;
    ASSERT_EQ(create_nt(nt, 0x80000001u, inherit, 0, ATTRIBUTES), d3d11::S_OK);
    EXPECT_EQ(handles.get_flags(static_cast<Handle>(inherit)) & 1u, 1u);
    (void)handles.close(static_cast<Handle>(inherit));
    // "OpenSharedResource1(nt) after creator released 0x00000000 data=match":
    // the NT handle keeps the resource.
    release_texture(nt);
    ASSERT_EQ(open(48, nh, winapi::IID_ID3D11TEXTURE2D, opened), d3d11::S_OK);
    EXPECT_EQ(desc_of(opened)[10], 0x802u);
    // "reopened CreateSharedHandle 0x00000000".
    u64 reopened = 0;
    EXPECT_EQ(create_nt(opened, 0x80000001u, reopened), d3d11::S_OK);
    (void)handles.close(static_cast<Handle>(reopened));
    release_texture(opened);
    // "CloseHandle(nt) ok=1".
    EXPECT_TRUE(handles.close(static_cast<Handle>(nh)));
    release_texture(kt);
}

TEST_F(D3D11SharingTest, NamesShareTheKernelObjectNamespace) {
    const GuestAddr nt = make(87, 0x802);
    const GuestAddr other = make(87, 0x802);
    // A name no other test run uses (the registry is visible to every
    // Vitreo process of the scope).
    const std::string pid = std::to_string(::getpid());
    const std::u16string base = u"vitreo_share_test_" +
                                std::u16string(pid.begin(), pid.end());
    const std::u16string local = u"Local\\" + base;
    std::u16string upper = local;
    for (char16_t& c : upper) {
        if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - 32);
    }
    u64 named = 0;
    ASSERT_EQ(create_nt(nt, 0x80000001u, named, write_name(local.c_str())),
              d3d11::S_OK);
    u64 again = 0;
    // "CreateSharedHandle same name same resource 0x887a002c", "... other
    // resource 0x887a002c".
    EXPECT_EQ(create_nt(nt, 0x80000001u, again, write_name(local.c_str())),
              kShareNameExists);
    EXPECT_EQ(create_nt(other, 0x80000001u, again, write_name(local.c_str())),
              kShareNameExists);
    // "CreateSharedHandle name of an event 0x887a002c" and "CreateEvent with
    // a shared resource's name ok=0 gle=6": one namespace.
    const std::string event_name = "Local\\vitreo_share_event_" + pid;
    ASSERT_TRUE(named_object_register(event_name, NamedObjectKind::Event));
    const std::u16string event16(event_name.begin(), event_name.end());
    EXPECT_EQ(create_nt(other, 0x80000001u, again,
                        write_name(event16.c_str())),
              kShareNameExists);
    named_object_unregister(event_name, NamedObjectKind::Event);
    const std::string local8(local.begin(), local.end());
    EXPECT_TRUE(named_object_kind_conflict(local8, NamedObjectKind::Event));
    // "OpenSharedResourceByName ...".
    const struct {
        std::u16string name;
        u32 access;
        u32 hr;
    } rows[] = {
        {local, 0x80000001u, d3d11::S_OK},
        {local, 0x80000000u, d3d11::S_OK},
        {local, 0x1u, kShareNotAvailable},
        {local, 0x0u, kShareNotAvailable},
        {local, 0x10000000u, d3d11::S_OK},
        {base, 0x80000001u, d3d11::S_OK},     // "no Local prefix"
        {upper, 0x80000001u, d3d11::S_OK},    // "other case"
        {local + u"_none", 0x80000001u, d3d11::E_INVALIDARG},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(std::string(row.name.begin(), row.name.end()));
        GuestAddr opened = 0;
        EXPECT_EQ(open_by_name(row.name.c_str(), row.access, opened), row.hr);
        if (row.hr == d3d11::S_OK) {
            ASSERT_NE(opened, 0u);
            EXPECT_EQ(desc_of(opened)[10], 0x802u);
            release_texture(opened);
        } else {
            EXPECT_EQ(opened, 0u);
        }
    }
    GuestAddr opened = 0;
    EXPECT_EQ(open_by_name(nullptr, 0x80000001u, opened), d3d11::E_INVALIDARG);
    // "OpenSharedResourceByName after CloseHandle 0x80070057": the name
    // belongs to the handle's object, not to the texture.
    EXPECT_TRUE(handles.close(static_cast<Handle>(named)));
    EXPECT_EQ(open_by_name(local.c_str(), 0x80000001u, opened),
              d3d11::E_INVALIDARG);
    EXPECT_FALSE(named_object_kind_conflict(local8, NamedObjectKind::Event));
    release_texture(nt);
    release_texture(other);
}

TEST_F(D3D11SharingTest, KeyedMutexOwnedByADestroyedOpeningIsAbandoned) {
    // [handles] "KMNT ...": ownership moves between openings by key; an
    // opening released while it owns the mutex abandons it, and from then on
    // every AcquireSync -- any key -- is WAIT_ABANDONED and ReleaseSync
    // DXGI_ERROR_INVALID_CALL.
    const GuestAddr k = make(87, 0x900);
    u64 h = 0;
    ASSERT_EQ(create_nt(k, 0x80000001u, h), d3d11::S_OK);
    GuestAddr o = 0;
    ASSERT_EQ(open(48, h, winapi::IID_ID3D11TEXTURE2D, o), d3d11::S_OK);
    EXPECT_EQ(sync(k, 8, 0, 0), d3d11::S_OK);
    EXPECT_EQ(sync(o, 8, 0, 0), kShareTimeout);
    EXPECT_EQ(sync(k, 9, 3, 0), d3d11::S_OK);
    EXPECT_EQ(sync(o, 8, 3, 0), d3d11::S_OK);
    EXPECT_EQ(sync(o, 9, 4, 0), d3d11::S_OK);
    GuestAddr third = 0;
    ASSERT_EQ(open(48, h, winapi::IID_ID3D11TEXTURE2D, third), d3d11::S_OK);
    EXPECT_EQ(sync(third, 8, 4, 0), d3d11::S_OK);
    release_texture(third);
    EXPECT_EQ(sync(k, 8, 4, 100), kShareAbandoned);
    EXPECT_EQ(sync(k, 8, 9, 0), kShareAbandoned);
    EXPECT_EQ(sync(k, 9, 4, 0), kIC);
    EXPECT_EQ(sync(o, 8, 4, 0), kShareAbandoned);
    release_texture(o);
    release_texture(k);
    (void)handles.close(static_cast<Handle>(h));
}

TEST_F(D3D11SharingTest, BuffersAndOneDimensionalTexturesHaveNoSharedForm) {
    // Windows shares these ([handles] "CreateBuffer SHARED ... kmt set",
    // "CreateTexture1D SHARED ... kmt set"); a backend without extended
    // sharing has no shared storage for them, and says so where the handle
    // is asked for instead of handing out a NULL "success".
    const std::array<u32, 6> buffer_desc{256, 0, 0x8, 0, 0x2, 0};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(buffer_desc.data()),
        sizeof(buffer_desc)).is_ok());
    ASSERT_EQ(dispatch_device(device, 3, {GPU_BUFFER_DESC, 0, GPU_BUFFER_OUT}),
              d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    write_iid(winapi::IID_IDXGIRESOURCE);
    set_arg(0, buffer);
    set_arg(1, IID);
    set_arg(2, OUT2);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_BufferMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, memory.read_u64(OUT2).value());
    set_arg(1, OUT);
    d3d11.impl_DxgiResourceMethod(marshaler, 8);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kIC);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
}

TEST_F(D3D11SharingTest, FeatureLevel9RefusesSharedR8G8B8A8) {
    // [fl9_3]: SHAREABLE is still reported for R8G8B8A8_UNORM(_SRGB), but
    // Learn D3D11_FORMAT_SUPPORT2_SHAREABLE: they "are never shareable when
    // using feature level 9"; creating one shared fails (SH, KM, SH|NT all
    // 0x80070057) while B8G8R8A8_UNORM and R16G16B16A16_FLOAT create.
    ASSERT_TRUE(memory.write_u32(LEVELS, d3d11::FEATURE_LEVEL_9_3).is_ok());
    prepare_create();
    set_arg(4, LEVELS);
    set_arg(5, 1);
    {
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_D3D11CreateDevice(marshaler);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    }
    const GuestAddr fl9 = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr FORMAT2 = DATA_BASE + 0x1B00;
    for (const u32 format : {28u, 29u, 87u, 10u}) {
        SCOPED_TRACE(format);
        ASSERT_TRUE(memory.write_u32(FORMAT2, format).is_ok());
        EXPECT_EQ(dispatch_device(fl9, 33, {3, FORMAT2, 8}), d3d11::S_OK);
        EXPECT_NE(memory.read_u32(FORMAT2 + 4).value() & 0x400u, 0u);
        const bool refused = format == 28u || format == 29u;
        for (const u32 misc : {0x2u, 0x100u, 0x802u}) {
            const GuestAddr texture = make(format, misc, 0x28, 1, 1, 1, 0, 0,
                                           fl9);
            EXPECT_EQ(last_hr, refused ? d3d11::E_INVALIDARG : d3d11::S_OK);
            release_texture(texture);
        }
    }
}

TEST_F(D3D11SharingTest, ForeignPresentHandsOverOnlyBgraSurfaces) {
    // A flip chain on another process's HWND hands its back buffer's shared
    // storage to the owner, which presents it as 32-bit BGRA (the only
    // format present_shared_window_surface accepts). A shared back buffer of
    // another format -- which now has shared storage too -- must still take
    // the pixel transport, or the owner would reject every frame.
    backend.state.force_shared_handle = true;
    backend.state.unique_shared_handles = true;
    backend.state.wait_idle_ok = true;
    for (const u32 format : {87u, 10u}) {
        SCOPED_TRACE(format);
        std::atomic<unsigned> surfaces{0};
        std::atomic<unsigned> pixels{0};
        d3d11.set_foreign_window_presenter(
            [](u64 hwnd) { return hwnd == kWindow; },
            [&](u64, const std::vector<u8>&, u32, u32) {
                ++pixels;
                return true;
            },
            [&](u64, u64, u32, u32, u32, u32, u32) {
                ++surfaces;
                return true;
            });
        backend.state.readback_rgba.assign(64u * 48u * 4u, 0x5Au);
        Desc desc;
        desc.format = format;
        desc.count = 2;
        desc.effect = 3;  // FLIP_SEQUENTIAL
        GuestAddr chain = 0;
        ASSERT_EQ(create(desc, &chain), d3d11::S_OK);
        EXPECT_EQ(call(chain, 8, {0, 0}), d3d11::S_OK);  // Present
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (format == 87u && surfaces.load() == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (format == 87u) {
            EXPECT_EQ(surfaces.load(), 1u);
            EXPECT_EQ(pixels.load(), 0u);
        } else {
            EXPECT_EQ(surfaces.load(), 0u);
            EXPECT_EQ(pixels.load(), 1u);
        }
        EXPECT_EQ(call(chain, 2, {}), 0u);  // Release
        d3d11.set_foreign_window_presenter({}, {}, {});
    }
}


/// A GPU completion the test finishes by hand.
class ManualCompletion final : public graphics::GpuCompletion {
public:
    bool wait() override {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return m_done; });
        return true;
    }
    void finish() {
        {
            std::lock_guard lock(m_mutex);
            m_done = true;
        }
        m_cv.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_done{false};
};


// The keyed-mutex state crosses processes (the probe's [xproc] "keyed" and
// "keyedexit" rows): another process acquires a released key only once the
// releaser's GPU work has completed, and a process that exits owning the
// mutex abandons it (WAIT_ABANDONED, measured).
TEST(SharedKeyedMutexProcessTest, ReleaseReachesAnotherProcessAfterItsGpuWork) {
    const u32 surface = static_cast<u32>(::getpid());
    u64 nonce = 0;
    arc4random_buf(&nonce, sizeof(nonce));
    auto mutex = SharedKeyedMutex::attach(surface, nonce | 1u);
    ASSERT_NE(mutex, nullptr);
    ASSERT_TRUE(mutex->cross_process());
    ASSERT_EQ(mutex->acquire(1, 0, 0), SharedKeyedMutex::Result::Acquired);
    auto pending = std::make_shared<ManualCompletion>();
    ASSERT_TRUE(mutex->release(1, 7, pending));
    // Openings of this process are ordered after the work already.
    EXPECT_EQ(mutex->acquire(2, 7, 0), SharedKeyedMutex::Result::Acquired);
    ASSERT_TRUE(mutex->release(2, 7, pending));

    int ready[2];
    ASSERT_EQ(::pipe(ready), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        auto other = SharedKeyedMutex::attach(surface, nonce | 1u);
        if (other == nullptr) ::_exit(10);
        // Released with key 7, but the releaser's GPU work is outstanding.
        if (other->acquire(1, 7, 50) != SharedKeyedMutex::Result::Timeout) {
            ::_exit(11);
        }
        char byte = 1;
        if (::write(ready[1], &byte, 1) != 1) ::_exit(12);
        if (other->acquire(1, 7, 5000) != SharedKeyedMutex::Result::Acquired) {
            ::_exit(13);
        }
        // Exit owning it.
        ::_exit(0);
    }
    char byte = 0;
    ASSERT_EQ(::read(ready[0], &byte, 1), 1);
    pending->finish();
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    ::close(ready[0]);
    ::close(ready[1]);
    // The child exited as the owner: abandoned, whatever the key.
    EXPECT_EQ(mutex->acquire(3, 7, 100), SharedKeyedMutex::Result::Abandoned);
    EXPECT_EQ(mutex->acquire(3, 99, 0), SharedKeyedMutex::Result::Abandoned);
    EXPECT_FALSE(mutex->release(3, 1));
}

TEST_F(D3D11ManagerX86Test, KeyedMutexReadsTheWholeUint64KeyOnX86) {
    // AcquireSync(this, UINT64 Key, DWORD dwMilliseconds): on x86 the key is
    // two stack slots and the timeout the fourth (measured: full 64-bit keys
    // honoured on x86 too).
    for (usize index = 0; index < 10; ++index) set_arg(index, 0);
    set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
    set_arg(6, d3d11::SDK_VERSION);
    set_arg(7, static_cast<u32>(DEVICE_OUT));
    set_arg(8, static_cast<u32>(LEVEL_OUT));
    set_arg(9, static_cast<u32>(CONTEXT_OUT));
    {
        Marshaler create(memory);
        create.attach_cpu(cpu);
        d3d11.impl_D3D11CreateDevice(create);
        ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    }
    const GuestAddr device = memory.read_u32(DEVICE_OUT).value();
    const std::array<u32, 11> desc{16, 16, 1, 1, 87, 1, 0, 0, 0x28, 0, 0x100};
    ASSERT_TRUE(memory.write_bytes(DATA_BASE + 0x400,
                                   reinterpret_cast<const Byte*>(desc.data()),
                                   sizeof(desc)).is_ok());
    set_arg(0, static_cast<u32>(device));
    set_arg(1, DATA_BASE + 0x400);
    set_arg(2, 0);
    set_arg(3, DATA_BASE + 0x440);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    const u32 texture = memory.read_u32(DATA_BASE + 0x440).value();
    const u32 mutex = texture + 0x28;  // the IDXGIKeyedMutex facet
    const auto call = [&](usize slot, u64 key, u32 ms) {
        set_arg(0, mutex);
        set_arg(1, static_cast<u32>(key));
        set_arg(2, static_cast<u32>(key >> 32));
        set_arg(3, ms);
        d3d11.impl_KeyedMutexMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };
    ASSERT_EQ(call(8, 0, 0), d3d11::S_OK);
    ASSERT_EQ(call(9, 0x200000001ull, 0), d3d11::S_OK);
    EXPECT_EQ(call(8, 1, 0), WAIT_TIMEOUT) << "the high half matters";
    EXPECT_EQ(call(8, 0x200000001ull, 0), d3d11::S_OK);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
