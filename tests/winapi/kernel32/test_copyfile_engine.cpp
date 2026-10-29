/// @file test_copyfile_engine.cpp
/// @brief CopyFile2 / CopyFileExW / CopyFileW through the shared copy engine.
///
/// Every expected value comes from tools/oracle_probe/s4_copyfile_probe.expected
/// (Windows 11, identical on x86 and x64); the probe line is cited per check.
/// The guest progress routine is simulated by the guest-call bridge, which
/// reads each COPYFILE2_MESSAGE out of guest memory the way the guest would.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <capsule/vfs/vfs.hpp>
#include <capsule/vfs/host_filesystem.hpp>

#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kStack = 0x80000;
constexpr GuestAddr kSrcStr = 0x2000;
constexpr GuestAddr kDstStr = 0x2400;
constexpr GuestAddr kParams = 0x3000;
constexpr GuestAddr kCancel = 0x3400;
constexpr GuestAddr kRoutine = 0x7000'1000;  // never executed: bridged
constexpr u32 kSentinel = 0xDEADu;
constexpr u64 kMiB = 1024 * 1024;

struct Msg {
    u32 type = 0;
    u32 stream = 0;
    u64 hs = 0, hd = 0;
    u64 chunk = 0, csize = 0, ssize = 0, sdone = 0, total = 0, tdone = 0;
    u32 phase = 0, hr = 0;
    u64 ctx = 0;
};

class CopyFileEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/vitreo_copyengine_XXXXXX";
        const char* d = ::mkdtemp(tmpl);
        ASSERT_NE(d, nullptr);
        root = d;
        ASSERT_TRUE(memory.map_region(0x1000, 0x100000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
        files = std::make_unique<FileManager>(vfs, handles, memory);
        // RuntimeContext routes CloseHandle of a file to FileManager.
        handles.set_close_callback([this](Handle h, HandleType type, void* object) {
            return files->on_handle_close(h, type, object);
        });
        mgr = std::make_unique<Kernel32ExtManager>(memory, vfs);
        mgr->set_file_manager(files.get());
        mgr->set_guest_call([this](GuestAddr fn, const u64* a, usize n, bool x64) -> u64 {
            EXPECT_EQ(fn, kRoutine);
            return on_callback(a, n, x64);
        });
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    void make_file(const std::string& name, u64 size) {
        std::ofstream f(root + "/" + name, std::ios::binary);
        std::string chunk(4096, 'x');
        for (u64 left = size; left != 0;) {
            const u64 n = std::min<u64>(left, chunk.size());
            f.write(chunk.data(), static_cast<std::streamsize>(n));
            left -= n;
        }
    }
    [[nodiscard]] i64 host_size(const std::string& name) const {
        std::error_code ec;
        const auto s = std::filesystem::file_size(root + "/" + name, ec);
        return ec ? -1 : static_cast<i64>(s);
    }
    void put_wide(GuestAddr at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) {
            (void)memory.write_u16(at + i * 2, static_cast<u16>(s[i]));
        }
        (void)memory.write_u16(at + s.size() * 2, 0);
    }

    /// Calls @p impl with @p args laid out as the x86 or x64 ABI would.
    u64 call(void (Kernel32ExtManager::*impl)(Marshaler&), std::vector<u64> args,
             bool x64) {
        cpu::CpuState cpu;
        if (x64) {
            cpu.init_long_mode();
            cpu.gpr.rsp = kStack;
            u64* regs[4] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) {
                    *regs[i] = args[i];
                } else {
                    (void)memory.write_u64(kStack + 0x28 + (i - 4) * 8, args[i]);
                }
            }
        } else {
            cpu.init_protected_mode();
            cpu.set_sp(kStack);
            (void)memory.write_u32(kStack, 0xDEADBEEF);
            for (usize i = 0; i < args.size(); ++i) {
                (void)memory.write_u32(kStack + 4 + static_cast<GuestAddr>(i) * 4,
                                       static_cast<u32>(args[i]));
            }
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        set_last_error(kSentinel);
        ((*mgr).*impl)(m);
        return x64 ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFu);
    }

    /// COPYFILE2_EXTENDED_PARAMETERS at kParams.
    void put_params(bool x64, u32 size, u32 flags, GuestAddr cancel, GuestAddr routine,
                    u64 ctx) {
        for (u32 i = 0; i < 128; i += 4) (void)memory.write_u32(kParams + i, 0);
        (void)memory.write_u32(kParams + 0, size);
        (void)memory.write_u32(kParams + 4, flags);
        if (x64) {
            (void)memory.write_u64(kParams + 8, cancel);
            (void)memory.write_u64(kParams + 16, routine);
            (void)memory.write_u64(kParams + 24, ctx);
        } else {
            (void)memory.write_u32(kParams + 8, static_cast<u32>(cancel));
            (void)memory.write_u32(kParams + 12, static_cast<u32>(routine));
            (void)memory.write_u32(kParams + 16, static_cast<u32>(ctx));
        }
    }

    u64 on_callback(const u64* a, usize n, bool x64) {
        const usize idx = seq.size();
        if (n == 2) {  // PCOPYFILE2_PROGRESS_ROUTINE(pMessage, pvCallbackContext)
            const GuestAddr p = static_cast<GuestAddr>(a[0]);
            Msg g;
            g.ctx = a[1];
            g.type = memory.read_u32(p).value_or(0);
            const GuestAddr info = p + 8;
            auto rd32 = [&](GuestAddr q) { return memory.read_u32(q).value_or(0); };
            auto rd64 = [&](GuestAddr q) { return memory.read_u64(q).value_or(0); };
            auto rdh = [&](GuestAddr q) { return x64 ? rd64(q) : rd32(q); };
            // winbase.h offsets; the 64-bit fields start at +32 on x64, +24 on x86.
            const GuestAddr q = info + (x64 ? 24 : 16);
            if (g.type == 6) {
                g.phase = rd32(info);
                g.stream = rd32(info + 4);
                g.hr = rd32(info + 8);
                g.ssize = rd64(info + 24);
                g.total = rd64(info + 40);
            } else {
                g.stream = rd32(info);
                g.hs = rdh(info + 8);
                g.hd = rdh(info + (x64 ? 16 : 12));
                if (g.type == 1) {
                    g.chunk = rd64(q); g.csize = rd64(q + 8);
                    g.ssize = rd64(q + 16); g.total = rd64(q + 24);
                } else if (g.type == 2) {
                    g.chunk = rd64(q); g.csize = rd64(q + 8); g.ssize = rd64(q + 16);
                    g.sdone = rd64(q + 24); g.total = rd64(q + 32); g.tdone = rd64(q + 40);
                } else if (g.type == 3) {
                    g.ssize = rd64(q); g.total = rd64(q + 8);
                } else if (g.type == 4) {
                    g.ssize = rd64(q); g.sdone = rd64(q + 8);
                    g.total = rd64(q + 16); g.tdone = rd64(q + 24);
                }
            }
            msgs.push_back(g);
            seq.push_back(static_cast<char>('0' + g.type));
        } else {  // LPPROGRESS_ROUTINE
            Msg g;
            if (x64) {
                EXPECT_EQ(n, 9u);
                g.total = a[0]; g.tdone = a[1]; g.ssize = a[2]; g.sdone = a[3];
                g.stream = static_cast<u32>(a[4]); g.type = static_cast<u32>(a[5]);
                g.hs = a[6]; g.hd = a[7]; g.ctx = a[8];
            } else {
                // Each LARGE_INTEGER is two stdcall slots.
                EXPECT_EQ(n, 13u);
                g.total = a[0] | (a[1] << 32); g.tdone = a[2] | (a[3] << 32);
                g.ssize = a[4] | (a[5] << 32); g.sdone = a[6] | (a[7] << 32);
                g.stream = static_cast<u32>(a[8]); g.type = static_cast<u32>(a[9]);
                g.hs = a[10]; g.hd = a[11]; g.ctx = a[12];
            }
            msgs.push_back(g);
            seq.push_back(static_cast<char>('0' + g.type));
        }
        if (reply_at_type >= 0 && static_cast<int>(msgs.back().type) == reply_at_type &&
            !replied) {
            replied = true;
            return reply;
        }
        (void)idx;
        return 0;
    }

    u64 copy_file2(bool x64) {
        put_wide(kSrcStr, src_name);
        put_wide(kDstStr, dst_name);
        return call(&Kernel32ExtManager::impl_CopyFile2, {kSrcStr, kDstStr, kParams}, x64);
    }
    u64 copy_file_ex(bool x64, GuestAddr routine, GuestAddr cancel, u32 flags) {
        put_wide(kSrcStr, src_name);
        put_wide(kDstStr, dst_name);
        return call(&Kernel32ExtManager::impl_CopyFileExW,
                    {kSrcStr, kDstStr, routine, 0x1234, cancel, flags}, x64);
    }

    std::string root;
    GuestMemory memory;
    capsule::Vfs vfs;
    HandleTable handles;
    std::unique_ptr<FileManager> files;
    std::unique_ptr<Kernel32ExtManager> mgr;
    std::string src_name = "C:\\src.bin";
    std::string dst_name = "C:\\dst.bin";
    std::vector<Msg> msgs;
    std::string seq;
    int reply_at_type = -1;
    u64 reply = 0;
    bool replied = false;
};

