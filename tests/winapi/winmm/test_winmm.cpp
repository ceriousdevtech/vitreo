/// @file test_winmm.cpp
/// @brief winmm.dll waveOut / waveIn / midi / mixer / PlaySound contract tests.
///
/// Expected values come from Microsoft Learn and from the Windows run of
/// tools/oracle_probe/s7_winmm_probe (.expected = x64, .x86.expected = x86);
/// each assertion names the probe line it mirrors.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/media/winmm/winmm.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <windowing/contracts/wave_output.hpp>
#include <capsule/vfs/vfs.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

/// Records what winmm hands the host stream. `auto_complete` returns every
/// block from another thread, like the CoreAudio refill callback.
class FakeWaveOutput final : public windowing::WaveOutput {
public:
    ~FakeWaveOutput() override {
        for (auto& f : pending_completions) f.wait();
    }
    u32 device_count() const noexcept override { return 1; }
    bool supports(const windowing::WaveOutputFormat&) const noexcept override {
        return true;
    }
    bool open(const windowing::WaveOutputFormat& format,
              CompletionCallback callback) override {
        opened = true;
        ++open_count;
        opened_format = format;
        completion = std::move(callback);
        return true;
    }
    bool is_open() const noexcept override { return opened; }
    void close() override { opened = false; }
    bool submit(u64 token, std::span<const Byte> pcm) override {
        submitted_tokens.push_back(token);
        submitted.assign(pcm.begin(), pcm.end());
        if (auto_complete) {
            auto cb = completion;
            const u32 size = static_cast<u32>(pcm.size());
            pending_completions.push_back(std::async(std::launch::async,
                [cb, token, size] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    if (cb) cb(token, size);
                }));
        }
        return true;
    }
    void reset() override { reset_called = true; }
    void set_paused(bool paused) override { paused_value = paused; }
    bool set_volume(float gain) override {
        volume = gain;
        return true;
    }

    bool opened{false};
    int open_count{0};
    bool reset_called{false};
    bool paused_value{false};
    bool auto_complete{false};
    float volume{0.0f};
    std::vector<u64> submitted_tokens;
    windowing::WaveOutputFormat opened_format{};
    CompletionCallback completion;
    std::vector<Byte> submitted;
    std::vector<std::future<void>> pending_completions;
};

constexpr GuestAddr kData = 0x10000;
constexpr GuestAddr kStack = 0x40000;
constexpr u32 WAVE_MAPPER = 0xFFFFFFFFu;

struct Call {
    u32 message;
    u64 handle;
    u64 instance;
    u64 param1;
};

