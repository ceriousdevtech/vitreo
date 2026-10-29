// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crt_stdio_blocking.cpp
/// @brief Blocking standard-input reads, per-descriptor locks and standard-stream read validation

#include "crt_stdio_test_fixture.hpp"

namespace {

// Each simulated guest thread has its own registers, marshaler and stack.
u64 call_on_thread(OsHandleRig& rig, GuestAddr stack, const char* name,
                   std::initializer_list<u64> args) {
    cpu::CpuState cpu;
    if (rig.x64) cpu.init_long_mode(); else cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler m(rig.memory);
    m.attach_cpu(cpu);
    usize i = 0;
    for (u64 arg : args) {
        if (rig.x64) {
            if (i == 0) cpu.gpr.rcx = arg;
            if (i == 1) cpu.gpr.rdx = arg;
            if (i == 2) cpu.gpr.r8 = arg;
            if (i == 3) cpu.gpr.r9 = arg;
        } else {
            (void)rig.memory.write_u32(stack + 4 + i * 4, static_cast<u32>(arg));
        }
        ++i;
    }
    auto entry = std::find_if(rig.exports.entries.begin(), rig.exports.entries.end(),
        [name](const auto& entry) { return entry.name == name; });
    EXPECT_NE(entry, rig.exports.entries.end());
    if (entry == rig.exports.entries.end()) return 0;
    entry->handler(m);
    return rig.x64 ? cpu.gpr.rax : cpu.gpr.rax & 0xFFFFFFFFu;
}

}  // namespace

// Native Windows x86/x64: crt_blocking_stdio_probe.expected. Operations on
// another descriptor progress; close of the blocked descriptor waits.
TEST_F(CrtStdioTest, BlockingStandardInputDoesNotStallOtherDescriptors) {
    using namespace std::chrono_literals;
    const auto dir = std::filesystem::path(tmp_path("blockingstdio")).parent_path();
    for (bool x64 : {false, true}) {
        for (const char* reader_name : {"fgets", "fread", "fgetc", "_read"}) {
            SCOPED_TRACE(x64 ? "x64" : "x86");
            SCOPED_TRACE(reader_name);
            StdHandleRig rig(x64, dir);
            Handle inread = 0, inwrite = 0, outread = 0, outwrite = 0;
            ASSERT_TRUE(rig.files.create_anonymous_pipe(inread, inwrite, false));
            ASSERT_TRUE(rig.files.create_anonymous_pipe(outread, outwrite, false));
            ASSERT_TRUE(rig.console.set_std_handle(STD_INPUT_HANDLE, inread));
            ASSERT_TRUE(rig.console.set_std_handle(STD_OUTPUT_HANDLE, outwrite));
            const u64 stream = rig.call("__acrt_iob_func", {0});
            ASSERT_TRUE(rig.m.write_ansi_string(kBufAddr, "x").is_ok());
            std::promise<void> started;
            auto started_future = started.get_future();
            auto reader = std::async(std::launch::async, [&] {
                started.set_value();
                if (std::string_view(reader_name) == "fgets")
                    return call_on_thread(rig, 0x19000, reader_name, {kBufAddr + 0x400, 32, stream});
                if (std::string_view(reader_name) == "fread")
                    return call_on_thread(rig, 0x19000, reader_name, {kBufAddr + 0x400, 1, 3, stream});
                if (std::string_view(reader_name) == "fgetc")
                    return call_on_thread(rig, 0x19000, reader_name, {stream});
                return call_on_thread(rig, 0x19000, reader_name, {0, kBufAddr + 0x400, 3});
            });
            started_future.wait();
            std::this_thread::sleep_for(50ms);
            EXPECT_EQ(reader.wait_for(0ms), std::future_status::timeout);
            auto writer = std::async(std::launch::async, [&] {
                return call_on_thread(rig, 0x1A000, "_write", {1, kBufAddr, 1});
            });
            auto closer = std::async(std::launch::async, [&] {
                return call_on_thread(rig, 0x1B000, "_close", {0});
            });
            const auto write_status = writer.wait_for(200ms);
            const auto close_status = closer.wait_for(50ms);
            EXPECT_EQ(write_status, std::future_status::ready);
            EXPECT_EQ(close_status, std::future_status::timeout);
            auto query = std::async(std::launch::async, [&] {
                return call_on_thread(rig, 0x1C000, "_get_osfhandle", {0});
            });
            auto empty_read = std::async(std::launch::async, [&] {
                return call_on_thread(rig, 0x1D000, "fread", {0, 0, 1, stream});
            });
            const auto query_status = query.wait_for(200ms);
            EXPECT_EQ(query_status, std::future_status::ready);
            if (query_status == std::future_status::ready) EXPECT_EQ(query.get(), inread);
            EXPECT_EQ(empty_read.wait_for(200ms), std::future_status::ready);
            // Always unblock the reader, including against the faulty owner.
            EXPECT_TRUE(rig.files.write_handle_bytes(inwrite, "ok\n"));
            EXPECT_TRUE(rig.files.close_file(inwrite));
            EXPECT_EQ(reader.get(), std::string_view(reader_name) == "fgets" ? kBufAddr + 0x400
                : std::string_view(reader_name) == "fgetc" ? u64{'o'} : 3u);
            EXPECT_EQ(writer.get(), 1u);
            EXPECT_EQ(closer.get(), 0u);
            // Capture the query result before the close retires the handle.
            // The ready checks above are the ordering contract; a faulty
            // owner may delay it until after that retirement.
            if (query.valid()) (void)query.get();
            EXPECT_EQ(empty_read.get(), 0u);
            EXPECT_EQ(rig.drain(outread), "x");
            EXPECT_EQ(rig.call("_close", {1}), 0u);
            EXPECT_TRUE(rig.files.close_file(outread));
        }
    }
}