TEST_F(CopyFileEngineTest, CopyFile2RejectsAnyOtherDwSizeWithoutTouchingLastError) {
    make_file("src.bin", 10);
    for (bool x64 : {false, true}) {
        // cf2_dwsize_0 / _small / _big: E_INVALIDARG, gle untouched, no copy.
        const u32 v1 = x64 ? 32u : 20u;
        for (u32 size : {0u, v1 - 4, v1 + 8}) {
            put_params(x64, size, 0, 0, 0, 0);
            EXPECT_EQ(static_cast<u32>(copy_file2(x64)), 0x80070057u) << size;
            EXPECT_EQ(get_last_error(), kSentinel);
            EXPECT_EQ(host_size("dst.bin"), -1);
        }
        // cf2_v2_zeroed: sizeof(COPYFILE2_EXTENDED_PARAMETERS_V2) is accepted.
        put_params(x64, x64 ? 112u : 64u, 0, 0, 0, 0);
        EXPECT_EQ(copy_file2(x64), 0u);
        EXPECT_EQ(get_last_error(), 0u);
        std::filesystem::remove(root + "/dst.bin");
        // cf2_flag bit=40000000: E_INVALIDARG, gle untouched.
        put_params(x64, v1, 0x40000000u, 0, 0, 0);
        EXPECT_EQ(static_cast<u32>(copy_file2(x64)), 0x80070057u);
        EXPECT_EQ(get_last_error(), kSentinel);
        // cf2_bad_flag (0x80000000) is accepted.
        put_params(x64, v1, 0x80000000u, 0, 0, 0);
        EXPECT_EQ(copy_file2(x64), 0u);
        std::filesystem::remove(root + "/dst.bin");
    }
}

TEST_F(CopyFileEngineTest, CopyFile2SmallFileMessagesMatchWindows) {
    make_file("src.bin", 10);
    for (bool x64 : {false, true}) {
        msgs.clear(); seq.clear();
        put_params(x64, x64 ? 32u : 20u, 0, 0, kRoutine, 0x5678);
        EXPECT_EQ(copy_file2(x64), 0u);                 // cf2_seq_small hr=0
        EXPECT_EQ(get_last_error(), 0u);                // gle=0
        EXPECT_EQ(seq, "3124");
        ASSERT_EQ(msgs.size(), 4u);
        for (const auto& g : msgs) {
            EXPECT_EQ(g.ctx, 0x5678u);
            EXPECT_EQ(g.stream, 1u);
            EXPECT_NE(g.hs, 0u);
            EXPECT_NE(g.hd, 0u);
            EXPECT_EQ(g.ssize, 10u);
            EXPECT_EQ(g.total, 10u);
        }
        EXPECT_EQ(msgs[1].chunk, 0u);
        EXPECT_EQ(msgs[1].csize, 10u);
        EXPECT_EQ(msgs[2].csize, 10u);
        EXPECT_EQ(msgs[2].sdone, 10u);
        EXPECT_EQ(msgs[2].tdone, 10u);
        EXPECT_EQ(msgs[3].sdone, 10u);
        EXPECT_EQ(host_size("dst.bin"), 10);
        std::filesystem::remove(root + "/dst.bin");
    }
}

