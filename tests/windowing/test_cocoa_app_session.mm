// SPDX-License-Identifier: LGPL-2.1-only
#import <Cocoa/Cocoa.h>
#include "cocoa_bridge_test_fixture.hpp"
#include "ipc/macos_app_identity.hpp"
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

namespace {
void run_for(double seconds) {
    const auto until = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    while (std::chrono::steady_clock::now() < until) {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
            untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
            inMode:NSDefaultRunLoopMode dequeue:YES];
        if (event) [NSApp sendEvent:event];
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
    }
}
WindowData session_window(HWND hwnd, const wchar_t* title) {
    WindowData data;
    data.handle = hwnd;
    data.style = WS::OVERLAPPEDWINDOW;
    data.rect = {100, 100, 540, 380};
    data.client_rect = {0, 0, 440, 280};
    data.title = title;
    data.visible = true;
    return data;
}
}

TEST_F(CocoaBridgeTest, DockSessionTargetExcludesHiddenAndToolWindows) {
    auto data = session_window(0x801, L"Dock target");
    ASSERT_TRUE(bridge.create_window(data.handle, data).is_ok());
    bridge.set_visible(data.handle, true);
    EXPECT_EQ(bridge.dock_session_target(), data.handle);
    auto tool = session_window(0x802, L"Tool");
    tool.ex_style = WS_EX::TOOLWINDOW;
    ASSERT_TRUE(bridge.create_window(tool.handle, tool).is_ok());
    bridge.set_visible(tool.handle, true);
    bridge.on_native_window_became_key(tool.handle);
    EXPECT_EQ(bridge.dock_session_target(), data.handle);
    bridge.set_visible(data.handle, false);
    EXPECT_EQ(bridge.dock_session_target(), NULL_HWND);
}

TEST_F(CocoaBridgeTest, DockSessionTrayReopenIsDeliveredOnce) {
    auto data = session_window(0x803, L"Tray target");
    ASSERT_TRUE(bridge.create_window(data.handle, data).is_ok());
    bridge.set_visible(data.handle, false);
    unsigned delivered = 0;
    bridge.set_notify_icon_reopen_target([] { return std::optional<u64>(7); });
    bridge.set_notify_icon_input_handler([&](u64 key, NotifyIconInput input, i32, i32) {
        EXPECT_EQ(key, 7u);
        EXPECT_EQ(input, NotifyIconInput::Activate);
        ++delivered;
    });
    bridge.publish_dock_session();
    bridge.on_dock_reopen(false);
    EXPECT_EQ(delivered, 1u);
    EXPECT_FALSE(bridge.native_window_is_visible(data.handle));
}

// This helper runs in a freshly exec'd process: it exercises real AppKit
// activation policy and the distributed-notification transport, not a mock.
TEST_F(CocoaBridgeTest, DockSessionNativeWorker) {
    const char* endpoint = getenv("VITREO_DOCK_TEST_FD");
    if (!endpoint) GTEST_SKIP() << "subprocess helper";
    MessageQueue queue;
    bridge.set_message_queue(queue);
    // The AppUserModelID group this worker reports (default: the root's).
    if (const char* group = getenv("VITREO_DOCK_TEST_GROUP")) {
        const std::string value = group;
        bridge.set_app_user_model_id_resolver([value](HWND) { return value; });
    }
    const int fd = atoi(endpoint);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    auto data = session_window(0x810, L"Vitreo Dock regression child");
    ASSERT_TRUE(bridge.create_window(data.handle, data).is_ok());
    bridge.set_visible(data.handle, true);
    run_for(2.2);
    const auto policy = [NSApp activationPolicy];
    const char ready = policy == NSApplicationActivationPolicyAccessory ? 'a'
        : policy == NSApplicationActivationPolicyRegular ? 'r' : 'x';
    ASSERT_EQ(write(fd, &ready, 1), 1);
    bool waiting_restore = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        char command = 0;
        if (read(fd, &command, 1) == 1) {
            if (command == 'q') break;
            if (command == 'm') {
                bridge.on_native_window_became_key(data.handle);
                for (NSWindow* window in [NSApp windows]) {
                    if ([window.title isEqualToString:@"Vitreo Dock regression child"])
                        [window miniaturize:nil];
                }
                waiting_restore = true;
                const char ack = 'm';
                write(fd, &ack, 1);
            }
            if (command == 'p') {
                const char policy = [NSApp activationPolicy] == NSApplicationActivationPolicyRegular ? 'r' : 'a';
                write(fd, &policy, 1);
            }
        }
        run_for(0.03);
        MSG close_message{};
        if (queue.peek(close_message, data.handle, WM::CLOSE, WM::CLOSE, PM::REMOVE)) {
            const char closed = 'c';
            write(fd, &closed, 1);
        }
        if (waiting_restore) {
            for (NSWindow* window in [NSApp windows]) {
                if ([window.title isEqualToString:@"Vitreo Dock regression child"] &&
                    !window.miniaturized && window.keyWindow && NSApp.active) {
                    waiting_restore = false;
                    const char restored = 'w';
                    write(fd, &restored, 1);
                }
            }
        }
    }
    close(fd);
}

