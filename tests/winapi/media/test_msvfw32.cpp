/// @file test_msvfw32.cpp
/// @brief ICInfo / ICOpen / ICSendMessage against Drivers32 mappings.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/media/msvfw32/msvfw32.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

class Msvfw32Test : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(MEM_BASE, 0x10000,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(STACK_BASE, 0x1000,
                                       MemoryProtection::ReadWrite).is_ok());
        cpu_.init_protected_mode();
        registry_ = std::make_unique<ThunkRegistry>(0x70000000);
        mgr_.add_driver(kVidc, kVp60, "vp6vfw.dll");
        auto exports = create_msvfw32_thunks(mgr_);
        ASSERT_TRUE(registry_->register_dll("msvfw32.dll", std::move(exports)).is_ok());
    }

    void set_args(const std::vector<u32>& args) {
        const GuestAddr frame = STACK_BASE + 0x400;
        cpu_.set_sp(frame);
        (void)memory_.write_u32(frame, 0xDEADBEEF);
        for (usize i = 0; i < args.size(); ++i) {
            (void)memory_.write_u32(
                frame + 4 + static_cast<GuestAddr>(i * 4), args[i]);
        }
    }

    [[nodiscard]] Marshaler make_marshaler() {
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        return m;
    }

    [[nodiscard]] u32 return_value() const {
        return static_cast<u32>(cpu_.gpr.rax & 0xFFFFFFFFu);
    }

    static constexpr GuestAddr MEM_BASE = 0x10000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr INFO_ADDR = MEM_BASE + 0x100;
    static constexpr u32 kVidc = 0x63646976;
    static constexpr u32 kVp60 = 0x30365056;

    GuestMemory memory_;
    cpu::CpuState cpu_;
    Msvfw32Manager mgr_;
    std::unique_ptr<ThunkRegistry> registry_;
};

TEST_F(Msvfw32Test, IcInfoEnumeratesSeededDriver) {
    (void)memory_.write_u32(INFO_ADDR, 568);
    set_args({kVidc, 0, static_cast<u32>(INFO_ADDR)});
    auto m = make_marshaler();
    mgr_.impl_ICInfo(m);
    EXPECT_EQ(return_value(), 1u);
    auto handler = memory_.read_u32(INFO_ADDR + 8);
    ASSERT_TRUE(handler.is_ok());
    EXPECT_EQ(handler.value(), kVp60);
}

TEST_F(Msvfw32Test, IcInfoByFourcc) {
    (void)memory_.write_u32(INFO_ADDR, 568);
    set_args({kVidc, kVp60, static_cast<u32>(INFO_ADDR)});
    auto m = make_marshaler();
    mgr_.impl_ICInfo(m);
    EXPECT_EQ(return_value(), 1u);
}

TEST_F(Msvfw32Test, IcInfoUnknownReturnsFalse) {
    (void)memory_.write_u32(INFO_ADDR, 568);
    set_args({kVidc, 0x30385058u, static_cast<u32>(INFO_ADDR)}); // 'XP80'
    auto m = make_marshaler();
    mgr_.impl_ICInfo(m);
    EXPECT_EQ(return_value(), 0u);
}

TEST_F(Msvfw32Test, IcOpenReturnsHandleAndSendMessageIsUnsupported) {
    set_args({kVidc, kVp60, 1}); // ICMODE_DECOMPRESS
    {
        auto m = make_marshaler();
        mgr_.impl_ICOpen(m);
    }
    EXPECT_EQ(return_value(), 0u); // GuestCall required for DRV_OPEN

    std::vector<u32> msgs;
    mgr_.set_guest_call([&](GuestAddr proc, const u64* args, usize count,
                            bool is_64bit) -> u64 {
        EXPECT_EQ(proc, 0xBEEF0001u);
        EXPECT_EQ(count, 5u);
        EXPECT_FALSE(is_64bit);
        msgs.push_back(static_cast<u32>(args[2]));
        if (args[2] == 3) { // DRV_OPEN
            EXPECT_EQ(args[3], 0u);
            EXPECT_NE(args[4], 0u);
            return 0x11223344;
        }
        if (args[2] == 0x5002) { // ICM_GETINFO
            EXPECT_EQ(args[0], 0x11223344u);
            return 0;
        }
        return 1;
    });

    set_args({kVidc, kVp60, 1});
    {
        auto m = make_marshaler();
        mgr_.impl_ICOpen(m);
    }
    const u32 hic = return_value();
    EXPECT_NE(hic, 0u);
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[0], 1u); // DRV_LOAD
    EXPECT_EQ(msgs[1], 2u); // DRV_ENABLE
    EXPECT_EQ(msgs[2], 3u); // DRV_OPEN

    set_args({hic, 0x5002u, static_cast<u32>(INFO_ADDR), 568}); // ICM_GETINFO
    {
        auto m = make_marshaler();
        mgr_.impl_ICSendMessage(m);
    }
    EXPECT_EQ(return_value(), 0u);

    set_args({hic});
    {
        auto m = make_marshaler();
        mgr_.impl_ICClose(m);
    }
    EXPECT_EQ(return_value(), 0u); // ICERR_OK
    ASSERT_EQ(msgs.size(), 7u);
    EXPECT_EQ(msgs[3], 0x5002u);
    EXPECT_EQ(msgs[4], 4u); // DRV_CLOSE
    EXPECT_EQ(msgs[5], 5u); // DRV_DISABLE
    EXPECT_EQ(msgs[6], 6u); // DRV_FREE
}

