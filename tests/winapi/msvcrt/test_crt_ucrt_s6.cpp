/// @file test_crt_ucrt_s6.cpp
/// @brief _crt_at_quick_exit / quick_exit, _wcsicoll, strtok_s,
///        __stdio_common_vfwscanf and qsort_s against real-Windows ucrtbase
///        outputs, both ABIs. Expected values: oracle/crt_ucrt_s6_probe.c run
///        on winoracle (crt_ucrt_s6_oracle.inc is generated from its output).

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "capsule/vfs/vfs.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/msvcrt/io/crt_stdio.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/msvcrt/runtime/crt_init.hpp"
#include "winapi/msvcrt/text/crt_string.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {
#include "crt_ucrt_s6_oracle.inc"

constexpr GuestAddr kBase = 0x10000;
constexpr GuestAddr kStack = 0x18000;
constexpr GuestAddr kStrA = 0x11000;
constexpr GuestAddr kStrB = 0x11100;
constexpr GuestAddr kBuf = 0x11200;      // strtok_s buffer, sort array
constexpr GuestAddr kOther = 0x11300;    // strtok_s second buffer
constexpr GuestAddr kCtx = 0x11400;      // strtok_s char** context
constexpr GuestAddr kSlots = 0x11500;    // scanf destinations, 0x40 apart
constexpr GuestAddr kVa = 0x11800;       // scanf va_list
constexpr GuestAddr kFormat = 0x11900;
constexpr GuestAddr kContext = 0x11a00;  // qsort_s context
constexpr GuestAddr kHandler = 0x1234;
constexpr GuestAddr kCompare = 0x5000;
constexpr GuestAddr kStream = 0x7000;
constexpr u32 kEinval = 0x16;

struct Harness {
    GuestMemory memory;
    capsule::Vfs vfs;
    CrtMemoryManager crt{memory, 0x20000000, 0x10000};
    CrtStringManager strings;
    CrtStdioManager io;
    CrtDataBlock crt_data;
    HandleTable handles;
    ThreadManager threads{handles};
    ProcessManager process;
    DllExports exports;
    cpu::CpuState cpu;
    Marshaler m{memory};
    bool x64;
    unsigned iph_calls = 0, iph_errno = 0;
    // qsort_s comparator state.
    unsigned cmp_calls = 0, cmp_bad = 0;
    GuestAddr sort_base = kBuf;
    u64 sort_count = 0, sort_width = 4;

