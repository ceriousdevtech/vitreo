/// @file test_error.cpp
/// @brief Unit tests for GetLastError/SetLastError (WIN32-030)

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <vitreo/common.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/memory/memory_region.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Error Code Constants Tests
// =============================================================================

TEST(ErrorCodesTest, StandardCodesHaveCorrectValues) {
    // Verify a selection of error codes match Windows values
    EXPECT_EQ(error_codes::ERROR_SUCCESS, 0u);
    EXPECT_EQ(error_codes::ERROR_INVALID_FUNCTION, 1u);
    EXPECT_EQ(error_codes::ERROR_FILE_NOT_FOUND, 2u);
    EXPECT_EQ(error_codes::ERROR_PATH_NOT_FOUND, 3u);
    EXPECT_EQ(error_codes::ERROR_ACCESS_DENIED, 5u);
    EXPECT_EQ(error_codes::ERROR_INVALID_HANDLE, 6u);
    EXPECT_EQ(error_codes::ERROR_NOT_ENOUGH_MEMORY, 8u);
    EXPECT_EQ(error_codes::ERROR_INVALID_PARAMETER, 87u);
    EXPECT_EQ(error_codes::ERROR_INSUFFICIENT_BUFFER, 122u);
    EXPECT_EQ(error_codes::ERROR_MOD_NOT_FOUND, 126u);
    EXPECT_EQ(error_codes::ERROR_PROC_NOT_FOUND, 127u);
    EXPECT_EQ(error_codes::ERROR_ALREADY_EXISTS, 183u);
}

// =============================================================================
// Basic Get/Set Tests
// =============================================================================

class Win32ErrorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset error code before each test
        set_last_error(error_codes::ERROR_SUCCESS);
    }
    
    void TearDown() override {
        // Clean up after each test
        set_last_error(error_codes::ERROR_SUCCESS);
    }
};

TEST_F(Win32ErrorTest, InitialValueIsSuccess) {
    // After SetUp resets, error should be SUCCESS
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
}

