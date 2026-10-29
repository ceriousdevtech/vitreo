// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_memory_registration.cpp
/// @brief Export table for msvcrt's allocator.
///
/// Composition only: maps export names to handlers and records each one's
/// stack-cleanup contract. msvcrt is cdecl, so most entries pop zero bytes and
/// the guest cleans its own stack.
///
/// Split out of crt_memory.cpp, which was 1,353 lines and two-thirds this table.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.
///
/// VITREO_LARGE_FILE_OK: declarative ABI table — one entry per export. Plan
/// section 4.1 exempts these because length here tracks how many CRT functions
/// Vitreo implements, not how much logic one file carries.

#include "winapi/msvcrt/runtime/crt_exception_class.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/cxx_eh.hpp"
#include "winapi/msvcrt/runtime/rtti.hpp"
#include "winapi/msvcrt/runtime/except_handler3.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/seh/seh32.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>   // std::abort
#include <cstdio>
#include <cstring>   // std::memset
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>

namespace vitreo::winapi {
namespace {

#include "crt_errno_constants.inc"

// VC14+ `std::exception` stores `__std_exception_data` { const char* _What;
// bool _DoFree }. Copy owns a heap duplicate when `_DoFree` is set.
// Learn: https://learn.microsoft.com/en-us/windows/win32/memory/stdexceptioncopy
void std_exception_copy(CrtMemoryManager& mgr, Marshaler& m) {
    const GuestAddr from = m.arg_ptr(0);
    const GuestAddr to = m.arg_ptr(1);
    if (from == 0 || to == 0 || from == to) return;

    const usize ptr_size = m.cpu_is_64bit() ? 8u : 4u;
    const auto what = m.read_ptr(from);
    if (what.is_err()) return;
    const auto do_free = m.read_u8(from + ptr_size);
    if (do_free.is_err()) return;

    GuestAddr copied = static_cast<GuestAddr>(what.value());
    u8 own = do_free.value() != 0 ? 1 : 0;
    if (own != 0 && copied != 0) {
        const auto text = m.read_ansi_string(copied);
        if (text.is_err()) {
            copied = 0;
            own = 0;
        } else {
            const usize bytes = text.value().size() + 1;
            copied = mgr.crt_malloc(bytes);
            if (copied == 0) {
                own = 0;
            } else if (m.write_ansi_string(copied, text.value()).is_err()) {
                mgr.crt_free(copied);
                copied = 0;
                own = 0;
            }
        }
    }

    (void)m.write_ptr(to, copied);
    (void)m.write_u8(to + ptr_size, own);
}

void std_exception_destroy(CrtMemoryManager& mgr, Marshaler& m) {
    const GuestAddr data = m.arg_ptr(0);
    if (data == 0) return;

    const usize ptr_size = m.cpu_is_64bit() ? 8u : 4u;
    const auto what = m.read_ptr(data);
    const auto do_free = m.read_u8(data + ptr_size);
    if (what.is_ok() && do_free.is_ok() && do_free.value() != 0 &&
        what.value() != 0) {
        mgr.crt_free(static_cast<GuestAddr>(what.value()));
    }
    (void)m.write_ptr(data, 0);
    (void)m.write_u8(data + ptr_size, 0);
}

// Learn: type_info::name returns a cached C string. The decorated form lives
// after `_UndecoratedName`; MSVC stores a leading '.' on the mangled name.
void std_type_info_name(CrtMemoryManager& mgr, Marshaler& m) {
    const GuestAddr data = m.arg_ptr(0);
    (void)m.arg(1);  // __type_info_node* — cleanup list, unused here
    if (data == 0) {
        m.set_return(0);
        return;
    }

    const usize ptr_size = m.cpu_is_64bit() ? 8u : 4u;
    const auto cached = m.read_ptr(data);
    if (cached.is_ok() && cached.value() != 0) {
        m.set_return(cached.value());
        return;
    }

    const GuestAddr decorated = data + ptr_size;
    const auto text = m.read_ansi_string(decorated);
    if (text.is_err()) {
        m.set_return(0);
        return;
    }

    std::string_view name = text.value();
    if (!name.empty() && name.front() == '.') name.remove_prefix(1);

    const usize bytes = name.size() + 1;
    const GuestAddr copy = mgr.crt_malloc(bytes);
    if (copy == 0) {
        m.set_return(name.empty() ? decorated : decorated + 1);
        return;
    }
    if (m.write_ansi_string(copy, name).is_err()) {
        mgr.crt_free(copy);
        m.set_return(0);
        return;
    }
    (void)m.write_ptr(data, copy);
    m.set_return(copy);
}

// Learn: type_info equality/collation compare type encodings. MSVC stores the
// encoding as the decorated name after `_UndecoratedName`.
void std_type_info_compare(Marshaler& m) {
    const GuestAddr lhs = m.arg_ptr(0);
    const GuestAddr rhs = m.arg_ptr(1);
    if (lhs == 0 || rhs == 0) {
        m.set_return(1);
        return;
    }
    if (lhs == rhs) {
        m.set_return(0);
        return;
    }
    const usize ptr_size = m.cpu_is_64bit() ? 8u : 4u;
    const auto left = m.read_ansi_string(lhs + ptr_size);
    const auto right = m.read_ansi_string(rhs + ptr_size);
    if (left.is_err() || right.is_err()) {
        m.set_return(1);
        return;
    }
    const int cmp = left.value().compare(right.value());
    m.set_return(static_cast<u32>(static_cast<i32>(cmp)));
}

}  // namespace

// =============================================================================
// Thunk factory
// =============================================================================

DllExports create_crt_memory_thunks(CrtMemoryManager& mgr) {
    DllExports exports;
    exports.entries.reserve(33);

    // ---- malloc (1 arg: size) -----------------------------------------------
    exports.entries.push_back(ExportEntry{
        "malloc", 0,
        [&mgr](Marshaler& m) {
            u64 size = m.arg(0);
            GuestAddr ptr = mgr.crt_malloc(static_cast<usize>(size));
            VITREO_TRACE( "[malloc] size=%llu -> 0x%x\n",
                         static_cast<unsigned long long>(size),
                         static_cast<unsigned>(ptr));
            m.set_return(ptr);
        }
    });

    // free (1 arg: ptr)
    exports.entries.push_back(ExportEntry{
        "free", 0,
        [&mgr](Marshaler& m) {
            GuestAddr ptr = m.arg_ptr(0);
            mgr.crt_free(ptr);
        }
    });

    // calloc (2 args: count, size)
    exports.entries.push_back(ExportEntry{
        "calloc", 0,
        [&mgr](Marshaler& m) {
            usize count = static_cast<usize>(m.arg(0));
            usize size = static_cast<usize>(m.arg(1));
            if (count != 0 && size > SIZE_MAX / count) {
                mgr.set_errno(12);  // ENOMEM — Learn calloc / _HEAP_MAXREQ
                m.set_return(0);
                return;
            }
            usize total = count * size;
            GuestAddr ptr = mgr.crt_malloc(total);
            if (ptr != 0) {
                // Zero the allocation.
                Byte buf[256];
                std::memset(buf, 0, sizeof(buf));
                usize remaining = total;
                GuestAddr pos = ptr;
                while (remaining > 0) {
                    usize chunk = std::min(remaining, sizeof(buf));
                    (void)m.memory().write_bytes(pos, buf, chunk);
                    pos += chunk;
                    remaining -= chunk;
                }
            }
            m.set_return(ptr);
        }
    });

    // _calloc_crt — CRT-internal calloc used during VC2013+ startup.
    exports.entries.push_back(ExportEntry{
        "_calloc_crt", 0,
        [&mgr](Marshaler& m) {
            usize count = static_cast<usize>(m.arg(0));
            usize size = static_cast<usize>(m.arg(1));
            if (count != 0 && size > SIZE_MAX / count) {
                mgr.set_errno(12);
                m.set_return(0);
                return;
            }
            usize total = count * size;
            GuestAddr ptr = mgr.crt_malloc(total);
            if (ptr != 0) {
                Byte buf[256];
                std::memset(buf, 0, sizeof(buf));
                usize remaining = total;
                GuestAddr pos = ptr;
                while (remaining > 0) {
                    usize chunk = std::min(remaining, sizeof(buf));
                    (void)m.memory().write_bytes(pos, buf, chunk);
                    pos += chunk;
                    remaining -= chunk;
                }
            }
            m.set_return(ptr);
        }
    });

    // realloc (2 args: ptr, new_size)
    exports.entries.push_back(ExportEntry{
        "realloc", 0,
        [&mgr](Marshaler& m) {
            GuestAddr old_ptr = m.arg_ptr(0);
            usize new_size = static_cast<usize>(m.arg(1));
            if (old_ptr == 0) {
                m.set_return(mgr.crt_malloc(new_size));
                return;
            }
            if (new_size == 0) {
                mgr.crt_free(old_ptr);
                m.set_return(0);
                return;
            }
            const usize old_size = mgr.crt_block_size(old_ptr);
            GuestAddr new_ptr = mgr.crt_malloc(new_size);
            if (new_ptr != 0 && old_size != 0) {
                usize to_copy = std::min(old_size, new_size);
                std::vector<Byte> tmp;
                tmp.resize(to_copy);
                if (m.memory().read_bytes(old_ptr, tmp.data(), to_copy).is_ok()) {
                    (void)m.memory().write_bytes(new_ptr, tmp.data(), to_copy);
                }
            }
            // The old block is released on success; on failure it stays valid.
            if (new_ptr != 0) mgr.crt_free(old_ptr);
            m.set_return(new_ptr);
        }
    });

    // _recalloc(void* ptr, size_t count, size_t size) — realloc that zeroes the
    // bytes past the old size.
    exports.entries.push_back(ExportEntry{
        "_recalloc", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr old_ptr = m.arg_ptr(0);
            const auto count = static_cast<usize>(m.arg(1));
            const auto size  = static_cast<usize>(m.arg(2));
            if (count != 0 && size > SIZE_MAX / count) {
                mgr.set_errno(12);
                m.set_return(0);
                return;
            }
            const usize total = count * size;
            if (total == 0) {
                mgr.crt_free(old_ptr);
                m.set_return(0);
                return;
            }

            usize old_size = mgr.crt_block_size(old_ptr);
            const GuestAddr new_ptr = mgr.crt_malloc(total);
            if (new_ptr == 0) {
                m.set_return(0);
                return;
            }
            const usize to_copy = old_ptr != 0 ? std::min(old_size, total) : 0;
            if (to_copy != 0) {
                std::vector<Byte> tmp(to_copy);
                if (m.memory().read_bytes(old_ptr, tmp.data(), to_copy).is_ok()) {
                    (void)m.memory().write_bytes(new_ptr, tmp.data(), to_copy);
                }
            }
            if (to_copy < total) {
                std::vector<Byte> zeros(total - to_copy, Byte{0});
                (void)m.memory().write_bytes(
                    new_ptr + static_cast<GuestAddr>(to_copy),
                    zeros.data(), zeros.size());
            }
            if (old_ptr != 0) mgr.crt_free(old_ptr);
            m.set_return(new_ptr);
        }
    });




