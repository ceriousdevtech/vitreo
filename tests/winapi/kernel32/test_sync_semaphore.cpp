/// @file test_sync_semaphore.cpp
/// @brief SyncManager semaphore tests: QuerySemaphore, named reopen, OpenSemaphore/OpenMutex contracts and handle inheritance (THREAD-002)

#include "sync_test_fixture.hpp"

TEST_F(SyncTest, QuerySemaphoreReportsCurrentAndMaximum) {
    Handle h = m_mgr->create_semaphore(/*initial=*/2, /*maximum=*/5, "");
    ASSERT_NE(h, 0u);
    i32 current = -1;
    i32 maximum = -1;
    ASSERT_TRUE(m_mgr->query_semaphore(h, &current, &maximum));
    EXPECT_EQ(current, 2);
    EXPECT_EQ(maximum, 5);

    i32 prev = -1;
    ASSERT_TRUE(m_mgr->release_semaphore(h, 1, &prev));
    EXPECT_EQ(prev, 2);
    ASSERT_TRUE(m_mgr->query_semaphore(h, &current, &maximum));
    EXPECT_EQ(current, 3);
    EXPECT_EQ(maximum, 5);

    EXPECT_FALSE(m_mgr->query_semaphore(static_cast<Handle>(0xDEAD), &current,
                                        &maximum));
}

TEST_F(SyncTest, ReopeningNamedSemaphoreReturnsDistinctClosableHandle) {
    Handle first = m_mgr->create_semaphore(0, 1, "shared_semaphore");
    Handle second = m_mgr->create_semaphore(0, 1, "shared_semaphore");

    ASSERT_NE(first, second);
    EXPECT_TRUE(m_mgr->close_handle(first));
    EXPECT_TRUE(m_mgr->close_handle(second));
}

// =============================================================================
// Thunk Export Tests
// =============================================================================

TEST_F(SyncTest, OpenSemaphoreOpensNamedSemaphore) {
    Handle created = m_mgr->create_semaphore(1, 1, "VitreoOpenSem");
    ASSERT_NE(created, 0u);

    Handle opened = m_mgr->open_semaphore("VitreoOpenSem");
    ASSERT_NE(opened, 0u);
    EXPECT_NE(opened, created);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);

    EXPECT_EQ(m_mgr->open_semaphore("MissingSem"), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);

    EXPECT_TRUE(m_mgr->close_handle(created));
    EXPECT_TRUE(m_mgr->close_handle(opened));
}

