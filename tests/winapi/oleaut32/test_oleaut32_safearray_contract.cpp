// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_oleaut32_safearray_contract.cpp
/// @brief SAFEARRAY / BSTR contract tests against Windows.
///
/// Every expected value comes from tools/oracle_probe/oleaut32_safearray_probe
/// (.expected = Windows 10 x64, .x86.expected = WOW64), not from running
/// Vitreo. The fixture wires a real process heap through the COM task
/// allocator, as dll_registry does for a guest process.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/com/oleaut32/oleaut32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/kernel32/memory/memory.hpp"

#include <map>
#include <string_view>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 kDispEBadIndex      = 0x8002000Bu;
constexpr u32 kDispEOverflow      = 0x8002000Au;
constexpr u32 kDispEArrayIsLocked = 0x8002000Du;
constexpr u32 kEInvalidArg        = 0x80070057u;
constexpr u32 kEUnexpected        = 0x8000FFFFu;

// Fake COM object: vtable slots hold these sentinel "function" addresses and
// the guest-call hook counts AddRef / Release per object.
constexpr GuestAddr kFnQi      = 0xA0001;
constexpr GuestAddr kFnAddRef  = 0xA0002;
constexpr GuestAddr kFnRelease = 0xA0003;

class SafeArrayContractTest : public ::testing::Test {
protected:
    GuestMemory          memory;
    HandleTable          handles;
    VirtualMemoryManager vmm{memory};
    HeapManager          heap_mgr{vmm, memory, handles};
    ComTaskMemoryManager task_memory{heap_mgr};
    OleAut32Manager      mgr;
    cpu::CpuState        cpu;
    std::map<GuestAddr, int> addrefs;
    std::map<GuestAddr, int> releases;

    static constexpr GuestAddr kData  = 0x10000;
    static constexpr GuestAddr kStack = 0x20000;
    static constexpr GuestAddr kVtbl  = 0x10F00;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kData, 0x20000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.gpr.rsp = kStack + 0x800;
        mgr.set_task_memory(&task_memory);
        mgr.set_guest_call([this](GuestAddr fn, const u64* args, usize n,
                                  bool) -> u64 {
            if (n == 0) return 0;
            const GuestAddr obj = static_cast<GuestAddr>(args[0]);
            if (fn == kFnAddRef) return ++addrefs[obj];
            if (fn == kFnRelease) return ++releases[obj];
            return 0;
        });
        (void)memory.write_u64(kVtbl, kFnQi);
        (void)memory.write_u64(kVtbl + 8, kFnAddRef);
        (void)memory.write_u64(kVtbl + 16, kFnRelease);
    }

    Marshaler mk(u64 a0 = 0, u64 a1 = 0, u64 a2 = 0) {
        cpu.gpr.rcx = a0;
        cpu.gpr.rdx = a1;
        cpu.gpr.r8 = a2;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        return m;
    }

    GuestAddr create(u16 vt, std::initializer_list<std::pair<u32, i32>> b) {
        GuestAddr p = kData + 0x100;
        for (const auto& [c, l] : b) {
            (void)memory.write_u32(p, c);
            (void)memory.write_u32(p + 4, static_cast<u32>(l));
            p += 8;
        }
        auto m = mk(vt, b.size(), kData + 0x100);
        return mgr.safe_array_create(m);
    }

    GuestAddr make_object(GuestAddr at) {
        (void)memory.write_u64(at, kVtbl);
        return at;
    }

    GuestAddr indices(std::initializer_list<i32> idx) {
        GuestAddr p = kData + 0x200;
        for (i32 v : idx) {
            (void)memory.write_u32(p, static_cast<u32>(v));
            p += 4;
        }
        return kData + 0x200;
    }

    GuestAddr bstr(std::u16string_view text) {
        const GuestAddr src = kData + 0x400;
        for (usize i = 0; i < text.size(); ++i) {
            (void)memory.write_u16(src + i * 2, text[i]);
        }
        auto m = mk();
        return mgr.sys_alloc_string_len(m, src, static_cast<u32>(text.size()));
    }
};

