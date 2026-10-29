// SPDX-License-Identifier: LGPL-2.1-only

#include "ipc/process_session.hpp"
#include "ipc/process_handle.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <pthread.h>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

namespace vitreo::ipc {
namespace {

class ProcessSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
            ("vitreo-process-session-test-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        ASSERT_TRUE(std::filesystem::create_directories(root, ec));

        image = root / "child.exe";
        std::ofstream(image) << "guest image";

        runtime = root / "record-worker.sh";
        std::ofstream script(runtime);
        script << "#!/bin/sh\n"
                  "printf 'ppid=%s\\n' \"$PPID\" > \"$VITREO_SESSION_TEST_OUTPUT\"\n"
                  "printf 'service=%s\\n' \"$VITREO_SERVICE_NAME\" >> "
                  "\"$VITREO_SESSION_TEST_OUTPUT\"\n"
                  "printf 'macos_role=%s\\n' \"$VITREO_MACOS_PROCESS_ROLE\" >> "
                  "\"$VITREO_SESSION_TEST_OUTPUT\"\n"
                  "for arg in \"$@\"; do printf '%s\\n' \"$arg\" >> "
                  "\"$VITREO_SESSION_TEST_OUTPUT\"; last=\"$arg\"; done\n"
                  "/bin/rm -f \"$last\"\n"
                  "exit 0\n";
        script.close();
        ASSERT_EQ(::chmod(runtime.c_str(), 0700), 0);

        output = root / "argv.txt";
        ASSERT_EQ(::setenv("VITREO_SESSION_TEST_OUTPUT", output.c_str(), 1), 0);
    }