TEST_F(CopyFileEngineTest, CopyFile2BigFileKeepsThreeChunksInFlight) {
    make_file("big.bin", 3 * kMiB + 5);
    src_name = "C:\\big.bin";
    put_params(true, 32, 0, 0, kRoutine, 0x5678);
    EXPECT_EQ(copy_file2(true), 0u);
    EXPECT_EQ(seq, "3111212224");                       // cf2_seq_big
    ASSERT_EQ(msgs.size(), 10u);
    // ChunkStarted reports the stream's chunk size even for the 5-byte tail.
    EXPECT_EQ(msgs[5].type, 1u);
    EXPECT_EQ(msgs[5].chunk, 3u);
    EXPECT_EQ(msgs[5].csize, kMiB);
    EXPECT_EQ(msgs[8].type, 2u);
    EXPECT_EQ(msgs[8].chunk, 3u);
    EXPECT_EQ(msgs[8].csize, 5u);
    EXPECT_EQ(msgs[8].sdone, 3 * kMiB + 5);
    EXPECT_EQ(host_size("dst.bin"), static_cast<i64>(3 * kMiB + 5));
}

TEST_F(CopyFileEngineTest, CopyFile2CancelStopQuietPauseMatchWindows) {
    make_file("big.bin", 3 * kMiB + 5);
    src_name = "C:\\big.bin";
    struct Row { u64 act; int at; const char* seq; u32 hr; i64 dst; };
    const Row rows[] = {
        {1, 2, "311124", 0x800704d3u, -1},              // cf2_cancel_at_chunk_finished
        {1, 3, "34", 0x800704d3u, -1},                  // cf2_cancel_at_stream_started
        {1, 4, "3111212224", 0x800704d3u, -1},          // cf2_cancel_at_stream_finished
        {2, 3, "34", 0x800704d3u, 3 * kMiB + 5},        // cf2_stop_at_stream_started
        {2, 1, "314", 0x800704d3u, -1},                 // cf2_stop_at_chunk_started
        {2, 4, "3111212224", 0x800704d3u, 3 * kMiB + 5},// cf2_stop_at_stream_finished
        {3, 2, "31112", 0u, 3 * kMiB + 5},              // cf2_quiet_at_chunk_finished
        {4, 2, "311124", 0x80070beau, 3 * kMiB},        // cf2_pause_at_chunk_finished
        {4, 4, "3111212224", 0u, 3 * kMiB + 5},         // cf2_pause_at_stream_finished
    };
    for (const auto& row : rows) {
        std::filesystem::remove(root + "/dst.bin");
        msgs.clear(); seq.clear();
        reply = row.act; reply_at_type = row.at; replied = false;
        put_params(true, 32, 0, 0, kRoutine, 0x5678);
        EXPECT_EQ(static_cast<u32>(copy_file2(true)), row.hr) << row.act << "@" << row.at;
        EXPECT_EQ(get_last_error(), row.hr & 0xFFFFu) << row.act << "@" << row.at;
        EXPECT_EQ(seq, row.seq) << row.act << "@" << row.at;
        EXPECT_EQ(host_size("dst.bin"), row.dst) << row.act << "@" << row.at;
    }
    // The trailing STREAM_FINISHED after a cancel carries the bytes done.
    std::filesystem::remove(root + "/dst.bin");
    msgs.clear(); seq.clear();
    reply = 1; reply_at_type = 2; replied = false;
    put_params(true, 32, 0, 0, kRoutine, 0x5678);
    (void)copy_file2(true);
    ASSERT_EQ(msgs.size(), 6u);
    EXPECT_EQ(msgs[5].type, 4u);
    EXPECT_EQ(msgs[5].sdone, kMiB);
}

