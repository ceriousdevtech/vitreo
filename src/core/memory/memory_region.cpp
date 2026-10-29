/// @file memory_region.cpp
/// @brief Implementation of MemoryRegion

#include "memory_region.hpp"
#include "jit_region_registry.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace vitreo {

// =============================================================================
// MemoryRegion Implementation
// =============================================================================

MemoryRegion::MemoryRegion(Byte* data, usize size, MemoryProtection prot,
                           bool owns) noexcept
    : data_(data), size_(size), protection_(prot), owns_(owns) {}

MemoryRegion::MemoryRegion(MemoryRegion&& other) noexcept
    : data_(other.data_), size_(other.size_), protection_(other.protection_),
      owns_(other.owns_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.protection_ = MemoryProtection::None;
    other.owns_ = true;
}

MemoryRegion& MemoryRegion::operator=(MemoryRegion&& other) noexcept {
    if (this != &other) {
        // Free existing memory (only if we own it)
        if (data_ != nullptr && owns_) {
            munmap(data_, size_);
        }

        data_ = other.data_;
        size_ = other.size_;
        protection_ = other.protection_;
        owns_ = other.owns_;

        other.data_ = nullptr;
        other.size_ = 0;
        other.protection_ = MemoryProtection::None;
        other.owns_ = true;
    }
    return *this;
}

MemoryRegion::~MemoryRegion() {
    if (data_ != nullptr && owns_) {
        munmap(data_, size_);
    }
}

VResult<MemoryRegion> MemoryRegion::allocate(usize size, MemoryProtection prot) {
    if (size == 0) {
        return VResult<MemoryRegion>::err(Error::InvalidArgument);
    }
    
    // Round up to page size
    const usize page_size = static_cast<usize>(getpagesize());
    const usize aligned_size = align_up(size, page_size);
    
    int mmap_prot = to_mmap_prot(prot);

    // macOS ARM64: MAP_JIT and MAP_ANONYMOUS can return overlapping virtual
    // addresses.  If our allocation lands inside a JIT code slab we must NOT
    // munmap it (that would punch a hole in the JIT slab, destroying compiled
    // code).  Instead we deliberately leak the overlapping mapping and retry
    // with a hint past the JIT region.
    void* ptr = nullptr;
    for (int attempt = 0; attempt < 8; ++attempt) {
        void* hint = nullptr;
        if (attempt > 0) {
            // Hint past the highest JIT slab end.
            uintptr_t past = JitRegionRegistry::instance().max_end();
            if (past) {
                past += static_cast<uintptr_t>(attempt) * 16 * page_size;
                past = (past + page_size - 1) & ~(page_size - 1);
                hint = reinterpret_cast<void*>(past);
            }
        }
        void* p = mmap(hint, aligned_size, mmap_prot,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr,
                    "[mem] host mmap FAILED: size=%zu prot=%d attempt=%d errno=%d (%s)\n",
                    aligned_size, mmap_prot, attempt, errno, std::strerror(errno));
            return VResult<MemoryRegion>::err(Error::OutOfMemory);
        }
        uintptr_t start = reinterpret_cast<uintptr_t>(p);
        uintptr_t end = start + aligned_size;
        if (!JitRegionRegistry::instance().overlaps(start, end)) {
            if (attempt > 0) {
                fprintf(stderr, "[mem] guest alloc relocated to %p-0x%lx (attempt %d)\n",
                        p, end, attempt);
            }
            ptr = p;
            break;
        }
        // Do NOT munmap — that would destroy JIT code pages.
        // The leaked mapping is small and harmless.
        fprintf(stderr, "[mem] guest alloc %p-0x%lx overlaps JIT slab, leaking and retrying (attempt %d)\n",
                p, end, attempt);
    }

    if (!ptr) {
        fprintf(stderr,
                "[mem] guest alloc EXHAUSTED 8 attempts (size=%zu) — every mmap landed inside a JIT slab; reporting OOM\n",
                aligned_size);
        return VResult<MemoryRegion>::err(Error::OutOfMemory);
    }
    
    return VResult<MemoryRegion>::ok(MemoryRegion(static_cast<Byte*>(ptr), aligned_size, prot));
}

VoidResult MemoryRegion::set_protection(MemoryProtection prot) {
    if (data_ == nullptr) {
        return VoidResult::err(Error::InvalidArgument);
    }
    
    int mmap_prot = to_mmap_prot(prot);
    if (mprotect(data_, size_, mmap_prot) != 0) {
        return VoidResult::err(Error::AccessDenied);
    }
    
    protection_ = prot;
    return VoidResult::ok();
}

Byte* MemoryRegion::release() noexcept {
    Byte* ptr = data_;
    data_ = nullptr;
    size_ = 0;
    protection_ = MemoryProtection::None;
    return ptr;
}

} // namespace vitreo