TEST_F(CocoaBridgeTest, DockSessionNativeChildRestoreAndOwnerHandoff) {
    // The existing test environment deliberately makes every test an accessory.
    // This regression needs a real root plus separately exec'd accessory children.
    struct RootRole {
        std::string previous = getenv("VITREO_MACOS_PROCESS_ROLE")
            ? getenv("VITREO_MACOS_PROCESS_ROLE") : "";
        RootRole() { setenv("VITREO_MACOS_PROCESS_ROLE", "root", 1); }
        ~RootRole() {
            if (previous.empty()) unsetenv("VITREO_MACOS_PROCESS_ROLE");
            else setenv("VITREO_MACOS_PROCESS_ROLE", previous.c_str(), 1);
        }
    } role;

    auto root_window = session_window(0x820, L"Vitreo Dock regression owner");
    ASSERT_TRUE(bridge.create_window(root_window.handle, root_window).is_ok());
    bridge.set_visible(root_window.handle, false);
    char executable[4096];
    uint32_t length = sizeof(executable);
    ASSERT_EQ(_NSGetExecutablePath(executable, &length), 0);
    struct Worker { pid_t pid; int fd; };
    auto spawn_worker = [&](bool same_session) {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return Worker{-1, -1};
        fcntl(sockets[0], F_SETFL, O_NONBLOCK);
        extern char** environ;
        std::vector<std::string> environment;
        for (char** e = environ; *e; ++e) {
            const std::string item(*e);
            if (!item.starts_with("VITREO_MACOS_PROCESS_ROLE=") &&
                !item.starts_with("VITREO_MACOS_APP_SESSION=") &&
                !item.starts_with("VITREO_DOCK_TEST_FD=")) environment.push_back(item);
        }
        uuid_t uuid; uuid_generate_random(uuid);
        char separate[37]; uuid_unparse_lower(uuid, separate);
        environment.push_back("VITREO_MACOS_PROCESS_ROLE=child");
        environment.push_back("VITREO_MACOS_APP_SESSION=" +
            (same_session ? ipc::macos_app_identity() : std::string(separate)));
        environment.push_back("VITREO_DOCK_TEST_FD=" + std::to_string(sockets[1]));
        std::vector<char*> env;
        for (auto& item : environment) env.push_back(item.data());
        env.push_back(nullptr);
        char filter[] = "--gtest_filter=CocoaBridgeTest.DockSessionNativeWorker";
        char* argv[] = {executable, filter, nullptr};
        pid_t child = 0;
        const int result = posix_spawn(&child, executable, nullptr, nullptr, argv, env.data());
        close(sockets[1]);
        return Worker{result == 0 ? child : -1, sockets[0]};
    };
    auto response = [&](Worker worker, double timeout) {
        char value = 0;
        const auto until = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(static_cast<int>(timeout * 1000));
        while (std::chrono::steady_clock::now() < until) {
            if (read(worker.fd, &value, 1) == 1) return value;
            run_for(0.02);
        }
        return value;
    };
    auto finish = [](Worker worker) {
        write(worker.fd, "q", 1);
        close(worker.fd);
        int status = 0;
        EXPECT_EQ(waitpid(worker.pid, &status, 0), worker.pid);
        EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    };
    Worker first = spawn_worker(true);
    ASSERT_GT(first.pid, 0);
    EXPECT_EQ(response(first, 5), 'a');
    Worker second = spawn_worker(true);
    ASSERT_GT(second.pid, 0);
    EXPECT_EQ(response(second, 5), 'a');
    Worker unrelated = spawn_worker(false);
    ASSERT_GT(unrelated.pid, 0);
    // A separate launch elects its own owner; it never joins this family.
    EXPECT_EQ(response(unrelated, 5), 'r');
    EXPECT_EQ([NSApp activationPolicy], NSApplicationActivationPolicyRegular);
    for (Worker worker : {first, second, unrelated}) {
        write(worker.fd, "m", 1);
        EXPECT_EQ(response(worker, 2), 'm');
        run_for(0.1);
    }
    [NSApp activateIgnoringOtherApps:YES];
    run_for(0.1);
    bridge.on_dock_reopen(false);
    EXPECT_EQ(response(second, 3), 'w');
    EXPECT_EQ(response(first, 0.2), 0);
    EXPECT_EQ(response(unrelated, 0.2), 0);
    // A crash has no leave notification: stale OS identity must be pruned.
    EXPECT_EQ(kill(second.pid, SIGKILL), 0);
    int crashed_status = 0;
    EXPECT_EQ(waitpid(second.pid, &crashed_status, 0), second.pid);
    EXPECT_TRUE(WIFSIGNALED(crashed_status));
    close(second.fd);
    run_for(1.2);
    [NSApp activateIgnoringOtherApps:YES];
    run_for(0.1);
    bridge.on_dock_reopen(false);
    EXPECT_EQ(response(first, 3), 'w');
    bridge.request_guest_quit();
    EXPECT_EQ(response(first, 2), 'c');
    EXPECT_EQ(response(unrelated, 0.2), 0);
    bridge.stop_dock_session();
    run_for(1.3);
    write(first.fd, "p", 1);
    EXPECT_EQ(response(first, 2), 'r');
    finish(first);
    finish(unrelated);
}