TEST_F(CopyFileEngineTest, CopyFile2ErrorMessagesAndCancelFlag) {
    make_file("src.bin", 10);
    // cf2_seq_missing_src: one ERROR message, phase PREPARE_SOURCE, stream 0.
    src_name = "C:\\nope.bin";
    put_params(true, 32, 0, 0, kRoutine, 0x5678);
    EXPECT_EQ(static_cast<u32>(copy_file2(true)), 0x80070002u);
    EXPECT_EQ(get_last_error(), 2u);
    EXPECT_EQ(seq, "6");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].phase, 1u);
    EXPECT_EQ(msgs[0].stream, 0u);
    EXPECT_EQ(msgs[0].hr, 0x80070002u);
    // cf2_seq_fail_if_exists: phase PREPARE_DEST, stream 1, sizes of the source.
    src_name = "C:\\src.bin";
    make_file("dst.bin", 3);
    msgs.clear(); seq.clear();
    put_params(true, 32, 0x1, 0, kRoutine, 0x5678);
    EXPECT_EQ(static_cast<u32>(copy_file2(true)), 0x80070050u);
    EXPECT_EQ(seq, "6");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].phase, 2u);
    EXPECT_EQ(msgs[0].stream, 1u);
    EXPECT_EQ(msgs[0].hr, 0x80070050u);
    EXPECT_EQ(msgs[0].ssize, 10u);
    EXPECT_EQ(host_size("dst.bin"), 3);
    // cf2_pfcancel_true: aborted before anything is opened.
    std::filesystem::remove(root + "/dst.bin");
    msgs.clear(); seq.clear();
    (void)memory.write_u32(kCancel, 1);
    put_params(false, 20, 0, kCancel, kRoutine, 0x5678);
    EXPECT_EQ(static_cast<u32>(copy_file2(false)), 0x800704d3u);
    EXPECT_EQ(get_last_error(), 0x4d3u);
    EXPECT_TRUE(seq.empty());
    EXPECT_EQ(host_size("dst.bin"), -1);
}

TEST_F(CopyFileEngineTest, CopyFileExProgressRoutineMatchesWindows) {
    make_file("big.bin", 3 * kMiB + 5);
    src_name = "C:\\big.bin";
    for (bool x64 : {false, true}) {
        std::filesystem::remove(root + "/dst.bin");
        msgs.clear(); seq.clear();
        reply_at_type = -1;
        EXPECT_EQ(copy_file_ex(x64, kRoutine, 0, 0), 1u);   // cfx_seq_big ret=1
        EXPECT_EQ(get_last_error(), 0u);
        EXPECT_EQ(seq, "10000");
        ASSERT_EQ(msgs.size(), 5u);
        EXPECT_EQ(msgs[0].total, 3 * kMiB + 5);
        EXPECT_EQ(msgs[0].tdone, 0u);
        EXPECT_EQ(msgs[1].tdone, kMiB);
        EXPECT_EQ(msgs[4].tdone, 3 * kMiB + 5);
        EXPECT_EQ(msgs[4].sdone, 3 * kMiB + 5);
        EXPECT_EQ(msgs[4].stream, 1u);
        EXPECT_EQ(msgs[4].ctx, 0x1234u);
        EXPECT_NE(msgs[4].hs, 0u);
    }
    // cfx_cancel_at_chunk: FALSE/ERROR_REQUEST_ABORTED, destination deleted.
    std::filesystem::remove(root + "/dst.bin");
    msgs.clear(); seq.clear();
    reply = 1; reply_at_type = 0; replied = false;
    EXPECT_EQ(copy_file_ex(true, kRoutine, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), 0x4d3u);
    EXPECT_EQ(seq, "10");
    EXPECT_EQ(host_size("dst.bin"), -1);
    // cfx_stop_at_stream: the pre-sized destination stays.
    msgs.clear(); seq.clear();
    reply = 2; reply_at_type = 1; replied = false;
    EXPECT_EQ(copy_file_ex(false, kRoutine, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), 0x4d3u);
    EXPECT_EQ(seq, "1");
    EXPECT_EQ(host_size("dst.bin"), static_cast<i64>(3 * kMiB + 5));
    // cfx_quiet_at_stream: no more callbacks, the copy completes.
    std::filesystem::remove(root + "/dst.bin");
    msgs.clear(); seq.clear();
    reply = 3; reply_at_type = 1; replied = false;
    EXPECT_EQ(copy_file_ex(true, kRoutine, 0, 0), 1u);
    EXPECT_EQ(seq, "1");
    EXPECT_EQ(host_size("dst.bin"), static_cast<i64>(3 * kMiB + 5));
}

TEST_F(CopyFileEngineTest, CopyFileExRejectsFlagsOutsideTheMeasuredMask) {
    make_file("src.bin", 10);
    // cfx_flag sweep: each rejected bit is FALSE/ERROR_INVALID_PARAMETER, no copy.
    for (u32 bit : {0x10u, 0x20u, 0x40u, 0x100u, 0x4000u, 0x10000u, 0x100000u,
                    0x1000000u, 0x8000000u, 0x20000000u, 0x80000000u}) {
        EXPECT_EQ(copy_file_ex(true, 0, 0, bit), 0u) << std::hex << bit;
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
        EXPECT_EQ(host_size("dst.bin"), -1);
    }
    for (u32 bit : {0x2u, 0x4u, 0x8u, 0x800u, 0x1000u, 0x2000u, 0x8000u, 0x40000u,
                    0x200000u, 0x400000u, 0x800000u, 0x2000000u, 0x4000000u,
                    0x10000000u}) {
        EXPECT_EQ(copy_file_ex(false, 0, 0, bit), 1u) << std::hex << bit;
        EXPECT_EQ(host_size("dst.bin"), 10) << std::hex << bit;
        std::filesystem::remove(root + "/dst.bin");
    }
}

