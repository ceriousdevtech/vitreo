// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_rtti.cpp
/// @brief MSVC RTTI entry points (`__RTDynamicCast`, `__RTtypeid`,
///        `__RTCastToVoid`, `__AdjustPointer`).
///
/// Builds a real MSVC RTTI graph in guest memory for
///
///     struct B1 { virtual ~B1(); };
///     struct B2 { virtual ~B2(); };
///     struct Derived : B1, B2 { };
///
/// laid out the way MSVC lays it out: `B1` shares `Derived`'s vfptr at offset
/// 0, `B2` has its own vfptr at offset 8, and each vftable carries a complete
/// object locator at `vftable[-1]` whose `offset` says where that vfptr sits.

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/msvcrt/runtime/rtti.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

class RttiTest : public ::testing::Test {
protected:
    static constexpr GuestAddr REGION = 0x10000;
    static constexpr GuestAddr STACK  = 0x80000;

    // Object: [0]=vfptr(Derived/B1), [8]=vfptr(B2)
    static constexpr GuestAddr OBJ        = REGION + 0x100;
    static constexpr GuestAddr VFTABLE_D  = REGION + 0x200;
    static constexpr GuestAddr VFTABLE_B2 = REGION + 0x240;
    static constexpr GuestAddr COL_D      = REGION + 0x300;
    static constexpr GuestAddr COL_B2     = REGION + 0x320;
    static constexpr GuestAddr CHD        = REGION + 0x340;
    static constexpr GuestAddr BASE_ARRAY = REGION + 0x360;
    static constexpr GuestAddr BCD_D      = REGION + 0x380;
    static constexpr GuestAddr BCD_B1     = REGION + 0x3A0;
    static constexpr GuestAddr BCD_B2     = REGION + 0x3C0;
    static constexpr GuestAddr TD_D       = REGION + 0x400;
    static constexpr GuestAddr TD_B1      = REGION + 0x440;
    static constexpr GuestAddr TD_B2      = REGION + 0x480;
    static constexpr GuestAddr TD_OTHER   = REGION + 0x4C0;
    // A second descriptor for B1, as a different module would emit.
    static constexpr GuestAddr TD_B1_ALT  = REGION + 0x500;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(REGION, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.mode = cpu::CpuMode::Protected;
        cpu.gpr.rsp = STACK + 0x800;

        write_type_descriptor(TD_D, ".?AUDerived@@");
        write_type_descriptor(TD_B1, ".?AUB1@@");
        write_type_descriptor(TD_B2, ".?AUB2@@");
        write_type_descriptor(TD_OTHER, ".?AUUnrelated@@");
        write_type_descriptor(TD_B1_ALT, ".?AUB1@@");

        put(OBJ + 0, VFTABLE_D);
        put(OBJ + 8, VFTABLE_B2);
        put(VFTABLE_D - 4, COL_D);
        put(VFTABLE_B2 - 4, COL_B2);

        write_locator(COL_D, /*offset=*/0);
        write_locator(COL_B2, /*offset=*/8);

        put(CHD + 0, 0);            // signature
        put(CHD + 4, 1);            // attributes: multiple inheritance
        put(CHD + 8, 3);            // numBaseClasses
        put(CHD + 12, BASE_ARRAY);

        put(BASE_ARRAY + 0, BCD_D);
        put(BASE_ARRAY + 4, BCD_B1);
        put(BASE_ARRAY + 8, BCD_B2);

        write_base(BCD_D, TD_D, /*contained=*/2, /*mdisp=*/0);
        write_base(BCD_B1, TD_B1, /*contained=*/0, /*mdisp=*/0);
        write_base(BCD_B2, TD_B2, /*contained=*/0, /*mdisp=*/8);
    }

    void put(GuestAddr addr, u64 value) {
        ASSERT_TRUE(memory.write_u32(addr, static_cast<u32>(value)).is_ok());
    }

