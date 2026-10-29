// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_audio_endpoints.cpp
/// @brief MMDevice endpoint, endpoint format, render client and ActivateAudioInterfaceAsync
/// tests.

#include "com_test_common.hpp"
#include "com_test_shell_link_fixture.hpp"
#include "com_test_audio_doubles.hpp"

TEST_F(ShellLinkEncodingTest,
       MMDeviceEnumeratorRepresentsNoEndpointsWithoutClassFailure) {
    constexpr Guid clsid_mmdevice_enumerator{
        0xBCDE0395u, 0xE52Fu, 0x467Cu,
        {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
    constexpr Guid iid_mmdevice_enumerator{
        0xA95664D2u, 0x9614u, 0x4F35u,
        {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
    constexpr GuestAddr collection_out = OUT_ADDR + 4;
    constexpr GuestAddr count_out = OUT_ADDR + 8;
    constexpr GuestAddr endpoint_out = OUT_ADDR + 12;
    constexpr GuestAddr notification_client = DATA_BASE + 0x900;

    write_guid(CLSID_ADDR, clsid_mmdevice_enumerator);
    write_guid(IID_ADDR, iid_mmdevice_enumerator);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 enumerator = read_u32(OUT_ADDR);
    ASSERT_NE(enumerator, 0u);
    const u32 enumerator_vtable = read_u32(enumerator);
    for (u32 slot = 0; slot < 8; ++slot) {
        EXPECT_NE(read_u32(enumerator_vtable + slot * 4), 0u) << "slot " << slot;
    }

    // A system with no WASAPI endpoints still returns an enumerator and a
    // valid empty collection from EnumAudioEndpoints.
    set_args({enumerator, 2, 1, collection_out}); // eAll, DEVICE_STATE_ACTIVE
    auto enum_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.mmdevice_enumerator_enum_audio_endpoints(enum_marshaler),
              com::S_OK);
    const u32 collection = read_u32(collection_out);
    ASSERT_NE(collection, 0u);
    const u32 collection_vtable = read_u32(collection);
    for (u32 slot = 0; slot < 5; ++slot) {
        EXPECT_NE(read_u32(collection_vtable + slot * 4), 0u) << "slot " << slot;
    }

    set_args({collection, count_out});
    auto count_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.mmdevice_collection_get_count(count_marshaler), com::S_OK);
    EXPECT_EQ(read_u32(count_out), 0u);

    ASSERT_TRUE(memory_.write_u32(endpoint_out, 0xDEADBEEFu).is_ok());
    set_args({enumerator, 0, 2, endpoint_out}); // eRender, eCommunications
    auto default_marshaler = make_marshaler();
    EXPECT_EQ(
        mgr_.mmdevice_enumerator_get_default_audio_endpoint(default_marshaler),
        0x80070490u); // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
    EXPECT_EQ(read_u32(endpoint_out), 0u);

    set_args({enumerator, notification_client});
    auto register_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.mmdevice_enumerator_register_notification(register_marshaler),
              com::S_OK);
    set_args({enumerator, notification_client});
    auto unregister_marshaler = make_marshaler();
    EXPECT_EQ(
        mgr_.mmdevice_enumerator_unregister_notification(unregister_marshaler),
        com::S_OK);
    set_args({enumerator, notification_client});
    auto missing_marshaler = make_marshaler();
    EXPECT_EQ(
        mgr_.mmdevice_enumerator_unregister_notification(missing_marshaler),
        0x80070490u);

    set_args({collection});
    auto collection_release_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.mmdevice_collection_release(collection_release_marshaler), 0u);
    set_args({enumerator});
    auto enumerator_release_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.mmdevice_enumerator_release(enumerator_release_marshaler), 0u);
}