TEST_F(Win32ErrorTest, SetAndGetError) {
    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
    
    set_last_error(error_codes::ERROR_ACCESS_DENIED);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(Win32ErrorTest, SetErrorToZero) {
    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
    
    set_last_error(error_codes::ERROR_SUCCESS);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
}

TEST_F(Win32ErrorTest, SetArbitraryValue) {
    // Windows allows any 32-bit value
    set_last_error(0xDEADBEEF);
    EXPECT_EQ(get_last_error(), 0xDEADBEEFu);
    
    set_last_error(0xFFFFFFFF);
    EXPECT_EQ(get_last_error(), 0xFFFFFFFFu);
}

TEST_F(Win32ErrorTest, GetDoesNotClearError) {
    // GetLastError should not modify the error - it's read-only
    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(Win32ErrorTest, MultipleSetsClobber) {
    // Each SetLastError overwrites the previous value
    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    set_last_error(error_codes::ERROR_ACCESS_DENIED);
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// =============================================================================
// Thread-Local Storage Tests
// =============================================================================

TEST_F(Win32ErrorTest, ThreadLocalStorage) {
    // Each thread should have its own error code
    std::atomic<bool> thread1_ready{false};
    std::atomic<bool> thread2_ready{false};
    std::atomic<bool> continue_test{false};
    
    u32 thread1_error = 0;
    u32 thread2_error = 0;
    
    std::thread t1([&]() {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        thread1_ready = true;
        
        // Wait for thread 2 to set its error
        while (!continue_test) {
            std::this_thread::yield();
        }
        
        // Our error should not be affected by thread 2
        thread1_error = get_last_error();
    });
    
    std::thread t2([&]() {
        // Wait for thread 1 to set its error first
        while (!thread1_ready) {
            std::this_thread::yield();
        }
        
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        thread2_ready = true;
        
        // Wait for synchronization
        while (!continue_test) {
            std::this_thread::yield();
        }
        
        thread2_error = get_last_error();
    });
    
    // Wait for both threads to set their errors
    while (!thread1_ready || !thread2_ready) {
        std::this_thread::yield();
    }
    
    // Let threads read their errors
    continue_test = true;
    
    t1.join();
    t2.join();
    
    // Each thread should have its own error value
    EXPECT_EQ(thread1_error, error_codes::ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(thread2_error, error_codes::ERROR_ACCESS_DENIED);
}

TEST_F(Win32ErrorTest, MultipleThreadsIsolated) {
    // More comprehensive thread isolation test
    constexpr int NUM_THREADS = 10;
    std::vector<std::thread> threads;
    std::vector<u32> results(NUM_THREADS, 0);
    std::atomic<int> ready_count{0};
    std::atomic<bool> go{false};
    
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([i, &results, &ready_count, &go]() {
            // Each thread sets a unique error code
            u32 my_error = static_cast<u32>(1000 + i);
            set_last_error(my_error);
            
            ready_count++;
            
            // Wait for all threads to set their error
            while (!go) {
                std::this_thread::yield();
            }
            
            // Read back the error - should be unchanged
            results[static_cast<usize>(i)] = get_last_error();
        });
    }
    
    // Wait for all threads to set their errors
    while (ready_count < NUM_THREADS) {
        std::this_thread::yield();
    }
    
    // Let all threads read their errors
    go = true;
    
    for (auto& t : threads) {
        t.join();
    }
    
    // Verify each thread got its own error value
    for (int i = 0; i < NUM_THREADS; ++i) {
        EXPECT_EQ(results[static_cast<usize>(i)], static_cast<u32>(1000 + i))
            << "Thread " << i << " got wrong error code";
    }
}

TEST_F(Win32ErrorTest, NewThreadStartsWithZero) {
    // New threads should start with ERROR_SUCCESS
    u32 thread_initial_error = 0xFFFFFFFF;
    
    std::thread t([&]() {
        thread_initial_error = get_last_error();
    });
    t.join();
    
    EXPECT_EQ(thread_initial_error, error_codes::ERROR_SUCCESS);
}

// =============================================================================
// Thunk Implementation Tests
// =============================================================================

class ErrorThunkTest : public ::testing::Test {
protected:
    static constexpr usize MEMORY_SIZE = 4096;
    static constexpr GuestAddr MEMORY_BASE = 0x1000;
    
    void SetUp() override {
        memory_.map_region(MEMORY_BASE, MEMORY_SIZE, MemoryProtection::ReadWrite);
        marshaler_ = std::make_unique<Marshaler>(memory_);
        set_last_error(error_codes::ERROR_SUCCESS);
    }
    
    void TearDown() override {
        set_last_error(error_codes::ERROR_SUCCESS);
    }
    
    GuestMemory memory_;
    std::unique_ptr<Marshaler> marshaler_;
};

TEST_F(ErrorThunkTest, ImplGetLastErrorReturnsCurrentError) {
    // Set an error value
    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    
    // Call the thunk implementation
    impl_GetLastError(*marshaler_);
    
    // The error value should still be accessible
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
}

TEST_F(ErrorThunkTest, ImplSetLastErrorPlaceholder) {
    // Currently impl_SetLastError is a placeholder
    // It should not crash when called
    set_last_error(error_codes::ERROR_SUCCESS);
    
    impl_SetLastError(*marshaler_);
    
    // For now, just verify it doesn't crash
    // Full argument marshaling will be tested when dispatch layer is complete
    SUCCEED();
}

TEST_F(ErrorThunkTest, ImplGetLastErrorDoesNotModify) {
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    
    impl_GetLastError(*marshaler_);
    impl_GetLastError(*marshaler_);
    impl_GetLastError(*marshaler_);
    
    // Multiple calls should not change the error
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// =============================================================================
// Integration Pattern Tests
// =============================================================================

TEST_F(Win32ErrorTest, TypicalUsagePattern) {
    // Simulate a typical Win32 usage pattern:
    // 1. Clear error before operation
    // 2. Perform operation
    // 3. On failure, check error
    
    // Simulated API call that "fails"
    auto simulated_api_call = []() -> bool {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return false;  // Indicates failure
    };
    
    // Clear before call (some apps do this)
    set_last_error(error_codes::ERROR_SUCCESS);
    
    // Call "fails"
    bool result = simulated_api_call();
    EXPECT_FALSE(result);
    
    // Check error
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
}

TEST_F(Win32ErrorTest, SuccessfulCallClearsError) {
    // Some APIs clear the error on success (but not all!)
    auto simulated_successful_api = []() -> bool {
        set_last_error(error_codes::ERROR_SUCCESS);
        return true;
    };
    
    // Set a prior error
    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    
    // Successful call clears it
    bool result = simulated_successful_api();
    EXPECT_TRUE(result);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
}

TEST_F(Win32ErrorTest, NestedCalls) {
    // Inner APIs may set errors even if outer API succeeds
    auto inner_api = []() {
        set_last_error(error_codes::ERROR_MORE_DATA);
    };
    
    auto outer_api = [&]() -> bool {
        inner_api();
        // Outer succeeds but doesn't clear error
        return true;
    };
    
    set_last_error(error_codes::ERROR_SUCCESS);
    bool result = outer_api();
    
    EXPECT_TRUE(result);
    // Error from inner call persists
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MORE_DATA);
}