TEST_F(Msvfw32Test, IcOpenFailsWhenDrvLoadReturnsZero) {
    std::vector<u32> msgs;
    mgr_.set_guest_call([&](GuestAddr, const u64* args, usize, bool) -> u64 {
        msgs.push_back(static_cast<u32>(args[2]));
        if (args[2] == 1) { // DRV_LOAD
            return 0;
        }
        return 1;
    });
    set_args({kVidc, kVp60, 1});
    {
        auto m = make_marshaler();
        mgr_.impl_ICOpen(m);
    }
    EXPECT_EQ(return_value(), 0u);
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0], 1u);
}

TEST_F(Msvfw32Test, IcOpenFailsWhenDrvOpenReturnsZero) {
    mgr_.set_guest_call([](GuestAddr, const u64* args, usize, bool) -> u64 {
        if (args[2] == 3) { // DRV_OPEN
            return 0;
        }
        return 1;
    });
    set_args({kVidc, kVp60, 1});
    {
        auto m = make_marshaler();
        mgr_.impl_ICOpen(m);
    }
    EXPECT_EQ(return_value(), 0u);
}

TEST_F(Msvfw32Test, DrawDibOpenFailsHonestly) {
    set_args({});
    auto m = make_marshaler();
    mgr_.impl_DrawDibOpen(m);
    EXPECT_EQ(return_value(), 0u);
}

// Learn, ICLocate: a candidate is kept only when it answers the mode's query
// message (ICM_DECOMPRESS_QUERY for ICMODE_DECOMPRESS) with ICERR_OK.
TEST_F(Msvfw32Test, IcLocateKeepsOnlyADriverThatAcceptsTheQuery) {
    bool accept = false;
    std::vector<u32> msgs;
    mgr_.set_guest_call([&](GuestAddr, const u64* args, usize, bool) -> u64 {
        msgs.push_back(static_cast<u32>(args[2]));
        if (args[2] == 3) return 0x77; // DRV_OPEN
        if (args[2] == 0x4000 + 11) {  // ICM_DECOMPRESS_QUERY(lpbiIn, lpbiOut)
            EXPECT_EQ(args[3], static_cast<u64>(INFO_ADDR));
            EXPECT_EQ(args[4], 0u);
            return accept ? 0 : static_cast<u64>(static_cast<i64>(-2)); // ICERR_BADFORMAT
        }
        return 1;
    });
    set_args({kVidc, kVp60, static_cast<u32>(INFO_ADDR), 0, 2}); // ICMODE_DECOMPRESS
    {
        auto m = make_marshaler();
        mgr_.impl_ICLocate(m);
    }
    EXPECT_EQ(return_value(), 0u);
    EXPECT_NE(std::find(msgs.begin(), msgs.end(), 4u), msgs.end()); // refused: DRV_CLOSE
    accept = true;
    set_args({kVidc, kVp60, static_cast<u32>(INFO_ADDR), 0, 2});
    {
        auto m = make_marshaler();
        mgr_.impl_ICLocate(m);
    }
    EXPECT_NE(return_value(), 0u);
}

// ICDecompress (WINAPIV) sends ICM_DECOMPRESS with an ICDECOMPRESS built from
// its arguments (vfw.h: dwFlags, lpbiInput, lpInput, lpbiOutput, lpOutput,
// ckid; 24 bytes on x86).
TEST_F(Msvfw32Test, IcDecompressBuildsTheIcdecompress) {
    u64 seen_size = 0;
    std::vector<u32> fields;
    mgr_.set_guest_call([&](GuestAddr, const u64* args, usize, bool) -> u64 {
        if (args[2] == 3) return 0x77;
        if (args[2] == 0x4000 + 13) {
            seen_size = args[4];
            for (u32 i = 0; i < 6; ++i) {
                fields.push_back(memory_.read_u32(static_cast<GuestAddr>(args[3]) + i * 4).value());
            }
            return 0;
        }
        return 1;
    });
    set_args({kVidc, kVp60, 2});
    {
        auto m = make_marshaler();
        mgr_.impl_ICOpen(m);
    }
    const u32 hic = return_value();
    ASSERT_NE(hic, 0u);
    set_args({hic, 0x08000000u, 0x11110, 0x12220, 0x13330, 0x14440});
    {
        auto m = make_marshaler();
        mgr_.impl_ICDecompress(m);
    }
    EXPECT_EQ(return_value(), 0u);
    EXPECT_EQ(seen_size, 24u);
    EXPECT_EQ(fields, (std::vector<u32>{0x08000000u, 0x11110, 0x12220, 0x13330, 0x14440, 0}));
    EXPECT_EQ(cpu_.sp(), STACK_BASE + 0x400u); // the scratch reservation is undone
}
