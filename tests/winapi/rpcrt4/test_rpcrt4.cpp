/// @file test_rpcrt4.cpp
/// @brief Tests for rpcrt4.dll UUID APIs (32-bit stdcall).
///
/// These exports previously resolved to the generic missing-export stub, which
/// returns 0xFFFFFFFF — a *non-zero* RPC_STATUS, i.e. an error — without
/// writing any out-parameter, and pops 0 stdcall bytes. Battle.net-Setup.exe
/// builds its session hash from UuidCreate + UuidToStringA and its bootstrapper
/// thread died silently when both "failed".

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/com/rpcrt4/rpcrt4.hpp>
#include <winapi/kernel32/memory/global_local.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/module/module.hpp>
#include <winapi/com/ole32/task_memory.hpp>
#include <capsule/registry/registry.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <set>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

ExportEntry* find_export(DllExports& exports, const char* name) {
    for (auto& e : exports.entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

} // namespace

class Rpcrt4Test : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA_BASE, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
        exports = create_rpcrt4_thunks(gl_mgr, task_memory, ndr_proxies);
    }

    void push_arg(usize index, u32 value) {
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4 + index * 4), value);
    }

    void call(const char* name) {
        auto* e = find_export(exports, name);
        ASSERT_NE(e, nullptr) << "missing export: " << name;
        e->handler(marshaler);
    }

    u32 ret() const { return static_cast<u32>(cpu.gpr.rax); }

    /// Read a NUL-terminated ANSI string out of guest memory.
    std::string read_ansi(GuestAddr addr) {
        std::string s;
        for (u32 i = 0; i < 256; ++i) {
            auto c = memory.read_u8(addr + i);
            if (!c.is_ok() || c.value() == 0) break;
            s.push_back(static_cast<char>(c.value()));
        }
        return s;
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE  = 0x1000;

    GuestMemory          memory;
    cpu::CpuState        cpu;
    Marshaler            marshaler{memory};
    VirtualMemoryManager vmm{memory};
    HandleTable          handles;
    HeapManager          heap_mgr{vmm, memory, handles};
    GlobalLocalManager   gl_mgr{heap_mgr};
    ComTaskMemoryManager task_memory{heap_mgr};
    capsule::Registry    reg;
    ThunkRegistry        thunks{0x70000000};
    ModuleManager        modules{thunks};
    NdrProxyRegistrar    ndr_proxies{reg, modules, thunks};
    DllExports           exports;
};

// -----------------------------------------------------------------------------
// Arity — the stub's stdcall_bytes=0 silently leaked the caller's arguments.
// -----------------------------------------------------------------------------

TEST_F(Rpcrt4Test, ExportsDeclareCorrectStdcallArity) {
    struct { const char* name; u32 bytes; } expected[] = {
        {"UuidCreate", 4}, {"UuidCreateSequential", 4},
        {"UuidToStringA", 8}, {"UuidToStringW", 8},
        {"UuidFromStringA", 8}, {"UuidFromStringW", 8},
        {"RpcStringFreeA", 4}, {"RpcStringFreeW", 4},
        {"NdrDllRegisterProxy", 12},
        {"NdrDllUnregisterProxy", 12},
        {"NdrDllGetClassObject", 24},
        {"NdrDllCanUnloadNow", 4},
        {"NdrCStdStubBuffer_Release", 8},
        {"NdrCStdStubBuffer2_Release", 8},
        {"NdrOleAllocate", 4},
        {"NdrOleFree", 4},
        {"CStdStubBuffer_QueryInterface", 12},
        {"CStdStubBuffer_AddRef", 4},
        {"CStdStubBuffer_Release", 4},
        {"CStdStubBuffer_DebugServerRelease", 8},
        {"IUnknown_QueryInterface_Proxy", 12},
        {"IUnknown_AddRef_Proxy", 4},
        {"IUnknown_Release_Proxy", 4},
    };
    for (const auto& x : expected) {
        auto* e = find_export(exports, x.name);
        ASSERT_NE(e, nullptr) << "missing export: " << x.name;
        EXPECT_EQ(static_cast<u32>(e->stdcall_bytes), x.bytes) << x.name;
    }
}

