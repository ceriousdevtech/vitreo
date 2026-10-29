#include <gtest/gtest.h>

#include <audio/contracts/audio_renderer.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/media/dsound/dsound.hpp>

#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <array>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

/// Records the ring it was handed so tests can assert that DirectSound gives
/// the backend the guest's own sample memory rather than a copy.
class FakeVoice final : public audio::IAudioVoice {
public:
    FakeVoice(const audio::PcmFormat& format, const u8* ring, u32 ring_bytes)
        : format(format), ring(ring), ring_bytes(ring_bytes) {}

    bool play(bool loop) override { playing = true; looping = loop; return true; }
    void pause() override { playing = false; }
    void stop() override { playing = false; }
    [[nodiscard]] bool is_playing() const override { return playing; }
    [[nodiscard]] u32 play_cursor() const override { return cursor; }
    [[nodiscard]] u32 write_cursor() const override {
        return ring_bytes ? (cursor + 64) % ring_bytes : 0;
    }
    void set_position(u32 offset) override { cursor = offset; }
    void set_volume(i32 v) override { volume = v; }
    void set_pan(i32 p) override { pan = p; }
    void set_frequency(u32 hz) override { frequency = hz; }

    audio::PcmFormat format;
    const u8* ring;
    u32 ring_bytes;
    bool playing{false};
    bool looping{false};
    u32 cursor{0};
    i32 volume{0};
    i32 pan{0};
    u32 frequency{0};
};

class FakeRenderer final : public audio::IAudioRenderer {
public:
    std::unique_ptr<audio::IAudioVoice> create_voice(const audio::PcmFormat& format,
                                                     const u8* ring,
                                                     u32 ring_bytes) override {
        auto voice = std::make_unique<FakeVoice>(format, ring, ring_bytes);
        voices.push_back(voice.get());
        return voice;
    }

    std::vector<FakeVoice*> voices;
};

constexpr GuestAddr SCRATCH = 0x10000;

/// Build a WAVEFORMATEX and DSBUFFERDESC in guest memory.
struct BufferDesc {
    GuestAddr desc;
    GuestAddr format;
};

} // namespace

class DirectSoundManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(SCRATCH, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        manager.set_memory(&memory);
        manager.set_thunk_registry(&registry);
        ASSERT_TRUE(registry.register_dll("dsound.dll",
                                          create_dsound_thunks(manager)).is_ok());
    }

    [[nodiscard]] GuestAddr create_device() {
        EXPECT_EQ(manager.direct_sound_create(0, SCRATCH, 0), dsound::DS_OK);
        return memory.read_u32(SCRATCH).value();
    }

    /// Lay out a secondary-buffer descriptor at SCRATCH+0x100.
    [[nodiscard]] BufferDesc make_desc(u32 flags, u32 bytes, u16 channels = 2,
                                       u32 rate = 22050, u16 bits = 16) {
        const GuestAddr format = SCRATCH + 0x100;
        const GuestAddr desc = SCRATCH + 0x140;
        const u16 block_align = static_cast<u16>(channels * (bits / 8));
        EXPECT_TRUE(memory.write_u16(format, 1).is_ok());  // WAVE_FORMAT_PCM
        EXPECT_TRUE(memory.write_u16(format + 2, channels).is_ok());
        EXPECT_TRUE(memory.write_u32(format + 4, rate).is_ok());
        EXPECT_TRUE(memory.write_u32(format + 8, rate * block_align).is_ok());
        EXPECT_TRUE(memory.write_u16(format + 12, block_align).is_ok());
        EXPECT_TRUE(memory.write_u16(format + 14, bits).is_ok());
        EXPECT_TRUE(memory.write_u16(format + 16, 0).is_ok());

        EXPECT_TRUE(memory.write_u32(desc, 20).is_ok());
        EXPECT_TRUE(memory.write_u32(desc + 4, flags).is_ok());
        EXPECT_TRUE(memory.write_u32(desc + 8, bytes).is_ok());
        EXPECT_TRUE(memory.write_u32(desc + 12, 0).is_ok());
        EXPECT_TRUE(memory.write_u32(desc + 16, format).is_ok());
        return BufferDesc{desc, format};
    }

    GuestMemory memory;
    ThunkRegistry registry{0x70000000};
    FakeRenderer renderer;
    DirectSoundManager manager{&renderer};
};

TEST_F(DirectSoundManagerTest, CreateBuildsVtableAndReturnsObject) {
    const GuestAddr object = create_device();
    ASSERT_NE(object, 0u);
    const GuestAddr vtable = memory.read_u32(object).value();
    ASSERT_NE(vtable, 0u);
    // Slot 6 is SetCooperativeLevel in the IDirectSound layout.
    auto expected = registry.get_thunk_address("dsound.dll",
                                               "Vitreo_DSound_SetCooperativeLevel");
    ASSERT_TRUE(expected.is_ok());
    EXPECT_EQ(memory.read_u32(vtable + 6 * 4).value(), expected.value());
}

TEST_F(DirectSoundManagerTest, AggregationAndUnknownDeviceFailHonestly) {
    EXPECT_EQ(manager.direct_sound_create(0, SCRATCH, 0x1234),
              dsound::CLASS_E_NOAGGREGATION);
    // A non-null, non-GUID_NULL device GUID has no driver behind it.
    ASSERT_TRUE(memory.write_u32(SCRATCH + 0x200, 0xDEADBEEF).is_ok());
    EXPECT_EQ(manager.direct_sound_create(SCRATCH + 0x200, SCRATCH, 0),
              dsound::DSERR_NODRIVER);
}

TEST_F(DirectSoundManagerTest, SecondaryBufferVoiceUsesGuestSampleMemory) {
    const GuestAddr object = create_device();
    const auto desc = make_desc(dsound::DSBCAPS_CTRLVOLUME, 4096);
    ASSERT_EQ(manager.create_sound_buffer(object, desc.desc, SCRATCH + 4, 0),
              dsound::DS_OK);
    const GuestAddr buffer = memory.read_u32(SCRATCH + 4).value();
    ASSERT_NE(buffer, 0u);
    ASSERT_EQ(renderer.voices.size(), 1u);

    // Lock must hand back a pointer into the buffer's own sample ring, and the
    // voice must be reading that exact memory.
    ASSERT_EQ(manager.buffer_lock(buffer, 0, 4096, SCRATCH + 8, SCRATCH + 12,
                                  SCRATCH + 16, SCRATCH + 20, 0), dsound::DS_OK);
    const GuestAddr samples = memory.read_u32(SCRATCH + 8).value();
    EXPECT_EQ(memory.read_u32(SCRATCH + 12).value(), 4096u);
    EXPECT_EQ(memory.read_u32(SCRATCH + 20).value(), 0u);  // no wrap
    EXPECT_EQ(renderer.voices[0]->ring,
              reinterpret_cast<const u8*>(memory.host_ptr(samples, 4096)));
    EXPECT_EQ(renderer.voices[0]->ring_bytes, 4096u);
    EXPECT_EQ(renderer.voices[0]->format.sample_rate, 22050u);
}

TEST_F(DirectSoundManagerTest, LockWrapsAroundRingEnd) {
    const GuestAddr object = create_device();
    const auto desc = make_desc(0, 4096);
    ASSERT_EQ(manager.create_sound_buffer(object, desc.desc, SCRATCH + 4, 0),
              dsound::DS_OK);
    const GuestAddr buffer = memory.read_u32(SCRATCH + 4).value();

    // Lock 1024 bytes starting 512 before the end: 512 + 512 across the seam.
    ASSERT_EQ(manager.buffer_lock(buffer, 4096 - 512, 1024, SCRATCH + 8,
                                  SCRATCH + 12, SCRATCH + 16, SCRATCH + 20, 0),
              dsound::DS_OK);
    const GuestAddr first = memory.read_u32(SCRATCH + 8).value();
    const GuestAddr second = memory.read_u32(SCRATCH + 16).value();
    EXPECT_EQ(memory.read_u32(SCRATCH + 12).value(), 512u);
    EXPECT_EQ(memory.read_u32(SCRATCH + 20).value(), 512u);
    EXPECT_EQ(second, first - (4096 - 512));  // wraps to the ring base
}

TEST_F(DirectSoundManagerTest, PlayStopAndStatusTrackLooping) {
    const GuestAddr object = create_device();
    const auto desc = make_desc(0, 2048);
    ASSERT_EQ(manager.create_sound_buffer(object, desc.desc, SCRATCH + 4, 0),
              dsound::DS_OK);
    const GuestAddr buffer = memory.read_u32(SCRATCH + 4).value();

    ASSERT_EQ(manager.buffer_play(buffer, 0, 0, dsound::DSBPLAY_LOOPING),
              dsound::DS_OK);
    ASSERT_EQ(manager.buffer_get_status(buffer, SCRATCH + 8), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH + 8).value(),
              dsound::DSBSTATUS_PLAYING | dsound::DSBSTATUS_LOOPING);
    EXPECT_TRUE(renderer.voices[0]->looping);

    ASSERT_EQ(manager.buffer_stop(buffer), dsound::DS_OK);
    ASSERT_EQ(manager.buffer_get_status(buffer, SCRATCH + 8), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH + 8).value(), 0u);

    // A non-zero dwReserved1 is invalid on Windows.
    EXPECT_EQ(manager.buffer_play(buffer, 1, 0, 0), dsound::E_INVALIDARG);
}

TEST_F(DirectSoundManagerTest, ControlsRequireTheirCapabilityFlag) {
    const GuestAddr object = create_device();
    const auto desc = make_desc(dsound::DSBCAPS_CTRLVOLUME, 2048);
    ASSERT_EQ(manager.create_sound_buffer(object, desc.desc, SCRATCH + 4, 0),
              dsound::DS_OK);
    const GuestAddr buffer = memory.read_u32(SCRATCH + 4).value();

    EXPECT_EQ(manager.buffer_set_volume(buffer, -2000), dsound::DS_OK);
    EXPECT_EQ(renderer.voices[0]->volume, -2000);
    EXPECT_EQ(manager.buffer_get_volume(buffer, SCRATCH + 8), dsound::DS_OK);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(SCRATCH + 8).value()), -2000);
    EXPECT_EQ(manager.buffer_set_volume(buffer, -20000), dsound::E_INVALIDARG);

    // Pan and frequency were never requested, so they are unavailable.
    EXPECT_EQ(manager.buffer_set_pan(buffer, 0), dsound::DSERR_CONTROLUNAVAIL);
    EXPECT_EQ(manager.buffer_set_frequency(buffer, 11025),
              dsound::DSERR_CONTROLUNAVAIL);
}

TEST_F(DirectSoundManagerTest, DuplicateSharesSamplesButNotPlaybackState) {
    const GuestAddr object = create_device();
    const auto desc = make_desc(dsound::DSBCAPS_CTRLVOLUME, 2048);
    ASSERT_EQ(manager.create_sound_buffer(object, desc.desc, SCRATCH + 4, 0),
              dsound::DS_OK);
    const GuestAddr original = memory.read_u32(SCRATCH + 4).value();
    ASSERT_EQ(manager.buffer_play(original, 0, 0, 0), dsound::DS_OK);

    ASSERT_EQ(manager.duplicate_sound_buffer(object, original, SCRATCH + 8),
              dsound::DS_OK);
    const GuestAddr copy = memory.read_u32(SCRATCH + 8).value();
    ASSERT_NE(copy, original);
    ASSERT_EQ(renderer.voices.size(), 2u);

    // Same sample memory, independent playback state.
    EXPECT_EQ(renderer.voices[1]->ring, renderer.voices[0]->ring);
    EXPECT_TRUE(renderer.voices[0]->is_playing());
    EXPECT_FALSE(renderer.voices[1]->is_playing());

    // Releasing the duplicate must not free the shared ring: the original can
    // still be locked afterwards.
    EXPECT_EQ(manager.buffer_release(copy), 0u);
    EXPECT_EQ(manager.buffer_lock(original, 0, 2048, SCRATCH + 12, SCRATCH + 16,
                                  0, 0, 0), dsound::DS_OK);
}

