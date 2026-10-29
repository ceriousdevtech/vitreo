#pragma once

#include <vitreo/common.hpp>

#include <array>

namespace vitreo::graphics {

struct PaletteEntry {
    u8 red{0};
    u8 green{0};
    u8 blue{0};
    u8 flags{0};
};

class IndexedColorTable {
public:
    static constexpr u32 ENTRY_COUNT = 256;

    [[nodiscard]] bool set_entries(u32 start, u32 count,
                                   const PaletteEntry* entries) noexcept {
        if (!entries || start > ENTRY_COUNT || count > ENTRY_COUNT - start) {
            return false;
        }
        for (u32 index = 0; index < count; ++index) {
            m_entries[start + index] = entries[index];
        }
        ++m_generation;
        return true;
    }

    [[nodiscard]] const PaletteEntry* entries(u32 start, u32 count) const noexcept {
        if (start > ENTRY_COUNT || count > ENTRY_COUNT - start) return nullptr;
        return m_entries.data() + start;
    }

    [[nodiscard]] u64 generation() const noexcept { return m_generation; }

private:
    std::array<PaletteEntry, ENTRY_COUNT> m_entries{};
    u64 m_generation{0};
};

} // namespace vitreo::graphics