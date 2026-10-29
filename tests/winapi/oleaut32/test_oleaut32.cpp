// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_oleaut32.cpp
/// @brief Unit tests for OLEAUT32.dll API stubs

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/com/oleaut32/oleaut32.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <capsule/registry/registry.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Windows Type Definitions
// =============================================================================

// BSTR is a pointer to a wide character string with a 4-byte length prefix
// The length (in bytes) is stored at [BSTR - 4]
using BSTR = u32; // Guest pointer

// VARIANT type constants
enum VARENUM : u16 {
    VT_EMPTY    = 0,
    VT_NULL     = 1,
    VT_I2       = 2,
    VT_I4       = 3,
    VT_BSTR     = 8,
    VT_DISPATCH = 9,
    VT_BOOL     = 11,
    VT_VARIANT  = 12,
    VT_UNKNOWN  = 13,
    VT_UI4      = 19,
};

// VARIANT structure (16 bytes on x86, simplified)
#pragma pack(push, 1)
struct VARIANT {
    u16 vt;           // 0: VARENUM type
    u16 wReserved1;   // 2
    u16 wReserved2;   // 4
    u16 wReserved3;   // 6
    union {
        i32 lVal;     // VT_I4
        u32 ulVal;    // VT_UI4
        i16 iVal;     // VT_I2
        u32 bstrVal;  // VT_BSTR (guest pointer)
        u32 punkVal;  // VT_UNKNOWN (guest pointer)
        u64 ullVal;   // For 64-bit values
    };
};
#pragma pack(pop)

static_assert(sizeof(VARIANT) == 16, "VARIANT must be 16 bytes");

// SAFEARRAYBOUND structure
#pragma pack(push, 1)
struct SAFEARRAYBOUND {
    u32 cElements;  // Number of elements in dimension
    i32 lLbound;    // Lower bound
};
#pragma pack(pop)

// SAFEARRAY structure (simplified)
#pragma pack(push, 1)
struct SAFEARRAY {
    u16 cDims;      // Count of dimensions
    u16 fFeatures;  // Flags
    u32 cbElements; // Size of each element
    u32 cLocks;     // Lock count
    u32 pvData;     // Guest pointer to data
    SAFEARRAYBOUND rgsabound[1]; // Array of bounds (flexible array)
};
#pragma pack(pop)

// HRESULT constants
static constexpr u32 S_OK = 0x00000000;
static constexpr u32 E_NOTIMPL = 0x80004001;

// =============================================================================
// Test Fixture
// =============================================================================

class OleAut32Test : public ::testing::Test {
protected:
    void SetUp() override {
        // Map memory for guest operations
        auto res = memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());

        // Map stack region
        auto stack_res = memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());

        // Map OLEAUT32 scratch region for BSTR allocations
        auto scratch_res = memory.map_region(OLEAUT32_SCRATCH_BASE,
                                              OLEAUT32_SCRATCH_SIZE,
                                              MemoryProtection::ReadWrite);
        ASSERT_TRUE(scratch_res.is_ok());

        cpu.gpr.rsp = STACK_BASE + 0x800;
    }

    /// Helper: create a Marshaler with arg0=rcx, arg1=rdx, arg2=r8
    Marshaler make_marshaler(GuestAddr a0 = 0, GuestAddr a1 = 0, GuestAddr a2 = 0) {
        cpu.gpr.rcx = a0;
        cpu.gpr.rdx = a1;
        cpu.gpr.r8  = a2;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        return m;
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE = 0x1000;

    GuestMemory memory;
    cpu::CpuState cpu;
    OleAut32Manager mgr;
};

// =============================================================================
// SysAllocString Tests
// =============================================================================

// 1. SysAllocString returns valid BSTR pointer for non-NULL string
TEST_F(OleAut32Test, SysAllocString_NonNull_ReturnsBstr) {
    constexpr GuestAddr str_addr = DATA_BASE + 0x100;
    memory.write_u16(str_addr, u'A');
    memory.write_u16(str_addr + 2, u'B');
    memory.write_u16(str_addr + 4, 0); // null terminator

    auto m = make_marshaler(str_addr);
    GuestAddr bstr = mgr.sys_alloc_string(m);
    EXPECT_NE(bstr, 0u);

    // Verify BSTR memory layout: byte_length at bstr-4 should be 4 (2 chars * 2)
    u32 byte_len = memory.read_u32(bstr - 4).value();
    EXPECT_EQ(byte_len, 4u);

    // Verify string data
    EXPECT_EQ(memory.read_u16(bstr).value(), u'A');
    EXPECT_EQ(memory.read_u16(bstr + 2).value(), u'B');
    // Null terminator
    EXPECT_EQ(memory.read_u16(bstr + 4).value(), 0u);
}

// 2. SysAllocString returns 0 for NULL pointer
TEST_F(OleAut32Test, SysAllocString_NullPtr_ReturnsZero) {
    auto m = make_marshaler(0);
    GuestAddr bstr = mgr.sys_alloc_string(m);
    EXPECT_EQ(bstr, 0u);
}

// 3. SysAllocString returns distinct pointers
TEST_F(OleAut32Test, SysAllocString_ReturnsDistinctPointers) {
    constexpr GuestAddr str_addr = DATA_BASE + 0x100;
    memory.write_u16(str_addr, u'X');
    memory.write_u16(str_addr + 2, 0);

    auto m1 = make_marshaler(str_addr);
    GuestAddr bstr1 = mgr.sys_alloc_string(m1);
    auto m2 = make_marshaler(str_addr);
    GuestAddr bstr2 = mgr.sys_alloc_string(m2);
    auto m3 = make_marshaler(str_addr);
    GuestAddr bstr3 = mgr.sys_alloc_string(m3);

    EXPECT_NE(bstr1, 0u);
    EXPECT_NE(bstr2, 0u);
    EXPECT_NE(bstr3, 0u);
    EXPECT_NE(bstr1, bstr2);
    EXPECT_NE(bstr2, bstr3);
}

// =============================================================================
// SysAllocStringLen Tests
// =============================================================================

// 4. SysAllocStringLen allocates even with NULL source (zero-filled)
TEST_F(OleAut32Test, SysAllocStringLen_NullSource_StillAllocates) {
    auto m = make_marshaler();
    GuestAddr bstr = mgr.sys_alloc_string_len(m, 0, 10);
    EXPECT_NE(bstr, 0u);

    // byte_length should be 20 (10 chars * 2)
    u32 byte_len = memory.read_u32(bstr - 4).value();
    EXPECT_EQ(byte_len, 20u);

    // Data should be zero-filled
    EXPECT_EQ(memory.read_u16(bstr).value(), 0u);
}

// 5. SysAllocStringLen copies source data
TEST_F(OleAut32Test, SysAllocStringLen_ValidSource_CopiesData) {
    constexpr GuestAddr str_addr = DATA_BASE + 0x200;
    memory.write_u16(str_addr, u'H');
    memory.write_u16(str_addr + 2, u'i');

    auto m = make_marshaler();
    GuestAddr bstr = mgr.sys_alloc_string_len(m, str_addr, 2);
    EXPECT_NE(bstr, 0u);

    u32 byte_len = memory.read_u32(bstr - 4).value();
    EXPECT_EQ(byte_len, 4u);
    EXPECT_EQ(memory.read_u16(bstr).value(), u'H');
    EXPECT_EQ(memory.read_u16(bstr + 2).value(), u'i');
    EXPECT_EQ(memory.read_u16(bstr + 4).value(), 0u); // null terminator
}

