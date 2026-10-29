// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file security_stubs_fixture.hpp
/// @brief Shared fixture and helpers for the advapi32 SecurityStubs unit
///        tests (test_security*.cpp). Defines the SecurityStubsTest fixture
///        and the open_primary_token / find_export test helpers.

#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>
#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace vitreo::winapi::security_test {

// Test setup opens the current process token with rights needed by the
// subsequent query/duplicate/adjust operation. NULL is not a process handle.
// OpenProcessToken contract: processthreadsapi.h; production reports errors
// through its explicit fourth argument.
inline bool open_primary_token(SecurityStubs& stubs, Handle& token) {
    u32 error = error_codes::ERROR_SUCCESS;
    const bool result = stubs.open_process_token(
        0xFFFFFFFFu, HandleAccess::All, token, error);
    EXPECT_EQ(error, error_codes::ERROR_SUCCESS);
    return result;
}

inline ExportEntry* find_export(DllExports& exports, const char* name) {
    for (auto& entry : exports.entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

} // namespace vitreo::winapi::security_test

using security_test::open_primary_token;
using security_test::find_export;

// =============================================================================
// Fixture
// =============================================================================

class SecurityStubsTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_handles = std::make_unique<HandleTable>();
        m_stubs   = std::make_unique<SecurityStubs>(*m_handles);
    }

    std::unique_ptr<HandleTable>   m_handles;
    std::unique_ptr<SecurityStubs> m_stubs;
};
