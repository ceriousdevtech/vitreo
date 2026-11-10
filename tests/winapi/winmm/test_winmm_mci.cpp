/// @file test_winmm_mci.cpp
/// @brief MCI cdaudio against Windows' recorded answers, and mixed-mode
///        playback.
///
/// tests/oracle_data/mci_cdaudio_probe_{32,64}{a,b}.expected are Windows'
/// cdaudio driver over mounted ISOs of 1491 (a) and 4420 (b) sectors on
/// drive F: (tools/oracle_probe/mci_cdaudio_probe.c). The same discs are
/// synthesized here, mounted on F:, and every string and command-block row
/// is replayed. Rows where Vitreo deliberately differs are listed in
/// kStringDeviations / the command checks with the reason.

#include <gtest/gtest.h>

#include "../../capsule/cd_image_fixtures.hpp"
#include "capsule/vfs/iso_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/media/winmm/winmm_mci.hpp"
#include "windowing/contracts/wave_output.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

using namespace vitreo;
using namespace vitreo::winapi;

namespace mcic {
#include "winapi/media/winmm/mci_constants.inc"
}
using namespace mcic;

namespace {

std::vector<std::vector<std::string>> rows(const std::string& file) {
    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + file);
    EXPECT_TRUE(in.good()) << file;
    std::vector<std::vector<std::string>> out;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        usize start = 0;
        for (;;) {
            const auto tab = line.find('\t', start);
            f.push_back(line.substr(start, tab - start));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        out.push_back(std::move(f));
    }
    return out;
}

/// Completes every submitted buffer at once and keeps the PCM.
class RecordingOutput final : public windowing::WaveOutput {
public:
    explicit RecordingOutput(bool auto_complete, std::vector<u8>* sink)
        : auto_complete_(auto_complete), sink_(sink) {}
    u32 device_count() const noexcept override { return 1; }
    bool supports(const windowing::WaveOutputFormat&) const noexcept override { return true; }
    bool open(const windowing::WaveOutputFormat& f, CompletionCallback cb) override {
        format_ = f;
        cb_ = std::move(cb);
        open_ = true;
        return true;
    }
    bool is_open() const noexcept override { return open_; }
    void close() override { open_ = false; }
    bool submit(u64 token, std::span<const Byte> pcm) override {
        if (sink_) sink_->insert(sink_->end(), pcm.begin(), pcm.end());
        if (auto_complete_) cb_(token, static_cast<u32>(pcm.size()));
        return true;
    }
    void reset() override {}
    void set_paused(bool) override {}
    bool set_volume(float) override { return true; }
    windowing::WaveOutputFormat format_{};

private:
    bool auto_complete_;
    std::vector<u8>* sink_;
    CompletionCallback cb_;
    bool open_ = false;
};

struct Rig {
    explicit Rig(const std::filesystem::path& image) {
        auto fs = std::make_unique<capsule::IsoFileSystem>(image.string());
        EXPECT_TRUE(fs->valid()) << fs->error_message();
        EXPECT_TRUE(vfs.mount_drive('F', std::move(fs)).is_ok());
        factory = [this]() -> std::unique_ptr<windowing::WaveOutput> {
            auto out = std::make_unique<RecordingOutput>(auto_complete, &pcm);
            outputs.push_back(out.get());
            return out;
        };
        notifier = [this](u64 hwnd, u32 msg, u64 wp, u64 lp) {
            std::lock_guard g(notice_lock);
            notices.push_back({hwnd, msg, wp, lp});
            notice_cv.notify_all();
            return true;
        };
        mci = std::make_unique<MciManager>(vfs, factory, notifier);
    }
    std::string str(const std::string& cmd, u32& error, u64 hwnd = 0) {
        std::string result;
        error = mci->send_string(cmd, result, hwnd);
        return result;
    }
    capsule::Vfs vfs;
    MciManager::OutputFactory factory;
    MciManager::WindowNotifier notifier;
    bool auto_complete = true;
    std::vector<u8> pcm;
    std::vector<RecordingOutput*> outputs;
    struct Notice { u64 hwnd; u32 msg; u64 wparam; u64 lparam; };
    std::mutex notice_lock;
    std::condition_variable notice_cv;
    std::vector<Notice> notices;
    std::unique_ptr<MciManager> mci;
};