// =============================================================================
// SysStringLen Tests
// =============================================================================

// 6. SysStringLen returns correct character count
TEST_F(OleAut32Test, SysStringLen_ReturnsCharCount) {
    constexpr GuestAddr str_addr = DATA_BASE + 0x100;
    memory.write_u16(str_addr, u'A');
    memory.write_u16(str_addr + 2, u'B');
    memory.write_u16(str_addr + 4, u'C');
    memory.write_u16(str_addr + 6, 0);

    auto m = make_marshaler(str_addr);
    GuestAddr bstr = mgr.sys_alloc_string(m);
    EXPECT_NE(bstr, 0u);

    auto m2 = make_marshaler(bstr);
    u32 len = mgr.sys_string_len(m2, bstr);
    EXPECT_EQ(len, 3u); // 3 characters
}

// 6b. SysStringByteLen returns correct byte count
TEST_F(OleAut32Test, SysStringByteLen_ReturnsByteCount) {
    constexpr GuestAddr str_addr = DATA_BASE + 0x100;
    memory.write_u16(str_addr, u'A');
    memory.write_u16(str_addr + 2, u'B');
    memory.write_u16(str_addr + 4, u'C');
    memory.write_u16(str_addr + 6, 0);

    auto m = make_marshaler(str_addr);
    GuestAddr bstr = mgr.sys_alloc_string(m);

    auto m2 = make_marshaler(bstr);
    u32 blen = mgr.sys_string_byte_len(m2, bstr);
    EXPECT_EQ(blen, 6u); // 3 chars * 2 bytes
}

// 6c. SysStringLen returns 0 for NULL BSTR
TEST_F(OleAut32Test, SysStringLen_Null_ReturnsZero) {
    auto m = make_marshaler(0);
    EXPECT_EQ(mgr.sys_string_len(m, 0), 0u);
    EXPECT_EQ(mgr.sys_string_byte_len(m, 0), 0u);
}

// =============================================================================
// SysFreeString Tests
// =============================================================================

// 7. SysFreeString doesn't crash on valid BSTR
TEST_F(OleAut32Test, SysFreeString_ValidBstr_DoesNotCrash) {
    constexpr GuestAddr str_addr = DATA_BASE + 0x100;
    memory.write_u16(str_addr, u'A');
    memory.write_u16(str_addr + 2, 0);

    auto m = make_marshaler(str_addr);
    GuestAddr bstr = mgr.sys_alloc_string(m);
    EXPECT_NE(bstr, 0u);

    mgr.sys_free_string(m, bstr);
}

// 8. SysFreeString doesn't crash on NULL
TEST_F(OleAut32Test, SysFreeString_Null_DoesNotCrash) {
    auto m = make_marshaler(0);
    mgr.sys_free_string(m, 0);
}

// =============================================================================
// SysReAllocStringLen Tests
// =============================================================================

// 9. SysReAllocStringLen returns TRUE and writes new BSTR pointer
TEST_F(OleAut32Test, SysReAllocStringLen_ReturnsTrue) {
    constexpr GuestAddr str_addr = DATA_BASE + 0x100;
    constexpr GuestAddr pbstr_addr = DATA_BASE + 0x200;

    memory.write_u16(str_addr, u'X');
    memory.write_u16(str_addr + 2, u'Y');
    memory.write_u16(str_addr + 4, 0);
    memory.write_u32(pbstr_addr, 0); // initially NULL

    // arg0=pbstr, arg1=psz, arg2=cch
    auto m = make_marshaler(pbstr_addr, str_addr, 2);
    u32 result = mgr.sys_realloc_string_len(m);
    EXPECT_EQ(result, 1u);

    // *pbstr should now point to a valid BSTR
    u32 new_bstr = memory.read_u32(pbstr_addr).value();
    EXPECT_NE(new_bstr, 0u);

    // Verify the new BSTR content
    u32 byte_len = memory.read_u32(new_bstr - 4).value();
    EXPECT_EQ(byte_len, 4u); // 2 chars * 2
    EXPECT_EQ(memory.read_u16(new_bstr).value(), u'X');
    EXPECT_EQ(memory.read_u16(new_bstr + 2).value(), u'Y');
}

// =============================================================================
// VariantInit Tests
// =============================================================================

// 10. VariantInit zeroes VARIANT structure
TEST_F(OleAut32Test, VariantInit_ZeroesStructure) {
    constexpr GuestAddr var_addr = DATA_BASE + 0x300;

    // Pre-fill with non-zero values (24-byte x64 VARIANT)
    for (size_t i = 0; i < 24; ++i) {
        memory.write_u8(var_addr + i, 0xAA);
    }

    // Set up marshaler with VARIANT pointer in RCX
    cpu.gpr.rcx = var_addr;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    mgr.variant_init(m);

    // Verify all 24 bytes are zeroed (x64 VARIANT; CpuState defaults to Long)
    uint8_t buf[24];
    ASSERT_TRUE(memory.read_bytes(var_addr, buf, 24).is_ok());

    for (size_t i = 0; i < 24; ++i) {
        EXPECT_EQ(buf[i], 0u) << "Byte " << i << " should be zero";
    }
}

// 11. VariantInit results in VT_EMPTY
TEST_F(OleAut32Test, VariantInit_SetsVtEmpty) {
    constexpr GuestAddr var_addr = DATA_BASE + 0x300;

    // Pre-fill with VT_BSTR
    memory.write_u16(var_addr, VT_BSTR);

    cpu.gpr.rcx = var_addr;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    mgr.variant_init(m);

    u16 vt = memory.read_u16(var_addr).value();
    EXPECT_EQ(vt, VT_EMPTY);
}

// =============================================================================
// VariantClear Tests
// =============================================================================

// 12. VariantClear returns S_OK
TEST_F(OleAut32Test, VariantClear_ReturnsSOK) {
    constexpr GuestAddr var_addr = DATA_BASE + 0x400;

    // Write a VT_I4 variant
    memory.write_u16(var_addr, VT_I4);
    memory.write_u32(var_addr + 8, 42);

    cpu.gpr.rcx = var_addr;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    u32 hr = mgr.variant_clear(m);
    EXPECT_EQ(hr, S_OK);
}

// 13. VariantClear handles VT_BSTR
TEST_F(OleAut32Test, VariantClear_HandlesBstr) {
    constexpr GuestAddr var_addr = DATA_BASE + 0x400;

    // Write a VT_BSTR variant
    memory.write_u16(var_addr, VT_BSTR);
    memory.write_u32(var_addr + 8, 0x12345678); // fake BSTR pointer

    cpu.gpr.rcx = var_addr;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    u32 hr = mgr.variant_clear(m);
    EXPECT_EQ(hr, S_OK);

    // After clear, type should be VT_EMPTY
    u16 vt = memory.read_u16(var_addr).value();
    EXPECT_EQ(vt, VT_EMPTY);
}

// 14. VariantClear handles VT_I4
TEST_F(OleAut32Test, VariantClear_HandlesI4) {
    constexpr GuestAddr var_addr = DATA_BASE + 0x400;

    memory.write_u16(var_addr, VT_I4);
    memory.write_u32(var_addr + 8, 999);

    cpu.gpr.rcx = var_addr;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    u32 hr = mgr.variant_clear(m);
    EXPECT_EQ(hr, S_OK);
}

