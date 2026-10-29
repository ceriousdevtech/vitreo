// SPDX-License-Identifier: LGPL-2.1-only
// Test for the MSVC C++ EH personality (`__CxxFrameHandler3`) replacement
// in src/winapi/msvcrt/cxx_eh.cpp.
//
// We synthesise a minimal MSVC C++ EH layout in guest memory — FuncInfo +
// TryBlockMap + HandlerArray + TypeDescriptor + ThrowInfo + CatchableType —
// and verify that dispatch_cxx_exception_32 produces the right result for
// the matching, type-mismatch, and out-of-try-range cases.

#include <gtest/gtest.h>
#include <cstring>

#include "core/memory/guest_memory.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "winapi/msvcrt/runtime/cxx_eh.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

// Layout helpers — keep all offsets in one place.
struct GuestLayout {
    static constexpr GuestAddr REGION_BASE   = 0x10000;
    static constexpr u32       REGION_SIZE   = 0x10000;

    // Subregions
    static constexpr u32 FRAME_OFF      = 0x100;   // EH registration record (12B)
    static constexpr u32 EREC_OFF       = 0x200;   // EXCEPTION_RECORD
    static constexpr u32 CTX_OFF        = 0x400;   // CONTEXT
    static constexpr u32 FUNCINFO_OFF   = 0x800;   // FuncInfo + maps
    static constexpr u32 TRYBLOCK_OFF   = 0x830;
    static constexpr u32 HANDLER_OFF    = 0x880;
    static constexpr u32 CATCH_TD_OFF   = 0x900;
    static constexpr u32 THROW_TD_OFF   = 0x980;
    static constexpr u32 THROWINFO_OFF  = 0xA00;
    static constexpr u32 CTA_OFF        = 0xA20;
    static constexpr u32 CT_OFF         = 0xA40;
    static constexpr u32 THROW_OBJ_OFF  = 0xB00;
};

// Write a NUL-terminated mangled-name TypeDescriptor at td_off.
// TypeDescriptor layout: vftable(4) + spare(4) + name[].
void write_type_descriptor(GuestMemory& mem, u32 td_off, std::string_view name) {
    GuestAddr base = GuestLayout::REGION_BASE + td_off;
    (void)mem.write_u32(base + 0, 0);       // vftable (ignored by name match)
    (void)mem.write_u32(base + 4, 0);       // spare
    for (usize i = 0; i < name.size(); ++i) {
        (void)mem.write_u8(base + 8 + i, static_cast<u8>(name[i]));
    }
    (void)mem.write_u8(base + 8 + name.size(), 0);  // terminator
}

struct CxxScenario {
    std::string throw_type;
    std::string catch_type;
    bool        by_reference{true};
    i32         current_state{0};
    i32         try_low{0};
    i32         try_high{0};
    u32         catch_handler_addr{0x004ABC00u};
    i32         disp_catch_obj{-0x20};
    u32         throw_payload{0xDEADBEEFu};
};

// Build the full memory layout for one scenario. Returns the assembled
// addresses needed to call dispatch_cxx_exception_32.
struct LayoutAddrs {
    u32 funcinfo_addr;
    u32 frame_addr;
    u32 erec_addr;
    u32 ctx_addr;
};