    void write_type_descriptor(GuestAddr addr, std::string_view name) {
        put(addr + 0, 0);   // vftable
        put(addr + 4, 0);   // spare
        for (usize i = 0; i < name.size(); ++i) {
            ASSERT_TRUE(memory.write_u8(addr + 8 + i,
                                        static_cast<u8>(name[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u8(addr + 8 + name.size(), 0).is_ok());
    }

    void write_locator(GuestAddr addr, u32 offset) {
        put(addr + 0, 0);        // signature: x86, absolute pointers
        put(addr + 4, offset);   // offset of this vfptr in the complete object
        put(addr + 8, 0);        // cdOffset
        put(addr + 12, TD_D);    // the complete object's dynamic type
        put(addr + 16, CHD);
    }

    void write_base(GuestAddr addr, GuestAddr type_desc, u32 contained,
                    i32 mdisp, i32 pdisp = -1, i32 vdisp = 0) {
        put(addr + 0, type_desc);
        put(addr + 4, contained);
        put(addr + 8, static_cast<u32>(mdisp));
        put(addr + 12, static_cast<u32>(pdisp));
        put(addr + 16, static_cast<u32>(vdisp));
        put(addr + 20, 0);   // attributes
    }

    /// Lay out cdecl arguments the way an x86 caller does and run `fn`.
    u32 call(void (*fn)(Marshaler&), std::initializer_list<u32> args) {
        u32 slot = 0;
        for (u32 value : args) {
            EXPECT_TRUE(memory.write_u32(
                static_cast<GuestAddr>(cpu.gpr.rsp) + 4 + slot * 4,
                value).is_ok());
            ++slot;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        fn(m);
        return static_cast<u32>(cpu.gpr.rax);
    }

    u32 dynamic_cast_call(GuestAddr inptr, i32 vf_delta, GuestAddr src,
                          GuestAddr target, u32 is_reference = 0) {
        return call(&rt_dynamic_cast, {static_cast<u32>(inptr),
                                       static_cast<u32>(vf_delta),
                                       static_cast<u32>(src),
                                       static_cast<u32>(target),
                                       is_reference});
    }

    GuestMemory memory;
    cpu::CpuState cpu;
};

// dynamic_cast<Derived*>(B2*) — the pointer must move back to the top of the
// complete object, i.e. `inptr - locator.offset`.
TEST_F(RttiTest, DowncastFromSecondBaseFindsCompleteObject) {
    EXPECT_EQ(dynamic_cast_call(OBJ + 8, 0, TD_B2, TD_D), OBJ);
}

// dynamic_cast<B1*>(B2*) — a cross-cast succeeds because the complete object
// holds exactly one B1 subobject.
TEST_F(RttiTest, CrossCastBetweenSiblingBases) {
    EXPECT_EQ(dynamic_cast_call(OBJ + 8, 0, TD_B2, TD_B1), OBJ);
}

// dynamic_cast<B2*>(Derived*) — an upcast still has to apply the base's mdisp.
TEST_F(RttiTest, UpcastAppliesBaseDisplacement) {
    EXPECT_EQ(dynamic_cast_call(OBJ, 0, TD_D, TD_B2), OBJ + 8);
}

TEST_F(RttiTest, CastToUnrelatedTypeReturnsNull) {
    EXPECT_EQ(dynamic_cast_call(OBJ + 8, 0, TD_B2, TD_OTHER), 0u);
}

TEST_F(RttiTest, NullPointerCastReturnsNull) {
    EXPECT_EQ(dynamic_cast_call(0, 0, TD_B2, TD_D), 0u);
}

TEST_F(RttiTest, IdenticalSourceAndTargetIsIdentity) {
    EXPECT_EQ(dynamic_cast_call(OBJ + 8, 0, TD_B2, TD_B2), OBJ + 8);
}

// Each module emits its own TypeDescriptor for a shared type, so descriptors
// must compare by decorated name and not by address — otherwise every
// cross-DLL dynamic_cast in a Qt/CEF-sized guest fails.
TEST_F(RttiTest, TypeDescriptorsMatchByDecoratedName) {
    EXPECT_EQ(dynamic_cast_call(OBJ + 8, 0, TD_B2, TD_B1_ALT), OBJ);
}

// A non-polymorphic or freed pointer must fail the cast, never fault.
TEST_F(RttiTest, GarbageObjectPointerFailsWithoutFaulting) {
    ASSERT_TRUE(memory.write_u32(OBJ + 0x40, 0x41414141).is_ok());
    EXPECT_EQ(dynamic_cast_call(OBJ + 0x40, 0, TD_B2, TD_D), 0u);
}

TEST_F(RttiTest, TypeidReportsCompleteObjectType) {
    EXPECT_EQ(call(&rt_typeid, {static_cast<u32>(OBJ)}), TD_D);
    EXPECT_EQ(call(&rt_typeid, {static_cast<u32>(OBJ + 8)}), TD_D);
}

TEST_F(RttiTest, CastToVoidReturnsCompleteObject) {
    EXPECT_EQ(call(&rt_cast_to_void, {static_cast<u32>(OBJ + 8)}), OBJ);
}

TEST_F(RttiTest, AdjustPointerAppliesMemberDisplacement) {
    const GuestAddr pmd = REGION + 0x600;
    put(pmd + 0, 0x10);                            // mdisp
    put(pmd + 4, static_cast<u32>(-1));            // pdisp: no virtual base
    put(pmd + 8, 0);                               // vdisp
    EXPECT_EQ(call(&adjust_pointer_export,
                   {static_cast<u32>(OBJ), static_cast<u32>(pmd)}), OBJ + 0x10);
}

// With pdisp >= 0 the object holds a vbtable pointer at obj+pdisp, and the
// entry at vdisp is the displacement from that slot to the virtual base.
TEST_F(RttiTest, AdjustPointerFollowsVirtualBaseTable) {
    const GuestAddr pmd = REGION + 0x600;
    const GuestAddr vbtable = REGION + 0x700;
    put(pmd + 0, 4);       // mdisp
    put(pmd + 4, 8);       // pdisp: vbptr lives at obj+8
    put(pmd + 8, 12);      // vdisp: entry 3 of the vbtable
    put(OBJ + 8, vbtable);
    put(vbtable + 12, 0x30);

    EXPECT_EQ(call(&adjust_pointer_export,
                   {static_cast<u32>(OBJ), static_cast<u32>(pmd)}),
              OBJ + 8 + 0x30 + 4);
}

} // namespace
