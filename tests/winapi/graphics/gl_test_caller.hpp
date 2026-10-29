#pragma once
/// @file gl_test_caller.hpp
/// @brief A guest-thread caller for the opengl32/gdi32 export tests: calls a
///        registered export with a real x86 stdcall or x64 frame (garbage in
///        unused registers, the last error seeded with a stale sentinel), so
///        "untouched" and the stdcall byte count are observable.

#include <gtest/gtest.h>

#include <vector>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/error.hpp"

namespace vitreo::winapi::gl::test {

inline constexpr u32 kStale = 0xDEAD;     // a last error a success must leave alone

/// One guest thread's view: its own CPU and stack, shared memory and exports.
class Caller {
public:
    Caller(GuestMemory& memory, ThunkRegistry& registry, bool x64, int slot)
        : m_memory(memory), m_registry(registry), m_x64(x64),
          m_frame(kStackBase + static_cast<GuestAddr>(slot) * 0x2000 + 0x1000) {
        if (x64) m_cpu.init_long_mode(); else m_cpu.init_protected_mode();
    }

    static constexpr GuestAddr kStackBase = 0x80000;

    /// Calls export @p name; the last error is the stale sentinel before the
    /// call, so "untouched" is observable.
    u64 call(const char* name, std::vector<u64> args = {}) {
        return call_in("opengl32.dll", name, std::move(args));
    }

    u64 call_in(const char* dll, const char* name, std::vector<u64> args) {
        auto addr = m_registry.get_thunk_address(dll, name);
        EXPECT_TRUE(addr.is_ok()) << name;
        if (!addr.is_ok()) return ~0ull;
        auto handler = m_registry.resolve_handler(addr.value());
        EXPECT_TRUE(handler.is_ok()) << name;
        if (!handler.is_ok()) return ~0ull;
        set_last_error(kStale);
        m_cpu.set_sp(m_frame);
        m_cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        m_cpu.gpr.rcx = m_cpu.gpr.rdx = m_cpu.gpr.r8 = m_cpu.gpr.r9 = 0xDDDDDDDDDDDDDDDDull;
        if (m_x64) {
            (void)m_memory.write_u64(m_frame, 0xDEADBEEF);
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) {
                    (i == 0 ? m_cpu.gpr.rcx : i == 1 ? m_cpu.gpr.rdx : i == 2 ? m_cpu.gpr.r8 : m_cpu.gpr.r9) = args[i];
                } else {
                    (void)m_memory.write_u64(m_frame + 0x28 + (i - 4) * 8, args[i]);
                }
            }
        } else {
            (void)m_memory.write_u32(m_frame, 0xDEADBEEF);
            for (usize i = 0; i < args.size(); ++i) {
                (void)m_memory.write_u32(m_frame + 4 + static_cast<GuestAddr>(i * 4), static_cast<u32>(args[i]));
            }
            EXPECT_EQ(m_registry.resolve_stdcall_bytes(addr.value()), args.size() * 4) << name;
        }
        Marshaler m(m_memory);
        m.attach_cpu(m_cpu);
        (*handler.value())(m);
        return m_x64 ? m_cpu.gpr.rax : (m_cpu.gpr.rax & 0xFFFFFFFFull);
    }

    u64 err() const { return get_last_error(); }
    u64 make(u64 dc, u64 rc) { return call("wglMakeCurrent", {dc, rc}); }
    u64 create(u64 dc) { return call("wglCreateContext", {dc}); }
    u64 cur_ctx() { return call("wglGetCurrentContext"); }
    u64 cur_dc() { return call("wglGetCurrentDC"); }

private:
    GuestMemory& m_memory;
    ThunkRegistry& m_registry;
    bool m_x64;
    GuestAddr m_frame;
    cpu::CpuState m_cpu;
};

}  // namespace vitreo::winapi::gl::test