    explicit Harness(bool is64) : x64(is64) {
        EXPECT_TRUE(memory.map_region(kBase, 0x10000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(crt_data.initialize(memory));
        const auto add = [this](DllExports part) {
            for (auto& e : part.entries) exports.entries.push_back(std::move(e));
        };
        add(create_crt_string_thunks(strings, crt, vfs));
        add(create_crt_memory_thunks(crt));
        add(create_crt_process_thunks(process, crt_data, threads, crt));
        add_crt_scanf_exports(exports, io, crt, crt_data);
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(kStack);
        m.attach_cpu(cpu);
        crt.exchange_invalid_parameter_handler(kHandler);
        crt.set_guest_caller([this](GuestAddr fn, const u64* args, usize count) -> u64 {
            if (fn == kHandler) {
                EXPECT_EQ(count, 5u);
                ++iph_calls;
                iph_errno = crt.get_errno();
                return 0;
            }
            EXPECT_EQ(fn, kCompare);
            EXPECT_EQ(count, 3u);
            ++cmp_calls;
            if (args[0] != kContext) cmp_bad |= 1;
            const GuestAddr end = sort_base + sort_count * sort_width;
            for (int i = 1; i <= 2; ++i)
                if (args[i] < sort_base || args[i] >= end) cmp_bad |= 2;
            const GuestAddr a = static_cast<GuestAddr>(args[1]);
            const GuestAddr b = static_cast<GuestAddr>(args[2]);
            if (sort_width == 3) {
                for (u32 k = 0; k < 3; ++k) {
                    const u8 x = memory.read_u8(a + k).value(), y = memory.read_u8(b + k).value();
                    if (x != y) return static_cast<u32>(x < y ? -1 : 1);
                }
                return 0;
            }
            const i32 x = static_cast<i32>(memory.read_u32(a).value());
            const i32 y = static_cast<i32>(memory.read_u32(b).value());
            return static_cast<u32>(x < y ? -1 : x > y ? 1 : 0);
        });
    }

    void reset() {
        iph_calls = iph_errno = 0;
        crt.set_errno(0x1234);
    }

    u64 call(const char* name, std::initializer_list<u64> args) {
        usize i = 0;
        for (u64 a : args) {
            if (x64) {
                switch (i) {
                    case 0: cpu.gpr.rcx = a; break;
                    case 1: cpu.gpr.rdx = a; break;
                    case 2: cpu.gpr.r8 = a; break;
                    case 3: cpu.gpr.r9 = a; break;
                    default: EXPECT_TRUE(memory.write_u64(kStack + 8 * (i + 1), a).is_ok());
                }
            } else {
                EXPECT_TRUE(memory.write_u32(kStack + 4 + 4 * i, static_cast<u32>(a)).is_ok());
            }
            ++i;
        }
        cpu.gpr.rax = 0xDEADBEEFDEADBEEFull;
        const ExportEntry* found = nullptr;
        for (const auto& e : exports.entries) if (e.name == name) found = &e;
        EXPECT_NE(found, nullptr) << name;
        if (found) found->handler(m);
        return x64 ? cpu.gpr.rax : static_cast<u32>(cpu.gpr.rax);
    }

    void write_ptr(GuestAddr at, u64 v) {
        if (x64) EXPECT_TRUE(memory.write_u64(at, v).is_ok());
        else EXPECT_TRUE(memory.write_u32(at, static_cast<u32>(v)).is_ok());
    }
    u64 read_ptr(GuestAddr at) {
        return x64 ? memory.read_u64(at).value() : memory.read_u32(at).value();
    }
    void put_units(GuestAddr at, const u16* units) {
        usize i = 0;
        do { EXPECT_TRUE(memory.write_u16(at + 2 * i, units[i]).is_ok()); } while (units[i++]);
    }
};

std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%x", v);
    return b;
}

}  // namespace

// ---- _crt_at_quick_exit / quick_exit -----------------------------------------

// Replays the probe's child processes: the same registrations and calls, the
// same "<registration results>:<functions run>" transcript, the same exit code.
TEST(CrtUcrtS6Test, QuickExitMatchesWindowsBothAbis) {
    constexpr GuestAddr kA = 0xA000, kB = 0xB000, kC = 0xC000, kD = 0xD000,
                        kE = 0xE000, kF = 0xF000, kR = 0x9000;
    for (bool x64 : {false, true}) {
        for (const S6Quick& q : kS6Quick) {
            const std::string mode = q.mode;
            if (mode == "flush") continue;  // stream buffering: see quick_exit in crt_init.cpp
            SCOPED_TRACE(std::string(x64 ? "x64 " : "x86 ") + mode);
            Harness h(x64);
            std::string transcript;
            h.crt.set_void_callback([&](GuestAddr f) {
                static const std::map<GuestAddr, char> names = {
                    {kA, 'A'}, {kB, 'B'}, {kC, 'C'}, {kD, 'D'}, {kE, 'E'}, {kF, 'F'}, {kR, 'R'}};
                transcript += names.at(f);
                if (f == kR) transcript += hex(static_cast<u32>(h.call("_crt_at_quick_exit", {kE})));
            });
            const auto quick = [&](GuestAddr f) {
                transcript += hex(static_cast<u32>(h.call("_crt_at_quick_exit", {f})));
            };
            const auto atexit_ = [&](GuestAddr f) {
                transcript += hex(static_cast<u32>(h.call("_crt_atexit", {f})));
            };
            if (mode == "lifo") {
                quick(kA); quick(kB); quick(kC); atexit_(kD); transcript += ":";
                h.call("quick_exit", {7});
            } else if (mode == "nested") {
                quick(kA); quick(kR); quick(kB); transcript += ":";
                h.call("quick_exit", {9});
            } else if (mode == "null") {
                quick(0); quick(kA); transcript += ":";
                h.call("quick_exit", {5});
            } else if (mode == "exit") {
                quick(kA); atexit_(kD); transcript += ":";
                h.call("exit", {3});
            } else if (mode == "many") {
                for (int i = 0; i < 40; ++i)
                    if (h.call("_crt_at_quick_exit", {i == 0 ? kF : kE}) != 0) transcript += "!";
                transcript += ":";
                h.call("quick_exit", {4});
            } else if (mode == "zero") {
                transcript += ":";
                h.call("quick_exit", {0});
            } else {
                ADD_FAILURE() << "unknown mode";
                continue;
            }
            EXPECT_EQ(transcript, q.output);
            EXPECT_TRUE(h.process.has_exited());
            EXPECT_EQ(h.process.exit_code(), q.exit_code);
        }
    }
}