LayoutAddrs build_layout(GuestMemory& mem, const CxxScenario& s) {
    LayoutAddrs a{};

    using GL = GuestLayout;
    const GuestAddr base = GL::REGION_BASE;

    // -- TypeDescriptors --
    write_type_descriptor(mem, GL::CATCH_TD_OFF, s.catch_type);
    write_type_descriptor(mem, GL::THROW_TD_OFF, s.throw_type);

    // -- HandlerType --
    constexpr u32 HT_IS_REFERENCE = 0x08;
    GuestAddr ht = base + GL::HANDLER_OFF;
    (void)mem.write_u32(ht + 0,  s.by_reference ? HT_IS_REFERENCE : 0);
    (void)mem.write_u32(ht + 4,  base + GL::CATCH_TD_OFF);
    (void)mem.write_u32(ht + 8,  static_cast<u32>(s.disp_catch_obj));
    (void)mem.write_u32(ht + 12, s.catch_handler_addr);

    // -- TryBlockMapEntry --
    GuestAddr tbe = base + GL::TRYBLOCK_OFF;
    (void)mem.write_u32(tbe + 0,  static_cast<u32>(s.try_low));
    (void)mem.write_u32(tbe + 4,  static_cast<u32>(s.try_high));
    (void)mem.write_u32(tbe + 8,  static_cast<u32>(s.try_high));   // catch_high
    (void)mem.write_u32(tbe + 12, 1);                              // n_catches
    (void)mem.write_u32(tbe + 16, base + GL::HANDLER_OFF);

    // -- FuncInfo --
    GuestAddr fi = base + GL::FUNCINFO_OFF;
    (void)mem.write_u32(fi + 0,  0x19930520);    // magic
    (void)mem.write_u32(fi + 4,  4);             // maxState
    (void)mem.write_u32(fi + 8,  0);             // unwindMap (unused in v1)
    (void)mem.write_u32(fi + 12, 1);             // n_try_blocks
    (void)mem.write_u32(fi + 16, base + GL::TRYBLOCK_OFF);
    (void)mem.write_u32(fi + 20, 0);             // n_ip_to_state
    (void)mem.write_u32(fi + 24, 0);             // ip_to_state

    // -- CatchableType (single entry) --
    GuestAddr ct = base + GL::CT_OFF;
    (void)mem.write_u32(ct + 0,  0);                            // properties
    (void)mem.write_u32(ct + 4,  base + GL::THROW_TD_OFF);      // pType
    (void)mem.write_u32(ct + 8,  0);                            // pmd.mdisp
    (void)mem.write_u32(ct + 12, 0);                            // pmd.pdisp
    (void)mem.write_u32(ct + 16, 0);                            // pmd.vdisp
    (void)mem.write_u32(ct + 20, 4);                            // size_or_offset
    (void)mem.write_u32(ct + 24, 0);                            // copy_function

    // -- CatchableTypeArray: [count(4)][CatchableType*][...] --
    GuestAddr cta = base + GL::CTA_OFF;
    (void)mem.write_u32(cta + 0, 1);
    (void)mem.write_u32(cta + 4, base + GL::CT_OFF);

    // -- ThrowInfo --
    GuestAddr ti = base + GL::THROWINFO_OFF;
    (void)mem.write_u32(ti + 0,  0);          // attributes
    (void)mem.write_u32(ti + 4,  0);          // pmfn_unwind
    (void)mem.write_u32(ti + 8,  0);          // forward_compat
    (void)mem.write_u32(ti + 12, base + GL::CTA_OFF);

    // -- Throw object (just 4 bytes for this minimal test) --
    GuestAddr obj = base + GL::THROW_OBJ_OFF;
    (void)mem.write_u32(obj, s.throw_payload);

    // -- EH registration node (frame): pNext(4) | handler(4) | state(4) --
    GuestAddr frame = base + GL::FRAME_OFF;
    (void)mem.write_u32(frame + 0, 0xFFFFFFFFu);                 // pNext = end
    (void)mem.write_u32(frame + 4, 0xCAFEBABEu);                 // handler (unused by dispatcher)
    (void)mem.write_u32(frame + 8, static_cast<u32>(s.current_state));

    // -- EXCEPTION_RECORD (write fields directly; nparams=3, args = magic, obj, ti) --
    constexpr u32 EXCEPTION_CPP_MSVC = 0xE06D7363u;
    u32 erec = base + GL::EREC_OFF;
    (void)mem.write_u32(erec + cpu::EREC32_CODE,       EXCEPTION_CPP_MSVC);
    (void)mem.write_u32(erec + cpu::EREC32_FLAGS,      1);
    (void)mem.write_u32(erec + cpu::EREC32_RECORD,     0);
    (void)mem.write_u32(erec + cpu::EREC32_ADDRESS,    0x12345678u);
    (void)mem.write_u32(erec + cpu::EREC32_NUM_PARAMS, 3);
    (void)mem.write_u32(erec + cpu::EREC32_PARAMS + 0, 0x19930520u);
    (void)mem.write_u32(erec + cpu::EREC32_PARAMS + 4, static_cast<u32>(obj));
    (void)mem.write_u32(erec + cpu::EREC32_PARAMS + 8,
                         static_cast<u32>(base + GL::THROWINFO_OFF));

    // -- CONTEXT --
    (void)mem.write_u32(base + GL::CTX_OFF + cpu::CTX32_EIP, 0x11111111u);

    a.funcinfo_addr = base + GL::FUNCINFO_OFF;
    a.frame_addr    = base + GL::FRAME_OFF;
    a.erec_addr     = base + GL::EREC_OFF;
    a.ctx_addr      = base + GL::CTX_OFF;
    return a;
}

