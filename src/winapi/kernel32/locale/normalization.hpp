#pragma once
/// @file normalization.hpp
/// @brief NormalizeString, IsNormalizedString and the IdnTo* conversions.
///
/// Windows exports these five from kernelbase.dll, kernel32.dll and
/// Normaliz.dll (ordinals 1-5, each forwarding to kernelbase). The contract is
/// Microsoft Learn's winnls.h pages plus what winoracle measured where Learn
/// is silent: tools/oracle_probe/normaliz_probe.c, normaliz_limits_probe.c and
/// normaliz_edges_probe.c, whose .expected files the unit test replays.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <optional>
#include <string>

namespace vitreo::winapi::nls {

/// One call's string arguments as the guest passed them.
struct StringArgs {
    bool src_null = false;
    i32 src_len = 0;      ///< As passed; -1 means NUL-terminated.
    std::u16string src;   ///< The counted units (through the NUL for -1).
    bool dst_null = false;
    i32 dst_len = 0;
};

/// What a call does to the guest.
struct StringResult {
    i32 ret = 0;
    std::optional<u32> last_error;  ///< nullopt: GetLastError is left as it was.
    std::u16string written;         ///< Units stored at the start of the destination.
};

/// int NormalizeString(NORM_FORM, LPCWSTR, int, LPWSTR, int).
[[nodiscard]] StringResult normalize_string(i32 form, const StringArgs& args);

/// BOOL IsNormalizedString(NORM_FORM, LPCWSTR, int). Destination fields unused.
[[nodiscard]] StringResult is_normalized_string(i32 form, const StringArgs& args);

enum class IdnApi { ToAscii, ToUnicode, ToNameprepUnicode };

/// int IdnToAscii / IdnToUnicode / IdnToNameprepUnicode(DWORD, LPCWSTR, int,
/// LPWSTR, int).
[[nodiscard]] StringResult idn_convert(IdnApi api, u32 flags, const StringArgs& args);

/// The five exports, by name. Normaliz.dll publishes the same entries at
/// ordinals 1-5 (normaliz_ordinal).
[[nodiscard]] DllExports create_normalization_thunks();

/// Normaliz.dll's ordinal for one of the five names, or 0.
[[nodiscard]] u16 normaliz_ordinal(std::string_view name) noexcept;

} // namespace vitreo::winapi::nls