// ---- _wcsicoll -------------------------------------------------------------------

TEST(CrtUcrtS6Test, WcsicollMatchesWindowsBothAbis) {
    for (bool x64 : {false, true}) {
        Harness h(x64);
        usize index = 0;
        for (const S6Coll& c : kS6Coll) {
            SCOPED_TRACE(std::string(x64 ? "x64" : "x86") + " case " + std::to_string(index++));
            h.put_units(kStrA, c.a);
            h.put_units(kStrB, c.b);
            h.reset();
            const u32 r = static_cast<u32>(h.call("_wcsicoll", {c.null_a ? 0 : kStrA, c.null_b ? 0 : kStrB}));
            EXPECT_EQ(r, c.ret);
            EXPECT_EQ(h.crt.get_errno(), c.err);
            EXPECT_EQ(h.iph_calls, c.iph_calls);
            EXPECT_EQ(h.iph_errno, c.iph_errno);
        }
    }
}

// ---- strtok_s ------------------------------------------------------------------

TEST(CrtUcrtS6Test, StrtokSMatchesWindowsBothAbis) {
    for (bool x64 : {false, true}) {
        Harness h(x64);
        const auto offset = [&](u64 p, u32 null_value) -> u32 {
            if (p == 0) return null_value;
            if (p >= kOther && p < kOther + 8) return static_cast<u32>(0x100 + p - kOther);
            return static_cast<u32>(p - kBuf);
        };
        for (const S6TokRow& row : kS6TokRows) {
            SCOPED_TRACE(std::string(x64 ? "x64" : "x86") + " case " + hex(row.cse) +
                         " step " + std::to_string(row.step));
            u64 ret = 0;
            if (row.cse < 0x10) {
                const S6TokSeq& seq = kS6TokSeqs[row.cse];
                if (row.step == 0) {
                    for (u32 i = 0; i < 16; ++i) (void)h.memory.write_u8(kBuf + i, 0xcc);
                    for (u32 i = 0; i == 0 || seq.text[i - 1]; ++i) (void)h.memory.write_u8(kBuf + i, seq.text[i]);
                    h.write_ptr(kCtx, 0);
                }
                for (u32 i = 0; i == 0 || seq.delims[i - 1]; ++i) (void)h.memory.write_u8(kStrA + i, seq.delims[i]);
                h.reset();
                ret = h.call("strtok_s", {row.step == 0 ? kBuf : 0, kStrA, kCtx});
            } else {
                // The probe's error cases: buf = "p q", other = "x y".
                const char* p = "p q";
                const char* o = "x y";
                for (u32 i = 0; i < 4; ++i) {
                    (void)h.memory.write_u8(kBuf + i, static_cast<u8>(p[i]));
                    (void)h.memory.write_u8(kOther + i, static_cast<u8>(o[i]));
                }
                (void)h.memory.write_u8(kStrA, ' ');
                (void)h.memory.write_u8(kStrA + 1, 0);
                const u32 c = row.cse - 0x10;
                h.write_ptr(kCtx, c >= 4 ? kOther + 2 : 0);
                h.reset();
                switch (c) {
                    case 0: ret = h.call("strtok_s", {0, kStrA, kCtx}); break;
                    case 1: ret = h.call("strtok_s", {kBuf, 0, kCtx}); break;
                    case 2: ret = h.call("strtok_s", {kBuf, kStrA, 0}); break;
                    case 3: ret = h.call("strtok_s", {0, kStrA, 0}); break;
                    case 4: ret = h.call("strtok_s", {0, 0, kCtx}); break;
                    case 5: ret = h.call("strtok_s", {0, kStrA, kCtx}); break;
                    default: ret = h.call("strtok_s", {kBuf, kStrA, kCtx}); break;
                }
            }
            EXPECT_EQ(offset(ret, 0xff), row.ret);
            EXPECT_EQ(offset(h.read_ptr(kCtx), 0xffff), row.ctx);
            EXPECT_EQ(h.crt.get_errno(), row.err);
            EXPECT_EQ(h.iph_calls, row.iph_calls);
            EXPECT_EQ(h.iph_errno, row.iph_errno);
            for (u32 i = 0; i < row.n; ++i)
                EXPECT_EQ(h.memory.read_u8(kBuf + i).value(), row.buf[i]) << "byte " << i;
        }
    }
}

