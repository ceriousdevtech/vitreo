// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/system/activeds/activeds.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <optional>
#include <string>

namespace vitreo::winapi {
namespace {
struct AdsiExport { const char* name; u16 ordinal; u32 stack_bytes; };
#include "activeds_contract.inc"

// Guest threads execute on dedicated host threads, as does Win32 last-error.
// This provider-error storage is separate from Win32 last-error.
struct ProviderError { u32 code = 0; std::u16string text, provider; };
thread_local ProviderError provider_error;

std::optional<std::u16string> read_units(Marshaler& m, GuestAddr pointer) {
    std::u16string text;
    if (pointer == 0) return text;
    for (;;) {
        auto unit = m.read_u16(pointer);
        if (unit.is_err()) return std::nullopt;
        if (unit.value() == 0) return text;
        text.push_back(static_cast<char16_t>(unit.value()));
        pointer += sizeof(u16);
    }
}

bool write_units(Marshaler& m, GuestAddr pointer, u32 capacity,
                 const std::u16string& text) {
    if (capacity == 0) return true;
    const usize count = std::min(text.size(), static_cast<usize>(capacity - 1));
    for (usize i = 0; i < count; ++i)
        if (m.write_u16(pointer + i * sizeof(u16), static_cast<u16>(text[i])).is_err())
            return false;
    return m.write_u16(pointer + count * sizeof(u16), 0).is_ok();
}
}

DllExports create_activeds_thunks() {
    DllExports exports;
    exports.entries.push_back({kADsGetObject.name, kADsGetObject.ordinal,
        [](Marshaler& m) {
            const auto path = read_units(m, m.arg_ptr(0));
            // Windows preserves ppObject on these failures (both-bit oracle).
            // Do not write through it, even when the requested binding fails.
            if (!path) { m.set_return(kE_POINTER); return; }
            if (path->empty()) { m.set_return(kE_FAIL); return; }
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true))
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED activeds!ADsGetObject: "
                             "directory-service provider binding\n");
            m.set_return(kE_NOTIMPL);
        }, kADsGetObject.stack_bytes});
    exports.entries.push_back({kADsGetLastError.name, kADsGetLastError.ordinal,
        [](Marshaler& m) {
            const GuestAddr error = m.arg_ptr(0), text = m.arg_ptr(1), name = m.arg_ptr(3);
            // Windows validates all three pointers even at zero capacities.
            if (error == 0 || text == 0 || name == 0) {
                m.set_return(kE_POINTER); return;
            }
            if (m.write_u32(error, provider_error.code).is_err() ||
                !write_units(m, text, m.arg32(2), provider_error.text) ||
                !write_units(m, name, m.arg32(4), provider_error.provider)) {
                m.set_return(kE_POINTER); return;
            }
            m.set_return(kS_OK);
        }, kADsGetLastError.stack_bytes});
    exports.entries.push_back({kADsSetLastError.name, kADsSetLastError.ordinal,
        [](Marshaler& m) {
            auto text = read_units(m, m.arg_ptr(1));
            auto provider = read_units(m, m.arg_ptr(2));
            if (!text || !provider) {
                std::fprintf(stderr, "[vitreo] activeds!ADsSetLastError: unreadable guest string\n");
                return;
            }
            provider_error = {m.arg32(0), std::move(*text), std::move(*provider)};
        }, kADsSetLastError.stack_bytes});
    return exports;
}
}