TEST_F(Rpcrt4Test, NoDuplicateRegistrations) {
    std::set<std::string> seen;
    for (const auto& e : exports.entries) {
        EXPECT_TRUE(seen.insert(e.name).second)
            << "duplicate export shadows an earlier one: " << e.name;
    }
}

// -----------------------------------------------------------------------------
// UuidCreate
// -----------------------------------------------------------------------------

TEST_F(Rpcrt4Test, UuidCreateReturnsOkAndWritesSixteenBytes) {
    // Poison the buffer so "wrote nothing" cannot masquerade as success.
    for (u32 i = 0; i < kUuidSize; ++i) {
        (void)memory.write_u8(DATA_BASE + i, 0xCD);
    }
    push_arg(0, DATA_BASE);
    call("UuidCreate");
    EXPECT_EQ(ret(), rpc_status::RPC_S_OK);

    bool any_written = false;
    for (u32 i = 0; i < kUuidSize; ++i) {
        if (memory.read_u8(DATA_BASE + i).value() != 0xCD) any_written = true;
    }
    EXPECT_TRUE(any_written) << "UuidCreate left the output buffer untouched";
}

TEST_F(Rpcrt4Test, UuidCreateSetsVersion4AndRfc4122Variant) {
    push_arg(0, DATA_BASE);
    call("UuidCreate");
    // The version nibble lives in octet 7, not octet 6. RFC 4122 numbers the
    // octets of the big-endian *wire* form, but UuidCreate fills a UUID
    // struct whose Data1/Data2/Data3 are little-endian -- so wire octet 6
    // (Data3's high byte) is struct octet 7. Confirmed on real Windows:
    // UuidCreate produced ...bb 4c..., i.e. octet6=0xbb, octet7=0x4c.
    const u8 octet7 = memory.read_u8(DATA_BASE + 7).value();
    const u8 octet8 = memory.read_u8(DATA_BASE + 8).value();
    EXPECT_EQ(octet7 & 0xF0, 0x40) << "version nibble must be 4";
    // Data4 is a plain byte array with no swap, so octet 8 is octet 8.
    EXPECT_EQ(octet8 & 0xC0, 0x80) << "variant bits must be RFC 4122";
}

TEST_F(Rpcrt4Test, UuidCreateProducesDistinctValues) {
    push_arg(0, DATA_BASE);
    call("UuidCreate");
    std::string first;
    for (u32 i = 0; i < kUuidSize; ++i) {
        first.push_back(static_cast<char>(memory.read_u8(DATA_BASE + i).value()));
    }
    push_arg(0, DATA_BASE);
    call("UuidCreate");
    std::string second;
    for (u32 i = 0; i < kUuidSize; ++i) {
        second.push_back(static_cast<char>(memory.read_u8(DATA_BASE + i).value()));
    }
    // Callers use these as correlation keys; repeats would collide sessions.
    EXPECT_NE(first, second);
}