// probe: create_vt vt=0..4a (x64 column).
TEST_F(SafeArrayContractTest, CreateElementSizeAndFeaturesPerVartype) {
    struct Row { u16 vt; u32 cb; u16 ff; };
    const Row ok[] = {
        {0x02, 2, 0x80},  {0x03, 4, 0x80},  {0x04, 4, 0x80},  {0x05, 8, 0x80},
        {0x06, 8, 0x80},  {0x07, 8, 0x80},  {0x08, 8, 0x180}, {0x09, 8, 0x440},
        {0x0a, 4, 0x80},  {0x0b, 2, 0x80},  {0x0c, 24, 0x880}, {0x0d, 8, 0x240},
        {0x0e, 16, 0x80}, {0x10, 1, 0x80},  {0x11, 1, 0x80},  {0x12, 2, 0x80},
        {0x13, 4, 0x80},  {0x14, 8, 0x80},  {0x15, 8, 0x80},  {0x16, 4, 0x80},
        {0x17, 4, 0x80},  {0x25, 8, 0x80},  {0x26, 8, 0x80},
    };
    for (const Row& r : ok) {
        const GuestAddr psa = create(r.vt, {{3, 0}});
        ASSERT_NE(psa, 0u) << "vt=" << r.vt;
        EXPECT_EQ(memory.read_u32(psa + 4).value(), r.cb) << "vt=" << r.vt;
        EXPECT_EQ(memory.read_u16(psa + 2).value(), r.ff) << "vt=" << r.vt;
        if (r.ff & 0x80) {
            EXPECT_EQ(memory.read_u32(psa - 4).value(), r.vt);
        }
        const GuestAddr out = kData + 0x300;
        (void)memory.write_u16(out, 0xEEEE);
        auto gv = mk(psa, out);
        EXPECT_EQ(mgr.safe_array_get_vartype(gv), 0u);
        EXPECT_EQ(memory.read_u16(out).value(), r.vt);
    }
    for (u16 vt : {0x00, 0x01, 0x0f, 0x18, 0x1e, 0x1f, 0x24, 0x40, 0x48,
                   0xFFFF, 0x2003, 0x4003, 0x1003}) {
        EXPECT_EQ(create(vt, {{3, 0}}), 0u) << "vt=" << vt;
    }
}

// probe: md*, md_bound, md_put_*, md_ptr_*, md_data.
TEST_F(SafeArrayContractTest, MultiDimensionalOrderMatchesWindows) {
    const GuestAddr psa = create(3 /*VT_I4*/, {{42, 1}, {2, 23}});
    ASSERT_NE(psa, 0u);
    // Stored reversed: raw[0] = {2,23}, raw[1] = {42,1}.
    EXPECT_EQ(memory.read_u32(psa + 24).value(), 2u);
    EXPECT_EQ(memory.read_u32(psa + 28).value(), 23u);
    EXPECT_EQ(memory.read_u32(psa + 32).value(), 42u);
    EXPECT_EQ(memory.read_u32(psa + 36).value(), 1u);
    const GuestAddr out = kData + 0x300;
    struct B { u32 dim; u32 lhr; u32 lb; u32 uhr; u32 ub; };
    for (const B& b : {B{0, kDispEBadIndex, 0x5555, kDispEBadIndex, 0x5555},
                       B{1, 0, 1, 0, 42}, B{2, 0, 23, 0, 24},
                       B{3, kDispEBadIndex, 0x5555, kDispEBadIndex, 0x5555}}) {
        (void)memory.write_u32(out, 0x5555);
        auto l = mk(psa, b.dim, out);
        EXPECT_EQ(mgr.safe_array_get_lbound(l), b.lhr);
        EXPECT_EQ(memory.read_u32(out).value(), b.lb);
        (void)memory.write_u32(out, 0x5555);
        auto u = mk(psa, b.dim, out);
        EXPECT_EQ(mgr.safe_array_get_ubound(u), b.uhr);
        EXPECT_EQ(memory.read_u32(out).value(), b.ub);
    }
    const GuestAddr pv = memory.read_u64(psa + 16).value();
    const GuestAddr val = kData + 0x310;
    auto put = [&](std::initializer_list<i32> idx, u32 v) {
        (void)memory.write_u32(val, v);
        auto m = mk(psa, indices(idx), val);
        return mgr.safe_array_put_element(m);
    };
    auto ptr = [&](std::initializer_list<i32> idx) -> std::pair<u32, u64> {
        auto m = mk(psa, indices(idx), out);
        const u32 hr = mgr.safe_array_ptr_of_index(m);
        return {hr, memory.read_u64(out).value() - pv};
    };
    EXPECT_EQ(put({3, 23}, 0x1111), 0u);
    EXPECT_EQ(ptr({3, 23}).second, 8u);
    EXPECT_EQ(put({3, 24}, 0x2222), 0u);
    EXPECT_EQ(ptr({3, 24}).second, 0xb0u);
    EXPECT_EQ(put({1, 23}, 0x2222), 0u);
    EXPECT_EQ(put({43, 23}, 0x2222), kDispEBadIndex);
    EXPECT_EQ(ptr({3, 25}).first, kDispEBadIndex);
    auto nullout = mk(psa, indices({3, 23}), 0);
    EXPECT_EQ(mgr.safe_array_ptr_of_index(nullout), kEInvalidArg);
    auto nullpsa = mk(0, indices({3, 23}), out);
    EXPECT_EQ(mgr.safe_array_ptr_of_index(nullpsa), kEInvalidArg);
    EXPECT_EQ(memory.read_u32(pv + 2 * 4).value(), 0x1111u);
    EXPECT_EQ(memory.read_u32(pv + 44 * 4).value(), 0x2222u);
}