// Learn OpenMutexW / OpenSemaphoreW (handles_waits_probe.expected): NULL name →
// ERROR_INVALID_PARAMETER; wrong kind → ERROR_INVALID_HANDLE; bInheritHandle
// sets HANDLE_FLAG_INHERIT; dwDesiredAccess is stored on the handle.
TEST_F(SyncTest, OpenMutexAndSemaphoreContractMatchesWindows) {
    constexpr u32 SYNCHRONIZE = 0x00100000u;
    set_last_error(0xDEAD);
    EXPECT_EQ(m_mgr->open_mutex("", SYNCHRONIZE, false), NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    set_last_error(0xDEAD);
    EXPECT_EQ(m_mgr->open_semaphore("", SYNCHRONIZE, false), NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    Handle ev = m_mgr->create_event(true, false, "VitreoOpenCrossKind");
    ASSERT_NE(ev, NULL_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_EQ(m_mgr->open_mutex("VitreoOpenCrossKind", SYNCHRONIZE, false),
              NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_EQ(m_mgr->open_semaphore("VitreoOpenCrossKind", SYNCHRONIZE, false),
              NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    EXPECT_TRUE(m_mgr->close_handle(ev));

    Handle mx = m_mgr->create_mutex(false, "VitreoOpenMutexInherit");
    ASSERT_NE(mx, NULL_HANDLE);
    Handle opened =
        m_mgr->open_mutex("VitreoOpenMutexInherit", SYNCHRONIZE, true);
    ASSERT_NE(opened, NULL_HANDLE);
    EXPECT_EQ(m_handles->get_flags(opened), HandleFlags::Inherit);
    EXPECT_EQ(m_handles->get_access(opened), SYNCHRONIZE);
    EXPECT_TRUE(m_mgr->close_handle(opened));
    EXPECT_TRUE(m_mgr->close_handle(mx));

    Handle sem = m_mgr->create_semaphore(0, 1, "VitreoOpenSemInherit");
    ASSERT_NE(sem, NULL_HANDLE);
    Handle osem =
        m_mgr->open_semaphore("VitreoOpenSemInherit", SYNCHRONIZE, true);
    ASSERT_NE(osem, NULL_HANDLE);
    EXPECT_EQ(m_handles->get_flags(osem), HandleFlags::Inherit);
    EXPECT_EQ(m_handles->get_access(osem), SYNCHRONIZE);
    EXPECT_TRUE(m_mgr->close_handle(osem));
    EXPECT_TRUE(m_mgr->close_handle(sem));
}

// Learn ReleaseSemaphore + handles_waits_probe.expected:
// over-max → ERROR_TOO_MANY_POSTS (298); count<=0 → ERROR_INVALID_PARAMETER;
// bad handle → ERROR_INVALID_HANDLE; SYNCHRONIZE-only → ERROR_ACCESS_DENIED.
TEST_F(SyncTest, ReleaseSemaphoreFailureLastErrors) {
    constexpr u32 SYNCHRONIZE = 0x00100000u;
    constexpr u32 SEMAPHORE_MODIFY_STATE = 0x0002u;
    Handle full = m_mgr->create_semaphore(1, 1, "");
    ASSERT_NE(full, NULL_HANDLE);
    i32 prev = -1;
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->release_semaphore(full, 1, &prev));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_TOO_MANY_POSTS);
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->release_semaphore(full, 0, &prev));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->release_semaphore(full, -1, &prev));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->release_semaphore(0, 1, &prev));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    Handle ev = m_mgr->create_event(false, false, "");
    ASSERT_NE(ev, NULL_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->release_semaphore(ev, 1, &prev));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

    Handle named = m_mgr->create_semaphore(0, 2, "VitreoReleaseSemAccess");
    ASSERT_NE(named, NULL_HANDLE);
    Handle sync_only =
        m_mgr->open_semaphore("VitreoReleaseSemAccess", SYNCHRONIZE, false);
    ASSERT_NE(sync_only, NULL_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->release_semaphore(sync_only, 1, &prev));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    Handle modify =
        m_mgr->open_semaphore("VitreoReleaseSemAccess", SEMAPHORE_MODIFY_STATE,
                              false);
    ASSERT_NE(modify, NULL_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_TRUE(m_mgr->release_semaphore(modify, 1, &prev));
    EXPECT_EQ(prev, 0);

    EXPECT_TRUE(m_mgr->close_handle(full));
    EXPECT_TRUE(m_mgr->close_handle(ev));
    EXPECT_TRUE(m_mgr->close_handle(sync_only));
    EXPECT_TRUE(m_mgr->close_handle(modify));
    EXPECT_TRUE(m_mgr->close_handle(named));
}

// Learn CreateSemaphoreW: SECURITY_ATTRIBUTES.bInheritHandle decides Inherit.
TEST_F(SyncTest, CreateSemaphoreHonorsInheritAttribute) {
    Handle plain = m_mgr->create_semaphore(0, 1, "", false);
    ASSERT_NE(plain, NULL_HANDLE);
    EXPECT_EQ(m_handles->get_flags(plain) & HandleFlags::Inherit, 0u);
    Handle inheritable = m_mgr->create_semaphore(0, 1, "", true);
    ASSERT_NE(inheritable, NULL_HANDLE);
    EXPECT_EQ(m_handles->get_flags(inheritable) & HandleFlags::Inherit,
              HandleFlags::Inherit);
    EXPECT_TRUE(m_mgr->close_handle(plain));
    EXPECT_TRUE(m_mgr->close_handle(inheritable));
}