TEST_F(DirectSoundManagerTest, PrimaryBufferRejectsDirectWrites) {
    const GuestAddr object = create_device();
    // A primary descriptor carries no size and no format.
    ASSERT_TRUE(memory.write_u32(SCRATCH + 0x140, 20).is_ok());
    ASSERT_TRUE(memory.write_u32(SCRATCH + 0x144,
                                 dsound::DSBCAPS_PRIMARYBUFFER).is_ok());
    ASSERT_TRUE(memory.write_u32(SCRATCH + 0x148, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SCRATCH + 0x14C, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SCRATCH + 0x150, 0).is_ok());
    ASSERT_EQ(manager.create_sound_buffer(object, SCRATCH + 0x140, SCRATCH + 4, 0),
              dsound::DS_OK);
    const GuestAddr primary = memory.read_u32(SCRATCH + 4).value();

    // No voice is created for the primary mix destination.
    EXPECT_TRUE(renderer.voices.empty());
    EXPECT_EQ(manager.buffer_lock(primary, 0, 16, SCRATCH + 8, SCRATCH + 12,
                                  0, 0, 0), dsound::DSERR_PRIOLEVELNEEDED);
    // Position queries are meaningless on the primary buffer.
    EXPECT_EQ(manager.buffer_get_current_position(primary, SCRATCH + 8, SCRATCH + 12),
              dsound::DSERR_INVALIDCALL);
}

TEST_F(DirectSoundManagerTest, PrimaryBufferAcceptsDx7DescriptorSize) {
    const GuestAddr object = create_device();
    // DX7+ DSBUFFERDESC is 36 bytes (includes guid3DAlgorithm). RCT3 creates
    // its primary buffer this way with DSBCAPS_PRIMARYBUFFER|DSBCAPS_CTRL3D.
    ASSERT_TRUE(memory.write_u32(SCRATCH + 0x140, 36).is_ok());
    ASSERT_TRUE(memory.write_u32(
                    SCRATCH + 0x144,
                    dsound::DSBCAPS_PRIMARYBUFFER | dsound::DSBCAPS_CTRL3D)
                    .is_ok());
    ASSERT_TRUE(memory.write_u32(SCRATCH + 0x148, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SCRATCH + 0x14C, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SCRATCH + 0x150, 0).is_ok());
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(memory.write_u32(SCRATCH + 0x154 + i * 4, 0).is_ok());
    }
    ASSERT_EQ(manager.create_sound_buffer(object, SCRATCH + 0x140, SCRATCH + 4, 0),
              dsound::DS_OK);
    const GuestAddr primary = memory.read_u32(SCRATCH + 4).value();
    EXPECT_NE(primary, 0u);

    // A bogus dwSize must still fail honestly.
    ASSERT_TRUE(memory.write_u32(SCRATCH + 0x140, 24).is_ok());
    EXPECT_EQ(manager.create_sound_buffer(object, SCRATCH + 0x140, SCRATCH + 8, 0),
              dsound::E_INVALIDARG);

    // IID_IDirectSound3DListener — RCT3 QIs this immediately after creating
    // the CTRL3D primary and later calls SetAllParameters on it.
    constexpr std::array<u8, 16> iid_listener{
        0x84, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11,
        0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
    ASSERT_TRUE(memory.write_bytes(SCRATCH + 0x200, iid_listener.data(),
                                   iid_listener.size()).is_ok());
    ASSERT_EQ(manager.buffer_query_interface(primary, SCRATCH + 0x200,
                                             SCRATCH + 0x220),
              dsound::DS_OK);
    const GuestAddr listener = memory.read_u32(SCRATCH + 0x220).value();
    ASSERT_NE(listener, 0u);
    ASSERT_NE(listener, primary);

    ASSERT_TRUE(memory.write_u32(SCRATCH + 0x300, 64).is_ok());
    ASSERT_EQ(manager.listener_set_all_parameters(listener, SCRATCH + 0x300, 0),
              dsound::DS_OK);
}

TEST_F(DirectSoundManagerTest, BadFormatsAreRejected) {
    const GuestAddr object = create_device();
    // 24-bit PCM is not supported by the renderer contract.
    const auto desc = make_desc(0, 2048, 2, 22050, 24);
    EXPECT_EQ(manager.create_sound_buffer(object, desc.desc, SCRATCH + 4, 0),
              dsound::DSERR_BADFORMAT);

    // A zero-length secondary buffer is invalid.
    const auto empty = make_desc(0, 0);
    EXPECT_EQ(manager.create_sound_buffer(object, empty.desc, SCRATCH + 4, 0),
              dsound::E_INVALIDARG);
}

TEST_F(DirectSoundManagerTest, GetFormatReportsTheBufferFormat) {
    const GuestAddr object = create_device();
    const auto desc = make_desc(0, 2048, 1, 11025, 8);
    ASSERT_EQ(manager.create_sound_buffer(object, desc.desc, SCRATCH + 4, 0),
              dsound::DS_OK);
    const GuestAddr buffer = memory.read_u32(SCRATCH + 4).value();

    const GuestAddr out = SCRATCH + 0x200;
    ASSERT_EQ(manager.buffer_get_format(buffer, out, 18, SCRATCH + 8), dsound::DS_OK);
    EXPECT_EQ(memory.read_u16(out).value(), 1u);          // WAVE_FORMAT_PCM
    EXPECT_EQ(memory.read_u16(out + 2).value(), 1u);      // mono
    EXPECT_EQ(memory.read_u32(out + 4).value(), 11025u);
    EXPECT_EQ(memory.read_u16(out + 14).value(), 8u);
    EXPECT_EQ(memory.read_u32(SCRATCH + 8).value(), 18u);

    // A null format pointer is the size query.
    ASSERT_EQ(manager.buffer_get_format(buffer, 0, 0, SCRATCH + 8), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH + 8).value(), 18u);
}

TEST_F(DirectSoundManagerTest, EnumerationSupportsUnicodeContextAndEarlyStop) {
    std::vector<u64> ids;
    manager.set_guest_call([&](GuestAddr fn, const u64* args, usize count, bool x64) {
        EXPECT_EQ(fn, 0x140001000u);
        EXPECT_EQ(count, 4u);
        EXPECT_TRUE(x64);
        EXPECT_EQ(args[3], 0x1234567887654321ull);
        EXPECT_EQ(memory.read_u16(args[1]).value(), 'V');
        ids.push_back(args[0]);
        return u64{1};
    });
    EXPECT_EQ(manager.enumerate(0x140001000, 0x1234567887654321, true, false, true), 0u);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 0u);
    EXPECT_NE(ids[1], 0u);
    EXPECT_EQ(manager.get_device_id(ids[1], SCRATCH), 0u);
    std::array<Byte,16> original{}, copied{};
    ASSERT_TRUE(memory.read_bytes(ids[1], original.data(), 16).is_ok());
    ASSERT_TRUE(memory.read_bytes(SCRATCH, copied.data(), 16).is_ok());
    EXPECT_EQ(original, copied);
    EXPECT_NE(create_device(), 0u);
    EXPECT_EQ(manager.direct_sound_create(ids[1], SCRATCH, 0), 0u);
    u32 calls = 0;
    manager.set_guest_call([&](GuestAddr, const u64*, usize, bool) { ++calls; return u64{0}; });
    EXPECT_EQ(manager.enumerate(1, 0, false, false, false), 0u);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(manager.enumerate(1, 0, true, true, true), 0u);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(manager.enumerate(0, 0, false, false, false), dsound::E_INVALIDARG);
}

TEST_F(DirectSoundManagerTest, CaptureCreateWithNoDeviceLeavesTheOutputUntouched) {
    // audio_capture_contract_probe: with no device behind the GUID,
    // DirectSoundCaptureCreate(8) is DSERR_NODRIVER and *ppDSC is NOT written
    // ("CaptureCreate8(unknown) hr=88780078 out=untouched"); the Mac with no
    // input device (here: no capture backend at all) is the same case.
    auto exports = create_dsound_thunks(manager);
    for (bool x64 : {false, true}) {
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(SCRATCH + 0x800);
        Marshaler m(memory);
        m.attach_cpu(cpu);
        bool seen = false;
        for (const auto& entry : exports.entries) {
            if (entry.name != "DirectSoundCaptureCreate8") continue;
            seen = true;
            EXPECT_EQ(entry.ordinal, 12u);
            ASSERT_TRUE(memory.write_u64(SCRATCH, ~u64{0}).is_ok());
            if (x64) { cpu.gpr.rcx=0; cpu.gpr.rdx=SCRATCH; cpu.gpr.r8=0; }
            else {
                ASSERT_TRUE(memory.write_u32(cpu.sp()+4, 0).is_ok());
                ASSERT_TRUE(memory.write_u32(cpu.sp()+8, SCRATCH).is_ok());
                ASSERT_TRUE(memory.write_u32(cpu.sp()+12, 0).is_ok());
            }
            entry.handler(m);
            EXPECT_EQ(cpu.gpr.rax, dsound::DSERR_NODRIVER);
            EXPECT_EQ(memory.read_u64(SCRATCH).value(), ~u64{0});
        }
        EXPECT_TRUE(seen);
    }
}

// =============================================================================
// DirectSound capture, IDirectSoundNotify and Full Duplex.
//
// Expected values: Learn (DirectSound reference) and the Windows runs
// tools/oracle_probe/audio_capture_contract_probe{32,64}.expected (quoted per
// assertion). The parameter selects the guest pointer width.
// =============================================================================
#include "../fake_audio_capture.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <tuple>

