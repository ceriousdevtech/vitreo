// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_string_locale_registration.cpp
/// @brief _itoa_s / _itow_s and the name-returning setlocale family
///        (_create_locale, setlocale, _wsetlocale).
///
/// Split out of crt_string_registration.cpp. Complementary to crt_locale.cpp
/// (CrtLocaleManager); this file holds only the exports that hand a locale
/// name or token back to the guest.

#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <memory>
#include <string>

namespace vitreo::winapi {

namespace {
#include "crt_itoa_constants.inc"
#include "../memory/crt_errno_constants.inc"
/// Opaque _locale_t handed out by _create_locale for the C locale. Only ever
/// passed back to the _l-suffixed CRT functions, every one of which behaves as
/// the C locale in Vitreo.
constexpr u64 kCLocaleToken = 0x0C10CA1Eull;
} // namespace

void add_crt_itoa_s_exports(DllExports& exports, CrtMemoryManager& mem_mgr) {
    // Windows-derived validation and failure-buffer ordering; size is in
    // characters, while int and wchar_t retain their Windows widths.
    for (bool wide : {false, true}) {
        exports.entries.push_back({wide ? "_itow_s" : "_itoa_s", 0,
            [&mem_mgr, wide](Marshaler& m) {
                const u32 value=static_cast<u32>(m.arg(0));
                const GuestAddr buffer=m.arg_ptr(1);
                const u64 capacity=m.arg(2);
                const i32 radix=static_cast<i32>(m.arg(3));
                const u64 unit=wide ? 2u : 1u;
                auto store=[&](u64 index,u16 ch) {
                    const GuestAddr target=buffer+index*unit;
                    const bool ok=wide ? m.write_u16(target,ch).is_ok()
                                       : m.write_u8(target,static_cast<u8>(ch)).is_ok();
                    if(!ok && m.cpu()) {
                        auto& fault=m.cpu()->pending_raise;
                        fault.active=true;fault.code=kErrnoAccessViolation;
                        fault.flags=0;fault.num_args=2;
                        fault.args[0]=kErrnoWriteFault;fault.args[1]=target;
                    }
                    return ok;
                };
                auto fail=[&](u32 error) {
                    mem_mgr.set_errno(error);
                    (void)mem_mgr.invalid_parameter();
                    m.set_return(error);
                };
                if(buffer==0 || capacity==0) {fail(kItoaInvalid);return;}
                if(!store(0,0))return;
                const bool negative=(value & 0x80000000u)!=0 && radix==10;
                if(capacity <= (negative ? 2u : 1u)) {fail(kItoaRange);return;}
                if(radix<2 || radix>36) {fail(kItoaInvalid);return;}
                u32 magnitude=negative ? 0u-value : value;
                u16 reversed[32];
                unsigned count=0;
                const unsigned start=negative ? 1u : 0u;
                if(negative && !store(0,u'-'))return;
                do {
                    const u32 digit=magnitude%static_cast<u32>(radix);
                    reversed[count]=static_cast<u16>(digit<10 ? u'0'+digit : u'a'+digit-10);
                    if(!store(start+count,reversed[count]))return;
                    ++count;
                    magnitude/=static_cast<u32>(radix);
                } while(magnitude!=0 && start+count<capacity);
                if(start+count>=capacity) {
                    if(!store(0,0))return;
                    fail(kItoaRange);return;
                }
                for(unsigned i=0;i<count;++i)if(!store(start+i,reversed[count-i-1]))return;
                if(!store(start+count,0))return;
                m.set_return(0);
            }});
    }
}

void add_crt_locale_name_exports(DllExports& exports, CrtMemoryManager& mem_mgr) {
    // char* setlocale(int category, const char* locale)
    auto locale_string = std::make_shared<GuestAddr>(0);
    exports.entries.push_back(
        // Learn (_create_locale): builds a _locale_t for the named category
        // and locale, or returns NULL if the locale is not supported. Vitreo
        // supports only the C locale, so anything else must be NULL rather
        // than a handle that silently behaves as C — a caller that gets a
        // non-NULL handle for "de-DE" would format numbers wrongly and never
        // know. NULL is the documented "unsupported" answer callers check.
        {"_create_locale", 0, [](Marshaler& m) {
            const GuestAddr locale_ptr = m.arg_ptr(1);
            if (locale_ptr == 0) {
                m.set_return(0);
                return;
            }
            auto locale = m.read_ansi_string(locale_ptr);
            if (locale.is_err()) {
                m.set_return(0);
                return;
            }
            const std::string& name = locale.value();
            if (name.empty() || name == "C" || name == "POSIX") {
                // A non-null opaque token. It is only ever passed back to the
                // _l-suffixed functions, which all behave as C here.
                m.set_return(kCLocaleToken);
                return;
            }
            m.set_return(0);
        }});

    exports.entries.push_back(
        {"setlocale", 0, [&mem_mgr, locale_string](Marshaler& m) {
            const GuestAddr locale_ptr = m.arg_ptr(1);
            if (locale_ptr != 0) {
                auto locale = m.read_ansi_string(locale_ptr);
                if (locale.is_err() ||
                    (!locale.value().empty() && locale.value() != "C" &&
                     locale.value() != "POSIX")) {
                    m.set_return(0);
                    return;
                }
            }
            if (*locale_string == 0) {
                *locale_string = mem_mgr.crt_malloc(2);
                if (*locale_string == 0) {
                    m.set_return(0);
                    return;
                }
                (void)m.write_u8(*locale_string, 'C');
                (void)m.write_u8(*locale_string + 1, 0);
            }
            m.set_return(*locale_string);
        }});

    // wchar_t* _wsetlocale(int category, const wchar_t* locale)
    //
    // The wide twin of setlocale, and it answers in wide characters: the
    // returned pointer is the locale NAME, which callers print or compare, so
    // handing back the narrow "C" buffer would give them a string that ends
    // after one character. Vitreo supports only the C locale, so any other
    // request is the documented NULL rather than a silent C fallback.
    auto wide_locale_string = std::make_shared<GuestAddr>(0);
    exports.entries.push_back(
        {"_wsetlocale", 0, [&mem_mgr, wide_locale_string](Marshaler& m) {
            const GuestAddr locale_ptr = m.arg_ptr(1);
            if (locale_ptr != 0) {
                auto locale = m.read_wide_string(locale_ptr);
                if (locale.is_err() ||
                    (!locale.value().empty() && locale.value() != "C" &&
                     locale.value() != "POSIX")) {
                    m.set_return(0);
                    return;
                }
            }
            if (*wide_locale_string == 0) {
                *wide_locale_string = mem_mgr.crt_malloc(4);
                if (*wide_locale_string == 0) {
                    m.set_return(0);
                    return;
                }
                (void)m.write_u16(*wide_locale_string, u'C');
                (void)m.write_u16(*wide_locale_string + 2, 0);
            }
            m.set_return(*wide_locale_string);
        }});
}

} // namespace vitreo::winapi
