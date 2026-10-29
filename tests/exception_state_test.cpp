// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include <gtest/gtest.h>
#include "core/cpu/state/exception_state.hpp"

using namespace vitreo::cpu;

// ===========================================================================
// ExceptionState::clear()
// ===========================================================================

TEST(ExceptionStateTest, ClearResetsAll) {
    ExceptionState es;
    es.pending_raise.active = true;
    es.pending_raise.code = 0xC0000005;
    es.pending_raise.flags = 1;
    es.pending_raise.num_args = 2;
    es.pending_unwind.active = true;
    es.pending_unwind.target_ip = 0x401000;
    es.pending_unwind.target_frame = 0x7FFE0000;

    es.clear();

    EXPECT_FALSE(es.pending_raise.active);
    EXPECT_EQ(es.pending_raise.code, 0u);
    EXPECT_EQ(es.pending_raise.flags, 0u);
    EXPECT_EQ(es.pending_raise.num_args, 0u);
    EXPECT_FALSE(es.pending_unwind.active);
    EXPECT_EQ(es.pending_unwind.target_ip, 0u);
    EXPECT_EQ(es.pending_unwind.target_frame, 0u);
}

// ===========================================================================
// PendingRaise active/clear cycle
// ===========================================================================

TEST(ExceptionStateTest, PendingRaiseDefaultInactive) {
    ExceptionState es;
    EXPECT_FALSE(es.pending_raise.active);
    EXPECT_EQ(es.pending_raise.code, 0u);
}

TEST(ExceptionStateTest, PendingRaiseActivateThenClear) {
    ExceptionState::PendingRaise pr;
    pr.active = true;
    pr.code = 0xC0000005;
    pr.flags = 0;
    pr.num_args = 2;
    pr.args[0] = 0;
    pr.args[1] = 0x00401000;

    EXPECT_TRUE(pr.active);
    EXPECT_EQ(pr.code, 0xC0000005u);
    EXPECT_EQ(pr.num_args, 2u);

    pr.clear();

    EXPECT_FALSE(pr.active);
    EXPECT_EQ(pr.code, 0u);
    EXPECT_EQ(pr.flags, 0u);
    EXPECT_EQ(pr.num_args, 0u);
}

// ===========================================================================
// PendingUnwind active/clear cycle
// ===========================================================================

TEST(ExceptionStateTest, PendingUnwindDefaultInactive) {
    ExceptionState es;
    EXPECT_FALSE(es.pending_unwind.active);
    EXPECT_EQ(es.pending_unwind.target_ip, 0u);
    EXPECT_EQ(es.pending_unwind.target_frame, 0u);
}

TEST(ExceptionStateTest, PendingUnwindActivateThenClear) {
    ExceptionState::PendingUnwind pu;
    pu.active = true;
    pu.target_ip = 0x00401000;
    pu.target_frame = 0x7FFE0000;

    EXPECT_TRUE(pu.active);
    EXPECT_EQ(pu.target_ip, 0x00401000u);
    EXPECT_EQ(pu.target_frame, 0x7FFE0000u);

    pu.clear();

    EXPECT_FALSE(pu.active);
    EXPECT_EQ(pu.target_ip, 0u);
    EXPECT_EQ(pu.target_frame, 0u);
}

// ===========================================================================
// Independent clear — clearing raise doesn't affect unwind
// ===========================================================================

TEST(ExceptionStateTest, ClearRaiseDoesNotAffectUnwind) {
    ExceptionState es;
    es.pending_raise.active = true;
    es.pending_raise.code = 1;
    es.pending_unwind.active = true;
    es.pending_unwind.target_ip = 0xDEAD;

    es.pending_raise.clear();

    EXPECT_FALSE(es.pending_raise.active);
    EXPECT_TRUE(es.pending_unwind.active);
    EXPECT_EQ(es.pending_unwind.target_ip, 0xDEADu);
}
