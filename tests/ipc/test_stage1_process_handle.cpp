// SPDX-License-Identifier: LGPL-2.1-only
// Stage 1: the kqueue/NOTE_EXIT process reaper exposes a real host process's
// liveness as an ntsync event, so a guest process HANDLE can be waited on and
// its exit code retrieved across the forked Vitreo host processes.

#include <gtest/gtest.h>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include "ipc/process_handle.hpp"
#include "core/sync/ntsync.hpp"

using namespace vitreo;

TEST(Stage1ProcessHandle, TracksChildExitAndCapturesCode) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        usleep(200 * 1000);  // live 200ms so we register before it exits
        _exit(7);
    }

    ipc::ProcessObject* obj = ipc::track_process(pid);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->pid, pid);
    EXPECT_FALSE(obj->exited.load());  // still alive right after track

    ntsync::ObjectId id = obj->exit_event;
    u32 r = ntsync::global().wait(&id, 1, /*wait_all=*/false, /*ms=*/5000, /*tid=*/1);
    EXPECT_EQ(r, ntsync::WAIT_OBJECT_0);
    EXPECT_TRUE(obj->exited.load());
    EXPECT_EQ(obj->exit_code.load(), 7u);

    ::waitpid(pid, nullptr, 0);  // reap our zombie (test hygiene)
}

TEST(Stage1ProcessHandle, AlreadyExitedPidReportsExited) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        _exit(0);
    }
    usleep(150 * 1000);  // let it exit before we track it

    ipc::ProcessObject* obj = ipc::track_process(pid);
    ASSERT_NE(obj, nullptr);

    ntsync::ObjectId id = obj->exit_event;
    u32 r = ntsync::global().wait(&id, 1, /*wait_all=*/false, /*ms=*/5000, /*tid=*/1);
    EXPECT_EQ(r, ntsync::WAIT_OBJECT_0);
    EXPECT_TRUE(obj->exited.load());

    ::waitpid(pid, nullptr, 0);
}

TEST(Stage1ProcessHandle, CapturesFullWindowsExitCodeViaPipe) {
    // Host wait status only keeps 8 bits; the exit-code pipe carries the full
    // Windows DWORD (e.g. HRESULT 0x80070005).
    int exit_pipe[2] = {-1, -1};
    ASSERT_EQ(::pipe(exit_pipe), 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        ::close(exit_pipe[0]);
        usleep(50 * 1000);
        ipc::publish_windows_exit_code(exit_pipe[1], 0x80070005u);
        _exit(0);  // host status deliberately disagrees with Windows code
    }
    ::close(exit_pipe[1]);

    ipc::ProcessObject* obj = ipc::track_process(pid);
    ASSERT_NE(obj, nullptr);
    obj->attach_windows_exit_fd(exit_pipe[0]);

    ntsync::ObjectId id = obj->exit_event;
    u32 r = ntsync::global().wait(&id, 1, /*wait_all=*/false, /*ms=*/5000, /*tid=*/1);
    EXPECT_EQ(r, ntsync::WAIT_OBJECT_0);
    EXPECT_TRUE(obj->exited.load());
    EXPECT_EQ(obj->exit_code.load(), 0x80070005u);

    ::waitpid(pid, nullptr, 0);
}

TEST(Stage1ProcessHandle, InvalidPidReturnsNull) {
    EXPECT_EQ(ipc::track_process(0), nullptr);
    EXPECT_EQ(ipc::track_process(-1), nullptr);
}
