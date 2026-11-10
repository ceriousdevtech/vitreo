// Prepared acceptance tests for the measured Windows text extent memory contract.
// CMake registration/product implementation are intentionally owned separately.
#include <gtest/gtest.h>
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "../../../tools/oracle_probe/text_extent_output_contract.generated.hpp"

#include <algorithm>
#include <array>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;
namespace facts = text_extent_output_contract;

class TextExtentOutputContract : public ::testing::Test {
protected:
    static constexpr GuestAddr kStack = 0x10000;
    static constexpr GuestAddr kAnsi = 0x20000;
    static constexpr GuestAddr kWide = kAnsi + 0x100;
    static constexpr GuestAddr kOutput = 0x30000;
    static constexpr GuestAddr kReadonly = 0x40000;
    static constexpr GuestAddr kStraddleRegion = 0x50000;
    static constexpr usize kRegionBytes = 0x4000;
    static constexpr GuestAddr kStraddle =
        kStraddleRegion + kRegionBytes - facts::long_bytes;
    static constexpr GuestAddr kUnmapped = 0x60000;
    static constexpr Handle kInvalidDc = 0x12345678;

    HandleTable handles;
    GdiManager manager{handles};
    GuestMemory memory;
    windowing::CocoaBridge bridge;
    Handle display_dc = 0;
    Handle memory_dc = 0;

    void SetUp() override {
        manager.set_cocoa_bridge(&bridge);
        for (const GuestAddr base : {kStack, kAnsi, kOutput, kReadonly, kStraddleRegion}) {
            ASSERT_TRUE(memory.map_region(base, kRegionBytes,
                                         MemoryProtection::ReadWrite).is_ok());
        }
        const std::array<u8, 2> ansi{'M', 0};
        const std::array<u16, 2> wide{'M', 0};
        ASSERT_TRUE(memory.write_bytes(kAnsi, ansi.data(), sizeof(ansi)).is_ok());
        ASSERT_TRUE(memory.write_bytes(kWide, reinterpret_cast<const u8*>(wide.data()),
                                       sizeof(wide)).is_ok());
        display_dc = manager.get_dc(0);
        memory_dc = manager.create_compatible_dc(display_dc);
        ASSERT_NE(display_dc, 0u);
        ASSERT_NE(memory_dc, 0u);
    }

    struct Outcome {
        u32 result = 0, error = 0, code = 0, num_args = 0;
        bool raised = false;
        u64 operation = 0, address = 0;
    };

    Outcome call(const facts::Row& row, GuestAddr output) {
        auto exports = make_gdi32_exports(manager, {}, row.is64);
        const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
            [&row](const ExportEntry& value) { return value.name == row.api; });
        if (entry == exports.entries.end()) {
            ADD_FAILURE() << "Missing export " << row.api;
            return {};
        }
        const Handle dc = row.dc == "display" ? display_dc
                        : row.dc == "memory" ? memory_dc
                        : row.dc == "null" ? 0 : kInvalidDc;
        const GuestAddr source = row.null_string ? 0
            : row.api.back() == 'W' ? kWide : kAnsi;
        const std::array<u64, 4> args{
            dc, source, static_cast<u32>(row.count), output};
        cpu::CpuState state;
        if (row.is64) {
            state.init_long_mode();
            state.set_sp(kStack + 0x1000);
            state.gpr.rcx = args[0];
            state.gpr.rdx = args[1];
            state.gpr.r8 = args[2];
            state.gpr.r9 = args[3];
        } else {
            state.init_protected_mode();
            state.set_sp(kStack + 0x1000);
            for (usize i = 0; i < args.size(); ++i) {
                if (memory.write_u32(state.sp() + (i + 1) * sizeof(u32),
                                     static_cast<u32>(args[i])).is_err()) {
                    ADD_FAILURE() << "Could not marshal x86 arguments";
                    return {};
                }
            }
        }
        Marshaler marshaler(memory);
        marshaler.attach_cpu(state);
        set_last_error(facts::last_error_seed);
        entry->handler(marshaler);
        Outcome observed;
        observed.result = static_cast<u32>(state.gpr.rax);
        observed.error = get_last_error();
        observed.raised = state.pending_raise.active;
        observed.code = state.pending_raise.code;
        observed.num_args = state.pending_raise.num_args;
        observed.operation = state.pending_raise.args[0];
        observed.address = state.pending_raise.args[1];
        return observed;
    }

    void check(const facts::Row& row) {
        SCOPED_TRACE(std::string(row.api) + " abi=" + (row.is64 ? "64" : "32") +
                     " dc=" + std::string(row.dc) + " count=" + std::to_string(row.count) +
                     " null_string=" + std::to_string(row.null_string) +
                     " output=" + std::string(row.output));
        ASSERT_TRUE(memory.protect_region(kReadonly, kRegionBytes,
                                         MemoryProtection::ReadWrite).is_ok());
        std::array<u8, facts::size_bytes> sentinel;
        sentinel.fill(facts::output_sentinel);
        ASSERT_TRUE(memory.write_bytes(kOutput, sentinel.data(), sentinel.size()).is_ok());
        ASSERT_TRUE(memory.write_bytes(kReadonly, sentinel.data(), sentinel.size()).is_ok());
        ASSERT_TRUE(memory.write_bytes(kStraddle, sentinel.data(), facts::long_bytes).is_ok());
        ASSERT_TRUE(memory.protect_region(kReadonly, kRegionBytes,
                                         MemoryProtection::Read).is_ok());

        const GuestAddr output = row.output == "valid" ? kOutput
            : row.output == "null" ? 0
            : row.output == "readonly" ? kReadonly
            : row.output == "straddle" ? kStraddle : kUnmapped;
        const auto observed = call(row, output);
        const bool faulted = row.exception != 0;
        EXPECT_EQ(observed.raised, faulted);
        if (faulted) {
            EXPECT_EQ(observed.code, row.exception);
            EXPECT_EQ(observed.code, facts::access_violation);
            EXPECT_EQ(observed.num_args, 2u);
            EXPECT_EQ(observed.operation, row.operation);
            const GuestAddr base = row.operation != 0 ? output : 0;
            EXPECT_EQ(observed.address, base + static_cast<u64>(row.fault_offset));
        } else {
            ASSERT_TRUE(row.returned);
            EXPECT_EQ(observed.result, row.result);
        }
        // Do not freeze successful font measurements or ambient success
        // LastError behavior. Failure and exception errors are oracle-derived.
        if (faulted || row.result == 0) EXPECT_EQ(observed.error, row.error);

        std::array<u8, facts::size_bytes> bytes{};
        if (row.byte_count != 0) {
            ASSERT_TRUE(memory.read_bytes(output, bytes.data(), row.byte_count).is_ok());
            unsigned changed = 0;
            for (usize i = 0; i < row.byte_count; ++i) {
                EXPECT_EQ(bytes[i], row.bytes[i]) << "output byte " << i;
                if (bytes[i] != facts::output_sentinel) ++changed;
            }
            EXPECT_EQ(changed, row.changed);
        }
    }
};

TEST_F(TextExtentOutputContract, MatchesMeasuredZeroFailureAndFaultRowsInBothAbis) {
    // No projected row contains a successful positive-count metric or its
    // last error. Valid source M is used only to reach output failure paths.
    for (const auto& row : facts::rows) check(row);
}
