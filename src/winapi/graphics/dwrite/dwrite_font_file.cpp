// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/dwrite/dwrite_internal.hpp"

#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dwrite/font_backend.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace vitreo::winapi {
namespace {

constexpr usize MAX_CUSTOM_FONT_REFERENCE_KEY_SIZE = 1024 * 1024;

// x86 stdcall splits UINT64 into two dwords; x64 passes one register/slot.
u64 read_u64_argument(Marshaler& m, usize& index) {
    if (m.cpu_is_64bit()) {
        return m.arg(index++);
    }
    const u32 lo = static_cast<u32>(m.arg(index++));
    const u32 hi = static_cast<u32>(m.arg(index++));
    return (static_cast<u64>(hi) << 32) | lo;
}

// Factory cache keys start with the guest loader address (loader_key).
template <class Map>
void erase_loader_entries(Map& entries, GuestAddr loader) {
    std::erase_if(entries, [loader](const auto& entry) {
        u64 owner = 0;
        std::memcpy(&owner, entry.first.data(), sizeof(owner));
        return owner == loader;
    });
}

std::optional<std::u16string> read_local_font_key(
    GuestMemory& memory, GuestAddr key, u32 key_size) {
    if (key == 0 || key_size < sizeof(char16_t) ||
        (key_size % sizeof(char16_t)) != 0) {
        return std::nullopt;
    }
    const usize units = key_size / sizeof(char16_t);
    std::u16string result;
    result.reserve(units);
    for (usize index = 0; index < units; ++index) {
        auto value = memory.read_u16(key + index * sizeof(u16));
        if (value.is_err()) return std::nullopt;
        if (value.value() == 0) return result;
        result.push_back(static_cast<char16_t>(value.value()));
    }
    return std::nullopt;
}

} // namespace

void DWriteManager::impl_RegisterFontCollectionLoader(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr loader = m.arg_ptr(1);
    auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory || loader == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (factory->second.collection_loaders.contains(loader)) {
        m.set_return(DWRITE_E_ALREADYREGISTERED);
        return;
    }
    if (!call_guest_method(loader, 1, nullptr, 0)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    factory->second.collection_loaders.insert(loader);
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] RegisterFontCollectionLoader "
                     "factory=0x%llx loader=0x%llx -> S_OK\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(loader));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_UnregisterFontCollectionLoader(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr loader = m.arg_ptr(1);
    auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory ||
        factory->second.collection_loaders.erase(loader) == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    erase_loader_entries(factory->second.custom_collections, loader);
    release_guest_object(loader);
    m.set_return(S_OK);
}

void DWriteManager::impl_RegisterFontFileLoader(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr loader = m.arg_ptr(1);
    auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory || loader == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (factory->second.file_loaders.contains(loader)) {
        m.set_return(DWRITE_E_ALREADYREGISTERED);
        return;
    }
    if (!call_guest_method(loader, 1, nullptr, 0)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    factory->second.file_loaders.insert(loader);
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] RegisterFontFileLoader "
                     "factory=0x%llx loader=0x%llx -> S_OK\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(loader));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_UnregisterFontFileLoader(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr loader = m.arg_ptr(1);
    auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory ||
        factory->second.file_loaders.erase(loader) == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    erase_loader_entries(factory->second.custom_font_files, loader);
    release_guest_object(loader);
    m.set_return(S_OK);
}