TEST_F(Rpcrt4Test, UuidCreateDrawsEveryRandomBitFreshFromTheHostCsprng) {
    // iphlp_misc_probe uuid-create: 64 draws are all version 4, RFC 4122
    // variant and pairwise distinct. Over 512 draws every one of the 122
    // random bit positions must take both values: a per-process engine with a
    // 32-bit seed passes that too, so this is a floor, and the rest of the
    // contract (independent streams across processes) is the arc4random_buf
    // source in generate_uuid_v4.
    std::set<std::string> seen;
    std::array<u32, kUuidSize * 8> ones{};
    constexpr u32 kDraws = 512;
    for (u32 n = 0; n < kDraws; ++n) {
        push_arg(0, DATA_BASE);
        call("UuidCreate");
        ASSERT_EQ(ret(), rpc_status::RPC_S_OK);
        std::string bytes;
        for (u32 i = 0; i < kUuidSize; ++i) bytes.push_back(static_cast<char>(memory.read_u8(DATA_BASE + i).value()));
        EXPECT_EQ(static_cast<u8>(bytes[7]) & 0xF0, 0x40);
        EXPECT_EQ(static_cast<u8>(bytes[8]) & 0xC0, 0x80);
        EXPECT_TRUE(seen.insert(bytes).second) << "repeated UUID";
        for (u32 bit = 0; bit < kUuidSize * 8; ++bit)
            if ((static_cast<u8>(bytes[bit / 8]) >> (bit % 8)) & 1u) ++ones[bit];
    }
    for (u32 bit = 0; bit < kUuidSize * 8; ++bit) {
        const bool fixed = (bit / 8 == 7 && bit % 8 >= 4) || (bit / 8 == 8 && bit % 8 >= 6);
        if (fixed) continue;
        EXPECT_GT(ones[bit], 0u) << "bit " << bit;
        EXPECT_LT(ones[bit], kDraws) << "bit " << bit;
    }
}

TEST_F(Rpcrt4Test, UuidCreateRejectsNullPointer) {
    push_arg(0, 0);
    call("UuidCreate");
    EXPECT_EQ(ret(), rpc_status::RPC_S_INVALID_ARG);
}

// -----------------------------------------------------------------------------
// UuidToStringA
// -----------------------------------------------------------------------------

TEST_F(Rpcrt4Test, UuidToStringAFormatsCanonicalLowercase) {
    // {01234567-89AB-CDEF-0123-456789ABCDEF} in guest (struct) byte order:
    // Data1/Data2/Data3 are little-endian integers, Data4 is a byte array.
    const u8 uuid[kUuidSize] = {
        0x67, 0x45, 0x23, 0x01,  // Data1 = 0x01234567
        0xAB, 0x89,              // Data2 = 0x89AB
        0xEF, 0xCD,              // Data3 = 0xCDEF
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    };
    for (u32 i = 0; i < kUuidSize; ++i) {
        (void)memory.write_u8(DATA_BASE + i, uuid[i]);
    }
    const GuestAddr out = DATA_BASE + 0x100;
    push_arg(0, DATA_BASE);
    push_arg(1, out);
    call("UuidToStringA");

    EXPECT_EQ(ret(), rpc_status::RPC_S_OK);
    const u32 str_addr = memory.read_u32(out).value();
    ASSERT_NE(str_addr, 0u) << "UuidToStringA did not write the out pointer";
    const std::string s = read_ansi(str_addr);
    EXPECT_EQ(s, "01234567-89ab-cdef-0123-456789abcdef");
    EXPECT_EQ(s.size(), kUuidStringLen);
}

TEST_F(Rpcrt4Test, UuidToStringARejectsNullArguments) {
    push_arg(0, 0);
    push_arg(1, DATA_BASE + 0x100);
    call("UuidToStringA");
    EXPECT_EQ(ret(), rpc_status::RPC_S_INVALID_ARG);

    push_arg(0, DATA_BASE);
    push_arg(1, 0);
    call("UuidToStringA");
    EXPECT_EQ(ret(), rpc_status::RPC_S_INVALID_ARG);
}

TEST_F(Rpcrt4Test, UuidToStringWEmitsWideCharacters) {
    for (u32 i = 0; i < kUuidSize; ++i) {
        (void)memory.write_u8(DATA_BASE + i, 0);
    }
    const GuestAddr out = DATA_BASE + 0x100;
    push_arg(0, DATA_BASE);
    push_arg(1, out);
    call("UuidToStringW");
    EXPECT_EQ(ret(), rpc_status::RPC_S_OK);

    const u32 str_addr = memory.read_u32(out).value();
    ASSERT_NE(str_addr, 0u);
    // An all-zero UUID renders as all zeros and hyphens.
    EXPECT_EQ(memory.read_u16(str_addr).value(), u16{'0'});
    EXPECT_EQ(memory.read_u16(str_addr + 8 * 2).value(), u16{'-'});
    EXPECT_EQ(memory.read_u16(str_addr + kUuidStringLen * 2).value(), u16{0});
}