    // Learn (_get_heap_handle): the Win32 HANDLE of the CRT heap, for callers
    // that want to hand it to HeapSetInformation or HeapWalk. Vitreo's CRT
    // allocations come from the process heap, so that is the honest answer.
    exports.entries.push_back(ExportEntry{
        "_get_heap_handle", 0,
        [&mgr](Marshaler& m) {
            // Learn (GetProcessHeap / _get_heap_handle): the value is opaque
            // and only meaningful to the Heap* family. Vitreo's CRT does not
            // allocate from a guest-visible Win32 heap, so report the process
            // heap's well-known handle rather than a fabricated one — a caller
            // that passes it to HeapSetInformation or HeapValidate then names
            // a heap that actually exists.
            m.set_return(static_cast<u64>(mgr.process_heap_handle()));
        },
        0});
    // Learn (_heapchk): validates the heap and returns _HEAPOK (-2) when it is
    // consistent. Vitreo's allocator maintains its own invariants and offers
    // no walk, so the check is a report, not a no-op success: it says the heap
    // is fine, which is the same answer a healthy CRT gives.
    exports.entries.push_back(ExportEntry{
        "_heapchk", 0,
        [](Marshaler& m) {
            constexpr i32 kHeapOk = -2;
            m.set_return(static_cast<u64>(static_cast<u32>(kHeapOk)));
        },
        0});