    void TearDown() override {
        (void)::unsetenv("VITREO_SESSION_TEST_OUTPUT");
        (void)::unsetenv("VITREO_SERVICE_NAME");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path root;
    std::filesystem::path image;
    std::filesystem::path runtime;
    std::filesystem::path output;
};

TEST_F(ProcessSessionTest, OwnsDirectWorkerAndPropagatesLaunchContext) {
    ProcessSession session;
    WorkerLaunchSpec spec;
    spec.host_image_path = image;
    spec.guest_image_path =
        "C:\\Program Files (x86)\\Starcraft\\StarEdit.exe";
    spec.guest_command_line = "\"StarEdit.exe\" /new";
    spec.guest_current_directory =
        "C:\\Program Files (x86)\\Starcraft";
    spec.capsule_name = "Starcraft";
    spec.inherited_pipes = "pipes";
    spec.inherited_sync = "sync";
    spec.inherited_mappings = "mappings";
    spec.guest_service_arguments_utf16 = {
        "002D002D0063006F006D002D0073006500720076006900630065"};
    spec.runtime_path = runtime;

    const pid_t child = session.launch_worker(spec);
    ASSERT_GT(child, 0);
    EXPECT_EQ(session.owned_worker_count(), 1u);

    session.wait_for_workers();
    EXPECT_EQ(session.owned_worker_count(), 0u);

    std::ifstream recorded(output);
    ASSERT_TRUE(recorded);
    std::vector<std::string> lines;
    for (std::string line; std::getline(recorded, line);) {
        lines.push_back(std::move(line));
    }
    ASSERT_GE(lines.size(), 25u);
    EXPECT_EQ(lines[0], "ppid=" + std::to_string(::getpid()));
    EXPECT_EQ(lines[1], "service=");
    EXPECT_EQ(lines[2], "macos_role=child");
    EXPECT_EQ(lines[3], "run");
    EXPECT_EQ(lines[4], "--relaunched-as");
    EXPECT_EQ(lines[5], spec.guest_image_path);
    EXPECT_EQ(lines[6], "--guest-cmdline");
    EXPECT_EQ(lines[7], spec.guest_command_line);
    EXPECT_EQ(lines[8], "--guest-cwd");
    EXPECT_EQ(lines[9], spec.guest_current_directory);
    EXPECT_EQ(lines[10], "--capsule");
    EXPECT_EQ(lines[11], spec.capsule_name);
    EXPECT_EQ(lines[12], "--inherited-pipes");
    EXPECT_EQ(lines[13], spec.inherited_pipes);
    EXPECT_EQ(lines[14], "--inherited-sync");
    EXPECT_EQ(lines[15], spec.inherited_sync);
    EXPECT_EQ(lines[16], "--inherited-mappings");
    EXPECT_EQ(lines[17], spec.inherited_mappings);
    EXPECT_EQ(lines[18], "--guest-service-arg-utf16");
    EXPECT_EQ(lines[19], spec.guest_service_arguments_utf16[0]);
    EXPECT_EQ(lines[20], "--guest-parent-pid");
    EXPECT_EQ(lines[21], std::to_string(::getpid()));
    EXPECT_EQ(lines[22], "--parent-death-fd");
    EXPECT_FALSE(lines[23].empty())
        << "worker did not receive a parent-death descriptor";
    // The guest image snapshot is always the final argument.
    EXPECT_FALSE(lines.back().empty());
    EXPECT_FALSE(std::filesystem::exists(lines.back()))
        << "worker did not remove its launch snapshot";

    errno = 0;
    EXPECT_EQ(::waitpid(child, nullptr, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD) << "session did not reap its direct worker";
}

TEST_F(ProcessSessionTest, ExtraEnvironmentOverridesInheritedValue) {
    ASSERT_EQ(::setenv("VITREO_SERVICE_NAME", "ParentSvc", 1), 0);

    ProcessSession session;
    WorkerLaunchSpec spec;
    spec.host_image_path = image;
    spec.guest_image_path = "C:\\service.exe";
    spec.runtime_path = runtime;
    spec.extra_env.push_back("VITREO_SERVICE_NAME=ChildSvc");

    const pid_t child = session.launch_worker(spec);
    ASSERT_GT(child, 0);
    session.wait_for_workers();

    std::ifstream recorded(output);
    ASSERT_TRUE(recorded);
    std::vector<std::string> lines;
    for (std::string line; std::getline(recorded, line);) {
        lines.push_back(std::move(line));
    }
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[1], "service=ChildSvc");
}

TEST_F(ProcessSessionTest, ElevatedWorkerReceivesAuthorizationCredential) {
    ProcessSession session;
    WorkerLaunchSpec spec;
    spec.host_image_path = image;
    spec.guest_image_path = "C:\\Setup.exe";
    spec.runtime_path = runtime;
    spec.elevation_credential =
        std::string("\x00\x11\xaa\xff", 4);

    ASSERT_GT(session.launch_worker(spec), 0);
    session.wait_for_workers();

    std::ifstream recorded(output);
    ASSERT_TRUE(recorded);
    std::vector<std::string> lines;
    for (std::string line; std::getline(recorded, line);) {
        lines.push_back(std::move(line));
    }
    const auto option = std::find(
        lines.begin(), lines.end(),
        "--guest-elevation-credential");
    ASSERT_NE(option, lines.end());
    ASSERT_NE(std::next(option), lines.end());
    EXPECT_EQ(*std::next(option), "0011aaff");
}

TEST_F(ProcessSessionTest, ServiceWorkerReceivesOneShotTokenCapability) {
    ProcessSession session;
    WorkerLaunchSpec spec;
    spec.host_image_path = image;
    spec.guest_image_path = "C:\\SteamService.exe";
    spec.runtime_path = runtime;
    spec.token_context = WorkerTokenContext::LocalSystemService;

    ASSERT_GT(session.launch_worker(spec), 0);
    session.wait_for_workers();

    std::ifstream recorded(output);
    ASSERT_TRUE(recorded);
    std::vector<std::string> lines;
    for (std::string line; std::getline(recorded, line);) {
        lines.push_back(std::move(line));
    }
    const auto option = std::find(
        lines.begin(), lines.end(), "--guest-token-context-fd");
    ASSERT_NE(option, lines.end());
    ASSERT_NE(std::next(option), lines.end());
    EXPECT_FALSE(std::next(option)->empty());
}

TEST(ProcessSessionCapabilityTest, RejectsOrdinaryFileAsTokenCapability) {
    const auto path = std::filesystem::temp_directory_path() /
        ("vitreo-token-capability-" + std::to_string(::getpid()));
    {
        std::ofstream file(path);
        file << "VTK1";
    }
    const int fd = ::open(path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    EXPECT_FALSE(consume_worker_token_context_capability(fd).has_value());
    EXPECT_EQ(::close(fd), -1) << "consumer must close rejected descriptors";
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ProcessSessionCapabilityTest, ConsumesValidLocalSystemCapabilityOnce) {
    int descriptors[2] = {-1, -1};
    ASSERT_EQ(::pipe(descriptors), 0);
    const std::array<u8, 8> payload{
        'V', 'T', 'K', '1', 1, 0xA7, 0x3C, 0x5E};
    ASSERT_EQ(::write(
                  descriptors[1], payload.data(), payload.size()),
              static_cast<ssize_t>(payload.size()));
    ASSERT_EQ(::close(descriptors[1]), 0);

    const auto context =
        consume_worker_token_context_capability(descriptors[0]);
    ASSERT_TRUE(context.has_value());
    EXPECT_EQ(*context, WorkerTokenContext::LocalSystemService);
    EXPECT_EQ(::close(descriptors[0]), -1)
        << "consumer must close accepted descriptors";
}

TEST_F(ProcessSessionTest, DetachedWorkerGetsNoParentDeathDescriptor) {
    ProcessSession session;
    WorkerLaunchSpec spec;
    spec.host_image_path = image;
    spec.guest_image_path = "C:\\Windows\\System32\\SteamService.exe";
    spec.runtime_path = runtime;
    spec.detached = true;

    const pid_t child = session.launch_worker(spec);
    ASSERT_GT(child, 0);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    session.wait_for_workers();

    std::ifstream recorded(output);
    ASSERT_TRUE(recorded);
    std::vector<std::string> lines;
    for (std::string line; std::getline(recorded, line);) {
        lines.push_back(std::move(line));
    }
    // A worker that must outlive its launcher is never death-watched.
    EXPECT_EQ(std::find(lines.begin(), lines.end(), "--parent-death-fd"),
              lines.end())
        << "detached worker must not receive a parent-death descriptor";
}

TEST_F(ProcessSessionTest,
       WorkerMonitoringCreatorGetsIndependentLifetime) {
    ProcessSession session;
    WorkerLaunchSpec spec;
    spec.host_image_path = image;
    spec.guest_image_path = "C:\\Windows\\System32\\monitor.exe";
    spec.runtime_path = runtime;
    spec.inherited_sync =
        "E,76,ready;P,88," + std::to_string(::getpid());

    const pid_t child = session.launch_worker(spec);
    ASSERT_GT(child, 0);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    session.wait_for_workers();

    std::ifstream recorded(output);
    ASSERT_TRUE(recorded);
    std::vector<std::string> lines;
    for (std::string line; std::getline(recorded, line);) {
        lines.push_back(std::move(line));
    }
    EXPECT_NE(std::find(lines.begin(), lines.end(), spec.inherited_sync),
              lines.end());
    EXPECT_EQ(std::find(lines.begin(), lines.end(), "--parent-death-fd"),
              lines.end())
        << "a child waiting on its creator must not be recursively waited";
}

// A Windows child routinely exits before its creator (PyInstaller's stage 2,
// every installer's helper). The creator's own exit then writes the release
// byte into a death pipe nobody reads. That write must fail with EPIPE: with
// SIGPIPE's default action the creator died before publishing its exit code,
// so ITS parent saw 0xFFFFFFFF - Steam logged hardwareupdater "result: -1"
// and relaunched it every 5 s forever.
TEST_F(ProcessSessionTest, CreatorOutlivingItsWorkerExitsNormally) {
    // The worker closes its read end and stays alive until told to go, so the
    // release byte always meets a reader-less pipe. Waiting for the worker to
    // exit instead raced the session's child-exit observer, which reaps it and
    // closes the death pipe first whenever its watcher runs in the creator.
    const auto closed = root / "death-fd-closed";
    const auto go = root / "worker-go";
    const auto closer = root / "close-death-fd.sh";
    {
        std::ofstream script(closer);
        script << "#!/bin/bash\n"
                  "prev=; fd=\n"
                  "for arg in \"$@\"; do\n"
                  "  [ \"$prev\" = --parent-death-fd ] && fd=\"$arg\"\n"
                  "  prev=\"$arg\"; last=\"$arg\"\n"
                  "done\n"
                  "/bin/rm -f \"$last\"\n"
                  "[ -n \"$fd\" ] && eval \"exec $fd<&-\"\n"
                  ": > '" << closed.string() << "'\n"
                  "for i in $(seq 100); do\n"
                  "  [ -e '" << go.string() << "' ] && exit 0\n"
                  "  sleep 0.1\n"
                  "done\n";
    }
    ASSERT_EQ(::chmod(closer.c_str(), 0700), 0);

    const pid_t creator = ::fork();
    ASSERT_GE(creator, 0);
    if (creator == 0) {
        ::signal(SIGPIPE, SIG_DFL);
        ProcessSession session;
        WorkerLaunchSpec spec;
        spec.host_image_path = image;
        spec.guest_image_path = "C:\\leaf.exe";
        spec.guest_command_line = "\"leaf.exe\"";
        spec.guest_current_directory = "C:\\";
        spec.capsule_name = "Test";
        spec.runtime_path = closer;
        const pid_t worker = session.launch_worker(spec);
        if (worker <= 0) ::_exit(2);
        bool reader_gone = false;
        for (int attempt = 0; attempt < 500 && !reader_gone; ++attempt) {
            reader_gone = std::filesystem::exists(closed);
            if (!reader_gone) {
                struct timespec tick {0, 10'000'000};
                ::nanosleep(&tick, nullptr);
            }
        }
        if (!reader_gone) ::_exit(3);
        session.wait_for_workers();
        ::_exit(0);
    }
    int status = 0;
    const pid_t waited = ::waitpid(creator, &status, 0);
    { std::ofstream release(go); }
    ASSERT_EQ(waited, creator);
    ASSERT_FALSE(WIFSIGNALED(status))
        << "creator died of signal " << WTERMSIG(status)
        << " (SIGPIPE is " << SIGPIPE << ")";
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(ProcessSessionParentDeathTest, PublishedExitCodeSurvivesAGoneReader) {
    // Child side of the same contract: publishing the Windows exit code after
    // the parent closed its end must not kill the publisher.
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::signal(SIGPIPE, SIG_DFL);
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) ::_exit(2);
        ::close(fds[0]);
        publish_windows_exit_code(fds[1], 7u);
        ::_exit(0);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_FALSE(WIFSIGNALED(status)) << "signal " << WTERMSIG(status);
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(ProcessSessionParentDeathTest, ChildSurvivesWhenCreatorWriteEndCloses) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::pipe(fds), 0);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        // CreateProcess does not kill the child when the creator exits.
        // Closing the write end is that exit. The child must still be alive.
        ::close(fds[1]);
        ::signal(SIGTERM, SIG_IGN);
        install_parent_death_watch(fds[0]);
        for (;;) ::pause();
        ::_exit(0);
    }

