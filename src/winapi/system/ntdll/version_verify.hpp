// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file version_verify.hpp
/// @brief RtlVerifyVersionInfo / VerifyVersionInfo comparison and
///        VerSetConditionMask encoding, shared by ntdll and kernel32.
///
/// Learn documents the type and condition constants; the comparison rules
/// below are measured on Windows 11 by
/// tools/oracle_probe/verify_version_info_probe.c
/// (tests/oracle_data/verify_version_info_probe*.expected):
///
/// - A zero type mask is STATUS_INVALID_PARAMETER.
/// - VER_SUITENAME: a zero requested mask always passes; otherwise VER_AND
///   needs every requested bit, VER_OR any of them, and any other condition
///   is STATUS_INVALID_PARAMETER, which wins over every failed comparison.
/// - VER_PLATFORMID, VER_BUILDNUMBER and VER_PRODUCT_TYPE are compared on
///   their own with their own condition; DWORD fields compare as signed
///   32-bit values; a condition outside VER_EQUAL..VER_LESS_EQUAL fails.
/// - Major, minor, service-pack major and service-pack minor form one
///   version number, walked in that order over the tested fields. The first
///   condition that is not VER_EQUAL governs the rest of the walk. An equal
///   field moves on to the next tested field, except service-pack minor,
///   which is always decided by the governing condition. The first unequal
///   field decides. A walk that ends on equal fields passes.
/// - Unknown type bits and the condition mask's top bit are ignored, and
///   dwOSVersionInfoSize is not checked.
/// - VerSetConditionMask returns 0 for a zero type mask; otherwise it ORs the
///   condition (low three bits) into the field of the HIGHEST type bit, at
///   bit index * 3, and sets bit 63. Field shifts of 64 or more wrap modulo
///   64 on x64 and drop the bits on x86 (the compiler's 64-bit shift).

#include <vitreo/types.hpp>

namespace vitreo::winapi::version_verify {

inline constexpr u32 kVerMinorVersion = 0x00000001;
inline constexpr u32 kVerMajorVersion = 0x00000002;
inline constexpr u32 kVerBuildNumber = 0x00000004;
inline constexpr u32 kVerPlatformId = 0x00000008;
inline constexpr u32 kVerServicePackMinor = 0x00000010;
inline constexpr u32 kVerServicePackMajor = 0x00000020;
inline constexpr u32 kVerSuiteName = 0x00000040;
inline constexpr u32 kVerProductType = 0x00000080;

inline constexpr u8 kVerEqual = 1;
inline constexpr u8 kVerGreater = 2;
inline constexpr u8 kVerGreaterEqual = 3;
inline constexpr u8 kVerLess = 4;
inline constexpr u8 kVerLessEqual = 5;
inline constexpr u8 kVerAnd = 6;
inline constexpr u8 kVerOr = 7;

/// The fields of an OSVERSIONINFOEX that take part in the comparison.
struct VersionFields {
    u32 major = 0;
    u32 minor = 0;
    u32 build = 0;
    u32 platform_id = 0;
    u16 service_pack_major = 0;
    u16 service_pack_minor = 0;
    u16 suite_mask = 0;
    u8 product_type = 0;
};

enum class Outcome { Satisfied, Mismatch, InvalidParameter };

/// VerSetConditionMask. @p long_mode selects the x64 shift behaviour.
[[nodiscard]] inline u64 set_condition_mask(u64 condition_mask, u32 type_mask,
                                            u32 condition,
                                            bool long_mode) noexcept {
    if (type_mask == 0) return 0;
    u32 index = 31;
    while ((type_mask & (1u << index)) == 0) --index;
    const u32 shift = index * 3;
    const u64 value = static_cast<u64>(condition & 0x7u);
    u64 field = 0;
    if (long_mode) {
        field = value << (shift & 63u);
    } else if (shift < 64) {
        field = value << shift;
    }
    return condition_mask | field | (u64{1} << 63);
}

namespace detail {

[[nodiscard]] inline u8 condition_of(u64 condition_mask, u32 type_bit) noexcept {
    u32 index = 0;
    while ((type_bit >> index) != 1u) ++index;
    return static_cast<u8>((condition_mask >> (index * 3)) & 0x7u);
}

[[nodiscard]] inline bool compare(i64 current, i64 requested, u8 condition) noexcept {
    switch (condition) {
    case kVerEqual: return current == requested;
    case kVerGreater: return current > requested;
    case kVerGreaterEqual: return current >= requested;
    case kVerLess: return current < requested;
    case kVerLessEqual: return current <= requested;
    default: return false;
    }
}

[[nodiscard]] inline i64 as_signed(u32 v) noexcept {
    return static_cast<i64>(static_cast<i32>(v));
}

}  // namespace detail

/// RtlVerifyVersionInfo's decision for @p requested against @p current.
[[nodiscard]] inline Outcome verify(const VersionFields& current,
                                    const VersionFields& requested,
                                    u32 type_mask, u64 condition_mask) noexcept {
    using detail::as_signed;
    using detail::compare;
    using detail::condition_of;
    if (type_mask == 0) return Outcome::InvalidParameter;

    bool satisfied = true;
    if ((type_mask & kVerSuiteName) != 0 && requested.suite_mask != 0) {
        const u8 c = condition_of(condition_mask, kVerSuiteName);
        const u16 common = current.suite_mask & requested.suite_mask;
        if (c == kVerAnd) {
            satisfied = common == requested.suite_mask;
        } else if (c == kVerOr) {
            satisfied = common != 0;
        } else {
            return Outcome::InvalidParameter;
        }
    }
    if ((type_mask & kVerPlatformId) != 0 &&
        !compare(as_signed(current.platform_id), as_signed(requested.platform_id),
                 condition_of(condition_mask, kVerPlatformId))) {
        satisfied = false;
    }
    if ((type_mask & kVerBuildNumber) != 0 &&
        !compare(as_signed(current.build), as_signed(requested.build),
                 condition_of(condition_mask, kVerBuildNumber))) {
        satisfied = false;
    }
    if ((type_mask & kVerProductType) != 0 &&
        !compare(current.product_type, requested.product_type,
                 condition_of(condition_mask, kVerProductType))) {
        satisfied = false;
    }

    struct Field { u32 bit; i64 current; i64 requested; };
    const Field group[] = {
        {kVerMajorVersion, as_signed(current.major), as_signed(requested.major)},
        {kVerMinorVersion, as_signed(current.minor), as_signed(requested.minor)},
        {kVerServicePackMajor, current.service_pack_major,
         requested.service_pack_major},
        {kVerServicePackMinor, current.service_pack_minor,
         requested.service_pack_minor},
    };
    bool governed = false;
    u8 governing = 0;
    for (const Field& f : group) {
        if ((type_mask & f.bit) == 0) continue;
        if (!governed || governing == kVerEqual) {
            governing = condition_of(condition_mask, f.bit);
            governed = true;
        }
        if (f.bit != kVerServicePackMinor && f.current == f.requested) continue;
        if (!compare(f.current, f.requested, governing)) satisfied = false;
        break;
    }
    return satisfied ? Outcome::Satisfied : Outcome::Mismatch;
}

}  // namespace vitreo::winapi::version_verify
