#pragma once

/// @file ws2_32_contract_fixture.hpp
/// @brief Shared fixture for the ws2_32 contract tests pinned to the Windows probes
///        (tools/oracle_probe/ws2_core_probe*.expected, ws2_opts_probe.expected).
///
/// Every expected value in this file is copied from a probe's .expected output
/// or from the Learn page it cites -- never from running Vitreo. The invoker
/// drives the real export lambdas in both calling conventions: x86 stdcall
/// (args on the stack) and x64 (RCX/RDX/R8/R9, then [RSP+0x28+8*(i-4)]).
///
/// The fixture lives in a named namespace so every test_ws2_32_contracts*.cpp part
/// shares ONE Ws2ContractTest type (gtest requires a single fixture class per
/// parameterized suite). The suite is instantiated once, in
/// test_ws2_32_contracts.cpp.

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <winapi/networking/ws2_32/ws2_32.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/memory/heap.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <poll.h>
#include <string>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

namespace vitreo::winapi::ws2_contract_test {

inline constexpr GuestAddr kStackBase = 0x80000;
inline constexpr GuestAddr kStackSize = 0x10000;
inline constexpr GuestAddr kData = 0x200000;
inline constexpr GuestAddr kDataSize = 0x100000;

class Ws2ContractTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ASSERT_TRUE(mem_.map_region(kStackBase, kStackSize, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(mem_.map_region(kData, kDataSize, MemoryProtection::ReadWrite).is_ok());
        handles_ = std::make_unique<HandleTable>();
        sync_ = std::make_unique<SyncManager>(*handles_);
        mgr_.set_sync_manager(sync_.get());
        mgr_.set_memory(&mem_);
        vmm_ = std::make_unique<VirtualMemoryManager>(mem_);
        heap_ = std::make_unique<HeapManager>(*vmm_, mem_, *handles_);
        mgr_.set_heap_manager(heap_.get());
        if (is64()) cpu_.init_long_mode();
        else cpu_.init_protected_mode();
        cpu_.set_sp(kStackBase + kStackSize - 0x200);
        exp_ = create_ws2_thunks(mgr_);
    }

    void TearDown() override {
        mgr_.shutdown_all_sockets();
        sync_.reset();
        handles_.reset();
    }

    [[nodiscard]] bool is64() const { return GetParam(); }

    /// Invoke an export; returns the full RAX.
    u64 call(const std::string& name, std::initializer_list<u64> args) {
        const ExportEntry* entry = nullptr;
        for (const auto& e : exp_.entries) {
            if (e.name == name) { entry = &e; break; }
        }
        if (entry == nullptr) {
            ADD_FAILURE() << "missing export " << name;
            return 0;
        }
        const u64 saved_sp = cpu_.sp();
        const std::vector<u64> a(args);
        if (is64()) {
            const u64 frame = 0x28 + 8 * (a.size() > 4 ? a.size() - 4 : 0);
            const u64 sp = (saved_sp - frame - 0x40) & ~u64{0xF};
            cpu_.set_sp(sp);
            (void)mem_.write_u64(sp, 0xDEADBEEF);
            u64* regs[4] = {&cpu_.gpr.rcx, &cpu_.gpr.rdx, &cpu_.gpr.r8, &cpu_.gpr.r9};
            for (usize i = 0; i < a.size(); ++i) {
                if (i < 4) *regs[i] = a[i];
                else (void)mem_.write_u64(sp + 0x28 + 8 * (i - 4), a[i]);
            }
        } else {
            const u64 sp = saved_sp - 4 - 4 * a.size();
            cpu_.set_sp(sp);
            (void)mem_.write_u32(static_cast<GuestAddr>(sp), 0xDEADBEEF);
            for (usize i = 0; i < a.size(); ++i) {
                (void)mem_.write_u32(static_cast<GuestAddr>(sp + 4 + 4 * i),
                                     static_cast<u32>(a[i]));
            }
        }
        Marshaler m(mem_);
        m.attach_cpu(cpu_);
        cpu_.gpr.rax = 0x5A5A5A5A5A5A5A5Aull;
        entry->handler(m);
        cpu_.set_sp(saved_sp);
        return is64() ? cpu_.gpr.rax : (cpu_.gpr.rax & 0xFFFFFFFFull);
    }

    u32 wsa_error() { return static_cast<u32>(call("WSAGetLastError", {})); }

    void startup() {
        ASSERT_EQ(call("WSAStartup", {0x0202, kData + 0x8000}), 0u);
    }

    [[nodiscard]] u64 ptr(GuestAddr at) {
        return is64() ? mem_.read_u64(at).value() : mem_.read_u32(at).value();
    }
    [[nodiscard]] std::string str(GuestAddr at) {
        std::string out;
        for (GuestAddr p = at; out.size() < 256; ++p) {
            auto c = mem_.read_u8(p);
            if (c.is_err()) return "<unreadable>";
            if (c.value() == 0) break;
            out.push_back(static_cast<char>(c.value()));
        }
        return out;
    }
    GuestAddr put_str(GuestAddr at, const char* s) {
        (void)mem_.write_bytes(at, reinterpret_cast<const Byte*>(s), std::strlen(s) + 1);
        return at;
    }

    [[nodiscard]] u64 invalid_socket() const {
        return is64() ? ~u64{0} : u64{0xFFFFFFFF};
    }

    // Probe-helper equivalents (tools/oracle_probe/ws2_probe_util.h). Scratch
    // lives at kData + 0xF000 and above.
    static constexpr GuestAddr kScratch = kData + 0xF000;
    void loop4(GuestAddr sa, u16 port) {
        (void)mem_.write_bytes(sa, std::vector<Byte>(16, 0).data(), 16);
        (void)mem_.write_u16(sa, 2);
        (void)mem_.write_u16(sa + 2, htons(port));
        (void)mem_.write_u32(sa + 4, htonl(INADDR_LOOPBACK));
    }
    u16 port_of(u64 s) {
        (void)mem_.write_u32(kScratch + 0x40, 16);
        if (call("getsockname", {s, kScratch, kScratch + 0x40}) != 0) return 0;
        return ntohs(mem_.read_u16(kScratch + 2).value());
    }
    u64 bound(int type, int proto) {
        const u64 s = call("socket", {2, static_cast<u64>(type), static_cast<u64>(proto)});
        loop4(kScratch + 0x80, 0);
        (void)call("bind", {s, kScratch + 0x80, 16});
        return s;
    }
    u64 tcp_listener() {
        const u64 l = bound(1, 6);
        (void)call("listen", {l, 16});
        return l;
    }
    void tcp_pair(u64* c, u64* a) {
        const u64 l = tcp_listener();
        loop4(kScratch + 0x100, port_of(l));
        *c = call("socket", {2, 1, 6});
        ASSERT_EQ(call("connect", {*c, kScratch + 0x100, 16}), 0u);
        *a = call("accept", {l, 0, 0});
        ASSERT_NE(*a, invalid_socket());
        (void)call("closesocket", {l});
    }
    void set_nb(u64 s, u32 on) {
        (void)mem_.write_u32(kScratch + 0x140, on);
        ASSERT_EQ(call("ioctlsocket", {s, 0x8004667E, kScratch + 0x140}), 0u);
    }
    u32 rc32(u64 v) { return static_cast<u32>(v); }

    GuestMemory mem_;
    cpu::CpuState cpu_;
    std::unique_ptr<HandleTable> handles_;
    std::unique_ptr<SyncManager> sync_;
    std::unique_ptr<VirtualMemoryManager> vmm_;
    std::unique_ptr<HeapManager> heap_;
    Ws2Manager mgr_;
    DllExports exp_;
};

}  // namespace vitreo::winapi::ws2_contract_test
