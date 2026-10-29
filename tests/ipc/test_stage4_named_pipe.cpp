// SPDX-License-Identifier: LGPL-2.1-only
// Stage 4: Windows named pipes over AF_UNIX sockets. Server in the parent,
// client in a forked child; bidirectional bytes + peer pid. Self-contained
// (uses sockets directly); runs regardless of the VITREO_IPC gate.

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <sys/socket.h>

#include "ipc/unix_pipe.hpp"

using namespace vitreo;
using namespace vitreo::ipc;

TEST(Stage4NamedPipe, CrossProcessBidirectional) {
    const std::string name = "\\\\.\\pipe\\vtm_pipe_" + std::to_string(::getpid());

    auto server = UnixPipe::create_server(name);
    ASSERT_NE(server, nullptr);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // Child = client. Retry connect briefly (server may still be in accept).
        std::shared_ptr<UnixPipe> client;
        for (int i = 0; i < 100 && !client; ++i) {
            client = UnixPipe::connect_client(name);
            if (!client) usleep(10 * 1000);
        }
        if (!client) _exit(10);
        char hello[6] = "hello";
        if (client->write(hello, 5) != 5) _exit(11);
        char back[4] = {0};
        if (client->read(back, 3) != 3) _exit(12);
        if (std::memcmp(back, "ack", 3) != 0) _exit(13);
        _exit(0);
    }

    auto conn = server->accept(/*timeout_ms=*/5000);
    ASSERT_NE(conn, nullptr);

    char buf[8] = {0};
    isize n = conn->read(buf, 5);
    EXPECT_EQ(n, 5);
    EXPECT_EQ(std::memcmp(buf, "hello", 5), 0);
    EXPECT_EQ(conn->write("ack", 3), 3);
    EXPECT_GT(conn->peer_pid(), 0);

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child step " << WEXITSTATUS(status);
}

TEST(Stage4NamedPipe, ConnectWithoutServerFails) {
    const std::string name = "\\\\.\\pipe\\vtm_nopipe_" + std::to_string(::getpid());
    EXPECT_EQ(UnixPipe::connect_client(name), nullptr);
}

TEST(Stage4NamedPipe, ClosedPeerReturnsWriteErrorWithoutSigpipe) {
    auto [writer, reader] = UnixPipe::create_pair();
    ASSERT_NE(writer, nullptr);
    ASSERT_NE(reader, nullptr);

    reader.reset();
    EXPECT_EQ(writer->write("x", 1), -1);
}

TEST(Stage4NamedPipe, ClosedPeerReturnsReadEof) {
    auto [reader, writer] = UnixPipe::create_pair();
    ASSERT_NE(reader, nullptr);
    ASSERT_NE(writer, nullptr);
    EXPECT_EQ(reader->peek_available(), 0);
    ASSERT_EQ(writer->write("x", 1), 1);
    writer.reset();
    EXPECT_EQ(reader->peek_available(), 1);
    char byte = '\0';
    EXPECT_EQ(reader->read(&byte, 1), 1);
    EXPECT_EQ(byte, 'x');
    EXPECT_EQ(reader->peek_available(), -1);
    EXPECT_EQ(reader->read(&byte, 1), 0);
}

