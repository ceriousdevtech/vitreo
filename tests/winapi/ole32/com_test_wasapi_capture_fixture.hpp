// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file com_test_wasapi_capture_fixture.hpp
/// @brief WasapiCaptureTest fixture and the capture-endpoint constants
/// (test_com_wasapi_capture_devices.cpp, test_com_wasapi_capture_stream.cpp). Test-local.

#include "com_test_wasapi_services_fixture.hpp"

// ---------------------------------------------------------------------------
// Capture endpoints: IMMDeviceEnumerator eCapture, the capture IAudioClient
// and IAudioCaptureClient, ActivateAudioInterfaceAsync for capture, and the
// one device list behind DirectSound capture, waveIn and MMDevice. Expected
// values: Learn and tools/oracle_probe/audio_capture_contract_probe
// {32,64}.expected (quoted per assertion); a fake host capture backend.
// ---------------------------------------------------------------------------

namespace {
constexpr Guid kClsidMMDeviceEnumerator{0xBCDE0395u, 0xE52Fu, 0x467Cu,
                                        {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
constexpr Guid kIidMMDeviceEnumerator{0xA95664D2u, 0x9614u, 0x4F35u,
                                      {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
constexpr Guid kIidMMEndpoint{0x1BE09788u, 0x6894u, 0x4089u,
                              {0x85, 0x86, 0x9A, 0x2A, 0x6C, 0x26, 0x5A, 0xC5}};
constexpr Guid kPkeyDeviceNames{0xA45C254Eu, 0xDF1Cu, 0x4EFDu,
                                {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}};
constexpr Guid kPkeyAudioEngine{0xF19F064Du, 0x082Cu, 0x4E27u,
                                {0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C}};
constexpr u32 kAudclntSBufferEmpty = 0x08890001u;
constexpr u32 kAudclntEndpointCreateFailed = 0x8889000Fu;
constexpr u32 kEAccessDenied = 0x80070005u;
constexpr u32 kFileNotFound = 0x80070002u;
constexpr u32 kStreamFlagsLoopbackCapture = 0x00020000u;
constexpr u32 kSrcDefaultQuality = 0x08000000u;
}  // namespace

class WasapiCaptureTest : public WasapiServicesTest {
protected:
    static constexpr GuestAddr C_DATA = DATA_BASE + 0x2000;
    static constexpr GuestAddr C_FRAMES = DATA_BASE + 0x2010;
    static constexpr GuestAddr C_FLAGS = DATA_BASE + 0x2018;
    static constexpr GuestAddr C_POS = DATA_BASE + 0x2020;
    static constexpr GuestAddr C_QPC = DATA_BASE + 0x2028;
    static constexpr GuestAddr C_KEY = DATA_BASE + 0x2040;
    static constexpr GuestAddr C_VALUE = DATA_BASE + 0x2080;

    void SetUp() override {
        WasapiServicesTest::SetUp();
        capture_.add_two_devices();
        mgr_.set_audio_capture(&capture_);
    }

    GuestAddr enumerator() {
        write_guid(CLSID_ADDR, kClsidMMDeviceEnumerator);
        write_guid(IID_ADDR, kIidMMDeviceEnumerator);
        using Create = u32 (ComCoreManager::*)(Marshaler&);
        EXPECT_EQ(invoke(static_cast<Create>(&ComCoreManager::co_create_instance),
                         {CLSID_ADDR, 0, 1, IID_ADDR, OUT}),
                  com::S_OK);
        return static_cast<GuestAddr>(ptr(OUT));
    }
    std::vector<std::u16string> collection_ids(GuestAddr enumerator, u32 flow, u32 mask = 1) {
        EXPECT_EQ(invoke(&ComCoreManager::mmdevice_enumerator_enum_audio_endpoints,
                         {enumerator, flow, mask, OUT}), com::S_OK);
        const GuestAddr collection = static_cast<GuestAddr>(ptr(OUT));
        EXPECT_EQ(invoke(&ComCoreManager::mmdevice_collection_get_count, {collection, OUT2}),
                  com::S_OK);
        const u32 count = memory_.read_u32(OUT2).value();
        std::vector<std::u16string> ids;
        for (u32 i = 0; i < count; ++i) {
            EXPECT_EQ(invoke(&ComCoreManager::mmdevice_collection_item, {collection, i, OUT}),
                      com::S_OK);
            ids.push_back(device_id(static_cast<GuestAddr>(ptr(OUT))));
        }
        EXPECT_EQ(invoke(&ComCoreManager::mmdevice_collection_item, {collection, count, OUT}),
                  kEInvalidArg);
        return ids;
    }
    std::u16string device_id(GuestAddr device) {
        EXPECT_EQ(invoke(&ComCoreManager::mmdevice_get_id, {device, OUT2}), com::S_OK);
        return wstr(static_cast<GuestAddr>(ptr(OUT2)));
    }
    static std::u16string endpoint(std::string_view uid) {
        const std::string id = audio::capture_endpoint_id(uid);
        return std::u16string(id.begin(), id.end());
    }
    GuestAddr capture_client(std::string uid = "uid-builtin") {
        auto m = make_marshaler();
        const GuestAddr client =
            mgr_.create_audio_client(m, ComCoreManager::AudioEndpointRef{true, std::move(uid)});
        EXPECT_NE(client, 0u);
        return client;
    }
    u32 init(GuestAddr client, u32 mode, u32 flags, u64 duration, GuestAddr format = FORMAT_ADDR) {
        return x64_ ? invoke(&ComCoreManager::audio_client_initialize,
                             {client, mode, flags, duration, 0, format, 0})
                    : invoke(&ComCoreManager::audio_client_initialize,
                             {client, mode, flags, duration & 0xFFFFFFFFu, duration >> 32, 0, 0,
                              format, 0});
    }
    u32 buffer_size(GuestAddr client) {
        EXPECT_EQ(invoke(&ComCoreManager::audio_client_get_buffer_size, {client, OUT2}), com::S_OK);
        return memory_.read_u32(OUT2).value();
    }
    u32 get_buffer(GuestAddr cc) {
        return invoke(&ComCoreManager::audio_capture_client_get_buffer,
                      {cc, C_DATA, C_FRAMES, C_FLAGS, C_POS, C_QPC});
    }
    u32 next_packet(GuestAddr cc) {
        EXPECT_EQ(invoke(&ComCoreManager::audio_capture_client_get_next_packet_size, {cc, OUT2}),
                  com::S_OK);
        return memory_.read_u32(OUT2).value();
    }
    u32 padding(GuestAddr client) {
        EXPECT_EQ(invoke(&ComCoreManager::audio_client_get_current_padding, {client, OUT2}),
                  com::S_OK);
        return memory_.read_u32(OUT2).value();
    }
    u32 release(GuestAddr cc, u32 frames) {
        return invoke(&ComCoreManager::audio_capture_client_release_buffer, {cc, frames});
    }
    /// `frames` float stereo frames, every sample `value`.
    std::vector<u8> float_frames(u32 frames, float value) {
        std::vector<u8> bytes(static_cast<usize>(frames) * 8u);
        for (usize i = 0; i < bytes.size(); i += 4) std::memcpy(&bytes[i], &value, 4);
        return bytes;
    }

    test::FakeCapture capture_;
};