class CxxEhTest : public ::testing::Test {
protected:
    void SetUp() override {
        memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(memory->map_region(GuestLayout::REGION_BASE,
                                        GuestLayout::REGION_SIZE,
                                        MemoryProtection::ReadWrite));
    }
    std::unique_ptr<GuestMemory> memory;
};

} // namespace

TEST_F(CxxEhTest, MatchesExactTypeName) {
    CxxScenario s{
        .throw_type = ".?AVMyException@@",
        .catch_type = ".?AVMyException@@",
        .by_reference = true,
        .current_state = 0,
        .try_low = 0,
        .try_high = 0,
        .catch_handler_addr = 0x004ABC00u,
        .disp_catch_obj = -0x10,
        .throw_payload = 0xDEADBEEFu,
    };
    auto a = build_layout(*memory, s);
    auto r = dispatch_cxx_exception_32(*memory, a.funcinfo_addr,
                                        a.frame_addr, a.erec_addr, a.ctx_addr);
    EXPECT_TRUE(r.match);
    EXPECT_EQ(r.handler_addr, s.catch_handler_addr);
    EXPECT_EQ(r.disp_catch_obj, s.disp_catch_obj);
    EXPECT_EQ(r.try_high, s.try_high);
}

TEST_F(CxxEhTest, EllipsisCatchMatchesAnything) {
    CxxScenario s{
        .throw_type = ".?AVBoostException@@",
        .catch_type = "",                 // catch(...) — empty type descriptor
        .by_reference = true,
        .current_state = 0,
        .try_low = 0, .try_high = 0,
    };
    auto a = build_layout(*memory, s);
    // Override catch_type by clearing the catch TypeDescriptor pointer
    // in the HandlerType (catch(...) is signalled by a null typeDescPtr).
    GuestAddr ht = GuestLayout::REGION_BASE + GuestLayout::HANDLER_OFF;
    (void)memory->write_u32(ht + 4, 0);

    auto r = dispatch_cxx_exception_32(*memory, a.funcinfo_addr,
                                        a.frame_addr, a.erec_addr, a.ctx_addr);
    EXPECT_TRUE(r.match);
}

TEST_F(CxxEhTest, TypeMismatchReturnsNoMatch) {
    CxxScenario s{
        .throw_type = ".?AVException_A@@",
        .catch_type = ".?AVException_B@@",
        .current_state = 0,
        .try_low = 0, .try_high = 0,
    };
    auto a = build_layout(*memory, s);
    auto r = dispatch_cxx_exception_32(*memory, a.funcinfo_addr,
                                        a.frame_addr, a.erec_addr, a.ctx_addr);
    EXPECT_FALSE(r.match);
}

TEST_F(CxxEhTest, StateOutsideTryReturnsNoMatch) {
    CxxScenario s{
        .throw_type = ".?AVX@@",
        .catch_type = ".?AVX@@",
        .current_state = 5,    // outside try range [0,0]
        .try_low = 0, .try_high = 0,
    };
    auto a = build_layout(*memory, s);
    auto r = dispatch_cxx_exception_32(*memory, a.funcinfo_addr,
                                        a.frame_addr, a.erec_addr, a.ctx_addr);
    EXPECT_FALSE(r.match);
}

TEST_F(CxxEhTest, NegativeStateReturnsNoMatch) {
    CxxScenario s{
        .throw_type = ".?AVX@@",
        .catch_type = ".?AVX@@",
        .current_state = -1,   // function not currently inside any try
        .try_low = 0, .try_high = 0,
    };
    auto a = build_layout(*memory, s);
    auto r = dispatch_cxx_exception_32(*memory, a.funcinfo_addr,
                                        a.frame_addr, a.erec_addr, a.ctx_addr);
    EXPECT_FALSE(r.match);
}