    ::close(fds[0]);
    struct timespec settle {0, 100'000'000};
    ::nanosleep(&settle, nullptr);
    ASSERT_EQ(::close(fds[1]), 0);

    struct timespec after {0, 200'000'000};
    ::nanosleep(&after, nullptr);
    EXPECT_EQ(::kill(child, 0), 0) << "creator exit terminated the worker";
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, WNOHANG), 0);
    ASSERT_EQ(::kill(child, SIGKILL), 0);
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
}

TEST_F(ProcessSessionTest, DeferredMappingStaysUntilOwnedWorkersExit) {
    const auto target = root / "host-app-dir";
    std::filesystem::create_directories(target);
    const auto mapping = root / "app-mapping";
    std::filesystem::create_directory_symlink(target, mapping);
    ASSERT_TRUE(std::filesystem::is_symlink(mapping));

    ProcessSession session;
    WorkerLaunchSpec spec;
    spec.host_image_path = image;
    spec.guest_image_path = "C:\\Vitreo\\Apps\\Game\\Setup.exe";
    spec.runtime_path = runtime;

    ASSERT_GT(session.launch_worker(spec), 0);
    session.defer_symlink_cleanup(mapping, target);
    EXPECT_TRUE(std::filesystem::is_symlink(mapping));

    session.wait_for_workers();
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::symlink_status(mapping)));
    EXPECT_TRUE(std::filesystem::is_directory(target))
        << "cleanup must not follow the link into the host directory";
}

