// SPDX-License-Identifier: LGPL-2.1-only
//
// Class linkage, fence flags, DeviceContextState and swap-chain buffer references.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

namespace classlink_blobs {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../oracle_data/d3d11_classlink_shaders.h"
#pragma clang diagnostic pop
}  // namespace classlink_blobs

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// [cl] and refmodel [cl]: the linkage and its instances as Windows makes
// them -- ids by first request, names and descriptors, the name-buffer rule,
// and the runtime reference an instance keeps on its linkage.
TEST_F(D3D11ManagerTest, ClassLinkageInstancesFollowTheMeasuredRules) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr LINKAGE_OUT = DATA_BASE + 0x1800;
    constexpr GuestAddr INSTANCE_OUT = DATA_BASE + 0x1808;
    constexpr GuestAddr NAME = DATA_BASE + 0x1810;
    constexpr GuestAddr DESC = DATA_BASE + 0x1840;
    constexpr GuestAddr TEXT = DATA_BASE + 0x1880;
    constexpr GuestAddr LENGTH = DATA_BASE + 0x18C0;
    const u32 device_before = count_of(d3d11, marshaler, cpu, device,
                                       &D3D11Manager::impl_DeviceMethod);
    set_arg(0, device);
    set_arg(1, LINKAGE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 19);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr linkage = memory.read_u64(LINKAGE_OUT).value();
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, device,
                       &D3D11Manager::impl_DeviceMethod), device_before + 1);

    const auto get_instance = [&](const char* name, u32 index) {
        EXPECT_TRUE(memory.write_bytes(NAME, reinterpret_cast<const Byte*>(name),
                                       std::strlen(name) + 1).is_ok());
        EXPECT_TRUE(memory.write_u64(INSTANCE_OUT, 0xA5A5).is_ok());
        set_arg(0, linkage);
        set_arg(1, NAME);
        set_arg(2, index);
        set_arg(3, INSTANCE_OUT);
        d3d11.impl_ClassLinkageMethod(marshaler, 7);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << name;
        return memory.read_u64(INSTANCE_OUT).value();
    };
    const auto desc_of = [&](GuestAddr instance) {
        std::array<u32, 8> words{};
        set_arg(0, instance);
        set_arg(1, DESC);
        d3d11.impl_ClassInstanceMethod(marshaler, 8);
        EXPECT_TRUE(memory.read_bytes(
            DESC, reinterpret_cast<Byte*>(words.data()), sizeof(words)).is_ok());
        return words;
    };
    const auto name_of = [&](GuestAddr instance, usize slot, u64 capacity,
                             bool buffer) {
        EXPECT_TRUE(memory.write_u64(LENGTH, capacity).is_ok());
        std::array<u8, 8> fill{};
        fill.fill(0x5A);
        EXPECT_TRUE(memory.write_bytes(TEXT, fill.data(), fill.size()).is_ok());
        set_arg(0, instance);
        set_arg(1, buffer ? TEXT : 0);
        set_arg(2, LENGTH);
        d3d11.impl_ClassInstanceMethod(marshaler, slot);
        std::array<u8, 8> bytes{};
        EXPECT_TRUE(memory.read_bytes(TEXT, bytes.data(), bytes.size()).is_ok());
        return std::pair{memory.read_u64(LENGTH).value(), bytes};
    };

    // InstanceId numbers names in first-request order; InstanceIndex is
    // the argument.
    const std::array<std::pair<const char*, u32>, 5> order{{
        {"nope", 0}, {"g_green", 1}, {"g_red", 2}, {"nope", 0},
        {"g_abstract", 3}}};
    std::vector<GuestAddr> instances;
    for (u32 i = 0; i < order.size(); ++i) {
        const GuestAddr instance = get_instance(order[i].first, i);
        ASSERT_NE(instance, 0u);
        EXPECT_EQ(std::count(instances.begin(), instances.end(), instance), 0)
            << "every call makes a new object";
        instances.push_back(instance);
        EXPECT_EQ(desc_of(instance),
                  (std::array<u32, 8>{order[i].second, i, 0, 0, 0, 0, 0, 0}))
            << order[i].first;
        // Instance name: strlen + 1 from the size query; type name empty.
        EXPECT_EQ(name_of(instance, 9, 0, false).first,
                  std::strlen(order[i].first) + 1);
        EXPECT_EQ(name_of(instance, 10, 32, true).first, 1u);
    }
    for (const GuestAddr instance : instances) {
        set_arg(0, instance);
        d3d11.impl_ClassInstanceMethod(marshaler, 2);
        EXPECT_EQ(cpu.gpr.rax, 0u);
    }

    // CreateClassInstance echoes the offsets with Created TRUE; only the type
    // name is known. Into three bytes GetTypeName copies "CR\0" and reports
    // the whole length, 5.
    ASSERT_TRUE(memory.write_bytes(NAME, reinterpret_cast<const Byte*>("CRed"),
                                   5).is_ok());
    set_arg(0, linkage);
    set_arg(1, NAME);
    set_arg(2, 5);
    set_arg(3, 6);
    set_arg(4, 7);
    set_arg(5, 8);
    set_arg(6, INSTANCE_OUT);
    d3d11.impl_ClassLinkageMethod(marshaler, 8);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr created = memory.read_u64(INSTANCE_OUT).value();
    EXPECT_EQ(desc_of(created), (std::array<u32, 8>{0, 0, 0, 5, 6, 7, 8, 1}));
    const auto small = name_of(created, 10, 3, true);
    EXPECT_EQ(small.first, 5u);
    EXPECT_EQ(small.second[0], 'C');
    EXPECT_EQ(small.second[1], 'R');
    EXPECT_EQ(small.second[2], 0u);
    EXPECT_EQ(small.second[3], 0x5Au);
    EXPECT_EQ(name_of(created, 9, 32, true).first, 1u);

    // The instance keeps its linkage: the application's last Release answers
    // 0, and GetClassLinkage hands it back with a count of 1.
    set_arg(0, linkage);
    d3d11.impl_ClassLinkageMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    set_arg(0, created);
    set_arg(1, LINKAGE_OUT);
    d3d11.impl_ClassInstanceMethod(marshaler, 7);
    EXPECT_EQ(memory.read_u64(LINKAGE_OUT).value(), linkage);
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, linkage,
                       &D3D11Manager::impl_ClassLinkageMethod), 1u);

    // A NULL name or out-parameter is E_INVALIDARG, the out left untouched.
    ASSERT_TRUE(memory.write_u64(INSTANCE_OUT, 0xA5A5).is_ok());
    set_arg(0, linkage);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, INSTANCE_OUT);
    d3d11.impl_ClassLinkageMethod(marshaler, 7);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(INSTANCE_OUT).value(), 0xA5A5u);
    set_arg(0, linkage);
    set_arg(1, NAME);
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ClassLinkageMethod(marshaler, 7);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    // Last references: the instance, then the linkage it kept.
    set_arg(0, created);
    d3d11.impl_ClassInstanceMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    set_arg(0, linkage);
    d3d11.impl_ClassLinkageMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_FALSE(
        kept_alive_by_runtime(linkage, &D3D11Manager::impl_ClassLinkageMethod));
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, device,
                       &D3D11Manager::impl_DeviceMethod), device_before);
}