TEST_F(ShellLinkEncodingTest,
       MMDeviceEnumeratorExposesActiveHostRenderEndpoint) {
    constexpr Guid clsid_mmdevice_enumerator{
        0xBCDE0395u, 0xE52Fu, 0x467Cu,
        {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
    constexpr Guid iid_mmdevice_enumerator{
        0xA95664D2u, 0x9614u, 0x4F35u,
        {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
    constexpr GuestAddr collection_out = OUT_ADDR + 4;
    constexpr GuestAddr count_out = OUT_ADDR + 8;
    constexpr GuestAddr endpoint_out = OUT_ADDR + 12;
    constexpr GuestAddr state_out = OUT_ADDR + 16;
    constexpr GuestAddr id_out = OUT_ADDR + 20;

    TestAudioRenderer renderer;
    mgr_.set_audio_renderer(&renderer);
    write_guid(CLSID_ADDR, clsid_mmdevice_enumerator);
    write_guid(IID_ADDR, iid_mmdevice_enumerator);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 enumerator = read_u32(OUT_ADDR);

    set_args({enumerator, 2, 1, collection_out});
    auto enum_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.mmdevice_enumerator_enum_audio_endpoints(enum_marshaler),
              com::S_OK);
    const u32 collection = read_u32(collection_out);
    set_args({collection, count_out});
    auto count_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.mmdevice_collection_get_count(count_marshaler), com::S_OK);
    EXPECT_EQ(read_u32(count_out), 1u);

    set_args({collection, 0, endpoint_out});
    auto item_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.mmdevice_collection_item(item_marshaler), com::S_OK);
    const u32 endpoint = read_u32(endpoint_out);
    ASSERT_NE(endpoint, 0u);
    const u32 endpoint_vtable = read_u32(endpoint);
    for (u32 slot = 0; slot < 7; ++slot) {
        EXPECT_NE(read_u32(endpoint_vtable + slot * 4), 0u) << "slot " << slot;
    }

    set_args({endpoint, state_out});
    auto state_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.mmdevice_get_state(state_marshaler), com::S_OK);
    EXPECT_EQ(read_u32(state_out), 1u);

    set_args({endpoint, id_out});
    auto id_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.mmdevice_get_id(id_marshaler), com::S_OK);
    const u32 id = read_u32(id_out);
    ASSERT_NE(id, 0u);
    EXPECT_EQ(memory_.read_u16(id).value(), static_cast<u16>('V'));

    set_args({enumerator, 0, 0, endpoint_out});
    auto default_render = make_marshaler();
    EXPECT_EQ(mgr_.mmdevice_enumerator_get_default_audio_endpoint(default_render),
              com::S_OK);
    EXPECT_NE(read_u32(endpoint_out), 0u);

    set_args({enumerator, 1, 0, endpoint_out});
    auto default_capture = make_marshaler();
    EXPECT_EQ(mgr_.mmdevice_enumerator_get_default_audio_endpoint(default_capture),
              0x80070490u);
    EXPECT_EQ(read_u32(endpoint_out), 0u);
}

