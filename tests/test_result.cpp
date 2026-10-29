/// @file test_result.cpp
/// @brief Unit tests for Result<T, E> type

#include <gtest/gtest.h>
#include <string>
#include <vitreo/result.hpp>
#include <vitreo/types.hpp>
#include <vitreo/common.hpp>

using namespace vitreo;

// =============================================================================
// Basic Result<T, E> Tests
// =============================================================================

TEST(ResultTest, OkReturnsValue) {
    auto result = Result<int, Error>::ok(42);
    EXPECT_TRUE(result.is_ok());
    EXPECT_FALSE(result.is_err());
    EXPECT_EQ(result.value(), 42);
}

TEST(ResultTest, ErrReturnsError) {
    auto result = Result<int, Error>::err(Error::NotFound);
    EXPECT_FALSE(result.is_ok());
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::NotFound);
}

TEST(ResultTest, BoolConversion) {
    auto ok_result = Result<int, Error>::ok(1);
    auto err_result = Result<int, Error>::err(Error::InvalidArgument);
    
    EXPECT_TRUE(static_cast<bool>(ok_result));
    EXPECT_FALSE(static_cast<bool>(err_result));
}

// =============================================================================
// value_or Tests
// =============================================================================

TEST(ResultTest, ValueOrReturnsValueWhenOk) {
    auto result = Result<int, Error>::ok(42);
    EXPECT_EQ(result.value_or(0), 42);
}

TEST(ResultTest, ValueOrReturnsDefaultWhenErr) {
    auto result = Result<int, Error>::err(Error::NotFound);
    EXPECT_EQ(result.value_or(99), 99);
}

// =============================================================================
// map Tests
// =============================================================================

TEST(ResultTest, MapTransformsValue) {
    auto result = Result<int, Error>::ok(10);
    auto mapped = result.map([](int x) { return x * 2; });
    
    EXPECT_TRUE(mapped.is_ok());
    EXPECT_EQ(mapped.value(), 20);
}

TEST(ResultTest, MapPropagatesError) {
    auto result = Result<int, Error>::err(Error::NotFound);
    auto mapped = result.map([](int x) { return x * 2; });
    
    EXPECT_TRUE(mapped.is_err());
    EXPECT_EQ(mapped.error(), Error::NotFound);
}

TEST(ResultTest, MapChangesType) {
    auto result = Result<int, Error>::ok(42);
    auto mapped = result.map([](int x) { return std::to_string(x); });
    
    EXPECT_TRUE(mapped.is_ok());
    EXPECT_EQ(mapped.value(), "42");
}

// =============================================================================
// and_then Tests
// =============================================================================

TEST(ResultTest, AndThenChainsOk) {
    auto divide = [](int x) -> Result<int, Error> {
        if (x == 0) return Result<int, Error>::err(Error::DivideByZero);
        return Result<int, Error>::ok(100 / x);
    };
    
    auto result = Result<int, Error>::ok(10);
    auto chained = result.and_then(divide);
    
    EXPECT_TRUE(chained.is_ok());
    EXPECT_EQ(chained.value(), 10);
}

TEST(ResultTest, AndThenPropagatesFirstError) {
    auto divide = [](int x) -> Result<int, Error> {
        if (x == 0) return Result<int, Error>::err(Error::DivideByZero);
        return Result<int, Error>::ok(100 / x);
    };
    
    auto result = Result<int, Error>::err(Error::NotFound);
    auto chained = result.and_then(divide);
    
    EXPECT_TRUE(chained.is_err());
    EXPECT_EQ(chained.error(), Error::NotFound);
}

TEST(ResultTest, AndThenPropagatesChainedError) {
    auto divide = [](int x) -> Result<int, Error> {
        if (x == 0) return Result<int, Error>::err(Error::DivideByZero);
        return Result<int, Error>::ok(100 / x);
    };
    
    auto result = Result<int, Error>::ok(0);
    auto chained = result.and_then(divide);
    
    EXPECT_TRUE(chained.is_err());
    EXPECT_EQ(chained.error(), Error::DivideByZero);
}

// =============================================================================
// Result<void, E> Tests
// =============================================================================

TEST(ResultVoidTest, OkIsOk) {
    auto result = Result<void, Error>::ok();
    EXPECT_TRUE(result.is_ok());
    EXPECT_FALSE(result.is_err());
}

TEST(ResultVoidTest, ErrIsErr) {
    auto result = Result<void, Error>::err(Error::AccessDenied);
    EXPECT_FALSE(result.is_ok());
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::AccessDenied);
}

