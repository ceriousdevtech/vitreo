// SPDX-License-Identifier: LGPL-2.1-only

/// @file host_thread_name.cpp
/// @brief Naming the calling host thread after a guest thread description.

#include "winapi/kernel32/thread/host_thread_name.hpp"

#include "winapi/kernel32/locale/string_conv.hpp"

#include <mach/thread_info.h>
#include <pthread.h>

namespace vitreo::winapi {

static_assert(HOST_THREAD_NAME_MAX_BYTES == MAXTHREADNAMESIZE - 1,
              "macOS keeps MAXTHREADNAMESIZE - 1 bytes of a thread name");

std::string host_thread_name_from_utf16(std::u16string_view description) {
    // A host thread name is a C string: it ends at the first NUL.
    if (const auto nul = description.find(u'\0');
        nul != std::u16string_view::npos) {
        description = description.substr(0, nul);
    }
    // utf16_to_utf8 refuses input with an unpaired surrogate (it returns an
    // empty string), and Windows accepts any UTF-16 in a description, so
    // substitute U+FFFD for the unpaired halves first.
    std::u16string text(description);
    for (usize index = 0; index < text.size(); ++index) {
        const char16_t unit = text[index];
        const bool high = unit >= 0xD800 && unit <= 0xDBFF;
        const bool low = unit >= 0xDC00 && unit <= 0xDFFF;
        if (high && index + 1 < text.size() && text[index + 1] >= 0xDC00 &&
            text[index + 1] <= 0xDFFF) {
            ++index;  // a well-formed pair
        } else if (high || low) {
            text[index] = 0xFFFD;
        }
    }
    std::string name = utf16_to_utf8(
        reinterpret_cast<const u16*>(text.data()), static_cast<int>(text.size()));
    if (name.size() > HOST_THREAD_NAME_MAX_BYTES) {
        // name[cut] is the first byte dropped. While it is a continuation byte
        // the character it belongs to straddles the cut, so drop that
        // character whole.
        usize cut = HOST_THREAD_NAME_MAX_BYTES;
        while (cut > 0 && (static_cast<u8>(name[cut]) & 0xC0u) == 0x80u) --cut;
        name.resize(cut);
    }
    return name;
}

int name_calling_host_thread(std::u16string_view description) {
    const std::string name = host_thread_name_from_utf16(description);
    return pthread_setname_np(name.c_str());
}

} // namespace vitreo::winapi