// Learn (Core Audio device properties): PKEY_AudioEngine_DeviceFormat,
// {f19f064d-082c-4e27-bc73-6882a1bb8e4c},0, is a VT_BLOB holding the
// endpoint's WAVEFORMATEX. It must agree with IAudioClient::GetMixFormat —
// a client reads the property to size its pipeline and then initialises the
// client with what it read.
//
// Answering S_OK/VT_EMPTY instead is not a harmless "property absent": the
// client sees NumChannels=0 and SampleRate=0, fails to create its render
// pipeline, and falls back to a simulated null device. In UE that fallback
// runs a callback thread that deadlocked against the game thread.
TEST_F(ShellLinkEncodingTest, AudioEndpointReportsDeviceFormatMatchingMixFormat) {
    constexpr Guid clsid_mmdevice_enumerator{
        0xBCDE0395u, 0xE52Fu, 0x467Cu,
        {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
    constexpr Guid iid_mmdevice_enumerator{
        0xA95664D2u, 0x9614u, 0x4F35u,
        {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
    constexpr Guid pkey_audioengine_fmtid{
        0xF19F064Du, 0x082Cu, 0x4E27u,
        {0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C}};
    constexpr GuestAddr endpoint_out = OUT_ADDR + 12;
    constexpr GuestAddr store_out = OUT_ADDR + 40;
    constexpr GuestAddr key_addr = DATA_BASE + 0x900;
    constexpr GuestAddr propvariant = DATA_BASE + 0x940;

    TestAudioRenderer renderer;
    mgr_.set_audio_renderer(&renderer);
    write_guid(CLSID_ADDR, clsid_mmdevice_enumerator);
    write_guid(IID_ADDR, iid_mmdevice_enumerator);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 enumerator = read_u32(OUT_ADDR);

    set_args({enumerator, 0, 0, endpoint_out});
    auto default_render = make_marshaler();
    ASSERT_EQ(mgr_.mmdevice_enumerator_get_default_audio_endpoint(default_render),
              com::S_OK);
    const u32 endpoint = read_u32(endpoint_out);
    ASSERT_NE(endpoint, 0u);

    set_args({endpoint, 0, store_out});
    auto store_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.mmdevice_open_property_store(store_marshaler), com::S_OK);
    const u32 store = read_u32(store_out);
    ASSERT_NE(store, 0u);

    // PROPERTYKEY = { GUID fmtid; DWORD pid; }
    write_guid(key_addr, pkey_audioengine_fmtid);
    ASSERT_TRUE(memory_.write_u32(key_addr + sizeof(Guid), 0u).is_ok());
    for (u32 i = 0; i < 24; i += 4) {
        ASSERT_TRUE(memory_.write_u32(propvariant + i, 0xCCCCCCCCu).is_ok());
    }
    set_args({store, key_addr, propvariant});
    auto value_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.property_store_get_value(value_marshaler), com::S_OK);

    // VT_BLOB with a WAVEFORMATEX-sized payload, not VT_EMPTY.
    EXPECT_EQ(memory_.read_u16(propvariant).value(), 65u);
    const u32 blob_size = read_u32(propvariant + 8);
    EXPECT_EQ(blob_size, 18u);
    const u32 blob = read_u32(propvariant + 12);  // 32-bit guest layout
    ASSERT_NE(blob, 0u);

    // It must agree with the mix format GetMixFormat hands out on rate and
    // channel count (the sample representation may differ: Learn,
    // GetMixFormat — an integer device behind a floating-point engine).
    constexpr GuestAddr mix_out = OUT_ADDR + 44;
    set_args({0, mix_out});
    auto mix_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.audio_client_get_mix_format(mix_marshaler), com::S_OK);
    const u32 mix = read_u32(mix_out);
    ASSERT_NE(mix, 0u);
    EXPECT_EQ(memory_.read_u16(blob + 2).value(), memory_.read_u16(mix + 2).value());
    EXPECT_EQ(read_u32(blob + 4), read_u32(mix + 4));
    // Specifically: a usable channel count and sample rate.
    EXPECT_EQ(memory_.read_u16(blob + 2).value(), 2u);      // nChannels
    EXPECT_EQ(read_u32(blob + 4), 48000u);                  // nSamplesPerSec
    EXPECT_EQ(memory_.read_u16(blob + 14).value(), 16u);    // wBitsPerSample
}

TEST_F(ShellLinkEncodingTest, WasapiRenderClientSubmitsExtensiblePcm) {
    constexpr GuestAddr format = DATA_BASE + 0x700;
    constexpr GuestAddr buffer_size = OUT_ADDR + 24;
    constexpr GuestAddr render_out = OUT_ADDR + 28;
    constexpr GuestAddr samples_out = OUT_ADDR + 32;
    constexpr Guid iid_audio_render_client{
        0xF294ACFCu, 0x3146u, 0x4483u,
        {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
    TestAudioRenderer renderer;
    mgr_.set_audio_renderer(&renderer);
    auto marshaler = make_marshaler();
    const GuestAddr client = mgr_.create_audio_client(marshaler);
    ASSERT_NE(client, 0u);

    // WAVEFORMATEXTENSIBLE, stereo 48-kHz 16-bit PCM:
    // KSDATAFORMAT_SUBTYPE_PCM {00000001-0000-0010-8000-00aa00389b71}.
    ASSERT_TRUE(memory_.write_u16(format, 0xFFFEu).is_ok());
    ASSERT_TRUE(memory_.write_u16(format + 2, 2u).is_ok());
    ASSERT_TRUE(memory_.write_u32(format + 4, 48000u).is_ok());
    ASSERT_TRUE(memory_.write_u32(format + 8, 192000u).is_ok());
    ASSERT_TRUE(memory_.write_u16(format + 12, 4u).is_ok());
    ASSERT_TRUE(memory_.write_u16(format + 14, 16u).is_ok());
    ASSERT_TRUE(memory_.write_u16(format + 16, 22u).is_ok());
    ASSERT_TRUE(memory_.write_u16(format + 18, 16u).is_ok());
    ASSERT_TRUE(memory_.write_u32(format + 20, 3u).is_ok());
    write_guid(format + 24, Guid{0x00000001u, 0x0000u, 0x0010u,
                                 {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}});

    // x86 ABI: each REFERENCE_TIME occupies two stack slots.
    set_args({static_cast<u32>(client), 0, 0, 400000u, 0, 0, 0,
              static_cast<u32>(format), 0});
    auto init = make_marshaler();
    ASSERT_EQ(mgr_.audio_client_initialize(init), com::S_OK);
    EXPECT_EQ(renderer.last_format.sample_rate, 48000u);
    ASSERT_NE(renderer.last_push, nullptr);

    set_args({static_cast<u32>(client), static_cast<u32>(buffer_size)});
    auto size = make_marshaler();
    ASSERT_EQ(mgr_.audio_client_get_buffer_size(size), com::S_OK);
    EXPECT_EQ(read_u32(buffer_size), 1920u);

    write_guid(IID_ADDR, iid_audio_render_client);
    set_args({static_cast<u32>(client), static_cast<u32>(IID_ADDR),
              static_cast<u32>(render_out)});
    auto service = make_marshaler();
    ASSERT_EQ(mgr_.audio_client_get_service(service), com::S_OK);
    const u32 render = read_u32(render_out);
    ASSERT_NE(render, 0u);

    set_args({render, 4u, static_cast<u32>(samples_out)});
    auto get_buffer = make_marshaler();
    ASSERT_EQ(mgr_.audio_render_client_get_buffer(get_buffer), com::S_OK);
    const u32 samples = read_u32(samples_out);
    ASSERT_NE(samples, 0u);
    ASSERT_TRUE(memory_.write_u16(samples, 0x1234u).is_ok());
    set_args({render, 4u, 0u});
    auto release_buffer = make_marshaler();
    ASSERT_EQ(mgr_.audio_render_client_release_buffer(release_buffer), com::S_OK);
    ASSERT_GE(renderer.last_push->submitted.size(), 2u);
    EXPECT_EQ(renderer.last_push->submitted[0], 0x34u);
    EXPECT_EQ(renderer.last_push->submitted[1], 0x12u);

    set_args({static_cast<u32>(client)});
    auto start = make_marshaler();
    EXPECT_EQ(mgr_.audio_client_start(start), com::S_OK);
}

TEST_F(ShellLinkEncodingTest, AudioActivationAsyncOwnsCompletionAndInterfacesBothAbis) {
    constexpr Guid audio_iid{0x1CB9AD4Cu,0xDBFAu,0x4C32u,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
    TestAudioRenderer renderer;
    mgr_.set_audio_renderer(&renderer);
    write_guid(IID_ADDR, audio_iid);
    for (bool wide : {false, true}) {
      for (unsigned scenario : {0u, 1u, 2u, 3u}) {
        const u32 expected = scenario == 0 ? 0u : scenario == 1 ? 0x80070002u : scenario == 2 ? 0x80070490u : com::E_NOTIMPL;
        mgr_.set_audio_renderer(scenario == 2 ? nullptr : &renderer);
        if (wide) cpu_.init_long_mode(); else cpu_.init_protected_mode();
        auto args = [&](std::initializer_list<u64> values) {
            cpu_.set_sp(STACK_BASE + 0x400);
            unsigned i = 0;
            for (auto v : values) {
                if (!wide) (void)memory_.write_u32(cpu_.gpr.rsp+4+4*i, static_cast<u32>(v));
                else if (i == 0) cpu_.gpr.rcx=v;
                else if (i == 1) cpu_.gpr.rdx=v;
                else if (i == 2) cpu_.gpr.r8=v;
                else if (i == 3) cpu_.gpr.r9=v;
                else (void)memory_.write_u64(cpu_.gpr.rsp+40+8*(i-4),v);
                ++i;
            }
        };
        auto m = make_marshaler();
        const GuestAddr handler=DATA_BASE+0x800, vtable=DATA_BASE+0x900;
        const GuestAddr result_out=DATA_BASE+0xa00, interface_out=DATA_BASE+0xa10;
        const auto stride=wide?8:4;
        ASSERT_TRUE(m.write_wide_string(WIDE_DIR_ADDR,scenario == 1 ? "missing-device" : "{E6327CAD-DCEC-4949-AE8A-991E976A79D2}").is_ok());
        ASSERT_TRUE(m.write_ptr(handler,vtable).is_ok());
        ASSERT_TRUE(m.write_ptr(vtable+stride,0x100).is_ok());
        ASSERT_TRUE(m.write_ptr(vtable+2*stride,0x200).is_ok());
        ASSERT_TRUE(m.write_ptr(vtable+3*stride,0x300).is_ok());
        u32 refs=1, callbacks=0;
        GuestAddr pending=0, worker=0, returned_client=0;
        mgr_.set_audio_activation_thread([&](GuestAddr fn,GuestAddr op){worker=fn;pending=op;return true;});
        mgr_.set_guest_call([&](GuestAddr fn,const u64* av,usize count,bool is64)->u64 {
            EXPECT_EQ(is64,wide);
            if(fn==0x100){EXPECT_EQ(av[0],handler);return ++refs;}
            if(fn==0x200){EXPECT_EQ(av[0],handler);return --refs;}
            if(fn==0x300){
                EXPECT_EQ(count,2u);EXPECT_EQ(av[0],handler);EXPECT_EQ(av[1],pending);++callbacks;
                args({pending,result_out,interface_out});
                EXPECT_EQ(mgr_.audio_activation_get_result(m),com::S_OK);
                EXPECT_EQ(memory_.read_u32(result_out).value_or(1),expected);
                returned_client=m.read_ptr(interface_out).value_or(0);EXPECT_EQ(returned_client != 0,expected == 0);
                return 0;
            }
            auto resolved=registry_.resolve_handler(fn);
            EXPECT_TRUE(resolved.is_ok());
            args({av[0]});
            if(resolved.is_ok())(*resolved.value())(m);
            return cpu_.gpr.rax;
        });
        args({0,IID_ADDR,0,handler,OUT_ADDR});
        (void)m.write_ptr(OUT_ADDR,0x1234);
        EXPECT_EQ(mgr_.activate_audio_interface_async(m),com::E_INVALIDARG);
        EXPECT_EQ(m.read_ptr(OUT_ADDR).value_or(0),0x1234u);
        args({WIDE_DIR_ADDR,IID_ADDR,scenario == 3 ? DATA_BASE+0xb00 : 0,handler,OUT_ADDR});
        ASSERT_EQ(mgr_.activate_audio_interface_async(m),com::S_OK);
        EXPECT_EQ(refs,2u);EXPECT_EQ(callbacks,0u);
        EXPECT_EQ(m.read_ptr(OUT_ADDR).value_or(0),pending);
        args({pending,result_out,interface_out});
        (void)memory_.write_u32(result_out,0xdead);
        EXPECT_EQ(mgr_.audio_activation_get_result(m),0x8000000eu);
        EXPECT_EQ(memory_.read_u32(result_out).value_or(0),0xdeadu);
        // The worker retains ownership after the caller releases its reference.
        args({pending});EXPECT_EQ(mgr_.audio_activation_release(m),1u);EXPECT_EQ(refs,2u);
        auto dispatch=registry_.resolve_handler(worker);ASSERT_TRUE(dispatch.is_ok());
        args({pending});(*dispatch.value())(m);
        EXPECT_EQ(callbacks,1u);EXPECT_EQ(refs,1u);
        if (returned_client) { args({returned_client});EXPECT_EQ(mgr_.audio_client_release(m),0u); }
      }
    }
}
