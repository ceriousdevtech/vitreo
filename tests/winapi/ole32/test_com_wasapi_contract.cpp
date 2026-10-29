// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_wasapi_contract.cpp
/// @brief WASAPI shared-mode render contract tests (IAudioClient / IAudioRenderClient).

#include "com_test_common.hpp"
#include "com_test_wasapi_contract_fixture.hpp"

TEST_F(WasapiContractTest, MixFormatIsExtensibleFloatAndInitializeAcceptsIt) {
    ASSERT_EQ(call({0, PTR_OUT}, &ComCoreManager::audio_client_get_mix_format),
              com::S_OK);
    const u32 mix = read_u32(PTR_OUT);
    ASSERT_NE(mix, 0u);
    // WAVEFORMATEXTENSIBLE, 32-bit IEEE float stereo at the engine rate.
    EXPECT_EQ(memory_.read_u16(mix).value(), 0xFFFEu);
    EXPECT_EQ(memory_.read_u16(mix + 2).value(), 2u);
    EXPECT_EQ(read_u32(mix + 4), 48000u);
    EXPECT_EQ(read_u32(mix + 8), 384000u);
    EXPECT_EQ(memory_.read_u16(mix + 12).value(), 8u);
    EXPECT_EQ(memory_.read_u16(mix + 14).value(), 32u);
    EXPECT_EQ(memory_.read_u16(mix + 16).value(), 22u);
    EXPECT_EQ(memory_.read_u16(mix + 18).value(), 32u);
    EXPECT_EQ(read_u32(mix + 20), 0x3u);  // SPEAKER_FRONT_LEFT | FRONT_RIGHT
    Guid sub{};
    ASSERT_TRUE(memory_.read_bytes(mix + 24, reinterpret_cast<Byte*>(&sub),
                                   sizeof(sub)).is_ok());
    EXPECT_EQ(std::memcmp(&sub, &kSubtypeFloat, sizeof(sub)), 0);

    // Learn: "Initialize always accepts the stream format obtained from a
    // GetMixFormat call on the same device" — including event-driven with a
    // zero buffer duration and period.
    const u32 client = new_client();
    EXPECT_EQ(initialize(client, 0, kStreamFlagsEventCallback, 0, mix), com::S_OK);
    EXPECT_TRUE(renderer_.last_format.float_samples);
    EXPECT_EQ(renderer_.last_format.bits_per_sample, 32u);
    EXPECT_EQ(renderer_.last_format.channels, 2u);
    EXPECT_EQ(renderer_.last_format.channel_mask, 0x3u);
    EXPECT_EQ(initialize(client, 0, kStreamFlagsEventCallback, 0, mix),
              kAudclntAlreadyInitialized);
}

