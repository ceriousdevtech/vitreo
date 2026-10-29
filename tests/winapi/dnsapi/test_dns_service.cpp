/// DNS_SERVICE_INSTANCE layout measured with cl against SDK 10.0.26100.0
/// windns.h: x64 size 72 (name 0, host 8, port 32), x86 size 40 (port 16).

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/networking/dnsapi/dnsapi.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

void set_arg(cpu::CpuState& cpu, GuestMemory& memory, bool x64, usize index,
             u64 value) {
    if (x64) {
        switch (index) {
            case 0: cpu.gpr.rcx = value; return;
            case 1: cpu.gpr.rdx = value; return;
            case 2: cpu.gpr.r8 = value; return;
            case 3: cpu.gpr.r9 = value; return;
            default: break;
        }
        const GuestAddr slot = cpu.gpr.rsp + 0x28 + (index - 4) * 8;
        ASSERT_TRUE(memory.write_u64(slot, value).is_ok());
        return;
    }
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4 + index * 4,
                                 static_cast<u32>(value)).is_ok());
}

ExportEntry& find_export(DllExports& exports, const char* name) {
    for (auto& entry : exports.entries) {
        if (entry.name == name) return entry;
    }
    ADD_FAILURE() << name;
    return exports.entries.front();
}

} // namespace

TEST(DnsServiceTest, ConstructCopiesNameHostAndPortOnBothBitnesses) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x20000000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    DnsapiManager mgr;
    auto exports = create_dnsapi_thunks(mgr);
    for (const bool x64 : {false, true}) {
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode();
        else cpu.init_protected_mode();
        cpu.set_sp(0x20004000);
        cpu.gpr.rsp = 0x20004000;
        Marshaler m(memory);
        m.attach_cpu(cpu);

        const GuestAddr name = 0x20001000;
        const GuestAddr host = 0x20001200;
        ASSERT_TRUE(m.write_wide_string(name, "Spotify").is_ok());
        ASSERT_TRUE(m.write_wide_string(host, "host.local").is_ok());
        const u64 args[10] = {name, host, 0, 0, 57621, 0, 0, 0, 0, 0};
        for (usize i = 0; i < 10; ++i) set_arg(cpu, memory, x64, i, args[i]);

        find_export(exports, "DnsServiceConstructInstance").handler(m);
        const GuestAddr inst = static_cast<GuestAddr>(cpu.gpr.rax);
        ASSERT_NE(inst, 0u);
        const u32 name_at = x64 ? 0u : 0u;
        const u32 host_at = x64 ? 8u : 4u;
        const u32 port_at = x64 ? 32u : 16u;
        const auto name_ptr = m.read_ptr(inst + name_at);
        const auto host_ptr = m.read_ptr(inst + host_at);
        const auto port = m.read_u16(inst + port_at);
        ASSERT_TRUE(name_ptr.is_ok());
        ASSERT_TRUE(host_ptr.is_ok());
        ASSERT_TRUE(port.is_ok());
        EXPECT_EQ(port.value(), 57621u);
        EXPECT_EQ(m.read_u16(static_cast<GuestAddr>(name_ptr.value())).value(),
                  static_cast<u16>('S'));
        EXPECT_EQ(m.read_u16(static_cast<GuestAddr>(host_ptr.value())).value(),
                  static_cast<u16>('h'));

        set_arg(cpu, memory, x64, 0, inst);
        find_export(exports, "DnsServiceFreeInstance").handler(m);
    }
}

TEST(DnsServiceTest, ConstructRejectsNullNameAndBrowseRejectsBadVersion) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x20000000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    DnsapiManager mgr;
    auto exports = create_dnsapi_thunks(mgr);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(0x20004000);
    cpu.gpr.rsp = 0x20004000;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    for (usize i = 0; i < 10; ++i) set_arg(cpu, memory, true, i, 0);
    find_export(exports, "DnsServiceConstructInstance").handler(m);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    const GuestAddr request = 0x20002000;
    ASSERT_TRUE(memory.write_u32(request, 9).is_ok());
    set_arg(cpu, memory, true, 0, request);
    set_arg(cpu, memory, true, 1, 0);
    find_export(exports, "DnsServiceBrowse").handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 87u);
}

TEST(DnsServiceTest, ResolveLocalhostReturnsPendingAndCallsBack) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x20000000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    DnsapiManager mgr;
    u32 cb_status = 0xFFFFFFFFu;
    GuestAddr cb_instance = 0;
    u64 cb_context = 0;
    mgr.set_guest_call([&](GuestAddr fn, const u64* args, usize count, bool is64) -> u64 {
        EXPECT_EQ(fn, 0x1111u);
        EXPECT_EQ(count, 3u);
        EXPECT_TRUE(is64);
        cb_status = static_cast<u32>(args[0]);
        cb_context = args[1];
        cb_instance = static_cast<GuestAddr>(args[2]);
        return 0;
    });
    auto exports = create_dnsapi_thunks(mgr);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(0x20004000);
    cpu.gpr.rsp = 0x20004000;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    const GuestAddr query = 0x20001000;
    const GuestAddr request = 0x20002000;
    const GuestAddr cancel = 0x20002200;
    ASSERT_TRUE(m.write_wide_string(query, "localhost").is_ok());
    ASSERT_TRUE(memory.write_u32(request, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(request + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(request + 8, query).is_ok());
    ASSERT_TRUE(memory.write_u64(request + 16, 0x1111).is_ok());
    ASSERT_TRUE(memory.write_u64(request + 24, 0x42).is_ok());
    set_arg(cpu, memory, true, 0, request);
    set_arg(cpu, memory, true, 1, cancel);
    find_export(exports, "DnsServiceResolve").handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 9506u);
    EXPECT_EQ(cb_status, 0u);
    EXPECT_EQ(cb_context, 0x42u);
    EXPECT_NE(cb_instance, 0u);
    const auto host = m.read_ptr(cb_instance + 8);
    ASSERT_TRUE(host.is_ok());
    EXPECT_EQ(m.read_u16(static_cast<GuestAddr>(host.value())).value(),
              static_cast<u16>('l'));
    set_arg(cpu, memory, true, 0, cancel);
    find_export(exports, "DnsServiceResolveCancel").handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

TEST(DnsServiceTest, BrowseUnknownTypeReturnsPendingAndCancelSucceeds) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x20000000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    DnsapiManager mgr;
    u32 cb_status = 0xFFFFFFFFu;
    mgr.set_guest_call([&](GuestAddr, const u64* args, usize count, bool) -> u64 {
        EXPECT_EQ(count, 3u);
        cb_status = static_cast<u32>(args[0]);
        return 0;
    });
    auto exports = create_dnsapi_thunks(mgr);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(0x20004000);
    cpu.gpr.rsp = 0x20004000;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    const GuestAddr query = 0x20001000;
    const GuestAddr request = 0x20002000;
    const GuestAddr cancel = 0x20002200;
    ASSERT_TRUE(m.write_wide_string(query, "_vitreo-missing._tcp.local").is_ok());
    ASSERT_TRUE(memory.write_u32(request, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(request + 8, query).is_ok());
    ASSERT_TRUE(memory.write_u64(request + 16, 0x2222).is_ok());
    ASSERT_TRUE(memory.write_u64(request + 24, 0x7).is_ok());
    set_arg(cpu, memory, true, 0, request);
    set_arg(cpu, memory, true, 1, cancel);
    find_export(exports, "DnsServiceBrowse").handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 9506u);
    EXPECT_EQ(cb_status, 9501u);
    set_arg(cpu, memory, true, 0, cancel);
    find_export(exports, "DnsServiceBrowseCancel").handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}
