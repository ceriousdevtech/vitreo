#include "core/cpu/seh/seh.hpp"
#include <gtest/gtest.h>

using namespace vitreo;
using namespace vitreo::cpu;

class SehTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_seh = std::make_unique<SehManager>();
    }
    std::unique_ptr<SehManager> m_seh;
};

// Test 1: Add function table
TEST_F(SehTest, AddFunctionTable) {
    RuntimeFunction funcs[] = {
        {0x1000, 0x1100, 0x2000},
        {0x1100, 0x1200, 0x2100},
    };
    EXPECT_TRUE(m_seh->add_function_table(funcs, 2, 0x400000));
    EXPECT_EQ(m_seh->function_count(), 2u);
}

// Test 2: Lookup function entry by RIP
TEST_F(SehTest, LookupFindsEntry) {
    RuntimeFunction funcs[] = {
        {0x1000, 0x1100, 0x2000},
    };
    m_seh->add_function_table(funcs, 1, 0x400000);
    
    GuestAddr image_base = 0;
    auto* found = m_seh->lookup_function_entry(0x401050, image_base);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->begin_address, 0x1000u);
    EXPECT_EQ(image_base, 0x400000u);
}

// Test 3: Lookup returns null for unknown RIP
TEST_F(SehTest, LookupReturnsNullUnknown) {
    GuestAddr image_base = 0;
    auto* found = m_seh->lookup_function_entry(0x999999, image_base);
    EXPECT_EQ(found, nullptr);
}

// Test 4: Delete function table
TEST_F(SehTest, DeleteFunctionTable) {
    RuntimeFunction funcs[] = {{0x1000, 0x1100, 0x2000}};
    m_seh->add_function_table(funcs, 1, 0x400000);
    EXPECT_TRUE(m_seh->delete_function_table(0x400000));
    EXPECT_EQ(m_seh->function_count(), 0u);
}

// Test 5: RaiseException with handler
TEST_F(SehTest, RaiseExceptionWithHandler) {
    bool handler_called = false;
    m_seh->set_unhandled_exception_filter([&handler_called](const ExceptionRecord& rec) -> i32 {
        handler_called = true;
        EXPECT_EQ(rec.exception_code, EXCEPTION_ACCESS_VIOLATION);
        return EXCEPTION_CONTINUE_EXECUTION;
    });
    
    m_seh->raise_exception(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr, 0x401000);
    EXPECT_TRUE(handler_called);
}

// Test 6: SetUnhandledExceptionFilter stores and returns previous
TEST_F(SehTest, SetFilterReturnsPrevious) {
    auto prev = m_seh->set_unhandled_exception_filter([](const ExceptionRecord&) -> i32 {
        return EXCEPTION_CONTINUE_SEARCH;
    });
    EXPECT_FALSE(prev); // No previous
    
    auto prev2 = m_seh->set_unhandled_exception_filter(nullptr);
    EXPECT_TRUE(prev2); // Previous was set
}

// Test 7: Continue execution
TEST_F(SehTest, ContinueExecution) {
    m_seh->set_unhandled_exception_filter([](const ExceptionRecord&) -> i32 {
        return EXCEPTION_CONTINUE_EXECUTION;
    });
    
    ExceptionRecord rec;
    rec.exception_code = EXCEPTION_INT_DIVIDE_BY_ZERO;
    rec.exception_address = 0x401000;
    
    i32 result = m_seh->dispatch_exception(rec);
    EXPECT_EQ(result, EXCEPTION_CONTINUE_EXECUTION);
}

// Test 8: Continue search
TEST_F(SehTest, ContinueSearch) {
    ExceptionRecord rec;
    rec.exception_code = EXCEPTION_ACCESS_VIOLATION;
    rec.exception_address = 0x401000;
    
    i32 result = m_seh->dispatch_exception(rec);
    EXPECT_EQ(result, EXCEPTION_CONTINUE_SEARCH);
}

// Test 9: Exception record has correct code
TEST_F(SehTest, ExceptionRecordCorrectCode) {
    u32 captured_code = 0;
    m_seh->set_unhandled_exception_filter([&captured_code](const ExceptionRecord& rec) -> i32 {
        captured_code = rec.exception_code;
        return EXCEPTION_CONTINUE_SEARCH;
    });
    
    m_seh->raise_exception(EXCEPTION_STACK_OVERFLOW, 0, 0, nullptr, 0x401000);
    EXPECT_EQ(captured_code, EXCEPTION_STACK_OVERFLOW);
}

// Test 10: Null table rejected
TEST_F(SehTest, NullTableRejected) {
    EXPECT_FALSE(m_seh->add_function_table(nullptr, 0, 0x400000));
}
