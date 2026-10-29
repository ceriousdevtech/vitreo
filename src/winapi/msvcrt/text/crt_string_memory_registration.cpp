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
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

void add_crt_memory_op_exports(DllExports& exports, CrtStringManager& mgr) {
    // ordinal 17: void* memset(void* dest, int c, size_t count)
    exports.entries.push_back(
        {"memset", 17, [&mgr](Marshaler& m) {
            (void)mgr;
            GuestAddr dest = m.arg_ptr(0);
            u8 c = static_cast<u8>(m.arg(1) & 0xFF);
            usize count = static_cast<usize>(m.arg(2));
            if (count > 0) {
                Byte* dp = m.memory().host_ptr_mut(dest, count);
                if (dp) {
                    ::memset(dp, c, count);
                    if (store_ring_enabled()) mem_watch_bytes(dest, dp, count, "crt.memset");
                } else {
                    Byte buf[4096];
                    ::memset(buf, c, sizeof(buf));
                    GuestAddr pos = dest;
                    usize remaining = count;
                    while (remaining > 0) {
                        usize chunk = std::min(remaining, sizeof(buf));
                        (void)m.memory().write_bytes(pos, buf, chunk);
                        pos += chunk;
                        remaining -= chunk;
                    }
                }
            }
            m.set_return(m.arg(0));
        }});

    // ordinal 18: void* memcpy(void* dest, const void* src, size_t count)
    exports.entries.push_back(
        {"memcpy", 18, [&mgr](Marshaler& m) {
            (void)mgr;
            GuestAddr dest = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            usize count = static_cast<usize>(m.arg(2));
            if (count > 0) {
                Byte* dp = m.memory().host_ptr_mut(dest, count);
                const Byte* sp = m.memory().host_ptr(src, count);
                if (dp && sp) {
                    // Property apply can land float fields via CRT memcpy and
                    // bypass JIT store watches; optional value pattern catches
                    // that path (e.g. drawable UV slots).
                    const u32 pattern = vitreo::mem_watch_value_pattern();
                    if (pattern != 0 && count >= 4 &&
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
                    ::memcpy(dp, sp, count);
                    if (store_ring_enabled()) mem_watch_bytes(dest, dp, count, "crt.memcpy");
                } else {
                    // Cross-region fallback
                    GuestAddr src_pos = src, dst_pos = dest;
                    usize remaining = count;
                    Byte buf[4096];
                    while (remaining > 0) {
                        usize chunk = std::min(remaining, sizeof(buf));
                        (void)m.memory().read_bytes(src_pos, buf, chunk);
                        (void)m.memory().write_bytes(dst_pos, buf, chunk);
                        src_pos += chunk;
                        dst_pos += chunk;
                        remaining -= chunk;
                    }
                }
            }
            m.set_return(m.arg(0));
        }});

    // ordinal 19: void* memmove(void* dest, const void* src, size_t count)
    exports.entries.push_back(
        {"memmove", 19, [&mgr](Marshaler& m) {
            (void)mgr;
            GuestAddr dest = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            usize count = static_cast<usize>(m.arg(2));
            if (count > 0) {
                Byte* dp = m.memory().host_ptr_mut(dest, count);
                const Byte* sp = m.memory().host_ptr(src, count);
                if (dp && sp) {
                    ::memmove(dp, sp, count);
                    if (store_ring_enabled()) mem_watch_bytes(dest, dp, count, "crt.memmove");
                } else {
                    // Use a full temporary buffer for overlap-safe memmove
                    std::vector<Byte> buf(count);
                    (void)m.memory().read_bytes(src, buf.data(), count);
                    (void)m.memory().write_bytes(dest, buf.data(), count);
                }
            }
            m.set_return(m.arg(0));
        }});

    // ordinal 20: int memcmp(const void* buf1, const void* buf2, size_t count)
    exports.entries.push_back(
        {"memcmp", 20, [&mgr](Marshaler& m) {
            (void)mgr;
            GuestAddr buf1 = m.arg_ptr(0);
            GuestAddr buf2 = m.arg_ptr(1);
            usize count = static_cast<usize>(m.arg(2));
            int result = 0;
            if (count > 0) {
                const Byte* p1 = m.memory().host_ptr(buf1, count);
                const Byte* p2 = m.memory().host_ptr(buf2, count);
                if (p1 && p2) {
                    result = ::memcmp(p1, p2, count);
                } else {
                    usize safe_count = std::min(count, MAX_BULK_SIZE);
                    std::vector<Byte> a(safe_count);
                    std::vector<Byte> b(safe_count);
                    (void)m.memory().read_bytes(buf1, a.data(), safe_count);
                    (void)m.memory().read_bytes(buf2, b.data(), safe_count);
                    result = ::memcmp(a.data(), b.data(), safe_count);
                }
            }
            m.set_return(static_cast<u64>(static_cast<i64>(result)));
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
}

} // namespace vitreo::winapi
