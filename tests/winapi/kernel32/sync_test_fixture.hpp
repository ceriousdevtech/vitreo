#pragma once
/// @file sync_test_fixture.hpp
/// @brief Shared SyncTest fixture and includes for the SyncManager (THREAD-002) tests.
///
/// Included by every test_sync*.cpp. SyncTest is a global-namespace class
/// defined identically in each including translation unit, because every
/// TEST_F of a suite must name the same fixture type; all its members are
/// defined in-class.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <ipc/ipc_gate.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/synchronization/critical_section.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/common/object_access_constants.hpp>
#include <core/sync/ntsync.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class SyncTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_handles = std::make_unique<HandleTable>();
        m_mgr = std::make_unique<SyncManager>(*m_handles);
        m_handles->set_close_callback(
            [this](Handle h, HandleType type, void*) {
                return m_mgr->on_handle_close(h, type);
            });
    }

    void TearDown() override {
        m_handles->set_close_callback(nullptr);
        m_mgr.reset();
        m_handles.reset();
    }
    std::unique_ptr<HandleTable> m_handles;
    std::unique_ptr<SyncManager> m_mgr;
};