// probe: zero_dim, ubound_ovf*, ubound_empty*, cdims17, huge_wrap, big_68mb.
TEST_F(SafeArrayContractTest, BoundsEdgeCases) {
    const GuestAddr z = create(3, {{0, 42}, {5, 1}});
    ASSERT_NE(z, 0u);
    EXPECT_EQ(memory.read_u32(z + 24).value(), 5u);
    EXPECT_EQ(memory.read_u32(z + 28).value(), 1u);
    EXPECT_EQ(memory.read_u32(z + 36).value(), 42u);
    EXPECT_NE(memory.read_u64(z + 16).value(), 0u);
    const GuestAddr out = kData + 0x300;
    auto ub1 = mk(z, 1, out);
    EXPECT_EQ(mgr.safe_array_get_ubound(ub1), 0u);
    EXPECT_EQ(memory.read_u32(out).value(), 0x29u);

    for (auto [c, l] : {std::pair<u32, i32>{2, 0x7FFFFFFF},
                        {0x20, 0x7FFFFFF0}, {0, static_cast<i32>(0x80000000u)}}) {
        const GuestAddr a = create(0x11, {{c, l}});
        ASSERT_NE(a, 0u);
        auto m = mk(a, 1, out);
        EXPECT_EQ(mgr.safe_array_get_ubound(m), kDispEOverflow);
        EXPECT_EQ(memory.read_u32(out).value(), 0xFFFFFFFFu);
    }
    const GuestAddr e = create(0x11, {{0, 5}});
    auto m = mk(e, 1, out);
    EXPECT_EQ(mgr.safe_array_get_ubound(m), 0u);
    EXPECT_EQ(memory.read_u32(out).value(), 4u);

    EXPECT_EQ(create(3, {{0x10000, 0}, {0x10001, 0}}), 0u);
    EXPECT_NE(create(3, {{0x01100000, 0}}), 0u); // 68 MB: no 16 MB cap
    const GuestAddr s17 = create(3, {{1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0},
                                     {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0},
                                     {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}});
    ASSERT_NE(s17, 0u);
    EXPECT_EQ(memory.read_u16(s17).value(), 17u);
    auto c0 = mk(3, 0, kData + 0x100);
    EXPECT_EQ(mgr.safe_array_create(c0), 0u);
    auto cn = mk(3, 1, 0);
    EXPECT_EQ(mgr.safe_array_create(cn), 0u);
}