std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "%x", v);
    return b;
}

/// Rows where Vitreo's cdaudio differs on purpose: command -> (error, text).
///  - play/resume on a data-only disc: Windows' virtual ISO drive accepts
///    them and jumps to the end; a real drive (the oracle VM's emulated ATAPI
///    drive: MCIERR_HARDWARE) refuses to play audio from a data track, which
///    is the drive Vitreo models; the mode then stays as it was.
///  - other MCI device types: no driver in Vitreo (MCIERR_CANNOT_LOAD_DRIVER),
///    so "sysinfo all quantity" counts cdaudio only.
const std::map<std::string, std::pair<std::string, std::string>> kStringDeviations = {
    {"play cd", {"106", ""}},
    {"resume cd", {"106", ""}},
    {"open waveaudio alias w wait", {"10a", ""}},
    {"open sequencer alias s wait", {"10a", ""}},
    {"open avivideo alias v wait", {"10a", ""}},
    {"sysinfo all quantity", {"0", "1"}},
};

}  // namespace

TEST(WinmmMci, StringCommandsMatchWindowsOnBothRecordedDiscs) {
    for (const auto& [file, sectors] : std::vector<std::pair<std::string, u32>>{
             {"mci_cdaudio_probe_32a.expected", 1491}, {"mci_cdaudio_probe_32b.expected", 4420},
             {"mci_cdaudio_probe_64a.expected", 1491}, {"mci_cdaudio_probe_64b.expected", 4420}}) {
        SCOPED_TRACE(file);
        const auto iso = test_cd::make_iso("vitreo_mci_" + std::to_string(sectors) + ".iso", sectors);
        Rig rig(iso);
        usize replayed = 0;
        bool after_play = false;
        for (const auto& row : rows(file)) {
            if (row.size() < 4 || row[0] != "str") continue;
            const std::string& cmd = row[1];
            std::string want_error = row[2];
            std::string want_text = row[3].substr(1, row[3].size() - 2);
            if (auto it = kStringDeviations.find(cmd); it != kStringDeviations.end()) {
                want_error = it->second.first;
                want_text = it->second.second;
            }
            // After the refused "play", Windows' mode and position rows
            // describe its jump to the end: Vitreo is still where it was.
            if (cmd == "play cd") after_play = true;
            if (after_play && (cmd == "status cd mode" || cmd == "status cd position")) {
                after_play = cmd != "status cd position";
                continue;
            }
            if (cmd == "status cd mode" && row[3] == "[playing]") continue;  // after resume
            u32 error = 0;
            const std::string got = rig.str(cmd, error);
            EXPECT_EQ(hex(error), want_error) << cmd;
            EXPECT_EQ(got, want_text) << cmd;
            ++replayed;
        }
        EXPECT_GT(replayed, 80u);
        std::filesystem::remove(iso);
    }
}

TEST(WinmmMci, ErrorStringsMatchWindows) {
    usize n = 0;
    for (const auto& row : rows("mci_errtext_probe32.expected")) {
        if (row.size() != 2) continue;
        char* end = nullptr;
        const u32 code = static_cast<u32>(std::strtoul(row[0].c_str(), &end, 16));
        if (end == nullptr || *end != '\0') continue;
        const char* text = MciManager::error_text(code);
        ASSERT_NE(text, nullptr) << row[0];
        EXPECT_EQ(std::string(text), row[1]);
        ++n;
    }
    EXPECT_EQ(n, 108u);
    EXPECT_EQ(MciManager::error_text(0x9999), nullptr);
}

// ---------------------------------------------------------------------------
// mciSendCommand parameter blocks, both bitnesses
// ---------------------------------------------------------------------------