namespace {

constexpr GuestAddr CAP_BASE = 0x20000;
constexpr GuestAddr OUT = CAP_BASE + 0x000;
constexpr GuestAddr OUT2 = CAP_BASE + 0x010;
constexpr GuestAddr OUT3 = CAP_BASE + 0x020;
constexpr GuestAddr OUT4 = CAP_BASE + 0x030;
constexpr GuestAddr GUID_ADDR = CAP_BASE + 0x040;
constexpr GuestAddr IID_ADDR = CAP_BASE + 0x060;
constexpr GuestAddr DESC = CAP_BASE + 0x100;
constexpr GuestAddr FMT = CAP_BASE + 0x180;
constexpr GuestAddr FX = CAP_BASE + 0x200;
constexpr GuestAddr CAPS = CAP_BASE + 0x2C0;
constexpr GuestAddr NOTIFY = CAP_BASE + 0x300;
constexpr GuestAddr WFX_OUT = CAP_BASE + 0x400;
constexpr GuestAddr RDESC = CAP_BASE + 0x480;
constexpr u64 SENTINEL = 0x1234;

struct G { u32 d1; u16 d2; u16 d3; u8 d4[8]; };
// dsound.h / unknwn.h / ksmedia.h
constexpr G kIUnknown{0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr G kIClassFactory{1, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr G kIDirectSound{0x279AFA83u, 0x4981, 0x11CE, {0xA5, 0x21, 0, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
constexpr G kIDirectSound8{0xC50A7E93u, 0xF395, 0x4834, {0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66}};
constexpr G kIDirectSoundBuffer{0x279AFA85u, 0x4981, 0x11CE, {0xA5, 0x21, 0, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
constexpr G kIDirectSoundBuffer8{0x6825a449u, 0x7524, 0x4d82, {0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e}};
constexpr G kIDirectSoundCapture{0xb0210781u, 0x89cd, 0x11d0, {0xaf, 0x08, 0x00, 0xa0, 0xc9, 0x25, 0xcd, 0x16}};
constexpr G kIDirectSoundCaptureBuffer{0xb0210782u, 0x89cd, 0x11d0, {0xaf, 0x08, 0x00, 0xa0, 0xc9, 0x25, 0xcd, 0x16}};
constexpr G kIDirectSoundCaptureBuffer8{0x00990df4u, 0x0dbb, 0x4872, {0x83, 0x3e, 0x6d, 0x30, 0x3e, 0x80, 0xae, 0xb6}};
constexpr G kIDirectSoundNotify{0xb0210783u, 0x89cd, 0x11d0, {0xaf, 0x08, 0x00, 0xa0, 0xc9, 0x25, 0xcd, 0x16}};
constexpr G kIDirectSoundFullDuplex{0xedcb4c7au, 0xdaab, 0x4216, {0xa4, 0x2e, 0x6c, 0x50, 0x59, 0x6d, 0xdc, 0x1d}};
constexpr G kCLSIDDirectSoundCapture8{0xe4bcac13u, 0x7f99, 0x4908, {0x9a, 0x8e, 0x74, 0xe3, 0xbf, 0x24, 0xb6, 0xe1}};
constexpr G kCLSIDDirectSound8{0x3901cc3fu, 0x84b5, 0x4fa4, {0xba, 0x35, 0xaa, 0x81, 0x72, 0xb8, 0xa0, 0x9b}};
constexpr G kDefaultPlayback{0xdef00000u, 0x9c6d, 0x47ed, {0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03}};
constexpr G kDefaultCapture{0xdef00001u, 0x9c6d, 0x47ed, {0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03}};
constexpr G kDefaultVoiceCapture{0xdef00003u, 0x9c6d, 0x47ed, {0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03}};
constexpr G kUnknownDevice{0x12345678u, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
constexpr G kGuidNull{0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
constexpr G kAllObjects{0xaa114de5u, 0xc262, 0x4169, {0xa1, 0xc8, 0x23, 0xd6, 0x98, 0xcc, 0x73, 0xb5}};
constexpr G kClassAec{0xBF963D80u, 0xC559, 0x11D0, {0x8A, 0x2B, 0x00, 0xA0, 0xC9, 0x25, 0x5A, 0xC1}};
constexpr G kMsAec{0xcdebb919u, 0x379a, 0x488a, {0x87, 0x65, 0xf5, 0x3c, 0xfd, 0x36, 0xde, 0x40}};
constexpr G kSystemAec{0x1c22c56du, 0x9879, 0x4f5b, {0xa3, 0x89, 0x27, 0x99, 0x6d, 0xdc, 0x28, 0x10}};
constexpr G kClassNs{0xe07f903fu, 0x62fd, 0x4e60, {0x8c, 0xdd, 0xde, 0xa7, 0x23, 0x66, 0x65, 0xb5}};
constexpr G kMsNs{0x11c5c73bu, 0x66e9, 0x4ba1, {0xa0, 0xba, 0xe8, 0x14, 0xc6, 0xee, 0xd9, 0x2d}};
constexpr G kSubPcm{0x00000001u, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr G kSubFloat{0x00000003u, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr G kSubAdpcm{0x00000002u, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

}  // namespace

class DirectSoundCaptureTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        x64 = GetParam();
        ASSERT_TRUE(memory.map_region(CAP_BASE, 0x10000, MemoryProtection::ReadWrite).is_ok());
        manager.set_memory(&memory);
        manager.set_thunk_registry(&registry);
        manager.set_audio_capture(&capture);
        manager.note_guest_bitness(x64);
        manager.set_event_signaler([this](u64 handle) {
            std::lock_guard lock(signal_mutex);
            signals.push_back(handle);
            return true;
        });
        ASSERT_TRUE(registry.register_dll("dsound.dll", create_dsound_thunks(manager)).is_ok());
        capture.add_two_devices();
    }

    [[nodiscard]] u64 ptr(GuestAddr at) {
        return x64 ? memory.read_u64(at).value() : memory.read_u32(at).value();
    }
    void set_ptr(GuestAddr at, u64 value) {
        if (x64) ASSERT_TRUE(memory.write_u64(at, value).is_ok());
        else ASSERT_TRUE(memory.write_u32(at, static_cast<u32>(value)).is_ok());
    }
    void put_guid(GuestAddr at, const G& g) {
        ASSERT_TRUE(memory.write_bytes(at, reinterpret_cast<const Byte*>(&g), 16).is_ok());
    }
    void put_guid(GuestAddr at, const std::array<u8, 16>& g) {
        ASSERT_TRUE(memory.write_bytes(at, reinterpret_cast<const Byte*>(g.data()), 16).is_ok());
    }
    [[nodiscard]] std::array<u8, 16> get_guid(GuestAddr at) {
        std::array<u8, 16> g{};
        EXPECT_TRUE(memory.read_bytes(at, reinterpret_cast<Byte*>(g.data()), 16).is_ok());
        return g;
    }
    // dsound.h DSCBUFFERDESC / DSCBUFFERDESC1 per pointer width.
    [[nodiscard]] u32 dscbufferdesc() const { return x64 ? 40u : 28u; }
    [[nodiscard]] u32 dscbufferdesc1() const { return x64 ? 24u : 20u; }

    /// WAVEFORMATEX at FMT (cbSize 0), or WAVEFORMATEXTENSIBLE when `sub`.
    void put_format(u16 tag, u16 channels, u32 rate, u16 bits, const G* sub = nullptr,
                    u16 valid = 0, u16 cb = 22, int align_delta = 0) {
        const u16 align = static_cast<u16>(channels * (bits / 8) + align_delta);
        ASSERT_TRUE(memory.write_u16(FMT, sub ? 0xFFFE : tag).is_ok());
        ASSERT_TRUE(memory.write_u16(FMT + 2, channels).is_ok());
        ASSERT_TRUE(memory.write_u32(FMT + 4, rate).is_ok());
        ASSERT_TRUE(memory.write_u32(FMT + 8, rate * align).is_ok());
        ASSERT_TRUE(memory.write_u16(FMT + 12, align).is_ok());
        ASSERT_TRUE(memory.write_u16(FMT + 14, bits).is_ok());
        ASSERT_TRUE(memory.write_u16(FMT + 16, sub ? cb : 0).is_ok());
        if (sub) {
            ASSERT_TRUE(memory.write_u16(FMT + 18, valid ? valid : bits).is_ok());
            ASSERT_TRUE(memory.write_u32(FMT + 20, channels == 1 ? 4u : 3u).is_ok());
            put_guid(FMT + 24, *sub);
        }
    }
    void put_desc(u32 size, u32 flags, u32 bytes, u32 reserved = 0, GuestAddr format = FMT,
                  u32 fx_count = 0, GuestAddr fx = 0) {
        std::vector<Byte> zero(48, 0);
        ASSERT_TRUE(memory.write_bytes(DESC, zero.data(), zero.size()).is_ok());
        ASSERT_TRUE(memory.write_u32(DESC, size).is_ok());
        ASSERT_TRUE(memory.write_u32(DESC + 4, flags).is_ok());
        ASSERT_TRUE(memory.write_u32(DESC + 8, bytes).is_ok());
        ASSERT_TRUE(memory.write_u32(DESC + 12, reserved).is_ok());
        set_ptr(DESC + 16, format);
        const u32 p = x64 ? 8u : 4u;
        ASSERT_TRUE(memory.write_u32(DESC + 16 + p, fx_count).is_ok());
        set_ptr(DESC + 16 + 2 * p, fx);
    }
    void put_fx(GuestAddr at, const G& cls, const G& instance, u32 flags = 2, u32 size = 48,
                u32 reserved1 = 0) {
        ASSERT_TRUE(memory.write_u32(at, size).is_ok());
        ASSERT_TRUE(memory.write_u32(at + 4, flags).is_ok());
        put_guid(at + 8, cls);
        put_guid(at + 24, instance);
        ASSERT_TRUE(memory.write_u32(at + 40, reserved1).is_ok());
        ASSERT_TRUE(memory.write_u32(at + 44, 0).is_ok());
    }

    [[nodiscard]] GuestAddr make_capture(bool version8 = true) {
        set_ptr(OUT, SENTINEL);
        EXPECT_EQ(manager.capture_create(0, OUT, 0, version8), dsound::DS_OK);
        return static_cast<GuestAddr>(ptr(OUT));
    }
    /// A 16 kHz mono 16-bit buffer of `bytes` bytes on `capture`.
    [[nodiscard]] u32 try_buffer(GuestAddr capture_object, u32 bytes = 32000, u32 flags = 0) {
        put_format(1, 1, 16000, 16);
        put_desc(dscbufferdesc(), flags, bytes);
        set_ptr(OUT2, SENTINEL);
        return manager.capture_create_buffer(capture_object, DESC, OUT2, 0);
    }
    [[nodiscard]] GuestAddr make_buffer(GuestAddr capture_object, u32 bytes = 32000) {
        EXPECT_EQ(try_buffer(capture_object, bytes), dsound::DS_OK);
        return static_cast<GuestAddr>(ptr(OUT2));
    }
    [[nodiscard]] GuestAddr qi_cbuffer(GuestAddr buffer, const G& iid, u32 expect) {
        put_guid(IID_ADDR, iid);
        set_ptr(OUT3, SENTINEL);
        EXPECT_EQ(manager.cbuffer_query_interface(buffer, IID_ADDR, OUT3), expect);
        return static_cast<GuestAddr>(ptr(OUT3));
    }
    [[nodiscard]] u32 set_positions(GuestAddr notify, std::vector<std::pair<u32, u64>> list,
                                    bool null_array = false) {
        const u32 stride = x64 ? 16u : 8u;  // DSBPOSITIONNOTIFY {DWORD; HANDLE}
        for (usize i = 0; i < list.size(); ++i) {
            const GuestAddr e = NOTIFY + static_cast<GuestAddr>(i) * stride;
            EXPECT_TRUE(memory.write_u32(e, list[i].first).is_ok());
            if (x64) EXPECT_TRUE(memory.write_u64(e + 8, list[i].second).is_ok());
            else EXPECT_TRUE(memory.write_u32(e + 4, static_cast<u32>(list[i].second)).is_ok());
        }
        return manager.notify_set_positions(notify, static_cast<u32>(list.size()),
                                            null_array ? 0 : NOTIFY);
    }
    [[nodiscard]] std::vector<u64> take_signals() {
        std::lock_guard lock(signal_mutex);
        std::vector<u64> out;
        out.swap(signals);
        return out;
    }
    void put_render_desc(u32 flags, u32 bytes) {
        ASSERT_TRUE(memory.write_u32(RDESC, 36).is_ok());
        ASSERT_TRUE(memory.write_u32(RDESC + 4, flags).is_ok());
        ASSERT_TRUE(memory.write_u32(RDESC + 8, bytes).is_ok());
        ASSERT_TRUE(memory.write_u32(RDESC + 12, 0).is_ok());
        set_ptr(RDESC + 16, FMT);
    }

    bool x64{false};
    GuestMemory memory;
    ThunkRegistry registry{0x70000000};
    FakeRenderer renderer;
    test::FakeCapture capture;
    DirectSoundManager manager{&renderer};
    std::mutex signal_mutex;
    std::vector<u64> signals;
};

INSTANTIATE_TEST_SUITE_P(Bitness, DirectSoundCaptureTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? std::string("x64") : std::string("x86");
                         });

TEST_P(DirectSoundCaptureTest, ExportsMatchTheDllExportTable) {
    // dsound.dll export table (System32 and SysWOW64): every export at its
    // ordinal, and no other entry carries an ordinal.
    const auto exports = create_dsound_thunks(manager);
    for (const auto& expected : dsound::kDsoundExports) {
        const auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                                     [&](const ExportEntry& e) { return e.name == expected.name; });
        ASSERT_NE(it, exports.entries.end()) << expected.name;
        EXPECT_EQ(it->ordinal, expected.ordinal) << expected.name;
    }
    for (const auto& e : exports.entries) {
        if (e.ordinal == 0) continue;
        EXPECT_TRUE(std::any_of(std::begin(dsound::kDsoundExports), std::end(dsound::kDsoundExports),
                                [&](const auto& x) { return e.name == x.name; })) << e.name;
    }
    // dsound.h: DSERR_ALREADYINITIALIZED is MAKE_DSHRESULT(130).
    EXPECT_EQ(dsound::DSERR_ALREADYINITIALIZED, 0x88780082u);
}

TEST_P(DirectSoundCaptureTest, NoInputDeviceMeansNoDriverEverywhere) {
    // The Windows no-device answers (measured with no device behind a
    // DirectSound GUID): enumeration calls nothing, GetDeviceID and Create
    // are DSERR_NODRIVER, the output untouched.
    capture.list.clear();
    capture.default_uid.clear();
    int calls = 0;
    manager.set_guest_call([&](GuestAddr, const u64*, usize, bool) { ++calls; return u64{1}; });
    EXPECT_EQ(manager.enumerate(0x401000, 0, true, true, x64), dsound::DS_OK);
    EXPECT_EQ(calls, 0);
    put_guid(GUID_ADDR, kDefaultCapture);
    EXPECT_EQ(manager.get_device_id(GUID_ADDR, OUT), dsound::DSERR_NODRIVER);
    set_ptr(OUT, SENTINEL);
    EXPECT_EQ(manager.capture_create(0, OUT, 0, true), dsound::DSERR_NODRIVER);
    EXPECT_EQ(ptr(OUT), SENTINEL);
}

TEST_P(DirectSoundCaptureTest, EnumerationListsPrimaryThenDefaultFirst) {
    struct Call { u64 guid; std::u16string desc; std::u16string module; std::string desc_a; u64 ctx; };
    std::vector<Call> calls;
    bool wide = true;
    usize stop_after = 0;
    manager.set_guest_call([&](GuestAddr fn, const u64* args, usize count, bool is64) {
        EXPECT_EQ(fn, 0x401000u);
        EXPECT_EQ(count, 4u);
        EXPECT_EQ(is64, x64);
        Call c{args[0], {}, {}, {}, args[3]};
        auto wstr = [&](GuestAddr at) {
            std::u16string t;
            for (;; at += 2) {
                const u16 u = memory.read_u16(at).value();
                if (u == 0) break;
                t.push_back(static_cast<char16_t>(u));
            }
            return t;
        };
        if (wide) {
            c.desc = wstr(static_cast<GuestAddr>(args[1]));
            c.module = wstr(static_cast<GuestAddr>(args[2]));
        } else {
            for (GuestAddr at = static_cast<GuestAddr>(args[1]);; ++at) {
                const u8 b = memory.read_u8(at).value();
                if (b == 0) break;
                c.desc_a.push_back(static_cast<char>(b));
            }
        }
        calls.push_back(c);
        return u64{stop_after == 0 || calls.size() < stop_after};
    });
    ASSERT_EQ(manager.enumerate(0x401000, 0x5555, true, true, x64), dsound::DS_OK);
    // "ds enum[0] null-guid=1 desc=Primary Sound Capture Driver module-empty=1",
    // then "default-first-then-mm-order=1 desc-is-friendly-name=1
    // module-is-endpoint-id=1 guid-is-endpoint-id-guid=1".
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[0].guid, 0u);
    EXPECT_EQ(calls[0].desc, u"Primary Sound Capture Driver");
    EXPECT_EQ(calls[0].module, u"");
    EXPECT_EQ(calls[0].ctx, 0x5555u);
    EXPECT_EQ(calls[1].desc, u"MacBook Pro Microphone");
    const std::string id1 = audio::capture_endpoint_id("uid-builtin");
    EXPECT_EQ(calls[1].module, std::u16string(id1.begin(), id1.end()));
    EXPECT_EQ(calls[2].desc, u"USB Headset Microphone");
    EXPECT_EQ(id1.substr(0, 18), "{0.0.1.00000000}.{");
    const auto g1 = audio::capture_device_guid("uid-builtin");
    u32 d1 = 0;
    std::memcpy(&d1, g1.data(), 4);
    char text[16];
    std::snprintf(text, sizeof(text), "%08x", d1);
    EXPECT_EQ(id1.substr(18, 8), text);  // the endpoint id's GUID is the device GUID
    // ANSI: the same entries.
    calls.clear();
    wide = false;
    ASSERT_EQ(manager.enumerate(0x401000, 0, false, true, x64), dsound::DS_OK);
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[0].desc_a, "Primary Sound Capture Driver");
    EXPECT_EQ(calls[1].desc_a, "MacBook Pro Microphone");
    // A callback returning FALSE stops ("calls=1"); NULL callback E_INVALIDARG.
    calls.clear();
    wide = true;
    stop_after = 1;
    ASSERT_EQ(manager.enumerate(0x401000, 0, true, true, x64), dsound::DS_OK);
    EXPECT_EQ(calls.size(), 1u);
    EXPECT_EQ(manager.enumerate(0, 0, true, true, x64), dsound::E_INVALIDARG);
}

TEST_P(DirectSoundCaptureTest, GetDeviceIdResolvesTheCaptureDefaults) {
    const auto builtin = audio::capture_device_guid("uid-builtin");
    const auto usb = audio::capture_device_guid("uid-usb");
    put_guid(GUID_ADDR, kDefaultCapture);
    EXPECT_EQ(manager.get_device_id(GUID_ADDR, OUT), dsound::DS_OK);
    EXPECT_EQ(get_guid(OUT), builtin);
    put_guid(GUID_ADDR, kDefaultVoiceCapture);
    EXPECT_EQ(manager.get_device_id(GUID_ADDR, OUT), dsound::DS_OK);
    EXPECT_EQ(get_guid(OUT), builtin);
    put_guid(GUID_ADDR, usb);
    EXPECT_EQ(manager.get_device_id(GUID_ADDR, OUT), dsound::DS_OK);
    EXPECT_EQ(get_guid(OUT), usb);
    // "GetDeviceID(unknown guid) hr=88780078 out-untouched=1"; GUID_NULL too.
    std::array<u8, 16> poison{};
    poison.fill(0xcc);
    put_guid(OUT, poison);
    put_guid(GUID_ADDR, kUnknownDevice);
    EXPECT_EQ(manager.get_device_id(GUID_ADDR, OUT), dsound::DSERR_NODRIVER);
    EXPECT_EQ(get_guid(OUT), poison);
    put_guid(GUID_ADDR, kGuidNull);
    EXPECT_EQ(manager.get_device_id(GUID_ADDR, OUT), dsound::DSERR_NODRIVER);
    EXPECT_EQ(manager.get_device_id(0, OUT), dsound::E_INVALIDARG);
    EXPECT_EQ(manager.get_device_id(GUID_ADDR, 0), dsound::E_INVALIDARG);
}

TEST_P(DirectSoundCaptureTest, CaptureCreateOutcomes) {
    // "CaptureCreate8(NULL|DefaultCapture|DefaultVoiceCapture|device|GUID_NULL)
    // hr=0 out=set", "(DefaultPlayback|unknown) hr=88780078 out=untouched",
    // "(ppDSC NULL) hr=80070057", "(outer) hr=80040110 out=untouched".
    for (const G* g : {&kDefaultCapture, &kDefaultVoiceCapture, &kGuidNull}) {
        put_guid(GUID_ADDR, *g);
        set_ptr(OUT, SENTINEL);
        EXPECT_EQ(manager.capture_create(GUID_ADDR, OUT, 0, true), dsound::DS_OK);
        EXPECT_NE(ptr(OUT), SENTINEL);
        EXPECT_EQ(manager.capture_release(static_cast<GuestAddr>(ptr(OUT))), 0u);
    }
    put_guid(GUID_ADDR, audio::capture_device_guid("uid-usb"));
    EXPECT_EQ(manager.capture_create(GUID_ADDR, OUT, 0, true), dsound::DS_OK);
    for (const G* g : {&kDefaultPlayback, &kUnknownDevice}) {
        put_guid(GUID_ADDR, *g);
        set_ptr(OUT, SENTINEL);
        EXPECT_EQ(manager.capture_create(GUID_ADDR, OUT, 0, true), dsound::DSERR_NODRIVER);
        EXPECT_EQ(ptr(OUT), SENTINEL);
    }
    EXPECT_EQ(manager.capture_create(0, 0, 0, true), dsound::E_INVALIDARG);
    set_ptr(OUT, SENTINEL);
    EXPECT_EQ(manager.capture_create(0, OUT, 0x5000, true), dsound::CLASS_E_NOAGGREGATION);
    EXPECT_EQ(ptr(OUT), SENTINEL);
    // The vtable is pointer-width: slot 3 is CreateCaptureBuffer (dsound.h).
    const GuestAddr object = make_capture();
    const GuestAddr vtable = static_cast<GuestAddr>(ptr(object));
    const auto thunk = registry.get_thunk_address("dsound.dll",
                                                  "Vitreo_DSCapture_CreateCaptureBuffer");
    ASSERT_TRUE(thunk.is_ok());
    EXPECT_EQ(ptr(vtable + 3 * (x64 ? 8 : 4)), thunk.value());
}

TEST_P(DirectSoundCaptureTest, CaptureObjectInterfacesCapsAndInitialize) {
    const GuestAddr object = make_capture();
    // "QI(IUnknown) same=0", "QI(IDirectSoundCapture) same=1",
    // "QI(IDirectSound|IDirectSoundFullDuplex) hr=80004002 out=null".
    put_guid(IID_ADDR, kIUnknown);
    ASSERT_EQ(manager.capture_query_interface(object, IID_ADDR, OUT3), dsound::DS_OK);
    const GuestAddr unknown = static_cast<GuestAddr>(ptr(OUT3));
    EXPECT_NE(unknown, object);
    put_guid(IID_ADDR, kIDirectSoundCapture);
    ASSERT_EQ(manager.capture_query_interface(unknown, IID_ADDR, OUT3), dsound::DS_OK);
    EXPECT_EQ(ptr(OUT3), object);
    for (const G* g : {&kIDirectSound, &kIDirectSoundFullDuplex}) {
        put_guid(IID_ADDR, *g);
        set_ptr(OUT3, SENTINEL);
        EXPECT_EQ(manager.capture_query_interface(object, IID_ADDR, OUT3), dsound::E_NOINTERFACE);
        EXPECT_EQ(ptr(OUT3), 0u);
    }
    // One reference count behind both pointers: creation + 2 QIs.
    EXPECT_EQ(manager.capture_add_ref(object), 4u);
    EXPECT_EQ(manager.capture_release(unknown), 3u);
    // "GetCaps(NULL|dwSize=0|dwSize=big) hr=80070057",
    // "GetCaps hr=0 flags=1 formats=fffff channels-1-or-2=1".
    EXPECT_EQ(manager.capture_get_caps(object, 0), dsound::E_INVALIDARG);
    ASSERT_TRUE(memory.write_u32(CAPS, 0).is_ok());
    EXPECT_EQ(manager.capture_get_caps(object, CAPS), dsound::E_INVALIDARG);
    ASSERT_TRUE(memory.write_u32(CAPS, 20).is_ok());
    EXPECT_EQ(manager.capture_get_caps(object, CAPS), dsound::E_INVALIDARG);
    ASSERT_TRUE(memory.write_u32(CAPS, 16).is_ok());
    ASSERT_EQ(manager.capture_get_caps(object, CAPS), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(CAPS + 4).value(), dsound::DSCCAPS_MULTIPLECAPTURE);
    EXPECT_EQ(memory.read_u32(CAPS + 8).value(), 0xfffffu);
    EXPECT_EQ(memory.read_u32(CAPS + 12).value(), 2u);  // the default device's channels
    // "Initialize(NULL|DefaultCapture) on created hr=88780082".
    EXPECT_EQ(manager.capture_initialize(object, 0), dsound::DSERR_ALREADYINITIALIZED);
    put_guid(GUID_ADDR, kDefaultCapture);
    EXPECT_EQ(manager.capture_initialize(object, GUID_ADDR), dsound::DSERR_ALREADYINITIALIZED);
}

TEST_P(DirectSoundCaptureTest, ClassFactoryHandsOutUninitializedObjects) {
    put_guid(GUID_ADDR, kCLSIDDirectSoundCapture8);
    put_guid(IID_ADDR, kIClassFactory);
    ASSERT_EQ(manager.dll_get_class_object(GUID_ADDR, IID_ADDR, OUT), dsound::DS_OK);
    const GuestAddr factory = static_cast<GuestAddr>(ptr(OUT));
    ASSERT_NE(factory, 0u);
    put_guid(IID_ADDR, kIDirectSoundCapture);
    EXPECT_EQ(manager.factory_create_instance(factory, 0x99, IID_ADDR, OUT4),
              dsound::CLASS_E_NOAGGREGATION);
    ASSERT_EQ(manager.factory_create_instance(factory, 0, IID_ADDR, OUT4), dsound::DS_OK);
    const GuestAddr object = static_cast<GuestAddr>(ptr(OUT4));
    // "uninit GetCaps hr=887800aa", "uninit CreateCaptureBuffer hr=887800aa
    // out=untouched", "uninit Initialize(NULL) hr=0", "again hr=88780082".
    ASSERT_TRUE(memory.write_u32(CAPS, 16).is_ok());
    EXPECT_EQ(manager.capture_get_caps(object, CAPS), dsound::DSERR_UNINITIALIZED);
    EXPECT_EQ(try_buffer(object), dsound::DSERR_UNINITIALIZED);
    EXPECT_EQ(ptr(OUT2), SENTINEL);
    EXPECT_EQ(manager.capture_initialize(object, 0), dsound::DS_OK);
    EXPECT_EQ(manager.capture_initialize(object, 0), dsound::DSERR_ALREADYINITIALIZED);
    EXPECT_EQ(manager.capture_get_caps(object, CAPS), dsound::DS_OK);
    EXPECT_EQ(manager.capture_release(object), 0u);
    // "uninit Initialize(unknown guid) hr=88780078".
    ASSERT_EQ(manager.factory_create_instance(factory, 0, IID_ADDR, OUT4), dsound::DS_OK);
    put_guid(GUID_ADDR, kUnknownDevice);
    EXPECT_EQ(manager.capture_initialize(static_cast<GuestAddr>(ptr(OUT4)), GUID_ADDR),
              dsound::DSERR_NODRIVER);
    EXPECT_EQ(manager.factory_release(factory), 0u);
    // Playback classes are not served (no uninitialized playback object).
    put_guid(GUID_ADDR, kCLSIDDirectSound8);
    put_guid(IID_ADDR, kIClassFactory);
    set_ptr(OUT, SENTINEL);
    EXPECT_EQ(manager.dll_get_class_object(GUID_ADDR, IID_ADDR, OUT),
              dsound::CLASS_E_CLASSNOTAVAILABLE);
    EXPECT_EQ(ptr(OUT), 0u);
}

TEST_P(DirectSoundCaptureTest, CreateCaptureBufferValidatesLikeWindows) {
    const GuestAddr object = make_capture();
    // Argument checks leave *ppDSCBuffer untouched (measured "out=untouched").
    EXPECT_EQ(manager.capture_create_buffer(object, 0, 0, 0), dsound::E_INVALIDARG);
    put_format(1, 1, 16000, 16);
    put_desc(dscbufferdesc(), 0, 32000);
    EXPECT_EQ(manager.capture_create_buffer(object, DESC, 0, 0), dsound::E_INVALIDARG);
    set_ptr(OUT2, SENTINEL);
    EXPECT_EQ(manager.capture_create_buffer(object, 0, OUT2, 0), dsound::E_INVALIDARG);
    EXPECT_EQ(ptr(OUT2), SENTINEL);
    EXPECT_EQ(manager.capture_create_buffer(object, DESC, OUT2, object),
              dsound::CLASS_E_NOAGGREGATION);
    EXPECT_EQ(ptr(OUT2), SENTINEL);

    struct Case { const char* name; std::function<void()> setup; u32 hr; const char* out; };
    const u32 full = dscbufferdesc();
    const u32 ctrlfx = dsound::DSCBCAPS_CTRLFX;
    const Case cases[] = {
        {"dwSize=0", [&] { put_format(1, 1, 16000, 16); put_desc(0, 0, 32000); }, dsound::E_INVALIDARG, "untouched"},
        {"dwSize=DSCBUFFERDESC1", [&] { put_format(1, 1, 16000, 16); put_desc(dscbufferdesc1(), 0, 32000); }, dsound::DS_OK, "set"},
        {"dwSize=big", [&] { put_format(1, 1, 16000, 16); put_desc(full + 4, 0, 32000); }, dsound::E_INVALIDARG, "untouched"},
        {"dwReserved=5", [&] { put_format(1, 1, 16000, 16); put_desc(full, 0, 32000, 5); }, dsound::E_INVALIDARG, "untouched"},
        {"format NULL", [&] { put_desc(full, 0, 32000, 0, 0); }, dsound::E_INVALIDARG, "untouched"},
        {"bytes=0", [&] { put_format(1, 1, 16000, 16); put_desc(full, 0, 0); }, dsound::E_INVALIDARG, "untouched"},
        {"bytes=3 align=2", [&] { put_format(1, 1, 16000, 16); put_desc(full, 0, 3); }, dsound::E_INVALIDARG, "untouched"},
        {"bytes=2 < DSBSIZE_MIN", [&] { put_format(1, 1, 16000, 16); put_desc(full, 0, 2); }, dsound::E_INVALIDARG, "untouched"},
        {"pcm8 bytes=4", [&] { put_format(1, 1, 16000, 8); put_desc(full, 0, 4); }, dsound::DS_OK, "set"},
        {"pcm8 bytes=3", [&] { put_format(1, 1, 16000, 8); put_desc(full, 0, 3); }, dsound::E_INVALIDARG, "untouched"},
        {"DSBSIZE_MAX+1", [&] { put_format(1, 1, 16000, 8); put_desc(full, 0, 0x10000000u); }, dsound::E_INVALIDARG, "untouched"},
        {"flags=0x1", [&] { put_format(1, 1, 16000, 16); put_desc(full, 0x1, 32000); }, dsound::E_INVALIDARG, "untouched"},
        {"flags=0x10", [&] { put_format(1, 1, 16000, 16); put_desc(full, 0x10, 32000); }, dsound::E_INVALIDARG, "untouched"},
        {"WAVEMAPPED", [&] { put_format(1, 1, 16000, 16); put_desc(full, dsound::DSCBCAPS_WAVEMAPPED, 32000); }, dsound::DS_OK, "set"},
        {"pcm16 stereo 44k", [&] { put_format(1, 2, 44100, 16); put_desc(full, 0, 3200 * 4); }, dsound::DS_OK, "set"},
        {"pcm24 stereo", [&] { put_format(1, 2, 48000, 24); put_desc(full, 0, 6 * 1600); }, dsound::DS_OK, "set"},
        {"pcm32 stereo", [&] { put_format(1, 2, 48000, 32); put_desc(full, 0, 8 * 1600); }, dsound::DS_OK, "set"},
        {"float stereo", [&] { put_format(3, 2, 48000, 32); put_desc(full, 0, 8 * 1600); }, dsound::DS_OK, "set"},
        {"ext pcm16", [&] { put_format(0, 2, 48000, 16, &kSubPcm); put_desc(full, 0, 4 * 1600); }, dsound::DS_OK, "set"},
        {"ext float", [&] { put_format(0, 2, 48000, 32, &kSubFloat); put_desc(full, 0, 8 * 1600); }, dsound::DS_OK, "set"},
        {"192 kHz", [&] { put_format(1, 1, 192000, 16); put_desc(full, 0, 3200); }, dsound::DS_OK, "set"},
        {"1 kHz", [&] { put_format(1, 1, 1000, 16); put_desc(full, 0, 3200); }, dsound::DS_OK, "set"},
        {"12-bit", [&] { put_format(1, 1, 16000, 12); put_desc(full, 0, 3200); }, dsound::E_INVALIDARG, "untouched"},
        {"0 channels", [&] { put_format(1, 0, 16000, 16); put_desc(full, 0, 3200); }, dsound::E_INVALIDARG, "untouched"},
        {"rate 0", [&] { put_format(1, 1, 0, 16); put_desc(full, 0, 3200); }, dsound::E_INVALIDARG, "untouched"},
        {"bad nBlockAlign", [&] { put_format(1, 1, 16000, 16, nullptr, 0, 22, 1); put_desc(full, 0, 3 * 1600); }, dsound::E_INVALIDARG, "untouched"},
        {"adpcm tag", [&] { put_format(2, 1, 16000, 16); put_desc(full, 0, 3200); }, dsound::DSERR_BADFORMAT, "null"},
        {"pcm 3 channels", [&] { put_format(1, 3, 16000, 16); put_desc(full, 0, 6 * 1600); }, dsound::DSERR_NODRIVER, "null"},
        {"pcm 6 channels", [&] { put_format(1, 6, 48000, 16); put_desc(full, 0, 12 * 1600); }, dsound::DSERR_NODRIVER, "null"},
        // Windows mixes the stereo input up to an extensible 3-channel buffer
        // ("ext pcm16 3ch hr=0"); Vitreo makes only mono<->stereo exactly and
        // refuses the rest (DSERR_BADFORMAT, UNIMPLEMENTED, .docs/contracts/dsound.md).
        {"ext 3 channels", [&] { put_format(0, 3, 48000, 16, &kSubPcm); put_desc(full, 0, 6 * 1600); }, dsound::DSERR_BADFORMAT, "null"},
        {"ext cbSize=10", [&] { put_format(0, 2, 48000, 32, &kSubFloat, 0, 10); put_desc(full, 0, 8 * 1600); }, dsound::E_INVALIDARG, "untouched"},
        {"ext ADPCM subtype", [&] { put_format(0, 2, 48000, 32, &kSubAdpcm); put_desc(full, 0, 8 * 1600); }, dsound::DSERR_BADFORMAT, "null"},
        {"ext pcm24 valid=20", [&] { put_format(0, 2, 48000, 24, &kSubPcm, 20); put_desc(full, 0, 6 * 1600); }, dsound::DSERR_BADFORMAT, "null"},
        {"ext pcm24 valid=24", [&] { put_format(0, 2, 48000, 24, &kSubPcm, 24); put_desc(full, 0, 6 * 1600); }, dsound::DS_OK, "set"},
        {"ext valid>bits", [&] { put_format(0, 2, 48000, 16, &kSubPcm, 24); put_desc(full, 0, 4 * 1600); }, dsound::E_INVALIDARG, "untouched"},
        {"CTRLFX no effects", [&] { put_format(1, 1, 16000, 16); put_desc(full, ctrlfx, 32000); }, dsound::DS_OK, "set"},
        // Well-formed effect requests: Windows 11 accepts them (and exposes no
        // effect object); Vitreo performs no echo cancellation or noise
        // suppression, so it refuses them with dsound.h's "effects ... could
        // not be found on the system" (UNIMPLEMENTED, .docs/contracts/dsound.md).
        {"NS soft", [&] { put_format(1, 1, 16000, 16); put_fx(FX, kClassNs, kMsNs); put_desc(full, ctrlfx, 32000, 0, FMT, 1, FX); }, dsound::DSERR_FXUNAVAILABLE, "null"},
        {"NS hard", [&] { put_format(1, 1, 16000, 16); put_fx(FX, kClassNs, kMsNs, 1); put_desc(full, ctrlfx, 32000, 0, FMT, 1, FX); }, dsound::DSERR_FXUNAVAILABLE, "null"},
        {"AEC system", [&] { put_format(1, 1, 16000, 16); put_fx(FX, kClassAec, kSystemAec); put_desc(full, ctrlfx, 32000, 0, FMT, 1, FX); }, dsound::DSERR_FXUNAVAILABLE, "null"},
        {"class NS instance AEC", [&] { put_format(1, 1, 16000, 16); put_fx(FX, kClassNs, kMsAec); put_desc(full, ctrlfx, 32000, 0, FMT, 1, FX); }, dsound::DSERR_FXUNAVAILABLE, "null"},
        {"NS no location", [&] { put_format(1, 1, 16000, 16); put_fx(FX, kClassNs, kMsNs, 0); put_desc(full, ctrlfx, 32000, 0, FMT, 1, FX); }, dsound::E_INVALIDARG, "untouched"},
        {"NS both locations", [&] { put_format(1, 1, 16000, 16); put_fx(FX, kClassNs, kMsNs, 3); put_desc(full, ctrlfx, 32000, 0, FMT, 1, FX); }, dsound::E_INVALIDARG, "untouched"},
        {"unknown class", [&] { put_format(1, 1, 16000, 16); put_fx(FX, kGuidNull, kMsNs); put_desc(full, ctrlfx, 32000, 0, FMT, 1, FX); }, dsound::E_INVALIDARG, "untouched"},
        {"unknown instance", [&] { put_format(1, 1, 16000, 16); put_fx(FX, kClassNs, kGuidNull); put_desc(full, ctrlfx, 32000, 0, FMT, 1, FX); }, dsound::E_INVALIDARG, "untouched"},
        {"fx dwSize=0", [&] { put_format(1, 1, 16000, 16); put_fx(FX, kClassNs, kMsNs, 2, 0); put_desc(full, ctrlfx, 32000, 0, FMT, 1, FX); }, dsound::E_INVALIDARG, "untouched"},
        {"fx reserved1=1", [&] { put_format(1, 1, 16000, 16); put_fx(FX, kClassNs, kMsNs, 2, 48, 1); put_desc(full, ctrlfx, 32000, 0, FMT, 1, FX); }, dsound::E_INVALIDARG, "untouched"},
        {"NS without CTRLFX", [&] { put_format(1, 1, 16000, 16); put_fx(FX, kClassNs, kMsNs); put_desc(full, 0, 32000, 0, FMT, 1, FX); }, dsound::E_INVALIDARG, "untouched"},
        {"count=1 desc NULL", [&] { put_format(1, 1, 16000, 16); put_desc(full, ctrlfx, 32000, 0, FMT, 1, 0); }, dsound::E_INVALIDARG, "untouched"},
        {"count=0 desc set", [&] { put_format(1, 1, 16000, 16); put_fx(FX, kClassNs, kMsNs); put_desc(full, 0, 32000, 0, FMT, 0, FX); }, dsound::E_INVALIDARG, "untouched"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        c.setup();
        set_ptr(OUT2, SENTINEL);
        const u32 hr = manager.capture_create_buffer(object, DESC, OUT2, 0);
        EXPECT_EQ(hr, c.hr);
        const u64 out = ptr(OUT2);
        EXPECT_EQ(std::string(out == SENTINEL ? "untouched" : out == 0 ? "null" : "set"), c.out);
        if (hr == dsound::DS_OK) {
            // The host stream delivers the buffer's own format.
            ASSERT_NE(capture.last, nullptr);
            EXPECT_EQ(capture.last->format.block_align(), memory.read_u16(FMT + 12).value());
            EXPECT_EQ(manager.cbuffer_release(static_cast<GuestAddr>(out)), 0u);
        }
    }
    // A refused effect request leaves the descriptors as given.
    put_format(1, 1, 16000, 16);
    put_fx(FX, kClassNs, kMsNs, dsound::DSCFX_LOCHARDWARE);
    put_desc(full, ctrlfx, 32000, 0, FMT, 1, FX);
    set_ptr(OUT2, SENTINEL);
    ASSERT_EQ(manager.capture_create_buffer(object, DESC, OUT2, 0), dsound::DSERR_FXUNAVAILABLE);
    EXPECT_EQ(ptr(OUT2), 0u);
    EXPECT_EQ(memory.read_u32(FX + 4).value(), dsound::DSCFX_LOCHARDWARE);
    put_desc(full, ctrlfx, 32000);
    ASSERT_EQ(manager.capture_create_buffer(object, DESC, OUT2, 0), dsound::DS_OK);
    // One buffer per object: "CreateCaptureBuffer(second) hr=8878000a
    // out=null"; validation still comes first ("second, bytes=3" E_INVALIDARG).
    EXPECT_EQ(try_buffer(object), dsound::DSERR_ALLOCATED);
    EXPECT_EQ(ptr(OUT2), 0u);
    EXPECT_EQ(try_buffer(object, 3), dsound::E_INVALIDARG);
    // A second capture object gets its own buffer (DSCCAPS_MULTIPLECAPTURE).
    const GuestAddr other = make_capture();
    EXPECT_EQ(try_buffer(other), dsound::DS_OK);
    // CTRLFX needs a DirectSoundCaptureCreate8 object: DSERR_DS8_REQUIRED.
    const GuestAddr old = make_capture(false);
    put_desc(full, ctrlfx, 32000);
    EXPECT_EQ(manager.capture_create_buffer(old, DESC, OUT2, 0), dsound::DSERR_DS8_REQUIRED);
}

TEST_P(DirectSoundCaptureTest, CaptureBufferInterfacesAndQueries) {
    const GuestAddr object = make_capture();
    const GuestAddr buffer = make_buffer(object);
    // GetCaps: "flags=0 bytes=32000 reserved=0"; NULL / dwSize 0 E_INVALIDARG.
    EXPECT_EQ(manager.cbuffer_get_caps(buffer, 0), dsound::E_INVALIDARG);
    ASSERT_TRUE(memory.write_u32(CAPS, 0).is_ok());
    EXPECT_EQ(manager.cbuffer_get_caps(buffer, CAPS), dsound::E_INVALIDARG);
    ASSERT_TRUE(memory.write_u32(CAPS, 16).is_ok());
    ASSERT_EQ(manager.cbuffer_get_caps(buffer, CAPS), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(CAPS + 4).value(), 0u);
    EXPECT_EQ(memory.read_u32(CAPS + 8).value(), 32000u);
    EXPECT_EQ(memory.read_u32(CAPS + 12).value(), 0u);
    // GetFormat: "(NULL,0,NULL) 80070057", "(NULL,0,&wr) wr=18", "(buf,10) and
    // (buf,16) 80070057 wr untouched", "(buf,64) wr=18 ... byte18=cc".
    EXPECT_EQ(manager.cbuffer_get_format(buffer, 0, 0, 0), dsound::E_INVALIDARG);
    ASSERT_EQ(manager.cbuffer_get_format(buffer, 0, 0, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 18u);
    ASSERT_TRUE(memory.write_u32(OUT4, 77).is_ok());
    EXPECT_EQ(manager.cbuffer_get_format(buffer, WFX_OUT, 10, OUT4), dsound::E_INVALIDARG);
    EXPECT_EQ(manager.cbuffer_get_format(buffer, WFX_OUT, 16, OUT4), dsound::E_INVALIDARG);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 77u);
    std::vector<Byte> fill(64, 0xcc);
    ASSERT_TRUE(memory.write_bytes(WFX_OUT, fill.data(), fill.size()).is_ok());
    ASSERT_EQ(manager.cbuffer_get_format(buffer, WFX_OUT, 64, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 18u);
    EXPECT_EQ(memory.read_u16(WFX_OUT).value(), 1u);
    EXPECT_EQ(memory.read_u16(WFX_OUT + 2).value(), 1u);
    EXPECT_EQ(memory.read_u32(WFX_OUT + 4).value(), 16000u);
    EXPECT_EQ(memory.read_u16(WFX_OUT + 16).value(), 0u);
    EXPECT_EQ(memory.read_u8(WFX_OUT + 18).value(), 0xccu);
    EXPECT_EQ(manager.cbuffer_get_format(buffer, WFX_OUT, 64, 0), dsound::DS_OK);
    // A WAVEFORMATEXTENSIBLE buffer reports 40 bytes ("format-size=40").
    const GuestAddr other = make_capture();
    put_format(0, 2, 48000, 32, &kSubFloat);
    put_desc(dscbufferdesc(), 0, 8 * 1600);
    ASSERT_EQ(manager.capture_create_buffer(other, DESC, OUT4, 0), dsound::DS_OK);
    ASSERT_EQ(manager.cbuffer_get_format(static_cast<GuestAddr>(ptr(OUT4)), 0, 0, OUT4),
              dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 40u);
    // GetStatus(NULL), GetCurrentPosition(NULL, NULL): E_INVALIDARG; either
    // cursor alone is fine.
    EXPECT_EQ(manager.cbuffer_get_status(buffer, 0), dsound::E_INVALIDARG);
    EXPECT_EQ(manager.cbuffer_get_current_position(buffer, 0, 0), dsound::E_INVALIDARG);
    EXPECT_EQ(manager.cbuffer_get_current_position(buffer, OUT4, 0), dsound::DS_OK);
    EXPECT_EQ(manager.cbuffer_get_current_position(buffer, 0, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 0u);
    EXPECT_EQ(manager.cbuffer_initialize(buffer, object, DESC), dsound::DSERR_ALREADYINITIALIZED);
    // QI: "Buffer8 same=1", "IUnknown same=0", "IDirectSoundBuffer 80004002
    // out=null", "Notify same-as-buffer=0", "notify QI back-is-buffer=1".
    EXPECT_EQ(qi_cbuffer(buffer, kIDirectSoundCaptureBuffer8, dsound::DS_OK), buffer);
    EXPECT_EQ(qi_cbuffer(buffer, kIDirectSoundCaptureBuffer, dsound::DS_OK), buffer);
    const GuestAddr unknown = qi_cbuffer(buffer, kIUnknown, dsound::DS_OK);
    EXPECT_NE(unknown, buffer);
    EXPECT_EQ(qi_cbuffer(buffer, kIDirectSoundBuffer, dsound::E_NOINTERFACE), 0u);
    const GuestAddr notify = qi_cbuffer(buffer, kIDirectSoundNotify, dsound::DS_OK);
    EXPECT_NE(notify, buffer);
    put_guid(IID_ADDR, kIDirectSoundCaptureBuffer);
    ASSERT_EQ(manager.notify_query_interface(notify, IID_ADDR, OUT3), dsound::DS_OK);
    EXPECT_EQ(ptr(OUT3), buffer);
    // GetObjectInPath: no effects "8878001e out=untouched"; with CTRLFX
    // "88781161"; GetFXStatus E_INVALIDARG either way, status untouched.
    set_ptr(OUT4, SENTINEL);
    put_guid(GUID_ADDR, kAllObjects);
    put_guid(IID_ADDR, kIUnknown);
    EXPECT_EQ(manager.cbuffer_get_object_in_path(buffer, GUID_ADDR, 0, IID_ADDR, OUT4),
              dsound::DSERR_CONTROLUNAVAIL);
    EXPECT_EQ(ptr(OUT4), SENTINEL);
    EXPECT_EQ(manager.cbuffer_get_fx_status(buffer, 0, 0), dsound::E_INVALIDARG);
    ASSERT_TRUE(memory.write_u32(OUT4, 0x4d).is_ok());
    EXPECT_EQ(manager.cbuffer_get_fx_status(buffer, 1, OUT4), dsound::E_INVALIDARG);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 0x4du);
    // A DSCBCAPS_CTRLFX buffer (no effects): "GetObjectInPath hr=88781161",
    // "GetFXStatus(2) hr=80070057 status-untouched=1".
    const GuestAddr fxobj = make_capture();
    put_format(1, 1, 16000, 16);
    put_fx(FX, kClassAec, kMsAec);
    put_fx(FX + 48, kClassNs, kMsNs);
    put_desc(dscbufferdesc(), dsound::DSCBCAPS_CTRLFX, 32000, 0, FMT, 2, FX);
    EXPECT_EQ(manager.capture_create_buffer(fxobj, DESC, OUT4, 0), dsound::DSERR_FXUNAVAILABLE);
    put_desc(dscbufferdesc(), dsound::DSCBCAPS_CTRLFX, 32000);
    ASSERT_EQ(manager.capture_create_buffer(fxobj, DESC, OUT4, 0), dsound::DS_OK);
    const GuestAddr fxbuf = static_cast<GuestAddr>(ptr(OUT4));
    ASSERT_TRUE(memory.write_u32(CAPS, 16).is_ok());
    ASSERT_EQ(manager.cbuffer_get_caps(fxbuf, CAPS), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(CAPS + 4).value(), dsound::DSCBCAPS_CTRLFX);
    EXPECT_EQ(manager.cbuffer_get_object_in_path(fxbuf, GUID_ADDR, 0, IID_ADDR, OUT4),
              dsound::DSERR_OBJECTNOTFOUND);
    EXPECT_EQ(manager.cbuffer_get_fx_status(fxbuf, 2, OUT3), dsound::E_INVALIDARG);
    // A DirectSoundCaptureCreate (non-8) buffer has no IDirectSoundCaptureBuffer8.
    const GuestAddr v1 = make_capture(false);
    const GuestAddr v1buf = make_buffer(v1);
    EXPECT_EQ(qi_cbuffer(v1buf, kIDirectSoundCaptureBuffer8, dsound::E_NOINTERFACE), 0u);
    // Releasing the capture object destroys its buffer (measured: the buffer
    // holds no reference); the dead buffer refuses every call.
    test::FakeCaptureStream* v1stream = capture.last;
    EXPECT_EQ(manager.capture_release(v1), 0u);
    EXPECT_EQ(manager.cbuffer_get_status(v1buf, OUT4), dsound::E_INVALIDARG);
    EXPECT_EQ(manager.cbuffer_release(v1buf), 0u);
    (void)v1stream;
}

TEST_P(DirectSoundCaptureTest, NotificationPositionRules) {
    const GuestAddr object = make_capture();
    const GuestAddr buffer = make_buffer(object);
    const GuestAddr notify = qi_cbuffer(buffer, kIDirectSoundNotify, dsound::DS_OK);
    // "Set(0,NULL) 0", "Set(1,NULL) 80070057", "offset=size 80070057",
    // "unsorted 0", "duplicate 80070057", "NULL event 80070057", "bogus event
    // 0", "OFFSETSTOP first 0".
    EXPECT_EQ(manager.notify_set_positions(notify, 0, 0), dsound::DS_OK);
    EXPECT_EQ(set_positions(notify, {{100, 0x10}}, true), dsound::E_INVALIDARG);
    EXPECT_EQ(set_positions(notify, {{32000, 0x10}}), dsound::E_INVALIDARG);
    EXPECT_EQ(set_positions(notify, {{20000, 0x10}, {10000, 0x11}}), dsound::DS_OK);
    EXPECT_EQ(set_positions(notify, {{10000, 0x10}, {10000, 0x11}}), dsound::E_INVALIDARG);
    EXPECT_EQ(set_positions(notify, {{100, 0}}), dsound::E_INVALIDARG);
    EXPECT_EQ(set_positions(notify, {{100, 0x7ff0}}), dsound::DS_OK);
    EXPECT_EQ(set_positions(notify, {{dsound::DSBPN_OFFSETSTOP, 0x12}, {100, 0x10}}), dsound::DS_OK);
    // "Set while capturing 88780032", an empty set too.
    ASSERT_EQ(manager.cbuffer_start(buffer, dsound::DSCBSTART_LOOPING), dsound::DS_OK);
    EXPECT_EQ(set_positions(notify, {{100, 0x10}}), dsound::DSERR_INVALIDCALL);
    EXPECT_EQ(manager.notify_set_positions(notify, 0, 0), dsound::DSERR_INVALIDCALL);
    ASSERT_EQ(manager.cbuffer_stop(buffer), dsound::DS_OK);
}

TEST_P(DirectSoundCaptureTest, LockAndUnlockRules) {
    const GuestAddr object = make_capture();
    const GuestAddr buffer = make_buffer(object);
    ASSERT_EQ(manager.cbuffer_lock(buffer, 0, 10, OUT, OUT2, OUT3, OUT4, 0), dsound::DS_OK);
    const GuestAddr samples = static_cast<GuestAddr>(ptr(OUT));
    ASSERT_EQ(manager.cbuffer_unlock(buffer, samples, 10, 0, 0), dsound::DS_OK);
    auto prime = [&] {
        set_ptr(OUT, SENTINEL);
        set_ptr(OUT3, SENTINEL);
        ASSERT_TRUE(memory.write_u32(OUT2, 77).is_ok());
        ASSERT_TRUE(memory.write_u32(OUT4, 77).is_ok());
    };
    // "Lock(0 bytes|bytes>size|offset=size|flags=2) 80070057 p1=null l1=0
    // l2=0 p2=null".
    for (const auto& [offset, bytes, flags] :
         {std::tuple{0u, 0u, 0u}, std::tuple{0u, 32001u, 0u}, std::tuple{32000u, 10u, 0u},
          std::tuple{0u, 10u, 2u}}) {
        prime();
        EXPECT_EQ(manager.cbuffer_lock(buffer, offset, bytes, OUT, OUT2, OUT3, OUT4, flags),
                  dsound::E_INVALIDARG);
        EXPECT_EQ(ptr(OUT), 0u);
        EXPECT_EQ(memory.read_u32(OUT2).value(), 0u);
        EXPECT_EQ(ptr(OUT3), 0u);
        EXPECT_EQ(memory.read_u32(OUT4).value(), 0u);
    }
    // "ppv1 NULL: all untouched", "pdw1 NULL: p1=null, l2/p2 untouched".
    prime();
    EXPECT_EQ(manager.cbuffer_lock(buffer, 0, 10, 0, OUT2, OUT3, OUT4, 0), dsound::E_INVALIDARG);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 77u);
    EXPECT_EQ(ptr(OUT3), SENTINEL);
    prime();
    EXPECT_EQ(manager.cbuffer_lock(buffer, 0, 10, OUT, 0, OUT3, OUT4, 0), dsound::E_INVALIDARG);
    EXPECT_EQ(ptr(OUT), 0u);
    EXPECT_EQ(ptr(OUT3), SENTINEL);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 77u);
    // "Lock(ENTIRE offset=500) l1=31500 l2=500": ENTIREBUFFER replaces dwBytes.
    prime();
    ASSERT_EQ(manager.cbuffer_lock(buffer, 500, 0, OUT, OUT2, OUT3, OUT4,
                                   dsound::DSCBLOCK_ENTIREBUFFER), dsound::DS_OK);
    EXPECT_EQ(ptr(OUT), samples + 500);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 31500u);
    EXPECT_EQ(ptr(OUT3), samples);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 500u);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, samples + 500, 31500, samples, 500), dsound::DS_OK);
    // "100+32000 l1=31900 l2=100"; "offset=1" and "bytes=11" are fine.
    prime();
    ASSERT_EQ(manager.cbuffer_lock(buffer, 100, 32000, OUT, OUT2, OUT3, OUT4, 0), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 31900u);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 100u);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, samples + 100, 31900, samples, 100), dsound::DS_OK);
    ASSERT_EQ(manager.cbuffer_lock(buffer, 1, 11, OUT, OUT2, OUT3, OUT4, 0), dsound::DS_OK);
    EXPECT_EQ(ptr(OUT3), 0u);  // no wrap: second pointer NULL, size 0
    EXPECT_EQ(memory.read_u32(OUT4).value(), 0u);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, samples + 1, 11, 0, 0), dsound::DS_OK);
    // "pdw2 NULL: p2=null l2 untouched"; "ppv2 NULL: l2=0 p2 untouched".
    prime();
    ASSERT_EQ(manager.cbuffer_lock(buffer, 0, 10, OUT, OUT2, OUT3, 0, 0), dsound::DS_OK);
    EXPECT_EQ(ptr(OUT3), 0u);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 77u);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, samples, 10, 0, 0), dsound::DS_OK);
    prime();
    ASSERT_EQ(manager.cbuffer_lock(buffer, 0, 10, OUT, OUT2, 0, OUT4, 0), dsound::DS_OK);
    EXPECT_EQ(ptr(OUT3), SENTINEL);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 0u);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, samples, 10, 0, 0), dsound::DS_OK);
    // "Lock(wrap) l1=1000 l2=1000 p2-is-start=1"; "Lock while locked 80070057
    // q1=null m1=0"; "Unlock(wrong ptr) 80070057"; "Unlock(more bytes) 0";
    // then nothing is locked: "fewer bytes / again 80070057".
    ASSERT_EQ(manager.cbuffer_lock(buffer, 31000, 2000, OUT, OUT2, OUT3, OUT4, 0), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 1000u);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 1000u);
    EXPECT_EQ(ptr(OUT3), samples);
    set_ptr(CAPS, SENTINEL);
    ASSERT_TRUE(memory.write_u32(CAPS + 8, 77).is_ok());
    EXPECT_EQ(manager.cbuffer_lock(buffer, 0, 100, CAPS, CAPS + 8, 0, 0, 0), dsound::E_INVALIDARG);
    EXPECT_EQ(ptr(CAPS), 0u);
    EXPECT_EQ(memory.read_u32(CAPS + 8).value(), 0u);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, samples + 31004, 1000, samples, 1000), dsound::E_INVALIDARG);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, samples + 31000, 1010, samples, 1000), dsound::DS_OK);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, samples + 31000, 10, samples, 0), dsound::E_INVALIDARG);
    // "Lock(100) Unlock(50) relock 0": any matching Unlock releases the lock.
    ASSERT_EQ(manager.cbuffer_lock(buffer, 0, 100, OUT, OUT2, OUT3, OUT4, 0), dsound::DS_OK);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, samples, 50, 0, 0), dsound::DS_OK);
    ASSERT_EQ(manager.cbuffer_lock(buffer, 0, 10, OUT, OUT2, OUT3, OUT4, 0), dsound::DS_OK);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, samples, 10, 0, 0), dsound::DS_OK);
    // "Lock while capturing 0".
    ASSERT_EQ(manager.cbuffer_start(buffer, dsound::DSCBSTART_LOOPING), dsound::DS_OK);
    ASSERT_EQ(manager.cbuffer_lock(buffer, 0, 1000, OUT, OUT2, OUT3, OUT4, 0), dsound::DS_OK);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, samples, 1000, 0, 0), dsound::DS_OK);
    ASSERT_EQ(manager.cbuffer_stop(buffer), dsound::DS_OK);
    // "Lock(wrap, no second ptr) l1=1000 (stops at the end)", "Unlock 0", then
    // "Lock(after) 80070057": Windows 11 leaves that buffer unlockable.
    ASSERT_EQ(manager.cbuffer_lock(buffer, 31000, 2000, OUT, OUT2, 0, 0, 0), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 1000u);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, samples + 31000, 1000, 0, 0), dsound::DS_OK);
    EXPECT_EQ(manager.cbuffer_lock(buffer, 0, 10, OUT, OUT2, OUT3, OUT4, 0), dsound::E_INVALIDARG);
}

