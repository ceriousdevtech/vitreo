// SPDX-License-Identifier: LGPL-2.1-only
// Expected behavior: tools/oracle_probe/activeds_error_probe{32,64}.expected.
#include <gtest/gtest.h>
#include "winapi/system/activeds/activeds.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include <thread>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {
struct AdsiExport { const char* name; u16 ordinal; u32 stack_bytes; };
#include "winapi/system/activeds/activeds_contract.inc"
struct GuestCall {
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler m{memory};
    DllExports exports = create_activeds_thunks();
    explicit GuestCall(bool x64) {
        EXPECT_TRUE(memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x10800;
        m.attach_cpu(cpu);
    }
    u32 invoke(const AdsiExport& contract, std::initializer_list<u64> args) {
        usize index = 0;
        for (u64 arg : args) {
            if (cpu.is_64bit()) {
                switch (index) {
                    case 0: cpu.gpr.rcx = arg; break;
                    case 1: cpu.gpr.rdx = arg; break;
                    case 2: cpu.gpr.r8 = arg; break;
                    case 3: cpu.gpr.r9 = arg; break;
                    default: EXPECT_TRUE(memory.write_u64(cpu.gpr.rsp + 8 + index * 8, arg).is_ok()); break;
                }
            } else EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + index * 4, static_cast<u32>(arg)).is_ok());
            ++index;
        }
        for (auto& entry : exports.entries) if (entry.name == contract.name) {
            EXPECT_EQ(entry.ordinal, contract.ordinal);
            EXPECT_EQ(entry.stdcall_bytes, contract.stack_bytes);
            entry.handler(m);
            return static_cast<u32>(cpu.gpr.rax);
        }
        ADD_FAILURE() << contract.name;
        return ~u32{0};
    }
    void clear() { invoke(kADsSetLastError, {0, 0, 0}); }
    void seed() {
        ASSERT_TRUE(m.write_wide_string(0x11000, "abcdef").is_ok());
        ASSERT_TRUE(m.write_wide_string(0x12000, "provider").is_ok());
        invoke(kADsSetLastError, {0x87654321, 0x11000, 0x12000});
    }
    void sentinels() {
        ASSERT_TRUE(memory.write_u32(0x13000, 0xcccccccc).is_ok());
        for (u32 i = 0; i < 16; ++i) {
            ASSERT_TRUE(memory.write_u16(0x14000 + i * 2, 0xcccc).is_ok());
            ASSERT_TRUE(memory.write_u16(0x15000 + i * 2, 0xcccc).is_ok());
        }
    }
};
}

TEST(Activeds, FreshAndClearedStatePreservesWin32LastError) {
    for (bool x64 : {false, true}) {
        GuestCall call(x64);
        call.clear();
        call.sentinels();
        set_last_error(0x12345678);
        for (u32 repeat = 0; repeat < 2; ++repeat) {
            ASSERT_EQ(call.invoke(kADsGetLastError, {0x13000, 0x14000, 8, 0x15000, 8}), kS_OK);
            EXPECT_EQ(call.memory.read_u32(0x13000).value(), 0u);
            EXPECT_EQ(call.memory.read_u16(0x14000).value(), 0u);
            EXPECT_EQ(call.memory.read_u16(0x14002).value(), 0xcccc);
            EXPECT_EQ(call.memory.read_u16(0x15000).value(), 0u);
            EXPECT_EQ(get_last_error(), 0x12345678u);
        }
    }
}

TEST(Activeds, TruncationTerminatesAndZeroCapacityDoesNotWrite) {
    for (bool x64 : {false, true}) {
        GuestCall call(x64);
        call.seed();
        for (u32 capacity : {0u, 1u, 3u, 8u}) {
            call.sentinels();
            ASSERT_EQ(call.invoke(kADsGetLastError, {0x13000, 0x14000, capacity, 0x15000, capacity}), kS_OK);
            EXPECT_EQ(call.memory.read_u32(0x13000).value(), 0x87654321u);
            for (const auto& sample : {std::pair{GuestAddr{0x14000}, std::u16string(u"abcdef")},
                                       std::pair{GuestAddr{0x15000}, std::u16string(u"provider")}}) {
                const usize length = capacity ? std::min(sample.second.size(), usize{capacity - 1}) : 0;
                for (usize i = 0; i < length; ++i)
                    EXPECT_EQ(call.memory.read_u16(sample.first + i * 2).value(), sample.second[i]);
                EXPECT_EQ(call.memory.read_u16(sample.first + length * 2).value(), capacity ? 0 : 0xcccc);
                EXPECT_EQ(call.memory.read_u16(sample.first + (length + 1) * 2).value(), 0xcccc);
            }
        }
        call.clear();
    }
}

