/// @file crt_aligned.cpp
/// @brief _aligned_malloc / _aligned_offset_malloc and the rest of the family.
///
/// Contract: the Windows SDK's UCRT source, ucrt/heap/align.cpp. The block
/// layout is part of it: malloc'd base | gap | pointer to base | aligned
/// data, with the base pointer stored in the pointer-sized, pointer-aligned
/// cell just below `(data - gap)`, where gap = (0 - offset) & (PTR_SZ - 1).
/// Code that walks that header (debug heaps, custom allocators layered on
/// _aligned_malloc) sees the same thing it sees on Windows. PTR_SZ is the
/// guest's pointer size. Parameter checks are _VALIDATE_RETURN (errno EINVAL
/// plus the invalid-parameter handler); size overflow is _VALIDATE_RETURN_NOEXC
/// (errno ENOMEM only).
///
/// _expand is not available on Vitreo's CRT heap, so _aligned_offset_realloc
/// always takes the algorithm's allocate-and-move path -- the same path
/// Windows takes whenever _expand declines, which it may do at any time.

#include "winapi/msvcrt/memory/crt_memory.hpp"

#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 kEINVAL = 22;
constexpr u32 kENOMEM = 12;

bool is_pow2(u64 x) { return x != 0 && (x & (x - 1)) == 0; }

struct Aligned {
    Marshaler& m;
    CrtMemoryManager& mm;
    u64 ptr_sz;

    /// size_t arithmetic wraps at the guest's width.
    u64 mask() const { return ptr_sz == 8 ? ~0ULL : 0xFFFFFFFFULL; }

    u64 fail_invalid() const {
        mm.set_errno(kEINVAL);
        (void)mm.invalid_parameter();
        return 0;
    }

    /// The base pointer cell of an aligned block.
    u64 header_cell(u64 block) const { return (block & ~(ptr_sz - 1)) - ptr_sz; }
    u64 base_of(u64 block) const {
        return m.read_ptr(static_cast<GuestAddr>(header_cell(block))).value_or(0);
    }
    void copy(u64 dst, u64 src, u64 n) const {
        if (n == 0) return;
        std::vector<Byte> tmp(static_cast<usize>(n));
        if (m.memory().read_bytes(static_cast<GuestAddr>(src), tmp.data(), tmp.size()).is_ok()) {
            (void)m.memory().write_bytes(static_cast<GuestAddr>(dst), tmp.data(), tmp.size());
        }
    }

    u64 offset_malloc(u64 size, u64 align, u64 offset) const {
        if (!is_pow2(align)) return fail_invalid();
        if (!(offset == 0 || offset < size)) return fail_invalid();
        align = std::max(align, ptr_sz) - 1;
        const u64 gap = (0 - offset) & (ptr_sz - 1);
        const u64 nonuser = ptr_sz + gap + align;
        const u64 block_size = (nonuser + size) & mask();
        if (size > block_size) {
            mm.set_errno(kENOMEM);
            return 0;
        }
        const u64 base = mm.crt_malloc(static_cast<usize>(block_size));
        if (base == 0) return 0;
        const u64 ret = ((base + nonuser + offset) & ~align) - offset;
        (void)m.write_ptr(static_cast<GuestAddr>(ret - gap - ptr_sz), base);
        return ret;
    }

    void free_block(u64 block) const {
        if (block == 0) return;
        mm.crt_free(static_cast<GuestAddr>(base_of(block)));
    }

    u64 offset_realloc(u64 block, u64 size, u64 align, u64 offset) const {
        if (block == 0) return offset_malloc(size, align, offset);
        if (size == 0) {
            free_block(block);
            return 0;
        }
        if (!is_pow2(align)) return fail_invalid();
        if (!(offset == 0 || offset < size)) return fail_invalid();
        const u64 stptr = base_of(block);
        align = std::max(align, ptr_sz) - 1;
        const u64 gap = (0 - offset) & (ptr_sz - 1);
        const u64 diff = block - stptr;
        u64 movsz = static_cast<u64>(mm.crt_block_size(static_cast<GuestAddr>(stptr))) - diff;
        movsz = std::min(movsz, size);
        const u64 reqsz = (ptr_sz + gap + align + size) & mask();
        if (size > reqsz) {
            mm.set_errno(kENOMEM);
            return 0;
        }
        const u64 fresh = mm.crt_malloc(static_cast<usize>(reqsz));
        if (fresh == 0) return 0;
        const u64 ret = ((fresh + ptr_sz + gap + align + offset) & ~align) - offset;
        copy(ret, stptr + diff, movsz);
        mm.crt_free(static_cast<GuestAddr>(stptr));
        (void)m.write_ptr(static_cast<GuestAddr>(ret - gap - ptr_sz), fresh);
        return ret;
    }