TEST_F(CrtStdioTest, BlockingLowIoUsesIndependentDescriptorLocksAndOrdersReplacement) {
    using namespace std::chrono_literals;
    const auto dir = std::filesystem::path(tmp_path("lowio_lock")).parent_path();
    for (bool x64 : {false, true}) {
        for (bool standard : {false, true}) {
            for (bool replace : {false, true}) {
                SCOPED_TRACE(x64);
                SCOPED_TRACE(standard);
                SCOPED_TRACE(replace);
                StdHandleRig rig(x64, dir, true);
                unsigned invalid_calls = 0;
                rig.mm.exchange_invalid_parameter_handler(0x12800);
                rig.mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 {
                    ++invalid_calls; return 0;
                });
                Handle inread, inwrite, outread, outwrite, newread, newwrite;
                ASSERT_TRUE(rig.files.create_anonymous_pipe(inread, inwrite, false));
                ASSERT_TRUE(rig.files.create_anonymous_pipe(outread, outwrite, false));
                ASSERT_TRUE(rig.files.create_anonymous_pipe(newread, newwrite, false));
                auto input = rig.call("_open_osfhandle", {inread, kO_BINARY});
                const auto output = rig.call("_open_osfhandle", {outwrite, kO_BINARY});
                const auto replacement = rig.call("_open_osfhandle", {newread, kO_BINARY});
                if (standard) {
                    ASSERT_EQ(rig.call("_dup2", {input, 0}), 0u);
                    ASSERT_EQ(rig.call("_close", {input}), 0u);
                    input = 0;
                }
                ASSERT_TRUE(rig.m.write_ansi_string(kBufAddr, "x").is_ok());
                std::promise<void> entered;
                auto started = entered.get_future();
                auto reader = std::async(std::launch::async, [&] {
                    entered.set_value();
                    return call_on_thread(rig, 0x19000, "_read", {input, kBufAddr + 0x400, 3});
                });
                started.wait(); std::this_thread::sleep_for(50ms);
                EXPECT_EQ(reader.wait_for(0ms), std::future_status::timeout);
                auto writer = std::async(std::launch::async, [&] {
                    return call_on_thread(rig, 0x1A000, "_write", {output, kBufAddr, 1});
                });
                auto mutation = std::async(std::launch::async, [&] {
                    return replace ? call_on_thread(rig, 0x1B000, "_dup2", {replacement, input})
                                   : call_on_thread(rig, 0x1B000, "_close", {input});
                });
                EXPECT_EQ(writer.wait_for(200ms), std::future_status::ready);
                EXPECT_EQ(mutation.wait_for(50ms), std::future_status::timeout);
                auto query = std::async(std::launch::async, [&] {
                    return call_on_thread(rig, 0x1C000, "_get_osfhandle", {input});
                });
                auto zero = std::async(std::launch::async, [&] {
                    return call_on_thread(rig, 0x1D000, "_read", {input, 0, 0});
                });
                EXPECT_EQ(query.wait_for(200ms), std::future_status::ready);
                EXPECT_EQ(zero.wait_for(50ms), std::future_status::timeout);
                auto null_read = std::async(std::launch::async, [&] {
                    return call_on_thread(rig, 0x1E000, "_read", {input, 0, 1});
                });
                EXPECT_EQ(null_read.wait_for(50ms), std::future_status::timeout);
                // Release all tasks even when testing an owner without locks.
                EXPECT_TRUE(rig.files.write_handle_bytes(inwrite, "ok\n"));
                EXPECT_TRUE(rig.files.close_file(inwrite));
                EXPECT_EQ(reader.get(), 3u);
                EXPECT_EQ(writer.get(), 1u);
                EXPECT_EQ(mutation.get(), 0u);
                (void)query.get(); (void)zero.get();
                EXPECT_EQ(static_cast<u32>(null_read.get()), ~u32{0});
                EXPECT_EQ(invalid_calls, replace ? 1u : 0u);
                EXPECT_EQ(rig.drain(outread), "x");
                if (replace) EXPECT_EQ(rig.call("_close", {input}), 0u);
                EXPECT_EQ(rig.call("_close", {output}), 0u);
                EXPECT_EQ(rig.call("_close", {replacement}), 0u);
                EXPECT_TRUE(rig.files.close_file(outread));
                EXPECT_TRUE(rig.files.close_file(newwrite));
            }
        }
    }
}