TEST(Activeds, NullOutputValidationPrecedesAllWrites) {
    for (bool x64 : {false, true}) {
        GuestCall call(x64);
        call.seed();
        for (u32 missing : {0u, 1u, 3u}) for (u32 capacity : {0u, 3u}) {
            call.sentinels();
            set_last_error(0x12345678);
            EXPECT_EQ(call.invoke(kADsGetLastError, {missing == 0 ? 0u : 0x13000u,
                missing == 1 ? 0u : 0x14000u, capacity, missing == 3 ? 0u : 0x15000u, capacity}), kE_POINTER);
            EXPECT_EQ(call.memory.read_u32(0x13000).value(), 0xccccccccu);
            EXPECT_EQ(call.memory.read_u16(0x14000).value(), 0xcccc);
            EXPECT_EQ(call.memory.read_u16(0x15000).value(), 0xcccc);
            EXPECT_EQ(get_last_error(), 0x12345678u);
        }
        call.clear();
    }
}

TEST(Activeds, SetterCopiesCodeAndRawUtf16AndNullStringsBecomeEmpty) {
    for (bool x64 : {false, true}) {
        GuestCall call(x64);
        call.seed();
        set_last_error(0x12345678);
        // The Windows oracle retains supplied text even for Win32 code 5.
        call.invoke(kADsSetLastError, {5, 0x11000, 0});
        EXPECT_EQ(get_last_error(), 0x12345678u);
        ASSERT_EQ(call.invoke(kADsGetLastError, {0x13000, 0x14000, 8, 0x15000, 8}), kS_OK);
        EXPECT_EQ(call.memory.read_u32(0x13000).value(), 5u);
        EXPECT_EQ(call.memory.read_u16(0x14000).value(), u'a');
        EXPECT_EQ(call.memory.read_u16(0x15000).value(), 0u);
        ASSERT_TRUE(call.memory.write_u16(0x11000, 0xd83d).is_ok());
        ASSERT_TRUE(call.memory.write_u16(0x11002, 0xde00).is_ok());
        ASSERT_TRUE(call.memory.write_u16(0x11004, 0).is_ok());
        call.invoke(kADsSetLastError, {0x87654321, 0x11000, 0});
        ASSERT_EQ(call.invoke(kADsGetLastError, {0x13000, 0x14000, 2, 0x15000, 8}), kS_OK);
        EXPECT_EQ(call.memory.read_u16(0x14000).value(), 0xd83d);
        EXPECT_EQ(call.memory.read_u16(0x14002).value(), 0u);
        call.clear();
    }
}

TEST(Activeds, ErrorStateIsThreadLocal) {
    for (bool x64 : {false, true}) {
        GuestCall parent(x64);
        parent.seed();
        std::thread child([x64] {
            GuestCall call(x64);
            ASSERT_EQ(call.invoke(kADsGetLastError, {0x13000, 0x14000, 8, 0x15000, 8}), kS_OK);
            EXPECT_EQ(call.memory.read_u32(0x13000).value(), 0u);
            call.invoke(kADsSetLastError, {9, 0, 0});
        });
        child.join();
        ASSERT_EQ(parent.invoke(kADsGetLastError, {0x13000, 0x14000, 8, 0x15000, 8}), kS_OK);
        EXPECT_EQ(parent.memory.read_u32(0x13000).value(), 0x87654321u);
        parent.clear();
    }
}

TEST(Activeds, BindingFailuresPreserveOutputPointerAndDoNotClaimAnObject) {
    for (bool x64 : {false, true}) {
        GuestCall call(x64);
        ASSERT_TRUE(call.memory.write_u64(0x13000, 0xcccccccccccccccc).is_ok());
        ASSERT_TRUE(call.m.write_wide_string(0x11000, "").is_ok());
        set_last_error(0x12345678);
        EXPECT_EQ(call.invoke(kADsGetObject, {0, 0x12000, 0x13000}), kE_FAIL);
        EXPECT_EQ(call.invoke(kADsGetObject, {0x11000, 0x12000, 0x13000}), kE_FAIL);
        ASSERT_TRUE(call.m.write_wide_string(0x11000, "LDAP://server/object").is_ok());
        EXPECT_EQ(call.invoke(kADsGetObject, {0x11000, 0x12000, 0x13000}), kE_NOTIMPL);
        EXPECT_EQ(call.memory.read_u64(0x13000).value(), 0xccccccccccccccccu);
        EXPECT_EQ(get_last_error(), 0x12345678u);
    }
}