TEST_F(CxxEhTest, InstallCatchParamWritesReferencePointer) {
    CxxScenario s{
        .throw_type = ".?AVX@@",
        .catch_type = ".?AVX@@",
        .by_reference = true,
        .current_state = 0,
        .try_low = 0, .try_high = 0,
        .disp_catch_obj = -0x10,
    };
    auto a = build_layout(*memory, s);
    auto r = dispatch_cxx_exception_32(*memory, a.funcinfo_addr,
                                        a.frame_addr, a.erec_addr, a.ctx_addr);
    ASSERT_TRUE(r.match);
    EXPECT_TRUE(install_catch_param_32(*memory, r, a.frame_addr));

    // Slot is at (frame + 12) + disp_catch_obj.
    // frame = REGION_BASE + 0x100; so parent EBP = 0x10000 + 0x100 + 12 = 0x1010C.
    // slot = 0x1010C + (-0x10) = 0x100FC.
    auto val = memory->read_u32(0x100FC);
    ASSERT_TRUE(val.is_ok());
    // Reference catch writes a pointer to the throw object (PMD.mdisp = 0).
    EXPECT_EQ(val.value(), GuestLayout::REGION_BASE + GuestLayout::THROW_OBJ_OFF);
}

TEST_F(CxxEhTest, PatchContextSetsEipAndUpdatesState) {
    CxxScenario s{
        .throw_type = ".?AVX@@",
        .catch_type = ".?AVX@@",
        .current_state = 1,
        .try_low = 1, .try_high = 3,
        .catch_handler_addr = 0x00C0FFEEu,
    };
    auto a = build_layout(*memory, s);
    auto r = dispatch_cxx_exception_32(*memory, a.funcinfo_addr,
                                        a.frame_addr, a.erec_addr, a.ctx_addr);
    ASSERT_TRUE(r.match);
    constexpr u32 continuation = 0x0040CA7Cu;
    constexpr u32 resume_sp = 0x0001E000u;
    ASSERT_TRUE(memory->write_u32(a.frame_addr - 4, resume_sp).is_ok());
    patch_context_for_catch_32(*memory, r, a.frame_addr, a.ctx_addr,
                               continuation);

    auto eip = memory->read_u32(a.ctx_addr + cpu::CTX32_EIP);
    ASSERT_TRUE(eip.is_ok());
    EXPECT_EQ(eip.value(), continuation);
    EXPECT_EQ(memory->read_u32(a.ctx_addr + cpu::CTX32_EBP).value_or(0),
              a.frame_addr + 12);
    EXPECT_EQ(memory->read_u32(a.ctx_addr + cpu::CTX32_ESP).value_or(0),
              resume_sp);

    auto state = memory->read_u32(a.frame_addr + 8);
    ASSERT_TRUE(state.is_ok());
    EXPECT_EQ(state.value(), static_cast<u32>(s.try_high + 1));
}

TEST_F(CxxEhTest, CatchFuncletCallbackStackAccountsForSyntheticReturn) {
    constexpr u32 saved_epilogue_sp = 0x0001E000u;
    const u32 frame = GuestLayout::REGION_BASE + GuestLayout::FRAME_OFF;
    ASSERT_TRUE(memory->write_u32(frame - sizeof(u32),
                                  saved_epilogue_sp).is_ok());

    const auto callback_stack =
        catch_funclet_callback_stack_top_32(*memory, frame);

    ASSERT_TRUE(callback_stack.has_value());
    EXPECT_EQ(callback_stack.value(), saved_epilogue_sp + sizeof(u32));
}