// 15. VariantClear handles VT_EMPTY
TEST_F(OleAut32Test, VariantClear_HandlesEmpty) {
    constexpr GuestAddr var_addr = DATA_BASE + 0x400;

    // Write zeros (VT_EMPTY)
    for (size_t i = 0; i < 16; ++i) {
        memory.write_u8(var_addr + i, 0);
    }

    cpu.gpr.rcx = var_addr;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    u32 hr = mgr.variant_clear(m);
    EXPECT_EQ(hr, S_OK);
}

// =============================================================================
// VariantCopy Tests
// =============================================================================

// 16. VariantCopy copies 16-byte structure
TEST_F(OleAut32Test, VariantCopy_CopiesData) {
    constexpr GuestAddr dst_addr = DATA_BASE + 0x500;
    constexpr GuestAddr src_addr = DATA_BASE + 0x520;

    // Write source VARIANT: VT_I4, value = 0xDEADBEEF
    memory.write_u16(src_addr, VT_I4);
    memory.write_u16(src_addr + 2, 0);
    memory.write_u16(src_addr + 4, 0);
    memory.write_u16(src_addr + 6, 0);
    memory.write_u32(src_addr + 8, 0xDEADBEEF);
    memory.write_u32(src_addr + 12, 0);

    // Zero destination
    for (size_t i = 0; i < 16; ++i) {
        memory.write_u8(dst_addr + i, 0);
    }

    // arg(0) = dest, arg(1) = src
    cpu.gpr.rcx = dst_addr;
    cpu.gpr.rdx = src_addr;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    u32 hr = mgr.variant_copy(m);
    EXPECT_EQ(hr, S_OK);

    // Verify copy
    u16 vt = memory.read_u16(dst_addr).value();
    EXPECT_EQ(vt, VT_I4);

    u32 val = memory.read_u32(dst_addr + 8).value();
    EXPECT_EQ(val, 0xDEADBEEFu);
}

// 17. VariantCopy returns S_OK
TEST_F(OleAut32Test, VariantCopy_ReturnsSOK) {
    constexpr GuestAddr dst_addr = DATA_BASE + 0x500;
    constexpr GuestAddr src_addr = DATA_BASE + 0x520;

    memory.write_u16(src_addr, VT_EMPTY);

    cpu.gpr.rcx = dst_addr;
    cpu.gpr.rdx = src_addr;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    u32 hr = mgr.variant_copy(m);
    EXPECT_EQ(hr, S_OK);
}

// =============================================================================
// VariantChangeType Tests
// =============================================================================

// 18. VariantChangeType converts VT_I4 to VT_R8
TEST_F(OleAut32Test, VariantChangeType_I4toR8) {
    constexpr GuestAddr dst_addr = DATA_BASE + 0x600;
    constexpr GuestAddr src_addr = DATA_BASE + 0x620;

    // Write source: VT_I4, value = 42
    (void)memory.write_u16(src_addr, VT_I4);
    (void)memory.write_u16(src_addr + 2, 0);
    (void)memory.write_u16(src_addr + 4, 0);
    (void)memory.write_u16(src_addr + 6, 0);
    (void)memory.write_u32(src_addr + 8, 42);
    (void)memory.write_u32(src_addr + 12, 0);

    // arg(0)=dest, arg(1)=src, arg(2)=wFlags=0, arg(3)=targetVT=VT_R8(5)
    cpu.gpr.rcx = dst_addr;
    cpu.gpr.rdx = src_addr;
    cpu.gpr.r8  = 0;
    cpu.gpr.r9  = 5; // VT_R8
    Marshaler m(memory);
    m.attach_cpu(cpu);

    u32 hr = mgr.variant_change_type(m);
    EXPECT_EQ(hr, S_OK);

    u16 vt = memory.read_u16(dst_addr).value();
    EXPECT_EQ(vt, 5); // VT_R8
}

// 18b. VariantChangeType returns DISP_E_TYPEMISMATCH for unsupported conversion
TEST_F(OleAut32Test, VariantChangeType_Unsupported) {
    constexpr GuestAddr dst_addr = DATA_BASE + 0x600;
    constexpr GuestAddr src_addr = DATA_BASE + 0x620;

    // Write source: VT_EMPTY
    (void)memory.write_u16(src_addr, VT_EMPTY);
    for (u32 i = 2; i < 16; i++)
        (void)memory.write_u8(src_addr + i, 0);

    // target VT_DISPATCH (9) — unsupported
    cpu.gpr.rcx = dst_addr;
    cpu.gpr.rdx = src_addr;
    cpu.gpr.r8  = 0;
    cpu.gpr.r9  = VT_DISPATCH;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    u32 hr = mgr.variant_change_type(m);
    EXPECT_EQ(hr, 0x80020005u); // DISP_E_TYPEMISMATCH
}

// =============================================================================
// SafeArrayCreate Tests
// =============================================================================

// 19. SafeArrayCreate allocates a 1-D array
TEST_F(OleAut32Test, SafeArrayCreate_OneDim) {
    constexpr GuestAddr bound_addr = DATA_BASE + 0x700;

    // Write SAFEARRAYBOUND: cElements=10, lLbound=0
    (void)memory.write_u32(bound_addr, 10);
    (void)memory.write_u32(bound_addr + 4, 0);

    // arg(0)=VT_I4(3), arg(1)=cDims=1, arg(2)=rgsabound
    auto m = make_marshaler(3, 1, bound_addr);

    u32 psa = mgr.safe_array_create(m);
    EXPECT_NE(psa, 0u);

    // Read back the SAFEARRAY header
    u16 cDims = memory.read_u16(psa).value();
    EXPECT_EQ(cDims, 1);
    u32 cbElements = memory.read_u32(psa + 4).value();
    EXPECT_EQ(cbElements, 4u); // VT_I4 = 4 bytes
}

// =============================================================================
// SafeArrayDestroy Tests
// =============================================================================

// 20. SafeArrayDestroy(NULL) is S_OK on Windows
// (tools/oracle_probe/oleaut32_safearray_probe.expected: destroy_null hr=0).
TEST_F(OleAut32Test, SafeArrayDestroy_NullReturnsSOk) {
    auto m = make_marshaler(0 /*psa=NULL*/);
    u32 hr = mgr.safe_array_destroy(m);
    EXPECT_EQ(hr, 0u);
}

// 21. SafeArrayDestroy with valid psa returns S_OK
TEST_F(OleAut32Test, SafeArrayDestroy_ValidPsa_ReturnsSOK) {
    // Write a minimal SAFEARRAY at DATA_BASE (x64: pvData at +16)
    GuestAddr psa = DATA_BASE;
    (void)memory.write_u16(psa + 0,  1); // cDims
    (void)memory.write_u16(psa + 2,  0); // fFeatures
    (void)memory.write_u32(psa + 4,  4); // cbElements
    (void)memory.write_u32(psa + 8,  0); // cLocks
    (void)memory.write_u32(psa + 12, 0); // padding
    (void)memory.write_u64(psa + 16, 0); // pvData = null
    auto m = make_marshaler(psa);
    u32 hr = mgr.safe_array_destroy(m);
    EXPECT_EQ(hr, static_cast<u32>(S_OK));
}