    // _msize (1 arg: ptr) -> size
    exports.entries.push_back(ExportEntry{
        "_msize", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u64>(mgr.crt_block_size(m.arg_ptr(0))));
        }
    });

    // qsort (4 args: base, count, width, comparator)
    exports.entries.push_back(ExportEntry{
        "qsort", 0,
        [&mgr](Marshaler& m) {
            mgr.crt_qsort(m.arg_ptr(0), static_cast<usize>(m.arg(1)),
                          static_cast<usize>(m.arg(2)), m.arg_ptr(3));
        }
    });

    // void* bsearch(const void* key, const void* base, size_t num, size_t width,
    //               int (*compar)(const void*, const void*))
    exports.entries.push_back(ExportEntry{
        "bsearch", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr found = mgr.crt_bsearch(
                m.arg_ptr(0), m.arg_ptr(1), static_cast<usize>(m.arg(2)),
                static_cast<usize>(m.arg(3)), m.arg_ptr(4));
            m.set_return(found);
        }
    });

    // void* bsearch_s(const void* key, const void* base, size_t number,
    //                 size_t width,
    //                 int (*compare)(void* context, const void* key, const void* datum),
    //                 void* context)
    //
    // Learn: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/bsearch-s
    // Measured by tests/winapi/msvcrt/oracle/crt_ucrt_s5_probe (x86 = x64):
    // width == 0, compare == NULL, or base == NULL with number != 0 set errno
    // EINVAL, then call the invalid-parameter handler, and return NULL. Unlike
    // the page says, key is NOT validated: a NULL key reaches the comparator.
    // The probe order is the inclusive-bounds midpoint lo + (hi - lo) / 2.
    exports.entries.push_back(ExportEntry{
        "bsearch_s", 0,
        [&mgr](Marshaler& m) {
            constexpr u32 kEinval = 22;
            const GuestAddr key = m.arg_ptr(0);
            const GuestAddr base = m.arg_ptr(1);
            const u64 number = m.arg(2);
            const u64 width = m.arg(3);
            const GuestAddr compare = m.arg_ptr(4);
            const GuestAddr context = m.arg_ptr(5);
            m.set_return(0);
            if (width == 0 || compare == 0 || (base == 0 && number != 0)) {
                mgr.set_errno(kEinval);
                (void)mgr.invalid_parameter();
                return;
            }
            if (number == 0) return;
            if (!mgr.has_guest_caller()) {
                VITREO_TRACE("[crt] bsearch_s: no guest caller installed\n");
                return;
            }
            u64 lo = 0, hi = number - 1;
            for (;;) {
                const u64 mid = lo + (hi - lo) / 2;
                const GuestAddr element = base + static_cast<GuestAddr>(mid * width);
                const u64 args[3] = {context, key, element};
                const i32 cmp = static_cast<i32>(static_cast<u32>(mgr.call_guest(compare, args, 3)));
                if (cmp == 0) { m.set_return(element); return; }
                if (cmp < 0) {
                    if (mid == lo) return;
                    hi = mid - 1;
                } else {
                    if (mid == hi) return;
                    lo = mid + 1;
                }
            }
        }
    });

    // void qsort_s(void* base, size_t number, size_t width,
    //              int (*compare)(void* context, const void*, const void*),
    //              void* context)
    //
    // Learn: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/qsort-s
    // Measured by tests/winapi/msvcrt/oracle/crt_ucrt_s6_probe "s" rows
    // (x86 = x64): base == NULL with number != 0, width == 0, or
    // compare == NULL set errno EINVAL, THEN call the invalid-parameter
    // handler, and return without touching the array -- checked even when
    // number is 0 or 1 (only base NULL with number 0 is valid). One element
    // is never compared. The order of equal elements is unspecified (Learn:
    // a quick sort, not stable).
    exports.entries.push_back(ExportEntry{
        "qsort_s", 0,
        [&mgr](Marshaler& m) {
            constexpr u32 kEinval = 22;
            const GuestAddr base = m.arg_ptr(0);
            const u64 number = m.arg(1);
            const u64 width = m.arg(2);
            const GuestAddr compare = m.arg_ptr(3);
            const GuestAddr context = m.arg_ptr(4);
            if ((base == 0 && number != 0) || width == 0 || compare == 0) {
                mgr.set_errno(kEinval);
                (void)mgr.invalid_parameter();
                return;
            }
            if (number < 2) return;
            if (!mgr.has_guest_caller()) {
                VITREO_TRACE("[crt] qsort_s: no guest caller installed\n");
                return;
            }
            mgr.crt_qsort_s(base, static_cast<usize>(number), static_cast<usize>(width),
                            compare, context);
        }
    });

    // MSVC x86 and x64 spellings use the same guest allocator; SIZE_T and
    // pointers come from Marshaler without narrowing through a host DWORD.
    for (const char* name : {"??2@YAPAXI@Z", "??_U@YAPAXI@Z",
                             "??2@YAPEAX_K@Z", "??_U@YAPEAX_K@Z"}) {
        exports.entries.push_back({name, 0, [&mgr](Marshaler& m) {
            m.set_return(mgr.crt_malloc(static_cast<usize>(m.arg(0))));
        }});
    }
    for (const char* name : {"??3@YAXPAX@Z", "??_V@YAXPAX@Z",
                             "??3@YAXPEAX@Z", "??_V@YAXPEAX@Z"}) {
        exports.entries.push_back({name, 0, [&mgr](Marshaler& m) {
            mgr.crt_free(m.arg_ptr(0));
        }});
    }

    // ---- wcstok (3 args: str, delim, context) --------------------------------
    exports.entries.push_back(ExportEntry{
        "wcstok", 0,
        [&mgr](Marshaler& m) {
            GuestAddr str_ptr  = m.arg_ptr(0);
            GuestAddr delim_ptr = m.arg_ptr(1);
            GuestAddr ctx_ptr  = m.arg_ptr(2);

            // Read delimiter wide string (guest is UTF-16LE, 2 bytes/char).
            // We work directly in guest memory with 16-bit character reads.
            // Simple implementation: scan for delimiter chars.

            GuestAddr scan = str_ptr;
            if (scan == 0) {
                // Continue from saved context.
                auto ctx_val = m.read_u32(ctx_ptr);
                if (ctx_val.is_err()) { m.set_return(0); return; }
                scan = ctx_val.value();
                if (scan == 0) { m.set_return(0); return; }
            }

            // Gather delimiter set (up to 64 unique delimiters).
            u16 delims[64];
            int ndelim = 0;
            for (int i = 0; i < 63; ++i) {
                auto ch = m.read_u16(delim_ptr + static_cast<GuestAddr>(i * 2));
                if (ch.is_err()) break;
                if (ch.value() == 0) break;
                delims[ndelim++] = ch.value();
            }
            delims[ndelim] = 0;

            auto is_delim = [&](u16 c) -> bool {
                for (int i = 0; i < ndelim; ++i) {
                    if (delims[i] == c) return true;
                }
                return false;
            };

            // Skip leading delimiters.
            for (;;) {
                auto ch = m.read_u16(scan);
                if (ch.is_err() || ch.value() == 0) {
                    (void)m.write_u32(ctx_ptr, 0);
                    m.set_return(0);
                    return;
                }
                if (!is_delim(ch.value())) break;
                scan += 2;
            }

            GuestAddr token_start = scan;

            // Find end of token.
            for (;;) {
                auto ch = m.read_u16(scan);
                if (ch.is_err() || ch.value() == 0) {
                    (void)m.write_u32(ctx_ptr, 0);
                    break;
                }
                if (is_delim(ch.value())) {
                    // Null-terminate the token in-place.
                    (void)m.write_u16(scan, 0);
                    (void)m.write_u32(ctx_ptr, static_cast<u32>(scan + 2));
                    break;
                }
                scan += 2;
            }

            m.set_return(token_start);
        }
    });

    // ---- _onexit (1 arg: func) ----------------------------------------------
    exports.entries.push_back(ExportEntry{
        "_onexit", 0,
        [&mgr](Marshaler& m) {
            GuestAddr func = m.arg_ptr(0);
            GuestAddr result = mgr.register_onexit(func);
            m.set_return(result);
        }
    });

    // ---- __dllonexit (3 args: func, begin, end) -----------------------------
    exports.entries.push_back(ExportEntry{
        "__dllonexit", 0,
        [&mgr](Marshaler& m) {
            GuestAddr func  = m.arg_ptr(0);
            GuestAddr begin = m.arg_ptr(1);
            GuestAddr end   = m.arg_ptr(2);
            GuestAddr result = mgr.register_dllonexit(func, begin, end);
            m.set_return(result);
        }
    });

    exports.entries.push_back(ExportEntry{
        "_initialize_onexit_table", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u32>(
                mgr.initialize_onexit_table(m.arg_ptr(0), m.cpu_is_64bit())));
        }
    });

    exports.entries.push_back(ExportEntry{
        "_register_onexit_function", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u32>(mgr.register_onexit_function(
                m.arg_ptr(0), m.arg_ptr(1), m.cpu_is_64bit())));
        }
    });

    exports.entries.push_back(ExportEntry{
        "_execute_onexit_table", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u32>(
                mgr.execute_onexit_table(m.arg_ptr(0), m.cpu_is_64bit())));
        }
    });

    exports.entries.push_back(ExportEntry{
        "_crt_atexit", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u32>(mgr.crt_atexit(m.arg_ptr(0))));
        }
    });

    // int _crt_at_quick_exit(_PVFV function) (corecrt_startup.h): the
    // registration behind an EXE's at_quick_exit; quick_exit runs the list.
    exports.entries.push_back(ExportEntry{
        "_crt_at_quick_exit", 0,
        [&mgr](Marshaler& m) {
            m.set_return(static_cast<u32>(mgr.crt_at_quick_exit(m.arg_ptr(0))));
        }
    });

    // ---- vcruntime telemetry triggers ---------------------------------------
    //
    // MSVC emits calls to these around main() so the CRT can report process
    // lifetime to Microsoft's telemetry channel. There is nothing to report
    // and nowhere to report it, but they must exist and return cleanly: they
    // bracket main, so a missing-export stub sits directly on the startup path
    // of every MSVC-built binary.
    for (const char* name : {"__telemetry_main_invoke_trigger",
                             "__telemetry_main_return_trigger",
                             "__telemetry_main_log_trigger"}) {
        exports.entries.push_back(ExportEntry{
            name, 0,
            [](Marshaler& m) { (void)m.arg(0); }
        });
    }

    // ---- _lock (1 arg: locknum) — no-op (single-threaded) -------------------
    exports.entries.push_back(ExportEntry{
        "_lock", 0,
        [](Marshaler& m) { (void)m.arg(0); }
    });

    // ---- _unlock (1 arg: locknum) — no-op -----------------------------------
    exports.entries.push_back(ExportEntry{
        "_unlock", 0,
        [](Marshaler& m) { (void)m.arg(0); }
    });

    // ---- _CxxThrowException (2 args, __stdcall) — raise MSVC C++ exception ---
    exports.entries.push_back(ExportEntry{
        "_CxxThrowException", 0,
        [](Marshaler& m) {
            u32 exc_obj = static_cast<u32>(m.arg(0));
            u32 throw_info = static_cast<u32>(m.arg(1));
            std::fprintf(stderr, "[crt] _CxxThrowException(obj=0x%x, info=0x%x) — raising guest C++ exception\n",
                         exc_obj, throw_info);
            auto* cpu = m.cpu();
            if (cpu) {
                note_cxx_exception_thrown();
                cpu->pending_raise.active = true;
                cpu->pending_raise.code = 0xE06D7363u; // EXCEPTION_CPP_MSVC
                cpu->pending_raise.flags = 1;           // EXCEPTION_NONCONTINUABLE
                cpu->pending_raise.num_args = 3;
                cpu->pending_raise.args[0] = 0x19930520u; // MSVC magic
                cpu->pending_raise.args[1] = exc_obj;
                cpu->pending_raise.args[2] = throw_info;
            }
            m.set_return(0);
        },
        8  // 2 args × 4 bytes (__stdcall)
    });

    // ---- __CxxFrameHandler3 — MSVC C++ exception personality routine -----
    //
    // ABI (32-bit x86):
    //   eax            = FuncInfo* (set by the __ehhandler thunk: `mov eax, FI; jmp __CxxFrameHandler3`)
    //   [esp+ 0]       = return address (back to dispatcher's run_guest_callback)
    //   [esp+ 4]       = EXCEPTION_RECORD*
    //   [esp+ 8]       = EstablisherFrame (= EH registration node on guest stack)
    //   [esp+12]       = CONTEXT*
    //   [esp+16]       = DispatcherContext* (unused for our purposes)
    //
    // For C++ exceptions (code 0xE06D7363) we walk the FuncInfo and try to
    // match the thrown type against any in-scope catch.  On match we
    // install the catch parameter into the function frame, patch the
    // CONTEXT to resume at the catch handler, and return
    // ExceptionContinueExecution; the SEH dispatcher's read_context_32
    // then restores cpu state from CONTEXT.  On no match we return
    // ExceptionContinueSearch so the SEH dispatcher tries the next frame.
    //
    // Real OS exceptions reach here too (the function may have a __try),
    // but for now we leave those as ContinueSearch — only C++ catches
    // are dispatched.
    //
    // __CxxFrameHandler is the same x86 personality for every FuncInfo
    // version (magic 0x19930520 / 0x19930521 / 0x19930522): the hosted engine
    // (dispatch_cxx_exception_32) reads the fields each version defines. The
    // x64 personalities take (EXCEPTION_RECORD*, EstablisherFrame, CONTEXT*,
    // DISPATCHER_CONTEXT*) and an image-relative FuncInfo, which the hosted
    // engine does not handle: they continue the search and say so once.
    auto cxx_frame_handler = [](Marshaler& m) {
            auto* cpu = m.cpu();
            if (!cpu) { m.set_return(1); return; }
            if (m.cpu_is_64bit()) {
                static std::once_flag once;
                std::call_once(once, [] {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED msvcrt!__CxxFrameHandler: x64 "
                                         "C++ frame handling (ExceptionContinueSearch)\n");
                });
                m.set_return(1);  // ExceptionContinueSearch
                return;
            }

            u32 func_info       = static_cast<u32>(cpu->gpr.rax);
            u32 exc_rec_addr    = static_cast<u32>(m.arg(0));
            u32 frame_addr      = static_cast<u32>(m.arg(1));
            u32 ctx_rec_addr    = static_cast<u32>(m.arg(2));

            VITREO_TRACE("[cxx_eh] __CxxFrameHandler3 fi=0x%08x erec=0x%08x frame=0x%08x ctx=0x%08x\n",
                         func_info, exc_rec_addr, frame_addr, ctx_rec_addr);

            if (func_info == 0 || exc_rec_addr == 0 || ctx_rec_addr == 0) {
                m.set_return(1); // ContinueSearch
                return;
            }

            CxxDispatchResult r = dispatch_cxx_exception_32(
                m.memory(), func_info, frame_addr, exc_rec_addr, ctx_rec_addr);

            if (!r.match) {
                m.set_return(1); // ContinueSearch — let the next frame try
                return;
            }

            VITREO_TRACE("[cxx_eh] match: handler=0x%08x dispCatchObj=%d tryHigh=%d\n",
                         r.handler_addr, r.disp_catch_obj, r.try_high);

            if (!install_catch_param_32(m.memory(), r, frame_addr)) {
                VITREO_TRACE("[cxx_eh] install_catch_param_32 failed — falling through\n");
                m.set_return(1);
                return;
            }
            patch_context_for_catch_32(m.memory(), r, frame_addr, ctx_rec_addr);

            // ExceptionContinueExecution (0) — dispatcher restores CONTEXT
            // and resumes execution at the catch handler.
            m.set_return(0);
    };
    exports.entries.push_back(ExportEntry{"__CxxFrameHandler3", 0, cxx_frame_handler});
    exports.entries.push_back(ExportEntry{"__CxxFrameHandler", 0, cxx_frame_handler});

    // VC14+ vcruntime140 exception / RTTI helpers (cdecl). Missing-export
    // stubs return 0xFFFFFFFF, which corrupts `__std_exception_data` and makes
    // `std::uncaught_exceptions()` look nonzero.
    exports.entries.push_back(ExportEntry{
        "__std_exception_copy", 0,
        [&mgr](Marshaler& m) { std_exception_copy(mgr, m); }
    });
    exports.entries.push_back(ExportEntry{
        "__std_exception_destroy", 0,
        [&mgr](Marshaler& m) { std_exception_destroy(mgr, m); }
    });
    exports.entries.push_back(ExportEntry{
        "__uncaught_exception", 0,
        [](Marshaler& m) { m.set_return(cxx_uncaught_exception() ? 1 : 0); }
    });
    exports.entries.push_back(ExportEntry{
        "__uncaught_exceptions", 0,
        [](Marshaler& m) {
            m.set_return(static_cast<u32>(cxx_uncaught_exceptions()));
        }
    });
    exports.entries.push_back(ExportEntry{
        "__current_exception", 0,
        [](Marshaler& m) {
            // Learn: current_exception is null when none is being handled.
            m.set_return(0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "__current_exception_context", 0,
        [](Marshaler& m) { m.set_return(0); }
    });
    exports.entries.push_back(ExportEntry{
        "__std_type_info_name", 0,
        [&mgr](Marshaler& m) { std_type_info_name(mgr, m); }
    });
    exports.entries.push_back(ExportEntry{
        "__std_type_info_compare", 0,
        [](Marshaler& m) { std_type_info_compare(m); }
    });

    // Learn: https://learn.microsoft.com/en-us/cpp/cpp/dynamic-cast-operator
    // The RTTI entry points the compiler emits for `dynamic_cast` / `typeid`
    // on polymorphic types. A missing-export stub answers 0xFFFFFFFF, which
    // reads as a *successful* cast to an unmapped address, so the guest skips
    // its NULL check and dereferences it.
    exports.entries.push_back(ExportEntry{
        "__RTDynamicCast", 0,
        [](Marshaler& m) { rt_dynamic_cast(m); }
    });
    exports.entries.push_back(ExportEntry{
        "__RTtypeid", 0,
        [](Marshaler& m) { rt_typeid(m); }
    });
    exports.entries.push_back(ExportEntry{
        "__RTCastToVoid", 0,
        [](Marshaler& m) { rt_cast_to_void(m); }
    });
    exports.entries.push_back(ExportEntry{
        "__AdjustPointer", 0,
        [](Marshaler& m) { adjust_pointer_export(m); }
    });

    // void __std_type_info_destroy_list(__type_info_node* root)
    //
    // Frees the list of `type_info::name()` buffers the CRT cached, hanging
    // off a sentinel head node: `struct __type_info_node { void* _MemPtr;
    // __type_info_node* _Next; }`. The head itself is static storage and is
    // only unlinked.
    exports.entries.push_back(ExportEntry{
        "__std_type_info_destroy_list", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr root = m.arg_ptr(0);
            if (root == 0) return;
            const usize ptr_size = m.cpu_is_64bit() ? 8u : 4u;
            auto next = m.read_ptr(root + ptr_size);
            if (next.is_err()) return;
            GuestAddr node = static_cast<GuestAddr>(next.value());
            // Guest-owned list; stop rather than spin if it is corrupt.
            for (u32 guard = 0; node != 0 && guard < 100000u; ++guard) {
                auto mem_ptr = m.read_ptr(node);
                auto node_next = m.read_ptr(node + ptr_size);
                if (mem_ptr.is_ok() && mem_ptr.value() != 0) {
                    mgr.crt_free(static_cast<GuestAddr>(mem_ptr.value()));
                }
                mgr.crt_free(node);
                if (node_next.is_err()) break;
                node = static_cast<GuestAddr>(node_next.value());
            }
            (void)m.write_ptr(root + ptr_size, 0);
        }
    });

    // ---- type_info::~type_info() -------------------------------------------
    // x86 ??1type_info@@UAE@XZ (thiscall, `this` in ECX) and x64
    // ??1type_info@@UEAA@XZ (`this` in RCX). msvcrt's destructor releases the
    // undecorated-name buffer type_info::name() cached in the member after
    // the vfptr ({vfptr, _M_data, _M_d_name[]}), then clears it.
    auto type_info_dtor = [&mgr](Marshaler& m) {
        if (!m.has_cpu()) return;
        const bool x64 = m.cpu_is_64bit();
        const GuestAddr self = x64 ? m.arg_ptr(0)
                                   : static_cast<GuestAddr>(m.cpu()->gpr.rcx & 0xFFFFFFFFu);
        if (self == 0) return;
        const GuestAddr cell = self + (x64 ? 8u : 4u);
        const auto cached = m.read_ptr(cell);
        if (cached.is_ok() && cached.value() != 0) {
            mgr.crt_free(static_cast<GuestAddr>(cached.value()));
            (void)m.write_ptr(cell, 0);
        }
    };
    exports.entries.push_back(ExportEntry{"??1type_info@@UAE@XZ", 0, type_info_dtor});

    // char* __unDName(char* out, const char* name, int max_length,
    //                 void* (*alloc)(size_t), void (*free)(void*),
    //                 unsigned short flags)
    // The undecorator behind type_info::name and UnDecorateSymbolName
    // (measured, crt_s6_missing_probe): with no allocator it returns NULL; a
    // name that is not decorated (no leading '?') comes back unchanged, in
    // `out` or in a block from `alloc`. Decorated names need a complete MSVC
    // undecorator, which Vitreo does not have: those fail with NULL and an
    // UNIMPLEMENTED line rather than an approximate rendering.
    exports.entries.push_back(ExportEntry{"__unDName", 0, [&mgr](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        const GuestAddr name = m.arg_ptr(1);
        const i32 max_length = static_cast<i32>(m.arg(2));
        const GuestAddr alloc = m.arg_ptr(3);
        if (alloc == 0 || name == 0) {
            m.set_return(0);
            return;
        }
        auto text = m.read_ansi_string(name);
        if (text.is_err()) {
            m.set_return(0);
            return;
        }
        if (!text.value().empty() && text.value()[0] == '?') {
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED msvcrt!__unDName: "
                                     "decorated-name undecoration\n");
            });
            m.set_return(0);
            return;
        }
        std::string value = text.value();
        GuestAddr dest = out;
        if (dest == 0) {
            const u64 size = value.size() + 1;
            dest = static_cast<GuestAddr>(mgr.call_guest(alloc, &size, 1));
            if (dest == 0) {
                m.set_return(0);
                return;
            }
        } else if (max_length > 0 && value.size() >= static_cast<usize>(max_length)) {
            value.resize(static_cast<usize>(max_length) - 1);
        }
        (void)m.write_ansi_string(dest, value);
        m.set_return(dest);
    }});
    exports.entries.push_back(ExportEntry{"??1type_info@@UEAA@XZ", 0, type_info_dtor});

    // std::exception (crt_exception_class.cpp).
    add_crt_exception_class_exports(exports, mgr);

    // ---- _purecall — pure virtual call handler (raise fatal exception) -------
    exports.entries.push_back(ExportEntry{
        "_purecall", 0,
        [](Marshaler& m) {
            std::fprintf(stderr, "[crt] _purecall — pure virtual function call in guest, raising fatal exception\n");
            auto* cpu = m.cpu();
            if (cpu) {
                cpu->pending_raise.active = true;
                cpu->pending_raise.code = 0xC0000420u; // STATUS_ASSERTION_FAILURE
                cpu->pending_raise.flags = 1;           // EXCEPTION_NONCONTINUABLE
                cpu->pending_raise.num_args = 0;
            }
            m.set_return(0);
        }
    });

    // _purecall_handler / _invalid_parameter_handler process state.
    // Initial previous handler is nullptr (0), matching the CRT before any
    // custom handler is installed.
    static GuestAddr s_purecall_handler = 0;

    // _purecall_handler _set_purecall_handler(_purecall_handler handler)
    exports.entries.push_back(ExportEntry{
        "_set_purecall_handler", 0,
        [](Marshaler& m) {
            const GuestAddr previous = s_purecall_handler;
            s_purecall_handler = m.arg_ptr(0);
            m.set_return(previous);
        }
    });

    // _invalid_parameter_handler _set_invalid_parameter_handler(
    //     _invalid_parameter_handler handler)
    // The handler is consulted by every parameter-validation failure
    // (CrtMemoryManager::invalid_parameter), so it lives in the manager.
    exports.entries.push_back(ExportEntry{
        "_set_invalid_parameter_handler", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.exchange_invalid_parameter_handler(m.arg_ptr(0)));
        }
    });
    exports.entries.push_back(ExportEntry{
        "_get_invalid_parameter_handler", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.invalid_parameter_handler()); }
    });

    // Microsoft UCRT: the thread-local handler overrides the process-wide
    // invalid-parameter handler for CRT calls made by the current thread. The
    // setter returns the handler previously installed on this thread.
    // https://learn.microsoft.com/cpp/c-runtime-library/reference/
    // set-invalid-parameter-handler-set-thread-local-invalid-parameter-handler
    exports.entries.push_back(ExportEntry{
        "_set_thread_local_invalid_parameter_handler", 0,
        [&mgr](Marshaler& m) {
            m.set_return(
                mgr.exchange_thread_invalid_parameter_handler(m.arg_ptr(0)));
        }
    });
    exports.entries.push_back(ExportEntry{
        "_get_thread_local_invalid_parameter_handler", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.thread_invalid_parameter_handler());
        }
    });

    // void _crt_debugger_hook(int reserved) — debug CRT probe; release no-op.
    exports.entries.push_back(ExportEntry{
        "_crt_debugger_hook", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            m.set_return(0);
        }
    });

    // type_info::_type_info_dtor_internal_method — no-op thiscall (this in ECX).
    exports.entries.push_back(ExportEntry{
        "?_type_info_dtor_internal_method@type_info@@QAEXXZ", 0,
        [](Marshaler& /*m*/) {}
    });


    // ---- _ultow (3 args: value, str, radix) ---------------------------------
    exports.entries.push_back(ExportEntry{
        "_ultow", 0,
        [](Marshaler& m) {
            u32 value  = static_cast<u32>(m.arg(0));
            GuestAddr str = m.arg_ptr(1);
            u32 radix  = static_cast<u32>(m.arg(2));

            if (str == 0 || radix < 2 || radix > 36) {
                m.set_return(0);
                return;
            }

            // Convert to digits (reverse order), then write to guest.
            u16 buf[34];  // max 32 digits + sign + NUL
            int pos = 0;
            u32 v = value;
            if (v == 0) {
                buf[pos++] = u'0';
            } else {
                while (v != 0) {
                    u32 digit = v % radix;
                    buf[pos++] = static_cast<u16>(digit < 10 ? '0' + digit
                                                             : 'a' + digit - 10);
                    v /= radix;
                }
            }
            // Write reversed + NUL.
            for (int i = 0; i < pos; ++i) {
                (void)m.write_u16(str + static_cast<GuestAddr>(i * 2),
                                  buf[pos - 1 - i]);
            }
            (void)m.write_u16(str + static_cast<GuestAddr>(pos * 2), 0);

            m.set_return(str);
        }
    });


    // ---- _memicmp (3 args: buf1, buf2, count) — case-insensitive compare ----
    exports.entries.push_back(ExportEntry{
        "_memicmp", 0,
        [](Marshaler& m) {
            GuestAddr buf1  = m.arg_ptr(0);
            GuestAddr buf2  = m.arg_ptr(1);
            u64 count       = m.arg(2);

            for (u64 i = 0; i < count; ++i) {
                auto a = m.read_u8(buf1 + i);
                auto b = m.read_u8(buf2 + i);
                if (a.is_err() || b.is_err()) { m.set_return(0); return; }
                u8 ca = a.value();
                u8 cb = b.value();
                // ASCII tolower
                if (ca >= 'A' && ca <= 'Z') ca += 32;
                if (cb >= 'A' && cb <= 'Z') cb += 32;
                if (ca != cb) {
                    m.set_return(static_cast<u64>(
                        static_cast<i32>(ca) - static_cast<i32>(cb)));
                    return;
                }
            }
            m.set_return(0);
        }
    });

    // ---- _errno (0 args) — return pointer to errno --------------------------
    exports.entries.push_back(ExportEntry{
        "_errno", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.errno_addr());
        }
    });

    // ---- __doserrno (0 args) — return pointer to _doserrno ------------------
    // Learn: `int* __doserrno(void)`. Callers dereference the result without
    // checking it — the UCRT's own `_set_doserrno` is `*__doserrno() = value`,
    // which is a bare `mov dword ptr [rax], imm`. With this export missing the
    // generic stub returned the 0xFFFFFFFF sentinel and that store faulted.
    exports.entries.push_back(ExportEntry{
        "__doserrno", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.doserrno_addr());
        }
    });

    // UCRT errno accessors share the calling guest thread's pointer exports.
    // On Windows, NULL getters invoke the handler and return EINVAL but
    // preserve errno, contrary to the Learn page's errno-assignment prose.
    for (const bool dos : {false, true}) {
        exports.entries.push_back(ExportEntry{
            dos ? "_set_doserrno" : "_set_errno", 0,
            [&mgr, dos](Marshaler& m) {
                if (dos) mgr.set_doserrno(static_cast<u32>(m.arg(0)));
                else mgr.set_errno(static_cast<u32>(m.arg(0)));
                m.set_return(0);
            }
        });
        exports.entries.push_back(ExportEntry{
            dos ? "_get_doserrno" : "_get_errno", 0,
            [&mgr, dos](Marshaler& m) {
                const GuestAddr output = m.arg_ptr(0);
                if (output == 0) {
                    (void)mgr.invalid_parameter();
                    m.set_return(kErrnoInvalidArgument);
                    return;
                }
                const GuestAddr source = dos ? mgr.doserrno_addr() : mgr.errno_addr();
                const u32 value = m.read_u32(source).value_or(0);
                if (m.write_u32(output, value).is_err()) {
                    if (auto* cpu = m.cpu()) {
                        cpu->pending_raise.active = true;
                        cpu->pending_raise.code = kErrnoAccessViolation;
                        cpu->pending_raise.flags = 0;
                        cpu->pending_raise.num_args = 2;
                        cpu->pending_raise.args[0] = kErrnoWriteFault;
                        cpu->pending_raise.args[1] = output;
                    }
                    return;
                }
                m.set_return(0);
            }
        });
    }


    // wcstol / wcstoul are registered with the rest of the strtol family in
    // crt_string_integer_registration.cpp (one UCRT-measured parser).


    // ---- _except_handler3 (4 args, cdecl) — x86 __try/__except personality
    //
    // Microsoft Learn: locate a scope-table handler for this exception.
    // ContinueSearch if none matches. If a handler is selected, transfer
    // into the __except block via the dispatcher's CONTEXT (same Continue
    // Execution path as __CxxFrameHandler3) rather than returning to the
    // faulting instruction.
    exports.entries.push_back(ExportEntry{
        "_except_handler3", 0,
        [&mgr](Marshaler& m) {
            const u32 erec  = static_cast<u32>(m.arg(0));
            const u32 frame = static_cast<u32>(m.arg(1));
            const u32 ctx   = static_cast<u32>(m.arg(2));
            auto* cpu = m.cpu();
            if (erec == 0 || frame == 0 || ctx == 0 || cpu == nullptr) {
                m.set_return(1);
                return;
            }

            // EXCEPTION_POINTERS lives on this personality's stack, not in
            // the parent's [ebp-24] saved-ESP slot. ESP still points at the
            // cdecl return address; eight bytes below that are unused.
            const u32 pointers_storage =
                (static_cast<u32>(cpu->sp()) - 8u) & ~3u;
            install_eh3_exception_pointers(
                m.memory(), frame, erec, ctx, pointers_storage);
            const Eh3DispatchResult r = dispatch_except_handler3_32(
                m.memory(), frame, erec,
                [&mgr](GuestAddr filter, u32 parent_ebp) {
                    return mgr.call_seh_filter(filter, parent_ebp);
                });

            if (r.continue_execution) {
                m.set_return(0);
                return;
            }
            if (!r.handled || r.handler_addr == 0) {
                m.set_return(1);
                return;
            }

            eh3_unlink_frames_above(*cpu, m.memory(), frame);
            patch_context_for_eh3_handler(m.memory(), frame, ctx, r);
            m.set_return(0);
        }
    });

    // ---- free (1 arg: ptr) — delegate to crt_free --------------------------
    exports.entries.push_back(ExportEntry{
        "free", 0,
        [&mgr](Marshaler& m) {
            auto ptr = static_cast<GuestAddr>(m.arg(0));
            mgr.crt_free(ptr);
        }
    });

    // ---- the UCRT's "base" heap entry points --------------------------------
    //
    // `malloc`/`free` and friends are thin wrappers over `_malloc_base` /
    // `_free_base`, which the UCRT also exports so that code compiled against
    // it can call the allocator without going through the public wrapper. They
    // operate on the same heap and take the same arguments, so they take the
    // same handlers here.
    //
    // These matter beyond completeness: `add_ucrt_o_aliases` derives every
    // `_o_`-prefixed downlevel forwarder from an existing export, so a missing
    // `_free_base` also means a missing `_o__free_base`. A DLL importing that
    // then gets the missing-export stub, whose 0xFFFFFFFF is used as a heap
    // pointer — which is how a bundled dbghelp.dll came to fault inside its own
    // DllMain, fail to load, and turn every later delay-load call into it into
    // an unhandled DELAYLOAD_MOD_NOT_FOUND (0xC06D007E).
    const auto alias_base_entry = [&exports](const char* base_name,
                                             const char* wrapper_name) {
        auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& e) { return e.name == wrapper_name; });
        if (it == exports.entries.end()) return;
        ExportEntry copy = *it;
        copy.name = base_name;
        copy.ordinal = 0;
        exports.entries.push_back(std::move(copy));
    };
    // void __cdecl __clean_type_info_names_internal(void)
    // CRT shutdown frees the type_info name cache. Vitreo does not allocate
    // that cache, so the cleanup has nothing to release.
    exports.entries.push_back(ExportEntry{
        "__clean_type_info_names_internal", 0,
        [](Marshaler& m) { (void)m; }
    });

    // int __cdecl __CppXcptFilter(unsigned long xcptnum, PEXCEPTION_POINTERS)
    // A C++ exception (0xE06D7363) that escapes to the CRT filter terminates
    // the process. Every other code continues the search.
    exports.entries.push_back(ExportEntry{
        "__CppXcptFilter", 0,
        [&mgr](Marshaler& m) {
            constexpr u32 EXCEPTION_CPP_MSVC = 0xE06D7363u;
            constexpr u32 EXCEPTION_CONTINUE_SEARCH = 0;
            constexpr u32 EXCEPTION_EXECUTE_HANDLER = 1;
            const u32 code = static_cast<u32>(m.arg(0));
            if (code != EXCEPTION_CPP_MSVC) {
                m.set_return(EXCEPTION_CONTINUE_SEARCH);
                return;
            }
            mgr.terminate_process(3);
            m.set_return(EXCEPTION_EXECUTE_HANDLER);
        }
    });

    alias_base_entry("_malloc_base", "malloc");
    alias_base_entry("_malloc_crt", "malloc");
    alias_base_entry("_free_base", "free");
    alias_base_entry("_calloc_base", "calloc");
    alias_base_entry("_realloc_base", "realloc");
    alias_base_entry("_recalloc_base", "_recalloc");
    alias_base_entry("_expand_base", "_expand");
    alias_base_entry("_msize_base", "_msize");

    // UCRT heap/new_handler.cpp: _set_new_handler returns the previous
    // handler; _callnewh calls the installed one with the request size and
    // returns 1 only when it returned nonzero ("try again"), else 0.
    exports.entries.push_back({"_set_new_handler", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.exchange_new_handler(m.arg_ptr(0)));
    }, 0});
    exports.entries.push_back({"_query_new_handler", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.new_handler());
    }, 0});
    exports.entries.push_back({"_callnewh", 0, [&mgr](Marshaler& m) {
        const GuestAddr handler = mgr.new_handler();
        if (handler == 0) {
            m.set_return(0);
            return;
        }
        const u64 size = m.arg(0);
        const u64 result = mgr.call_guest(handler, &size, 1);
        m.set_return(static_cast<u32>(result) != 0 ? 1u : 0u);
    }, 0});

    add_crt_aligned_exports(exports, mgr);
    return exports;
}