// -----------------------------------------------------------------------------
// UuidFromStringA — inverse of UuidToStringA (ISSetup GetProcAddress probe)
// -----------------------------------------------------------------------------

TEST_F(Rpcrt4Test, UuidFromStringAParsesCanonicalAndBraced) {
    const char* s = "01234567-89ab-cdef-0123-456789abcdef";
    for (u32 i = 0; s[i]; ++i) {
        (void)memory.write_u8(DATA_BASE + 0x200 + i, static_cast<u8>(s[i]));
    }
    (void)memory.write_u8(DATA_BASE + 0x200 + 36, 0);
    push_arg(0, DATA_BASE + 0x200);
    push_arg(1, DATA_BASE);
    call("UuidFromStringA");
    EXPECT_EQ(ret(), rpc_status::RPC_S_OK);
    EXPECT_EQ(memory.read_u8(DATA_BASE + 0).value(), 0x67);
    EXPECT_EQ(memory.read_u8(DATA_BASE + 3).value(), 0x01);
    EXPECT_EQ(memory.read_u8(DATA_BASE + 15).value(), 0xEF);

    const char* braced = "{01234567-89AB-CDEF-0123-456789ABCDEF}";
    for (u32 i = 0; braced[i]; ++i) {
        (void)memory.write_u8(DATA_BASE + 0x300 + i, static_cast<u8>(braced[i]));
    }
    (void)memory.write_u8(DATA_BASE + 0x300 + 38, 0);
    push_arg(0, DATA_BASE + 0x300);
    push_arg(1, DATA_BASE + 0x40);
    call("UuidFromStringA");
    EXPECT_EQ(ret(), rpc_status::RPC_S_OK);
    EXPECT_EQ(memory.read_u8(DATA_BASE + 0x40).value(), 0x67);
}

TEST_F(Rpcrt4Test, UuidFromStringANullStringWritesGuidNull) {
    for (u32 i = 0; i < kUuidSize; ++i) {
        (void)memory.write_u8(DATA_BASE + i, 0xCD);
    }
    push_arg(0, 0);
    push_arg(1, DATA_BASE);
    call("UuidFromStringA");
    EXPECT_EQ(ret(), rpc_status::RPC_S_OK);
    for (u32 i = 0; i < kUuidSize; ++i) {
        EXPECT_EQ(memory.read_u8(DATA_BASE + i).value(), 0) << i;
    }
}

TEST_F(Rpcrt4Test, UuidFromStringARejectsMalformed) {
    const char* bad = "not-a-uuid";
    for (u32 i = 0; bad[i]; ++i) {
        (void)memory.write_u8(DATA_BASE + 0x200 + i, static_cast<u8>(bad[i]));
    }
    (void)memory.write_u8(DATA_BASE + 0x200 + 10, 0);
    push_arg(0, DATA_BASE + 0x200);
    push_arg(1, DATA_BASE);
    call("UuidFromStringA");
    EXPECT_EQ(ret(), rpc_status::RPC_S_INVALID_STRING_UUID);
}

TEST_F(Rpcrt4Test, UuidToFromStringRoundTrip) {
    const u8 uuid[kUuidSize] = {
        0x67, 0x45, 0x23, 0x01, 0xAB, 0x89, 0xEF, 0xCD,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    };
    for (u32 i = 0; i < kUuidSize; ++i) {
        (void)memory.write_u8(DATA_BASE + i, uuid[i]);
    }
    const GuestAddr out = DATA_BASE + 0x100;
    push_arg(0, DATA_BASE);
    push_arg(1, out);
    call("UuidToStringA");
    ASSERT_EQ(ret(), rpc_status::RPC_S_OK);
    const u32 str_addr = memory.read_u32(out).value();

    for (u32 i = 0; i < kUuidSize; ++i) {
        (void)memory.write_u8(DATA_BASE + 0x40 + i, 0xCD);
    }
    push_arg(0, str_addr);
    push_arg(1, DATA_BASE + 0x40);
    call("UuidFromStringA");
    EXPECT_EQ(ret(), rpc_status::RPC_S_OK);
    for (u32 i = 0; i < kUuidSize; ++i) {
        EXPECT_EQ(memory.read_u8(DATA_BASE + 0x40 + i).value(), uuid[i]) << i;
    }
}

