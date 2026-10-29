// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file ntdll_ext_test_fixture.hpp
/// @brief Shared includes and the NtdllExtManager fixture for the ntdll extended tests.
///
/// Included by every test_ntdll_ext*.cpp. NtdllExtTest is a global-namespace class
/// defined identically in each including translation unit, because every TEST_F of a
/// suite must name the same fixture type; all its members are defined in-class.
/// NtdllDirectoryQueryTest and NtDispositionTest are used by one file each and stay
/// file-local there.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/system/ntdll/ntdll_ext.hpp>
#include <winapi/system/ntdll/ntdll.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/memory/file_mapping.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/kernel32/module/module.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/cpu/seh/seh32.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;


// =============================================================================
// Fixture
// =============================================================================

class NtdllExtTest : public ::testing::Test {
protected:
    NtdllExtManager mgr;
};