// =============================================================================
// OleCreateFontIndirect Tests
// =============================================================================

// 22. OleCreateFontIndirect returns E_NOTIMPL
TEST_F(OleAut32Test, OleCreateFontIndirect_ReturnsNotImpl) {
    u32 hr = mgr.ole_create_font_indirect();
    EXPECT_EQ(hr, E_NOTIMPL);
}

// =============================================================================
// RegisterTypeLib Tests
//
// RegisterTypeLib only has attributes to publish for a library that came out
// of Vitreo's LoadTypeLib. Anything else has to be refused: reporting success
// would leave the caller believing its interfaces are now resolvable.
// =============================================================================

// 23. RegisterTypeLib rejects an ITypeLib Vitreo did not hand out
TEST_F(OleAut32Test, RegisterTypeLib_UnknownTypeLibIsRejected) {
    constexpr u32 E_INVALIDARG = 0x80070057;

    auto m = make_marshaler(0, 0, 0);
    EXPECT_EQ(mgr.register_type_lib(m), E_INVALIDARG); // NULL ITypeLib

    auto m2 = make_marshaler(DATA_BASE, 0, 0);
    EXPECT_EQ(mgr.register_type_lib(m2), E_INVALIDARG); // foreign pointer
}

// =============================================================================
// LoadRegTypeLib Tests
// =============================================================================

// LoadRegTypeLib with no TypeLib registration must refuse with the documented
// HRESULT — never invent a library. InstallShield's -5009 / 0x8002801d came
// from the previous always-fail stub even after RegisterTypeLib had written
// HKCR\TypeLib; these tests pin both halves.
TEST_F(OleAut32Test, LoadRegTypeLib_UnregisteredReturnsLibNotRegistered) {
    constexpr u32 TYPE_E_LIBNOTREGISTERED = 0x8002801D;
    capsule::Registry registry;
    mgr.set_registry(&registry);

    // {94636247-BC39-4B8B-A728-2D1FBEBFA76A}
    const std::array<u8, 16> libid{
        0x47, 0x62, 0x63, 0x94, 0x39, 0xBC, 0x8B, 0x4B,
        0xA7, 0x28, 0x2D, 0x1F, 0xBE, 0xBF, 0xA7, 0x6A};
    ASSERT_TRUE(memory.write_bytes(DATA_BASE + 0x200, libid.data(),
                                   libid.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(DATA_BASE, 0xDEADBEEF).is_ok());

    // x64: rcx=guid, rdx=major, r8=minor, r9=lcid, [rsp+0x28]=pptlib
    cpu.gpr.rcx = DATA_BASE + 0x200;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8  = 0;
    cpu.gpr.r9  = 0x409;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, DATA_BASE).is_ok());

    Marshaler m(memory);
    m.attach_cpu(cpu);
    EXPECT_EQ(mgr.load_reg_type_lib(m), TYPE_E_LIBNOTREGISTERED);
    auto out = memory.read_u32(DATA_BASE);
    ASSERT_TRUE(out.is_ok());
    EXPECT_EQ(out.value(), 0u);
}

TEST_F(OleAut32Test, LoadRegTypeLib_RegisteredPathIsResolved) {
    constexpr u32 TYPE_E_CANTLOADLIBRARY = 0x80029C4A;
    capsule::Registry registry;
    mgr.set_registry(&registry);

    // Seed the same key RegisterTypeLib writes. No VFS → path resolves but
    // LoadTypeLib then fails with CANTLOADLIBRARY (proves lookup worked).
    auto key = registry.open_key(
        capsule::HKEY_CLASSES_ROOT,
        L"TypeLib\\{94636247-BC39-4B8B-A728-2D1FBEBFA76A}\\1.0\\409\\win32",
        /*create=*/true);
    ASSERT_TRUE(key.is_ok());
    ASSERT_TRUE(registry
                    .set_value(key.value(), L"",
                               capsule::RegValue{
                                   capsule::RegType::Sz,
                                   std::wstring(L"C:\\missing\\IsProBE.tlb")})
                    .is_ok());
    registry.close_key(key.value());

    const std::array<u8, 16> libid{
        0x47, 0x62, 0x63, 0x94, 0x39, 0xBC, 0x8B, 0x4B,
        0xA7, 0x28, 0x2D, 0x1F, 0xBE, 0xBF, 0xA7, 0x6A};
    ASSERT_TRUE(memory.write_bytes(DATA_BASE + 0x200, libid.data(),
                                   libid.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(DATA_BASE, 0xDEADBEEF).is_ok());

    cpu.gpr.rcx = DATA_BASE + 0x200;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8  = 0;
    cpu.gpr.r9  = 0x409;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, DATA_BASE).is_ok());

    Marshaler m(memory);
    m.attach_cpu(cpu);
    EXPECT_EQ(mgr.load_reg_type_lib(m), TYPE_E_CANTLOADLIBRARY);
    auto out = memory.read_u32(DATA_BASE);
    ASSERT_TRUE(out.is_ok());
    EXPECT_EQ(out.value(), 0u);
}

TEST_F(OleAut32Test, RegisteredInterfaceResolvesNumberedPeTypeLibResource) {
    const char* home = std::getenv("HOME");
    if (home == nullptr) GTEST_SKIP() << "HOME is unavailable";
    const std::filesystem::path drive_root =
        std::filesystem::path(home) /
        ".vitreo/capsules/Chrome/drives/c";
    const std::filesystem::path updater = drive_root /
        "Users/Default/AppData/Local/Google/GoogleUpdater/150.0.7863.0/"
        "updater.exe";
    if (!std::filesystem::exists(updater)) {
        GTEST_SKIP() << "Chrome updater fixture is unavailable";
    }

    capsule::Registry registry;
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(drive_root.string()))
                    .is_ok());
    mgr.set_registry(&registry);
    mgr.set_vfs(&vfs);

    const std::wstring iid_text =
        L"{86284E24-D3B8-58F2-8BFF-2D0A49D88734}";
    auto interface_key = registry.open_key(
        capsule::HKEY_CLASSES_ROOT,
        L"Interface\\" + iid_text + L"\\TypeLib", true);
    ASSERT_TRUE(interface_key.is_ok());
    ASSERT_TRUE(registry.set_value(
        interface_key.value(), L"", capsule::RegValue{
            capsule::RegType::Sz, iid_text}).is_ok());
    ASSERT_TRUE(registry.set_value(
        interface_key.value(), L"Version", capsule::RegValue{
            capsule::RegType::Sz, std::wstring(L"1.0")}).is_ok());
    registry.close_key(interface_key.value());

    auto path_key = registry.open_key(
        capsule::HKEY_CLASSES_ROOT,
        L"TypeLib\\" + iid_text + L"\\1.0\\0\\win32", true);
    ASSERT_TRUE(path_key.is_ok());
    ASSERT_TRUE(registry.set_value(
        path_key.value(), L"", capsule::RegValue{
            capsule::RegType::Sz,
            std::wstring(
                L"C:\\Users\\Default\\AppData\\Local\\Google\\"
                L"GoogleUpdater\\150.0.7863.0\\updater.exe\\2")})
                    .is_ok());
    registry.close_key(path_key.value());

    const TypeLibGuid iid{
        0x24, 0x4E, 0x28, 0x86, 0xB8, 0xD3, 0xF2, 0x58,
        0x8B, 0xFF, 0x2D, 0x0A, 0x49, 0xD8, 0x87, 0x34};
    auto info = mgr.resolve_registered_type_info(iid);
    ASSERT_TRUE(info.has_value());
    EXPECT_TRUE(info->has_guid);
    EXPECT_EQ(info->name, "IUpdaterInternalUser");
    EXPECT_GT(info->c_funcs, 0u);
    EXPECT_FALSE(info->funcs.empty());
    ASSERT_EQ(info->funcs[0].name, "Run");
    ASSERT_EQ(info->funcs[0].param_vts.size(), 1u);
    EXPECT_EQ(info->funcs[0].param_vts[0], 13u);  // VT_UNKNOWN
    EXPECT_EQ(info->funcs[0].param_hrefs[0], 0xC8);
    EXPECT_NE(info->funcs[0].param_iids[0], TypeLibGuid{});
}