TEST_F(CrtStdioTest, StandardStreamReadValidationMatchesLearn) {
    const auto dir = std::filesystem::path(tmp_path("stdreadvalidation")).parent_path();
    for (bool x64 : {false, true}) {
        StdHandleRig rig(x64, dir);
        unsigned invalid_calls = 0;
        rig.mm.exchange_invalid_parameter_handler(0x12800);
        rig.mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 {
            ++invalid_calls; return 0;
        });
        const auto stream = rig.call("__acrt_iob_func", {0});
        EXPECT_EQ(rig.call("fread", {0, 0, 1, stream}), 0u);
        EXPECT_EQ(invalid_calls, 0u);
        for (const auto& args : {std::array<u64, 4>{0, 1, 1, stream},
                                std::array<u64, 4>{kBufAddr, 1, 1, 0}}) {
            rig.mm.set_errno(0);
            const auto previous = invalid_calls;
            EXPECT_EQ(rig.call("fread", {args[0], args[1], args[2], args[3]}), 0u);
            EXPECT_EQ(rig.mm.get_errno(), 22u);
            EXPECT_EQ(invalid_calls, previous + 1);
        }
        for (const auto& args : {std::array<u64, 3>{0, 2, stream},
                                std::array<u64, 3>{kBufAddr, 0, stream},
                                std::array<u64, 3>{kBufAddr, ~u32{0}, stream},
                                std::array<u64, 3>{kBufAddr, 2, 0}}) {
            rig.mm.set_errno(0);
            const auto previous = invalid_calls;
            EXPECT_EQ(rig.call("fgets", {args[0], args[1], args[2]}), 0u);
            EXPECT_EQ(rig.mm.get_errno(), 22u);
            EXPECT_EQ(invalid_calls, previous + 1);
        }
    }
}