    /// _aligned_msize_base; (size_t)-1 on a null block.
    u64 msize(u64 block, u64 align, u64 offset) const {
        if (block == 0) {
            (void)fail_invalid();
            return ptr_sz == 8 ? ~0ULL : 0xFFFFFFFFULL;
        }
        const u64 base = base_of(block);
        const u64 total = mm.crt_block_size(static_cast<GuestAddr>(base));
        const u64 header = block - base;
        const u64 gap = (0 - offset) & (ptr_sz - 1);
        align = std::max(align, ptr_sz) - 1;
        const u64 footer = gap + align + ptr_sz - header;
        return total - header - footer;
    }

    u64 offset_recalloc(u64 block, u64 count, u64 size, u64 align, u64 offset) const {
        // _HEAP_MAXREQ (corecrt_malloc.h): SIZE_MAX & ~0x1F.
        const u64 heap_maxreq = (ptr_sz == 8 ? ~0ULL : 0xFFFFFFFFULL) & ~u64{0x1F};
        if (count > 0 && heap_maxreq / count < size) {
            mm.set_errno(kENOMEM);
            return 0;
        }
        const u64 user = size * count;
        u64 start_fill = 0;
        if (block != 0) start_fill = msize(block, align, offset);
        const u64 ret = offset_realloc(block, user, align, offset);
        if (ret != 0 && start_fill < user) {
            std::vector<Byte> zeros(static_cast<usize>(user - start_fill), Byte{0});
            (void)m.memory().write_bytes(static_cast<GuestAddr>(ret + start_fill),
                                         zeros.data(), zeros.size());
        }
        return ret;
    }
};

} // namespace

void add_crt_aligned_exports(DllExports& exports, CrtMemoryManager& mm) {
    auto add = [&exports, &mm](const char* name,
                               u64 (*fn)(const Aligned&, Marshaler&)) {
        exports.entries.push_back({name, 0, [fn, &mm](Marshaler& m) {
            const Aligned a{m, mm, m.cpu_is_64bit() ? 8u : 4u};
            m.set_return(fn(a, m));
        }, 0});
    };
    // void* _aligned_malloc(size_t size, size_t alignment)
    add("_aligned_malloc", [](const Aligned& a, Marshaler& m) {
        return a.offset_malloc(m.arg(0), m.arg(1), 0);
    });
    // void* _aligned_offset_malloc(size_t size, size_t alignment, size_t offset)
    add("_aligned_offset_malloc", [](const Aligned& a, Marshaler& m) {
        return a.offset_malloc(m.arg(0), m.arg(1), m.arg(2));
    });
    // void* _aligned_realloc(void* block, size_t size, size_t alignment)
    add("_aligned_realloc", [](const Aligned& a, Marshaler& m) {
        return a.offset_realloc(m.arg_ptr(0), m.arg(1), m.arg(2), 0);
    });
    // void* _aligned_offset_realloc(void*, size_t size, size_t alignment,
    //                               size_t offset)
    add("_aligned_offset_realloc", [](const Aligned& a, Marshaler& m) {
        return a.offset_realloc(m.arg_ptr(0), m.arg(1), m.arg(2), m.arg(3));
    });
    // void* _aligned_recalloc(void*, size_t count, size_t size, size_t alignment)
    add("_aligned_recalloc", [](const Aligned& a, Marshaler& m) {
        return a.offset_recalloc(m.arg_ptr(0), m.arg(1), m.arg(2), m.arg(3), 0);
    });
    // void* _aligned_offset_recalloc(void*, size_t count, size_t size,
    //                                size_t alignment, size_t offset)
    add("_aligned_offset_recalloc", [](const Aligned& a, Marshaler& m) {
        return a.offset_recalloc(m.arg_ptr(0), m.arg(1), m.arg(2), m.arg(3), m.arg(4));
    });
    // size_t _aligned_msize(void* block, size_t alignment, size_t offset)
    add("_aligned_msize", [](const Aligned& a, Marshaler& m) {
        return a.msize(m.arg_ptr(0), m.arg(1), m.arg(2));
    });
    // void _aligned_free(void* block)
    add("_aligned_free", [](const Aligned& a, Marshaler& m) {
        a.free_block(m.arg_ptr(0));
        return u64{0};
    });
}

} // namespace vitreo::winapi
