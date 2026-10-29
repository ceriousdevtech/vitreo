#pragma once
/// @file test_vmem_fixture.hpp
/// @brief Shared gtest fixture for the VirtualAlloc / VirtualFree / VirtualProtect tests (test_vmem*.cpp)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/kernel32/memory/memory.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class VirtualMemoryTest : public ::testing::Test {
protected:
    GuestMemory mem;
    VirtualMemoryManager vmm{mem};
};