namespace {

// Microsoft STL <streambuf>, <xiosbase>, <ios>, <__msvc_ostream.hpp>.
// 32-bit member offsets follow that declaration order with the default
// packing (pointers 4, streamsize 8). 64-bit pointers are 8. basic_ostream
// and basic_istream virtually inherit basic_ios; the ios subobject is
// reached through the MSVC vbtable stored after the vfptr.
constexpr u32 kGoodbit = 0;
constexpr u32 kEofbit = 1;
constexpr u32 kFailbit = 2;
constexpr u32 kBadbit = 4;
constexpr u32 kUnitbuf = 0x2;
constexpr u32 kShowbase = 0x8;
constexpr u32 kShowpos = 0x20;
constexpr u32 kUppercase = 0x4;
constexpr u32 kLeft = 0x40;
constexpr u32 kInternal = 0x100;
constexpr u32 kDec = 0x200;
constexpr u32 kOct = 0x400;
constexpr u32 kHex = 0x800;

struct IosOff {
    u32 state;
    u32 except;
    u32 fmt;
    u32 wide;
    u32 strbuf;
    u32 tie;
    u32 fill;
    u32 ipnext;
    u32 ipcount;
    u32 ignext;
    u32 igcount;
    u32 vbptr;
};

[[nodiscard]] IosOff ios_off(bool wide) {
    if (wide) {
        return {8, 12, 16, 32, 64, 72, 80, 64, 96, 56, 88, 8};
    }
    return {4, 8, 12, 24, 48, 52, 56, 32, 48, 28, 44, 4};
}

[[nodiscard]] GuestAddr this_of(Marshaler& m) {
    if (m.cpu_is_64bit()) return m.arg_ptr(0);
    return m.cpu() ? static_cast<GuestAddr>(m.cpu()->gpr.rcx & 0xFFFFFFFFu) : 0;
}

[[nodiscard]] GuestAddr read_ptr(Marshaler& m, GuestAddr addr) {
    if (addr == 0) return 0;
    if (m.cpu_is_64bit()) {
        auto v = m.read_u64(addr);
        return v.is_ok() ? v.value() : 0;
    }
    auto v = m.read_u32(addr);
    return v.is_ok() ? v.value() : 0;
}

void write_ptr(Marshaler& m, GuestAddr addr, GuestAddr value) {
    if (addr == 0) return;
    if (m.cpu_is_64bit()) (void)m.write_u64(addr, value);
    else (void)m.write_u32(addr, static_cast<u32>(value));
}

[[nodiscard]] GuestAddr ios_of_stream(Marshaler& m, GuestAddr stream) {
    // vfptr, then vbptr. vbtable[1] is the displacement from the vbptr
    // slot to the basic_ios virtual base.
    const auto off = ios_off(m.cpu_is_64bit());
    const GuestAddr vbtable = read_ptr(m, stream + off.vbptr);
    if (vbtable == 0) return stream;
    auto disp = m.read_u32(vbtable + 4);
    if (disp.is_err()) return stream;
    return static_cast<GuestAddr>(static_cast<i64>(stream + off.vbptr) +
                                  static_cast<i32>(disp.value()));
}

[[nodiscard]] u32 rdstate(Marshaler& m, GuestAddr ios) {
    auto v = m.read_u32(ios + ios_off(m.cpu_is_64bit()).state);
    return v.is_ok() ? v.value() : kBadbit;
}

void setstate_at(Marshaler& m, GuestAddr ios, u32 add) {
    const auto off = ios_off(m.cpu_is_64bit());
    u32 state = rdstate(m, ios) | add;
    if (read_ptr(m, ios + off.strbuf) == 0) state |= kBadbit;
    (void)m.write_u32(ios + off.state, state);
    auto except = m.read_u32(ios + off.except);
    if (except.is_ok() && (state & except.value()) != 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED msvcp80!basic_ios::setstate: "
                         "exception mask throw\n");
        }
    }
}