TEST(Stage4NamedPipe, NonblockingLargeWriteReturnsWithoutPeerReading) {
    auto [writer, reader] = UnixPipe::create_pair();
    ASSERT_NE(writer, nullptr);
    ASSERT_NE(reader, nullptr);
    int small_buffer = 4096;
    ASSERT_EQ(::setsockopt(writer->fd(), SOL_SOCKET, SO_SNDBUF,
                          &small_buffer, sizeof(small_buffer)), 0);
    std::vector<u8> payload(1024 * 1024, 0x5A);
    std::atomic<bool> returned{false};
    isize transferred = -1;
    std::thread sender([&] {
        transferred = writer->write_nonblocking(payload.data(), payload.size());
        returned.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(250);
    while (!returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool nonblocking = returned.load(std::memory_order_acquire);
    // Unblock a broken implementation before joining, so the regression
    // fails instead of hanging the test runner indefinitely.
    if (!nonblocking) ::shutdown(writer->fd(), SHUT_RDWR);
    sender.join();
    EXPECT_TRUE(nonblocking);
    EXPECT_GE(transferred, 0);
    EXPECT_LT(transferred, static_cast<isize>(payload.size()));
}

TEST(Stage4NamedPipe, SynchronousLargeWriteWaitsAndTransfersEveryByte) {
    auto [writer, reader] = UnixPipe::create_pair();
    ASSERT_NE(writer, nullptr);
    ASSERT_NE(reader, nullptr);
    int small_buffer = 4096;
    ASSERT_EQ(::setsockopt(writer->fd(), SOL_SOCKET, SO_SNDBUF,
                          &small_buffer, sizeof(small_buffer)), 0);
    std::vector<u8> payload(1024 * 1024, 0xA7);
    isize written = -1;
    std::thread sender([&] {
        written = writer->write(payload.data(), payload.size());
        ::shutdown(writer->fd(), SHUT_WR);
    });
    usize total = 0;
    u8 chunk[8192];
    for (;;) {
        const isize n = reader->read(chunk, sizeof(chunk));
        if (n <= 0) { EXPECT_EQ(n, 0); break; }
        for (isize i = 0; i < n; ++i) EXPECT_EQ(chunk[i], 0xA7);
        total += static_cast<usize>(n);
    }
    sender.join();
    EXPECT_EQ(written, static_cast<isize>(payload.size()));
    EXPECT_EQ(total, payload.size());
}

// Learn CreateNamedPipe: every instance of a pipe name is reachable, and a
// client connects to whichever instance is waiting. A second instance used to
// unlink and re-bind the name's socket, stranding the first instance's
// ConnectNamedPipe; closing the second then removed the name altogether.
// Crashpad's handler is exactly this: its first instance is created by the
// client process and inherited, the handler adds more, and at shutdown it
// connects to itself once per instance to wake each listening thread. The
// stranded one never woke, so every crash handler outlived Steam.
TEST(Stage4NamedPipe, InstancesOfOneNameShareTheNameAcrossInheritance) {
    const std::string name =
        "\\\\.\\pipe\\vtm_multi_" + std::to_string(::getpid());
    auto first = UnixPipe::create_server(name);
    ASSERT_NE(first, nullptr);
    // The creator hands the instance to a child and gives up the name, as
    // prepare_inherited_named_pipes does.
    first->release_path_ownership();
    const int inherited = ::dup(first->fd());
    ASSERT_GE(inherited, 0);

    int ready[2];
    int go[2];
    ASSERT_EQ(::pipe(ready), 0);
    ASSERT_EQ(::pipe(go), 0);
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // Child: forget the parent's copy (a real child starts empty), adopt
        // the inherited socket, then create a second instance of the name.
        first.reset();
        auto adopted = UnixPipe::adopt_server(inherited, name);
        if (!adopted) _exit(20);
        auto second = UnixPipe::create_server(name);
        if (!second) _exit(21);
        if (second->fd() != adopted->fd()) _exit(22);  // joined, not re-bound
        char c = 'r';
        if (::write(ready[1], &c, 1) != 1) _exit(23);
        // Two clients: each instance can take one.
        auto a = adopted->accept(3000);
        auto b = second->accept(3000);
        if (!a || !b) _exit(24);
        // Closing the second instance must leave the name served.
        second.reset();
        c = 'c';
        if (::write(ready[1], &c, 1) != 1) _exit(25);
        if (::read(go[0], &c, 1) != 1) _exit(26);
        auto third = adopted->accept(3000);
        if (!third) _exit(27);
        _exit(0);
    }
    ::close(inherited);
    // Keep only our ends, so a child that fails early reads as EOF here
    // instead of a hang.
    ::close(ready[1]);
    ::close(go[0]);
    char c = 0;
    const auto child_failed = [&] {
        int status = 0;
        (void)waitpid(pid, &status, 0);
        return testing::AssertionFailure()
               << "child step " << (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    };
    if (::read(ready[0], &c, 1) != 1) { EXPECT_TRUE(child_failed()); return; }
    auto client1 = UnixPipe::connect_client(name);
    auto client2 = UnixPipe::connect_client(name);
    EXPECT_NE(client1, nullptr);
    EXPECT_NE(client2, nullptr);
    if (::read(ready[0], &c, 1) != 1) { EXPECT_TRUE(child_failed()); return; }
    auto client3 = UnixPipe::connect_client(name);
    EXPECT_NE(client3, nullptr) << "closing one instance removed the name";
    c = 'g';
    ASSERT_EQ(::write(go[1], &c, 1), 1);
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child step " << WEXITSTATUS(status);
}

TEST(Stage4NamedPipe, SecondInstanceInOneProcessJoinsTheFirst) {
    const std::string name =
        "\\\\.\\pipe\\vtm_join_" + std::to_string(::getpid());
    auto first = UnixPipe::create_server(name);
    auto second = UnixPipe::create_server(name);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->fd(), second->fd());
    auto client = UnixPipe::connect_client(name);
    ASSERT_NE(client, nullptr);
    // Either instance may take it; the other sees nothing waiting.
    auto got_first = first->accept(1000);
    auto got_second = got_first ? nullptr : second->accept(1000);
    EXPECT_TRUE(got_first || got_second);
    // Last instance closing removes the name.
    first.reset();
    EXPECT_NE(UnixPipe::connect_client(name), nullptr);  // second still serves
    second.reset();
    EXPECT_EQ(UnixPipe::connect_client(name), nullptr);
}
