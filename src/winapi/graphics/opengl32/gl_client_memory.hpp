#pragma once
/// @file gl_client_memory.hpp
/// @brief The one client-memory policy of opengl32 and glu32: how a command
///        reads or writes an application's buffer, and how big a host buffer
///        a guest's arguments may make it allocate.
///
/// 1. Ranges are computed in checked arithmetic (gl_pixel_layout,
///    glu::image_span). A range whose size overflows, or which would end past
///    the largest guest user address (128 TiB, x64 Windows' user space), is
///    no client memory at all.
/// 2. Before anything of a client range's size is allocated on the host, the
///    whole range must be committed guest memory with the access the command
///    needs: read for an image the command unpacks, read and write for one it
///    packs into (bytes between groups keep their values, so the client's
///    bytes are read first). Adjacent allocations are crossed as Windows reads
///    across them. Memory that fails the test is "unreadable": on Windows the
///    caller takes an access violation; Vitreo's GL commands then have no
///    effect and log once, glu32's image calls return GL_OUT_OF_MEMORY (what
///    glu32 returns when it cannot use its input).
/// 3. Every other host buffer whose size a guest argument sets (the groups of
///    a glDrawPixels image, GLU's working images) is allocated without
///    aborting: a buffer larger than the guest could ever commit (the host's
///    memory: GlobalMemoryStatusEx's commit limit, ullTotalPageFile; and,
///    when known, the guest's user address space) is refused before
///    allocating, and an allocation the host refuses is detected first
///    (gl_host_can_allocate: Vitreo builds without exceptions, so a failed
///    std::vector growth would terminate the process). Either is
///    GL_OUT_OF_MEMORY (GL 1.1 spec 2.5: "there is not enough memory left to
///    execute the command"; Windows' opengl32 and glu32 allocate their
///    working copies inside the process, so a larger one fails there too) or
///    GLU_OUT_OF_MEMORY.
/// 4. Per-thread scratch keeps at most kGlScratchRetainBytes between calls:
///    one large image does not pin its size for the thread's life.

#include <vitreo/common.hpp>
#include "core/memory/guest_memory.hpp"
#include "core/memory/memory_region.hpp"

#include <algorithm>
#include <new>
#include <span>
#include <vector>

namespace vitreo::winapi::gl {

/// The largest guest user address of any Windows process (x64: 128 TiB - 64 KiB
/// - 1, MmHighestUserAddress). A client range ending past it cannot exist.
inline constexpr u64 kGlLargestUserAddress = 0x00007FFFFFFEFFFFull;

/// Scratch a thread keeps between calls (rule 4).
inline constexpr usize kGlScratchRetainBytes = usize{4} << 20;

/// The outcome of touching a client range.
enum class GlClientStatus : u8 {
    Ok,
    Null,           ///< the pointer is NULL
    Unreadable,     ///< not committed guest memory with the needed access (rule 2)
    OutOfMemory,    ///< the host buffer it needs cannot be had (rule 3)
};

/// Rule 2: [addr, addr + size) lies in committed guest memory, every byte
/// with @p access (adjacent regions included). An empty range is accessible;
/// one that wraps or ends past kGlLargestUserAddress (or the memory's own
/// maximum user address, when set) is not.
[[nodiscard]] bool gl_client_range_accessible(const GuestMemory& memory, GuestAddr addr, usize size,
                                              MemoryProtection access);

/// Rule 3: the largest host working buffer, in bytes, a guest's arguments
/// may size: the host's memory, and the guest's user address space when
/// @p memory knows it.
[[nodiscard]] usize gl_working_limit(const GuestMemory* memory = nullptr) noexcept;

/// Rule 3: whether the host can give @p bytes now. Vitreo builds without
/// exceptions, so a std::vector growth the host refuses would terminate the
/// process; the request is first made with nothrow ::operator new (never
/// elided) and released, so a refusal is seen and reported instead.
[[nodiscard]] bool gl_host_can_allocate(usize bytes) noexcept;

/// Rule 3: room for @p count elements of T within gl_working_limit that the
/// host can give.
template <typename T>
[[nodiscard]] bool gl_working_size_ok(usize count, const GuestMemory* memory = nullptr) noexcept {
    return count <= gl_working_limit(memory) / sizeof(T) && gl_host_can_allocate(count * sizeof(T));
}

/// Rule 3: @p v resized to @p count elements, or false (with @p v empty)
/// when that is more than gl_working_limit allows or the host cannot
/// allocate it. Never aborts.
template <typename T>
[[nodiscard]] bool gl_try_resize(std::vector<T>& v, usize count, const GuestMemory* memory = nullptr) noexcept {
    if (count > v.capacity() && !gl_working_size_ok<T>(count, memory)) {
        v = {};
        return false;
    }
    v.resize(count);
    return true;
}

/// Rule 3 for a vector filled by push_back: capacity for @p count elements.
template <typename T>
[[nodiscard]] bool gl_try_reserve(std::vector<T>& v, usize count, const GuestMemory* memory = nullptr) noexcept {
    if (count > v.capacity() && !gl_working_size_ok<T>(count, memory)) return false;
    v.reserve(count);
    return true;
}

/// gl_try_resize, then every element @p value.
template <typename T>
[[nodiscard]] bool gl_try_assign(std::vector<T>& v, usize count, const T& value,
                                 const GuestMemory* memory = nullptr) noexcept {
    v.clear();
    if (!gl_try_resize(v, count, memory)) return false;
    std::fill(v.begin(), v.end(), value);
    return true;
}

/// Rule 4: drops @p v's storage when it holds more than kGlScratchRetainBytes.
template <typename T>
void gl_trim_scratch(std::vector<T>& v) noexcept {
    if (v.capacity() > kGlScratchRetainBytes / sizeof(T)) v = {};
}

/// Reads client range [addr, addr + size) into @p storage after rule 2
/// (@p access: Read for an unpack, ReadWrite for a pack destination, whose
/// current bytes are what the caller packs into).
[[nodiscard]] GlClientStatus gl_read_client(const GuestMemory& memory, GuestAddr addr, usize size,
                                            MemoryProtection access, std::vector<u8>& storage);

}  // namespace vitreo::winapi::gl
