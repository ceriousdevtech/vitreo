/// @file theme_data.cpp
/// @brief Lookups over Aero's measured theme metrics.

#include "winapi/ui/uxtheme/theme_data.hpp"

namespace vitreo::winapi::uxtheme {

namespace {

#include "winapi/ui/uxtheme/theme_aero_metrics.inc"

char16_t fold(char16_t c) { return (c >= u'a' && c <= u'z') ? static_cast<char16_t>(c - 32) : c; }

bool equal_nocase(std::u16string_view a, std::u16string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (fold(a[i]) != fold(b[i])) return false;
    }
    return true;
}

}  // namespace

const VariantRecord& Variant::record() const { return kVariants[static_cast<usize>(index)]; }

std::span<const PartStateMetrics> Variant::part_states() const {
    const auto& r = record();
    return {kPartStates + r.ps_begin, r.ps_count};
}

std::span<const PropertyRecord> Variant::properties() const {
    const auto& r = record();
    return {kProperties + r.prop_begin, r.prop_count};
}

const PartStateMetrics* find_part_state(Variant v, i32 part, i32 state) {
    if (!v.valid()) return nullptr;
    for (const auto& ps : v.part_states()) {
        if (ps.part == part && ps.state == state) return &ps;
    }
    return nullptr;
}

const PropertyRecord* find_property(Variant v, i32 part, i32 state, i32 id, Origin* origin) {
    if (!v.valid()) return nullptr;
    const PropertyRecord* at_part = nullptr;
    const PropertyRecord* at_class = nullptr;
    for (const auto& p : v.properties()) {
        if (p.id != id) continue;
        if (p.part == part && p.state == state && state >= 0) {
            if (origin) *origin = Origin::State;
            return &p;
        }
        if (p.part == part && p.state < 0) at_part = &p;
        if (p.part < 0) at_class = &p;
    }
    if (at_part) {
        if (origin) *origin = Origin::Part;
        return at_part;
    }
    if (at_class) {
        if (origin) *origin = Origin::Class;
        return at_class;
    }
    return nullptr;
}

Variant find_variant(std::u16string_view subapp, std::u16string_view cls) {
    i32 base = -1;
    for (usize i = 0; i < std::size(kVariants); ++i) {
        if (!equal_nocase(kVariants[i].cls, cls)) continue;
        if (!subapp.empty() && equal_nocase(kVariants[i].subapp, subapp)) return Variant{static_cast<i32>(i)};
        if (kVariants[i].subapp.empty()) base = static_cast<i32>(i);
    }
    return Variant{base};
}

const DpiDrawSize* find_dpi_draw_size(Variant v, i32 part, u32 dpi) {
    for (const auto& d : kDpiDrawSizes) {
        if (d.variant == v.index && d.part == part && static_cast<u32>(d.dpi) == dpi) return &d;
    }
    return nullptr;
}

const ThemeFontRecord& font_record(u16 index) { return kFonts[index]; }

std::span<const i32> int_list(u16 offset) {
    const i32 n = kIntLists[offset];
    return {kIntLists + offset + 1, static_cast<usize>(n)};
}

}  // namespace vitreo::winapi::uxtheme
