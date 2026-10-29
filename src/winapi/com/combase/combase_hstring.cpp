// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_hstring.cpp
/// @brief HSTRING creation, lookup and the WindowsXxxString exports

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

#include <algorithm>
#include <cstdint>

namespace vitreo::winapi {

namespace {

constexpr u32 MAX_HSTRING_LENGTH = 1u << 20;
constexpr GuestAddr HSTRING_POINTER_OFFSET = 16;

constexpr GuestAddr owned_hstring_ref_count_offset(bool is_64bit) {
    return HSTRING_POINTER_OFFSET + (is_64bit ? 8 : 4);
}

constexpr GuestAddr owned_hstring_buffer_offset(bool is_64bit) {
    return owned_hstring_ref_count_offset(is_64bit) + 4;
}

constexpr usize owned_hstring_allocation_size(
    usize length, bool is_64bit) {
    // Leave two bytes after the UTF-16 terminator, matching the native
    // allocation granularity observed for both 32-bit and 64-bit HSTRINGs.
    return static_cast<usize>(owned_hstring_buffer_offset(is_64bit)) +
           length * sizeof(char16_t) + 4;
}

} // namespace

GuestAddr CombaseManager::create_hstring(
    std::u16string_view value, bool is_64bit) {
    if (value.empty()) return 0;
    if (value.size() > MAX_HSTRING_LENGTH) return 0;

    const GuestAddr process_heap = m_heap.get_process_heap();
    if (process_heap == NULL_HANDLE || process_heap == INVALID_HANDLE) return 0;
    const GuestAddr header = m_heap.heap_alloc(
        process_heap,
        HEAP_ZERO_MEMORY,
        owned_hstring_allocation_size(value.size(), is_64bit));
    if (header == 0) return 0;

    const GuestAddr ref_count =
        header + owned_hstring_ref_count_offset(is_64bit);
    const GuestAddr buffer =
        header + owned_hstring_buffer_offset(is_64bit);

    if (m_memory.write_u32(header, 0).is_err() ||
        m_memory.write_u32(
            header + 4, static_cast<u32>(value.size())).is_err() ||
        !write_pointer(
            header + HSTRING_POINTER_OFFSET, buffer, is_64bit) ||
        m_memory.write_u32(ref_count, 1).is_err()) {
        (void)m_heap.heap_free(process_heap, 0, header);
        return 0;
    }
    for (usize i = 0; i < value.size(); ++i) {
        if (m_memory.write_u16(buffer + i * 2, value[i]).is_err()) {
            (void)m_heap.heap_free(process_heap, 0, header);
            return 0;
        }
    }
    if (m_memory.write_u16(buffer + value.size() * 2, 0).is_err()) {
        (void)m_heap.heap_free(process_heap, 0, header);
        return 0;
    }
    // Guest code may release and HeapFree an HSTRING inline. Heap allocation
    // reuse can therefore return an address still present in this host-side
    // lookup; replace that stale metadata with the new live string.
    m_strings.insert_or_assign(
        header, OwnedHString{std::u16string(value), buffer, is_64bit});
    return header;
}

std::optional<GuestAddr> CombaseManager::hstring_buffer(
    GuestAddr string, u32* length, bool is_64bit) const {
    if (string == 0) {
        if (length != nullptr) *length = 0;
        return m_empty_buffer;
    }
    if (auto owned = m_strings.find(string); owned != m_strings.end()) {
        if (length != nullptr) {
            *length = static_cast<u32>(owned->second.value.size());
        }
        return owned->second.buffer;
    }

    auto length_result = m_memory.read_u32(string + 4);
    if (length_result.is_err() || length_result.value() > MAX_HSTRING_LENGTH) {
        return std::nullopt;
    }
    GuestAddr buffer = 0;
    if (is_64bit) {
        auto pointer = m_memory.read_u64(
            string + HSTRING_POINTER_OFFSET);
        if (pointer.is_err()) return std::nullopt;
        buffer = pointer.value();
    } else {
        auto pointer = m_memory.read_u32(
            string + HSTRING_POINTER_OFFSET);
        if (pointer.is_err()) return std::nullopt;
        buffer = pointer.value();
    }
    if (length_result.value() != 0 && buffer == 0) return std::nullopt;
    if (length != nullptr) *length = length_result.value();
    return buffer != 0 ? buffer : m_empty_buffer;
}

std::optional<std::u16string> CombaseManager::read_hstring(
    GuestAddr string, bool is_64bit) const {
    u32 length = 0;
    auto buffer = hstring_buffer(string, &length, is_64bit);
    if (!buffer.has_value()) return std::nullopt;
    std::u16string value(length, u'\0');
    for (u32 i = 0; i < length; ++i) {
        auto code_unit = m_memory.read_u16(*buffer + i * 2);
        if (code_unit.is_err()) return std::nullopt;
        value[i] = code_unit.value();
    }
    return value;
}

void CombaseManager::impl_WindowsCreateString(Marshaler& m) {
    const GuestAddr source = m.arg_ptr(0);
    const u32 length = static_cast<u32>(m.arg(1));
    const GuestAddr string_out = m.arg_ptr(2);
    // Measured (s7_winrt_probe.expected): a NULL out-pointer is E_INVALIDARG;
    // a NULL source with a nonzero length is E_POINTER.
    if (string_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(string_out, 0);
    if (source == 0 && length != 0) {
        m.set_return(E_POINTER);
        return;
    }
    std::u16string value(length, u'\0');
    for (u32 i = 0; i < length; ++i) {
        auto code_unit = m.read_u16(source + i * 2);
        if (code_unit.is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        value[i] = code_unit.value();
    }
    const GuestAddr string = create_hstring(value, m.cpu_is_64bit());
    if (!value.empty() && string == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (m.write_ptr(string_out, string).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_WindowsCreateStringReference(Marshaler& m) {
    const GuestAddr source = m.arg_ptr(0);
    const u32 length = static_cast<u32>(m.arg(1));
    const GuestAddr header = m.arg_ptr(2);
    const GuestAddr string_out = m.arg_ptr(3);
    if (header == 0 || string_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(string_out, 0);
    if (source == 0 && length != 0) {
        m.set_return(E_POINTER);
        return;
    }
    // sourceString[length] must be NUL, also for length 0 (measured,
    // s7_winrt_probe: ("abc", 0) and an unterminated buffer both return
    // E_STRING_NOT_NULL_TERMINATED).
    if (source != 0) {
        auto terminator = m.read_u16(source + static_cast<u64>(length) * 2);
        if (terminator.is_err() || terminator.value() != 0) {
            m.set_return(0x80000017u);  // E_STRING_NOT_NULL_TERMINATED
            return;
        }
    }
    if (length == 0) {
        const bool is_64bit = m.cpu_is_64bit();
        (void)m.write_u32(header, 1);
        (void)m.write_u32(header + 4, 0);
        (void)write_pointer(header + 16, 0, is_64bit);
        m.set_return(S_OK);
        return;
    }
    const bool is_64bit = m.cpu_is_64bit();
    if (m.write_u32(header, 1).is_err() ||
        m.write_u32(header + 4, length).is_err() ||
        !write_pointer(header + 16, source, is_64bit) ||
        m.write_ptr(string_out, header).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

// HRESULT WindowsCompareStringOrdinal(HSTRING, HSTRING, INT32* result)
// Ordinal UTF-16 comparison over the full lengths (embedded NULs count);
// NULL is the empty string. Measured (s7_winrt_probe): result NULL is
// E_INVALIDARG; the result is -1/0/1.
void CombaseManager::impl_WindowsCompareStringOrdinal(Marshaler& m) {
    const GuestAddr result = m.arg_ptr(2);
    if (result == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto left = hstring_value(m.arg_ptr(0), m.cpu_is_64bit());
    auto right = hstring_value(m.arg_ptr(1), m.cpu_is_64bit());
    if (!left || !right) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const int order = left->compare(*right);
    (void)m.write_u32(result, static_cast<u32>(order < 0 ? -1 : order > 0 ? 1 : 0));
    m.set_return(S_OK);
}

void CombaseManager::impl_WindowsDeleteString(Marshaler& m) {
    const GuestAddr string = m.arg_ptr(0);
    if (auto owned = m_strings.find(string); owned != m_strings.end()) {
        const GuestAddr ref_count_address =
            string + owned_hstring_ref_count_offset(
                         owned->second.is_64bit);
        auto ref_count = m_memory.read_u32(ref_count_address);
        if (ref_count.is_err() || ref_count.value() == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const u32 remaining = ref_count.value() - 1;
        if (m_memory.write_u32(ref_count_address, remaining).is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (remaining == 0) {
            m_strings.erase(owned);
            if (!m_heap.heap_free(
                    m_heap.get_process_heap(), 0, string)) {
                m.set_return(E_INVALIDARG);
                return;
            }
        }
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_WindowsGetStringRawBuffer(Marshaler& m) {
    if (!ensure_storage()) {
        m.set_return(0);
        return;
    }
    u32 length = 0;
    auto buffer = hstring_buffer(
        m.arg_ptr(0), &length, m.cpu_is_64bit());
    const GuestAddr length_out = m.arg_ptr(1);
    if (length_out != 0) (void)m.write_u32(length_out, length);
    m.set_return(buffer.value_or(0));
}

void CombaseManager::impl_WindowsDuplicateString(Marshaler& m) {
    const GuestAddr source = m.arg_ptr(0);
    const GuestAddr string_out = m.arg_ptr(1);
    if (string_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (m.write_ptr(string_out, 0).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    if (source == 0) {
        m.set_return(S_OK);
        return;
    }
    if (auto owned = m_strings.find(source); owned != m_strings.end()) {
        const GuestAddr ref_count_address =
            source + owned_hstring_ref_count_offset(
                         owned->second.is_64bit);
        auto ref_count = m_memory.read_u32(ref_count_address);
        if (ref_count.is_err() ||
            ref_count.value() == 0 ||
            ref_count.value() == UINT32_MAX) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (m_memory.write_u32(
                ref_count_address, ref_count.value() + 1).is_err() ||
            m.write_ptr(string_out, source).is_err()) {
            m.set_return(E_POINTER);
            return;
        }
        m.set_return(S_OK);
        return;
    }
    auto value = read_hstring(source, m.cpu_is_64bit());
    if (!value.has_value()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr copy = create_hstring(*value, m.cpu_is_64bit());
    if (!value->empty() && copy == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    (void)m.write_ptr(string_out, copy);
    m.set_return(S_OK);
}

void CombaseManager::impl_WindowsGetStringLen(Marshaler& m) {
    u32 length = 0;
    (void)hstring_buffer(
        m.arg_ptr(0), &length, m.cpu_is_64bit());
    m.set_return(length);
}

void CombaseManager::impl_WindowsStringHasEmbeddedNull(Marshaler& m) {
    const GuestAddr result_out = m.arg_ptr(1);
    if (result_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto value = read_hstring(m.arg_ptr(0), m.cpu_is_64bit());
    if (!value.has_value()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const bool embedded =
        std::find(value->begin(), value->end(), u'\0') != value->end();
    if (m.write_u32(result_out, embedded ? 1u : 0u).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_WindowsIsStringEmpty(Marshaler& m) {
    u32 length = 0;
    const bool valid =
        hstring_buffer(
            m.arg_ptr(0), &length, m.cpu_is_64bit()).has_value();
    m.set_return(valid && length == 0 ? 1u : 0u);
}

} // namespace vitreo::winapi