// probe: destroy_locked, destroy_null, unlock_zero, lock, redim_locked/fixed.
TEST_F(SafeArrayContractTest, LockingRules) {
    const GuestAddr a = create(3, {{2, 0}});
    auto lock = mk(a);
    EXPECT_EQ(mgr.safe_array_lock(lock), 0u);
    auto d = mk(a);
    EXPECT_EQ(mgr.safe_array_destroy(d), kDispEArrayIsLocked);
    EXPECT_EQ(memory.read_u32(a + 8).value(), 1u);
    EXPECT_NE(memory.read_u64(a + 16).value(), 0u);
    const GuestAddr nb = kData + 0x320;
    (void)memory.write_u32(nb, 8);
    (void)memory.write_u32(nb + 4, 2);
    auto rl = mk(a, nb);
    EXPECT_EQ(mgr.safe_array_redim(rl), kDispEArrayIsLocked);
    auto un = mk(a);
    EXPECT_EQ(mgr.safe_array_unlock(un), 0u);
    auto un2 = mk(a);
    EXPECT_EQ(mgr.safe_array_unlock(un2), kEUnexpected);
    (void)memory.write_u16(a + 2, 0x80 | 0x10); // + FADF_FIXEDSIZE
    auto rf = mk(a, nb);
    EXPECT_EQ(mgr.safe_array_redim(rf), kDispEArrayIsLocked);
    (void)memory.write_u16(a + 2, 0x80);
    const GuestAddr data = memory.read_u64(a + 16).value();
    auto d2 = mk(a);
    EXPECT_EQ(mgr.safe_array_destroy(d2), 0u);
    EXPECT_FALSE(task_memory.owns(data));      // data freed
    EXPECT_FALSE(task_memory.owns(a - 16));    // descriptor freed
    auto dn = mk(0);
    EXPECT_EQ(mgr.safe_array_destroy(dn), 0u);
    auto unull = mk(0);
    EXPECT_EQ(mgr.safe_array_unlock(unull), kEInvalidArg);
}

// probe: unk_put, unk_overwrite, unk_get, unk_destroy.
TEST_F(SafeArrayContractTest, InterfaceElementsAreAddRefedAndReleased) {
    const GuestAddr a = make_object(kData + 0x500);
    const GuestAddr b = make_object(kData + 0x520);
    const GuestAddr psa = create(13 /*VT_UNKNOWN*/, {{2, 0}});
    ASSERT_NE(psa, 0u);
    auto put_a = mk(psa, indices({0}), a);
    EXPECT_EQ(mgr.safe_array_put_element(put_a), 0u);
    EXPECT_EQ(addrefs[a], 1);
    EXPECT_EQ(releases[a], 0);
    const GuestAddr pv = memory.read_u64(psa + 16).value();
    EXPECT_EQ(memory.read_u64(pv).value(), a); // the pointer, not *pv
    auto put_b = mk(psa, indices({0}), b);
    EXPECT_EQ(mgr.safe_array_put_element(put_b), 0u);
    EXPECT_EQ(releases[a], 1);
    EXPECT_EQ(addrefs[b], 1);
    const GuestAddr out = kData + 0x300;
    auto get = mk(psa, indices({0}), out);
    EXPECT_EQ(mgr.safe_array_get_element(get), 0u);
    EXPECT_EQ(memory.read_u64(out).value(), b);
    EXPECT_EQ(addrefs[b], 2);
    auto d = mk(psa);
    EXPECT_EQ(mgr.safe_array_destroy(d), 0u);
    EXPECT_EQ(releases[b], 1);
    EXPECT_EQ(releases[a], 1);
    // IID_IUnknown lives in the 16 bytes before the descriptor.
    const GuestAddr disp = create(9 /*VT_DISPATCH*/, {{1, 0}});
    EXPECT_EQ(memory.read_u16(disp + 2).value(), 0x440u);
    EXPECT_EQ(memory.read_u32(disp - 16).value(), 0x00020400u);
}