// =============================================================================
// Copy and Move Tests
// =============================================================================

TEST(ResultTest, CopyConstructor) {
    auto original = Result<int, Error>::ok(42);
    auto copy = original;
    
    EXPECT_TRUE(copy.is_ok());
    EXPECT_EQ(copy.value(), 42);
    EXPECT_EQ(original.value(), 42);  // Original unchanged
}

TEST(ResultTest, MoveConstructor) {
    auto original = Result<std::string, Error>::ok("hello");
    auto moved = std::move(original);
    
    EXPECT_TRUE(moved.is_ok());
    EXPECT_EQ(moved.value(), "hello");
}

TEST(ResultTest, CopyAssignment) {
    auto a = Result<int, Error>::ok(1);
    auto b = Result<int, Error>::ok(2);
    b = a;
    
    EXPECT_EQ(b.value(), 1);
}

TEST(ResultTest, MoveAssignment) {
    auto a = Result<std::string, Error>::ok("test");
    auto b = Result<std::string, Error>::err(Error::NotFound);
    b = std::move(a);
    
    EXPECT_TRUE(b.is_ok());
    EXPECT_EQ(b.value(), "test");
}

// =============================================================================
// VResult Alias Tests
// =============================================================================

TEST(VResultTest, VResultAlias) {
    VResult<int> result = VResult<int>::ok(100);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 100);
}

TEST(VoidResultTest, VoidResultAlias) {
    VoidResult result = VoidResult::ok();
    EXPECT_TRUE(result.is_ok());
}

// =============================================================================
// Error String Tests
// =============================================================================

TEST(ErrorTest, ErrorToString) {
    EXPECT_STREQ(error_to_string(Error::None), "None");
    EXPECT_STREQ(error_to_string(Error::NotFound), "NotFound");
    EXPECT_STREQ(error_to_string(Error::DivideByZero), "DivideByZero");
}

// =============================================================================
// Types Tests
// =============================================================================

TEST(TypesTest, SizeAssertions) {
    EXPECT_EQ(sizeof(u8), 1);
    EXPECT_EQ(sizeof(u16), 2);
    EXPECT_EQ(sizeof(u32), 4);
    EXPECT_EQ(sizeof(u64), 8);
    EXPECT_EQ(sizeof(i8), 1);
    EXPECT_EQ(sizeof(i16), 2);
    EXPECT_EQ(sizeof(i32), 4);
    EXPECT_EQ(sizeof(i64), 8);
    EXPECT_EQ(sizeof(f32), 4);
    EXPECT_EQ(sizeof(f64), 8);
}

TEST(TypesTest, GuestAddressTypes) {
    EXPECT_EQ(sizeof(GuestAddr32), 4);
    EXPECT_EQ(sizeof(GuestAddr64), 8);
    EXPECT_EQ(sizeof(GuestAddr), 8);
}

// =============================================================================
// Common Utility Tests
// =============================================================================

TEST(CommonTest, AlignUp) {
    EXPECT_EQ(align_up<u64>(0, 4096), 0u);
    EXPECT_EQ(align_up<u64>(1, 4096), 4096u);
    EXPECT_EQ(align_up<u64>(4095, 4096), 4096u);
    EXPECT_EQ(align_up<u64>(4096, 4096), 4096u);
    EXPECT_EQ(align_up<u64>(4097, 4096), 8192u);
}

TEST(CommonTest, AlignDown) {
    EXPECT_EQ(align_down<u64>(0, 4096), 0u);
    EXPECT_EQ(align_down<u64>(1, 4096), 0u);
    EXPECT_EQ(align_down<u64>(4095, 4096), 0u);
    EXPECT_EQ(align_down<u64>(4096, 4096), 4096u);
    EXPECT_EQ(align_down<u64>(4097, 4096), 4096u);
}

TEST(CommonTest, IsAligned) {
    EXPECT_TRUE(is_aligned<u64>(0, 4096));
    EXPECT_FALSE(is_aligned<u64>(1, 4096));
    EXPECT_TRUE(is_aligned<u64>(4096, 4096));
    EXPECT_FALSE(is_aligned<u64>(4097, 4096));
}

TEST(CommonTest, IsPowerOfTwo) {
    EXPECT_FALSE(is_power_of_two(0));
    EXPECT_TRUE(is_power_of_two(1));
    EXPECT_TRUE(is_power_of_two(2));
    EXPECT_FALSE(is_power_of_two(3));
    EXPECT_TRUE(is_power_of_two(4));
    EXPECT_TRUE(is_power_of_two(4096));
}
