#pragma once
/// @file glu32_image.hpp
/// @brief gluScaleImage's pixel pipeline as host functions: unpacking a
///        client image into 16-bit components, the GLU 1.2 filter, and
///        packing back (also the source and the levels of
///        gluBuild{1,2}DMipmaps).
///
/// Measured on winoracle (tools/oracle_probe/gl11_state_probe64.expected,
/// glu-scale-* lines):
///  - every component goes through 16 bits: GL_UNSIGNED_BYTE v * 257,
///    GL_BYTE v * 516, GL_UNSIGNED_SHORT v, GL_SHORT v * 2, GL_UNSIGNED_INT
///    v >> 16, GL_INT v >> 15, GL_FLOAT (int)(v * 65535), each kept modulo
///    2^16; back: >> 8, >> 9, v, >> 1, v * 65537, (v * 65537) >> 1, v / 65535
///    (glu-scale-convert-*). Colour-index and stencil formats keep the raw
///    value (glu-scale-colorindex-ubyte-2x1-to-3x1).
///  - GL_BITMAP: a set bit is 65535 (1 for an index format); packing sets a
///    bit for a value > 32767 (index: an odd value), and the other bits of a
///    byte keep their value (glu-scale-bitmap-*).
///  - an exact 2:1 shrink in both directions averages each 2x2 block with
///    +2 rounding (glu-scale-ushort-2x2-0-0-1-2-to-1x1); any other size is a
///    box filter over [x - c/2, x + c/2) per output pixel (c the input/output
///    ratio, or [x - 0.5, x + 0.5) when magnifying) that wraps around the
///    image edge and truncates, in single precision
///    (glu-scale-rgba-ubyte-2x2-to-4x4, -lum-ubyte-3x1-to-7x1).
///  - the client layout follows the PACK_* / UNPACK_* modes as glDrawPixels /
///    glReadPixels do (spec 3.6.4 / 4.3.2), including SWAP_BYTES and LSB_FIRST.

#include <vitreo/common.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace vitreo::winapi::glu {

/// A host buffer whose allocation may fail: glu32 mallocs its working images
/// and reports GLU_OUT_OF_MEMORY when malloc fails, whatever the size, so the
/// allocation is new (std::nothrow), never one that aborts. The contents are
/// not initialised: every user writes each element before reading it (the
/// client copies are read whole, unpack_image and box_filter_scale write every
/// component).
template <typename T>
class HostBuffer {
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);

public:
    /// @p count uninitialised elements; false (and empty) when the host has no room.
    [[nodiscard]] bool allocate(usize count) noexcept {
        m_data.reset();
        m_size = 0;
        if (count > SIZE_MAX / sizeof(T)) return false;
        const usize bytes = (count == 0 ? 1 : count) * sizeof(T);
        // ::operator new itself, not a new-expression, so the allocation is
        // never elided and its failure is always seen.
        void* raw = ::operator new(bytes, std::nothrow);
        if (raw == nullptr) return false;
        m_data.reset(static_cast<T*>(raw));
        m_size = count;
        return true;
    }
    [[nodiscard]] T* data() noexcept { return m_data.get(); }
    [[nodiscard]] const T* data() const noexcept { return m_data.get(); }
    [[nodiscard]] usize size() const noexcept { return m_size; }
    T& operator[](usize i) noexcept { return m_data.get()[i]; }
    const T& operator[](usize i) const noexcept { return m_data.get()[i]; }
    void swap(HostBuffer& other) noexcept {
        m_data.swap(other.m_data);
        std::swap(m_size, other.m_size);
    }

private:
    struct Release {
        void operator()(T* p) const noexcept { ::operator delete(p); }
    };
    std::unique_ptr<T, Release> m_data;
    usize m_size{0};
};

/// width * height * comps, or nullopt when the product does not fit the host's
/// address space (an allocation that can only fail).
[[nodiscard]] std::optional<usize> component_count(i32 width, i32 height, u32 comps) noexcept;

struct PixelModes {
    bool swap_bytes{false};
    bool lsb_first{false};
    i32 row_length{0};
    i32 skip_rows{0};
    i32 skip_pixels{0};
    i32 alignment{4};
};

/// The formats and types gluScaleImage accepts (others: GLU_INVALID_ENUM,
/// glu-scale-format-*, -typein-*, -typeout-*).
[[nodiscard]] bool legal_format(u32 format) noexcept;
[[nodiscard]] bool legal_type(u32 type) noexcept;
[[nodiscard]] u32 format_components(u32 format) noexcept;
[[nodiscard]] bool is_index_format(u32 format) noexcept;

/// Bytes from the image pointer through the last byte an image of
/// @p width x @p height touches under @p modes, in checked arithmetic:
/// nullopt when the offsets or the span do not fit 64 bits or the span is
/// larger than any user address space (no client memory can hold it;
/// gl_client_memory.hpp rule 1). 0 for an empty image.
[[nodiscard]] std::optional<usize> image_span(const PixelModes& modes, i32 width, i32 height, u32 format,
                                              u32 type) noexcept;

/// Reads the image at @p data (image_span bytes) into @p out, width * height
/// * components 16-bit values (component_count), rows bottom to top. Nothing
/// is read when image_span is nullopt.
void unpack_image(const PixelModes& modes, i32 width, i32 height, u32 format, u32 type, const u8* data,
                  u16* out);

/// Writes @p in into @p data (image_span bytes holding the client's current
/// bytes, so bytes and bits the image does not cover keep them). Nothing is
/// written when image_span is nullopt.
void pack_image(const PixelModes& modes, i32 width, i32 height, u32 format, u32 type, const u16* in, u8* data);

/// The GLU 1.2 filter from @p in (wi x hi) to @p out (wo x ho), @p comps
/// components per pixel. False, with @p out untouched, when the host has no
/// room for the filter's tables.
[[nodiscard]] bool box_filter_scale(u32 comps, i32 wi, i32 hi, const u16* in, i32 wo, i32 ho, u16* out);

/// gluBuild*DMipmaps' level-0 size: the power of two nearest @p value, a
/// value exactly between two powers going to the larger (3 -> 4, 5 -> 4,
/// 6 -> 8, 10 -> 8, 12 -> 16; glu-build2d-* level0). Wider than i32: 2^31 - 1
/// rounds to 2^31. 0 for a value below 1.
[[nodiscard]] i64 nearest_power(i32 value) noexcept;

}  // namespace vitreo::winapi::glu
