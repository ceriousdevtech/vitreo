// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_string_memory_registration.cpp
/// @brief memset / memcpy / memmove / memcmp / strlen / wcslen / strcpy / strcat.
///
/// Split out of crt_string_registration.cpp.

#include "winapi/msvcrt/text/crt_string.hpp"
#include "winapi/msvcrt/text/crt_string_internal.hpp"
#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/mem_watch.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/msvcrt/memory/vcruntime_interpose.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

namespace {

/// Property apply can land float fields via CRT memcpy and bypass JIT store
/// watches; the optional value pattern catches that path (e.g. drawable UV
/// slots). Diagnostic only: reads the source, never changes the copy.
void watch_value_memcpy(Marshaler& m, GuestAddr dest, GuestAddr src, usize count) {
    const u32 pattern = vitreo::mem_watch_value_pattern();
    if (pattern == 0) return;
    const Byte* sp = m.memory().host_ptr(src, count);
    if (sp == nullptr) return;
    if (count >= 4 &&
        vitreo::mem_watch_value_hits().load(
            std::memory_order_relaxed) < 8u) {
        for (usize off = 0; off + 4 <= count; off += 4) {
            u32 word = 0;
            std::memcpy(&word, sp + off, sizeof(word));
            if (word != pattern) {
                continue;
            }
            const GuestAddr hit = dest + static_cast<GuestAddr>(off);
            u32 mode = 0xffffffffu;
            if (hit >= 4) {
                const Byte* mode_p =
                    m.memory().host_ptr(hit - 4, 4);
                if (mode_p) {
                    std::memcpy(&mode, mode_p, sizeof(mode));
                }
            }
            // Prefer drawable-like UV writes (mode 1/2 at -4).
            if (mode > 8u && count != 16) {
                continue;
            }
            if (vitreo::mem_watch_value_hits().fetch_add(
                    1, std::memory_order_relaxed) >= 8u) {
                break;
            }
            float f0 = 0.f, f1 = 0.f, f2 = 0.f, f3 = 0.f;
            if (count >= 4) std::memcpy(&f0, sp, 4);
            if (count >= 8) std::memcpy(&f1, sp + 4, 4);
            if (count >= 12) std::memcpy(&f2, sp + 8, 4);
            if (count >= 16) std::memcpy(&f3, sp + 12, 4);
            u32 ret = 0;
            if (m.has_cpu() && m.cpu() != nullptr) {
                ret = m.memory()
                          .read_u32(static_cast<GuestAddr>(
                              m.cpu()->sp()))
                          .value_or(0);
            }
            std::fprintf(
                stderr,
                "[watch-value] memcpy dest=0x%08x src=0x%08x "
                "n=%zu hit_off=%zu mode_at-4=%u "
                "srcf=%g,%g,%g,%g ret=0x%08x\n",
                static_cast<unsigned>(dest),
                static_cast<unsigned>(src), count, off, mode,
                static_cast<double>(f0),
                static_cast<double>(f1),
                static_cast<double>(f2),
                static_cast<double>(f3), ret);
            std::fflush(stderr);
            break;
        }
    }
}

/// After a successful guest-semantics store of [dest, dest+count).
void watch_store(Marshaler& m, GuestAddr dest, usize count, const char* tag) {
    if (count == 0 || !store_ring_enabled()) return;
    if (m.has_cpu() && m.cpu()->pending_raise.active) return;
    if (const Byte* dp = m.memory().host_ptr(dest, count)) mem_watch_bytes(dest, dp, count, tag);
}

} // namespace

