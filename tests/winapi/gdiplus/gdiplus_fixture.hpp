#pragma once
/// @file gdiplus_fixture.hpp
/// @brief Shared fixture for the GDI+ area tests: calls any gdiplus.dll
/// export by name, through the registered handler, in an x86 (stdcall) or
/// x64 (Windows x64 ABI) guest.
///
/// Use TEST_P(GdipTest, ...) with INSTANTIATE_GDIP_BITNESS(Suite) so every
/// contract test runs in both bitnesses. Arguments:
///   I(v)  integer / enum / BOOL / handle / pointer (pointer width on x64)
///   R(f)  REAL (XMM0-3 for positions 0-3 on x64, stack slot otherwise)
/// out<T>(), read_handle(), etc. read results back from guest memory.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/graphics/gdiplus/gdiplus.hpp>
#include <winapi/graphics/gdiplus/gdiplus_core.hpp>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/memory/heap.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/handle.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace vitreo::winapi::gdip_test {

using gdip::Status;

struct Arg {
    bool real{false};
    u64 value{0};
    float f{0};
};
inline Arg I(u64 v) { return Arg{false, v, 0}; }
inline Arg R(float f) { return Arg{true, 0, f}; }

class GdipTest : public ::testing::TestWithParam<bool> {
protected:
    // Guest layout: scratch data, then the stack.
    static constexpr GuestAddr DATA = 0x10000;       ///< 1 MiB scratch
    static constexpr usize DATA_SIZE = 0x100000;
    static constexpr GuestAddr STACK = 0x200000;     ///< 64 KiB stack
    static constexpr GuestAddr OUT = DATA;           ///< first out slot
    static constexpr GuestAddr OUT2 = DATA + 0x10;
    static constexpr GuestAddr OUT3 = DATA + 0x20;
    static constexpr GuestAddr BUF = DATA + 0x1000;  ///< general buffer

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA, DATA_SIZE, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK, 0x10000, MemoryProtection::ReadWrite).is_ok());
        if (x64()) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK + 0x8000;
        marshaler.attach_cpu(cpu);
        mgr.set_gdi_manager(&gdi);
        gdi.set_guest_memory(&memory);
        handlers = gdiplus_handlers(mgr);
    }

    [[nodiscard]] bool x64() const { return GetParam(); }
    [[nodiscard]] usize ptr_size() const { return x64() ? 8 : 4; }

    /// Call export @p name with @p args; returns the GpStatus.
    Status call(const std::string& name, std::initializer_list<Arg> args) {
        auto it = handlers.find(name);
        if (it == handlers.end()) {
            ADD_FAILURE() << name << " has no handler";
            return Status::NotImplemented;
        }
        usize i = 0;
        for (const Arg& a : args) {
            if (x64()) {
                const u64 bits = a.real ? float_bits(a.f) : a.value;
                if (i < 4) {
                    u64* gp[4] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
                    // The GPR slot of a REAL is undefined: poison it.
                    *gp[i] = a.real ? 0xDEADBEEFDEADBEEFull : bits;
                    if (a.real) cpu.sse.xmm[i].u32_[0] = static_cast<u32>(bits);
                } else {
                    EXPECT_TRUE(memory.write_u64(cpu.gpr.rsp + 8 + i * 8, bits).is_ok());
                }
            } else {
                const u32 bits = a.real ? static_cast<u32>(float_bits(a.f))
                                        : static_cast<u32>(a.value);
                EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + i * 4, bits).is_ok());
            }
            ++i;
        }
        it->second(marshaler);
        return static_cast<Status>(static_cast<u32>(cpu.gpr.rax));
    }

    /// The handle a GpXxx** output received (pointer width).
    [[nodiscard]] u64 read_handle(GuestAddr at = OUT) {
        return x64() ? memory.read_u64(at).value() : memory.read_u32(at).value();
    }
    /// Pre-fill an output slot so a test can see whether it was written.
    void poison(GuestAddr at, usize bytes = 8, u8 value = 0xCC) {
        std::vector<u8> fill(bytes, value);
        ASSERT_TRUE(memory.write_bytes(at, fill.data(), fill.size()).is_ok());
    }
    [[nodiscard]] float read_real(GuestAddr at) {
        const u32 bits = memory.read_u32(at).value();
        float f = 0;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    [[nodiscard]] i32 read_int(GuestAddr at) { return static_cast<i32>(memory.read_u32(at).value()); }
    void write_real(GuestAddr at, float f) {
        ASSERT_TRUE(memory.write_u32(at, static_cast<u32>(float_bits(f))).is_ok());
    }
    void write_int(GuestAddr at, i32 v) {
        ASSERT_TRUE(memory.write_u32(at, static_cast<u32>(v)).is_ok());
    }
    void write_reals(GuestAddr at, std::initializer_list<float> v) {
        usize i = 0;
        for (float f : v) write_real(at + 4 * i++, f);
    }
    void write_ints(GuestAddr at, std::initializer_list<i32> v) {
        usize i = 0;
        for (i32 x : v) write_int(at + 4 * i++, x);
    }
    void write_wide(GuestAddr at, const std::u16string& s) {
        for (usize i = 0; i < s.size(); ++i)
            ASSERT_TRUE(memory.write_u16(at + i * 2, static_cast<u16>(s[i])).is_ok());
        ASSERT_TRUE(memory.write_u16(at + s.size() * 2, 0).is_ok());
    }

    static u64 float_bits(float f) {
        u32 b = 0;
        std::memcpy(&b, &f, 4);
        return b;
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    HandleTable handle_table;
    VirtualMemoryManager vmm{memory, 0x7FFEFFFFULL};
    HeapManager heap{vmm, memory, handle_table};
    GdiManager gdi{handle_table};
    GdiplusManager mgr{heap};
    gdip::HandlerMap handlers;
};

#define INSTANTIATE_GDIP_BITNESS(Suite)                                                   \
    INSTANTIATE_TEST_SUITE_P(Bitness, Suite, ::testing::Values(false, true),             \
                             [](const ::testing::TestParamInfo<bool>& i) {                \
                                 return i.param ? std::string("x64") : std::string("x86"); \
                             })

}  // namespace vitreo::winapi::gdip_test
