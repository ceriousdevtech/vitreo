// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
// Original normalized HID collection. Format: .docs/contracts/virtual-gamepad-hid.md.
// Usage mapping: Microsoft "DirectInput and XUSB Devices". ABI: generated SDK facts.
#include "hardware/contracts/gamepad_host.hpp"
#include "winapi/common/hid_sdk.generated.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vitreo::winapi::gamepad_hid {
inline constexpr u8 kReportId = 1;
inline constexpr usize kReportBytes = 14;
using Report = std::array<u8, kReportBytes>;
using Bytes = std::vector<u8>;

inline constexpr std::array kButtonMasks{
    hid_sdk::kXINPUT_GAMEPAD_A, hid_sdk::kXINPUT_GAMEPAD_B,
    hid_sdk::kXINPUT_GAMEPAD_X, hid_sdk::kXINPUT_GAMEPAD_Y,
    hid_sdk::kXINPUT_GAMEPAD_LEFT_SHOULDER, hid_sdk::kXINPUT_GAMEPAD_RIGHT_SHOULDER,
    hid_sdk::kXINPUT_GAMEPAD_BACK, hid_sdk::kXINPUT_GAMEPAD_START,
    hid_sdk::kXINPUT_GAMEPAD_LEFT_THUMB, hid_sdk::kXINPUT_GAMEPAD_RIGHT_THUMB};
inline constexpr u16 kFirstButtonDataIndex = 5;
inline constexpr u16 kHatDataIndex = kFirstButtonDataIndex + kButtonMasks.size();
struct ValueField {
    u16 usage;
    u16 byte_offset;
    u16 bit_size;
    u16 data_index;
    u32 logical_max;
    bool has_null;
};
inline constexpr ValueField kValues[]{
    {hid_sdk::kHID_USAGE_GENERIC_X, 1, 16, 0, 65535, false},
    {hid_sdk::kHID_USAGE_GENERIC_Y, 3, 16, 1, 65535, false},
    {hid_sdk::kHID_USAGE_GENERIC_RX, 5, 16, 2, 65535, false},
    {hid_sdk::kHID_USAGE_GENERIC_RY, 7, 16, 3, 65535, false},
    {hid_sdk::kHID_USAGE_GENERIC_Z, 9, 16, 4, 65535, false},
    {hid_sdk::kHID_USAGE_GENERIC_HATSWITCH, 13, 8, kHatDataIndex, 7, true}};

inline void store16(std::span<u8> bytes, usize at, u16 value) {
    bytes[at] = static_cast<u8>(value);
    bytes[at + 1] = static_cast<u8>(value >> 8);
}
inline void store32(std::span<u8> bytes, usize at, u32 value) {
    store16(bytes, at, static_cast<u16>(value));
    store16(bytes, at + 2, static_cast<u16>(value >> 16));
}
inline u8 hat(u16 buttons) {
    const int x = int((buttons & hid_sdk::kXINPUT_GAMEPAD_DPAD_RIGHT) != 0) -
                  int((buttons & hid_sdk::kXINPUT_GAMEPAD_DPAD_LEFT) != 0);
    const int y = int((buttons & hid_sdk::kXINPUT_GAMEPAD_DPAD_DOWN) != 0) -
                  int((buttons & hid_sdk::kXINPUT_GAMEPAD_DPAD_UP) != 0);
    if (y < 0) return x < 0 ? 7 : x > 0 ? 1 : 0;
    if (y > 0) return x < 0 ? 5 : x > 0 ? 3 : 4;
    return x < 0 ? 6 : x > 0 ? 2 : 8;
}
inline Report encode(const hardware::GamepadState& state) {
    Report bytes{};
    bytes[0] = kReportId;
    store16(bytes, 1, static_cast<u16>(int(state.left_x) + 32768));
    store16(bytes, 3, static_cast<u16>(32767 - int(state.left_y)));
    store16(bytes, 5, static_cast<u16>(int(state.right_x) + 32768));
    store16(bytes, 7, static_cast<u16>(32767 - int(state.right_y)));
    const int difference = int(state.right_trigger) - int(state.left_trigger);
    const int z = 32768 + difference * (difference < 0 ? 32768 : 32767) / 255;
    store16(bytes, 9, static_cast<u16>(z));
    u16 buttons = 0;
    for (usize i = 0; i < kButtonMasks.size(); ++i)
        if ((state.buttons & kButtonMasks[i]) != 0) buttons |= static_cast<u16>(1u << i);
    store16(bytes, 11, buttons);
    bytes[13] = hat(state.buttons);
    return bytes;
}