// ---- __stdio_common_vfwscanf ---------------------------------------------------------

namespace {

// A text-mode ("r") stream as Learn fopen defines it on input: CR-LF reads as
// LF and CTRL+Z is end-of-file. ungetc pushes back one byte.
struct TextStream {
    std::vector<u8> bytes;
    usize pos = 0;
    i32 get() {
        if (pos >= bytes.size() || bytes[pos] == 0x1a) return -1;
        if (bytes[pos] == '\r' && pos + 1 < bytes.size() && bytes[pos + 1] == '\n') {
            pos += 2;
            return '\n';
        }
        return bytes[pos++];
    }
    i32 unget(i32 c) {
        if (c == -1 || pos == 0) return -1;
        pos -= (c == '\n' && pos >= 2 && bytes[pos - 2] == '\r') ? 2 : 1;
        return c;
    }
};

}  // namespace

TEST(CrtUcrtS6Test, VfwscanfMatchesWindowsOnTextStreamsBothAbis) {
    for (bool x64 : {false, true}) {
        for (const S6ScanRow& row : kS6ScanRows) {
            SCOPED_TRACE(std::string(x64 ? "x64" : "x86") + " case " + hex(row.cse));
            Harness h(x64);
            TextStream stream;
            const S6ScanCase* c = row.cse < 0x20 ? &kS6ScanCases[row.cse] : nullptr;
            if (c && std::string(c->mode) != "r") continue;  // see the test below
            if (c) stream.bytes.assign(c->bytes, c->bytes + c->n);
            else stream.bytes = {'1'};
            h.io.configure_guest_character_io([&](u32) { return stream.get(); },
                                              [&](i32 ch, u32) { return stream.unget(ch); });
            const u16 int_format[] = {'%', 'd', 0};
            h.put_units(kFormat, c ? c->format : int_format);
            for (u32 i = 0; i < 0x100; ++i) (void)h.memory.write_u8(kSlots + i, 0);
            // Destinations, prefilled as the probe does.
            const GuestAddr s0 = kSlots, s1 = kSlots + 0x40, s2 = kSlots + 0x80;
            std::vector<u64> va;
            const u32 kind = c ? c->kind : 4;
            switch (kind) {
                case 0: (void)h.memory.write_u32(s0, 0x5a5a);
                        for (u32 i = 0; i < 8; ++i) (void)h.memory.write_u16(s1 + 2 * i, 0xcccc);
                        (void)h.memory.write_u16(s2, 0x5a5a); va = {s0, s1, s2}; break;
                case 1: for (GuestAddr s : {s0, s1, s2}) (void)h.memory.write_u16(s, 0x5a5a);
                        va = {s0, s1, s2}; break;
                case 2: (void)h.memory.write_u16(s0, 0x5a5a); (void)h.memory.write_u32(s1, 0x5a5a);
                        va = {s0, s1}; break;
                case 3: (void)h.memory.write_u32(s0, 0x5a5a); (void)h.memory.write_u16(s1, 0x5a5a);
                        va = {s0, s1}; break;
                case 4: (void)h.memory.write_u32(s0, 0x5a5a); va = {s0}; break;
                case 5: for (u32 i = 0; i < 8; ++i) (void)h.memory.write_u8(s0 + i, 0xcc);
                        va = {s0}; break;
                case 6: for (u32 i = 0; i < 8; ++i) (void)h.memory.write_u16(s0 + 2 * i, 0xcccc);
                        va = {s0, 2}; break;
                case 7: for (u32 i = 0; i < 8; ++i) (void)h.memory.write_u16(s0 + 2 * i, 0xcccc);
                        (void)h.memory.write_u32(s1, 0x5a5a); va = {s0, s1}; break;
                default: (void)h.memory.write_u32(s0, 0x5a5a); (void)h.memory.write_u32(s1, 0x5a5a);
                         va = {s0, s1}; break;
            }
            for (usize i = 0; i < va.size(); ++i) h.write_ptr(kVa + i * (x64 ? 8 : 4), va[i]);
            const u64 options = c ? c->options : 0;
            const GuestAddr file = row.cse == 0x20 ? 0 : kStream;
            const GuestAddr format = row.cse == 0x21 ? 0 : kFormat;
            h.reset();
            const u32 r = static_cast<u32>(
                x64 ? h.call("__stdio_common_vfwscanf", {options, file, format, 0, kVa})
                    : h.call("__stdio_common_vfwscanf",
                             {options & 0xffffffffu, options >> 32, file, format, 0, kVa}));
            EXPECT_EQ(r, row.ret);
            EXPECT_EQ(h.crt.get_errno(), row.err);
            EXPECT_EQ(h.iph_calls, row.iph_calls);
            EXPECT_EQ(h.iph_errno, row.iph_errno);
            u64 v[4] = {0, 0, 0, 0};
            const auto u16s = [&](GuestAddr at) {
                u64 x = 0;
                for (u32 i = 0; i < 4; ++i) x |= u64{h.memory.read_u16(at + 2 * i).value()} << (16 * i);
                return x;
            };
            switch (kind) {
                case 0: v[0] = h.memory.read_u32(s0).value(); v[1] = u16s(s1);
                        v[2] = h.memory.read_u16(s2).value(); break;
                case 1: v[0] = h.memory.read_u16(s0).value(); v[1] = h.memory.read_u16(s1).value();
                        v[2] = h.memory.read_u16(s2).value(); break;
                case 2: v[0] = h.memory.read_u16(s0).value(); v[1] = h.memory.read_u32(s1).value(); break;
                case 3: v[0] = h.memory.read_u32(s0).value(); v[1] = h.memory.read_u16(s1).value(); break;
                case 4: v[0] = h.memory.read_u32(s0).value(); break;
                case 5: for (u32 i = 0; i < 8; ++i)
                            v[i / 4] |= u64{h.memory.read_u8(s0 + i).value()} << (16 * (i % 4));
                        break;
                case 6: v[0] = u16s(s0); break;
                case 7: v[0] = u16s(s0); v[1] = h.memory.read_u32(s1).value(); break;
                default: v[0] = h.memory.read_u32(s0).value(); v[1] = h.memory.read_u32(s1).value(); break;
            }
            if (row.cse < 0x20) {
                for (u32 i = 0; i < 4; ++i) EXPECT_EQ(v[i], row.v[i]) << "value " << i;
            }
            if (row.cse != 0x20) EXPECT_EQ(static_cast<u32>(stream.get()), row.next);
        }
    }
}

