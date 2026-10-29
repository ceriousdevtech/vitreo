// SPDX-License-Identifier: LGPL-2.1-only
// Stage 3: cross-process named sync objects (event/mutex/semaphore) backed by
// shm, consumed lock-free via atomics. The module is self-contained (it uses
// shm_open directly), so these tests run regardless of the VITREO_IPC gate.

#include <gtest/gtest.h>

#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <libproc.h>

#include "ipc/named_sync.hpp"
#include "ipc/ipc_gate.hpp"
#include "core/sync/ntsync.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"

using namespace vitreo;
using namespace vitreo::ipc;

TEST(Stage3NamedSync, CrossProcessManualEventReleasesWaiter) {
    const std::string name = "Global\\vtm_evt_" + std::to_string(::getpid());
    auto ev = SharedSyncObject::create_or_open(name, SyncKind::Event,
                                               /*manual=*/1, /*initial=*/0, /*create=*/true);
    ASSERT_NE(ev, nullptr);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        auto cev = SharedSyncObject::create_or_open(name, SyncKind::Event, 0, 0, /*create=*/false);
        if (!cev) _exit(10);
        SharedSyncObject* arr[1] = {cev.get()};
        u32 r = wait_set(arr, 1, /*wait_all=*/false, /*timeout=*/5000, /*tid=*/2);
        _exit(r == ntsync::WAIT_OBJECT_0 ? 0 : 11);
    }

    usleep(200 * 1000);   // ensure child is waiting
    ev->set_event();      // signal across processes

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child step " << WEXITSTATUS(status);
}

TEST(Stage3NamedSync, AutoResetEventConsumedByExactlyOne) {
    const std::string name = "Global\\vtm_auto_" + std::to_string(::getpid());
    auto ev = SharedSyncObject::create_or_open(name, SyncKind::Event, /*manual=*/0, /*initial=*/1, true);
    ASSERT_NE(ev, nullptr);
    SharedSyncObject* arr[1] = {ev.get()};
    // First wait consumes the signal; second times out.
    EXPECT_EQ(wait_set(arr, 1, false, 0, 1), ntsync::WAIT_OBJECT_0);
    EXPECT_EQ(wait_set(arr, 1, false, 0, 1), ntsync::WAIT_TIMEOUT);
}

TEST(Stage3NamedSync, SemaphoreCountsAcrossWaits) {
    const std::string name = "Global\\vtm_sem_" + std::to_string(::getpid());
    auto s = SharedSyncObject::create_or_open(name, SyncKind::Semaphore, /*init=*/2, /*max=*/4, true);
    ASSERT_NE(s, nullptr);
    SharedSyncObject* arr[1] = {s.get()};
    EXPECT_EQ(wait_set(arr, 1, false, 0, 1), ntsync::WAIT_OBJECT_0);  // 2->1
    EXPECT_EQ(wait_set(arr, 1, false, 0, 1), ntsync::WAIT_OBJECT_0);  // 1->0
    EXPECT_EQ(wait_set(arr, 1, false, 0, 1), ntsync::WAIT_TIMEOUT);   // 0
    std::int32_t prev = -1;
    EXPECT_TRUE(s->release_semaphore(1, &prev));
    EXPECT_EQ(prev, 0);
    EXPECT_EQ(wait_set(arr, 1, false, 0, 1), ntsync::WAIT_OBJECT_0);  // 1->0
}

TEST(Stage3NamedSync, MutexMutualExclusion) {
    const std::string name = "Global\\vtm_mtx_" + std::to_string(::getpid());
    auto m = SharedSyncObject::create_or_open(name, SyncKind::Mutex, /*owner=*/0, /*tid=*/0, true);
    ASSERT_NE(m, nullptr);
    SharedSyncObject* arr[1] = {m.get()};
    EXPECT_EQ(wait_set(arr, 1, false, 0, /*tid=*/5), ntsync::WAIT_OBJECT_0);  // tid5 acquires
    // A different tid cannot acquire while held.
    EXPECT_EQ(wait_set(arr, 1, false, 0, /*tid=*/6), ntsync::WAIT_TIMEOUT);
    EXPECT_TRUE(m->release_mutex(5));
    EXPECT_EQ(wait_set(arr, 1, false, 0, /*tid=*/6), ntsync::WAIT_OBJECT_0);  // now free
}

TEST(Stage3NamedSync, ZombieProcessDoesNotKeepNamedMutexAlive) {
    // Learn CreateMutex: the mutex object is destroyed when its last handle
    // is closed, and process termination closes handles. A worker that
    // _exit's becomes a Darwin zombie until waitpid; that must not keep the
    // name visible (ERROR_ALREADY_EXISTS) for the next CreateMutex.
    const std::string name = "Global\\vtm_zombie_mtx_" +
                             std::to_string(::getpid());
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        auto owned = SharedSyncObject::create_or_open(
            name, SyncKind::Mutex, /*owner=*/1, /*tid=*/1, /*create=*/true);
        if (!owned || !owned->was_created()) _exit(10);
        _exit(0);
    }

    int waited = 0;
    while (waited < 50) {
        proc_bsdinfo info {};
        const int n = ::proc_pidinfo(child, PROC_PIDTBSDINFO, 0, &info,
                                     static_cast<int>(sizeof(info)));
        if (n == static_cast<int>(sizeof(info)) && info.pbi_status == SZOMB) {
            break;
        }
        ::usleep(10 * 1000);
        ++waited;
    }

    auto recreated = SharedSyncObject::create_or_open(
        name, SyncKind::Mutex, /*owner=*/0, /*tid=*/0, /*create=*/true);
    ASSERT_NE(recreated, nullptr);
    EXPECT_TRUE(recreated->was_created());

    int status = 0;
    EXPECT_EQ(::waitpid(child, &status, 0), child);
}