// =============================================================================
// LoadTypeLib Tests
// =============================================================================

// 24. LoadTypeLib validates its out-parameter before anything else
TEST_F(OleAut32Test, LoadTypeLib_NullArgumentsAreRejected) {
    constexpr u32 E_INVALIDARG = 0x80070057;

    auto m = make_marshaler(DATA_BASE, 0, 0);
    EXPECT_EQ(mgr.load_type_lib(m), E_INVALIDARG); // ppTypeLib == NULL

    // A NULL szFile is rejected, and the out-parameter is cleared first so the
    // caller never releases a stale pointer.
    ASSERT_TRUE(memory.write_u32(DATA_BASE, 0xDEADBEEF).is_ok());
    auto m2 = make_marshaler(0, DATA_BASE, 0);
    EXPECT_EQ(mgr.load_type_lib(m2), E_INVALIDARG);
    auto out = memory.read_u32(DATA_BASE);
    ASSERT_TRUE(out.is_ok());
    EXPECT_EQ(out.value(), 0u);
}

// 25. Without a capsule VFS there is no file to read, and LoadTypeLib says so
//     rather than returning a library with invented attributes.
TEST_F(OleAut32Test, LoadTypeLib_WithoutVfsFailsToLoad) {
    constexpr u32 TYPE_E_CANTLOADLIBRARY = 0x80029C4A;

    // "a.tlb" as a null-terminated UTF-16 string.
    const std::array<u16, 6> path{u'a', u'.', u't', u'l', u'b', 0};
    for (usize i = 0; i < path.size(); ++i) {
        ASSERT_TRUE(memory.write_u16(DATA_BASE + 0x100 +
                                     static_cast<GuestAddr>(i * 2),
                                     path[i]).is_ok());
    }

    auto m = make_marshaler(DATA_BASE + 0x100, DATA_BASE, 0);
    EXPECT_EQ(mgr.load_type_lib(m), TYPE_E_CANTLOADLIBRARY);
    auto out = memory.read_u32(DATA_BASE);
    ASSERT_TRUE(out.is_ok());
    EXPECT_EQ(out.value(), 0u);
}

// =============================================================================
// Thunk Export Creation Tests
// =============================================================================

// 25. create_oleaut32_thunks returns non-empty exports
TEST_F(OleAut32Test, CreateThunks_ReturnsExports) {
    DllExports exports = create_oleaut32_thunks(mgr);
    EXPECT_FALSE(exports.entries.empty());
    EXPECT_GE(exports.entries.size(), 10u);
}

// 26. create_oleaut32_thunks includes SysAllocString
TEST_F(OleAut32Test, CreateThunks_IncludesSysAllocString) {
    DllExports exports = create_oleaut32_thunks(mgr);

    bool found = false;
    for (const auto& entry : exports.entries) {
        if (entry.name == "SysAllocString") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "SysAllocString should be in exports";
}

// 27. create_oleaut32_thunks includes VariantInit
TEST_F(OleAut32Test, CreateThunks_IncludesVariantInit) {
    DllExports exports = create_oleaut32_thunks(mgr);

    bool found = false;
    for (const auto& entry : exports.entries) {
        if (entry.name == "VariantInit") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "VariantInit should be in exports";
}

// 28. create_oleaut32_thunks includes SafeArrayCreate
TEST_F(OleAut32Test, CreateThunks_IncludesSafeArrayCreate) {
    DllExports exports = create_oleaut32_thunks(mgr);

    bool found = false;
    for (const auto& entry : exports.entries) {
        if (entry.name == "SafeArrayCreate") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "SafeArrayCreate should be in exports";
}

TEST_F(OleAut32Test, GetErrorInfoPe32WritesOnlyGuestPointerWidth) {
    DllExports exports = create_oleaut32_thunks(mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "GetErrorInfo";
        });
    ASSERT_NE(entry, exports.entries.end());

    cpu.init_protected_mode();
    const u32 sp = static_cast<u32>(STACK_BASE + 0x800);
    const GuestAddr out = DATA_BASE + 0x600;
    cpu.set_sp(sp);
    ASSERT_TRUE(memory.write_u32(sp, 0xDEADC0DEu));
    ASSERT_TRUE(memory.write_u32(sp + 4, 0));   // dwReserved
    ASSERT_TRUE(memory.write_u32(sp + 8, static_cast<u32>(out)));
    ASSERT_TRUE(memory.write_u32(out, 0xA5A5A5A5u));
    ASSERT_TRUE(memory.write_u32(out + 4, 0xCAFEBABEu));

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::RAX)), 1u);
    EXPECT_EQ(memory.read_u32(out).value_or(0xFFFFFFFFu), 0u);
    EXPECT_EQ(memory.read_u32(out + 4).value_or(0), 0xCAFEBABEu);
}

// =============================================================================
// SafeArray ordinal table + Get/PutElement. Values independently checked against
// automation_export_ordinals_probe.x86.expected and .x64.expected.
// =============================================================================

