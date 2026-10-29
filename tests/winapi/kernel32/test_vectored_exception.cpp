// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_vectored_exception.cpp
/// @brief Add/RemoveVectored{Exception,Continue}Handler lists (errhandlingapi.h;
///        rules measured by tools/oracle_probe/vectored_continue_probe)

#include <gtest/gtest.h>
#include <winapi/kernel32/process/vectored_exception.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

class VectoredExceptionTest : public ::testing::Test {
protected:
    void SetUp() override {
        clear_vectored_exception_handlers_for_test();
    }
    void TearDown() override {
        clear_vectored_exception_handlers_for_test();
    }
};

TEST_F(VectoredExceptionTest, NullHandlerIsRegistered) {
    // vectored_continue_probe add_veh_null / add_null: a NULL handler gets a
    // handle and is removable, on both lists.
    const u64 h = add_vectored_exception_handler(1, 0);
    ASSERT_NE(h, 0u);
    EXPECT_EQ(snapshot_vectored_exception_handlers().size(), 1u);
    EXPECT_TRUE(remove_vectored_exception_handler(h));
    const u64 c = add_vectored_continue_handler(0, 0);
    ASSERT_NE(c, 0u);
    EXPECT_TRUE(remove_vectored_continue_handler(c));
}

TEST_F(VectoredExceptionTest, RemoveUnknownOrNullFails) {
    EXPECT_FALSE(remove_vectored_exception_handler(0));
    EXPECT_FALSE(remove_vectored_exception_handler(0x100));
}

TEST_F(VectoredExceptionTest, FirstInsertsFrontLastAppends) {
    // Learn: First != 0 is called first; First == 0 is called last.
    const u64 h2 = add_vectored_exception_handler(1, 0x200);
    const u64 h3 = add_vectored_exception_handler(0, 0x300);
    const u64 h1 = add_vectored_exception_handler(1, 0x100);
    ASSERT_NE(h1, 0u);
    ASSERT_NE(h2, 0u);
    ASSERT_NE(h3, 0u);
    EXPECT_NE(h1, h2);
    EXPECT_NE(h2, h3);

    const auto order = snapshot_vectored_exception_handlers();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 0x100u);
    EXPECT_EQ(order[1], 0x200u);
    EXPECT_EQ(order[2], 0x300u);
}

TEST_F(VectoredExceptionTest, DuplicateHandlerGetsDistinctHandles) {
    const u64 a = add_vectored_exception_handler(0, 0xABC);
    const u64 b = add_vectored_exception_handler(0, 0xABC);
    ASSERT_NE(a, 0u);
    ASSERT_NE(b, 0u);
    EXPECT_NE(a, b);
    EXPECT_TRUE(remove_vectored_exception_handler(a));
    const auto left = snapshot_vectored_exception_handlers();
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0], 0xABCu);
    EXPECT_TRUE(remove_vectored_exception_handler(b));
    EXPECT_TRUE(snapshot_vectored_exception_handlers().empty());
}

TEST_F(VectoredExceptionTest, RemoveIsIdempotent) {
    const u64 h = add_vectored_exception_handler(0, 0x40);
    ASSERT_NE(h, 0u);
    EXPECT_TRUE(remove_vectored_exception_handler(h));
    EXPECT_FALSE(remove_vectored_exception_handler(h));
    EXPECT_TRUE(snapshot_vectored_exception_handlers().empty());
}

TEST_F(VectoredExceptionTest, ContinueListIsSeparateAndOrdered) {
    // vectored_continue_probe add / remove: a appended, b appended, c first
    // runs c a b; neither list's Remove accepts the other's handle.
    const u64 v = add_vectored_exception_handler(1, 0x10);
    const u64 a = add_vectored_continue_handler(0, 0xA);
    const u64 b = add_vectored_continue_handler(0, 0xB);
    const u64 c = add_vectored_continue_handler(1, 0xC);
    EXPECT_NE(a, v);
    EXPECT_NE(b, v);
    EXPECT_NE(c, v);
    EXPECT_EQ(snapshot_vectored_continue_handlers(), (std::vector<u64>{0xC, 0xA, 0xB}));
    EXPECT_EQ(snapshot_vectored_exception_handlers(), (std::vector<u64>{0x10}));
    EXPECT_FALSE(remove_vectored_continue_handler(v));
    EXPECT_FALSE(remove_vectored_exception_handler(a));
    EXPECT_FALSE(remove_vectored_continue_handler(0));
    EXPECT_TRUE(remove_vectored_continue_handler(a));
    EXPECT_FALSE(remove_vectored_continue_handler(a));
    EXPECT_EQ(snapshot_vectored_continue_handlers(), (std::vector<u64>{0xC, 0xB}));
}