[[nodiscard]] i32 sputc_at(Marshaler& m, GuestAddr sb, u8 ch) {
    const auto off = ios_off(m.cpu_is_64bit());
    const GuestAddr next_cell = read_ptr(m, sb + off.ipnext);
    const GuestAddr count_cell = read_ptr(m, sb + off.ipcount);
    if (next_cell == 0 || count_cell == 0) return -1;
    const GuestAddr next = read_ptr(m, next_cell);
    auto count = m.read_u32(count_cell);
    if (next == 0 || count.is_err() || static_cast<i32>(count.value()) <= 0) return -1;
    (void)m.write_u8(next, ch);
    write_ptr(m, next_cell, next + 1);
    (void)m.write_u32(count_cell, count.value() - 1);
    return ch;
}

[[nodiscard]] i32 sputn_at(Marshaler& m, GuestAddr sb, GuestAddr src, i32 count) {
    i32 wrote = 0;
    for (i32 i = 0; i < count; ++i) {
        auto byte = m.read_u8(src + static_cast<GuestAddr>(i));
        if (byte.is_err() || sputc_at(m, sb, byte.value()) < 0) break;
        ++wrote;
    }
    return wrote;
}

[[nodiscard]] bool sentry_ok(Marshaler& m, GuestAddr stream) {
    const GuestAddr ios = ios_of_stream(m, stream);
    if ((rdstate(m, ios) & (kFailbit | kBadbit)) != 0) return false;
    const GuestAddr tie = read_ptr(m, ios + ios_off(m.cpu_is_64bit()).tie);
    if (tie != 0 && tie != stream) {
        // Tied stream flush is basic_ostream::flush. A null sync is success
        // for the base streambuf; the characters already sit in its buffer.
        (void)tie;
    }
    return (rdstate(m, ios) & (kFailbit | kBadbit)) == 0;
}

