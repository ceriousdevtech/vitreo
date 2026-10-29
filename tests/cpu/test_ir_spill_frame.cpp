// SPDX-License-Identifier: LGPL-2.1-only
#include <core/cpu/jit/ir/ir_spill_frame.hpp>
#include <core/cpu/jit/ir/ir_passes.hpp>
#include <gtest/gtest.h>

namespace vitreo::cpu::jit::ir {

TEST(IrSpillFrame, NestedEntryPreservesOuterValuesAndRestoresPointer) {
    SpillFrame* active = nullptr;
    {
        SpillFrameScope outer(active);
        auto* outer_frame = active;
        ASSERT_NE(active, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(active) % 16, 0u);
        active->slots[0].lanes[0] = 0x0123456789ABCDEFull;
        active->slots[SpillFrame::kSlotCount - 1].lanes[1] = 0xFEDCBA9876543210ull;
        {
            SpillFrameScope inner(active);
            ASSERT_NE(active, outer_frame);
            active->slots[0].lanes[0] = 7;
            active->slots[SpillFrame::kSlotCount - 1].lanes[1] = 9;
        }
        ASSERT_EQ(active, outer_frame);
        EXPECT_EQ(active->slots[0].lanes[0], 0x0123456789ABCDEFull);
        EXPECT_EQ(active->slots[SpillFrame::kSlotCount - 1].lanes[1],
                  0xFEDCBA9876543210ull);
    }
    EXPECT_EQ(active, nullptr);
}

TEST(IrSpillFrame, EarlyReturnRestoresExistingInvocation) {
    SpillFrame backing;
    SpillFrame* active = &backing;
    const auto nested = [&] {
        SpillFrameScope scope(active);
        EXPECT_NE(active, &backing);
        return;
    };
    nested();
    EXPECT_EQ(active, &backing);
}

TEST(IrSpillFrame, PlannerReusesDeadSlotsAcrossMixedWidths) {
    IrBlock block;
    block.is_64bit = true;
    block.entry_rip = 0x100010000ull;
    IrBuilder builder(block);
    for (std::size_t i = 0; i < SpillFrame::kSlotCount * 3; ++i) {
        builder.set_rip(block.entry_rip + i * 5);
        const auto width = (i & 1) ? Type::I32 : Type::I64;
        const auto value = builder.constant(width, i);
        builder.gpr_write(GuestReg{static_cast<u8>(i % 16), width}, value);
    }
    std::vector<u8> slots(block.next_value, 0xFF);
    ASSERT_TRUE(plan_spill_slots(block, &slots));
    for (const auto& instruction : block.insts) {
        if (instruction.defines_value()) EXPECT_EQ(slots[instruction.result], 0u);
    }
}

TEST(IrSpillFrame, PlannerAcceptsCapacityAndRejectsOneMoreLiveValue) {
    for (std::size_t count : {SpillFrame::kSlotCount, SpillFrame::kSlotCount + 1}) {
        IrBlock block;
        block.is_64bit = true;
        block.entry_rip = 0x100010000ull;
        IrBuilder builder(block);
        builder.set_rip(block.entry_rip);
        std::vector<ValueId> values;
        for (std::size_t i = 0; i < count; ++i)
            values.push_back(builder.constant(Type::I64, i));
        // Every definition remains live until after the last definition.
        for (auto value : values)
            builder.gpr_write(GuestReg{0, Type::I64}, value);
        std::vector<u8> slots(block.next_value, 0xFF);
        EXPECT_EQ(plan_spill_slots(block, &slots), count <= SpillFrame::kSlotCount);
        if (count <= SpillFrame::kSlotCount) {
            for (std::size_t i = 0; i < count; ++i)
                EXPECT_EQ(slots[values[i]], i);
        }
    }
}

} // namespace vitreo::cpu::jit::ir