TEST_P(DirectSoundCaptureTest, CaptureFillsTheGuestRingAndSignals) {
    const GuestAddr object = make_capture();
    const GuestAddr buffer = make_buffer(object, 3200);  // 100 ms, 16 kHz mono 16-bit
    const GuestAddr notify = qi_cbuffer(buffer, kIDirectSoundNotify, dsound::DS_OK);
    ASSERT_EQ(set_positions(notify, {{1599, 0xA1}, {3199, 0xA2},
                                     {dsound::DSBPN_OFFSETSTOP, 0xA3}}), dsound::DS_OK);
    test::FakeCaptureStream* stream = capture.last;
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->format.sample_rate, 16000u);
    EXPECT_EQ(stream->device, "uid-builtin");
    // "Start(flags=2) 80070057", the status stays 0.
    EXPECT_EQ(manager.cbuffer_start(buffer, 2), dsound::E_INVALIDARG);
    ASSERT_EQ(manager.cbuffer_get_status(buffer, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 0u);
    // "Start(LOOPING) status=3".
    ASSERT_EQ(manager.cbuffer_start(buffer, dsound::DSCBSTART_LOOPING), dsound::DS_OK);
    EXPECT_EQ(stream->starts, 1);
    ASSERT_EQ(manager.cbuffer_get_status(buffer, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 3u);
    // Frames land in the guest ring; a position fires once the read cursor
    // passes it ("first notify index=0 read>=16000").
    stream->feed_frames(400, 0x11);  // 800 bytes
    EXPECT_TRUE(take_signals().empty());
    ASSERT_EQ(manager.cbuffer_get_current_position(buffer, OUT3, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT3).value(), 800u);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 800u);
    stream->feed_frames(400, 0x22);  // read cursor 1600 > 1599
    EXPECT_EQ(take_signals(), std::vector<u64>{0xA1});
    ASSERT_EQ(manager.cbuffer_lock(buffer, 0, 1600, OUT, OUT2, OUT3, OUT4, 0), dsound::DS_OK);
    const GuestAddr data = static_cast<GuestAddr>(ptr(OUT));
    EXPECT_EQ(memory.read_u8(data).value(), 0x11u);
    EXPECT_EQ(memory.read_u8(data + 799).value(), 0x11u);
    EXPECT_EQ(memory.read_u8(data + 800).value(), 0x22u);
    EXPECT_EQ(manager.cbuffer_unlock(buffer, data, 1600, 0, 0), dsound::DS_OK);
    // Wrap: 2000 more bytes cross 3199 and continue at 0.
    stream->feed_frames(1000, 0x33);
    EXPECT_EQ(take_signals(), std::vector<u64>{0xA2});
    ASSERT_EQ(manager.cbuffer_get_current_position(buffer, 0, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 400u);
    EXPECT_EQ(memory.read_u8(data).value(), 0x33u);
    // "Start while capturing 0", "Start(0) while looping status=3".
    EXPECT_EQ(manager.cbuffer_start(buffer, 0), dsound::DS_OK);
    ASSERT_EQ(manager.cbuffer_get_status(buffer, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 3u);
    // "Stop status=0 offsetstop-signaled=1", "position frozen", "Stop again
    // offsetstop-signaled=0".
    ASSERT_EQ(manager.cbuffer_stop(buffer), dsound::DS_OK);
    EXPECT_EQ(take_signals(), std::vector<u64>{0xA3});
    EXPECT_EQ(stream->stops, 1);
    ASSERT_EQ(manager.cbuffer_get_status(buffer, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 0u);
    stream->feed_frames(100, 0x44);  // a stopped stream delivers nothing
    ASSERT_EQ(manager.cbuffer_get_current_position(buffer, OUT3, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 400u);
    EXPECT_EQ(memory.read_u32(OUT3).value(), 400u);
    EXPECT_EQ(manager.cbuffer_stop(buffer), dsound::DS_OK);
    EXPECT_TRUE(take_signals().empty());
    // "restart read-continues=1".
    ASSERT_EQ(manager.cbuffer_start(buffer, dsound::DSCBSTART_LOOPING), dsound::DS_OK);
    stream->feed_frames(100, 0x55);
    ASSERT_EQ(manager.cbuffer_get_current_position(buffer, 0, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 600u);
    ASSERT_EQ(manager.cbuffer_stop(buffer), dsound::DS_OK);
    (void)take_signals();
    // Non-looping: "once end status=0 offsetstop-signaled=1 read=0 cap=0".
    ASSERT_EQ(manager.cbuffer_start(buffer, 0), dsound::DS_OK);
    ASSERT_EQ(manager.cbuffer_get_status(buffer, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 1u);
    stream->feed_frames(1600, 0x66);  // more than the 2600 bytes left
    EXPECT_EQ(take_signals(), (std::vector<u64>{0xA1, 0xA2, 0xA3}));
    ASSERT_EQ(manager.cbuffer_get_status(buffer, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 0u);
    ASSERT_EQ(manager.cbuffer_get_current_position(buffer, OUT3, OUT4), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(OUT3).value(), 0u);
    EXPECT_EQ(memory.read_u32(OUT4).value(), 0u);
    EXPECT_EQ(stream->requested_stops, 1);
    EXPECT_EQ(manager.cbuffer_release(buffer), 1u);  // the notify reference remains
    EXPECT_EQ(manager.notify_release(notify), 0u);
}

TEST_P(DirectSoundCaptureTest, MicrophonePermissionGatesTheStream) {
    // Unmeasured on Windows (Windows allows the microphone): a denied
    // microphone fails the stream open with DSERR_ACCESSDENIED (dsound.h),
    // never a buffer of fake silence.
    const GuestAddr object = make_capture();
    capture.state = audio::CapturePermission::Denied;
    EXPECT_EQ(try_buffer(object), dsound::DSERR_ACCESSDENIED);
    EXPECT_EQ(ptr(OUT2), 0u);
    EXPECT_EQ(capture.opens, 0);
    EXPECT_EQ(capture.requests, 0);
    // Undetermined: the user is asked, from the calling (guest) thread.
    capture.state = audio::CapturePermission::Undetermined;
    capture.answer = audio::CapturePermission::Denied;
    EXPECT_EQ(try_buffer(object), dsound::DSERR_ACCESSDENIED);
    EXPECT_EQ(capture.requests, 1);
    capture.state = audio::CapturePermission::Undetermined;
    capture.answer = audio::CapturePermission::Granted;
    EXPECT_EQ(try_buffer(object), dsound::DS_OK);
    EXPECT_EQ(capture.requests, 2);
    EXPECT_EQ(capture.opens, 1);
}

TEST_P(DirectSoundCaptureTest, FullDuplexCreatesBothSides) {
    put_format(1, 1, 16000, 16);
    put_render_desc(0, 32000);
    put_desc(dscbufferdesc(), 0, 32000);
    // Measured: a NULL output or capture descriptor E_INVALIDARG; aggregation
    // CLASS_E_NOAGGREGATION; every output untouched.
    set_ptr(OUT, SENTINEL);
    EXPECT_EQ(manager.full_duplex_create(0, 0, DESC, RDESC, 0x10010, 2, 0, OUT2, OUT3, 0),
              dsound::E_INVALIDARG);
    EXPECT_EQ(manager.full_duplex_create(0, 0, 0, RDESC, 0x10010, 2, OUT, OUT2, OUT3, 0),
              dsound::E_INVALIDARG);
    EXPECT_EQ(manager.full_duplex_create(0, 0, DESC, RDESC, 0x10010, 2, OUT, OUT2, OUT3, 0x77),
              dsound::CLASS_E_NOAGGREGATION);
    EXPECT_EQ(ptr(OUT), SENTINEL);
    // Success: IDirectSoundFullDuplex, IDirectSoundCaptureBuffer8 and
    // IDirectSoundBuffer8.
    ASSERT_EQ(manager.full_duplex_create(0, 0, DESC, RDESC, 0x10010, 2, OUT, OUT2, OUT3, 0),
              dsound::DS_OK);
    const GuestAddr fd = static_cast<GuestAddr>(ptr(OUT));
    const GuestAddr cbuf = static_cast<GuestAddr>(ptr(OUT2));
    const GuestAddr rbuf = static_cast<GuestAddr>(ptr(OUT3));
    EXPECT_EQ(qi_cbuffer(cbuf, kIDirectSoundCaptureBuffer8, dsound::DS_OK), cbuf);
    put_guid(IID_ADDR, kIDirectSoundBuffer8);
    EXPECT_EQ(manager.buffer_query_interface(rbuf, IID_ADDR, OUT4), dsound::DS_OK);
    EXPECT_EQ(manager.full_duplex_initialize(fd, 0, 0, DESC, RDESC, 0x10010, 2, OUT2, OUT3),
              dsound::DSERR_ALREADYINITIALIZED);
    put_guid(IID_ADDR, kIDirectSoundCapture);
    ASSERT_EQ(manager.full_duplex_query_interface(fd, IID_ADDR, OUT4), dsound::DS_OK);
    EXPECT_EQ(manager.capture_release(static_cast<GuestAddr>(ptr(OUT4))), 1u);
    put_guid(IID_ADDR, kIDirectSound8);
    ASSERT_EQ(manager.full_duplex_query_interface(fd, IID_ADDR, OUT4), dsound::DS_OK);
    EXPECT_EQ(manager.release(static_cast<GuestAddr>(ptr(OUT4))), 1u);
    EXPECT_EQ(manager.full_duplex_release(fd), 0u);
    // Without a playback device: DSERR_NODRIVER with the outputs untouched
    // ("env-norender ds FullDuplexCreate hr=88780078 fd=untouched").
    DirectSoundManager silent{nullptr};
    silent.set_memory(&memory);
    silent.set_thunk_registry(&registry);
    silent.set_audio_capture(&capture);
    silent.note_guest_bitness(x64);
    set_ptr(OUT, SENTINEL);
    set_ptr(OUT2, SENTINEL);
    EXPECT_EQ(silent.full_duplex_create(0, 0, DESC, RDESC, 0x10010, 2, OUT, OUT2, OUT3, 0),
              dsound::DSERR_NODRIVER);
    EXPECT_EQ(ptr(OUT), SENTINEL);
    EXPECT_EQ(ptr(OUT2), SENTINEL);
}

TEST_P(DirectSoundCaptureTest, RenderBuffersGainNotifications) {
    // Learn (IDirectSoundNotify8): only DSBCAPS_CTRLPOSITIONNOTIFY buffers;
    // SetNotificationPositions only while stopped; DSBPN_OFFSETSTOP on Stop.
    set_ptr(OUT, SENTINEL);
    ASSERT_EQ(manager.direct_sound_create(0, OUT, 0, true), dsound::DS_OK);
    const GuestAddr ds = static_cast<GuestAddr>(ptr(OUT));
    put_format(1, 1, 16000, 16);
    for (const u32 flags : {0u, dsound::DSBCAPS_CTRLPOSITIONNOTIFY}) {
        put_render_desc(flags, 3200);
        ASSERT_EQ(manager.create_sound_buffer(ds, RDESC, OUT2, 0), dsound::DS_OK);
        const GuestAddr buffer = static_cast<GuestAddr>(ptr(OUT2));
        put_guid(IID_ADDR, kIDirectSoundNotify);
        set_ptr(OUT3, SENTINEL);
        const u32 hr = manager.buffer_query_interface(buffer, IID_ADDR, OUT3);
        if (flags == 0) {
            EXPECT_EQ(hr, dsound::E_NOINTERFACE);
            EXPECT_EQ(ptr(OUT3), 0u);
            continue;
        }
        ASSERT_EQ(hr, dsound::DS_OK);
        const GuestAddr notify = static_cast<GuestAddr>(ptr(OUT3));
        EXPECT_NE(notify, buffer);
        EXPECT_EQ(set_positions(notify, {{3200, 0xB1}}), dsound::E_INVALIDARG);
        ASSERT_EQ(set_positions(notify, {{799, 0xB1}, {dsound::DSBPN_OFFSETSTOP, 0xB2}}),
                  dsound::DS_OK);
        ASSERT_EQ(manager.buffer_play(buffer, 0, 0, dsound::DSBPLAY_LOOPING), dsound::DS_OK);
        EXPECT_EQ(set_positions(notify, {{100, 0xB1}}), dsound::DSERR_INVALIDCALL);
        // The host poller sees the play cursor pass 799.
        renderer.voices.back()->cursor = 1000;
        for (int i = 0; i < 500; ++i) {
            {
                std::lock_guard lock(signal_mutex);
                if (!signals.empty()) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        EXPECT_EQ(take_signals(), std::vector<u64>{0xB1});
        ASSERT_EQ(manager.buffer_stop(buffer), dsound::DS_OK);
        EXPECT_EQ(take_signals(), std::vector<u64>{0xB2});
        EXPECT_EQ(manager.notify_release(notify), 1u);
    }
    // A pointer-width IDirectSound8 vtable: slot 11 is VerifyCertification.
    const GuestAddr vtable = static_cast<GuestAddr>(ptr(ds));
    const auto thunk = registry.get_thunk_address("dsound.dll", "Vitreo_DSound_VerifyCertification");
    ASSERT_TRUE(thunk.is_ok());
    EXPECT_EQ(ptr(vtable + 11 * (x64 ? 8 : 4)), thunk.value());
}

TEST_P(DirectSoundCaptureTest, ChannelCountsFollowTheInputDevice) {
    // DSCCAPS.dwChannels is the device's own count (Learn: "1 is mono, 2 is
    // stereo, and so on"); a buffer needs the device's channels or an exact
    // mono<->stereo conversion, anything else is DSERR_BADFORMAT
    // (UNIMPLEMENTED: no speaker matrix).
    capture.list.push_back(audio::CaptureDevice{"uid-quad", u"Quad Array", 48000, 4, 0x33u});
    put_guid(GUID_ADDR, audio::capture_device_guid("uid-quad"));
    set_ptr(OUT, SENTINEL);
    ASSERT_EQ(manager.capture_create(GUID_ADDR, OUT, 0, true), dsound::DS_OK);
    const GuestAddr quad = static_cast<GuestAddr>(ptr(OUT));
    ASSERT_TRUE(memory.write_u32(CAPS, 16).is_ok());
    ASSERT_EQ(manager.capture_get_caps(quad, CAPS), dsound::DS_OK);
    EXPECT_EQ(memory.read_u32(CAPS + 12).value(), 4u);
    EXPECT_EQ(try_buffer(quad), dsound::DSERR_BADFORMAT);  // 16 kHz mono
    EXPECT_EQ(ptr(OUT2), 0u);
    put_format(0, 4, 48000, 16, &kSubPcm);
    put_desc(dscbufferdesc(), 0, 8 * 1600);
    ASSERT_EQ(manager.capture_create_buffer(quad, DESC, OUT2, 0), dsound::DS_OK);
    ASSERT_NE(capture.last, nullptr);
    EXPECT_EQ(capture.last->format.channels, 4u);
    EXPECT_EQ(capture.last->device, "uid-quad");
    // The mono USB input feeds a stereo buffer (each sample in both channels).
    put_guid(GUID_ADDR, audio::capture_device_guid("uid-usb"));
    ASSERT_EQ(manager.capture_create(GUID_ADDR, OUT, 0, true), dsound::DS_OK);
    const GuestAddr usb = static_cast<GuestAddr>(ptr(OUT));
    put_format(1, 2, 44100, 16);
    put_desc(dscbufferdesc(), 0, 4 * 4410);
    ASSERT_EQ(manager.capture_create_buffer(usb, DESC, OUT2, 0), dsound::DS_OK);
    EXPECT_EQ(capture.last->format.channels, 2u);
}