// Parser operations use this same immutable collection definition as the
// producer. These helpers handle raw field bits; HIDP scaling/range/status
// semantics belong to the WinAPI adapter.
inline const ValueField* find_value(u16 usage) {
    for (const auto& field : kValues) if (field.usage == usage) return &field;
    return nullptr;
}
inline u32 read_value(const Report& report, const ValueField& field) {
    const u32 low = report[field.byte_offset];
    return field.bit_size == 8 ? low : low | (u32(report[field.byte_offset + 1]) << 8);
}
inline void write_value(Report& report, const ValueField& field, u32 value) {
    report[field.byte_offset] = static_cast<u8>(value);
    if (field.bit_size == 16) report[field.byte_offset + 1] = static_cast<u8>(value >> 8);
}
inline bool button_pressed(const Report& report, u16 usage) {
    if (usage == 0 || usage > kButtonMasks.size()) return false;
    const usize bit = usage - 1;
    return (report[11 + bit / 8] & (1u << (bit % 8))) != 0;
}
inline bool write_button(Report& report, u16 usage, bool pressed) {
    if (usage == 0 || usage > kButtonMasks.size()) return false;
    const usize bit = usage - 1;
    const u8 mask = static_cast<u8>(1u << (bit % 8));
    auto& byte = report[11 + bit / 8];
    byte = pressed ? static_cast<u8>(byte | mask) : static_cast<u8>(byte & ~mask);
    return true;
}