// Binary and ccs=UTF-8/UTF-16LE streams read UTF-16 units / decoded
// characters on Windows (probe rows 2, 3, 4, 13). Vitreo does not implement
// those yet: the call reads nothing and returns EOF (with an UNIMPLEMENTED
// log line) rather than misreading the stream.
TEST(CrtUcrtS6Test, VfwscanfRefusesNonAnsiStreams) {
    using W = CrtStdioManager::WideEncoding;
    for (bool x64 : {false, true}) {
        for (W enc : {W::Binary, W::Utf8, W::Utf16Le}) {
            Harness h(x64);
            bool read = false;
            h.io.configure_guest_character_io([&](u32) { read = true; return -1; },
                                              [&](i32, u32) { return -1; });
            h.io.configure_guest_stream_encoding([enc](u32) { return enc; });
            const u16 format[] = {'%', 'd', 0};
            h.put_units(kFormat, format);
            h.write_ptr(kVa, kSlots);
            const u32 r = static_cast<u32>(
                x64 ? h.call("__stdio_common_vfwscanf", {0, kStream, kFormat, 0, kVa})
                    : h.call("__stdio_common_vfwscanf", {0, 0, kStream, kFormat, 0, kVa}));
            EXPECT_EQ(r, 0xffffffffu);
            EXPECT_FALSE(read);
        }
    }
}