TEST_F(WasapiContractTest, InitializeFormatAndFlagInventory) {
    struct Case { const char* name; std::function<void()> fmt; u32 mode; u32 flags; u32 hr; };
    const Case cases[] = {
        {"float tag", [&] { write_format(3, 2, 48000, 32); }, 0, 0, com::S_OK},
        {"pcm16 tag", [&] { write_format(1, 2, 48000, 16); }, 0, 0, com::S_OK},
        {"pcm 24-in-32", [&] { write_extensible(2, 48000, 32, 24, 3, kSubtypePcm); },
         0, 0, com::S_OK},
        {"pcm 24 packed", [&] { write_extensible(2, 48000, 24, 24, 3, kSubtypePcm); },
         0, 0, com::S_OK},
        {"rate != mix", [&] { write_format(3, 2, 44100, 32); }, 0, 0,
         kAudclntUnsupportedFormat},
        {"rate != mix autoconvert", [&] { write_format(3, 2, 44100, 32); }, 0,
         kStreamFlagsAutoConvertPcm | kStreamFlagsSrcDefaultQuality, com::S_OK},
        {"5.1 != mix", [&] { write_extensible(6, 48000, 32, 32, 0x3F, kSubtypeFloat); },
         0, 0, kAudclntUnsupportedFormat},
        {"5.1 autoconvert", [&] { write_extensible(6, 48000, 32, 32, 0x3F, kSubtypeFloat); },
         0, kStreamFlagsAutoConvertPcm, com::S_OK},
        {"block align lies", [&] { write_format(1, 2, 48000, 16, 6); }, 0, 0,
         kEInvalidArg},
        {"extensible cbSize 0", [&] { write_format(0xFFFEu, 2, 48000, 32, 0, 0, 32, 3,
                                                   kSubtypeFloat); }, 0, 0, kEInvalidArg},
        {"valid > container", [&] { write_extensible(2, 48000, 16, 24, 3, kSubtypePcm); },
         0, 0, kEInvalidArg},
        {"ac3 subtype", [&] { write_extensible(2, 48000, 16, 16, 3, kSubtypeAc3); },
         0, 0, kAudclntUnsupportedFormat},
        {"mp3 tag", [&] { write_format(0x55, 2, 48000, 16); }, 0, 0,
         kAudclntUnsupportedFormat},
        {"float64", [&] { write_format(3, 2, 48000, 64); }, 0, 0,
         kAudclntUnsupportedFormat},
        {"exclusive", [&] { write_format(3, 2, 48000, 32); }, 1, 0,
         kAudclntExclusiveModeNotAllowed},
        {"bad share mode", [&] { write_format(3, 2, 48000, 32); }, 2, 0, kEInvalidArg},
        {"undefined flag", [&] { write_format(3, 2, 48000, 32); }, 0, 0x1u, kEInvalidArg},
        {"loopback exclusive", [&] { write_format(3, 2, 48000, 32); }, 1,
         kStreamFlagsLoopback, kEInvalidArg},
    };
    for (const auto& c : cases) {
        c.fmt();
        const u32 client = new_client();
        ASSERT_NE(client, 0u) << c.name;
        EXPECT_EQ(initialize(client, c.mode, c.flags, 400000u), c.hr) << c.name;
    }
    const u32 client = new_client();
    EXPECT_EQ(initialize(client, 0, 0, 0, 0), kEPointer);
    // The 5.1 stream reached the host voice with its speaker mask intact.
    write_extensible(6, 48000, 32, 32, 0x3F, kSubtypeFloat);
    EXPECT_EQ(initialize(new_client(), 0, kStreamFlagsAutoConvertPcm, 0), com::S_OK);
    EXPECT_EQ(renderer_.last_format.channels, 6u);
    EXPECT_EQ(renderer_.last_format.channel_mask, 0x3Fu);
}

TEST_F(WasapiContractTest, IsFormatSupportedSharedModeClosestMatch) {
    auto is_supported = [&](u32 mode, GuestAddr closest) {
        return call({0, mode, static_cast<u32>(FORMAT_ADDR), static_cast<u32>(closest)},
                    &ComCoreManager::audio_client_is_format_supported);
    };
    write_format(3, 2, 48000, 32);
    EXPECT_EQ(is_supported(0, 0), kEPointer);
    EXPECT_EQ(is_supported(7, PTR_OUT), kEInvalidArg);
    EXPECT_EQ(call({0, 0, 0, static_cast<u32>(PTR_OUT)},
                   &ComCoreManager::audio_client_is_format_supported), kEPointer);

    ASSERT_TRUE(memory_.write_u32(PTR_OUT, 0xCCCCCCCCu).is_ok());
    EXPECT_EQ(is_supported(0, PTR_OUT), com::S_OK);
    EXPECT_EQ(read_u32(PTR_OUT), 0u);

    write_format(1, 2, 48000, 16);  // integer at the mix rate/channels
    EXPECT_EQ(is_supported(0, PTR_OUT), com::S_OK);
    EXPECT_EQ(read_u32(PTR_OUT), 0u);

    write_format(3, 2, 44100, 32);  // similar: the mix format is offered
    EXPECT_EQ(is_supported(0, PTR_OUT), com::S_FALSE);
    const u32 closest = read_u32(PTR_OUT);
    ASSERT_NE(closest, 0u);
    EXPECT_EQ(memory_.read_u16(closest).value(), 0xFFFEu);
    EXPECT_EQ(read_u32(closest + 4), 48000u);

    write_format(0x55, 2, 48000, 16);  // compressed: nothing similar
    ASSERT_TRUE(memory_.write_u32(PTR_OUT, 0xCCCCCCCCu).is_ok());
    EXPECT_EQ(is_supported(0, PTR_OUT), kAudclntUnsupportedFormat);
    EXPECT_EQ(read_u32(PTR_OUT), 0u);

    write_format(3, 2, 48000, 32);
    EXPECT_EQ(is_supported(1, 0), kAudclntUnsupportedFormat);
}