TEST_F(ProcessSessionTest, DeferredCleanupLeavesRetargetedMapping) {
    const auto target = root / "host-app-dir";
    const auto other = root / "other-app-dir";
    std::filesystem::create_directories(target);
    std::filesystem::create_directories(other);
    const auto mapping = root / "app-mapping";
    std::filesystem::create_directory_symlink(target, mapping);

    ProcessSession session;
    session.defer_symlink_cleanup(mapping, target);

    // Another session replaced the mapping between deferral and cleanup.
    std::filesystem::remove(mapping);
    std::filesystem::create_directory_symlink(other, mapping);

    session.wait_for_workers();
    EXPECT_TRUE(std::filesystem::is_symlink(mapping));
    EXPECT_EQ(std::filesystem::read_symlink(mapping), other);
}

// The launcher of an installer that respawns itself elevated exits first; the
// respawned child, and the agent it starts, still open the image through the
// mapping (another process verifies its signature by the path
// QueryFullProcessImageName reports). The mapping and its lease outlive the
// session while any worker runs.
TEST_F(ProcessSessionTest, DeferredMappingSurvivesWorkersStillRunning) {
    const auto target = root / "host-app-dir";
    std::filesystem::create_directories(target);
    const auto mapping = root / "app-mapping";
    std::filesystem::create_directory_symlink(target, mapping);
    const auto lease = root / "app-mapping.vitreo-lease";
    const int lease_fd = ::open(lease.c_str(), O_CREAT | O_RDWR, 0600);
    ASSERT_GE(lease_fd, 0);
    ASSERT_EQ(::flock(lease_fd, LOCK_EX | LOCK_NB), 0);

    const auto linger = root / "linger-worker.sh";
    {
        std::ofstream script(linger);
        script << "#!/bin/sh\nexec /bin/sleep 30\n";
    }
    ASSERT_EQ(::chmod(linger.c_str(), 0700), 0);

    ProcessSession session;
    WorkerLaunchSpec spec;
    spec.host_image_path = image;
    spec.guest_image_path = "C:\\Vitreo\\Apps\\Game\\Setup.exe";
    spec.runtime_path = linger;
    const pid_t worker = session.launch_worker(spec);
    ASSERT_GT(worker, 0);
    session.defer_symlink_cleanup(mapping.string(), target.string(),
                                  lease.string(), lease_fd);

    session.wait_for_workers(/*grace=*/false);
    EXPECT_TRUE(std::filesystem::is_symlink(mapping))
        << "a running worker lost the path to its own image";
    EXPECT_TRUE(std::filesystem::exists(lease))
        << "unlinking the lease lets a new launch reclaim the live mapping";
    // The worker inherited the lease descriptor, so the lock is still held.
    const int probe = ::open(lease.c_str(), O_RDWR);
    ASSERT_GE(probe, 0);
    EXPECT_NE(::flock(probe, LOCK_EX | LOCK_NB), 0);
    (void)::close(probe);

    ASSERT_EQ(::kill(worker, SIGKILL), 0);
    int status = 0;
    while (::waitpid(worker, &status, 0) < 0 && errno == EINTR) {}
}

