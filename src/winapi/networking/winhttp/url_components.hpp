// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file url_components.hpp
/// @brief One URL_COMPONENTS codec for WinHttpCrackUrl, InternetCrackUrlA and
///        InternetCrackUrlW, in both guest bitnesses.
///
/// Contract sources:
///  - Learn: WinHttpCrackUrl, URL_COMPONENTS (winhttp), InternetCrackUrlW.
///  - Layout: _WINHTTP_URL_COMPONENTS (winhttp.h) and URL_COMPONENTSA/W
///    (wininet.h), SDK 10.0.26100.0 — the same field sequence in both headers.
///  - Everything Learn leaves open (in-place vs copy per component, required
///    sizes on ERROR_INSUFFICIENT_BUFFER, ICU_DECODE/ICU_ESCAPE, scheme/port
///    edge cases, dwStructSize, last error on success) is measured by
///    tools/oracle_probe/http_crackurl_probe.c on Windows x86 and x64; its
///    .expected files are the regression specification.

#include <vitreo/common.hpp>

#include <functional>
#include <string>

namespace vitreo::winapi {

class Marshaler;

namespace urlcrack {

enum class Api : u8 { WinHttp, WinInetA, WinInetW };

/// Offsets of URL_COMPONENTS for one guest bitness. Derived by the natural
/// alignment rules from the header's field sequence (DWORD, LPxSTR, DWORD,
/// INTERNET_SCHEME, LPxSTR, DWORD, INTERNET_PORT, then four LPxSTR/DWORD
/// pairs); checked against the probe's offsetof() output by a unit test.
struct Layout {
    u32 size = 0;
    u32 ptr[6] = {};   ///< scheme, host, user, password, path, extra
    u32 len[6] = {};
    u32 nscheme = 0;
    u32 port = 0;
};
[[nodiscard]] Layout layout(bool is64) noexcept;

/// Guest scratch allocation used when WinINet (ANSI) returns an in-place
/// pointer to a decoded copy instead of into the caller's string.
using ScratchAlloc = std::function<GuestAddr(u64 size)>;

/// Perform the whole guest-visible call. Returns the BOOL result and sets the
/// thread's last error exactly as measured (including ERROR_SUCCESS on
/// success).
[[nodiscard]] u32 crack(Marshaler& m, Api api, GuestAddr url, u32 url_length,
                        u32 flags, GuestAddr components,
                        const ScratchAlloc& scratch = {});

/// Parse a WinHTTP target URL (as WinHttpCrackUrl would) into the url and
/// host arguments Windows hands to a PAC script's FindProxyForURL. Returns 0
/// or the WinHttpCrackUrl error (e.g. ERROR_WINHTTP_UNRECOGNIZED_SCHEME).
[[nodiscard]] u32 proxy_script_arguments(const std::u16string& url, std::string& script_url,
                                         std::string& host);

} // namespace urlcrack
} // namespace vitreo::winapi