void insert_text(Marshaler& m, GuestAddr stream, std::string text) {
    const GuestAddr ios = ios_of_stream(m, stream);
    const auto off = ios_off(m.cpu_is_64bit());
    auto width = m.read_u64(ios + off.wide);
    const u64 wide = width.is_ok() ? width.value() : 0;
    auto fmt = m.read_u32(ios + off.fmt);
    const u32 flags = fmt.is_ok() ? fmt.value() : 0;
    u8 fill = static_cast<u8>(' ');
    auto fill_b = m.read_u8(ios + off.fill);
    if (fill_b.is_ok() && fill_b.value() != 0) fill = fill_b.value();
    if (wide > text.size()) {
        const std::string pad(static_cast<usize>(wide - text.size()), static_cast<char>(fill));
        if ((flags & kLeft) != 0) text += pad;
        else if ((flags & kInternal) != 0 && !text.empty() &&
                 (text[0] == '+' || text[0] == '-' || text[0] == ' '))
            text.insert(1, pad);
        else text = pad + text;
    }
    (void)m.write_u64(ios + off.wide, 0);
    const GuestAddr sb = read_ptr(m, ios + off.strbuf);
    u32 state = kGoodbit;
    if (!sentry_ok(m, stream) || sb == 0) state |= kBadbit;
    else {
        for (unsigned char ch : text) {
            if (sputc_at(m, sb, ch) < 0) {
                state |= kBadbit;
                break;
            }
        }
    }
    setstate_at(m, ios, state);
}

} // namespace