// probe: var_put_bstr, var_put_unk, var_get_unk, var_destroy.
TEST_F(SafeArrayContractTest, VariantElementsAreDeepCopied) {
    const GuestAddr psa = create(12 /*VT_VARIANT*/, {{2, 0}});
    const GuestAddr v = kData + 0x600;
    const GuestAddr s = bstr(u"hello");
    ASSERT_NE(s, 0u);
    (void)memory.write_u16(v, 8 /*VT_BSTR*/);
    (void)memory.write_u64(v + 8, s);
    auto put = mk(psa, indices({0}), v);
    EXPECT_EQ(mgr.safe_array_put_element(put), 0u);
    const GuestAddr cell = memory.read_u64(psa + 16).value();
    EXPECT_EQ(memory.read_u16(cell).value(), 8u);
    const GuestAddr copy = memory.read_u64(cell + 8).value();
    EXPECT_NE(copy, s);
    EXPECT_EQ(memory.read_u32(copy - 4).value(), 10u);

    const GuestAddr obj = make_object(kData + 0x540);
    (void)memory.write_u16(v, 13 /*VT_UNKNOWN*/);
    (void)memory.write_u64(v + 8, obj);
    auto put_u = mk(psa, indices({1}), v);
    EXPECT_EQ(mgr.safe_array_put_element(put_u), 0u);
    EXPECT_EQ(addrefs[obj], 1);
    const GuestAddr g = kData + 0x640;
    auto get = mk(psa, indices({1}), g);
    EXPECT_EQ(mgr.safe_array_get_element(get), 0u);
    EXPECT_EQ(memory.read_u16(g).value(), 13u);
    EXPECT_EQ(addrefs[obj], 2);
    // Overwriting the BSTR cell frees the old copy.
    (void)memory.write_u16(v, 3 /*VT_I4*/);
    auto put_i = mk(psa, indices({0}), v);
    EXPECT_EQ(mgr.safe_array_put_element(put_i), 0u);
    EXPECT_FALSE(task_memory.owns(copy - 8));
    auto d = mk(psa);
    EXPECT_EQ(mgr.safe_array_destroy(d), 0u);
    EXPECT_EQ(releases[obj], 1);
}

// probe: redim, redim2d, vector*, vartype_nohave_*, copy_bstr, copy_null.
TEST_F(SafeArrayContractTest, RedimVectorVartypeAndCopy) {
    const GuestAddr a = create(3, {{3, 0}, {4, 10}});
    const GuestAddr val = kData + 0x310;
    (void)memory.write_u32(val, 0x77);
    auto put = mk(a, indices({2, 11}), val);
    EXPECT_EQ(mgr.safe_array_put_element(put), 0u);
    const GuestAddr nb = kData + 0x320;
    (void)memory.write_u32(nb, 6);
    (void)memory.write_u32(nb + 4, 20);
    auto r = mk(a, nb);
    EXPECT_EQ(mgr.safe_array_redim(r), 0u);
    const GuestAddr out = kData + 0x300;
    auto l1 = mk(a, 1, out);  (void)mgr.safe_array_get_lbound(l1);
    EXPECT_EQ(memory.read_u32(out).value(), 0u);
    auto u1 = mk(a, 1, out);  (void)mgr.safe_array_get_ubound(u1);
    EXPECT_EQ(memory.read_u32(out).value(), 2u);
    auto l2 = mk(a, 2, out);  (void)mgr.safe_array_get_lbound(l2);
    EXPECT_EQ(memory.read_u32(out).value(), 0x14u);
    auto u2 = mk(a, 2, out);  (void)mgr.safe_array_get_ubound(u2);
    EXPECT_EQ(memory.read_u32(out).value(), 0x19u);
    EXPECT_EQ(memory.read_u32(memory.read_u64(a + 16).value() + 20).value(), 0x77u);

    (void)memory.write_u16(a + 2, 0); // drop FADF_HAVEVARTYPE
    auto gv = mk(a, out);
    EXPECT_EQ(mgr.safe_array_get_vartype(gv), kEInvalidArg);

    auto vm = mk();
    const GuestAddr vec = mgr.sa_create_vector(vm, 3, 5, 3);
    ASSERT_NE(vec, 0u);
    EXPECT_EQ(memory.read_u16(vec + 2).value(), 0x2080u);
    EXPECT_EQ(memory.read_u64(vec + 16).value(), vec + 0x20);
    EXPECT_EQ(mgr.sa_create_vector(vm, 0, 0, 1), 0u);

    const GuestAddr src = create(8, {{3, 0}});
    const GuestAddr s = bstr(u"xy");
    auto pb = mk(src, indices({1}), s);
    EXPECT_EQ(mgr.safe_array_put_element(pb), 0u);
    const GuestAddr pout = kData + 0x330;
    auto cp = mk(src, pout);
    EXPECT_EQ(mgr.safe_array_copy(cp), 0u);
    const GuestAddr dst = memory.read_u64(pout).value();
    ASSERT_NE(dst, 0u);
    EXPECT_EQ(memory.read_u16(dst + 2).value(), 0x180u);
    EXPECT_EQ(memory.read_u32(dst - 4).value(), 8u);
    const GuestAddr sd = memory.read_u64(src + 16).value();
    const GuestAddr dd = memory.read_u64(dst + 16).value();
    EXPECT_NE(memory.read_u64(dd + 8).value(), memory.read_u64(sd + 8).value());
    EXPECT_EQ(memory.read_u64(dd).value(), 0u);
    (void)memory.write_u64(pout, 0x1234);
    auto cn = mk(0, pout);
    EXPECT_EQ(mgr.safe_array_copy(cn), 0u);
    EXPECT_EQ(memory.read_u64(pout).value(), 0u);
}