TEST_F(CopyFileEngineTest, CopyFileRefusesHiddenOrSystemDestinationUnlessSourceHasIt) {
    make_file("src.bin", 10);
    auto copy_file = [&](bool fail_if_exists) {
        put_wide(kSrcStr, src_name);
        put_wide(kDstStr, dst_name);
        return call(&Kernel32ExtManager::impl_CopyFileW,
                    {kSrcStr, kDstStr, fail_if_exists ? 1u : 0u}, false);
    };
    make_file("dst.bin", 3);
    ASSERT_TRUE(vfs.set_attributes("C:\\dst.bin", 0x2).is_ok());
    EXPECT_EQ(copy_file(false), 0u);                        // cfw_dst_hidden
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_EQ(host_size("dst.bin"), 3);
    ASSERT_TRUE(vfs.set_attributes("C:\\dst.bin", 0x4).is_ok());
    EXPECT_EQ(copy_file(false), 0u);                        // cfw_dst_system
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    // cfw_hidden_over_hidden: allowed, result 0x22 (HIDDEN|ARCHIVE).
    ASSERT_TRUE(vfs.set_attributes("C:\\dst.bin", 0x2).is_ok());
    ASSERT_TRUE(vfs.set_attributes("C:\\src.bin", 0x2).is_ok());
    EXPECT_EQ(copy_file(false), 1u);
    EXPECT_EQ(get_last_error(), 0u);
    auto st = vfs.stat("C:\\dst.bin");
    ASSERT_TRUE(st.is_ok());
    EXPECT_EQ(static_cast<u32>(st.value().attributes), 0x22u);
    EXPECT_EQ(host_size("dst.bin"), 10);
    // cfw_ro_over_ro: a READONLY destination is refused even from a READONLY source.
    ASSERT_TRUE(vfs.set_attributes("C:\\src.bin", 0x1).is_ok());
    ASSERT_TRUE(vfs.set_attributes("C:\\dst.bin", 0x1).is_ok());
    EXPECT_EQ(copy_file(false), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
}

TEST_F(CopyFileEngineTest, SetAttributesNormalClearsHiddenAndReadOnlyTogether) {
    // Clearing READONLY and HIDDEN in one SetFileAttributes(NORMAL) left
    // HIDDEN behind on the host (the xattr write ran while the file was still
    // read-only), which made CopyFile over it fail (s4_copyfile_probe
    // cfw_dst_hidden after cf2_src_hidden_ro).
    make_file("a.bin", 1);
    ASSERT_TRUE(vfs.set_attributes("C:\\a.bin", 0x3).is_ok());
    ASSERT_TRUE(vfs.set_attributes("C:\\a.bin", 0x80).is_ok());
    auto st = vfs.stat("C:\\a.bin");
    ASSERT_TRUE(st.is_ok());
    EXPECT_EQ(static_cast<u32>(st.value().attributes) & 0x7u, 0u);
}

TEST_F(CopyFileEngineTest, CopyFileExPreservesSourceLastWriteTime) {
    // Learn CopyFileW: the copy keeps the source last-write time
    // (files_ns_probe CopyFileW_write_time_exact).
    make_file("src.bin", 7);
    struct timespec times[2]{};
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = 1'600'000'000;
    ASSERT_EQ(::utimensat(AT_FDCWD, (root + "/src.bin").c_str(), times, 0), 0);
    EXPECT_EQ(copy_file_ex(false, 0, 0, 0), 1u);
    auto src = vfs.stat("C:\\src.bin");
    auto dst = vfs.stat("C:\\dst.bin");
    ASSERT_TRUE(src.is_ok());
    ASSERT_TRUE(dst.is_ok());
    EXPECT_EQ(dst.value().last_write_time, src.value().last_write_time);
}

}  // namespace