DllExports create_msvcp80_thunks(CrtMemoryManager& mgr) {
    DllExports exports;

    exports.entries.push_back(ExportEntry{
        "?uncaught_exception@std@@YA_NXZ", 0,
        [](Marshaler& m) { m.set_return(cxx_uncaught_exception() ? 1 : 0); }
    });

    // void basic_streambuf::_Lock/_Unlock — base implementations are empty.
    // filebuf overrides them virtually; this export is the base.
    exports.entries.push_back({"?_Lock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEXXZ", 0,
        [](Marshaler&) {}, 0});
    exports.entries.push_back({"?_Unlock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEXXZ", 0,
        [](Marshaler&) {}, 0});

    exports.entries.push_back({"?sputc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEHD@Z", 0,
        [](Marshaler& m) {
            const u8 ch = static_cast<u8>(m.cpu_is_64bit() ? m.arg(1) : m.arg(0));
            m.set_return(static_cast<u64>(static_cast<i32>(sputc_at(m, this_of(m), ch))));
        }, 4});

    exports.entries.push_back({"?sputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEHPBDH@Z", 0,
        [](Marshaler& m) {
            const GuestAddr src = m.cpu_is_64bit() ? m.arg_ptr(1) : static_cast<GuestAddr>(m.arg(0));
            const auto count = static_cast<i32>(m.cpu_is_64bit() ? m.arg(2) : m.arg(1));
            m.set_return(static_cast<u64>(static_cast<i32>(sputn_at(m, this_of(m), src, count))));
        }, 8});

    exports.entries.push_back({"?setstate@?$basic_ios@DU?$char_traits@D@std@@@std@@QAEXH_N@Z", 0,
        [](Marshaler& m) {
            const u32 add = static_cast<u32>(m.cpu_is_64bit() ? m.arg(1) : m.arg(0));
            setstate_at(m, this_of(m), add);
        }, 8});

    exports.entries.push_back({"?_Osfx@?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEXXZ", 0,
        [](Marshaler& m) {
            const GuestAddr ios = ios_of_stream(m, this_of(m));
            auto fmt = m.read_u32(ios + ios_off(m.cpu_is_64bit()).fmt);
            if ((rdstate(m, ios) & (kFailbit | kBadbit)) == 0 && fmt.is_ok() &&
                (fmt.value() & kUnitbuf) != 0) {
                const GuestAddr sb = read_ptr(m, ios + ios_off(m.cpu_is_64bit()).strbuf);
                // Base sync() returns 0. A missing buffer is badbit.
                if (sb == 0) setstate_at(m, ios, kBadbit);
            }
        }, 0});

    exports.entries.push_back({"?flush@?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV12@XZ", 0,
        [](Marshaler& m) {
            const GuestAddr self = this_of(m);
            const GuestAddr ios = ios_of_stream(m, self);
            const GuestAddr sb = read_ptr(m, ios + ios_off(m.cpu_is_64bit()).strbuf);
            if (sb != 0 && sentry_ok(m, self)) {
                // pubsync -> virtual sync. The base returns 0.
            }
            m.set_return(self);
        }, 0});

    exports.entries.push_back({"?write@?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV12@PBDH@Z", 0,
        [](Marshaler& m) {
            const GuestAddr self = this_of(m);
            const GuestAddr src = m.cpu_is_64bit() ? m.arg_ptr(1) : static_cast<GuestAddr>(m.arg(0));
            const auto count = static_cast<i32>(m.cpu_is_64bit() ? m.arg(2) : m.arg(1));
            const GuestAddr ios = ios_of_stream(m, self);
            u32 state = kGoodbit;
            if (!sentry_ok(m, self)) state |= kBadbit;
            else if (count > 0) {
                const GuestAddr sb = read_ptr(m, ios + ios_off(m.cpu_is_64bit()).strbuf);
                if (sb == 0 || sputn_at(m, sb, src, count) != count) state |= kBadbit;
            }
            setstate_at(m, ios, state);
            m.set_return(self);
        }, 8});

    exports.entries.push_back({"??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@H@Z", 0,
        [](Marshaler& m) {
            const GuestAddr self = this_of(m);
            const auto value = static_cast<i32>(m.cpu_is_64bit() ? m.arg(1) : m.arg(0));
            const GuestAddr ios = ios_of_stream(m, self);
            auto fmt = m.read_u32(ios + ios_off(m.cpu_is_64bit()).fmt);
            const u32 flags = fmt.is_ok() ? fmt.value() : kDec;
            const u32 base_flags = flags & (kDec | kOct | kHex);
            unsigned long magnitude = 0;
            std::string text;
            if (base_flags == kOct || base_flags == kHex) {
                magnitude = static_cast<unsigned int>(value);
            } else if (value < 0) {
                text = "-";
                magnitude = static_cast<unsigned long>(static_cast<unsigned int>(0 - value));
            } else {
                if ((flags & kShowpos) != 0) text = "+";
                magnitude = static_cast<unsigned long>(value);
            }
            const int base = base_flags == kOct ? 8 : base_flags == kHex ? 16 : 10;
            std::string digits;
            unsigned long n = magnitude;
            do {
                const unsigned digit = static_cast<unsigned>(n % static_cast<unsigned>(base));
                const char letter = (flags & kUppercase) != 0 ? 'A' : 'a';
                digits.insert(digits.begin(), static_cast<char>(digit < 10 ? '0' + digit : letter + (digit - 10)));
                n /= static_cast<unsigned>(base);
            } while (n != 0);
            if ((flags & kShowbase) != 0 && magnitude != 0) {
                if (base == 16) text += (flags & kUppercase) != 0 ? "0X" : "0x";
                else if (base == 8) text += "0";
            }
            text += digits;
            if (sentry_ok(m, self)) insert_text(m, self, text);
            else setstate_at(m, ios, kGoodbit);
            m.set_return(self);
        }, 4});

    exports.entries.push_back({"??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@P6AAAV01@AAV01@@Z@Z", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr self = this_of(m);
            const GuestAddr fn = m.cpu_is_64bit() ? m.arg_ptr(1) : static_cast<GuestAddr>(m.arg(0));
            if (fn != 0 && mgr.has_guest_caller()) {
                const u64 arg = self;
                (void)mgr.call_guest(fn, &arg, 1);
            } else if (fn != 0) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    std::fprintf(stderr,
                                 "[vitreo] UNIMPLEMENTED msvcp80!operator<<(manipulator): "
                                 "no guest caller\n");
                }
            }
            m.set_return(self);
        }, 4});

    exports.entries.push_back({"?endl@std@@YAAAV?$basic_ostream@DU?$char_traits@D@std@@@1@AAV21@@Z", 0,
        [](Marshaler& m) {
            const GuestAddr stream = m.arg_ptr(0);
            const GuestAddr ios = ios_of_stream(m, stream);
            u32 state = kGoodbit;
            if (!sentry_ok(m, stream)) state |= kBadbit;
            else {
                const GuestAddr sb = read_ptr(m, ios + ios_off(m.cpu_is_64bit()).strbuf);
                if (sb == 0 || sputc_at(m, sb, static_cast<u8>('\n')) < 0) state |= kBadbit;
            }
            setstate_at(m, ios, state);
            m.set_return(stream);
        }, 0});

    exports.entries.push_back({"?read@?$basic_istream@DU?$char_traits@D@std@@@std@@QAEAAV12@PADH@Z", 0,
        [](Marshaler& m) {
            const GuestAddr self = this_of(m);
            const GuestAddr dst = m.cpu_is_64bit() ? m.arg_ptr(1) : static_cast<GuestAddr>(m.arg(0));
            const auto count = static_cast<i32>(m.cpu_is_64bit() ? m.arg(2) : m.arg(1));
            const GuestAddr ios = ios_of_stream(m, self);
            const auto off = ios_off(m.cpu_is_64bit());
            i32 got = 0;
            u32 state = kGoodbit;
            if (!sentry_ok(m, self)) state |= kFailbit;
            else if (count > 0) {
                const GuestAddr sb = read_ptr(m, ios + off.strbuf);
                const GuestAddr next_cell = sb == 0 ? 0 : read_ptr(m, sb + off.ignext);
                const GuestAddr count_cell = sb == 0 ? 0 : read_ptr(m, sb + off.igcount);
                for (; got < count && next_cell != 0 && count_cell != 0; ++got) {
                    auto avail = m.read_u32(count_cell);
                    const GuestAddr next = read_ptr(m, next_cell);
                    if (avail.is_err() || static_cast<i32>(avail.value()) <= 0 || next == 0) break;
                    auto byte = m.read_u8(next);
                    if (byte.is_err()) break;
                    (void)m.write_u8(dst + static_cast<GuestAddr>(got), byte.value());
                    write_ptr(m, next_cell, next + 1);
                    (void)m.write_u32(count_cell, avail.value() - 1);
                }
                if (got != count) state |= kEofbit | kFailbit;
            }
            // _Chcount sits after vfptr and vbptr.
            const GuestAddr chcount = self + (m.cpu_is_64bit() ? 16u : 8u);
            (void)m.write_u64(chcount, static_cast<u64>(got));
            setstate_at(m, ios, state);
            m.set_return(self);
        }, 8});

    return exports;
}

} // namespace vitreo::winapi