// The taskbar groups by AppUserModelID, not by process tree (Learn
// "Application User Model IDs"): a child with the root's ID stays under the
// root's Dock tile, a child with another ID -- a game a launcher started --
// gets its own, and the root's Dock Quit closes only its own group.
TEST_F(CocoaBridgeTest, DockSessionChildWithAnotherAppUserModelIdOwnsItsTile) {
    struct RootRole {
        std::string previous = getenv("VITREO_MACOS_PROCESS_ROLE")
            ? getenv("VITREO_MACOS_PROCESS_ROLE") : "";
        RootRole() { setenv("VITREO_MACOS_PROCESS_ROLE", "root", 1); }
        ~RootRole() {
            if (previous.empty()) unsetenv("VITREO_MACOS_PROCESS_ROLE");
            else setenv("VITREO_MACOS_PROCESS_ROLE", previous.c_str(), 1);
        }
    } role;
    auto root_window = session_window(0x840, L"Vitreo Dock group owner");
    ASSERT_TRUE(bridge.create_window(root_window.handle, root_window).is_ok());
    bridge.set_visible(root_window.handle, false);
    char executable[4096];
    uint32_t length = sizeof(executable);
    ASSERT_EQ(_NSGetExecutablePath(executable, &length), 0);
    struct Worker { pid_t pid; int fd; };
    auto spawn_worker = [&](const char* group) {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return Worker{-1, -1};
        fcntl(sockets[0], F_SETFL, O_NONBLOCK);
        extern char** environ;
        std::vector<std::string> environment;
        for (char** e = environ; *e; ++e) {
            const std::string item(*e);
            if (!item.starts_with("VITREO_MACOS_PROCESS_ROLE=") &&
                !item.starts_with("VITREO_MACOS_APP_SESSION=") &&
                !item.starts_with("VITREO_DOCK_TEST_GROUP=") &&
                !item.starts_with("VITREO_DOCK_TEST_FD=")) environment.push_back(item);
        }
        environment.push_back("VITREO_MACOS_PROCESS_ROLE=child");
        environment.push_back("VITREO_MACOS_APP_SESSION=" + ipc::macos_app_identity());
        if (group) environment.push_back(std::string("VITREO_DOCK_TEST_GROUP=") + group);
        environment.push_back("VITREO_DOCK_TEST_FD=" + std::to_string(sockets[1]));
        std::vector<char*> env;
        for (auto& item : environment) env.push_back(item.data());
        env.push_back(nullptr);
        char filter[] = "--gtest_filter=CocoaBridgeTest.DockSessionNativeWorker";
        char* argv[] = {executable, filter, nullptr};
        pid_t child = 0;
        const int result = posix_spawn(&child, executable, nullptr, nullptr, argv, env.data());
        close(sockets[1]);
        return Worker{result == 0 ? child : -1, sockets[0]};
    };
    auto response = [&](Worker worker, double timeout) {
        char value = 0;
        const auto until = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(static_cast<int>(timeout * 1000));
        while (std::chrono::steady_clock::now() < until) {
            if (read(worker.fd, &value, 1) == 1) return value;
            run_for(0.02);
        }
        return value;
    };
    auto finish = [](Worker worker) {
        write(worker.fd, "q", 1);
        close(worker.fd);
        int status = 0;
        EXPECT_EQ(waitpid(worker.pid, &status, 0), worker.pid);
        EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    };
    Worker helper = spawn_worker(nullptr);
    ASSERT_GT(helper.pid, 0);
    EXPECT_EQ(response(helper, 5), 'a');
    Worker game = spawn_worker("image:C:\\GAMES\\GAME.EXE");
    ASSERT_GT(game.pid, 0);
    EXPECT_EQ(response(game, 5), 'r');
    EXPECT_EQ([NSApp activationPolicy], NSApplicationActivationPolicyRegular);
    bridge.request_guest_quit();
    EXPECT_EQ(response(helper, 2), 'c');
    EXPECT_EQ(response(game, 0.5), 0);
    write(game.fd, "p", 1);
    EXPECT_EQ(response(game, 2), 'r');
    finish(helper);
    finish(game);
}

TEST_F(CocoaBridgeTest, DockSessionWorkerShutdownDetachesBeforeNativeCleanup) {
    auto data = session_window(0x830, L"Dock shutdown regression");
    ASSERT_TRUE(bridge.create_window(data.handle, data).is_ok());
    std::thread worker([&] { bridge.begin_shutdown(); });
    worker.join();
    EXPECT_FALSE(bridge.route_dock_session());
    run_for(0.1);
    EXPECT_EQ([NSApp activationPolicy], NSApplicationActivationPolicyAccessory);
}
