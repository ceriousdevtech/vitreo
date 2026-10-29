#pragma once

/// @file ws2_loopback_fixture.hpp
/// @brief Shared fixture for the end-to-end ws2_32 loopback contract tests.
///
/// Included by every test_ws2_32_loopback*.cpp. Ws2LoopbackTest is a
/// global-namespace class defined identically in each including translation
/// unit, because every TEST_F of a suite must name the same fixture type; all
/// its members are defined in-class. LoopbackServer and ThunkInvoker are
/// global-namespace classes for the same reason: a fixture member of
/// anonymous-namespace type would differ per translation unit.
///
/// Drives the actual ws2_32 thunk lambdas (the same ones the guest hits via
/// the IAT) through a stdcall-on-stack invocation harness against a
/// localhost TCP server thread serving a deterministic multi-MB payload.
///
/// This catches contract bugs that a Windows replay of the calls cannot
/// reach, because socket I/O (recv/send/select/connect) is skipped.
///
/// The patterns exercised mirror Steam's bootstrap downloader:
///   socket → ioctlsocket(FIONBIO=1) → connect → select(write) → send
///        → repeat[ select(read) → recv ] until EOF.
///
/// Failure modes this test is designed to catch:
///   - recv writing fewer/more bytes than it returned
///   - corrupted bytes in guest memory (write_bytes path)
///   - select returning ready when no data
///   - select reporting wrong sockets ready
///   - send fragmenting in unexpected ways
///   - non-blocking connect not completing via poll fallback
///   - premature EOF causing a (0:200) style empty-body failure

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <winapi/networking/ws2_32/ws2_32.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// LoopbackServer — minimal localhost TCP server serving a fixed payload.
// =============================================================================

class LoopbackServer {
public:
    /// Serve `payload` to every accepted connection, then close-graceful.
    /// `max_chunk` bounds the per-write size to provoke fragmentation and
    /// force the client into multi-recv loops (default 65536 bytes).
    LoopbackServer(std::vector<uint8_t> payload, size_t max_chunk = 65536)
        : payload_(std::move(payload)), max_chunk_(max_chunk) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;  // ephemeral
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        socklen_t alen = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &alen);
        port_ = ntohs(addr.sin_port);
        if (::listen(listen_fd_, 4) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        thread_ = std::thread([this] { run(); });
    }

    ~LoopbackServer() {
        stop();
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] uint16_t port() const noexcept { return port_; }
    [[nodiscard]] bool ok() const noexcept { return listen_fd_ >= 0; }

private:
    void run() {
        while (!stop_.load(std::memory_order_acquire)) {
            sockaddr_in cli{};
            socklen_t clen = sizeof(cli);
            int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &clen);
            if (cfd < 0) return;
            // Disable Nagle so chunk-fragment patterns hit the wire as written.
            int yes = 1;
            ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

            // Drain whatever the client sends (we don't parse — we just dump
            // the payload unconditionally). Read until we see a CRLFCRLF
            // (HTTP-style) or 1 KB, whichever first.
            char rbuf[1024];
            int total = 0;
            while (total < static_cast<int>(sizeof(rbuf))) {
                ssize_t r = ::recv(cfd, rbuf + total, sizeof(rbuf) - total, 0);
                if (r <= 0) break;
                total += static_cast<int>(r);
                if (total >= 4 && std::memcmp(rbuf + total - 4, "\r\n\r\n", 4) == 0) break;
            }

            // Send payload in deterministic chunks.
            size_t off = 0;
            while (off < payload_.size() && !stop_.load(std::memory_order_acquire)) {
                size_t want = std::min(max_chunk_, payload_.size() - off);
                ssize_t w = ::send(cfd, payload_.data() + off, want, 0);
                if (w <= 0) break;
                off += static_cast<size_t>(w);
            }
            ::shutdown(cfd, SHUT_WR);
            // Drain any trailing client data so close is graceful.
            char drain[256];
            while (::recv(cfd, drain, sizeof(drain), 0) > 0) {}
            ::close(cfd);
        }
    }

    std::vector<uint8_t> payload_;
    size_t max_chunk_;
    int listen_fd_{-1};
    uint16_t port_{0};
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

// =============================================================================
// Thunk dispatch helper — builds a 32-bit stdcall stack frame and invokes
// the registered ExportEntry handler.
// =============================================================================

class ThunkInvoker {
public:
    ThunkInvoker(GuestMemory& mem, cpu::CpuState& cpu, const DllExports& exp)
        : mem_(mem), cpu_(cpu), exp_(exp) {}