void add_crt_memory_op_exports(DllExports& exports, CrtStringManager& mgr) {
    // memset / memcpy / memmove / memcmp share the checked host bodies of
    // vcruntime_interpose.hpp: guest-store protection, Windows fault records
    // (STATUS_ACCESS_VIOLATION / STATUS_GUARD_PAGE_VIOLATION after the bytes
    // before the faulting page are written), memmove-safe memcpy (every
    // Windows memcpy is: tests/oracle_data/crt_memcpy_overlap_probe*.expected)
    // and memcmp returning exactly -1/0/+1 (crt_memcmp_sign_probe*.expected).

    // ordinal 17: void* memset(void* dest, int c, size_t count)
    exports.entries.push_back(
        {"memset", 17, [&mgr](Marshaler& m) {
            (void)mgr;
            vcrt::run_body(vcrt::Fn::Memset, m);
            watch_store(m, m.arg_ptr(0), static_cast<usize>(m.arg(2)), "crt.memset");
        }});

    // ordinal 18: void* memcpy(void* dest, const void* src, size_t count)
    exports.entries.push_back(
        {"memcpy", 18, [&mgr](Marshaler& m) {
            (void)mgr;
            const usize count = static_cast<usize>(m.arg(2));
            if (count > 0) watch_value_memcpy(m, m.arg_ptr(0), m.arg_ptr(1), count);
            vcrt::run_body(vcrt::Fn::Memcpy, m);
            watch_store(m, m.arg_ptr(0), count, "crt.memcpy");
        }});

    // ordinal 19: void* memmove(void* dest, const void* src, size_t count)
    exports.entries.push_back(
        {"memmove", 19, [&mgr](Marshaler& m) {
            (void)mgr;
            vcrt::run_body(vcrt::Fn::Memmove, m);
            watch_store(m, m.arg_ptr(0), static_cast<usize>(m.arg(2)), "crt.memmove");
        }});

    // ordinal 20: int memcmp(const void* buf1, const void* buf2, size_t count)
    exports.entries.push_back(
        {"memcmp", 20, [&mgr](Marshaler& m) {
            (void)mgr;
            vcrt::run_body(vcrt::Fn::Memcmp, m);
        }});

    // ordinal 21: size_t strlen(const char* str)
    exports.entries.push_back(
        {"strlen", 21, [&mgr](Marshaler& m) {
            (void)mgr;
            GuestAddr addr = m.arg_ptr(0);
            usize len = guest_strlen(m.memory(), addr);
            m.set_return(static_cast<u64>(len));
        }});

    // ordinal 22: size_t wcslen(const wchar_t* str)
    exports.entries.push_back(
        {"wcslen", 22, [&mgr](Marshaler& m) {
            (void)mgr;
            GuestAddr addr = m.arg_ptr(0);
            usize len = guest_wcslen(m.memory(), addr);
            m.set_return(static_cast<u64>(len));
        }});

    // ordinal 23: char* strcpy(char* dest, const char* src)
    exports.entries.push_back(
        {"strcpy", 23, [&mgr](Marshaler& m) {
            (void)mgr;
            GuestAddr dest = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            usize len = guest_strlen(m.memory(), src);
            usize copy_len = len + 1;
            usize safe_len = std::min(copy_len, MAX_BULK_SIZE);
            std::vector<Byte> buf(safe_len);
            (void)m.memory().read_bytes(src, buf.data(), safe_len);
            if (safe_len > 0) buf[safe_len - 1] = 0;
            (void)m.memory().write_bytes(dest, buf.data(), safe_len);
            m.set_return(m.arg(0));
        }});

    // ordinal 24: char* strcat(char* dest, const char* src)
    exports.entries.push_back(
        {"strcat", 24, [&mgr](Marshaler& m) {
            (void)mgr;
            GuestAddr dest = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            usize dest_len = guest_strlen(m.memory(), dest);
            usize src_len = guest_strlen(m.memory(), src);
            usize copy_len = src_len + 1;
            usize safe_len = std::min(copy_len, MAX_BULK_SIZE);
            std::vector<Byte> buf(safe_len);
            (void)m.memory().read_bytes(src, buf.data(), safe_len);
            if (safe_len > 0) buf[safe_len - 1] = 0;
            (void)m.memory().write_bytes(dest + dest_len, buf.data(), safe_len);
            m.set_return(m.arg(0));
        }});

    // The vcruntime140 interposer's host targets (vcruntime_interpose.hpp):
    // one internal entry per interposed primitive, resolved with
    // include_internal and never visible to guest lookup. They are separate
    // from the public slots above because on x86 install_guest_crt_accelerators
    // overwrites the memcpy/memmove/memset sleds with guest code and withdraws
    // them from in-chain, so those slots no longer reach the host.
    //
    // L3-K class A (kernel32_in_chain.hpp): each returns to its caller and
    // runs no guest code. The only other way out is a memory fault, raised
    // through pending_raise -- a structural raise that breaks to the outer
    // dispatcher, exactly as EnterCriticalSection's EXCEPTION_POSSIBLE_DEADLOCK.
    for (usize i = 0; i < vcrt::kFnCount; ++i) {
        const auto fn = static_cast<vcrt::Fn>(i);
        ExportEntry e{vcrt::internal_export_name(fn), 0,
                      [fn](Marshaler& m) { vcrt::run_body(fn, m); }};
        e.internal = true;
        e.in_chain = InChainClass::A;
        exports.entries.push_back(std::move(e));
    }
    // The resume entries a faulted memcpy/memmove/memset continues from after
    // EXCEPTION_CONTINUE_EXECUTION (vcrt::deliver): the faulting context's
    // RIP, with the original arguments in place and the progress in
    // EAX/RAX. Never called; resumed into. Class A for the same reasons.
    for (auto kind : {vcrt::ResumeKind::Move, vcrt::ResumeKind::Fill}) {
        ExportEntry e{vcrt::resume_export_name(kind), 0,
                      [kind](Marshaler& m) { vcrt::run_resume(kind, m); }};
        e.internal = true;
        e.in_chain = InChainClass::A;
        exports.entries.push_back(std::move(e));
    }
}

} // namespace vitreo::winapi