TEST_F(ProcessSessionTest, DeferredCleanupNeverRemovesRealDirectory) {
    const auto target = root / "host-app-dir";
    std::filesystem::create_directories(target);
    const auto mapping = root / "app-mapping";

    ProcessSession session;
    session.defer_symlink_cleanup(mapping, target);

    // Real capsule content took the name after the mapping was deferred.
    std::filesystem::create_directories(mapping);
    std::ofstream(mapping / "installed.dat") << "payload";

    session.wait_for_workers();
    EXPECT_TRUE(std::filesystem::is_directory(mapping));
    EXPECT_TRUE(std::filesystem::exists(mapping / "installed.dat"));
}

TEST_F(ProcessSessionTest, LaunchDoesNotRunParentForkHandlers) {
    // A parent fork prepare handler can stop all allocator users, including
    // the UI thread. Worker creation must not enter that parent-wide barrier.
    static std::atomic<unsigned> prepares{0};
    static const int registered = ::pthread_atfork(
        +[] { prepares.fetch_add(1, std::memory_order_relaxed); }, nullptr, nullptr);
    ASSERT_EQ(registered, 0);
    const unsigned before = prepares.load(std::memory_order_relaxed);
    ProcessSession session;
    WorkerLaunchSpec spec;
    spec.host_image_path = image;
    spec.guest_image_path = "C:\\child.exe";
    spec.runtime_path = runtime;
    ASSERT_GT(session.launch_worker(spec), 0);
    session.wait_for_workers();
    EXPECT_EQ(prepares.load(std::memory_order_relaxed), before);
    EXPECT_TRUE(std::filesystem::exists(output));
}

TEST_F(ProcessSessionTest, ExecFailureDoesNotLeaveOwnedWorker) {
    ProcessSession session;
    WorkerLaunchSpec spec;
    spec.host_image_path = image;
    spec.guest_image_path = "C:\\child.exe";
    spec.runtime_path = root / "missing-runtime";

    EXPECT_EQ(session.launch_worker(spec), -1);
    EXPECT_EQ(session.owned_worker_count(), 0u);
}

} // namespace
} // namespace vitreo::ipc