/// A WinmmManager plus a guest CPU in either bitness, driven through the
/// exported thunks.
class WinmmHarness {
public:
    explicit WinmmHarness(bool x64) : x64_(x64), manager(memory, wave, vfs) {
        EXPECT_TRUE(memory.map_region(kData, 0x30000,
                                      MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(kStack, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        exports = create_winmm_thunks(manager);
    }

    /// Call export `name` with `args` in the guest calling convention.
    u64 call(std::string_view name, std::initializer_list<u64> args) {
        const auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& e) { return e.name == name; });
        EXPECT_NE(it, exports.entries.end()) << name;
        if (it == exports.entries.end()) return ~0ull;
        const GuestAddr sp = kStack + 0x8000;
        cpu.set_sp(sp);
        usize i = 0;
        for (u64 a : args) {
            if (x64_) {
                switch (i) {
                case 0: cpu.gpr.rcx = a; break;
                case 1: cpu.gpr.rdx = a; break;
                case 2: cpu.gpr.r8 = a; break;
                case 3: cpu.gpr.r9 = a; break;
                default:
                    EXPECT_TRUE(memory.write_u64(sp + 0x28 + (i - 4) * 8, a).is_ok());
                }
            } else {
                EXPECT_TRUE(memory.write_u32(sp + 4 + i * 4,
                                             static_cast<u32>(a)).is_ok());
            }
            ++i;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        it->handler(m);
        return x64_ ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFull);
    }

    [[nodiscard]] const WaveHdrLayout& hdr() const { return wavehdr_layout(x64_); }
    u32 u32_at(GuestAddr a) { return memory.read_u32(a).value_or(0xDEADBEEF); }
    u64 u64_at(GuestAddr a) { return memory.read_u64(a).value_or(0xDEADBEEF); }

    /// A prepared-ready WAVEHDR at `at` pointing at `bytes` bytes of data.
    void make_header(GuestAddr at, GuestAddr data, u32 bytes) {
        std::vector<Byte> zero(hdr().size, 0);
        ASSERT_TRUE(memory.write_bytes(at, zero.data(), zero.size()).is_ok());
        if (x64_) ASSERT_TRUE(memory.write_u64(at + hdr().data_offset, data).is_ok());
        else ASSERT_TRUE(memory.write_u32(at + hdr().data_offset,
                                          static_cast<u32>(data)).is_ok());
        ASSERT_TRUE(memory.write_u32(at + hdr().buffer_length_offset, bytes).is_ok());
    }

    /// 44.1 kHz 16-bit stereo PCM WAVEFORMATEX at `at`.
    void make_pcm_format(GuestAddr at, u16 tag = 1, u16 channels = 2,
                         u32 rate = 44100, u16 bits = 16, u16 align = 0) {
        const u16 block = align ? align : static_cast<u16>(channels * bits / 8);
        ASSERT_TRUE(memory.write_u16(at + 0, tag).is_ok());
        ASSERT_TRUE(memory.write_u16(at + 2, channels).is_ok());
        ASSERT_TRUE(memory.write_u32(at + 4, rate).is_ok());
        ASSERT_TRUE(memory.write_u32(at + 8, rate * block).is_ok());
        ASSERT_TRUE(memory.write_u16(at + 12, block).is_ok());
        ASSERT_TRUE(memory.write_u16(at + 14, bits).is_ok());
        ASSERT_TRUE(memory.write_u16(at + 16, 0).is_ok());
    }

    bool x64_;
    GuestMemory memory;
    capsule::Vfs vfs;
    FakeWaveOutput wave;
    WinmmManager manager;
    DllExports exports;
    cpu::CpuState cpu;
};

/// Record every waveOutProc call, synchronous or from the service thread.
struct CallbackLog {
    std::mutex mutex;
    std::vector<Call> calls;
    std::vector<std::thread::id> threads;
    void attach(WinmmManager& mgr) {
        const auto record = [this](GuestAddr, u64 handle, u32 message,
                                   u64 instance, u64 param1, u64) {
            std::lock_guard<std::mutex> g(mutex);
            calls.push_back({message, handle, instance, param1});
            threads.push_back(std::this_thread::get_id());
        };
        mgr.set_wave_callback_invoker(record);
        mgr.set_wave_sync_callback_invoker(record);
    }
    usize size() {
        std::lock_guard<std::mutex> g(mutex);
        return calls.size();
    }
};

constexpr u32 WOM_OPEN = 0x3BB;
constexpr u32 WOM_CLOSE = 0x3BC;
constexpr u32 WOM_DONE = 0x3BD;
constexpr u32 CALLBACK_FUNCTION = 0x30000;

bool wait_for(const std::function<bool()>& predicate) {
    for (int i = 0; i < 400; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

} // namespace

// =============================================================================
// WAVEHDR layout: mmeapi.h is pshpack1; s7_winmm_probe `sizes` wavehdr=0x30
// (x64) / 0x20 (x86).
// =============================================================================
TEST(WinmmAbiTest, WaveHdrLayoutsMatchBothBitnesses) {
    EXPECT_EQ(wavehdr32.data_offset, 0u);
    EXPECT_EQ(wavehdr32.buffer_length_offset, 4u);
    EXPECT_EQ(wavehdr32.user_offset, 12u);
    EXPECT_EQ(wavehdr32.flags_offset, 16u);
    EXPECT_EQ(wavehdr32.loops_offset, 20u);
    EXPECT_EQ(wavehdr32.size, 32u);
    EXPECT_EQ(wavehdr64.data_offset, 0u);
    EXPECT_EQ(wavehdr64.buffer_length_offset, 8u);
    EXPECT_EQ(wavehdr64.user_offset, 16u);
    EXPECT_EQ(wavehdr64.flags_offset, 24u);
    EXPECT_EQ(wavehdr64.loops_offset, 28u);
    EXPECT_EQ(wavehdr64.size, 48u);
}

TEST(WinmmTimeTest, TimeGetTimeUsesSystemUptime) {
    const u32 first = time_get_time();
    const u32 second = time_get_time();
    EXPECT_NE(first, 0u);
    EXPECT_GE(second, first);
}

// =============================================================================
// waveOut lifecycle, both bitnesses
// =============================================================================
class WinmmWaveOutTest : public ::testing::TestWithParam<bool> {};

TEST_P(WinmmWaveOutTest, OpenWriteDoneCloseUsesGuestLayout) {
    const bool x64 = GetParam();
    WinmmHarness h(x64);
    CallbackLog log;
    log.attach(h.manager);
    const GuestAddr fmt = kData, phwo = kData + 0x100, hdr = kData + 0x200,
                    pcm = kData + 0x400;
    h.make_pcm_format(fmt);
    ASSERT_TRUE(h.memory.write_u64(phwo, 0xCCCCCCCCCCCCCCCCull).is_ok());
    const u64 instance = x64 ? 0x1122334455667788ull : 0x11223344ull;

    ASSERT_EQ(h.call("waveOutOpen", {phwo, WAVE_MAPPER, fmt, 0x5000, instance,
                                     CALLBACK_FUNCTION}), 0u);
    const u64 hwo = x64 ? h.u64_at(phwo) : h.u32_at(phwo);
    ASSERT_NE(hwo, 0u);
    // HWAVEOUT is pointer-sized: x86 leaves the upper sentinel half alone.
    if (!x64) EXPECT_EQ(h.u32_at(phwo + 4), 0xCCCCCCCCu);
    // fn_open r=0 cb_at_return=1: WOM_OPEN before waveOutOpen returns,
    // on the caller's thread, with the full DWORD_PTR instance.
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log.calls[0].message, WOM_OPEN);
    EXPECT_EQ(log.calls[0].handle, hwo);
    EXPECT_EQ(log.calls[0].instance, instance);
    EXPECT_EQ(log.threads[0], std::this_thread::get_id());

    const std::array<u8, 8> data{1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(h.memory.write_bytes(pcm, reinterpret_cast<const Byte*>(data.data()),
                                     data.size()).is_ok());
    h.make_header(hdr, pcm, data.size());
    // write_unprepared r=22, prep f=2, write f=12.
    EXPECT_EQ(h.call("waveOutWrite", {hwo, hdr, h.hdr().size}), 34u);
    EXPECT_EQ(h.call("waveOutPrepareHeader", {hwo, hdr, h.hdr().size - 1}), 11u);
    EXPECT_EQ(h.call("waveOutPrepareHeader", {hwo, hdr, h.hdr().size}), 0u);
    EXPECT_EQ(h.u32_at(hdr + h.hdr().flags_offset), 0x2u);
    EXPECT_EQ(h.call("waveOutWrite", {hwo, hdr, h.hdr().size}), 0u);
    EXPECT_EQ(h.u32_at(hdr + h.hdr().flags_offset), 0x12u);
    ASSERT_EQ(h.wave.submitted.size(), data.size());
    EXPECT_EQ(static_cast<u8>(h.wave.submitted[7]), 8u);
    // write_again / unprep_inqueue / close_inqueue: WAVERR_STILLPLAYING.
    EXPECT_EQ(h.call("waveOutWrite", {hwo, hdr, h.hdr().size}), 33u);
    EXPECT_EQ(h.call("waveOutUnprepareHeader", {hwo, hdr, h.hdr().size}), 33u);
    EXPECT_EQ(h.call("waveOutClose", {hwo}), 33u);

    ASSERT_EQ(h.wave.submitted_tokens.size(), 1u);
    h.wave.completion(h.wave.submitted_tokens[0], data.size());
    ASSERT_TRUE(wait_for([&] { return log.size() == 2; }));
    // fn_done_cbs: WOM_DONE from another thread, header already f=3.
    EXPECT_EQ(log.calls[1].message, WOM_DONE);
    EXPECT_EQ(log.calls[1].param1, hdr);
    EXPECT_NE(log.threads[1], std::this_thread::get_id());
    EXPECT_EQ(h.u32_at(hdr + h.hdr().flags_offset), 0x3u);
    // unprep f=1: DONE survives unprepare.
    EXPECT_EQ(h.call("waveOutUnprepareHeader", {hwo, hdr, h.hdr().size}), 0u);
    EXPECT_EQ(h.u32_at(hdr + h.hdr().flags_offset), 0x1u);

    // getid id=ffffffff for a WAVE_MAPPER handle.
    const GuestAddr id = kData + 0x180;
    EXPECT_EQ(h.call("waveOutGetID", {hwo, id}), 0u);
    EXPECT_EQ(h.u32_at(id), WAVE_MAPPER);
    EXPECT_EQ(h.call("waveOutGetID", {hwo, 0}), 11u);
    EXPECT_EQ(h.call("waveOutGetID", {0x1234, id}), 5u);

    // close r=0 cb_at_return=1, close_again r=5, write_closed r=22.
    EXPECT_EQ(h.call("waveOutClose", {hwo}), 0u);
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log.calls[2].message, WOM_CLOSE);
    EXPECT_EQ(log.threads[2], std::this_thread::get_id());
    EXPECT_EQ(h.call("waveOutClose", {hwo}), 5u);
    EXPECT_EQ(h.call("waveOutWrite", {hwo, hdr, h.hdr().size}), 34u);
}

TEST_P(WinmmWaveOutTest, ResetReturnsEveryHeaderBeforeReturning) {
    const bool x64 = GetParam();
    WinmmHarness h(x64);
    CallbackLog log;
    log.attach(h.manager);
    const GuestAddr fmt = kData, phwo = kData + 0x100, pcm = kData + 0x1000;
    h.make_pcm_format(fmt);
    ASSERT_EQ(h.call("waveOutOpen", {phwo, WAVE_MAPPER, fmt, 0x5000, 7,
                                     CALLBACK_FUNCTION}), 0u);
    const u64 hwo = x64 ? h.u64_at(phwo) : h.u32_at(phwo);
    EXPECT_EQ(h.call("waveOutPause", {hwo}), 0u);
    std::array<GuestAddr, 3> headers{kData + 0x200, kData + 0x300, kData + 0x400};
    for (GuestAddr hdr : headers) {
        h.make_header(hdr, pcm, 64);
        ASSERT_EQ(h.call("waveOutPrepareHeader", {hwo, hdr, h.hdr().size}), 0u);
        ASSERT_EQ(h.call("waveOutWrite", {hwo, hdr, h.hdr().size}), 0u);
        EXPECT_EQ(h.u32_at(hdr + h.hdr().flags_offset), 0x12u); // paused_flags
    }
    // reset r=0 cb_at_return=3 f=3 f=3 f=3; pos_after_reset cb=0.
    EXPECT_EQ(h.call("waveOutReset", {hwo}), 0u);
    EXPECT_EQ(log.size(), 4u);
    for (GuestAddr hdr : headers) {
        EXPECT_EQ(h.u32_at(hdr + h.hdr().flags_offset), 0x3u);
    }
    EXPECT_TRUE(h.wave.reset_called);
    EXPECT_FALSE(h.wave.paused_value); // after_reset_write done=1
    const GuestAddr mmt = kData + 0x800;
    ASSERT_TRUE(h.memory.write_u32(mmt, 4).is_ok());
    EXPECT_EQ(h.call("waveOutGetPosition", {hwo, mmt, 12}), 0u);
    EXPECT_EQ(h.u32_at(mmt), 4u);
    EXPECT_EQ(h.u32_at(mmt + 4), 0u);
    // A stale completion for a returned block is ignored.
    h.wave.completion(h.wave.submitted_tokens[0], 64);
    EXPECT_EQ(h.call("waveOutReset", {0x1234}), 5u); // reset_bad
    EXPECT_EQ(h.call("waveOutClose", {hwo}), 0u);
}

INSTANTIATE_TEST_SUITE_P(Bitness, WinmmWaveOutTest, ::testing::Values(false, true),
    [](const ::testing::TestParamInfo<bool>& info) {
        return info.param ? std::string("x64") : std::string("x86");
    });

// =============================================================================
// waveOutOpen argument, format and callback contract
// =============================================================================
TEST(WinmmWaveOutOpenTest, ArgumentAndFormatChecks) {
    WinmmHarness h(true);
    const GuestAddr fmt = kData, phwo = kData + 0x100;
    h.make_pcm_format(fmt);
    const auto open = [&](u64 device, u32 flags) {
        EXPECT_TRUE(h.memory.write_u64(phwo, 0x5A5A5A5A5A5A5A5Aull).is_ok());
        return h.call("waveOutOpen", {phwo, device, fmt, 0, 0, flags});
    };
    // o_null_fmt / o_null_phwo / q_null_fmt: INVALPARAM, *phwo untouched.
    EXPECT_EQ(h.call("waveOutOpen", {phwo, WAVE_MAPPER, 0, 0, 0, 0}), 11u);
    EXPECT_EQ(h.call("waveOutOpen", {0, WAVE_MAPPER, fmt, 0, 0, 0}), 11u);
    EXPECT_EQ(h.call("waveOutOpen", {0, WAVE_MAPPER, fmt, 0, 0, 1}), 0u); // q_pcm_null_phwo
    // o_baddev r=2 h=0; q_pcm_baddev r=2.
    EXPECT_EQ(open(1, 0), 2u);
    EXPECT_EQ(h.u64_at(phwo), 0u);
    EXPECT_EQ(open(1, 1), 2u);
    EXPECT_EQ(h.u64_at(phwo), 0x5A5A5A5A5A5A5A5Aull); // query never writes

    // q_bad_align=0 but o_real_bad_align=20.
    h.make_pcm_format(fmt, 1, 2, 44100, 16, 3);
    EXPECT_EQ(open(WAVE_MAPPER, 1), 0u);
    EXPECT_EQ(open(WAVE_MAPPER, 0), 32u);
    EXPECT_EQ(h.u64_at(phwo), 0u);
    // q_12bit / q_0ch / q_6ch = 20.
    h.make_pcm_format(fmt, 1, 2, 48000, 12);
    EXPECT_EQ(open(WAVE_MAPPER, 1), 32u);
    h.make_pcm_format(fmt, 1, 6, 48000, 16);
    EXPECT_EQ(open(WAVE_MAPPER, 1), 32u);
    h.make_pcm_format(fmt, 1, 0, 48000, 16);
    EXPECT_EQ(open(WAVE_MAPPER, 1), 32u);
    // q_float16=0, o_real_float16=20; q_float32 / q_float64 = 0.
    h.make_pcm_format(fmt, 3, 2, 48000, 16);
    EXPECT_EQ(open(WAVE_MAPPER, 1), 0u);
    EXPECT_EQ(open(WAVE_MAPPER, 0), 32u);
    h.make_pcm_format(fmt, 3, 2, 48000, 64);
    EXPECT_EQ(open(WAVE_MAPPER, 1), 0u);
    // q_tag_unknown = 20.
    h.make_pcm_format(fmt, 0x55, 1, 22050, 4);
    EXPECT_EQ(open(WAVE_MAPPER, 1), 32u);
    // q_ext_cb0 = 20; q_ext_pcm16 = 0; q_ext_sub_unknown = 20.
    h.make_pcm_format(fmt, 0xFFFE, 2, 48000, 16);
    EXPECT_EQ(open(WAVE_MAPPER, 1), 32u);
    ASSERT_TRUE(h.memory.write_u16(fmt + 16, 22).is_ok());
    ASSERT_TRUE(h.memory.write_u16(fmt + 18, 16).is_ok());
    ASSERT_TRUE(h.memory.write_u32(fmt + 20, 3).is_ok());
    const std::array<u8, 16> pcm_guid{0x01, 0, 0, 0, 0x00, 0x00, 0x10, 0x00,
                                      0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    ASSERT_TRUE(h.memory.write_bytes(fmt + 24, reinterpret_cast<const Byte*>(pcm_guid.data()),
                                     16).is_ok());
    EXPECT_EQ(open(WAVE_MAPPER, 1), 0u);
    ASSERT_TRUE(h.memory.write_u32(fmt + 24, 0x55).is_ok());
    EXPECT_EQ(open(WAVE_MAPPER, 1), 32u);

    // An IEEE float stream reaches the host as 32-bit integer PCM.
    h.make_pcm_format(fmt, 3, 1, 48000, 32);
    ASSERT_EQ(open(0, 0), 0u);
    EXPECT_EQ(h.wave.opened_format.bits_per_sample, 32u);
    const u64 hwo = h.u64_at(phwo);
    const GuestAddr hdr = kData + 0x200, pcm = kData + 0x400;
    const float samples[2] = {0.5f, -1.0f};
    ASSERT_TRUE(h.memory.write_bytes(pcm, reinterpret_cast<const Byte*>(samples), 8).is_ok());
    h.make_header(hdr, pcm, 8);
    ASSERT_EQ(h.call("waveOutPrepareHeader", {hwo, hdr, 48}), 0u);
    ASSERT_EQ(h.call("waveOutWrite", {hwo, hdr, 48}), 0u);
    ASSERT_EQ(h.wave.submitted.size(), 8u);
    i32 converted[2];
    std::memcpy(converted, h.wave.submitted.data(), 8);
    EXPECT_EQ(converted[0], 0x40000000);
    EXPECT_EQ(converted[1], static_cast<i32>(0x80000000u));
    // getid_dev0 id=0.
    EXPECT_EQ(h.call("waveOutGetID", {hwo, kData + 0x180}), 0u);
    EXPECT_EQ(h.u32_at(kData + 0x180), 0u);
    EXPECT_EQ(h.call("waveOutReset", {hwo}), 0u);
    EXPECT_EQ(h.call("waveOutClose", {hwo}), 0u);
}

TEST(WinmmWaveOutOpenTest, CallbackKindsNotifyLikeWindows) {
    WinmmHarness h(true);
    struct Post { u64 target; u32 message; u64 wparam; u64 lparam; };
    std::mutex mu;
    std::vector<Post> thread_posts, window_posts;
    std::vector<u64> events;
    h.manager.set_wave_thread_notifier([&](u32 tid, u32 msg, u64 w, u64 l) {
        std::lock_guard<std::mutex> g(mu);
        thread_posts.push_back({tid, msg, w, l});
        return true;
    });
    h.manager.set_wave_window_notifier([&](u64 hwnd, u32 msg, u64 w, u64 l) {
        std::lock_guard<std::mutex> g(mu);
        window_posts.push_back({hwnd, msg, w, l});
        return true;
    });
    h.manager.set_wave_event_signaler([&](u64 ev) {
        std::lock_guard<std::mutex> g(mu);
        events.push_back(ev);
        return true;
    });
    h.manager.set_window_validator([](u64 hwnd) { return hwnd == 0x777; });
    const GuestAddr fmt = kData, phwo = kData + 0x100;
    h.make_pcm_format(fmt);

    // o_window_bad r=b (before the device check), o_window_null r=0.
    EXPECT_EQ(h.call("waveOutOpen", {phwo, 99, fmt, 0x12340, 0, 0x10000}), 11u);
    // th_open_msg / th_close_msg: posted before the call returns,
    // wParam = hwo, lParam = 0.
    ASSERT_EQ(h.call("waveOutOpen", {phwo, WAVE_MAPPER, fmt, 0x4242, 0, 0x20000}), 0u);
    u64 hwo = h.u64_at(phwo);
    ASSERT_EQ(thread_posts.size(), 1u);
    EXPECT_EQ(thread_posts[0].target, 0x4242u);
    EXPECT_EQ(thread_posts[0].message, WOM_OPEN);
    EXPECT_EQ(thread_posts[0].wparam, hwo);
    EXPECT_EQ(thread_posts[0].lparam, 0u);
    EXPECT_EQ(h.call("waveOutClose", {hwo}), 0u);
    ASSERT_EQ(thread_posts.size(), 2u);
    EXPECT_EQ(thread_posts[1].message, WOM_CLOSE);

    // wn_open_msg / wn_close_msg.
    ASSERT_EQ(h.call("waveOutOpen", {phwo, WAVE_MAPPER, fmt, 0x777, 0, 0x10000}), 0u);
    hwo = h.u64_at(phwo);
    ASSERT_EQ(window_posts.size(), 1u);
    EXPECT_EQ(window_posts[0].target, 0x777u);
    EXPECT_EQ(window_posts[0].message, WOM_OPEN);
    EXPECT_EQ(h.call("waveOutClose", {hwo}), 0u);
    EXPECT_EQ(window_posts.back().message, WOM_CLOSE);

    // ev_open / ev_done / ev_close: the event is set on every transition.
    ASSERT_EQ(h.call("waveOutOpen", {phwo, WAVE_MAPPER, fmt, 0x88, 0, 0x50000}), 0u);
    hwo = h.u64_at(phwo);
    EXPECT_EQ(events.size(), 1u);
    const GuestAddr hdr = kData + 0x200;
    h.make_header(hdr, kData + 0x400, 16);
    ASSERT_EQ(h.call("waveOutPrepareHeader", {hwo, hdr, 48}), 0u);
    ASSERT_EQ(h.call("waveOutWrite", {hwo, hdr, 48}), 0u);
    h.wave.completion(h.wave.submitted_tokens.back(), 16);
    ASSERT_TRUE(wait_for([&] {
        std::lock_guard<std::mutex> g(mu);
        return events.size() == 2;
    }));
    EXPECT_EQ(h.call("waveOutUnprepareHeader", {hwo, hdr, 48}), 0u);
    EXPECT_EQ(h.call("waveOutClose", {hwo}), 0u);
    EXPECT_EQ(events.size(), 3u);
    EXPECT_EQ(events[2], 0x88u);

    // o_cbtype t=40000: unknown kinds are accepted and never notified.
    ASSERT_EQ(h.call("waveOutOpen", {phwo, WAVE_MAPPER, fmt, 0x99, 0, 0x40000}), 0u);
    EXPECT_EQ(h.call("waveOutClose", {h.u64_at(phwo)}), 0u);
}

TEST(WinmmWaveOutOpenTest, ConcurrentStreamsGetTheirOwnHostStream) {
    WinmmHarness h(true);
    std::vector<FakeWaveOutput*> created;
    h.manager.set_wave_output_factory([&created] {
        auto out = std::make_unique<FakeWaveOutput>();
        created.push_back(out.get());
        return std::unique_ptr<windowing::WaveOutput>(std::move(out));
    });
    const GuestAddr fmt = kData, a = kData + 0x100, b = kData + 0x108;
    h.make_pcm_format(fmt);
    // multi_open ra=0 rb=0 distinct=1.
    ASSERT_EQ(h.call("waveOutOpen", {a, WAVE_MAPPER, fmt, 0, 0, 0}), 0u);
    ASSERT_EQ(h.call("waveOutOpen", {b, WAVE_MAPPER, fmt, 0, 0, 0}), 0u);
    EXPECT_NE(h.u64_at(a), h.u64_at(b));
    EXPECT_TRUE(h.wave.opened);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_TRUE(created[0]->opened);
    EXPECT_EQ(h.call("waveOutClose", {h.u64_at(a)}), 0u);
    EXPECT_TRUE(created[0]->opened); // the second stream keeps playing
    EXPECT_EQ(h.call("waveOutClose", {h.u64_at(b)}), 0u);
}

TEST(WinmmWaveOutOpenTest, ZeroLengthBufferCompletesInOrder) {
    WinmmHarness h(true);
    CallbackLog log;
    log.attach(h.manager);
    const GuestAddr fmt = kData, phwo = kData + 0x100;
    h.make_pcm_format(fmt);
    ASSERT_EQ(h.call("waveOutOpen", {phwo, WAVE_MAPPER, fmt, 0x5000, 0,
                                     CALLBACK_FUNCTION}), 0u);
    const u64 hwo = h.u64_at(phwo);
    const GuestAddr full = kData + 0x200, empty = kData + 0x300;
    h.make_header(full, kData + 0x400, 16);
    h.make_header(empty, kData + 0x400, 0);
    for (GuestAddr hdr : {full, empty}) {
        ASSERT_EQ(h.call("waveOutPrepareHeader", {hwo, hdr, 48}), 0u);
        ASSERT_EQ(h.call("waveOutWrite", {hwo, hdr, 48}), 0u); // write_empty_hdr r=0
    }
    EXPECT_EQ(h.u32_at(empty + 24), 0x12u);
    h.wave.completion(h.wave.submitted_tokens.back(), 16);
    ASSERT_TRUE(wait_for([&] { return log.size() == 3; }));
    EXPECT_EQ(log.calls[1].param1, full);
    EXPECT_EQ(log.calls[2].param1, empty);
    EXPECT_EQ(h.u32_at(empty + 24), 0x3u); // write_empty_hdr done=1 f=3
    // Returned headers may stay prepared; only queued ones block the close.
    EXPECT_EQ(h.call("waveOutClose", {hwo}), 0u);
}

// =============================================================================
// waveOutGetDevCaps / waveOutMessage / waveOutGetPosition
// =============================================================================
TEST(WinmmWaveOutQueryTest, DevCapsCopySizesAndValues) {
    WinmmHarness h(true);
    const GuestAddr buf = kData + 0x1000;
    const auto fill = [&] {
        std::vector<Byte> cc(256, 0xCC);
        ASSERT_TRUE(h.memory.write_bytes(buf, cc.data(), cc.size()).is_ok());
    };
    const auto touched = [&](usize from, usize n) {
        usize count = 0;
        for (usize i = 0; i < n; ++i) {
            if (h.memory.read_u8(buf + from + i).value() != 0xCC) ++count;
        }
        return count;
    };
    // caps_a_cb0 / caps_a_cb0_baddev: nothing written, NOERROR.
    fill();
    EXPECT_EQ(h.call("waveOutGetDevCapsA", {0, buf, 0}), 0u);
    EXPECT_EQ(h.call("waveOutGetDevCapsA", {5, buf, 0}), 0u);
    EXPECT_EQ(touched(0, 64), 0u);
    // caps_a_null / caps_a_baddev / caps_a_badhandle.
    EXPECT_EQ(h.call("waveOutGetDevCapsA", {0, 0, 52}), 11u);
    EXPECT_EQ(h.call("waveOutGetDevCapsA", {1, buf, 52}), 2u);
    EXPECT_EQ(h.call("waveOutGetDevCapsA", {0x1234, buf, 52}), 2u);
    // caps_a_cb cb=10: exactly 10 bytes; cb=101: WAVEOUTCAPS2A's 100.
    fill();
    EXPECT_EQ(h.call("waveOutGetDevCapsA", {WAVE_MAPPER, buf, 10}), 0u);
    EXPECT_EQ(touched(0, 64), 10u);
    fill();
    EXPECT_EQ(h.call("waveOutGetDevCapsA", {WAVE_MAPPER, buf, 101}), 0u);
    EXPECT_EQ(h.memory.read_u8(buf + 100).value(), 0xCCu);
    EXPECT_NE(h.memory.read_u8(buf + 99).value(), 0xCCu);
    // info caps_a (mapper): mid=1 pid=2 ver=0x500 name "Microsoft Sound Mapper".
    EXPECT_EQ(h.memory.read_u16(buf).value(), 1u);
    EXPECT_EQ(h.memory.read_u16(buf + 2).value(), 2u);
    EXPECT_EQ(h.u32_at(buf + 4), 0x500u);
    EXPECT_EQ(h.memory.read_u8(buf + 8).value(), 'M');
    EXPECT_EQ(h.u32_at(buf + 40), 0xFFFFFu);
    EXPECT_EQ(h.memory.read_u16(buf + 44).value(), 2u);
    EXPECT_EQ(h.u32_at(buf + 52), 0u); // mapper GUIDs are zero (info caps2_a)
    // caps_w_cb < 84 -> INVALPARAM; mapper W copies exactly 84 (cb=133).
    EXPECT_EQ(h.call("waveOutGetDevCapsW", {WAVE_MAPPER, buf, 83}), 11u);
    fill();
    EXPECT_EQ(h.call("waveOutGetDevCapsW", {WAVE_MAPPER, buf, 133}), 0u);
    EXPECT_EQ(touched(84, 64), 0u);
    EXPECT_NE(h.memory.read_u8(buf + 83).value(), 0xCCu);
    // Device W copies WAVEOUTCAPS2W: INIT_MMREG_MID(1) / INIT_MMREG_PID(100)
    // (info caps2_w id=0 g=d5a47fa8 ... g=e36dc310).
    fill();
    EXPECT_EQ(h.call("waveOutGetDevCapsW", {0, buf, 200}), 0u);
    EXPECT_EQ(h.memory.read_u16(buf + 2).value(), 100u);
    EXPECT_EQ(h.u32_at(buf + 4), 0xA00u);
    EXPECT_EQ(h.u32_at(buf + 84), 0xd5a47fa8u);
    EXPECT_EQ(h.u32_at(buf + 88), 0x11d16d98u);
    EXPECT_EQ(h.u32_at(buf + 100), 0xe36dc310u);
    EXPECT_EQ(h.u32_at(buf + 116), 0u);
    EXPECT_EQ(h.memory.read_u8(buf + 132).value(), 0xCCu);
}

TEST(WinmmWaveOutQueryTest, MessageAndPosition) {
    WinmmHarness h(true);
    const GuestAddr d1 = kData + 0x100, d2 = kData + 0x108;
    ASSERT_TRUE(h.memory.write_u64(d1, 0xCCCCCCCCCCCCCCCCull).is_ok());
    ASSERT_TRUE(h.memory.write_u64(d2, 0xCCCCCCCCCCCCCCCCull).is_ok());
    // msg_pref r=0 d1=cccccccc00000000 d2=cccccccc00000000.
    EXPECT_EQ(h.call("waveOutMessage", {WAVE_MAPPER, 0x2015, d1, d2}), 0u);
    EXPECT_EQ(h.u64_at(d1), 0xCCCCCCCC00000000ull);
    EXPECT_EQ(h.u64_at(d2), 0xCCCCCCCC00000000ull);
    EXPECT_EQ(h.call("waveOutMessage", {WAVE_MAPPER, 0x2017, d1, d2}), 0u);
    EXPECT_EQ(h.call("waveOutMessage", {WAVE_MAPPER, 0x2015, 0, 0}), 11u);  // msg_pref_null
    EXPECT_EQ(h.call("waveOutMessage", {WAVE_MAPPER, 0x2015, d1, 0}), 11u); // _d2null
    EXPECT_EQ(h.call("waveOutMessage", {WAVE_MAPPER, 0x4321, 0, 0}), 8u);   // unknown_mapper
    EXPECT_EQ(h.call("waveOutMessage", {0x1234, 0x4321, 0, 0}), 2u);        // msg_badh
    EXPECT_EQ(h.call("waveOutMessage", {0, 0x2015, d1, d2}), 11u);          // msg_zero_h
    EXPECT_EQ(h.call("waveOutMessage", {0, 0x4321, 0, 0}), 8u);             // dev0_unknown

    const GuestAddr fmt = kData, phwo = kData + 0x200, mmt = kData + 0x300;
    h.make_pcm_format(fmt);
    ASSERT_EQ(h.call("waveOutOpen", {phwo, WAVE_MAPPER, fmt, 0, 0, 0}), 0u);
    const u64 hwo = h.u64_at(phwo);
    EXPECT_EQ(h.call("waveOutMessage", {hwo, 0x2015, d1, d2}), 8u); // msg_handle_pref
    // pos_cb4 / pos_cb11 r=1, pos_null r=b, pos_badh r=5, pos_ms type=4,
    // pos_samples type=2.
    EXPECT_EQ(h.call("waveOutGetPosition", {hwo, mmt, 4}), 1u);
    EXPECT_EQ(h.call("waveOutGetPosition", {hwo, mmt, 11}), 1u);
    EXPECT_EQ(h.call("waveOutGetPosition", {hwo, 0, 12}), 11u);
    EXPECT_EQ(h.call("waveOutGetPosition", {0x1234, mmt, 12}), 5u);
    ASSERT_TRUE(h.memory.write_u32(mmt, 1).is_ok());
    EXPECT_EQ(h.call("waveOutGetPosition", {hwo, mmt, 12}), 0u);
    EXPECT_EQ(h.u32_at(mmt), 4u);
    ASSERT_TRUE(h.memory.write_u32(mmt, 2).is_ok());
    EXPECT_EQ(h.call("waveOutGetPosition", {hwo, mmt, 16}), 0u);
    EXPECT_EQ(h.u32_at(mmt), 2u);
    EXPECT_EQ(h.call("waveOutClose", {hwo}), 0u);
}

// =============================================================================
// Error text (wot_a / mot_a / wit_a lines)
// =============================================================================
TEST(WinmmErrorTextTest, MeasuredStringsAndEdgeCases) {
    WinmmHarness h(false);
    const GuestAddr buf = kData + 0x100;
    const auto text = [&](usize n) {
        std::string s;
        for (usize i = 0; i < n; ++i) {
            const u8 c = h.memory.read_u8(buf + i).value();
            if (c == 0) break;
            s.push_back(static_cast<char>(c));
        }
        return s;
    };
    EXPECT_EQ(h.call("waveOutGetErrorTextA", {33, buf, 256}), 0u);
    EXPECT_EQ(text(256), "Cannot perform this operation while media data is still "
                         "playing.  Reset the device, or wait until the data is "
                         "finished playing.");
    EXPECT_EQ(h.call("waveInGetErrorTextA", {20, buf, 256}), 0u);
    EXPECT_EQ(text(256), "The driver did not generate a valid OPEN callback.");
    // e=21 (MMSYSERR_MOREDATA) and MIDIERR codes are unknown to waveOut: 9,
    // with an empty string written.
    ASSERT_TRUE(h.memory.write_u8(buf, 'x').is_ok());
    EXPECT_EQ(h.call("waveOutGetErrorTextA", {21, buf, 256}), 9u);
    EXPECT_EQ(h.memory.read_u8(buf).value(), 0u);
    EXPECT_EQ(h.call("waveOutGetErrorTextA", {64, buf, 256}), 9u);
    EXPECT_EQ(h.call("midiOutGetErrorTextA", {32, buf, 256}), 9u);
    EXPECT_EQ(h.call("midiInGetErrorTextA", {71, buf, 256}), 0u);
    EXPECT_EQ(text(256), "Driver condition - do not callback this input event");
    // wot_a_cch0 (untouched) / wot_a_null / wot_a_null0 / wot_a_bad_cch0.
    ASSERT_TRUE(h.memory.write_u8(buf, 0xCC).is_ok());
    EXPECT_EQ(h.call("waveOutGetErrorTextA", {2, buf, 0}), 0u);
    EXPECT_EQ(h.memory.read_u8(buf).value(), 0xCCu);
    EXPECT_EQ(h.call("waveOutGetErrorTextA", {2, 0, 256}), 11u);
    EXPECT_EQ(h.call("waveOutGetErrorTextA", {2, 0, 0}), 0u);
    EXPECT_EQ(h.call("waveOutGetErrorTextA", {1000, buf, 0}), 0u);
    // wot_a_cch5 "A de" + NUL; wot_w_cch5 w4=0 w5=cccc.
    ASSERT_TRUE(h.memory.write_u8(buf + 5, 0xCC).is_ok());
    EXPECT_EQ(h.call("waveOutGetErrorTextA", {2, buf, 5}), 0u);
    EXPECT_EQ(text(5), "A de");
    EXPECT_EQ(h.memory.read_u8(buf + 5).value(), 0xCCu);
    ASSERT_TRUE(h.memory.write_u16(buf + 10, 0xCCCC).is_ok());
    EXPECT_EQ(h.call("midiOutGetErrorTextW", {68, buf, 5}), 0u);
    EXPECT_EQ(h.memory.read_u16(buf).value(), u'T');
    EXPECT_EQ(h.memory.read_u16(buf + 8).value(), 0u);
    EXPECT_EQ(h.memory.read_u16(buf + 10).value(), 0xCCCCu);
}

// =============================================================================
// MIDI and mixer with no device
// =============================================================================
TEST(WinmmNoDeviceTest, MidiErrorsAndMapperCaps) {
    WinmmHarness h(true);
    const GuestAddr ph = kData + 0x100, buf = kData + 0x200;
    ASSERT_TRUE(h.memory.write_u64(ph, 0x5A5A5A5A5A5A5A5Aull).is_ok());
    // mo_open_bad r=2 h=0 (pointer width); Learn: the mapper without a port
    // is MIDIERR_NODEVICE; mo_open_null r=b; mi_open_mapper r=2 h=0.
    EXPECT_EQ(h.call("midiOutOpen", {ph, 100, 0, 0, 0}), 2u);
    EXPECT_EQ(h.u64_at(ph), 0u);
    EXPECT_EQ(h.call("midiOutOpen", {ph, 0xFFFFFFFFu, 0, 0, 0}), 68u);
    EXPECT_EQ(h.call("midiOutOpen", {0, 0, 0, 0, 0}), 11u);
    EXPECT_EQ(h.call("midiInOpen", {ph, 0xFFFFFFFFu, 0, 0, 0}), 2u);
    EXPECT_EQ(h.call("midiOutClose", {0x1234}), 5u);
    EXPECT_EQ(h.call("midiOutShortMsg", {0x1234, 0x90}), 5u);
    // mo_caps_a_cb0 r=0, mo_caps_a_null r=b, mo_caps_a_bad r=2.
    EXPECT_EQ(h.call("midiOutGetDevCapsA", {100, buf, 0}), 0u);
    EXPECT_EQ(h.call("midiOutGetDevCapsA", {100, 0, 52}), 11u);
    EXPECT_EQ(h.call("midiOutGetDevCapsW", {100, buf, 84}), 2u);
    EXPECT_EQ(h.call("midiInGetDevCapsA", {100, buf, 44}), 2u);
    EXPECT_EQ(h.call("midiInGetDevCapsA", {100, buf, 0}), 0u);
    // info mo_caps_mapper: mid=1 pid=1 ver=500 tech=5 voices=0 notes=0
    // chmask=ffff sup=9 name="Microsoft MIDI Mapper".
    ASSERT_EQ(h.call("midiOutGetDevCapsA", {0xFFFFFFFFu, buf, 52}), 0u);
    EXPECT_EQ(h.memory.read_u16(buf).value(), 1u);
    EXPECT_EQ(h.memory.read_u16(buf + 2).value(), 1u);
    EXPECT_EQ(h.u32_at(buf + 4), 0x500u);
    EXPECT_EQ(h.memory.read_u8(buf + 8).value(), 'M');
    EXPECT_EQ(h.memory.read_u16(buf + 40).value(), 5u);
    EXPECT_EQ(h.memory.read_u16(buf + 46).value(), 0xFFFFu);
    EXPECT_EQ(h.u32_at(buf + 48), 9u);
}

class WinmmMixerTest : public ::testing::TestWithParam<bool> {};

TEST_P(WinmmMixerTest, ParameterChecksPrecedeTheObjectCheck) {
    const bool x64 = GetParam();
    WinmmHarness h(x64);
    const GuestAddr ph = kData + 0x100, s = kData + 0x200, arr = kData + 0x800;
    ASSERT_TRUE(h.memory.write_u64(ph, 0xCCCCCCCCCCCCCCCCull).is_ok());
    // mx_open_bad r=2 h=0 (x64 all 8 bytes, x86 low 4: cccccccc00000000).
    EXPECT_EQ(h.call("mixerOpen", {ph, 100, 0, 0, 0}), 2u);
    EXPECT_EQ(h.u64_at(ph), x64 ? 0u : 0xCCCCCCCC00000000ull);
    EXPECT_EQ(h.call("mixerOpen", {0, 0, 0, 0, 0}), 11u);            // mx_open_null
    EXPECT_EQ(h.call("mixerOpen", {ph, 100, 0, 0, 0x1}), 10u);       // mx_open_flag1
    EXPECT_EQ(h.call("mixerOpen", {ph, 100, 0x1234, 0, 0x30000}), 10u); // cbfunc
    EXPECT_EQ(h.call("mixerOpen", {ph, 100, 0, 0, 0x70000000}), 11u);   // badflag
    EXPECT_EQ(h.call("mixerOpen", {ph, 0x1234, 0, 0, 0x90000000}), 5u); // badwaveout
    // mixerGetID writes -1 on failure (mx_getid_*).
    ASSERT_TRUE(h.memory.write_u32(s, 0xCCCC).is_ok());
    EXPECT_EQ(h.call("mixerGetID", {0x1234, s, 0x80000000}), 5u);
    EXPECT_EQ(h.u32_at(s), 0xFFFFFFFFu);
    EXPECT_EQ(h.call("mixerGetID", {100, s, 0}), 2u);
    EXPECT_EQ(h.call("mixerGetID", {100, 0, 0}), 11u);
    EXPECT_EQ(h.call("mixerGetDevCapsA", {100, s, 0}), 0u);
    EXPECT_EQ(h.call("mixerGetDevCapsW", {100, 0, 80}), 11u);
    EXPECT_EQ(h.call("mixerGetDevCapsW", {100, s, 80}), 2u);

    // MIXERLINE: sizes3 mla=0xa8/0xac, mlw=0x118/0x11c.
    const u32 line_a = x64 ? 172 : 168, line_w = x64 ? 284 : 280;
    ASSERT_TRUE(h.memory.write_u32(s, line_a).is_ok());
    EXPECT_EQ(h.call("mixerGetLineInfoA", {100, s, 0}), 2u);            // badid
    EXPECT_EQ(h.call("mixerGetLineInfoA", {0x1234, s, 0x80000000}), 5u); // badh
    EXPECT_EQ(h.call("mixerGetLineInfoA", {100, s, 0xF}), 10u);          // badquery
    EXPECT_EQ(h.call("mixerGetLineInfoW", {100, s, 0}), 11u);            // w_cb_a
    ASSERT_TRUE(h.memory.write_u32(s, line_w).is_ok());
    EXPECT_EQ(h.call("mixerGetLineInfoW", {100, s, 0}), 2u);
    EXPECT_EQ(h.call("mixerGetLineInfoA", {100, 0, 0}), 11u);            // null

    // MIXERLINECONTROLS (0x18/0x1c): cControls@12 cbmxctrl@16.
    std::vector<Byte> zero(64, 0);
    ASSERT_TRUE(h.memory.write_bytes(s, zero.data(), zero.size()).is_ok());
    ASSERT_TRUE(h.memory.write_u32(s, x64 ? 28 : 24).is_ok());
    ASSERT_TRUE(h.memory.write_u32(s + 12, 1).is_ok());
    ASSERT_TRUE(h.memory.write_u32(s + 16, 148).is_ok());
    EXPECT_EQ(h.call("mixerGetLineControlsA", {100, s, 0}), 2u);          // nullarr passes
    EXPECT_EQ(h.call("mixerGetLineControlsA", {0x1234, s, 0x80000000}), 5u);
    EXPECT_EQ(h.call("mixerGetLineControlsA", {100, s, 0xF}), 10u);
    EXPECT_EQ(h.call("mixerGetLineControlsW", {100, s, 0}), 11u);         // w_cbctl_a
    ASSERT_TRUE(h.memory.write_u32(s + 12, 0).is_ok());
    EXPECT_EQ(h.call("mixerGetLineControlsA", {100, s, 0}), 11u);         // cctl0

    // MIXERCONTROLDETAILS (0x18/0x20): cbDetails/paDetails @16/20 (x86),
    // @20/24 (x64).
    ASSERT_TRUE(h.memory.write_bytes(s, zero.data(), zero.size()).is_ok());
    ASSERT_TRUE(h.memory.write_u32(s, x64 ? 32 : 24).is_ok());
    ASSERT_TRUE(h.memory.write_u32(s + 8, 1).is_ok());
    ASSERT_TRUE(h.memory.write_u32(s + (x64 ? 20 : 16), 4).is_ok());
    if (x64) ASSERT_TRUE(h.memory.write_u64(s + 24, arr).is_ok());
    else ASSERT_TRUE(h.memory.write_u32(s + 20, static_cast<u32>(arr)).is_ok());
    EXPECT_EQ(h.call("mixerGetControlDetailsA", {100, s, 0}), 2u);        // valid_badid
    EXPECT_EQ(h.call("mixerSetControlDetails", {0x1234, s, 0x80000000}), 5u);
    EXPECT_EQ(h.call("mixerGetControlDetailsA", {100, s, 1}), 11u);       // listtext
    EXPECT_EQ(h.call("mixerGetControlDetailsA", {100, s, 0xF}), 10u);     // badquery
    EXPECT_EQ(h.call("mixerGetControlDetailsA", {100, 0, 0}), 11u);       // null
    EXPECT_EQ(h.call("mixerClose", {0x1234}), 5u);
}

INSTANTIATE_TEST_SUITE_P(Bitness, WinmmMixerTest, ::testing::Values(false, true),
    [](const ::testing::TestParamInfo<bool>& info) {
        return info.param ? std::string("x64") : std::string("x86");
    });

TEST(WinmmNoDeviceTest, WaveInRejectsEveryDevice) {
    WinmmHarness h(true);
    const GuestAddr buf = kData + 0x100, fmt = kData + 0x200;
    h.make_pcm_format(fmt);
    EXPECT_EQ(h.call("waveInGetNumDevs", {}), 0u);
    EXPECT_EQ(h.call("waveInGetDevCapsA", {100, buf, 48}), 2u); // wi_caps_bad
    EXPECT_EQ(h.call("waveInGetDevCapsW", {100, buf, 0}), 0u);
    EXPECT_EQ(h.call("waveInClose", {0x1234}), 5u);              // wi_close_bad
    EXPECT_EQ(h.call("waveInOpen", {buf, 100, fmt, 0, 0, 0}), 2u); // wi_open_bad
    for (const char* name : {"waveInPrepareHeader", "waveInUnprepareHeader",
                             "waveInStart", "waveInStop", "waveInReset"}) {
        SCOPED_TRACE(name);
        EXPECT_EQ(h.call(name, {0x1234, buf, 48}), 5u);
    }
    // audio_capture_contract_probe "wi bad handle: AddBuffer(unprepared)
    // r=34": the header is checked before the handle.
    EXPECT_EQ(h.call("waveInAddBuffer", {0x1234, buf, 48}), 34u);
    // The mapper's preferred device: -1 with no device.
    const GuestAddr id = kData + 0x300, flags = kData + 0x304;
    EXPECT_EQ(h.call("waveInMessage", {WAVE_MAPPER, 0x2015, id, flags}), 0u);
    EXPECT_EQ(h.u32_at(id), 0xFFFFFFFFu);
}

// =============================================================================
// PlaySound
// =============================================================================
namespace {
/// 50 ms of 8 kHz 8-bit mono silence (the probe's s7_winmm_probe.rc data).
std::vector<u8> probe_wav() {
    std::vector<u8> w = {'R', 'I', 'F', 'F', 0xB4, 1, 0, 0, 'W', 'A', 'V', 'E',
                         'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                         0x40, 0x1f, 0, 0, 0x40, 0x1f, 0, 0, 1, 0, 8, 0,
                         'd', 'a', 't', 'a', 0x90, 1, 0, 0};
    w.resize(444, 0x80);
    return w;
}
constexpr u32 SND_ASYNC = 0x1, SND_NODEFAULT = 0x2, SND_MEMORY = 0x4,
              SND_LOOP = 0x8, SND_NOSTOP = 0x10, SND_FILENAME = 0x20000,
              SND_ALIAS_ID = 0x110000;
} // namespace

TEST(WinmmPlaySoundTest, MemoryFileAndStopSemantics) {
    WinmmHarness h(true);
    h.wave.auto_complete = true;
    const auto root = std::filesystem::temp_directory_path() /
                      ("vitreo_winmm_" + std::to_string(::getpid()));
    std::filesystem::create_directories(root);
    const std::vector<u8> wav = probe_wav();
    {
        std::ofstream f(root / "tone.wav", std::ios::binary);
        f.write(reinterpret_cast<const char*>(wav.data()),
                static_cast<std::streamsize>(wav.size()));
    }
    ASSERT_TRUE(h.vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    const GuestAddr mem = kData + 0x1000, name = kData + 0x100,
                    wname = kData + 0x400;
    ASSERT_TRUE(h.memory.write_bytes(mem, reinterpret_cast<const Byte*>(wav.data()),
                                     wav.size()).is_ok());

    // ps_mem r=1 le=0: the data chunk reaches the host stream as-is.
    set_last_error(0xDEAD);
    EXPECT_EQ(h.call("PlaySoundA", {mem, 0, SND_MEMORY | SND_NODEFAULT}), 1u);
    EXPECT_EQ(get_last_error(), 0u);
    EXPECT_EQ(h.wave.opened_format.samples_per_second, 8000u);
    EXPECT_EQ(h.wave.submitted.size(), 400u);

    // ps_mem_junk / ps_mem_notwave: FALSE, last error untouched.
    std::vector<Byte> junk(64, 7);
    ASSERT_TRUE(h.memory.write_bytes(kData + 0x2000, junk.data(), junk.size()).is_ok());
    set_last_error(0xDEAD);
    EXPECT_EQ(h.call("PlaySoundA", {kData + 0x2000, 0, SND_MEMORY | SND_NODEFAULT}), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    // ps_file_sync / ps_file_w_sync r=1 le=0.
    ASSERT_TRUE(h.memory.write_bytes(name, reinterpret_cast<const Byte*>("C:\\tone.wav"),
                                     12).is_ok());
    set_last_error(0xDEAD);
    EXPECT_EQ(h.call("PlaySoundA", {name, 0, SND_FILENAME | SND_NODEFAULT}), 1u);
    EXPECT_EQ(get_last_error(), 0u);
    const std::u16string wide = u"C:\\tone.wav";
    ASSERT_TRUE(h.memory.write_bytes(wname, reinterpret_cast<const Byte*>(wide.c_str()),
                                     (wide.size() + 1) * 2).is_ok());
    EXPECT_EQ(h.call("PlaySoundW", {wname, 0, SND_FILENAME | SND_NODEFAULT}), 1u);
    // ps_noflag_file r=1: a flagless name with no association is a file.
    EXPECT_EQ(h.call("sndPlaySoundA", {name, SND_NODEFAULT}), 1u);

    // ps_missing_w_nodefault le=3 (missing directory),
    // ps_noflag_missing le=2 (bare name).
    ASSERT_TRUE(h.memory.write_bytes(name, reinterpret_cast<const Byte*>("C:\\nodir\\x.wav"),
                                     15).is_ok());
    EXPECT_EQ(h.call("PlaySoundA", {name, 0, SND_FILENAME | SND_NODEFAULT}), 0u);
    EXPECT_EQ(get_last_error(), 3u);
    ASSERT_TRUE(h.memory.write_bytes(name, reinterpret_cast<const Byte*>("nosuch"), 7).is_ok());
    EXPECT_EQ(h.call("PlaySoundA", {name, 0, SND_NODEFAULT}), 0u);
    EXPECT_EQ(get_last_error(), 2u);
    // ps_aliasid_bad le=0x716.
    EXPECT_EQ(h.call("PlaySoundA", {0x1234, 0, SND_ALIAS_ID | SND_NODEFAULT}), 0u);
    EXPECT_EQ(get_last_error(), 1814u);

    // The process's first async sound leaves the last error 0
    // (ps_file_async le=0), later ones leave it alone (ps_loop_async le=dead).
    h.wave.auto_complete = false;
    set_last_error(0xDEAD);
    EXPECT_EQ(h.call("PlaySoundA", {mem, 0, SND_MEMORY | SND_ASYNC | SND_NODEFAULT}), 1u);
    EXPECT_EQ(get_last_error(), 0u);
    // Async loop: ps_nostop_busy FALSE (le untouched), ps_stop_null le=0,
    // ps_stop_idle2 le untouched.
    set_last_error(0xDEAD);
    EXPECT_EQ(h.call("PlaySoundA", {mem, 0, SND_MEMORY | SND_ASYNC | SND_LOOP |
                                            SND_NODEFAULT}), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(h.call("PlaySoundA", {mem, 0, SND_MEMORY | SND_ASYNC | SND_NOSTOP |
                                            SND_NODEFAULT}), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    const usize before = h.wave.submitted_tokens.size();
    h.wave.completion(h.wave.submitted_tokens.back(), 400); // loops: resubmits
    EXPECT_EQ(h.wave.submitted_tokens.size(), before + 1);
    EXPECT_EQ(h.call("PlaySoundA", {0, 0, 0}), 1u);
    EXPECT_EQ(get_last_error(), 0u);
    set_last_error(0xDEAD);
    EXPECT_EQ(h.call("PlaySoundW", {0, 0, 0}), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    // ps_file_empty: an empty name stops, TRUE.
    ASSERT_TRUE(h.memory.write_u8(name, 0).is_ok());
    EXPECT_EQ(h.call("PlaySoundA", {name, 0, SND_FILENAME}), 1u);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// =============================================================================
// waveIn with capture devices. Expected values: Learn (waveIn*, waveInProc,
// WIM_*) and tools/oracle_probe/audio_capture_contract_probe{32,64}.expected
// (quoted per assertion); a fake host capture backend, no microphone.
// =============================================================================
#include "../fake_audio_capture.hpp"
#include <audio/contracts/audio_capture.hpp>

namespace {
constexpr u32 WIM_OPEN = 0x3BE;
constexpr u32 WIM_CLOSE = 0x3BF;
constexpr u32 WIM_DATA = 0x3C0;
constexpr u32 CALLBACK_EVENT = 0x50000;
constexpr u32 CALLBACK_THREAD = 0x20000;
constexpr u32 CALLBACK_WINDOW = 0x10000;
constexpr u32 WAVE_FORMAT_QUERY = 0x1;
constexpr u32 WHDR_DONE = 0x1, WHDR_PREPARED = 0x2, WHDR_INQUEUE = 0x10;
constexpr u32 TIME_MS = 0x1, TIME_SAMPLES = 0x2, TIME_BYTES = 0x4, TIME_TICKS = 0x20;
constexpr GuestAddr kPhwi = kData + 0x000, kFmt = kData + 0x040, kCaps = kData + 0x100,
                    kMmt = kData + 0x400, kOut = kData + 0x420, kHdr = kData + 0x500,
                    kHdr2 = kData + 0x540, kHdr3 = kData + 0x580, kText = kData + 0x600,
                    kBuf = kData + 0x1000, kBuf2 = kData + 0x2000, kBuf3 = kData + 0x3000;
}  // namespace

class WinmmWaveInTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        x64 = GetParam();
        h = std::make_unique<WinmmHarness>(x64);
        capture.add_two_devices();
        h->manager.set_audio_capture(&capture);
        log.attach(h->manager);
    }
    void TearDown() override { h.reset(); }

    u64 phwi() { return x64 ? h->u64_at(kPhwi) : h->u32_at(kPhwi); }
    void set_phwi(u64 v) {
        if (x64) ASSERT_TRUE(h->memory.write_u64(kPhwi, v).is_ok());
        else ASSERT_TRUE(h->memory.write_u32(kPhwi, static_cast<u32>(v)).is_ok());
    }
    /// 16 kHz mono 16-bit, opened with a function callback.
    u64 open(u32 device = 0, u32 flags = CALLBACK_FUNCTION) {
        h->make_pcm_format(kFmt, 1, 1, 16000, 16);
        EXPECT_EQ(h->call("waveInOpen", {kPhwi, device, kFmt, 0x7000, 0x55, flags}), 0u);
        return phwi();
    }
    u32 flags_of(GuestAddr hdr) { return h->u32_at(hdr + h->hdr().flags_offset); }
    u32 recorded(GuestAddr hdr) { return h->u32_at(hdr + h->hdr().bytes_recorded_offset); }
    std::u16string wstr(GuestAddr at) {
        std::u16string t;
        for (;; at += 2) {
            const u16 u = h->memory.read_u16(at).value();
            if (u == 0) break;
            t.push_back(static_cast<char16_t>(u));
        }
        return t;
    }
    usize count(u32 message) {
        std::lock_guard<std::mutex> g(log.mutex);
        return static_cast<usize>(std::count_if(log.calls.begin(), log.calls.end(),
                                                [&](const Call& c) { return c.message == message; }));
    }

    bool x64{false};
    std::unique_ptr<WinmmHarness> h;
    test::FakeCapture capture;
    CallbackLog log;
};

INSTANTIATE_TEST_SUITE_P(Bitness, WinmmWaveInTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? std::string("x64") : std::string("x86");
                         });

TEST_P(WinmmWaveInTest, DevicesAndCapabilities) {
    // "wi GetNumDevs agrees-with-mmdevice=1"; ids: the default device first
    // ("default-first-then-mm-order=1"), szPname the friendly name.
    EXPECT_EQ(h->call("waveInGetNumDevs", {}), 2u);
    ASSERT_EQ(h->call("waveInGetDevCapsW", {0, kCaps, 80}), 0u);
    // "mid=1 pid=101 ver=a00 formats=fffff ... reserved=0".
    EXPECT_EQ(h->memory.read_u16(kCaps).value(), 1u);
    EXPECT_EQ(h->memory.read_u16(kCaps + 2).value(), 101u);
    EXPECT_EQ(h->u32_at(kCaps + 4), 0xa00u);
    EXPECT_EQ(wstr(kCaps + 8), u"MacBook Pro Microphone");
    EXPECT_EQ(h->u32_at(kCaps + 72), 0xfffffu);
    EXPECT_EQ(h->memory.read_u16(kCaps + 76).value(), 2u);
    EXPECT_EQ(h->memory.read_u16(kCaps + 78).value(), 0u);
    ASSERT_EQ(h->call("waveInGetDevCapsW", {1, kCaps, 80}), 0u);
    EXPECT_EQ(wstr(kCaps + 8), u"USB Headset Microphone");
    EXPECT_EQ(h->memory.read_u16(kCaps + 76).value(), 1u);
    // A name longer than MAXPNAMELEN - 1 is cut to 31 characters.
    capture.list[0].name = u"An Extremely Long Microphone Device Name";
    ASSERT_EQ(h->call("waveInGetDevCapsW", {1, kCaps, 80}), 0u);
    EXPECT_EQ(wstr(kCaps + 8), u"An Extremely Long Microphone De");
    ASSERT_EQ(h->call("waveInGetDevCapsA", {1, kCaps, 48}), 0u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(h->memory.host_ptr(kCaps + 8, 32))),
              "An Extremely Long Microphone De");
    // "cb=0 r=0", "NULL r=11", "W cb=79 r=11 / cb=80 r=0", "n r=2", "bad handle r=2".
    EXPECT_EQ(h->call("waveInGetDevCapsW", {0, kCaps, 0}), 0u);
    EXPECT_EQ(h->call("waveInGetDevCapsW", {0, 0, 80}), 11u);
    EXPECT_EQ(h->call("waveInGetDevCapsW", {0, kCaps, 79}), 11u);
    EXPECT_EQ(h->call("waveInGetDevCapsW", {2, kCaps, 80}), 2u);
    EXPECT_EQ(h->call("waveInGetDevCapsA", {2, kCaps, 48}), 2u);
    EXPECT_EQ(h->call("waveInGetDevCapsW", {0x9999, kCaps, 80}), 2u);
    // Bytes written: "W(0, cb=512) 512 past-caps2-zero=1", "A(0, cb=512) 96",
    // "A(0, cb=10) 10", "W(MAPPER, cb=512) 80", "A(MAPPER, cb=512) 96".
    auto written = [&](const char* name, u64 id, u32 cb) {
        std::vector<Byte> fill(600, 0xcc);
        EXPECT_TRUE(h->memory.write_bytes(kBuf, fill.data(), fill.size()).is_ok());
        EXPECT_EQ(h->call(name, {id, kBuf, cb}), 0u);
        int last = -1;
        for (int i = 0; i < 600; ++i) {
            if (h->memory.read_u8(kBuf + static_cast<GuestAddr>(i)).value() != 0xcc) last = i;
        }
        return last + 1;
    };
    EXPECT_EQ(written("waveInGetDevCapsW", 0, 512), 512);
    for (GuestAddr i = 128; i < 512; ++i) ASSERT_EQ(h->memory.read_u8(kBuf + i).value(), 0u);
    EXPECT_NE(h->memory.read_u8(kBuf + 80).value(), 0xccu);  // ManufacturerGuid written
    EXPECT_EQ(written("waveInGetDevCapsA", 0, 512), 96);
    EXPECT_EQ(written("waveInGetDevCapsA", 0, 10), 10);
    EXPECT_EQ(written("waveInGetDevCapsW", WAVE_MAPPER, 512), 80);
    EXPECT_EQ(written("waveInGetDevCapsA", WAVE_MAPPER, 512), 96);
    // "MAPPER r=0 mid=1 pid=2 ver=500 name=Microsoft Sound Mapper formats=fffff".
    ASSERT_EQ(h->call("waveInGetDevCapsW", {WAVE_MAPPER, kCaps, 80}), 0u);
    EXPECT_EQ(h->memory.read_u16(kCaps + 2).value(), 2u);
    EXPECT_EQ(h->u32_at(kCaps + 4), 0x500u);
    EXPECT_EQ(wstr(kCaps + 8), u"Microsoft Sound Mapper");
    EXPECT_EQ(h->u32_at(kCaps + 72), 0xfffffu);
    // An open handle answers its device's caps ("same-as-device=1").
    const u64 handle = open(1);
    ASSERT_EQ(h->call("waveInGetDevCapsW", {handle, kCaps, 80}), 0u);
    EXPECT_EQ(wstr(kCaps + 8), u"An Extremely Long Microphone De");
    EXPECT_EQ(h->call("waveInClose", {handle}), 0u);
}

TEST_P(WinmmWaveInTest, MessagesNameTheEndpointAndTheDefault) {
    // "Message(0, FUNCTIONINSTANCEIDSIZE) r=0 size-is-id-bytes=1",
    // "(0, FUNCTIONINSTANCEID) is-console-default-id=1", "(8-byte buffer) r=21",
    // "(n, ...) r=2", "(0, DEVICEINTERFACESIZE) r=0", "(0, unknown) r=8".
    const std::string id = audio::capture_endpoint_id("uid-builtin");
    ASSERT_EQ(h->call("waveInMessage", {0, 0x812, kOut, 0}), 0u);
    EXPECT_EQ(h->u32_at(kOut), (id.size() + 1) * 2);
    ASSERT_EQ(h->call("waveInMessage", {0, 0x811, kText, 512}), 0u);
    EXPECT_EQ(wstr(kText), std::u16string(id.begin(), id.end()));
    ASSERT_EQ(h->call("waveInMessage", {1, 0x811, kText, 512}), 0u);
    const std::string id1 = audio::capture_endpoint_id("uid-usb");
    EXPECT_EQ(wstr(kText), std::u16string(id1.begin(), id1.end()));
    EXPECT_EQ(h->call("waveInMessage", {0, 0x811, kText, 8}), 21u);
    EXPECT_EQ(h->call("waveInMessage", {2, 0x812, kOut, 0}), 2u);
    EXPECT_EQ(h->call("waveInMessage", {0, 0x80D, kOut, 0}), 0u);
    EXPECT_EQ(h->call("waveInMessage", {0, 0x7777, 0, 0}), 8u);
    // DRV_QUERYDEVICEINTERFACE(SIZE): Windows names the kernel-streaming
    // filter ("ks-filter-path=1"); Vitreo's inputs have none, so a device
    // answers as an id without a device interface does on Windows, the wave
    // mapper: "(MAPPER, DEVICEINTERFACESIZE) r=0 size=2", "(MAPPER,
    // DEVICEINTERFACE, 4 bytes) r=0 first=0000 second=7777", "(1 byte) r=21
    // first=7777"; NULL is r=11 ("(0, DEVICEINTERFACE, NULL) r=11",
    // "(0, DEVICEINTERFACESIZE, NULL) r=11"); "(n, ...) r=2".
    for (const u64 id : {u64{0}, u64{WAVE_MAPPER}}) {
        ASSERT_TRUE(h->memory.write_u32(kOut, 77).is_ok());
        ASSERT_EQ(h->call("waveInMessage", {id, 0x80D, kOut, 0}), 0u);
        EXPECT_EQ(h->u32_at(kOut), 2u);
        ASSERT_TRUE(h->memory.write_u32(kText, 0x77777777u).is_ok());
        ASSERT_EQ(h->call("waveInMessage", {id, 0x80C, kText, 4}), 0u);
        EXPECT_EQ(h->memory.read_u16(kText).value(), 0u);
        EXPECT_EQ(h->memory.read_u16(kText + 2).value(), 0x7777u);
        ASSERT_TRUE(h->memory.write_u16(kText, 0x7777).is_ok());
        EXPECT_EQ(h->call("waveInMessage", {id, 0x80C, kText, 1}), 21u);
        EXPECT_EQ(h->memory.read_u16(kText).value(), 0x7777u);
        EXPECT_EQ(h->call("waveInMessage", {id, 0x80C, 0, 512}), 11u);
        EXPECT_EQ(h->call("waveInMessage", {id, 0x80D, 0, 0}), 11u);
    }
    EXPECT_EQ(h->call("waveInMessage", {2, 0x80D, kOut, 0}), 2u);
    // "(MAPPER, PREFERRED_GET) r=0 pref=0 flags=0"; CONSOLEVOICECOM_GET the same.
    ASSERT_TRUE(h->memory.write_u32(kOut, 77).is_ok());
    ASSERT_TRUE(h->memory.write_u32(kOut + 4, 77).is_ok());
    ASSERT_EQ(h->call("waveInMessage", {WAVE_MAPPER, 0x2015, kOut, kOut + 4}), 0u);
    EXPECT_EQ(h->u32_at(kOut), 0u);
    EXPECT_EQ(h->u32_at(kOut + 4), 0u);
    ASSERT_EQ(h->call("waveInMessage", {WAVE_MAPPER, 0x2017, kOut, kOut + 4}), 0u);
    EXPECT_EQ(h->u32_at(kOut), 0u);
}

TEST_P(WinmmWaveInTest, OpenRulesAndCallbacks) {
    h->make_pcm_format(kFmt, 1, 1, 16000, 16);
    // "Open(phwi NULL) r=11", "QUERY r=0 phwi=untouched", "QUERY phwi NULL r=0",
    // "pwfx NULL r=11 untouched", "bad id r=2 null", "adpcm r=32 null", "bad
    // align r=11 null", "bad window r=11 untouched", "0 + DEFAULT_COMMUNICATION
    // r=10".
    EXPECT_EQ(h->call("waveInOpen", {0, 0, kFmt, 0, 0, 0}), 11u);
    set_phwi(0x1234);
    EXPECT_EQ(h->call("waveInOpen", {kPhwi, 0, kFmt, 0, 0, WAVE_FORMAT_QUERY}), 0u);
    EXPECT_EQ(phwi(), 0x1234u);
    EXPECT_EQ(h->call("waveInOpen", {0, 0, kFmt, 0, 0, WAVE_FORMAT_QUERY}), 0u);
    EXPECT_EQ(h->call("waveInOpen", {kPhwi, 0, 0, 0, 0, 0}), 11u);
    EXPECT_EQ(phwi(), 0x1234u);
    EXPECT_EQ(h->call("waveInOpen", {kPhwi, 2, kFmt, 0, 0, 0}), 2u);
    EXPECT_EQ(phwi(), 0u);
    set_phwi(0x1234);
    h->make_pcm_format(kFmt, 2, 1, 16000, 16);
    EXPECT_EQ(h->call("waveInOpen", {kPhwi, 0, kFmt, 0, 0, 0}), 32u);
    EXPECT_EQ(phwi(), 0u);
    set_phwi(0x1234);
    h->make_pcm_format(kFmt, 1, 1, 16000, 16, 3);
    EXPECT_EQ(h->call("waveInOpen", {kPhwi, 0, kFmt, 0, 0, 0}), 11u);
    EXPECT_EQ(phwi(), 0u);
    h->make_pcm_format(kFmt, 1, 1, 16000, 16);
    h->manager.set_window_validator([](u64 hwnd) { return hwnd == 0x777; });
    set_phwi(0x1234);
    EXPECT_EQ(h->call("waveInOpen", {kPhwi, 0, kFmt, 0x12345, 0, CALLBACK_WINDOW}), 11u);
    EXPECT_EQ(phwi(), 0x1234u);
    EXPECT_EQ(h->call("waveInOpen", {kPhwi, 0, kFmt, 0, 0, 0x10}), 10u);
    // Accepted: float 48 kHz, 8-bit stereo 11 kHz, an unknown callback type,
    // WAVE_ALLOWSYNC, an unknown flag, the mapper (GetID answers WAVE_MAPPER),
    // the mapper with WAVE_MAPPED_DEFAULT_COMMUNICATION_DEVICE, and the same
    // device twice.
    struct Ok { u16 tag; u16 ch; u32 rate; u16 bits; u32 device; u32 flags; };
    for (const Ok& o : {Ok{3, 2, 48000, 32, 0, 0}, Ok{1, 2, 11025, 8, 0, 0},
                        Ok{1, 1, 16000, 16, 0, 0x40000}, Ok{1, 1, 16000, 16, 0, 0x2},
                        Ok{1, 1, 16000, 16, 0, 0x100}, Ok{1, 1, 16000, 16, WAVE_MAPPER, 0},
                        Ok{1, 1, 16000, 16, WAVE_MAPPER, 0x10}}) {
        h->make_pcm_format(kFmt, o.tag, o.ch, o.rate, o.bits);
        ASSERT_EQ(h->call("waveInOpen", {kPhwi, o.device, kFmt, 0, 0, o.flags}), 0u);
        const u64 handle = phwi();
        ASSERT_EQ(h->call("waveInGetID", {handle, kOut}), 0u);
        EXPECT_EQ(h->u32_at(kOut), o.device);
        ASSERT_NE(capture.last, nullptr);
        EXPECT_EQ(capture.last->device, "uid-builtin");  // the mapper is device 0
        EXPECT_EQ(capture.last->format.sample_rate, o.rate);
        EXPECT_EQ(capture.last->format.float_samples, o.tag == 3);
        EXPECT_EQ(h->call("waveInClose", {handle}), 0u);
    }
    const u64 a = open(1);
    const u64 b = open(1);
    EXPECT_NE(a, b);
    EXPECT_EQ(capture.last->device, "uid-usb");
    EXPECT_EQ(h->call("waveInGetID", {a, 0}), 11u);
    EXPECT_EQ(h->call("waveInClose", {a}), 0u);
    EXPECT_EQ(h->call("waveInClose", {b}), 0u);

    // CALLBACK_FUNCTION: WIM_OPEN and WIM_CLOSE before the call returns, on
    // the calling thread ("WIM_OPEN-before-return=1 same-thread=1").
    log.calls.clear();
    const u64 f = open(0);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log.calls[0].message, WIM_OPEN);
    EXPECT_EQ(log.calls[0].handle, f);
    EXPECT_EQ(log.calls[0].instance, 0x55u);
    EXPECT_EQ(log.threads[0], std::this_thread::get_id());
    EXPECT_EQ(h->call("waveInClose", {f}), 0u);
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log.calls[1].message, WIM_CLOSE);
    // CALLBACK_EVENT: signaled by open and by close ("signaled-at-return=1").
    std::vector<u64> events;
    h->manager.set_wave_event_signaler([&](u64 ev) { events.push_back(ev); return true; });
    h->make_pcm_format(kFmt, 1, 1, 16000, 16);
    ASSERT_EQ(h->call("waveInOpen", {kPhwi, 0, kFmt, 0xE0, 0, CALLBACK_EVENT}), 0u);
    EXPECT_EQ(events, std::vector<u64>{0xE0});
    EXPECT_EQ(h->call("waveInClose", {phwi()}), 0u);
    EXPECT_EQ(events.size(), 2u);
    // CALLBACK_THREAD: MM_WIM_OPEN / MM_WIM_CLOSE posted with the handle.
    std::vector<std::tuple<u32, u32, u64>> posts;
    h->manager.set_wave_thread_notifier([&](u32 tid, u32 msg, u64 w, u64) {
        posts.emplace_back(tid, msg, w);
        return true;
    });
    ASSERT_EQ(h->call("waveInOpen", {kPhwi, 0, kFmt, 0x99, 0, CALLBACK_THREAD}), 0u);
    const u64 t = phwi();
    ASSERT_EQ(posts.size(), 1u);
    EXPECT_EQ(posts[0], std::make_tuple(0x99u, WIM_OPEN, t));
    EXPECT_EQ(h->call("waveInClose", {t}), 0u);
    ASSERT_EQ(posts.size(), 2u);
    EXPECT_EQ(std::get<1>(posts[1]), WIM_CLOSE);
}

TEST_P(WinmmWaveInTest, BuffersFillStopAndReset) {
    const u64 w = open(0);
    test::FakeCaptureStream* stream = capture.last;
    ASSERT_NE(stream, nullptr);
    const u32 hs = h->hdr().size;
    // Check order with a bad handle: "AddBuffer(unprepared) 34", "AddBuffer(NULL)
    // 11", "PrepareHeader(NULL) 11", "UnprepareHeader(INQUEUE) 33",
    // "AddBuffer(prepared) 5", "GetPosition(NULL) 11", "Stop/Reset 5".
    h->make_header(kHdr3, kBuf3, 3200);
    EXPECT_EQ(h->call("waveInAddBuffer", {0x9999, kHdr3, hs}), 34u);
    EXPECT_EQ(h->call("waveInAddBuffer", {0x9999, 0, hs}), 11u);
    EXPECT_EQ(h->call("waveInPrepareHeader", {0x9999, 0, hs}), 11u);
    ASSERT_TRUE(h->memory.write_u32(kHdr3 + h->hdr().flags_offset, WHDR_PREPARED | WHDR_INQUEUE).is_ok());
    EXPECT_EQ(h->call("waveInUnprepareHeader", {0x9999, kHdr3, hs}), 33u);
    EXPECT_EQ(h->call("waveInAddBuffer", {w, kHdr3, hs}), 33u);  // the flags claim INQUEUE
    ASSERT_TRUE(h->memory.write_u32(kHdr3 + h->hdr().flags_offset, WHDR_PREPARED).is_ok());
    EXPECT_EQ(h->call("waveInAddBuffer", {0x9999, kHdr3, hs}), 5u);
    EXPECT_EQ(h->call("waveInGetPosition", {0x9999, 0, 12}), 11u);
    EXPECT_EQ(h->call("waveInStop", {0x9999}), 5u);
    EXPECT_EQ(h->call("waveInReset", {0x9999}), 5u);
    // Prepare: "NULL 11", "cb small 11", "bad handle 5", "flags=DONE -> 2".
    h->make_header(kHdr, kBuf, 3200);
    h->make_header(kHdr2, kBuf2, 3200);
    EXPECT_EQ(h->call("waveInPrepareHeader", {w, 0, hs}), 11u);
    EXPECT_EQ(h->call("waveInPrepareHeader", {w, kHdr, hs - 1}), 11u);
    EXPECT_EQ(h->call("waveInPrepareHeader", {0x9999, kHdr, hs}), 5u);
    EXPECT_EQ(h->call("waveInAddBuffer", {w, kHdr, hs}), 34u);
    ASSERT_TRUE(h->memory.write_u32(kHdr + h->hdr().flags_offset, WHDR_DONE).is_ok());
    ASSERT_EQ(h->call("waveInPrepareHeader", {w, kHdr, hs}), 0u);
    EXPECT_EQ(flags_of(kHdr), WHDR_PREPARED);
    EXPECT_EQ(h->call("waveInPrepareHeader", {w, kHdr, hs}), 0u);
    ASSERT_EQ(h->call("waveInPrepareHeader", {w, kHdr2, hs}), 0u);
    // AddBuffer: "flags=12 recorded=0", "already queued 33", "Unprepare(queued) 33".
    ASSERT_TRUE(h->memory.write_u32(kHdr + h->hdr().bytes_recorded_offset, 0x1234).is_ok());
    ASSERT_EQ(h->call("waveInAddBuffer", {w, kHdr, hs}), 0u);
    EXPECT_EQ(flags_of(kHdr), WHDR_PREPARED | WHDR_INQUEUE);
    EXPECT_EQ(recorded(kHdr), 0u);
    EXPECT_EQ(h->call("waveInAddBuffer", {w, kHdr, hs}), 33u);
    EXPECT_EQ(h->call("waveInUnprepareHeader", {w, kHdr, hs}), 33u);
    ASSERT_EQ(h->call("waveInAddBuffer", {w, kHdr2, hs}), 0u);
    // "GetPosition before Start r=0 type=4 bytes=0".
    ASSERT_TRUE(h->memory.write_u32(kMmt, TIME_BYTES).is_ok());
    ASSERT_EQ(h->call("waveInGetPosition", {w, kMmt, 12}), 0u);
    EXPECT_EQ(h->u32_at(kMmt + 4), 0u);
    // "Start r=0", "Start again r=0"; data fills the buffers in order and
    // WIM_DATA arrives on another thread with the header
    // ("param1-is-header=1 other-thread=1", "done header flags=3 recorded=3200").
    ASSERT_EQ(h->call("waveInStart", {w}), 0u);
    EXPECT_EQ(h->call("waveInStart", {w}), 0u);
    EXPECT_EQ(stream->starts, 1);
    stream->feed_frames(1000, 0x11);  // 2000 bytes
    EXPECT_EQ(recorded(kHdr), 2000u);
    EXPECT_EQ(flags_of(kHdr), WHDR_PREPARED | WHDR_INQUEUE);
    stream->feed_frames(2000, 0x22);  // 4000 bytes: fills kHdr, 2800 into kHdr2
    ASSERT_TRUE(wait_for([&] { return count(WIM_DATA) == 1; }));
    EXPECT_EQ(flags_of(kHdr), WHDR_PREPARED | WHDR_DONE);
    EXPECT_EQ(recorded(kHdr), 3200u);
    EXPECT_EQ(h->memory.read_u8(kBuf).value(), 0x11u);
    EXPECT_EQ(h->memory.read_u8(kBuf + 1999).value(), 0x11u);
    EXPECT_EQ(h->memory.read_u8(kBuf + 2000).value(), 0x22u);
    EXPECT_EQ(recorded(kHdr2), 2800u);
    {
        std::lock_guard<std::mutex> g(log.mutex);
        const auto it = std::find_if(log.calls.begin(), log.calls.end(),
                                     [](const Call& c) { return c.message == WIM_DATA; });
        ASSERT_NE(it, log.calls.end());
        EXPECT_EQ(it->param1, kHdr);
        EXPECT_NE(log.threads[static_cast<usize>(it - log.calls.begin())], std::this_thread::get_id());
    }
    // GetPosition: "BYTES type=4", "SAMPLES type=2", "MS/SMPTE/TICKS/0x77 ->
    // type=4", "cb=4 r=0", "cb=0 r=0", "NULL r=11".
    ASSERT_TRUE(h->memory.write_u32(kMmt, TIME_BYTES).is_ok());
    ASSERT_EQ(h->call("waveInGetPosition", {w, kMmt, 12}), 0u);
    EXPECT_EQ(h->u32_at(kMmt + 4), 6000u);
    ASSERT_TRUE(h->memory.write_u32(kMmt, TIME_SAMPLES).is_ok());
    ASSERT_EQ(h->call("waveInGetPosition", {w, kMmt, 12}), 0u);
    EXPECT_EQ(h->u32_at(kMmt), TIME_SAMPLES);
    EXPECT_EQ(h->u32_at(kMmt + 4), 3000u);
    for (const u32 type : {TIME_MS, TIME_TICKS, 0x77u}) {
        ASSERT_TRUE(h->memory.write_u32(kMmt, type).is_ok());
        ASSERT_EQ(h->call("waveInGetPosition", {w, kMmt, 12}), 0u);
        EXPECT_EQ(h->u32_at(kMmt), TIME_BYTES);
    }
    EXPECT_EQ(h->call("waveInGetPosition", {w, kMmt, 4}), 0u);
    EXPECT_EQ(h->call("waveInGetPosition", {w, kMmt, 0}), 0u);
    EXPECT_EQ(h->call("waveInGetPosition", {w, 0, 12}), 11u);
    // Stop: "partial-returned=1 recorded-partial=1 empty-still-queued=1",
    // "flags partial=3 empty=12"; "Stop again r=0".
    h->make_header(kHdr3, kBuf3, 3200);
    ASSERT_EQ(h->call("waveInPrepareHeader", {w, kHdr3, hs}), 0u);
    ASSERT_EQ(h->call("waveInAddBuffer", {w, kHdr3, hs}), 0u);
    ASSERT_EQ(h->call("waveInStop", {w}), 0u);
    EXPECT_EQ(stream->stops, 1);
    ASSERT_TRUE(wait_for([&] { return count(WIM_DATA) == 2; }));
    EXPECT_EQ(flags_of(kHdr2), WHDR_PREPARED | WHDR_DONE);
    EXPECT_EQ(recorded(kHdr2), 2800u);
    EXPECT_EQ(flags_of(kHdr3), WHDR_PREPARED | WHDR_INQUEUE);
    EXPECT_EQ(h->call("waveInStop", {w}), 0u);
    stream->feed_frames(100, 0x33);  // a stopped device delivers nothing
    EXPECT_EQ(recorded(kHdr3), 0u);
    // "Close while queued r=33"; Reset: "returned=1 recorded=0 flags=3
    // position-zero=1"; "Reset again r=0".
    EXPECT_EQ(h->call("waveInClose", {w}), 33u);
    ASSERT_EQ(h->call("waveInReset", {w}), 0u);
    ASSERT_TRUE(wait_for([&] { return count(WIM_DATA) == 3; }));
    EXPECT_EQ(flags_of(kHdr3), WHDR_PREPARED | WHDR_DONE);
    EXPECT_EQ(recorded(kHdr3), 0u);
    ASSERT_TRUE(h->memory.write_u32(kMmt, TIME_BYTES).is_ok());
    ASSERT_EQ(h->call("waveInGetPosition", {w, kMmt, 12}), 0u);
    EXPECT_EQ(h->u32_at(kMmt + 4), 0u);
    EXPECT_EQ(h->call("waveInReset", {w}), 0u);
    // Unprepare: "r=0 flags=1" (DONE stays), "unprepared r=0", "cb small r=11".
    ASSERT_EQ(h->call("waveInUnprepareHeader", {w, kHdr, hs}), 0u);
    EXPECT_EQ(flags_of(kHdr), WHDR_DONE);
    EXPECT_EQ(h->call("waveInUnprepareHeader", {w, kHdr, hs}), 0u);
    EXPECT_EQ(h->call("waveInUnprepareHeader", {w, kHdr2, hs - 1}), 11u);
    // Zero-length buffer: "AddBuffer r=0 flags=12", "after Reset flags=3".
    h->make_header(kHdr, kBuf, 0);
    ASSERT_EQ(h->call("waveInPrepareHeader", {w, kHdr, hs}), 0u);
    ASSERT_EQ(h->call("waveInAddBuffer", {w, kHdr, hs}), 0u);
    EXPECT_EQ(flags_of(kHdr), WHDR_PREPARED | WHDR_INQUEUE);
    ASSERT_EQ(h->call("waveInReset", {w}), 0u);
    ASSERT_TRUE(wait_for([&] { return flags_of(kHdr) == (WHDR_PREPARED | WHDR_DONE); }));
    // lpData NULL at prepare time is not checked ("PrepareHeader(lpData NULL) r=0").
    h->make_header(kHdr2, 0, 3200);
    EXPECT_EQ(h->call("waveInPrepareHeader", {w, kHdr2, hs}), 0u);
    // A length that is not whole frames is filled to the byte ("recorded=3201").
    h->make_header(kHdr3, kBuf3, 3201);
    ASSERT_EQ(h->call("waveInPrepareHeader", {w, kHdr3, hs}), 0u);
    ASSERT_EQ(h->call("waveInAddBuffer", {w, kHdr3, hs}), 0u);
    ASSERT_EQ(h->call("waveInStart", {w}), 0u);
    stream->feed_frames(1601, 0x44);
    ASSERT_TRUE(wait_for([&] { return flags_of(kHdr3) == (WHDR_PREPARED | WHDR_DONE); }));
    EXPECT_EQ(recorded(kHdr3), 3201u);
    // "Start(no buffers) r=0": data is thrown away; "Reset(running) r=0".
    stream->feed_frames(100, 0x55);
    EXPECT_EQ(h->call("waveInReset", {w}), 0u);
    // "Close r=0 WIM_CLOSE-before-return=1", "Close again r=5", "Start(closed) r=5",
    // "Message(handle, ...) r=2".
    const usize before = log.size();
    ASSERT_EQ(h->call("waveInClose", {w}), 0u);
    ASSERT_GT(log.size(), before);
    EXPECT_EQ(log.calls.back().message, WIM_CLOSE);
    EXPECT_EQ(h->call("waveInClose", {w}), 5u);
    EXPECT_EQ(h->call("waveInStart", {w}), 5u);
    EXPECT_EQ(h->call("waveInMessage", {w, 0x812, kOut, 0}), 2u);
}

TEST_P(WinmmWaveInTest, MicrophonePermissionGatesOpen) {
    // Unmeasured on Windows: a denied microphone fails waveInOpen with a
    // documented waveInOpen error (MMSYSERR_NODRIVER) and *phwi = NULL.
    h->make_pcm_format(kFmt, 1, 1, 16000, 16);
    capture.state = audio::CapturePermission::Denied;
    set_phwi(0x1234);
    EXPECT_EQ(h->call("waveInOpen", {kPhwi, 0, kFmt, 0, 0, 0}), 6u);
    EXPECT_EQ(phwi(), 0u);
    EXPECT_EQ(capture.opens, 0);
    // A format query does not touch the device.
    EXPECT_EQ(h->call("waveInOpen", {kPhwi, 0, kFmt, 0, 0, WAVE_FORMAT_QUERY}), 0u);
    capture.state = audio::CapturePermission::Undetermined;
    capture.answer = audio::CapturePermission::Granted;
    EXPECT_EQ(h->call("waveInOpen", {kPhwi, 0, kFmt, 0, 0, 0}), 0u);
    EXPECT_EQ(capture.requests, 1);
    EXPECT_EQ(h->call("waveInClose", {phwi()}), 0u);
}

TEST_P(WinmmWaveInTest, ChannelsFollowTheInputDevice) {
    // WAVEINCAPS.wChannels is the input's own channel count; a format of
    // other channels opens only through an exact conversion (mono<->stereo),
    // anything else is WAVERR_BADFORMAT (UNIMPLEMENTED: no speaker matrix),
    // for a query too.
    capture.list.push_back(audio::CaptureDevice{"uid-quad", u"Quad Array", 48000, 4, 0x33u});
    ASSERT_EQ(h->call("waveInGetNumDevs", {}), 3u);
    ASSERT_EQ(h->call("waveInGetDevCapsW", {2, kCaps, 80}), 0u);
    EXPECT_EQ(h->memory.read_u16(kCaps + 76).value(), 4u);
    h->make_pcm_format(kFmt, 1, 1, 16000, 16);
    set_phwi(0x1234);
    EXPECT_EQ(h->call("waveInOpen", {kPhwi, 2, kFmt, 0, 0, WAVE_FORMAT_QUERY}), 32u);
    EXPECT_EQ(h->call("waveInOpen", {kPhwi, 2, kFmt, 0, 0, 0}), 32u);
    EXPECT_EQ(phwi(), 0u);
    // The mono USB input (id 1) records a stereo format.
    h->make_pcm_format(kFmt, 1, 2, 44100, 16);
    ASSERT_EQ(h->call("waveInOpen", {kPhwi, 1, kFmt, 0, 0, 0}), 0u);
    ASSERT_NE(capture.last, nullptr);
    EXPECT_EQ(capture.last->format.channels, 2u);
    EXPECT_EQ(capture.last->device, "uid-usb");
    EXPECT_EQ(h->call("waveInClose", {phwi()}), 0u);
}