TEST_F(WasapiContractTest, EventDrivenStreamIsSignalledEachEnginePeriod) {
    write_format(3, 2, 48000, 32);
    // Not initialised / not event driven.
    const u32 fresh = new_client();
    EXPECT_EQ(call({fresh, 0x1234u}, &ComCoreManager::audio_client_set_event_handle),
              kAudclntNotInitialized);
    const u32 polled = new_client();
    ASSERT_EQ(initialize(polled, 0, 0, 0), com::S_OK);
    EXPECT_EQ(call({polled, 0x1234u}, &ComCoreManager::audio_client_set_event_handle),
              kAudclntEventHandleNotExpected);

    const u32 client = new_client();
    ASSERT_EQ(initialize(client, 0, kStreamFlagsEventCallback, 0), com::S_OK);
    EXPECT_EQ(call({client}, &ComCoreManager::audio_client_start),
              kAudclntEventHandleNotSet);
    EXPECT_EQ(call({client, 0u}, &ComCoreManager::audio_client_set_event_handle),
              kEInvalidArg);
    EXPECT_EQ(call({client, 0x7FFF0u}, &ComCoreManager::audio_client_set_event_handle),
              kEInvalidArg);

    const Handle event = sync_.create_event(false, false, "");
    ASSERT_NE(event, NULL_HANDLE);
    ASSERT_EQ(call({client, event}, &ComCoreManager::audio_client_set_event_handle),
              com::S_OK);
    // Nothing signals a stream that has not started.
    EXPECT_EQ(sync_.wait_for_single_object(event, 50), WAIT_TIMEOUT);

    ASSERT_EQ(call({client}, &ComCoreManager::audio_client_start), com::S_OK);
    EXPECT_EQ(call({client}, &ComCoreManager::audio_client_start), kAudclntNotStopped);
    // Periodic, and with no data queued at all: the engine runs regardless.
    EXPECT_EQ(sync_.wait_for_single_object(event, 1000), WAIT_OBJECT_0);
    EXPECT_EQ(sync_.wait_for_single_object(event, 1000), WAIT_OBJECT_0);
    EXPECT_EQ(sync_.wait_for_single_object(event, 1000), WAIT_OBJECT_0);
    EXPECT_EQ(call({client}, &ComCoreManager::audio_client_reset), kAudclntNotStopped);

    EXPECT_EQ(call({client}, &ComCoreManager::audio_client_stop), com::S_OK);
    EXPECT_EQ(call({client}, &ComCoreManager::audio_client_stop), com::S_FALSE);
    (void)sync_.wait_for_single_object(event, 0);  // consume a pre-Stop tick
    EXPECT_EQ(sync_.wait_for_single_object(event, 60), WAIT_TIMEOUT);

    EXPECT_EQ(call({client}, &ComCoreManager::audio_client_reset), com::S_OK);
    EXPECT_EQ(call({client}, &ComCoreManager::audio_client_reset), com::S_FALSE);

    // The stream holds its own reference: the guest may close its handle.
    const Handle watcher = sync_.duplicate_handle(event, 0u, HandleAccess::All);
    ASSERT_NE(watcher, NULL_HANDLE);
    ASSERT_TRUE(sync_.close_handle(event));
    ASSERT_EQ(call({client}, &ComCoreManager::audio_client_start), com::S_OK);
    EXPECT_EQ(sync_.wait_for_single_object(watcher, 1000), WAIT_OBJECT_0);
    set_args({client});
    auto release = make_marshaler();
    EXPECT_EQ(mgr_.audio_client_release(release), 0u);
    (void)sync_.wait_for_single_object(watcher, 0);
    EXPECT_EQ(sync_.wait_for_single_object(watcher, 60), WAIT_TIMEOUT);
    (void)sync_.close_handle(watcher);
}

