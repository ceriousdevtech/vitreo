#pragma once
/// @file kbd_layout.hpp
/// @brief The US keyboard layout (HKL 0x04090409) Vitreo reports, as data.
///
/// Every table is generated from Windows 11's own answers
/// (tools/oracle_probe/s2msg_kbdlayout_probe -> tools/generate_keyboard_layout.py
/// -> kbd_us_layout.inc): MapVirtualKeyEx for all five map types,
/// VkKeyScanExW, and ToUnicodeEx under every Shift/Ctrl/Alt/CapsLock
/// combination. MapVirtualKey*, VkKeyScan*, ToUnicode*/ToAscii* and
/// TranslateMessage all read these, so they cannot disagree.

#include <vitreo/common.hpp>

namespace vitreo::winapi::kbd_us {

struct VkRow {
    u8 vk;
    u16 vsc;     ///< MAPVK_VK_TO_VSC
    u16 ch;      ///< MAPVK_VK_TO_CHAR
    u16 vsc_ex;  ///< MAPVK_VK_TO_VSC_EX (0xE0xx / 0xE1xx for prefixed keys)
};
struct ScanRow {
    u16 code;    ///< scan code, 0xE0xx / 0xE1xx when prefixed
    u8 vk;       ///< MAPVK_VSC_TO_VK
    u8 vk_ex;    ///< MAPVK_VSC_TO_VK_EX
};
struct CharRow {
    u16 ch;
    u16 vkscan;  ///< VkKeyScanExW: low byte VK, high byte shift state
};
struct ToUnicodeRow {
    u8 vk;
    u8 mods;     ///< kShift | kCtrl | kAlt | kCaps
    i8 ret;      ///< ToUnicodeEx's return (negative for a dead key)
    u16 units[4];
};

#include "kbd_us_layout.inc"

inline constexpr u32 kHkl = 0x04090409u;
inline constexpr u32 kShift = 1u;
inline constexpr u32 kCtrl = 2u;
inline constexpr u32 kAlt = 4u;
inline constexpr u32 kCaps = 8u;

/// MapVirtualKeyEx. False for a map type outside MAPVK_VK_TO_VSC ..
/// MAPVK_VK_TO_VSC_EX (Windows: 0 / ERROR_INVALID_PARAMETER,
/// s2msg_input_probe mapvk_type5 / mapvk_type7).
[[nodiscard]] inline bool map_virtual_key(u32 code, u32 type, u32& result) noexcept {
    result = 0;
    switch (type) {
    case 0: case 2: case 4:
        for (const VkRow& row : kVkRows) {
            if (row.vk == code) {
                result = type == 0 ? row.vsc : type == 2 ? row.ch : row.vsc_ex;
                break;
            }
        }
        return true;
    case 1: case 3:
        for (const ScanRow& row : kScanRows) {
            if (row.code == code) {
                result = type == 1 ? row.vk : row.vk_ex;
                break;
            }
        }
        return true;
    default:
        return false;
    }
}

/// VkKeyScanExW: -1 (0xFFFF) when no key types @p ch.
[[nodiscard]] inline u16 vk_key_scan(u16 ch) noexcept {
    for (const CharRow& row : kCharRows) {
        if (row.ch == ch) return row.vkscan;
    }
    return 0xFFFFu;
}

/// The modifier state ToUnicodeEx reads from a 256-byte key state: only the
/// generic VK_SHIFT / VK_CONTROL / VK_MENU down bits and VK_CAPITAL's toggle
/// bit (VK_LSHIFT alone types 'a': lshift_only_A).
[[nodiscard]] inline u32 modifiers(const u8* key_state) noexcept {
    u32 mods = 0;
    if ((key_state[0x10] & 0x80u) != 0) mods |= kShift;
    if ((key_state[0x11] & 0x80u) != 0) mods |= kCtrl;
    if ((key_state[0x12] & 0x80u) != 0) mods |= kAlt;
    if ((key_state[0x14] & 0x01u) != 0) mods |= kCaps;
    return mods;
}

/// ToUnicodeEx's translation of @p vk under @p mods; nullptr when the key
/// types nothing (return 0).
[[nodiscard]] inline const ToUnicodeRow* to_unicode(u32 vk, u32 mods) noexcept {
    for (const ToUnicodeRow& row : kToUnicodeRows) {
        if (row.vk == vk && row.mods == mods) return &row;
    }
    return nullptr;
}

} // namespace vitreo::winapi::kbd_us