// -----------------------------------------------------------------------------
// RpcStringFree — must NULL the caller's pointer (the guest re-checks it)
// -----------------------------------------------------------------------------

TEST_F(Rpcrt4Test, RpcStringFreeANullsCallersPointer) {
    const GuestAddr out = DATA_BASE + 0x100;
    push_arg(0, DATA_BASE);
    push_arg(1, out);
    call("UuidToStringA");
    ASSERT_NE(memory.read_u32(out).value(), 0u);

    push_arg(0, out);
    call("RpcStringFreeA");
    EXPECT_EQ(ret(), rpc_status::RPC_S_OK);
    EXPECT_EQ(memory.read_u32(out).value(), 0u);
}

TEST_F(Rpcrt4Test, RpcStringFreeAToleratesAlreadyFreedPointer) {
    const GuestAddr out = DATA_BASE + 0x100;
    (void)memory.write_u32(out, 0);
    push_arg(0, out);
    call("RpcStringFreeA");
    // Windows reports success for a pointer that is already NULL.
    EXPECT_EQ(ret(), rpc_status::RPC_S_OK);
}

TEST_F(Rpcrt4Test, RpcStringFreeARejectsNullHandle) {
    push_arg(0, 0);
    call("RpcStringFreeA");
    EXPECT_EQ(ret(), rpc_status::RPC_S_INVALID_ARG);
}

// -----------------------------------------------------------------------------
// Round trip — the sequence Battle.net-Setup.exe actually performs.
// -----------------------------------------------------------------------------