TEST_F(WasapiContractTest, RenderClientPacketRules) {
    write_format(1, 2, 48000, 16);
    const u32 client = new_client();
    ASSERT_EQ(initialize(client, 0, 0, 400000u), com::S_OK);
    ASSERT_EQ(call({client, SIZE_OUT}, &ComCoreManager::audio_client_get_buffer_size),
              com::S_OK);
    const u32 frames = read_u32(SIZE_OUT);
    ASSERT_EQ(frames, 1920u);  // 40 ms at 48 kHz
    write_guid(IID_ADDR, kIidAudioRenderClient);
    ASSERT_EQ(call({client, IID_ADDR, PTR_OUT}, &ComCoreManager::audio_client_get_service),
              com::S_OK);
    const u32 render = read_u32(PTR_OUT);
    ASSERT_NE(render, 0u);
    auto get = [&](u32 n, GuestAddr out = PTR_OUT) {
        return call({render, n, static_cast<u32>(out)},
                    &ComCoreManager::audio_render_client_get_buffer);
    };
    auto rel = [&](u32 n, u32 flags) {
        return call({render, n, flags},
                    &ComCoreManager::audio_render_client_release_buffer);
    };

    EXPECT_EQ(get(4, 0), kEPointer);
    // 0 frames: S_OK, *ppData untouched, no ReleaseBuffer owed.
    ASSERT_TRUE(memory_.write_u32(PTR_OUT, 0xAAAAAAAAu).is_ok());
    EXPECT_EQ(get(0), com::S_OK);
    EXPECT_EQ(read_u32(PTR_OUT), 0xAAAAAAAAu);
    EXPECT_EQ(rel(0, 0), com::S_OK);
    EXPECT_EQ(rel(4, 0), kAudclntOutOfOrder);
    EXPECT_EQ(get(frames + 1), kAudclntBufferTooLarge);

    ASSERT_EQ(get(100), com::S_OK);
    EXPECT_NE(read_u32(PTR_OUT), 0u);
    EXPECT_EQ(get(10), kAudclntOutOfOrder);
    EXPECT_EQ(call({client}, &ComCoreManager::audio_client_reset),
              kAudclntBufferOperationPending);
    EXPECT_EQ(rel(101, 0), kAudclntInvalidSize);
    EXPECT_EQ(rel(100, 0x1u), kEInvalidArg);  // DATA_DISCONTINUITY is capture-only
    ASSERT_NE(renderer_.last_push, nullptr);
    EXPECT_EQ(rel(50, kBufferFlagsSilent), com::S_OK);  // fewer than requested
    EXPECT_EQ(renderer_.last_push->submitted.size(), 50u * 4u);
    EXPECT_TRUE(std::all_of(renderer_.last_push->submitted.begin(),
                            renderer_.last_push->submitted.end(),
                            [](u8 b) { return b == 0; }));
    // 50 frames are now queued: the free space shrank by exactly that.
    EXPECT_EQ(get(frames - 50 + 1), kAudclntBufferTooLarge);
    EXPECT_EQ(get(frames - 50), com::S_OK);
    EXPECT_EQ(rel(0, 0), com::S_OK);  // hand the packet back unused
    EXPECT_EQ(get(1), com::S_OK);
    EXPECT_EQ(rel(1, 0), com::S_OK);
}