// ---- qsort_s -------------------------------------------------------------------------

TEST(CrtUcrtS6Test, QsortSMatchesWindowsBothAbis) {
    for (bool x64 : {false, true}) {
        Harness h(x64);
        for (const S6SortRow& row : kS6SortRows) {
            SCOPED_TRACE(std::string(x64 ? "x64" : "x86") + " case " + hex(row.cse));
            h.cmp_calls = h.cmp_bad = 0;
            if (row.cse < 0x20) {
                const S6SortValidation& v = kS6SortValidation[row.cse];
                const u32 init[] = {3, 1, 2};
                for (u32 i = 0; i < 3; ++i) (void)h.memory.write_u32(kBuf + 4 * i, init[i]);
                h.sort_count = v.number;
                h.sort_width = v.width ? v.width : 4;
                h.reset();
                h.call("qsort_s", {v.null_base ? 0 : kBuf, v.number, v.width,
                                   v.null_cmp ? 0 : kCompare, kContext});
                EXPECT_EQ(h.cmp_calls, row.cmp_calls);
            } else if (row.cse == 0x20) {
                const u32 ints[] = {5, 3, 9, 1, 7, 3, static_cast<u32>(-4), 0x7fffffff, 0x80000000u};
                for (u32 i = 0; i < 9; ++i) (void)h.memory.write_u32(kBuf + 4 * i, ints[i]);
                h.sort_count = 9;
                h.sort_width = 4;
                h.reset();
                h.call("qsort_s", {kBuf, 9, 4, kCompare, kContext});
                EXPECT_EQ(h.cmp_calls != 0, row.cmp_calls != 0);
            } else if (row.cse == 0x21) {
                const char* rec = "mopabczzzabbqrs";
                for (u32 i = 0; i < 15; ++i) (void)h.memory.write_u8(kBuf + i, static_cast<u8>(rec[i]));
                h.sort_count = 5;
                h.sort_width = 3;
                h.reset();
                h.call("qsort_s", {kBuf, 5, 3, kCompare, kContext});
                EXPECT_EQ(h.cmp_calls != 0, row.cmp_calls != 0);
            } else {
                (void)h.memory.write_u32(kBuf, 42);
                h.sort_count = 1;
                h.sort_width = 4;
                h.reset();
                h.call("qsort_s", {kBuf, 1, 4, kCompare, kContext});
                EXPECT_EQ(h.cmp_calls, row.cmp_calls);
            }
            EXPECT_EQ(h.crt.get_errno(), row.err);
            EXPECT_EQ(h.iph_calls, row.iph_calls);
            EXPECT_EQ(h.iph_errno, row.iph_errno);
            EXPECT_EQ(h.cmp_bad, row.cmp_bad);
            for (u32 i = 0; i < row.n; ++i) {
                const u32 got = row.cse == 0x21 ? h.memory.read_u8(kBuf + i).value()
                                                : h.memory.read_u32(kBuf + 4 * i).value();
                EXPECT_EQ(got, row.elems[i]) << "element " << i;
            }
        }
    }
}