TEST_F(OleAut32Test, SafeArrayOrdinals_MatchNativeWindows) {
    DllExports exports = create_oleaut32_thunks(mgr);
    auto ordinal_of = [&](const char* name) -> u16 {
        for (const auto& e : exports.entries) {
            if (e.name == name) return e.ordinal;
        }
        return 0;
    };
    auto stack_of = [&](const char* name) -> u32 {
        for (const auto& e : exports.entries) {
            if (e.name == name) return e.stdcall_bytes;
        }
        return 0xFFFFFFFFu;
    };

    EXPECT_EQ(ordinal_of("SafeArrayGetDim"), 17);
    EXPECT_EQ(ordinal_of("SafeArrayGetElemsize"), 18);
    EXPECT_EQ(ordinal_of("SafeArrayGetUBound"), 19);
    EXPECT_EQ(ordinal_of("SafeArrayGetLBound"), 20);
    EXPECT_EQ(ordinal_of("SafeArrayLock"), 21);
    EXPECT_EQ(ordinal_of("SafeArrayUnlock"), 22);
    EXPECT_EQ(ordinal_of("SafeArrayAccessData"), 23);
    EXPECT_EQ(ordinal_of("SafeArrayUnaccessData"), 24);
    EXPECT_EQ(ordinal_of("SafeArrayGetElement"), 25);
    EXPECT_EQ(ordinal_of("SafeArrayPutElement"), 26);
    EXPECT_EQ(ordinal_of("SafeArrayCopy"), 27);
    EXPECT_EQ(ordinal_of("SafeArrayGetVartype"), 77);
    EXPECT_EQ(ordinal_of("VariantCopyInd"), 11);
    EXPECT_EQ(ordinal_of("CreateErrorInfo"), 202);
    EXPECT_EQ(ordinal_of("BSTR_UserSize"), 283);
    EXPECT_EQ(ordinal_of("BSTR_UserMarshal"), 284);
    EXPECT_EQ(ordinal_of("BSTR_UserUnmarshal"), 285);
    EXPECT_EQ(ordinal_of("BSTR_UserFree"), 286);
    EXPECT_EQ(ordinal_of("VARIANT_UserSize"), 287);
    EXPECT_EQ(ordinal_of("VARIANT_UserFree"), 290);
    EXPECT_EQ(ordinal_of("LPSAFEARRAY_UserSize"), 291);
    EXPECT_EQ(ordinal_of("LPSAFEARRAY_UserFree"), 294);
    EXPECT_EQ(ordinal_of("LoadTypeLib"), 161);
    EXPECT_EQ(ordinal_of("LoadTypeLibEx"), 183);
    EXPECT_EQ(ordinal_of("RegisterTypeLib"), 163);
    EXPECT_EQ(ordinal_of("UnRegisterTypeLib"), 186);

    EXPECT_EQ(stack_of("SafeArrayGetElement"), 12u);
    EXPECT_EQ(stack_of("SafeArrayPutElement"), 12u);
    EXPECT_EQ(stack_of("SafeArrayAccessData"), 8u);
    EXPECT_EQ(stack_of("SafeArrayUnaccessData"), 4u);
    EXPECT_EQ(stack_of("CreateErrorInfo"), 4u);
    EXPECT_EQ(stack_of("BSTR_UserSize"), 12u);
    EXPECT_EQ(stack_of("BSTR_UserMarshal"), 12u);
    EXPECT_EQ(stack_of("BSTR_UserUnmarshal"), 12u);
    EXPECT_EQ(stack_of("BSTR_UserFree"), 8u);
    EXPECT_EQ(stack_of("VARIANT_UserFree"), 8u);
    EXPECT_EQ(stack_of("LPSAFEARRAY_UserFree"), 8u);
}

TEST_F(OleAut32Test, BSTR_UserFree_ClearsCallerPointer) {
    DllExports exports = create_oleaut32_thunks(mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "BSTR_UserFree" && candidate.ordinal == 286;
        });
    ASSERT_NE(entry, exports.entries.end());

    constexpr GuestAddr str_addr = DATA_BASE + 0x100;
    constexpr GuestAddr pbstr_addr = DATA_BASE + 0x200;
    (void)memory.write_u16(str_addr, u'A');
    (void)memory.write_u16(str_addr + 2, 0);
    auto alloc_m = make_marshaler(str_addr);
    GuestAddr bstr = mgr.sys_alloc_string(alloc_m);
    ASSERT_NE(bstr, 0u);
    ASSERT_TRUE(memory.write_u32(pbstr_addr, static_cast<u32>(bstr)));

    auto free_m = make_marshaler(/*pFlags*/ 0, pbstr_addr);
    entry->handler(free_m);
    EXPECT_EQ(memory.read_u32(pbstr_addr).value_or(0xFFFFFFFFu), 0u);
}

TEST_F(OleAut32Test, BSTR_UserMarshal_RoundTripNullAndString) {
    DllExports exports = create_oleaut32_thunks(mgr);
    auto find = [&](const char* name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& e) { return e.name == name; });
    };
    auto size_e = find("BSTR_UserSize");
    auto mar_e = find("BSTR_UserMarshal");
    auto unmar_e = find("BSTR_UserUnmarshal");
    ASSERT_NE(size_e, exports.entries.end());
    ASSERT_NE(mar_e, exports.entries.end());
    ASSERT_NE(unmar_e, exports.entries.end());

    constexpr GuestAddr buf_base = DATA_BASE + 0x900;
    constexpr GuestAddr pbstr = DATA_BASE + 0x200;
    constexpr GuestAddr str_addr = DATA_BASE + 0x100;

    // Null BSTR: size = align(1)+8 = 12 from StartingSize=1
    ASSERT_TRUE(memory.write_u32(pbstr, 0));
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = 1; // StartingSize
    cpu.gpr.r8 = pbstr;
    {
        Marshaler m(memory);
        m.attach_cpu(cpu);
        size_e->handler(m);
        EXPECT_EQ(cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::RAX)), 12u);
    }

    // Marshal null into buf_base (unaligned start +1)
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = buf_base + 1;
    cpu.gpr.r8 = pbstr;
    GuestAddr after = 0;
    {
        Marshaler m(memory);
        m.attach_cpu(cpu);
        mar_e->handler(m);
        after = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RAX));
    }
    EXPECT_EQ(memory.read_u32(buf_base + 4).value_or(0), 0xFFFFFFFFu); // aligned at +4
    EXPECT_EQ(memory.read_u32(buf_base + 8).value_or(1), 0u);
    EXPECT_EQ(after, buf_base + 12);

    // Unmarshal null back
    ASSERT_TRUE(memory.write_u32(pbstr, 0xDEADBEEFu));
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = buf_base + 1;
    cpu.gpr.r8 = pbstr;
    {
        Marshaler m(memory);
        m.attach_cpu(cpu);
        unmar_e->handler(m);
    }
    EXPECT_EQ(memory.read_u32(pbstr).value_or(1), 0u);

    // Non-null string round-trip
    (void)memory.write_u16(str_addr, u'O');
    (void)memory.write_u16(str_addr + 2, u'K');
    (void)memory.write_u16(str_addr + 4, 0);
    auto alloc_m = make_marshaler(str_addr);
    GuestAddr bstr = mgr.sys_alloc_string(alloc_m);
    ASSERT_NE(bstr, 0u);
    ASSERT_TRUE(memory.write_u32(pbstr, static_cast<u32>(bstr)));

    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = buf_base;
    cpu.gpr.r8 = pbstr;
    {
        Marshaler m(memory);
        m.attach_cpu(cpu);
        mar_e->handler(m);
        after = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RAX));
    }
    EXPECT_EQ(memory.read_u32(buf_base).value_or(0), 4u); // cBytes
    EXPECT_EQ(memory.read_u32(buf_base + 4).value_or(0), 2u); // clSize
    EXPECT_EQ(after, buf_base + 12);

    ASSERT_TRUE(memory.write_u32(pbstr, 0));
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = buf_base;
    cpu.gpr.r8 = pbstr;
    {
        Marshaler m(memory);
        m.attach_cpu(cpu);
        unmar_e->handler(m);
    }
    const u32 out = memory.read_u32(pbstr).value_or(0);
    ASSERT_NE(out, 0u);
    EXPECT_EQ(memory.read_u32(out - 4).value_or(0), 4u);
    EXPECT_EQ(memory.read_u16(out).value_or(0), u'O');
    EXPECT_EQ(memory.read_u16(out + 2).value_or(0), u'K');
}