    /// Invoke a ws2_32 export by name with stdcall args (4 bytes each).
    /// Returns the 32-bit value placed in EAX/RAX by the handler.
    /// Args are taken as uint64_t for convenience (truncated to 32 bits).
    uint32_t call(const std::string& name, std::initializer_list<uint64_t> args) {
        const ExportEntry* entry = nullptr;
        for (auto& e : exp_.entries) {
            if (e.name == name) { entry = &e; break; }
        }
        if (!entry) {
            ADD_FAILURE() << "Export not found: " << name;
            return 0;
        }

        // Reserve room on the guest stack for [ret_addr][arg0]...[argN-1].
        // The Marshaler reads args at sp()+4+i*4.
        uint32_t sp = static_cast<uint32_t>(cpu_.sp());
        const size_t frame = 4 + args.size() * 4;
        sp -= static_cast<uint32_t>(frame);
        cpu_.set_sp(sp);

        // Write a sentinel return address (not used — handler ret).
        EXPECT_TRUE(mem_.write_u32(sp, 0xDEADBEEFu).is_ok());
        size_t i = 0;
        for (uint64_t a : args) {
            EXPECT_TRUE(mem_.write_u32(
                sp + 4 + 4 * static_cast<uint32_t>(i),
                static_cast<uint32_t>(a)).is_ok());
            ++i;
        }

        Marshaler m(mem_);
        m.attach_cpu(cpu_);
        cpu_.gpr.rax = 0;
        entry->handler(m);
        uint32_t ret = static_cast<uint32_t>(cpu_.gpr.rax);

        // Restore the caller's stack pointer.
        cpu_.set_sp(sp + static_cast<uint32_t>(frame));
        return ret;
    }

private:
    GuestMemory& mem_;
    cpu::CpuState& cpu_;
    const DllExports& exp_;
};

// =============================================================================
// Deterministic payload generator — easy to verify byte-for-byte.
// =============================================================================

inline std::vector<uint8_t> make_payload(size_t n, uint64_t seed = 0xC0FFEEULL) {
    std::vector<uint8_t> out(n);
    std::mt19937_64 rng(seed);
    size_t i = 0;
    while (i + 8 <= n) {
        uint64_t v = rng();
        std::memcpy(out.data() + i, &v, 8);
        i += 8;
    }
    while (i < n) {
        out[i++] = static_cast<uint8_t>(rng());
    }
    return out;
}

// =============================================================================
// Test fixture
// =============================================================================

class Ws2LoopbackTest : public ::testing::Test {
protected:
    static constexpr GuestAddr STACK_BASE   = 0x80000;
    static constexpr GuestAddr STACK_SIZE   = 0x10000;
    static constexpr GuestAddr DATA_BASE    = 0x100000;
    static constexpr GuestAddr DATA_SIZE    = 0x2000000;  // 32 MB
    static constexpr GuestAddr FDSET_ADDR   = DATA_BASE + 0x1F00000;
    static constexpr GuestAddr ADDR_ADDR    = DATA_BASE + 0x1FFF000;
    static constexpr GuestAddr TIMEVAL_ADDR = DATA_BASE + 0x1FFF100;
    static constexpr GuestAddr OUT_ADDR     = DATA_BASE + 0x1FFF200;

