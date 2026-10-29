#pragma once

#include <vitreo/common.hpp>
#include "graphics/contracts/indexed_color_table.hpp"

#include <memory>

namespace vitreo::graphics {

class CpuSurface {
public:
    CpuSurface(u32 width, u32 height, u32 bits_per_pixel, u32 pitch)
        : m_width(width), m_height(height), m_bits_per_pixel(bits_per_pixel),
          m_pitch(pitch) {}

    void set_palette(std::shared_ptr<IndexedColorTable> palette) noexcept {
        m_palette = std::move(palette);
        ++m_generation;
    }
    void mark_dirty() noexcept { ++m_generation; }

    [[nodiscard]] u32 width() const noexcept { return m_width; }
    [[nodiscard]] u32 height() const noexcept { return m_height; }
    [[nodiscard]] u32 bits_per_pixel() const noexcept { return m_bits_per_pixel; }
    [[nodiscard]] u32 pitch() const noexcept { return m_pitch; }
    [[nodiscard]] u64 generation() const noexcept { return m_generation; }
    [[nodiscard]] const std::shared_ptr<IndexedColorTable>& palette() const noexcept {
        return m_palette;
    }

private:
    u32 m_width;
    u32 m_height;
    u32 m_bits_per_pixel;
    u32 m_pitch;
    u64 m_generation{0};
    std::shared_ptr<IndexedColorTable> m_palette;
};

} // namespace vitreo::graphics