TEST_F(Rpcrt4Test, CreateFormatFreeRoundTrip) {
    const GuestAddr out = DATA_BASE + 0x100;
    push_arg(0, DATA_BASE);
    call("UuidCreate");
    ASSERT_EQ(ret(), rpc_status::RPC_S_OK);

    push_arg(0, DATA_BASE);
    push_arg(1, out);
    call("UuidToStringA");
    ASSERT_EQ(ret(), rpc_status::RPC_S_OK);

    const u32 str_addr = memory.read_u32(out).value();
    ASSERT_NE(str_addr, 0u);
    const std::string s = read_ansi(str_addr);
    EXPECT_EQ(s.size(), kUuidStringLen);
    EXPECT_EQ(s[8], '-');
    EXPECT_EQ(s[13], '-');
    EXPECT_EQ(s[18], '-');
    EXPECT_EQ(s[23], '-');
    for (char c : s) {
        EXPECT_TRUE(c == '-' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "non-canonical character in UUID string: " << s;
    }

    push_arg(0, out);
    call("RpcStringFreeA");
    EXPECT_EQ(ret(), rpc_status::RPC_S_OK);
}

// -----------------------------------------------------------------------------
// NdrDllGetClassObject — IPSFactoryBuffer from the guest CStdPSFactoryBuffer
// -----------------------------------------------------------------------------

TEST_F(Rpcrt4Test, NdrDllGetClassObjectReturnsIPSFactoryBuffer) {
    ASSERT_TRUE(thunks.register_dll("rpcrt4.dll",
                                    create_rpcrt4_thunks(gl_mgr, task_memory,
                                                         ndr_proxies))
                    .is_ok());

    constexpr GuestAddr kClsid = DATA_BASE + 0x100;
    constexpr GuestAddr kIid = DATA_BASE + 0x110;
    constexpr GuestAddr kPpv = DATA_BASE + 0x120;
    constexpr GuestAddr kProxyList = DATA_BASE + 0x130;
    constexpr GuestAddr kFactory = DATA_BASE + 0x200;

    // Minimal GUID bytes for a proxy CLSID (any unique 16 bytes).
    std::array<u8, 16> clsid{};
    clsid[0] = 0x11;
    clsid[15] = 0x22;
    ASSERT_TRUE(memory.write_bytes(kClsid, clsid.data(), 16).is_ok());

    // IID_IPSFactoryBuffer {D5F569D0-593B-101A-B569-08002B2DBF7A}
    const std::array<u8, 16> iid_psf = {
        0xD0, 0x69, 0xF5, 0xD5, 0x3B, 0x59, 0x1A, 0x10,
        0xB5, 0x69, 0x08, 0x00, 0x2B, 0x2D, 0xBF, 0x7A};
    ASSERT_TRUE(memory.write_bytes(kIid, iid_psf.data(), 16).is_ok());
    ASSERT_TRUE(memory.write_u32(kPpv, 0).is_ok());
    // Non-null proxy file list pointer (content unused when pclsid matches).
    ASSERT_TRUE(memory.write_u32(kProxyList, 0).is_ok());

    push_arg(0, static_cast<u32>(kClsid));
    push_arg(1, static_cast<u32>(kIid));
    push_arg(2, static_cast<u32>(kPpv));
    push_arg(3, static_cast<u32>(kProxyList));
    push_arg(4, static_cast<u32>(kClsid)); // pclsid matches rclsid
    push_arg(5, static_cast<u32>(kFactory));
    call("NdrDllGetClassObject");
    EXPECT_EQ(ret(), 0u) << "S_OK";

    const u32 out = memory.read_u32(kPpv).value_or(0);
    EXPECT_EQ(out, static_cast<u32>(kFactory));
    const u32 vtbl = memory.read_u32(kFactory).value_or(0);
    EXPECT_NE(vtbl, 0u);
    const u32 refs = memory.read_u32(kFactory + 4).value_or(0);
    EXPECT_EQ(refs, 1u);
}

TEST_F(Rpcrt4Test, NdrDllGetClassObjectClsidMismatchFails) {
    ASSERT_TRUE(thunks.register_dll("rpcrt4.dll",
                                    create_rpcrt4_thunks(gl_mgr, task_memory,
                                                         ndr_proxies))
                    .is_ok());

    constexpr GuestAddr kClsid = DATA_BASE + 0x100;
    constexpr GuestAddr kOther = DATA_BASE + 0x140;
    constexpr GuestAddr kIid = DATA_BASE + 0x110;
    constexpr GuestAddr kPpv = DATA_BASE + 0x120;
    constexpr GuestAddr kProxyList = DATA_BASE + 0x130;
    constexpr GuestAddr kFactory = DATA_BASE + 0x200;

    std::array<u8, 16> clsid{};
    clsid[0] = 0x11;
    std::array<u8, 16> other{};
    other[0] = 0x99;
    ASSERT_TRUE(memory.write_bytes(kClsid, clsid.data(), 16).is_ok());
    ASSERT_TRUE(memory.write_bytes(kOther, other.data(), 16).is_ok());
    const std::array<u8, 16> iid_unk = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
    ASSERT_TRUE(memory.write_bytes(kIid, iid_unk.data(), 16).is_ok());
    ASSERT_TRUE(memory.write_u32(kPpv, 0xDEAD).is_ok());

    push_arg(0, static_cast<u32>(kClsid));
    push_arg(1, static_cast<u32>(kIid));
    push_arg(2, static_cast<u32>(kPpv));
    push_arg(3, static_cast<u32>(kProxyList));
    push_arg(4, static_cast<u32>(kOther));
    push_arg(5, static_cast<u32>(kFactory));
    call("NdrDllGetClassObject");
    EXPECT_EQ(ret(), 0x80040111u); // CLASS_E_CLASSNOTAVAILABLE
    EXPECT_EQ(memory.read_u32(kPpv).value_or(1), 0u);
}