TEST(Stage3NamedSync, DeadProcessOwnerIsAcquiredAsAbandoned) {
    const std::string name = "Global\\vtm_abandoned_" +
                             std::to_string(::getpid());
    auto observer = SharedSyncObject::create_or_open(
        name, SyncKind::Mutex, /*owner=*/0, /*tid=*/0, true);
    ASSERT_NE(observer, nullptr);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        auto owner = SharedSyncObject::create_or_open(
            name, SyncKind::Mutex, 0, 0, false);
        if (!owner) _exit(10);
        SharedSyncObject* child_arr[1] = {owner.get()};
        if (wait_set(child_arr, 1, false, 0, 17) !=
            ntsync::WAIT_OBJECT_0) {
            _exit(11);
        }
        _exit(0);  // Deliberately leave the mutex owned.
    }

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0) << "child step " << WEXITSTATUS(status);

    SharedSyncObject* arr[1] = {observer.get()};
    EXPECT_EQ(wait_set(arr, 1, false, 1000, /*tid=*/23),
              ntsync::WAIT_ABANDONED_0);
    EXPECT_TRUE(observer->release_mutex(23));
    EXPECT_EQ(wait_set(arr, 1, false, 0, /*tid=*/24),
              ntsync::WAIT_OBJECT_0);
}

TEST(Stage3NamedSync, ExplicitThreadAbandonmentIsReportedOnce) {
    const std::string name = "Global\\vtm_thread_abandoned_" +
                             std::to_string(::getpid());
    auto mutex = SharedSyncObject::create_or_open(
        name, SyncKind::Mutex, /*owner=*/0, /*tid=*/0, true);
    ASSERT_NE(mutex, nullptr);
    SharedSyncObject* arr[1] = {mutex.get()};

    ASSERT_EQ(wait_set(arr, 1, false, 0, /*tid=*/31),
              ntsync::WAIT_OBJECT_0);
    ASSERT_TRUE(mutex->abandon_mutex(31));
    EXPECT_EQ(wait_set(arr, 1, false, 0, /*tid=*/32),
              ntsync::WAIT_ABANDONED_0);
    ASSERT_TRUE(mutex->release_mutex(32));
    EXPECT_EQ(wait_set(arr, 1, false, 0, /*tid=*/33),
              ntsync::WAIT_OBJECT_0);
}

// Full SyncManager path: a named event created in one process and opened in
// another shares one shm object; SetEvent releases the peer's wait.
TEST(Stage3NamedSync, SyncManagerCrossProcessEvent) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "disabled by VITREO_IPC=0";
    const std::string name = "Global\\vtm_smevt_" + std::to_string(::getpid());

    winapi::HandleTable ph;
    winapi::SyncManager psm(ph);
    winapi::Handle hev = psm.create_event(/*manual=*/true, /*initial=*/false, name);
    ASSERT_NE(hev, winapi::NULL_HANDLE);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        winapi::HandleTable ch;
        winapi::SyncManager csm(ch);
        winapi::Handle chev =
            csm.open_event(/*SYNCHRONIZE=*/0x00100000, name);
        if (chev == winapi::NULL_HANDLE) _exit(10);
        u32 r = csm.wait_for_single_object(chev, 5000);
        _exit(r == winapi::WAIT_OBJECT_0 ? 0 : 11);
    }

    usleep(200 * 1000);
    psm.set_event(hev);
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child step " << WEXITSTATUS(status);
}

TEST(Stage3NamedSync, SyncManagerCrossProcessOpenMutex) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "disabled by VITREO_IPC=0";
    const std::string name = "Global\\vtm_smmx_" + std::to_string(::getpid());

    winapi::HandleTable ph;
    winapi::SyncManager psm(ph);
    winapi::Handle hmx = psm.create_mutex(/*initial_owner=*/false, name);
    ASSERT_NE(hmx, winapi::NULL_HANDLE);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        winapi::HandleTable ch;
        winapi::SyncManager csm(ch);
        winapi::Handle cmx =
            csm.open_mutex(name, /*SYNCHRONIZE=*/0x00100000u, false);
        if (cmx == winapi::NULL_HANDLE) _exit(10);
        u32 r = csm.wait_for_single_object(cmx, 0);
        _exit(r == winapi::WAIT_OBJECT_0 ? 0 : 11);
    }

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child step " << WEXITSTATUS(status);
}