inline const hid_sdk::Layout& layout(bool is64) { return is64 ? hid_sdk::x64 : hid_sdk::x86; }
inline Bytes capabilities(bool is64) {
    const auto& l = layout(is64);
    Bytes bytes(l.sizeof_HIDP_CAPS, 0);
    store16(bytes, l.offsetof_HIDP_CAPS_Usage, hid_sdk::kHID_USAGE_GENERIC_GAMEPAD);
    store16(bytes, l.offsetof_HIDP_CAPS_UsagePage, hid_sdk::kHID_USAGE_PAGE_GENERIC);
    store16(bytes, l.offsetof_HIDP_CAPS_InputReportByteLength, kReportBytes);
    store16(bytes, l.offsetof_HIDP_CAPS_NumberLinkCollectionNodes, 1);
    store16(bytes, l.offsetof_HIDP_CAPS_NumberInputButtonCaps, 1);
    store16(bytes, l.offsetof_HIDP_CAPS_NumberInputValueCaps, std::size(kValues));
    store16(bytes, l.offsetof_HIDP_CAPS_NumberInputDataIndices, kHatDataIndex + 1);
    return bytes;
}
inline Bytes button_capabilities(bool is64) {
    const auto& l = layout(is64);
    Bytes bytes(l.sizeof_HIDP_BUTTON_CAPS, 0);
    store16(bytes, l.offsetof_HIDP_BUTTON_CAPS_UsagePage, hid_sdk::kHID_USAGE_PAGE_BUTTON);
    bytes[l.offsetof_HIDP_BUTTON_CAPS_ReportID] = kReportId;
    store16(bytes, l.offsetof_HIDP_BUTTON_CAPS_BitField, hid_sdk::kInputVariable);
    store16(bytes, l.offsetof_HIDP_BUTTON_CAPS_LinkUsage, hid_sdk::kHID_USAGE_GENERIC_GAMEPAD);
    store16(bytes, l.offsetof_HIDP_BUTTON_CAPS_LinkUsagePage, hid_sdk::kHID_USAGE_PAGE_GENERIC);
    bytes[l.offsetof_HIDP_BUTTON_CAPS_IsRange] = 1;
    bytes[l.offsetof_HIDP_BUTTON_CAPS_IsAbsolute] = 1;
    // Modern Windows fills ReportCount=1 for a variable-button range
    // (hid_caps oracle). Classic callers treat this storage as reserved.
    store16(bytes, l.offsetof_HIDP_BUTTON_CAPS_ReportCount, 1);
    store16(bytes, l.offsetof_HIDP_BUTTON_CAPS_Range_UsageMin, 1);
    store16(bytes, l.offsetof_HIDP_BUTTON_CAPS_Range_UsageMax, kButtonMasks.size());
    store16(bytes, l.offsetof_HIDP_BUTTON_CAPS_Range_DataIndexMin, kFirstButtonDataIndex);
    store16(bytes, l.offsetof_HIDP_BUTTON_CAPS_Range_DataIndexMax, kHatDataIndex - 1);
    return bytes;
}
inline Bytes value_capabilities(bool is64, const ValueField& field) {
    const auto& l = layout(is64);
    Bytes bytes(l.sizeof_HIDP_VALUE_CAPS, 0);
    store16(bytes, l.offsetof_HIDP_VALUE_CAPS_UsagePage, hid_sdk::kHID_USAGE_PAGE_GENERIC);
    bytes[l.offsetof_HIDP_VALUE_CAPS_ReportID] = kReportId;
    store16(bytes, l.offsetof_HIDP_VALUE_CAPS_BitField,
            hid_sdk::kInputVariable | (field.has_null ? hid_sdk::kInputNullState : 0));
    store16(bytes, l.offsetof_HIDP_VALUE_CAPS_LinkUsage, hid_sdk::kHID_USAGE_GENERIC_GAMEPAD);
    store16(bytes, l.offsetof_HIDP_VALUE_CAPS_LinkUsagePage, hid_sdk::kHID_USAGE_PAGE_GENERIC);
    bytes[l.offsetof_HIDP_VALUE_CAPS_IsAbsolute] = 1;
    bytes[l.offsetof_HIDP_VALUE_CAPS_HasNull] = field.has_null ? 1 : 0;
    store16(bytes, l.offsetof_HIDP_VALUE_CAPS_BitSize, field.bit_size);
    store16(bytes, l.offsetof_HIDP_VALUE_CAPS_ReportCount, 1);
    store32(bytes, l.offsetof_HIDP_VALUE_CAPS_LogicalMax, field.logical_max);
    store32(bytes, l.offsetof_HIDP_VALUE_CAPS_PhysicalMax, field.logical_max);
    // Windows fills both ends of the union even for a non-range value cap
    // (hid_caps oracle); the SDK declares adjacent USAGE/USHORT members.
    store16(bytes, l.offsetof_HIDP_VALUE_CAPS_NotRange_Usage, field.usage);
    store16(bytes, l.offsetof_HIDP_VALUE_CAPS_NotRange_Usage + sizeof(u16), field.usage);
    store16(bytes, l.offsetof_HIDP_VALUE_CAPS_NotRange_DataIndex, field.data_index);
    store16(bytes, l.offsetof_HIDP_VALUE_CAPS_NotRange_DataIndex + sizeof(u16), field.data_index);
    return bytes;
}
inline constexpr std::array<u8, 32> kPreparsedData = [] {
    std::array<u8, 32> bytes{};
    constexpr std::string_view signature = "Vitreo HID gamepad / v1";
    static_assert(signature.size() <= bytes.size());
    for (usize i = 0; i < signature.size(); ++i) bytes[i] = static_cast<u8>(signature[i]);
    return bytes;
}();
inline bool recognizes(std::span<const u8> data) {
    return data.size() == kPreparsedData.size() &&
           std::equal(data.begin(), data.end(), kPreparsedData.begin());
}
} // namespace vitreo::winapi::gamepad_hid