    void SetUp() override {
        ASSERT_TRUE(mem_.map_region(STACK_BASE, STACK_SIZE, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(mem_.map_region(DATA_BASE, DATA_SIZE, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(mem_.map_region(WS2_SCRATCH_BASE, WS2_SCRATCH_SIZE, MemoryProtection::ReadWrite).is_ok());

        handles_ = std::make_unique<HandleTable>();
        sync_ = std::make_unique<SyncManager>(*handles_);
        handles_->set_close_callback(
            [this](Handle handle, HandleType type, void*) {
                return sync_->on_handle_close(handle, type);
            });
        mgr_.set_sync_manager(sync_.get());

        cpu_.init_protected_mode();
        cpu_.set_sp(STACK_BASE + STACK_SIZE - 0x100);

        exp_ = create_ws2_thunks(mgr_);
        inv_ = std::make_unique<ThunkInvoker>(mem_, cpu_, exp_);

        // WSAStartup(0x0202, &wsadata)
        const uint32_t WSADATA = OUT_ADDR;
        uint32_t rc = inv_->call("WSAStartup", {0x0202u, WSADATA});
        ASSERT_EQ(rc, 0u);
    }

    void TearDown() override {
        inv_.reset();
        if (handles_) handles_->set_close_callback(nullptr);
        sync_.reset();
        handles_.reset();
    }

    /// Drive the full Steam-style download pattern.
    /// Returns the exact bytes the guest received.
    std::vector<uint8_t> download_via_thunks(uint16_t server_port,
                                             size_t expected_total,
                                             const std::string& http_request) {
        // socket(AF_INET=2, SOCK_STREAM=1, IPPROTO_TCP=6)
        uint32_t sock = inv_->call("socket", {2u, 1u, 6u});
        EXPECT_GE(sock, 0x10000u);
        if (sock < 0x10000u) return {};

        // ioctlsocket(sock, FIONBIO=0x8004667E, &one)
        EXPECT_TRUE(mem_.write_u32(OUT_ADDR + 0x10, 1u).is_ok());
        uint32_t ic = inv_->call("ioctlsocket",
                                 {sock, 0x8004667Eu, OUT_ADDR + 0x10});
        EXPECT_EQ(ic, 0u);

        // Build sockaddr_in at ADDR_ADDR for 127.0.0.1:server_port
        EXPECT_TRUE(mem_.write_u16(ADDR_ADDR + 0, 2).is_ok());                 // AF_INET
        EXPECT_TRUE(mem_.write_u16(ADDR_ADDR + 2, htons(server_port)).is_ok());// port
        EXPECT_TRUE(mem_.write_u32(ADDR_ADDR + 4, htonl(INADDR_LOOPBACK)).is_ok());
        for (int i = 8; i < 16; ++i) (void)mem_.write_u8(ADDR_ADDR + i, 0);

        // A nonblocking connect reports SOCKET_ERROR / WSAEWOULDBLOCK and
        // completes in the background (ws2_core_probe connect_nb_ok: rc=-1
        // err=10035); the writability spin below waits for it.
        uint32_t cr = inv_->call("connect", {sock, ADDR_ADDR, 16u});
        if (cr != 0u) {
            EXPECT_EQ(cr, 0xFFFFFFFFu);
            EXPECT_EQ(mgr_.last_error(), 10035u);
        }

        // Spin a few zero-timeout selects waiting for the socket to become
        // writable (sanity — already writable on loopback after connect).
        auto fdset_with_one = [&](GuestAddr base, uint32_t s) {
            (void)mem_.write_u32(base, 1u);
            (void)mem_.write_u32(base + 4, s);
        };
        EXPECT_TRUE(mem_.write_u32(TIMEVAL_ADDR, 0u).is_ok());
        EXPECT_TRUE(mem_.write_u32(TIMEVAL_ADDR + 4, 0u).is_ok());

        bool writable = false;
        for (int spin = 0; spin < 100 && !writable; ++spin) {
            fdset_with_one(FDSET_ADDR + 0x100, sock);  // write set
            (void)mem_.write_u32(FDSET_ADDR, 0u);       // empty read set
            (void)mem_.write_u32(FDSET_ADDR + 0x200, 0u); // empty except set
            uint32_t sr = inv_->call(
                "select",
                {0u, FDSET_ADDR, FDSET_ADDR + 0x100, FDSET_ADDR + 0x200, TIMEVAL_ADDR});
            (void)sr;
            uint32_t wcount = mem_.read_u32(FDSET_ADDR + 0x100).value_or(0);
            writable = wcount >= 1;
            if (!writable) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(writable) << "loopback socket must become writable";

        // Write HTTP request bytes into guest mem and send.
        const GuestAddr REQ_BUF = OUT_ADDR + 0x100;
        EXPECT_LE(http_request.size(), 1024u);
        auto wr = mem_.write_bytes(REQ_BUF,
                                   reinterpret_cast<const Byte*>(http_request.data()),
                                   http_request.size());
        EXPECT_TRUE(wr.is_ok());
        uint32_t sent_total = 0;
        while (sent_total < http_request.size()) {
            uint32_t s = inv_->call("send",
                                    {sock,
                                     REQ_BUF + sent_total,
                                     static_cast<uint32_t>(http_request.size() - sent_total),
                                     0u});
            // SOCKET_ERROR = 0xFFFFFFFF
            if (s == 0xFFFFFFFFu) {
                // EWOULDBLOCK is fine on a really slow loopback; spin select
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            sent_total += s;
        }

        // Recv loop driven by select(read).
        const GuestAddr RECV_BUF = DATA_BASE + 0x1000;     // 16 MB scratch
        constexpr uint32_t RECV_CHUNK = 16384;
        std::vector<uint8_t> received;
        received.reserve(expected_total);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            // Wait briefly for readability.
            fdset_with_one(FDSET_ADDR, sock);             // read set
            (void)mem_.write_u32(FDSET_ADDR + 0x100, 0u); // empty write set
            (void)mem_.write_u32(FDSET_ADDR + 0x200, 0u); // empty except set
            // 10ms timeout
            (void)mem_.write_u32(TIMEVAL_ADDR, 0u);
            (void)mem_.write_u32(TIMEVAL_ADDR + 4, 10000u);
            (void)inv_->call(
                "select",
                {0u, FDSET_ADDR, FDSET_ADDR + 0x100, FDSET_ADDR + 0x200, TIMEVAL_ADDR});

            uint32_t r = inv_->call("recv", {sock, RECV_BUF, RECV_CHUNK, 0u});
            if (r == 0xFFFFFFFFu) {
                // WSAEWOULDBLOCK — keep polling
                continue;
            }
            if (r == 0) {
                // EOF — server done
                break;
            }
            // Read back from guest memory and append.
            std::vector<Byte> chunk(r);
            auto rb = mem_.read_bytes(RECV_BUF, chunk.data(), r);
            EXPECT_TRUE(rb.is_ok());
            received.insert(received.end(), chunk.begin(), chunk.end());
            if (received.size() >= expected_total + 4096) break;
        }

        // Cleanly close the socket.
        (void)inv_->call("shutdown", {sock, 1u});
        (void)inv_->call("closesocket", {sock});

        return received;
    }

    GuestMemory mem_;
    cpu::CpuState cpu_;
    Ws2Manager mgr_;
    DllExports exp_;
    std::unique_ptr<ThunkInvoker> inv_;
    std::unique_ptr<HandleTable> handles_;
    std::unique_ptr<SyncManager> sync_;
};