// The synthetic return address is pushed at exactly the MSVC epilogue SP —
// the dword the shared epilogue later pops into EDI. Callers that run the
// funclet via run_guest_callback must restore this slot after the callback.
TEST_F(CxxEhTest, CatchFuncletSentinelSlotIsEpilogueNonvolatileHome) {
    constexpr u32 saved_epilogue_sp = 0x0001E000u;
    constexpr u32 saved_edi = 0x7FFB9D00u;
    const u32 frame = GuestLayout::REGION_BASE + GuestLayout::FRAME_OFF;
    ASSERT_TRUE(memory->write_u32(frame - sizeof(u32),
                                  saved_epilogue_sp).is_ok());
    ASSERT_TRUE(memory->write_u32(saved_epilogue_sp, saved_edi).is_ok());

    const auto callback_stack =
        catch_funclet_callback_stack_top_32(*memory, frame);
    ASSERT_TRUE(callback_stack.has_value());

    const u32 sentinel_slot = callback_stack.value() - sizeof(u32);
    EXPECT_EQ(sentinel_slot, saved_epilogue_sp);

    // Simulate run_guest_callback's push of the x86 callback return sentinel.
    constexpr u32 kSentinel = 0xDEADC0DEu;
    ASSERT_TRUE(memory->write_u32(sentinel_slot, kSentinel).is_ok());
    EXPECT_EQ(memory->read_u32(saved_epilogue_sp).value_or(0), kSentinel);

    // After the funclet returns, the slot must be restored or `pop edi` in
    // the parent epilogue reloads DEADC0DE into the caller's EDI.
    ASSERT_TRUE(memory->write_u32(sentinel_slot, saved_edi).is_ok());
    EXPECT_EQ(memory->read_u32(saved_epilogue_sp).value_or(0), saved_edi);
}

// The exception object is a temporary in the *throwing* frame, and a
// catch-by-reference binds straight to it. Windows keeps it alive by running
// the catch body on the dispatcher's stack, below the throw site; entering the
// funclet at the catch function's own (unwound) ESP instead lets the first
// function the catch body calls reuse those bytes — the catch parameter then
// dereferences a vftable pointer some unrelated callee overwrote.
TEST_F(CxxEhTest, CatchFuncletEntersBelowTheThrowSiteSoTheObjectSurvives) {
    constexpr u32 saved_epilogue_sp = 0x0001E000u;
    constexpr u32 throw_site_sp = 0x0001D004u;   // deeper than the catch frame
    const u32 frame = GuestLayout::REGION_BASE + GuestLayout::FRAME_OFF;
    ASSERT_TRUE(memory->write_u32(frame - sizeof(u32),
                                  saved_epilogue_sp).is_ok());

    const auto entry =
        catch_funclet_entry_stack_32(*memory, frame, throw_site_sp);
    ASSERT_TRUE(entry.has_value());

    // Below the throw site, and 16-byte aligned as a real call site would be.
    EXPECT_LE(entry.value(), throw_site_sp);
    EXPECT_EQ(entry.value() % 16u, 0u);

    // An object anywhere between the throw site and the catch frame is now
    // above the funclet's stack, so nothing the catch body calls can reach it.
    constexpr u32 exception_object = throw_site_sp + 0x40u;
    EXPECT_LT(entry.value(), exception_object);
    EXPECT_LT(exception_object, saved_epilogue_sp);
}

TEST_F(CxxEhTest, CatchFuncletEntryStackRejectsAThrowSiteAboveTheCatchFrame) {
    constexpr u32 saved_epilogue_sp = 0x0001E000u;
    const u32 frame = GuestLayout::REGION_BASE + GuestLayout::FRAME_OFF;
    ASSERT_TRUE(memory->write_u32(frame - sizeof(u32),
                                  saved_epilogue_sp).is_ok());

    // Not a stack the dispatcher walked up: there is nothing below the catch
    // to protect, so the caller falls back to the epilogue stack.
    EXPECT_FALSE(catch_funclet_entry_stack_32(
        *memory, frame, saved_epilogue_sp + 0x100u).has_value());
    EXPECT_FALSE(catch_funclet_entry_stack_32(*memory, frame, 0).has_value());
}

TEST_F(CxxEhTest, CatchFuncletCallbackStackRejectsInvalidSavedStack) {
    const u32 frame = GuestLayout::REGION_BASE + GuestLayout::FRAME_OFF;

    ASSERT_TRUE(memory->write_u32(frame - sizeof(u32), 0).is_ok());
    EXPECT_FALSE(
        catch_funclet_callback_stack_top_32(*memory, frame).has_value());

    ASSERT_TRUE(memory->write_u32(frame - sizeof(u32), UINT32_MAX).is_ok());
    EXPECT_FALSE(
        catch_funclet_callback_stack_top_32(*memory, frame).has_value());

    EXPECT_FALSE(
        catch_funclet_callback_stack_top_32(*memory, 0).has_value());
}
