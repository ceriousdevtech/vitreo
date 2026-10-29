#pragma once

/// @file memory_region.hpp
/// @brief Memory region abstraction for guest memory management
///
/// Provides page-aligned memory regions with protection attributes.

#include <vitreo/common.hpp>

#include <memory>
#include <sys/mman.h>

namespace vitreo {

// =============================================================================
// Memory Protection Flags
// =============================================================================

/// Memory protection attributes
enum class MemoryProtection : u8 {
    None      = 0,
    Read      = 1 << 0,
    Write     = 1 << 1,
    Execute   = 1 << 2,
    ReadWrite = Read | Write,
    ReadExecute = Read | Execute,
    ReadWriteExecute = Read | Write | Execute,
};

/// Enable bitwise operations on MemoryProtection
inline constexpr MemoryProtection operator|(MemoryProtection a, MemoryProtection b) noexcept {
    return static_cast<MemoryProtection>(static_cast<u8>(a) | static_cast<u8>(b));
}

inline constexpr MemoryProtection operator&(MemoryProtection a, MemoryProtection b) noexcept {
    return static_cast<MemoryProtection>(static_cast<u8>(a) & static_cast<u8>(b));
}

inline constexpr bool has_flag(MemoryProtection prot, MemoryProtection flag) noexcept {
    return (static_cast<u8>(prot) & static_cast<u8>(flag)) == static_cast<u8>(flag);
}

/// Convert MemoryProtection to mmap PROT_* flags
inline int to_mmap_prot(MemoryProtection prot) noexcept {
    int flags = PROT_NONE;
    if (has_flag(prot, MemoryProtection::Read)) flags |= PROT_READ;
    if (has_flag(prot, MemoryProtection::Write)) flags |= PROT_WRITE;
    // Guest instructions are interpreted or translated into a separate
    // MAP_JIT arena. Writable guest code therefore needs writable host
    // backing, not a host RWX mapping forbidden by macOS W^X policy.
    if (has_flag(prot, MemoryProtection::Execute) &&
        !has_flag(prot, MemoryProtection::Write)) {
        flags |= PROT_EXEC;
    }
    return flags;
}

// =============================================================================
// MemoryRegion Class
// =============================================================================

/// A contiguous page-aligned memory region
///
/// Uses mmap for allocation to ensure page alignment and enable
/// memory protection changes.
class MemoryRegion {
public:
    /// Create an empty (invalid) region
    MemoryRegion() noexcept = default;
    
    /// Move constructor
    MemoryRegion(MemoryRegion&& other) noexcept;
    
    /// Move assignment
    MemoryRegion& operator=(MemoryRegion&& other) noexcept;
    
    /// No copy
    MemoryRegion(const MemoryRegion&) = delete;
    MemoryRegion& operator=(const MemoryRegion&) = delete;
    
    /// Destructor - unmaps memory
    ~MemoryRegion();
    
    /// Allocate a new memory region
    /// @param size Size in bytes (will be rounded up to page size)
    /// @param prot Initial protection attributes
    /// @return Result containing the region or error
    [[nodiscard]] static VResult<MemoryRegion> allocate(usize size, MemoryProtection prot = MemoryProtection::ReadWrite);

    /// Wrap an externally-owned host buffer WITHOUT taking ownership.
    /// The returned region will NOT munmap @p host_ptr on destruction; the
    /// caller (e.g. a shared-memory object) owns the lifetime. Used to bind
    /// cross-process shared memory (mmap MAP_SHARED) into the guest arena.
    [[nodiscard]] static MemoryRegion borrow(Byte* host_ptr, usize size,
                                             MemoryProtection prot) noexcept {
        return MemoryRegion(host_ptr, size, prot, /*owns=*/false);
    }

    /// True if this region owns (and will munmap) its backing memory.
    [[nodiscard]] bool owns() const noexcept { return owns_; }
    
    /// Check if this region is valid (has allocated memory)
    [[nodiscard]] bool is_valid() const noexcept { return data_ != nullptr; }
    
    /// Get pointer to start of region
    [[nodiscard]] Byte* data() noexcept { return data_; }
    [[nodiscard]] const Byte* data() const noexcept { return data_; }
    
    /// Get size of region in bytes
    [[nodiscard]] usize size() const noexcept { return size_; }
    
    /// Get current protection attributes
    [[nodiscard]] MemoryProtection protection() const noexcept { return protection_; }

    /// Record a protection value WITHOUT calling mprotect. Used by the linear
    /// memory model, which applies the actual (coarsened) mprotect separately
    /// via GuestMemory::linear_protect but still needs query_region/VirtualQuery
    /// to report the guest-requested protection.
    void set_protection_flag_only(MemoryProtection prot) noexcept { protection_ = prot; }
    
    /// Change protection attributes
    /// @param prot New protection attributes
    /// @return Success or error
    [[nodiscard]] VoidResult set_protection(MemoryProtection prot);
    
    /// Check if an offset is within the region
    [[nodiscard]] bool contains(usize offset) const noexcept {
        return offset < size_;
    }
    
    /// Check if a range is within the region
    [[nodiscard]] bool contains(usize offset, usize length) const noexcept {
        return offset < size_ && length <= size_ - offset;
    }
    
    /// Get pointer at offset (unsafe - no bounds checking)
    [[nodiscard]] Byte* ptr_at(usize offset) noexcept {
        return data_ + offset;
    }
    
    [[nodiscard]] const Byte* ptr_at(usize offset) const noexcept {
        return data_ + offset;
    }
    
    /// Release ownership of the memory (does not deallocate)
    Byte* release() noexcept;
    
private:
    /// Private constructor for allocation (owns=true) or borrowing (owns=false)
    MemoryRegion(Byte* data, usize size, MemoryProtection prot,
                 bool owns = true) noexcept;

    Byte* data_ = nullptr;
    usize size_ = 0;
    MemoryProtection protection_ = MemoryProtection::None;
    bool owns_ = true;  ///< When false, dtor/move do NOT munmap data_.
};

} // namespace vitreo