namespace {

struct BlockGuest {
    explicit BlockGuest(bool x64_abi) : x64(x64_abi) {
        EXPECT_TRUE(memory.map_region(0x10000, 0x20000, MemoryProtection::ReadWrite).is_ok());
    }
    u32 send(MciManager& mci, u32 id, u32 msg, u64 flags, GuestAddr parms) {
        cpu = cpu::CpuState{};
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x2f000;
        const u64 args[4] = {id, msg, flags, parms};
        if (x64) {
            cpu.gpr.rcx = args[0]; cpu.gpr.rdx = args[1]; cpu.gpr.r8 = args[2]; cpu.gpr.r9 = args[3];
        } else {
            for (u32 i = 0; i < 4; ++i) (void)memory.write_u32(0x2f004 + 4 * i, static_cast<u32>(args[i]));
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        mci.impl_mciSendCommand(m, false);
        return static_cast<u32>(cpu.gpr.rax);
    }
    void fill(GuestAddr at, u32 n) { for (u32 i = 0; i < n; ++i) (void)memory.write_u8(at + i, 0xcc); }
    void ptr(GuestAddr at, u64 v) { (void)(x64 ? memory.write_u64(at, v) : memory.write_u32(at, static_cast<u32>(v))); }
    std::string dump(GuestAddr at, u32 n) {
        std::string s;
        for (u32 i = 0; i < n; ++i) {
            char b[3];
            std::snprintf(b, sizeof b, "%02x", memory.read_u8(at + i).value());
            s += b;
        }
        return s;
    }
    bool x64;
    GuestMemory memory;
    cpu::CpuState cpu;
};

}  // namespace

TEST(WinmmMci, CommandBlocksMatchWindowsBothBitnesses) {
    for (const bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        const std::string file = x64 ? "mci_cdaudio_probe_64a.expected" : "mci_cdaudio_probe_32a.expected";
        std::vector<std::vector<std::string>> cmd_rows;
        for (auto& r : rows(file)) {
            if (!r.empty() && r[0] == "cmd") cmd_rows.push_back(r);
        }
        const auto iso = test_cd::make_iso("vitreo_mci_blocks.iso", 1491);
        Rig rig(iso);
        BlockGuest g(x64);
        usize next = 0;
        const auto check = [&](const char* label, u32 err, GuestAddr at, u32 n) {
            ASSERT_LT(next, cmd_rows.size());
            const auto& r = cmd_rows[next++];
            EXPECT_EQ(r[1], label);
            EXPECT_EQ(hex(err), r[2]) << label;
            EXPECT_EQ(g.dump(at, n), r[3]) << label;
        };
        const u32 ps = x64 ? 8 : 4;
        constexpr GuestAddr P = 0x11000, S = 0x12000;
        // open_typeid_element
        const u32 open_size = x64 ? 36 : 20;
        g.fill(P, open_size);
        g.ptr(P, 0);
        g.ptr(P + (x64 ? 12 : 8), MCI_DEVTYPE_CD_AUDIO);
        (void)g.memory.write_u8(S, 'F'); (void)g.memory.write_u8(S + 1, ':'); (void)g.memory.write_u8(S + 2, 0);
        g.ptr(P + (x64 ? 20 : 12), S);
        g.ptr(P + (x64 ? 28 : 16), 0);
        u32 e = g.send(*rig.mci, 0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_TYPE_ID | MCI_OPEN_ELEMENT | MCI_WAIT, P);
        // The element pointer differs between processes; compare the rest.
        ASSERT_LT(next, cmd_rows.size());
        {
            const auto& r = cmd_rows[next++];
            EXPECT_EQ(hex(e), r[2]);
            const std::string d = g.dump(P, open_size);
            const usize elem = (x64 ? 20 : 12) * 2;
            EXPECT_EQ(d.substr(0, elem), r[3].substr(0, elem));
            EXPECT_EQ(d.substr(elem + ps * 2), r[3].substr(elem + ps * 2));
        }
        const u32 id = g.memory.read_u32(P + (x64 ? 8 : 4)).value();
        EXPECT_EQ(id, 1u);
        EXPECT_EQ(rig.mci->device_id_for_name("cdaudio"), 0u);
        EXPECT_EQ(rig.mci->device_id_for_name("F:"), 1u);

        const u32 set_size = x64 ? 16 : 12, st_size = x64 ? 24 : 16;
        for (const u32 fmt : {MCI_FORMAT_MILLISECONDS, MCI_FORMAT_MSF, MCI_FORMAT_TMSF, MCI_FORMAT_BYTES,
                              MCI_FORMAT_FRAMES, MCI_FORMAT_SAMPLES}) {
            g.fill(P, set_size); g.ptr(P, 0); (void)g.memory.write_u32(P + ps, fmt);
            check("set_time_format", g.send(*rig.mci, id, MCI_SET, MCI_SET_TIME_FORMAT | MCI_WAIT, P), P, set_size);
            const auto status = [&](const char* label, u32 item, u64 flags, u32 track, bool with_track) {
                g.fill(P, st_size); g.ptr(P, 0);
                (void)g.memory.write_u32(P + 2 * ps, item);
                if (with_track) (void)g.memory.write_u32(P + 2 * ps + 4, track);
                check(label, g.send(*rig.mci, id, MCI_STATUS, flags, P), P, st_size);
            };
            status("status_length", MCI_STATUS_LENGTH, MCI_STATUS_ITEM | MCI_WAIT, 0, false);
            status("status_length_track1", MCI_STATUS_LENGTH, MCI_STATUS_ITEM | MCI_TRACK | MCI_WAIT, 1, true);
            status("status_position_track1", MCI_STATUS_POSITION, MCI_STATUS_ITEM | MCI_TRACK | MCI_WAIT, 1, true);
            status("status_position", MCI_STATUS_POSITION, MCI_STATUS_ITEM | MCI_WAIT, 0, false);
            status("status_start", MCI_STATUS_POSITION, MCI_STATUS_ITEM | MCI_STATUS_START | MCI_WAIT, 0, false);
        }
        for (const u32 item : {MCI_STATUS_NUMBER_OF_TRACKS, MCI_STATUS_MODE, MCI_STATUS_MEDIA_PRESENT,
                               MCI_STATUS_READY, MCI_STATUS_CURRENT_TRACK, MCI_STATUS_TIME_FORMAT,
                               MCI_CDA_STATUS_TYPE_TRACK, 0x12345u}) {
            g.fill(P, st_size); g.ptr(P, 0);
            (void)g.memory.write_u32(P + 2 * ps, item);
            (void)g.memory.write_u32(P + 2 * ps + 4, 1);
            check("status_item", g.send(*rig.mci, id, MCI_STATUS,
                  MCI_STATUS_ITEM | (item == MCI_CDA_STATUS_TYPE_TRACK ? MCI_TRACK : 0) | MCI_WAIT, P), P, st_size);
        }
        g.fill(P, st_size); g.ptr(P, 0); (void)g.memory.write_u32(P + 2 * ps, MCI_CDA_STATUS_TYPE_TRACK);
        check("status_type_notrack", g.send(*rig.mci, id, MCI_STATUS, MCI_STATUS_ITEM | MCI_WAIT, P), P, st_size);
        g.fill(P, st_size); g.ptr(P, 0); (void)g.memory.write_u32(P + 2 * ps, MCI_STATUS_LENGTH);
        (void)g.memory.write_u32(P + 2 * ps + 4, 7);
        check("status_length_track7", g.send(*rig.mci, id, MCI_STATUS, MCI_STATUS_ITEM | MCI_TRACK | MCI_WAIT, P), P, st_size);
        g.fill(P, st_size); g.ptr(P, 0); (void)g.memory.write_u32(P + 2 * ps, MCI_STATUS_LENGTH);
        check("status_noitemflag", g.send(*rig.mci, id, MCI_STATUS, MCI_WAIT, P), P, st_size);

        const u32 caps_size = x64 ? 16 : 12;
        for (const u32 item : {MCI_GETDEVCAPS_CAN_RECORD, MCI_GETDEVCAPS_HAS_AUDIO, MCI_GETDEVCAPS_HAS_VIDEO,
                               MCI_GETDEVCAPS_DEVICE_TYPE, MCI_GETDEVCAPS_USES_FILES,
                               MCI_GETDEVCAPS_COMPOUND_DEVICE, MCI_GETDEVCAPS_CAN_EJECT,
                               MCI_GETDEVCAPS_CAN_PLAY, MCI_GETDEVCAPS_CAN_SAVE, 0x777u}) {
            g.fill(P, caps_size); g.ptr(P, 0); (void)g.memory.write_u32(P + ps + 4, item);
            check("devcaps", g.send(*rig.mci, id, MCI_GETDEVCAPS, MCI_GETDEVCAPS_ITEM | MCI_WAIT, P), P, caps_size);
        }
        // MCI_INFO rows ("info\t<flag>\t<err>\t[text]").
        std::vector<std::vector<std::string>> info_rows;
        for (auto& r : rows(file)) if (!r.empty() && r[0] == "info") info_rows.push_back(r);
        usize k = 0;
        for (const u32 flag : {MCI_INFO_PRODUCT, MCI_INFO_MEDIA_IDENTITY, MCI_INFO_MEDIA_UPC, MCI_INFO_FILE}) {
            g.fill(S + 0x100, 128);
            g.fill(P, 0x20); g.ptr(P, 0); g.ptr(P + ps, S + 0x100); (void)g.memory.write_u32(P + 2 * ps, 128);
            const u32 err = g.send(*rig.mci, id, MCI_INFO, flag | MCI_WAIT, P);
            ASSERT_LT(k, info_rows.size());
            EXPECT_EQ(hex(err), info_rows[k][2]) << flag;
            if (err == 0) {
                std::string text;
                for (u32 i = 0; i < 128; ++i) {
                    const u8 c = g.memory.read_u8(S + 0x100 + i).value();
                    if (c == 0) break;
                    text.push_back(static_cast<char>(c));
                }
                EXPECT_EQ("[" + text + "]", info_rows[k][3]);
            }
            ++k;
        }
        // play on a data-only disc: Vitreo models a drive that refuses
        // (MCIERR_HARDWARE); Windows' ISO-mount drive accepted it.
        ASSERT_EQ(cmd_rows[next][1], "play");
        next += 2;  // play, mode_while_playing
        g.fill(P, 12); g.ptr(P, 0);
        EXPECT_EQ(g.send(*rig.mci, id, MCI_PLAY, 0, P), MCIERR_HARDWARE);
        const u32 seek_size = x64 ? 12 : 8, gen_size = x64 ? 8 : 4;
        g.fill(P, seek_size); g.ptr(P, 0);
        check("seek_start", g.send(*rig.mci, id, MCI_SEEK, MCI_SEEK_TO_START | MCI_WAIT, P), P, seek_size);
        check("seek_end", g.send(*rig.mci, id, MCI_SEEK, MCI_SEEK_TO_END | MCI_WAIT, P), P, seek_size);
        g.fill(P, gen_size); g.ptr(P, 0);
        check("stop", g.send(*rig.mci, id, MCI_STOP, MCI_WAIT, P), P, gen_size);
        check("pause", g.send(*rig.mci, id, MCI_PAUSE, MCI_WAIT, P), P, gen_size);
        check("bogus_msg", g.send(*rig.mci, id, 0x0899, MCI_WAIT, P), P, gen_size);
        check("close", g.send(*rig.mci, id, MCI_CLOSE, MCI_WAIT, P), P, gen_size);
        check("close_again", g.send(*rig.mci, id, MCI_CLOSE, MCI_WAIT, P), P, gen_size);
        // Unknown device id: MCIERR_UNSUPPORTED_FUNCTION, no dwReturn.
        g.fill(P, st_size); g.ptr(P, 0);
        EXPECT_EQ(g.send(*rig.mci, 0x4242, MCI_STATUS, MCI_STATUS_ITEM | MCI_WAIT, P), MCIERR_UNSUPPORTED_FUNCTION);
        // NULL parameter block: documented MCIERR_NULL_PARAMETER_BLOCK (Windows
        // itself faults here).
        EXPECT_EQ(g.send(*rig.mci, 0, MCI_OPEN, MCI_OPEN_TYPE | MCI_WAIT, 0), MCIERR_NULL_PARAMETER_BLOCK);
        // Device types without a Vitreo driver fail to load.
        g.fill(P, open_size); g.ptr(P, 0); g.ptr(P + (x64 ? 12 : 8), MCI_DEVTYPE_WAVEFORM_AUDIO);
        EXPECT_EQ(g.send(*rig.mci, 0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_TYPE_ID | MCI_WAIT, P),
                  MCIERR_CANNOT_LOAD_DRIVER);
        std::filesystem::remove(iso);
    }
}

// ---------------------------------------------------------------------------
// Mixed-mode disc: TOC, playback, notification
// ---------------------------------------------------------------------------

TEST(WinmmMci, MixedModeDiscReportsItsTocAndPlaysTheAudioTrack) {
    test_cd::MixedLayout layout;
    const auto mdf = test_cd::make_mixed_mode_mdf("vitreo_mci_mixed.mdf", layout);
    Rig rig(mdf);
    const u32 audio_start = layout.data_frames + 1 + layout.omitted;
    u32 e = 0;
    EXPECT_EQ(rig.str("open F: type cdaudio alias cd wait", e), "1");
    EXPECT_EQ(e, 0u);
    EXPECT_EQ(rig.str("status cd number of tracks", e), "2");
    EXPECT_EQ(rig.str("status cd type track 1", e), "other");
    EXPECT_EQ(rig.str("status cd type track 2", e), "audio");
    EXPECT_EQ(rig.str("status cd mode", e), "stopped");  // a disc with audio is ready
    EXPECT_EQ(rig.str("status cd ready", e), "true");
    const auto msf = [](u32 frames) {
        char b[16];
        std::snprintf(b, sizeof b, "%02u:%02u:%02u", frames / 4500, (frames / 75) % 60, frames % 75);
        return std::string(b);
    };
    EXPECT_EQ(rig.str("status cd position track 2", e), msf(audio_start + 150));
    EXPECT_EQ(rig.str("status cd length track 1", e), msf(audio_start));
    EXPECT_EQ(rig.str("status cd length track 2", e), msf(layout.audio_frames - 2));
    EXPECT_EQ(rig.str("status cd length", e), msf(audio_start + layout.audio_frames - 2));
    EXPECT_EQ(rig.str("set cd time format tmsf", e), "");
    EXPECT_EQ(rig.str("status cd position track 2", e), "02:00:00:00");
    // Audio cannot start inside the data track.
    rig.str("play cd from 1", e);
    EXPECT_EQ(e, MCIERR_HARDWARE);
    // Track 3 does not exist.
    rig.str("play cd from 2 to 3", e);
    EXPECT_EQ(e, MCIERR_OUTOFRANGE);

    // Play track 2 to the end and wait: every frame reaches the output.
    rig.str("play cd from 2 wait", e);
    ASSERT_EQ(e, 0u);
    ASSERT_EQ(rig.outputs.size(), 1u);
    EXPECT_EQ(rig.outputs[0]->format_.samples_per_second, 44100u);
    EXPECT_EQ(rig.outputs[0]->format_.channels, 2u);
    EXPECT_EQ(rig.outputs[0]->format_.bits_per_sample, 16u);
    ASSERT_EQ(rig.pcm.size(), static_cast<usize>(layout.audio_frames) * 2352);
    for (const u32 frame : {0u, 17u, layout.audio_frames - 1}) {
        const usize at = static_cast<usize>(frame) * 2352;
        EXPECT_EQ(rig.pcm[at] | (rig.pcm[at + 1] << 8), test_cd::audio_sample(frame, 0));
    }
    EXPECT_EQ(rig.str("status cd mode", e), "stopped");

    // A muted left channel plays zeros on the left only.
    rig.pcm.clear();
    rig.str("set cd audio left off", e);
    ASSERT_EQ(e, 0u);
    rig.str("play cd from 2:00:00:00 to 2:00:00:10 wait", e);
    ASSERT_EQ(e, 0u);
    ASSERT_EQ(rig.pcm.size(), 10u * 2352);
    EXPECT_EQ(rig.pcm[0] | (rig.pcm[1] << 8), 0);
    EXPECT_EQ(rig.pcm[2] | (rig.pcm[3] << 8), test_cd::audio_sample(0, 1));
    rig.str("set cd audio left on", e);

    // Notify: MM_MCINOTIFY with MCI_NOTIFY_SUCCESSFUL when playback ends.
    rig.notices.clear();
    rig.str("play cd from 2 to 2:00:00:20 notify", e, 0x1234);
    ASSERT_EQ(e, 0u);
    {
        std::unique_lock l(rig.notice_lock);
        ASSERT_TRUE(rig.notice_cv.wait_for(l, std::chrono::seconds(5), [&] { return !rig.notices.empty(); }));
        EXPECT_EQ(rig.notices[0].hwnd, 0x1234u);
        EXPECT_EQ(rig.notices[0].msg, MM_MCINOTIFY);
        EXPECT_EQ(rig.notices[0].wparam, MCI_NOTIFY_SUCCESSFUL);
        EXPECT_EQ(rig.notices[0].lparam, 1u);
    }
    rig.str("close cd", e);
    EXPECT_EQ(e, 0u);
    std::filesystem::remove(mdf);
}

TEST(WinmmMci, StoppingPlaybackAbortsItsNotification) {
    test_cd::MixedLayout layout;
    const auto mdf = test_cd::make_mixed_mode_mdf("vitreo_mci_stop.mdf", layout);
    Rig rig(mdf);
    rig.auto_complete = false;  // the stream never drains: playback stays on
    u32 e = 0;
    rig.str("open F: type cdaudio alias cd", e);
    rig.str("set cd time format tmsf", e);
    rig.str("play cd from 2 notify", e, 0x77);
    ASSERT_EQ(e, 0u);
    EXPECT_EQ(rig.str("status cd mode", e), "playing");
    EXPECT_EQ(rig.str("status cd current track", e), "2");
    rig.str("pause cd", e);
    EXPECT_EQ(rig.str("status cd mode", e), "paused");
    ASSERT_EQ(rig.notices.size(), 1u);
    EXPECT_EQ(rig.notices[0].wparam, MCI_NOTIFY_ABORTED);
    rig.str("resume cd", e);
    EXPECT_EQ(e, 0u);
    EXPECT_EQ(rig.str("status cd mode", e), "playing");
    rig.str("stop cd", e);
    EXPECT_EQ(rig.str("status cd mode", e), "stopped");
    rig.str("close cd", e);
    std::filesystem::remove(mdf);
}

TEST(WinmmMci, OtherDeviceTypesFailToLoadInsteadOfFakingSuccess) {
    const auto iso = test_cd::make_iso("vitreo_mci_types.iso", 300);
    Rig rig(iso);
    u32 e = 0;
    for (const char* cmd : {"open waveaudio", "open sequencer", "open avivideo alias v",
                            "open c:\\sound.wav", "open song.mid"}) {
        rig.str(cmd, e);
        EXPECT_EQ(e, MCIERR_CANNOT_LOAD_DRIVER) << cmd;
    }
    rig.str("open bogusdevice", e);
    EXPECT_EQ(e, MCIERR_INVALID_DEVICE_NAME);
    rig.str("status waveaudio mode", e);
    EXPECT_EQ(e, MCIERR_CANNOT_LOAD_DRIVER);
    // An unopened cdaudio is opened for one command and closed again.
    EXPECT_EQ(rig.str("status cdaudio number of tracks", e), "1");
    EXPECT_EQ(e, 0u);
    EXPECT_EQ(rig.mci->device_id_for_name("cdaudio"), 0u);
    rig.str("status cdaudio mode notify", e, 0x55);
    EXPECT_EQ(e, MCIERR_NOTIFY_ON_AUTO_OPEN);
    std::filesystem::remove(iso);
}