TEST_F(OleAut32Test, SafeArrayPutGetElement_RoundTripI4) {
    constexpr GuestAddr bound_addr = DATA_BASE + 0x700;
    constexpr GuestAddr idx_addr = DATA_BASE + 0x710;
    constexpr GuestAddr val_addr = DATA_BASE + 0x720;
    constexpr GuestAddr out_addr = DATA_BASE + 0x730;

    (void)memory.write_u32(bound_addr, 4);     // cElements
    (void)memory.write_u32(bound_addr + 4, 0); // lLbound
    auto create_m = make_marshaler(3 /*VT_I4*/, 1, bound_addr);
    u32 psa = mgr.safe_array_create(create_m);
    ASSERT_NE(psa, 0u);

    (void)memory.write_u32(idx_addr, 2);
    (void)memory.write_u32(val_addr, 0xA1B2C3D4u);
    auto put_m = make_marshaler(psa, idx_addr, val_addr);
    EXPECT_EQ(mgr.safe_array_put_element(put_m), 0u);

    (void)memory.write_u32(out_addr, 0);
    auto get_m = make_marshaler(psa, idx_addr, out_addr);
    EXPECT_EQ(mgr.safe_array_get_element(get_m), 0u);
    EXPECT_EQ(memory.read_u32(out_addr).value(), 0xA1B2C3D4u);

    // Bounds: UBound=3, LBound=0 for a 0-based 4-element array
    constexpr GuestAddr ub_addr = DATA_BASE + 0x740;
    constexpr GuestAddr lb_addr = DATA_BASE + 0x744;
    auto ub_m = make_marshaler(psa, 1, ub_addr);
    EXPECT_EQ(mgr.safe_array_get_ubound(ub_m), 0u);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(ub_addr).value()), 3);
    auto lb_m = make_marshaler(psa, 1, lb_addr);
    EXPECT_EQ(mgr.safe_array_get_lbound(lb_m), 0u);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(lb_addr).value()), 0);
}

TEST_F(OleAut32Test, SafeArrayAccessData_LocksAndReturnsPvData) {
    constexpr GuestAddr bound_addr = DATA_BASE + 0x700;
    constexpr GuestAddr out_addr = DATA_BASE + 0x710;
    (void)memory.write_u32(bound_addr, 2);
    (void)memory.write_u32(bound_addr + 4, 0);
    auto create_m = make_marshaler(3, 1, bound_addr);
    u32 psa = mgr.safe_array_create(create_m);
    ASSERT_NE(psa, 0u);
    const u64 expected_pv = memory.read_u64(psa + 16).value();

    auto acc = make_marshaler(psa, out_addr);
    EXPECT_EQ(mgr.safe_array_access_data(acc), 0u);
    EXPECT_EQ(memory.read_u64(out_addr).value(), expected_pv);
    EXPECT_EQ(memory.read_u32(psa + 8).value(), 1u); // cLocks
    auto unacc = make_marshaler(psa);
    EXPECT_EQ(mgr.safe_array_unaccess_data(unacc), 0u);
    EXPECT_EQ(memory.read_u32(psa + 8).value(), 0u);
}

TEST_F(OleAut32Test, SafeArrayPutGetElement_BstrByValue) {
    // Pass the BSTR itself as pvData, not its address.
    constexpr GuestAddr bound_addr = DATA_BASE + 0x700;
    constexpr GuestAddr idx_addr = DATA_BASE + 0x710;
    constexpr GuestAddr str_addr = DATA_BASE + 0x800;
    constexpr GuestAddr out_addr = DATA_BASE + 0x820;

    (void)memory.write_u16(str_addr, u'H');
    (void)memory.write_u16(str_addr + 2, u'i');
    (void)memory.write_u16(str_addr + 4, 0);
    auto alloc_m = make_marshaler(str_addr);
    GuestAddr bstr = mgr.sys_alloc_string(alloc_m);
    ASSERT_NE(bstr, 0u);

    (void)memory.write_u32(bound_addr, 2);
    (void)memory.write_u32(bound_addr + 4, 0);
    auto create_m = make_marshaler(8 /*VT_BSTR*/, 1, bound_addr);
    u32 psa = mgr.safe_array_create(create_m);
    ASSERT_NE(psa, 0u);

    (void)memory.write_u32(idx_addr, 0);
    // Pass BSTR by value (the guest pointer itself), not a pointer-to-BSTR.
    auto put_m = make_marshaler(psa, idx_addr, bstr);
    EXPECT_EQ(mgr.safe_array_put_element(put_m), 0u);

    (void)memory.write_u32(out_addr, 0);
    auto get_m = make_marshaler(psa, idx_addr, out_addr);
    EXPECT_EQ(mgr.safe_array_get_element(get_m), 0u);
    const u32 got = memory.read_u32(out_addr).value();
    ASSERT_NE(got, 0u);
    EXPECT_NE(got, static_cast<u32>(bstr)); // must be a copy
    EXPECT_EQ(memory.read_u16(got).value(), u'H');
    EXPECT_EQ(memory.read_u16(got + 2).value(), u'i');
}

// =============================================================================
// Multiple Manager Instance Test
// =============================================================================

// 29. Multiple managers have independent scratch offsets
TEST_F(OleAut32Test, MultipleManagers_IndependentAllocators) {
    OleAut32Manager mgr2;

    constexpr GuestAddr str_addr = DATA_BASE + 0x100;
    memory.write_u16(str_addr, u'A');
    memory.write_u16(str_addr + 2, 0);

    auto m1 = make_marshaler(str_addr);
    GuestAddr bstr1_a = mgr.sys_alloc_string(m1);
    auto m2 = make_marshaler(str_addr);
    GuestAddr bstr1_b = mgr2.sys_alloc_string(m2);

    // Both should return valid pointers (though they overlap in scratch space,
    // that's fine for independent manager testing)
    EXPECT_NE(bstr1_a, 0u);
    EXPECT_NE(bstr1_b, 0u);
}

TEST_F(OleAut32Test, SafeArrayCreate_X64VariantHas24ByteElements) {
    constexpr GuestAddr bound_addr = DATA_BASE + 0x700;
    (void)memory.write_u32(bound_addr, 2);
    (void)memory.write_u32(bound_addr + 4, 0);
    auto m = make_marshaler(12 /*VT_VARIANT*/, 1, bound_addr);
    u32 psa = mgr.safe_array_create(m);
    ASSERT_NE(psa, 0u);
    EXPECT_EQ(memory.read_u32(psa + 4).value(), 24u);
    EXPECT_NE(memory.read_u64(psa + 16).value(), 0u); // pvData (separate block)
    EXPECT_EQ(memory.read_u32(psa + 24).value(), 2u); // rgsabound[0].cElements
    EXPECT_EQ(memory.read_u16(psa + 2).value(), 0x0880u); // HAVEVARTYPE|VARIANT
    EXPECT_EQ(memory.read_u32(psa - 4).value(), 12u);     // hidden VARTYPE
}