// probe: bstr_align, bstr_huge*, bstr_bytelen, bstr_odd_roundtrip, bstr_18mb.
TEST_F(SafeArrayContractTest, BstrAllocationContract) {
    const GuestAddr s = bstr(u"abc");
    ASSERT_NE(s, 0u);
    EXPECT_EQ(s & 7u, 0u); // Win64 BSTRs are 8-byte aligned
    EXPECT_EQ(memory.read_u32(s - 4).value(), 6u);
    EXPECT_TRUE(task_memory.owns(s - 8));
    auto f = mk();
    mgr.sys_free_string(f, s);
    EXPECT_FALSE(task_memory.owns(s - 8)); // really freed
    auto h = mk();
    EXPECT_EQ(mgr.sys_alloc_string_len(h, 0, 0x7FFFFFFFu), 0u);
    EXPECT_EQ(mgr.sys_alloc_string_len(h, 0, 0xFFFFFFF0u), 0u);
    const GuestAddr big = mgr.sys_alloc_string_len(h, 0, 9 * 1024 * 1024);
    ASSERT_NE(big, 0u);
    EXPECT_EQ(mgr.sys_string_len(h, big), 9u * 1024 * 1024);

    const GuestAddr src = kData + 0x700;
    (void)memory.write_bytes(src, reinterpret_cast<const Byte*>("abcde"), 5);
    const GuestAddr bl = mgr.sys_alloc_string_byte_len(h, src, 5);
    EXPECT_EQ(mgr.sys_string_byte_len(h, bl), 5u);
    EXPECT_EQ(mgr.sys_string_len(h, bl), 2u);
    EXPECT_EQ(memory.read_u8(bl + 4).value(), 'e');
    EXPECT_EQ(memory.read_u8(bl + 5).value(), 0u);
    EXPECT_EQ(memory.read_u8(bl + 6).value(), 0u);
    const GuestAddr arr = create(8, {{1, 0}});
    auto put = mk(arr, indices({0}), bl);
    EXPECT_EQ(mgr.safe_array_put_element(put), 0u);
    const GuestAddr out = kData + 0x300;
    auto get = mk(arr, indices({0}), out);
    EXPECT_EQ(mgr.safe_array_get_element(get), 0u);
    EXPECT_EQ(memory.read_u32(memory.read_u64(out).value() - 4).value(), 5u);

    // SysReAllocStringLen writes the full 64-bit BSTR back.
    const GuestAddr pb = kData + 0x340;
    (void)memory.write_u64(pb, 0xFFFFFFFF00000000ull);
    (void)memory.write_u64(pb, 0);
    auto re = mk(pb, src, 2);
    EXPECT_EQ(mgr.sys_realloc_string_len(re), 1u);
    const GuestAddr nb = memory.read_u64(pb).value();
    EXPECT_TRUE(task_memory.owns(nb - 8));
}

TEST_F(SafeArrayContractTest, OrdinalsFromWindowsExportTable) {
    // dumpbin /exports C:\Windows\System32\oleaut32.dll (Windows 10).
    DllExports exports = create_oleaut32_thunks(mgr);
    auto find = [&](u16 ord) -> const ExportEntry* {
        for (const auto& e : exports.entries) {
            if (e.ordinal == ord) return &e;
        }
        return nullptr;
    };
    ASSERT_NE(find(150), nullptr);
    EXPECT_EQ(find(150)->name, "SysAllocStringByteLen");
    EXPECT_EQ(find(150)->stdcall_bytes, 8u);
    ASSERT_NE(find(148), nullptr);
    EXPECT_EQ(find(148)->name, "SafeArrayPtrOfIndex");
    EXPECT_EQ(find(148)->stdcall_bytes, 12u);
    ASSERT_NE(find(4), nullptr);
    EXPECT_EQ(find(4)->name, "SysAllocStringLen");
}

} // namespace