void DWriteManager::impl_CreateFontFileReference(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr path_address = m.arg_ptr(1);
    (void)m.arg_ptr(2); // Optional FILETIME is a cache hint.
    const GuestAddr file_out = m.arg_ptr(3);
    if (file_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(file_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory || path_address == 0 ||
        m_vfs == nullptr) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto path = read_utf16_z(path_address, 32768);
    if (path.is_err() || path.value().empty()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const std::string utf8_path = utf16_to_utf8(
        reinterpret_cast<const u16*>(path.value().data()),
        static_cast<int>(path.value().size()));
    if (m_vfs->stat(utf8_path).is_err()) {
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }
    const GuestAddr file = create_local_font_file(path.value());
    if (file == 0 || m.write_ptr(file_out, file).is_err()) {
        if (file != 0) m_objects.erase(file);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateFontFileReference path='%s' "
                     "-> S_OK file=0x%llx\n",
                     utf8_path.c_str(),
                     static_cast<unsigned long long>(file));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_CreateCustomFontFileReference(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr key = m.arg_ptr(1);
    const u32 key_size = static_cast<u32>(m.arg(2));
    const GuestAddr loader = m.arg_ptr(3);
    const GuestAddr file_out = m.arg_ptr(4);
    if (file_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(file_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory || key == 0 ||
        key_size == 0 || key_size > MAX_CUSTOM_FONT_REFERENCE_KEY_SIZE ||
        loader == 0 || !factory->second.file_loaders.contains(loader) ||
        !m_guest_call) {
        m.set_return(E_INVALIDARG);
        return;
    }

    const usize mapping_size = align_up(
        static_cast<usize>(key_size), GUEST_PAGE_SIZE);
    const GuestAddr key_copy = m_memory.find_free_gap(
        0x68000000ull, mapping_size);
    if (key_copy == 0 || m_memory.map_region(
            key_copy, mapping_size,
            MemoryProtection::ReadWrite).is_err()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    std::vector<u8> key_bytes(key_size);
    if (m_memory.read_bytes(
            key, reinterpret_cast<Byte*>(key_bytes.data()),
            key_bytes.size()).is_err() ||
        m_memory.write_bytes(
            key_copy, reinterpret_cast<const Byte*>(key_bytes.data()),
            key_bytes.size()).is_err() ||
        m_memory.protect_region(
            key_copy, mapping_size, MemoryProtection::Read).is_err()) {
        (void)m_memory.unmap_region(key_copy, mapping_size);
        m.set_return(E_POINTER);
        return;
    }
    if (!call_guest_method(loader, 1, nullptr, 0)) {
        (void)m_memory.unmap_region(key_copy, mapping_size);
        m.set_return(E_INVALIDARG);
        return;
    }

    const GuestAddr file = create_object(ObjectType::FontFile);
    if (file == 0) {
        release_guest_object(loader);
        (void)m_memory.unmap_region(key_copy, mapping_size);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    auto& object = m_objects.at(file);
    object.key_address = key_copy;
    object.key_size = key_size;
    object.key_mapping_size = mapping_size;
    object.font_file_loader = loader;
    if (m.write_ptr(file_out, file).is_err()) {
        m_objects.erase(file);
        release_guest_object(loader);
        (void)m_memory.unmap_region(key_copy, mapping_size);
        m.set_return(E_POINTER);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateCustomFontFileReference "
                     "loader=0x%llx key=0x%llx size=%u -> S_OK "
                     "file=0x%llx\n",
                     static_cast<unsigned long long>(loader),
                     static_cast<unsigned long long>(key), key_size,
                     static_cast<unsigned long long>(file));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FileGetReferenceKey(Marshaler& m) {
    const auto file = m_objects.find(m.arg_ptr(0));
    const GuestAddr key_out = m.arg_ptr(1);
    const GuestAddr size_out = m.arg_ptr(2);
    if (key_out == 0 || size_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(key_out, 0);
    (void)m.write_u32(size_out, 0);
    if (file == m_objects.end() ||
        file->second.type != ObjectType::FontFile ||
        file->second.key_address == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (file->second.key_size == 0 ||
        m.write_ptr(key_out, file->second.key_address).is_err() ||
        m.write_u32(size_out, file->second.key_size).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FileGetLoader(Marshaler& m) {
    const auto file = m_objects.find(m.arg_ptr(0));
    const GuestAddr loader_out = m.arg_ptr(1);
    if (loader_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(loader_out, 0);
    if (file == m_objects.end() ||
        file->second.type != ObjectType::FontFile) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (file->second.font_file_loader != 0) {
        const GuestAddr loader = file->second.font_file_loader;
        if (!call_guest_method(loader, 1, nullptr, 0)) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (m.write_ptr(loader_out, loader).is_err()) {
            release_guest_object(loader);
            m.set_return(E_POINTER);
            return;
        }
        m.set_return(S_OK);
        return;
    }
    if (m_local_loader == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto loader = m_objects.find(m_local_loader);
    if (loader == m_objects.end()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    ++loader->second.ref_count;
    if (m.write_ptr(loader_out, m_local_loader).is_err()) {
        --loader->second.ref_count;
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FileAnalyze(Marshaler& m) {
    const auto file = m_objects.find(m.arg_ptr(0));
    const GuestAddr supported_out = m.arg_ptr(1);
    const GuestAddr file_type_out = m.arg_ptr(2);
    const GuestAddr face_type_out = m.arg_ptr(3);
    const GuestAddr face_count_out = m.arg_ptr(4);
    if (supported_out == 0 || file_type_out == 0 ||
        face_count_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_u32(supported_out, 0);
    (void)m.write_u32(file_type_out, 0);
    if (face_type_out != 0) (void)m.write_u32(face_type_out, 6);
    (void)m.write_u32(face_count_out, 0);
    if (file == m_objects.end() ||
        file->second.type != ObjectType::FontFile) {
        m.set_return(E_INVALIDARG);
        return;
    }
    constexpr usize SCRATCH_SIZE = GUEST_PAGE_SIZE;
    const GuestAddr scratch_address =
        m_memory.find_free_gap(0x6B000000ull, SCRATCH_SIZE);
    if (scratch_address == 0 || m_memory.map_region(
            scratch_address, SCRATCH_SIZE,
            MemoryProtection::ReadWrite).is_err()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    struct ScratchMapping {
        GuestMemory& memory;
        GuestAddr address;
        ~ScratchMapping() {
            (void)memory.unmap_region(address, GUEST_PAGE_SIZE);
        }
    } scratch{m_memory, scratch_address};
    auto [load_hr, parsed_fonts] = load_font_file(
        file->second, scratch_address);
    if ((load_hr & 0x80000000u) != 0) {
        m.set_return(load_hr);
        return;
    }
    const auto& fonts = *parsed_fonts;
    if (fonts.empty()) {
        m.set_return(S_OK);
        return;
    }
    const u32 face_type = fonts.size() > 1 ? 2u : fonts.front().face_type;
    const u32 file_type = fonts.size() > 1
        ? 3u : face_type == 0 ? 1u : face_type == 1 ? 2u : 0u;
    if (m.write_u32(supported_out, file_type != 0 ? 1u : 0u).is_err() ||
        m.write_u32(file_type_out, file_type).is_err() ||
        (face_type_out != 0 &&
         m.write_u32(face_type_out, face_type).is_err()) ||
        m.write_u32(
            face_count_out, static_cast<u32>(fonts.size())).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_LocalLoaderGetPathLength(Marshaler& m) {
    const auto loader = m_objects.find(m.arg_ptr(0));
    const GuestAddr key = m.arg_ptr(1);
    const u32 key_size = static_cast<u32>(m.arg(2));
    const GuestAddr length_out = m.arg_ptr(3);
    if (length_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (loader == m_objects.end() ||
        loader->second.type != ObjectType::LocalFontFileLoader) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto path = read_local_font_key(m_memory, key, key_size);
    if (!path || path->size() > std::numeric_limits<u32>::max()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(m.write_u32(
        length_out, static_cast<u32>(path->size())).is_ok()
                     ? S_OK : E_POINTER);
}

void DWriteManager::impl_LocalLoaderGetPath(Marshaler& m) {
    const auto loader = m_objects.find(m.arg_ptr(0));
    const GuestAddr key = m.arg_ptr(1);
    const u32 key_size = static_cast<u32>(m.arg(2));
    const GuestAddr path_out = m.arg_ptr(3);
    const u32 path_size = static_cast<u32>(m.arg(4));
    if (loader == m_objects.end() ||
        loader->second.type != ObjectType::LocalFontFileLoader) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto path = read_local_font_key(m_memory, key, key_size);
    if (!path) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (path_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (path_size < path->size() + 1) {
        m.set_return(E_NOT_SUFFICIENT_BUFFER);
        return;
    }
    for (usize index = 0; index < path->size(); ++index) {
        if (m.write_u16(
                path_out + index * sizeof(u16), (*path)[index]).is_err()) {
            m.set_return(E_POINTER);
            return;
        }
    }
    m.set_return(m.write_u16(
        path_out + path->size() * sizeof(u16), 0).is_ok()
                     ? S_OK : E_POINTER);
}

void DWriteManager::impl_LocalLoaderGetLastWriteTime(Marshaler& m) {
    const auto loader = m_objects.find(m.arg_ptr(0));
    const GuestAddr key = m.arg_ptr(1);
    const u32 key_size = static_cast<u32>(m.arg(2));
    const GuestAddr time_out = m.arg_ptr(3);
    if (time_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (loader == m_objects.end() ||
        loader->second.type != ObjectType::LocalFontFileLoader ||
        m_vfs == nullptr) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto path = read_local_font_key(m_memory, key, key_size);
    if (!path) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const std::string utf8 = utf16_to_utf8(
        reinterpret_cast<const u16*>(path->data()),
        static_cast<int>(path->size()));
    auto info = m_vfs->stat(utf8);
    if (info.is_err()) {
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }
    m.set_return(m.write_u64(time_out, info.value().last_write_time).is_ok()
                     ? S_OK : E_POINTER);
}

void DWriteManager::impl_LocalLoaderCreateStreamFromKey(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr key = m.arg_ptr(1);
    const u32 key_size = static_cast<u32>(m.arg(2));
    const GuestAddr stream_out = m.arg_ptr(3);
    if (stream_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(stream_out, 0);
    const auto loader = m_objects.find(self);
    if (loader == m_objects.end() ||
        loader->second.type != ObjectType::LocalFontFileLoader ||
        m_vfs == nullptr) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto path = read_local_font_key(m_memory, key, key_size);
    if (!path) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const std::string utf8 = utf16_to_utf8(
        reinterpret_cast<const u16*>(path->data()),
        static_cast<int>(path->size()));
    auto info = m_vfs->stat(utf8);
    if (m_font_reuse) {
        // Streams of an unchanged file share one read-only mapping: a
        // fragment is only ever read (IDWriteFontFileStream::ReadFileFragment
        // returns `void const*`), and "unchanged" is the path, size and last
        // write time GetLastWriteTimeFromKey reports.
        if (info.is_err() || info.value().size > MAX_CUSTOM_FONT_FILE_SIZE) {
            m.set_return(DWRITE_E_FILEFORMAT);
            return;
        }
        usize mapped_size = 0;
        const GuestAddr data = acquire_stream_mapping(
            *path, utf8, info.value().size, info.value().last_write_time,
            mapped_size);
        if (data == 0) {
            m.set_return(DWRITE_E_FILEFORMAT);
            return;
        }
        const GuestAddr stream = create_object(ObjectType::FontFileStream);
        if (stream == 0) {
            release_stream_mapping(data);
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        auto& object = m_objects.at(stream);
        object.local_font_path = *path;
        object.stream_data_address = data;
        object.stream_file_size = info.value().size;
        object.stream_mapping_size = mapped_size;
        object.stream_shared_mapping = true;
        object.stream_last_write_time = info.value().last_write_time;
        if (m.write_ptr(stream_out, stream).is_err()) {
            release_stream_mapping(data);
            m_objects.erase(stream);
            m.set_return(E_POINTER);
            return;
        }
        if (dwrite_diag_enabled()) {
            dwrite_diag_printf(
                "[dwrite-diag] CreateStreamFromKey path='%s' size=%llu shared=0x%llx "
                "-> S_OK stream=0x%llx\n",
                utf8.c_str(), static_cast<unsigned long long>(info.value().size),
                static_cast<unsigned long long>(data),
                static_cast<unsigned long long>(stream));
        }
        m.set_return(S_OK);
        return;
    }
    auto bytes = read_vfs_file(*path);
    if (info.is_err() || !bytes) {
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }

    const usize mapped_size = align_up(
        std::max<usize>(bytes->size(), 1), GUEST_PAGE_SIZE);
    const GuestAddr data = m_memory.find_free_gap(0x6C000000ull, mapped_size);
    if (data == 0 || m_memory.map_region(
            data, mapped_size, MemoryProtection::ReadWrite).is_err()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if ((!bytes->empty() &&
         m_memory.write_bytes(
             data, reinterpret_cast<const Byte*>(bytes->data()),
             bytes->size()).is_err()) ||
        m_memory.protect_region(
            data, mapped_size, MemoryProtection::Read).is_err()) {
        (void)m_memory.unmap_region(data, mapped_size);
        m.set_return(E_OUTOFMEMORY);
        return;
    }

    const GuestAddr stream = create_object(ObjectType::FontFileStream);
    if (stream == 0) {
        (void)m_memory.unmap_region(data, mapped_size);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    auto& object = m_objects.at(stream);
    object.local_font_path = *path;
    object.stream_data_address = data;
    object.stream_file_size = bytes->size();
    object.stream_mapping_size = mapped_size;
    object.stream_last_write_time = info.value().last_write_time;
    if (m.write_ptr(stream_out, stream).is_err()) {
        (void)m_memory.unmap_region(data, mapped_size);
        m_objects.erase(stream);
        m.set_return(E_POINTER);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateStreamFromKey path='%s' "
                     "size=%zu -> S_OK stream=0x%llx\n",
                     utf8.c_str(), bytes->size(),
                     static_cast<unsigned long long>(stream));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_StreamReadFileFragment(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr fragment_start_out = m.arg_ptr(1);
    usize arg_index = 2;
    const u64 file_offset = read_u64_argument(m, arg_index);
    const u64 fragment_size = read_u64_argument(m, arg_index);
    const GuestAddr fragment_context_out = m.arg_ptr(arg_index);
    if (fragment_start_out == 0 || fragment_context_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(fragment_start_out, 0);
    (void)m.write_ptr(fragment_context_out, 0);

    const auto stream = m_objects.find(self);
    if (stream == m_objects.end() ||
        stream->second.type != ObjectType::FontFileStream ||
        stream->second.stream_data_address == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const u64 file_size = stream->second.stream_file_size;
    if (file_offset > file_size ||
        fragment_size > file_size - file_offset) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr fragment =
        stream->second.stream_data_address + file_offset;
    if (m.write_ptr(fragment_start_out, fragment).is_err() ||
        m.write_ptr(fragment_context_out, 0).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_StreamReleaseFileFragment(Marshaler& m) {
    // Fragments alias the stream's mapped file bytes; no per-fragment state.
    (void)m.arg_ptr(0);
    (void)m.arg_ptr(1);
}

void DWriteManager::impl_StreamGetFileSize(Marshaler& m) {
    const auto stream = m_objects.find(m.arg_ptr(0));
    const GuestAddr size_out = m.arg_ptr(1);
    if (size_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (stream == m_objects.end() ||
        stream->second.type != ObjectType::FontFileStream) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(m.write_u64(size_out, stream->second.stream_file_size).is_ok()
                     ? S_OK : E_POINTER);
}

void DWriteManager::impl_StreamGetLastWriteTime(Marshaler& m) {
    const auto stream = m_objects.find(m.arg_ptr(0));
    const GuestAddr time_out = m.arg_ptr(1);
    if (time_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (stream == m_objects.end() ||
        stream->second.type != ObjectType::FontFileStream) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(m.write_u64(
                     time_out, stream->second.stream_last_write_time).is_ok()
                     ? S_OK : E_POINTER);
}

} // namespace vitreo::winapi