TEST_F(OleAut32Test, SafeArrayPutGetElement_X64VariantKeepsSecondBstr) {
    constexpr GuestAddr bound_addr = DATA_BASE + 0x700;
    constexpr GuestAddr idx_addr = DATA_BASE + 0x710;
    constexpr GuestAddr var_addr = DATA_BASE + 0x720;
    constexpr GuestAddr out_addr = DATA_BASE + 0x750;
    constexpr GuestAddr str0 = DATA_BASE + 0x800;
    constexpr GuestAddr str1 = DATA_BASE + 0x820;

    auto write_utf16 = [&](GuestAddr addr, const char16_t* s) {
        usize i = 0;
        for (; s[i] != 0; ++i) {
            (void)memory.write_u16(addr + i * 2, static_cast<u16>(s[i]));
        }
        (void)memory.write_u16(addr + i * 2, 0);
    };
    write_utf16(str0, u"Sims3Launcher.ex_");
    write_utf16(str1, u"Sims3Launcher.exe");
    auto alloc0 = make_marshaler(str0);
    GuestAddr src_bstr = mgr.sys_alloc_string(alloc0);
    auto alloc1 = make_marshaler(str1);
    GuestAddr dst_bstr = mgr.sys_alloc_string(alloc1);
    ASSERT_NE(src_bstr, 0u);
    ASSERT_NE(dst_bstr, 0u);

    (void)memory.write_u32(bound_addr, 2);
    (void)memory.write_u32(bound_addr + 4, 0);
    auto create_m = make_marshaler(12 /*VT_VARIANT*/, 1, bound_addr);
    u32 psa = mgr.safe_array_create(create_m);
    ASSERT_NE(psa, 0u);

    auto put_bstr_variant = [&](u32 index, GuestAddr bstr) {
        (void)memory.write_u32(idx_addr, index);
        for (u32 i = 0; i < 24; ++i) {
            (void)memory.write_u8(var_addr + i, 0);
        }
        (void)memory.write_u16(var_addr, VT_BSTR);
        (void)memory.write_u64(var_addr + 8, bstr);
        auto put_m = make_marshaler(psa, idx_addr, var_addr);
        EXPECT_EQ(mgr.safe_array_put_element(put_m), 0u);
    };
    put_bstr_variant(0, src_bstr);
    put_bstr_variant(1, dst_bstr);

    (void)memory.write_u32(idx_addr, 1);
    for (u32 i = 0; i < 24; ++i) {
        (void)memory.write_u8(out_addr + i, 0);
    }
    auto get_m = make_marshaler(psa, idx_addr, out_addr);
    EXPECT_EQ(mgr.safe_array_get_element(get_m), 0u);
    EXPECT_EQ(memory.read_u16(out_addr).value(), VT_BSTR);
    const u64 got = memory.read_u64(out_addr + 8).value();
    ASSERT_NE(got, 0u);
    EXPECT_EQ(memory.read_u16(static_cast<GuestAddr>(got)).value(), u'S');
    EXPECT_EQ(memory.read_u16(static_cast<GuestAddr>(got) + 32).value(), u'e');
}

TEST_F(OleAut32Test, SafeArrayCreate_ProtectedModeUsesX86Layout) {
    cpu.mode = cpu::CpuMode::Protected;
    cpu.set_sp(STACK_BASE + 0x800);
    constexpr GuestAddr bound_addr = DATA_BASE + 0x700;
    (void)memory.write_u32(bound_addr, 2);
    (void)memory.write_u32(bound_addr + 4, 0);
    const GuestAddr sp = static_cast<GuestAddr>(cpu.sp());
    (void)memory.write_u32(sp + 0, 0);  // return address
    (void)memory.write_u32(sp + 4, 12); // VT_VARIANT
    (void)memory.write_u32(sp + 8, 1);  // cDims
    (void)memory.write_u32(sp + 12, static_cast<u32>(bound_addr));
    Marshaler m(memory);
    m.attach_cpu(cpu);
    u32 psa = mgr.safe_array_create(m);
    ASSERT_NE(psa, 0u);
    EXPECT_EQ(memory.read_u32(psa + 4).value(), 16u);
    EXPECT_NE(memory.read_u32(psa + 12).value(), 0u);  // pvData at +12
    EXPECT_EQ(memory.read_u32(psa + 16).value(), 2u);  // bounds at +16
}

// S7 audit: VariantTimeToSystemTime was a stub that always failed, and on x64
// read its pointer from the wrong slot (the DATE travels in XMM0).
// Expected values: tools/oracle_probe/s7_ole_probe.expected (x86 = x64).
TEST(OleAut32VariantTimeTest, ConvertsLikeWindowsOnBothBitnesses) {
    struct Case { double date; int ok; u16 y, mo, d, dow, h, mi, s; };
    const Case cases[] = {
        {45000.5, 1, 2023, 3, 15, 3, 12, 0, 0},
        {45000.0000115, 1, 2023, 3, 15, 3, 0, 0, 1},
        {45000.999994, 1, 2023, 3, 15, 3, 23, 59, 59},
        {-1.25, 1, 1899, 12, 29, 5, 6, 0, 0},
        {2958465.99999, 1, 9999, 12, 31, 5, 23, 59, 59},
        {2958466.0, 0, 0, 0, 0, 0, 0, 0, 0},
        {-657434.0, 1, 100, 1, 1, 5, 0, 0, 0},
        {-657435.0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0.0, 1, 1899, 12, 30, 6, 0, 0, 0},
    };
    constexpr GuestAddr kStack = 0x80000;
    constexpr GuestAddr kOut = 0x2000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    OleAut32Manager mgr;
    DllExports exports = create_oleaut32_thunks(mgr);
    const ExportEntry* entry = nullptr;
    for (const auto& e : exports.entries) {
        if (e.name == "VariantTimeToSystemTime") entry = &e;
    }
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->ordinal, 185u);
    EXPECT_EQ(entry->stdcall_bytes, 12u);  // DATE (8) + pointer
    for (const bool is64 : {false, true}) {
        for (const auto& c : cases) {
            cpu::CpuState cpu;
            u64 bits = 0;
            std::memcpy(&bits, &c.date, 8);
            if (is64) {
                cpu.init_long_mode();
                cpu.gpr.rsp = kStack + 0x800;
                cpu.sse.xmm[0].u64_[0] = bits;
                cpu.gpr.rdx = kOut;
            } else {
                cpu.init_protected_mode();
                cpu.set_sp(kStack + 0x800);
                ASSERT_TRUE(memory.write_u32(kStack + 0x804, static_cast<u32>(bits)).is_ok());
                ASSERT_TRUE(memory.write_u32(kStack + 0x808, static_cast<u32>(bits >> 32)).is_ok());
                ASSERT_TRUE(memory.write_u32(kStack + 0x80C, kOut).is_ok());
            }
            for (u32 i = 0; i < 16; ++i) ASSERT_TRUE(memory.write_u8(kOut + i, 0xCC).is_ok());
            Marshaler m(memory);
            m.attach_cpu(cpu);
            entry->handler(m);
            EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), static_cast<u32>(c.ok)) << c.date;
            if (!c.ok) {
                EXPECT_EQ(memory.read_u16(kOut).value(), 0xCCCCu) << c.date;
                continue;
            }
            EXPECT_EQ(memory.read_u16(kOut + 0).value(), c.y) << c.date;
            EXPECT_EQ(memory.read_u16(kOut + 2).value(), c.mo) << c.date;
            EXPECT_EQ(memory.read_u16(kOut + 4).value(), c.dow) << c.date;
            EXPECT_EQ(memory.read_u16(kOut + 6).value(), c.d) << c.date;
            EXPECT_EQ(memory.read_u16(kOut + 8).value(), c.h) << c.date;
            EXPECT_EQ(memory.read_u16(kOut + 10).value(), c.mi) << c.date;
            EXPECT_EQ(memory.read_u16(kOut + 12).value(), c.s) << c.date;
            EXPECT_EQ(memory.read_u16(kOut + 14).value(), 0u) << c.date;
        }
    }
}