// d3d11_classlink_probe [desc]: a linkage numbers every class-type and
// instance name it sees -- the ones a shader created with it declares (its
// IFCE chunk, in chunk order) and the ones GetClassInstance /
// CreateClassInstance ask for -- in first-seen order. Objects made earlier
// keep their numbers; a GetClassInstance descriptor stays zero but for the
// ids.
TEST_F(D3D11ManagerTest, ClassLinkageNumbersTheNamesItsShadersDeclare) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr LINKAGE_OUT = DATA_BASE + 0x1800;
    constexpr GuestAddr OUT = DATA_BASE + 0x1808;
    constexpr GuestAddr NAME = DATA_BASE + 0x1810;
    constexpr GuestAddr DESC = DATA_BASE + 0x1840;
    // The shader blobs are larger than the fixture's data page.
    constexpr GuestAddr BYTECODE = 0x100000;
    ASSERT_TRUE(memory.map_region(BYTECODE, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    set_arg(0, device);
    set_arg(1, LINKAGE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 19);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr linkage = memory.read_u64(LINKAGE_OUT).value();

    const auto instance = [&](const char* name, bool create) {
        EXPECT_TRUE(memory.write_bytes(NAME, reinterpret_cast<const Byte*>(name),
                                       std::strlen(name) + 1).is_ok());
        set_arg(0, linkage);
        set_arg(1, NAME);
        set_arg(2, 0);
        if (create) {
            set_arg(3, 0);
            set_arg(4, 0);
            set_arg(5, 0);
            set_arg(6, OUT);
        } else {
            set_arg(3, OUT);
        }
        d3d11.impl_ClassLinkageMethod(marshaler, create ? 8 : 7);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << name;
        return memory.read_u64(OUT).value();
    };
    const auto desc_of = [&](GuestAddr object) {
        std::array<u32, 8> words{};
        set_arg(0, object);
        set_arg(1, DESC);
        d3d11.impl_ClassInstanceMethod(marshaler, 8);
        EXPECT_TRUE(memory.read_bytes(
            DESC, reinterpret_cast<Byte*>(words.data()), sizeof(words)).is_ok());
        return words;
    };
    // InstanceId, InstanceIndex, TypeId, CB, offset, texture, sampler, Created.
    using Desc = std::array<u32, 8>;
    const GuestAddr early_get = instance("g_mul", false);
    const GuestAddr early_new = instance("MulK", true);
    const GuestAddr early_odd = instance("Zzz", true);
    EXPECT_EQ(desc_of(early_get), (Desc{0, 0, 0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(desc_of(early_new), (Desc{0, 0, 0, 0, 0, 0, 0, 1}));
    EXPECT_EQ(desc_of(early_odd), (Desc{0, 0, 1, 0, 0, 0, 0, 1}));

    // cs_dyn declares the types SampOp, BufOp, MulK, AddK, XorOp and the
    // instances g_buf, g_samp, g_adds, g_add0, g_mul, g_add1.
    ASSERT_TRUE(memory.write_bytes(BYTECODE, classlink_blobs::k_cs_dyn,
                                   sizeof(classlink_blobs::k_cs_dyn)).is_ok());
    set_arg(0, device);
    set_arg(1, BYTECODE);
    set_arg(2, sizeof(classlink_blobs::k_cs_dyn));
    set_arg(3, linkage);
    set_arg(4, OUT);
    d3d11.impl_DeviceMethod(marshaler, 18);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr shader = memory.read_u64(OUT).value();

    EXPECT_EQ(desc_of(early_get), (Desc{0, 0, 0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(desc_of(early_new), (Desc{0, 0, 0, 0, 0, 0, 0, 1}));
    std::vector<GuestAddr> made = {early_get, early_new, early_odd};
    made.push_back(instance("g_mul", false));
    EXPECT_EQ(desc_of(made.back()), (Desc{0, 0, 0, 0, 0, 0, 0, 0}));
    made.push_back(instance("g_buf", false));
    EXPECT_EQ(desc_of(made.back()), (Desc{1, 0, 0, 0, 0, 0, 0, 0}));
    made.push_back(instance("MulK", true));
    EXPECT_EQ(desc_of(made.back()), (Desc{0, 0, 0, 0, 0, 0, 0, 1}));
    made.push_back(instance("XorOp", true));
    EXPECT_EQ(desc_of(made.back()), (Desc{0, 0, 5, 0, 0, 0, 0, 1}));
    made.push_back(instance("Zzz", true));
    EXPECT_EQ(desc_of(made.back()), (Desc{0, 0, 1, 0, 0, 0, 0, 1}));

    for (const GuestAddr object : made) {
        set_arg(0, object);
        d3d11.impl_ClassInstanceMethod(marshaler, 2);
    }
    set_arg(0, shader);
    d3d11.impl_ComputeShaderMethod(marshaler, 2);
    set_arg(0, linkage);
    d3d11.impl_ClassLinkageMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// d3d11_classlink_probe [get] and [bad] (Learn *SetShader: "Each interface
// used by a shader must have a corresponding class instance or the shader
// will get disabled"): cs_multi has six interface slots. Bound with six
// matching instances it is returned by *GetShader with them -- as many as
// the caller's capacity, the count written back, the rest of the array
// untouched, the bound count when the array is NULL -- and every one it
// returns gains a reference while the binding itself adds none the
// application can see. Too few, too many, a NULL entry, a class of the
// wrong interface, an unknown class, an instance name the shader does not
// declare, and instances given to a shader without interfaces all bind no
// shader.
TEST_F(D3D11ManagerTest, SetShaderBindsAnInterfaceShaderOnlyWithItsInstances) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr LINKAGE_OUT = DATA_BASE + 0x1800;
    constexpr GuestAddr OUT = DATA_BASE + 0x1808;
    constexpr GuestAddr NAME = DATA_BASE + 0x1810;
    constexpr GuestAddr SET = DATA_BASE + 0x1840;     // 8 pointers
    constexpr GuestAddr GOT = DATA_BASE + 0x1880;     // 10 pointers
    constexpr GuestAddr COUNT = DATA_BASE + 0x18E0;
    // The shader blobs are larger than the fixture's data page.
    constexpr GuestAddr BYTECODE = 0x100000;
    ASSERT_TRUE(memory.map_region(BYTECODE, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    set_arg(0, device);
    set_arg(1, LINKAGE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 19);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr linkage = memory.read_u64(LINKAGE_OUT).value();

    const auto create_cs = [&](const unsigned char* code, usize size) {
        EXPECT_TRUE(memory.write_bytes(BYTECODE, code, size).is_ok());
        set_arg(0, device);
        set_arg(1, BYTECODE);
        set_arg(2, size);
        set_arg(3, linkage);
        set_arg(4, OUT);
        d3d11.impl_DeviceMethod(marshaler, 18);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(OUT).value();
    };
    const GuestAddr multi = create_cs(classlink_blobs::k_cs_multi,
                                      sizeof(classlink_blobs::k_cs_multi));
    const GuestAddr plain = create_cs(classlink_blobs::k_cs_plain,
                                      sizeof(classlink_blobs::k_cs_plain));
    const auto instance = [&](const char* name, bool create) {
        EXPECT_TRUE(memory.write_bytes(NAME, reinterpret_cast<const Byte*>(name),
                                       std::strlen(name) + 1).is_ok());
        set_arg(0, linkage);
        set_arg(1, NAME);
        set_arg(2, 0);
        if (create) {
            set_arg(3, 0);
            set_arg(4, 0);
            set_arg(5, 0);
            set_arg(6, OUT);
        } else {
            set_arg(3, OUT);
        }
        d3d11.impl_ClassLinkageMethod(marshaler, create ? 8 : 7);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << name;
        return memory.read_u64(OUT).value();
    };
    const GuestAddr xor_op = instance("XorOp", true);
    const GuestAddr unknown = instance("NoSuchClass", true);
    const GuestAddr add0 = instance("g_add0", false);
    const GuestAddr mul = instance("g_mul", false);
    const GuestAddr add1 = instance("g_add1", false);
    const GuestAddr buf = instance("g_buf", false);
    const GuestAddr shl = instance("g_shl", false);
    const GuestAddr g_xor = instance("g_xor", false);  // declares no data
    const std::vector<GuestAddr> good = {xor_op, add0, mul, add1, buf, shl};

    const auto set = [&](GuestAddr shader, std::vector<GuestAddr> list,
                         bool with_array = true) {
        for (usize i = 0; i < list.size(); ++i) {
            EXPECT_TRUE(memory.write_u64(SET + i * 8, list[i]).is_ok());
        }
        set_arg(0, context);
        set_arg(1, shader);
        set_arg(2, with_array ? SET : 0);
        set_arg(3, list.size());
        d3d11.impl_ContextMethod(marshaler, 69); // CSSetShader
    };
    struct Got {
        GuestAddr shader;
        u32 count;
        std::vector<GuestAddr> entries;  // 0x5A5A5A50 = untouched
    };
    const auto get = [&](u32 capacity, bool with_array) {
        for (u32 i = 0; i < 10; ++i) {
            EXPECT_TRUE(memory.write_u64(GOT + i * 8, 0x5A5A5A50u).is_ok());
        }
        EXPECT_TRUE(memory.write_u32(COUNT, capacity).is_ok());
        EXPECT_TRUE(memory.write_u64(OUT, 0xA5A5).is_ok());
        set_arg(0, context);
        set_arg(1, OUT);
        set_arg(2, with_array ? GOT : 0);
        set_arg(3, COUNT);
        d3d11.impl_ContextMethod(marshaler, 107); // CSGetShader
        Got got{memory.read_u64(OUT).value(), memory.read_u32(COUNT).value(),
                {}};
        for (u32 i = 0; i < 8; ++i) {
            got.entries.push_back(memory.read_u64(GOT + i * 8).value());
        }
        // Drop the references the call handed out.
        if (got.shader != 0) {
            set_arg(0, got.shader);
            d3d11.impl_ComputeShaderMethod(marshaler, 2);
        }
        for (const GuestAddr entry : got.entries) {
            if (entry == 0x5A5A5A50u || entry == 0) continue;
            set_arg(0, entry);
            d3d11.impl_ClassInstanceMethod(marshaler, 2);
        }
        return got;
    };
    const auto refs = [&](GuestAddr object) {
        return count_of(d3d11, marshaler, cpu, object,
                        &D3D11Manager::impl_ClassInstanceMethod);
    };
    constexpr GuestAddr kUntouched = 0x5A5A5A50u;

    EXPECT_EQ(refs(xor_op), 1u);
    set(multi, good);
    EXPECT_EQ(refs(xor_op), 1u);  // "refs bound: xor=1 add0=1"
    EXPECT_EQ(refs(add0), 1u);
    {
        const Got got = get(8, true);
        EXPECT_EQ(got.shader, multi);
        EXPECT_EQ(got.count, 6u);
        for (u32 i = 0; i < 6; ++i) EXPECT_EQ(got.entries[i], good[i]) << i;
        EXPECT_EQ(got.entries[6], kUntouched);
        EXPECT_EQ(got.entries[7], kUntouched);
    }
    {
        const Got got = get(2, true);
        EXPECT_EQ(got.count, 2u);
        EXPECT_EQ(got.entries[0], good[0]);
        EXPECT_EQ(got.entries[1], good[1]);
        EXPECT_EQ(got.entries[2], kUntouched);
    }
    {
        const Got got = get(0, true);
        EXPECT_EQ(got.shader, multi);
        EXPECT_EQ(got.count, 0u);
        EXPECT_EQ(got.entries[0], kUntouched);
    }
    EXPECT_EQ(get(8, false).count, 6u);  // "noarray: n=6"
    {
        // "refs after get: xor=2": a returned instance is the caller's.
        EXPECT_TRUE(memory.write_u32(COUNT, 8).is_ok());
        set_arg(0, context);
        set_arg(1, 0);
        set_arg(2, GOT);
        set_arg(3, COUNT);
        d3d11.impl_ContextMethod(marshaler, 107);
        EXPECT_EQ(refs(xor_op), 2u);
        for (u32 i = 0; i < 6; ++i) {
            set_arg(0, memory.read_u64(GOT + i * 8).value());
            d3d11.impl_ClassInstanceMethod(marshaler, 2);
        }
        EXPECT_EQ(refs(xor_op), 1u);
    }

    const auto expect_disabled = [&](const char* what) {
        const Got got = get(8, true);
        EXPECT_EQ(got.shader, 0u) << what;
        EXPECT_EQ(got.count, 0u) << what;
        EXPECT_EQ(got.entries[0], kUntouched) << what;
    };
    set(multi, {}, false);
    expect_disabled("bad_none");
    set(multi, {xor_op, add0, mul});
    expect_disabled("bad_three");
    set(multi, {xor_op, add0, mul, add1, buf, add0});
    expect_disabled("bad_wrong_iface");
    set(multi, {xor_op, add0, 0, add1, buf, shl});
    expect_disabled("bad_null_entry");
    set(multi, {unknown, add0, mul, add1, buf, shl});
    expect_disabled("bad_unknown_class");
    set(multi, {xor_op, add0, mul, add1, buf, shl, add0});
    expect_disabled("bad_seven");
    set(multi, {g_xor, add0, mul, add1, buf, shl});
    expect_disabled("multi_e (g_xor by name)");
    set(plain, good);
    expect_disabled("plain");
    set(plain, {}, false);
    EXPECT_EQ(get(8, true).shader, plain);
    set(multi, good);
    EXPECT_EQ(get(8, true).count, 6u);  // "good_again"
    set_arg(0, context);
    d3d11.impl_ContextMethod(marshaler, 110); // ClearState
    expect_disabled("cleared");
    EXPECT_EQ(refs(xor_op), 1u);  // "refs cleared: xor=1"

    for (const GuestAddr object :
         {xor_op, unknown, add0, mul, add1, buf, shl, g_xor}) {
        set_arg(0, object);
        d3d11.impl_ClassInstanceMethod(marshaler, 2);
        EXPECT_EQ(cpu.gpr.rax, 0u);
    }
    for (const GuestAddr shader : {multi, plain}) {
        set_arg(0, shader);
        d3d11.impl_ComputeShaderMethod(marshaler, 2);
        EXPECT_EQ(cpu.gpr.rax, 0u);
    }
    set_arg(0, linkage);
    d3d11.impl_ClassLinkageMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// [fence]: CreateFence's flag, out-parameter and CreateSharedHandle rules.
TEST_F(D3D11ManagerTest, CreateFenceFlagsFollowWindows) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr FENCE_OUT = DATA_BASE + 0x1840;
    constexpr GuestAddr HANDLE_OUT = DATA_BASE + 0x1848;
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(winapi::IID_ID3D11FENCE.data()),
        16).is_ok());
    const auto create = [&](u32 flags, GuestAddr out) {
        if (out != 0) EXPECT_TRUE(memory.write_u64(out, 0xA5A5).is_ok());
        set_arg(0, device);
        set_arg(1, 3);
        set_arg(2, flags);
        set_arg(3, IID_ADDRESS);
        set_arg(4, out);
        d3d11.impl_DeviceMethod(marshaler, 68);
        return static_cast<u32>(cpu.gpr.rax);
    };
    ASSERT_EQ(create(0x1, FENCE_OUT), d3d11::S_OK);  // 0x1 is accepted
    const GuestAddr fence = memory.read_u64(FENCE_OUT).value();
    for (const u32 flags : {0x4u, 0x8u, 0x10u}) {
        EXPECT_EQ(create(flags, FENCE_OUT), d3d11::E_INVALIDARG) << flags;
        EXPECT_EQ(memory.read_u64(FENCE_OUT).value(), 0u) << flags;
    }
    EXPECT_EQ(create(0, 0), 1u);  // NULL ppFence: S_FALSE
    // Unshared: E_INVALIDARG, the handle untouched.
    ASSERT_TRUE(memory.write_u64(HANDLE_OUT, 0xA5A5).is_ok());
    set_arg(0, fence);
    set_arg(1, 0);
    set_arg(2, 0x10000000u);
    set_arg(3, 0);
    set_arg(4, HANDLE_OUT);
    d3d11.impl_FenceMethod(marshaler, 7);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(HANDLE_OUT).value(), 0xA5A5u);
    set_arg(0, fence);
    d3d11.impl_FenceMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// [dcs]: CreateDeviceContextState's validation, and activation is the
// context's reference, not the application's.
TEST_F(D3D11ManagerTest, DeviceContextStateValidatesAndActivationIsInternal) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr CHOSEN = DATA_BASE + 0x1800;
    constexpr GuestAddr STATE_OUT = DATA_BASE + 0x1808;
    constexpr GuestAddr PREVIOUS = DATA_BASE + 0x1810;
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(winapi::IID_ID3D11DEVICE1.data()),
        16).is_ok());
    const auto create = [&](u32 flags, u32 level) {
        EXPECT_TRUE(memory.write_u32(LEVELS, level).is_ok());
        EXPECT_TRUE(memory.write_u32(CHOSEN, 0xABABu).is_ok());
        set_arg(0, device);
        set_arg(1, flags);
        set_arg(2, LEVELS);
        set_arg(3, 1);
        set_arg(4, d3d11::SDK_VERSION);
        set_arg(5, IID_ADDRESS);
        set_arg(6, CHOSEN);
        set_arg(7, STATE_OUT);
        d3d11.impl_DeviceMethod(marshaler, 47);
        return std::pair{static_cast<u32>(cpu.gpr.rax),
                         memory.read_u32(CHOSEN).value()};
    };
    // SINGLETHREADED on a free-threaded device, and a level that is no
    // D3D_FEATURE_LEVEL: E_INVALIDARG with *pChosenFeatureLevel 0.
    EXPECT_EQ(create(1, d3d11::FEATURE_LEVEL_11_0),
              (std::pair{d3d11::E_INVALIDARG, 0u}));
    EXPECT_EQ(create(0, 0x1234), (std::pair{d3d11::E_INVALIDARG, 0u}));
    // An undefined flag bit is ignored.
    ASSERT_EQ(create(2, d3d11::FEATURE_LEVEL_11_0),
              (std::pair{d3d11::S_OK, d3d11::FEATURE_LEVEL_11_0}));
    const GuestAddr state = memory.read_u64(STATE_OUT).value();

    set_arg(0, context);
    set_arg(1, state);
    set_arg(2, PREVIOUS);
    d3d11.impl_ContextMethod(marshaler, 131);  // SwapDeviceContextState
    const GuestAddr original = memory.read_u64(PREVIOUS).value();
    ASSERT_NE(original, 0u);
    // Measured "swap in: state count 1->1".
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, state,
                       &D3D11Manager::impl_DeviceContextStateMethod), 1u);
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, original,
                       &D3D11Manager::impl_DeviceContextStateMethod), 1u);
    // Swapping back hands the application a reference of its own: 2.
    set_arg(0, context);
    set_arg(1, original);
    set_arg(2, PREVIOUS);
    d3d11.impl_ContextMethod(marshaler, 131);
    EXPECT_EQ(memory.read_u64(PREVIOUS).value(), state);
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, state,
                       &D3D11Manager::impl_DeviceContextStateMethod), 2u);
}

// refmodel [swap] (measured in an interactive session, and matched by the x86
// probe under Vitreo): a swap chain's buffer is the chain's own; GetBuffer's
// reference counts on the CHAIN, not the device, and the chain outlives the
// application's last chain reference while a buffer reference is held.
TEST_F(D3D11ManagerTest, SwapChainBufferReferencesCountOnTheChain) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_IDXGIFACTORY1),
        sizeof(IID_IDXGIFACTORY1)).is_ok());
    set_arg(0, IID_ADDRESS);
    set_arg(1, FACTORY_OUT);
    dxgi.impl_CreateDXGIFactory1(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    const GuestAddr factory = memory.read_u64(FACTORY_OUT).value();
    // 64x64 B8G8R8A8, DISCARD, 1 buffer, windowed.
    const std::array<u32, 11> prefix{64, 64, 60, 1, 87, 0, 0, 1, 0, 0x20, 1};
    ASSERT_TRUE(memory.write_bytes(
        SWAP_DESC, reinterpret_cast<const Byte*>(prefix.data()),
        sizeof(prefix)).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 44, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(SWAP_DESC + 48, 0xCAFE).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 56, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 60, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 64, 0).is_ok());
    const u32 device_before = count_of(d3d11, marshaler, cpu, device,
                                       &D3D11Manager::impl_DeviceMethod);
    set_arg(0, factory);
    set_arg(1, device);
    set_arg(2, SWAP_DESC);
    set_arg(3, SWAP_OUT);
    dxgi.impl_CreateSwapChain(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr chain = memory.read_u64(SWAP_OUT).value();
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, device,
                       &D3D11Manager::impl_DeviceMethod), device_before + 1);
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, chain,
                       &D3D11Manager::impl_SwapChainMethod), 1u);

    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&winapi::IID_ID3D11TEXTURE2D),
        sizeof(winapi::IID_ID3D11TEXTURE2D)).is_ok());
    GuestAddr buffers[2]{};
    for (GuestAddr& buffer : buffers) {
        set_arg(0, chain);
        set_arg(1, 0);
        set_arg(2, IID_ADDRESS);
        set_arg(3, BUFFER_OUT);
        d3d11.impl_SwapChainMethod(marshaler, 9);  // GetBuffer(0)
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        buffer = memory.read_u64(BUFFER_OUT).value();
    }
    EXPECT_EQ(buffers[0], buffers[1]);
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, buffers[0],
                       &D3D11Manager::impl_Texture2DMethod), 2u);
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, chain,
                       &D3D11Manager::impl_SwapChainMethod), 2u);
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, device,
                       &D3D11Manager::impl_DeviceMethod), device_before + 1);

    set_arg(0, buffers[1]);
    d3d11.impl_Texture2DMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    set_arg(0, chain);
    d3d11.impl_SwapChainMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 1u) << "the buffer reference holds the chain";
    set_arg(0, buffers[0]);
    d3d11.impl_Texture2DMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_FALSE(kept_alive_by_runtime(buffers[0],
                                       &D3D11Manager::impl_Texture2DMethod));
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, device,
                       &D3D11Manager::impl_DeviceMethod), device_before);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
