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
std::optional<std::vector<u8>> DWriteManager::read_vfs_file(
    const std::u16string& path) const {
    if (m_vfs == nullptr || path.empty()) return std::nullopt;
    const std::string guest_path = utf16_to_utf8(
        reinterpret_cast<const u16*>(path.data()),
        static_cast<int>(path.size()));
    auto info = m_vfs->stat(guest_path);
    if (info.is_err() || info.value().size > MAX_CUSTOM_FONT_FILE_SIZE ||
        info.value().size > std::numeric_limits<usize>::max()) {
        return std::nullopt;
    }
    auto opened = m_vfs->open(
        guest_path, capsule::AccessMode::Read, capsule::ShareMode::Read,
        capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return std::nullopt;
    const capsule::FileHandle handle = opened.value();
    std::vector<u8> data(static_cast<usize>(info.value().size));
    usize offset = 0;
    while (offset < data.size()) {
        auto read = m_vfs->read(
            handle, std::span<u8>(data).subspan(offset));
        if (read.is_err() || read.value() == 0) {
            (void)m_vfs->close(handle);
            return std::nullopt;
        }
        offset += read.value();
    }
    (void)m_vfs->close(handle);
    return data;
}

u32 DWriteManager::read_guest_font_stream(
    const Object& file, GuestAddr scratch_address,
    const FragmentConsumer& consume, u64 fragment_limit,
    bool query_last_write_time) {
    if (file.font_file_loader == 0 || file.key_address == 0 ||
        file.key_size == 0 || !m_guest_call) {
        return E_INVALIDARG;
    }

    const GuestAddr stream_out = scratch_address + 0x30;
    const GuestAddr size_out = scratch_address + 0x40;
    const GuestAddr fragment_out = scratch_address + 0x50;
    const GuestAddr context_out = scratch_address + 0x60;
    if (!write_pointer(stream_out, 0)) return E_POINTER;
    const u64 create_args[]{
        file.key_address, file.key_size, stream_out,
    };
    const auto create_result = call_guest_method(
        file.font_file_loader, 3, create_args, std::size(create_args));
    if (!create_result) return E_INVALIDARG;
    const u32 create_hr = static_cast<u32>(*create_result);
    if ((create_hr & 0x80000000u) != 0) return create_hr;
    const auto stream_value = read_pointer(stream_out);
    if (stream_value.is_err() || stream_value.value() == 0) {
        return E_INVALIDARG;
    }
    const GuestAddr stream = stream_value.value();

    if (m_memory.write_u64(size_out, 0).is_err()) {
        release_guest_object(stream);
        return E_POINTER;
    }
    const u64 size_args[]{size_out};
    const auto size_result = call_guest_method(
        stream, 5, size_args, std::size(size_args));
    if (!size_result) {
        release_guest_object(stream);
        return E_INVALIDARG;
    }
    const u32 size_hr = static_cast<u32>(*size_result);
    if ((size_hr & 0x80000000u) != 0) {
        release_guest_object(stream);
        return size_hr;
    }
    const auto file_size = m_memory.read_u64(size_out);
    if (file_size.is_err() || file_size.value() > MAX_CUSTOM_FONT_FILE_SIZE ||
        file_size.value() > std::numeric_limits<usize>::max()) {
        release_guest_object(stream);
        return DWRITE_E_FILEFORMAT;
    }
    if (query_last_write_time) {
        // DirectWrite asks once per full load. Vitreo does
        // not rank font versions, so neither the value nor a failure
        // changes the result.
        const GuestAddr time_out = scratch_address + 0x70;
        const u64 time_args[]{time_out};
        if (m_memory.write_u64(time_out, 0).is_ok()) {
            (void)call_guest_method(stream, 6, time_args, std::size(time_args));
        }
    }
    const u64 read_size = std::min<u64>(file_size.value(), fragment_limit);
    const usize size = static_cast<usize>(read_size);
    if (!write_pointer(fragment_out, 0) ||
        !write_pointer(context_out, 0)) {
        release_guest_object(stream);
        return E_POINTER;
    }

    std::optional<u64> read_result;
    if (m_is_64bit) {
        const u64 read_args[]{
            fragment_out, 0, read_size, context_out,
        };
        read_result = call_guest_method(
            stream, 3, read_args, std::size(read_args));
    } else {
        // x86 stdcall passes each UINT64 as two consecutive 32-bit stack
        // words. GuestCall arguments represent physical ABI slots.
        const u64 read_args[]{
            fragment_out,
            0, 0,
            static_cast<u32>(read_size),
            static_cast<u32>(read_size >> 32),
            context_out,
        };
        read_result = call_guest_method(
            stream, 3, read_args, std::size(read_args));
    }
    if (!read_result) {
        release_guest_object(stream);
        return E_INVALIDARG;
    }
    const u32 read_hr = static_cast<u32>(*read_result);
    if ((read_hr & 0x80000000u) != 0) {
        release_guest_object(stream);
        return read_hr;
    }
    const auto fragment = read_pointer(fragment_out);
    const auto context = read_pointer(context_out);
    if (fragment.is_err() || (size != 0 && fragment.value() == 0) ||
        context.is_err()) {
        release_guest_object(stream);
        return E_POINTER;
    }

    const u32 result_hr = consume(fragment.value(), size);
    const u64 release_args[]{context.value()};
    (void)call_guest_method(
        stream, 4, release_args, std::size(release_args));
    release_guest_object(stream);
    return result_hr;
}

std::pair<u32, std::vector<u8>> DWriteManager::read_font_file_bytes(
    const Object& file, GuestAddr scratch_address) {
    if (!file.local_font_path.empty()) {
        auto bytes = read_vfs_file(file.local_font_path);
        return bytes ? std::pair{S_OK, std::move(*bytes)}
                     : std::pair{DWRITE_E_FILEFORMAT, std::vector<u8>{}};
    }
    std::vector<u8> bytes;
    const u32 hr = read_guest_font_stream(
        file, scratch_address, [&](GuestAddr fragment, usize size) -> u32 {
            bytes.resize(size);
            if (size != 0 && m_memory.read_bytes(
                    fragment, reinterpret_cast<Byte*>(bytes.data()),
                    bytes.size()).is_err()) {
                bytes.clear();
                return E_POINTER;
            }
            return S_OK;
        });
    if (hr != S_OK) bytes.clear();
    return {hr, std::move(bytes)};
}

// A full load: what a factory does the first time it meets a font file (see
// CreateCustomFontCollection for the per-factory reuse DirectWrite performs).
// The loader and stream are driven in full, and a parse from another factory
// or an earlier loader registration is reused only when the bytes the stream
// returns now are byte-for-byte the bytes that were parsed. A local file
// (IDWriteLocalFontFileLoader) is identified by its path, size, and last
// write time (IDWriteLocalFontFileLoader::GetLastWriteTimeFromKey).
std::pair<u32, DWriteManager::ParsedFonts> DWriteManager::load_font_file(
    const Object& file, GuestAddr scratch_address) {
    const auto parse = [](std::shared_ptr<const std::vector<u8>> bytes) {
        return std::make_shared<const std::vector<dwrite::MatchedFont>>(
            dwrite::fonts_from_data(std::move(bytes)));
    };
    if (!m_font_reuse) {
        auto [hr, bytes] = read_font_file_bytes(file, scratch_address);
        if ((hr & 0x80000000u) != 0) return {hr, {}};
        return {hr, std::make_shared<const std::vector<dwrite::MatchedFont>>(
                        dwrite::fonts_from_data(bytes))};
    }

    if (!file.local_font_path.empty()) {
        if (m_vfs == nullptr) return {DWRITE_E_FILEFORMAT, {}};
        const std::string utf8 = utf16_to_utf8(
            reinterpret_cast<const u16*>(file.local_font_path.data()),
            static_cast<int>(file.local_font_path.size()));
        const auto info = m_vfs->stat(utf8);
        if (info.is_err()) return {DWRITE_E_FILEFORMAT, {}};
        std::string key = "L:" + utf8;
        if (auto found = m_font_files.find(key);
            found != m_font_files.end() &&
            found->second.file_size == info.value().size &&
            found->second.last_write_time == info.value().last_write_time) {
            found->second.last_use = ++m_font_file_clock;
            return {S_OK, found->second.fonts};
        }
        auto bytes = read_vfs_file(file.local_font_path);
        if (!bytes) return {DWRITE_E_FILEFORMAT, {}};
        auto shared = std::make_shared<const std::vector<u8>>(std::move(*bytes));
        auto fonts = parse(shared);
        // Recorded under the identity observed before reading: if the file
        // changed meanwhile, the next stat no longer matches it.
        remember_font_file(std::move(key), std::move(shared), fonts,
                           info.value().size, info.value().last_write_time);
        return {S_OK, std::move(fonts)};
    }

    auto file_key = loader_key(file.font_file_loader, file.key_address, file.key_size);
    if (!file_key || file.key_size == 0) return {E_INVALIDARG, {}};
    std::string key = "C:" + *file_key;

    ParsedFonts fonts;
    std::shared_ptr<const std::vector<u8>> fresh;
    const u32 hr = read_guest_font_stream(
        file, scratch_address, [&](GuestAddr fragment, usize size) -> u32 {
            const Byte* view = size != 0 ? m_memory.host_ptr(fragment, size) : nullptr;
            std::vector<u8> copied;
            if (size != 0 && view == nullptr) {
                copied.resize(size);
                if (m_memory.read_bytes(fragment, reinterpret_cast<Byte*>(copied.data()),
                                        size).is_err()) {
                    return E_POINTER;
                }
                view = reinterpret_cast<const Byte*>(copied.data());
            }
            if (auto found = m_font_files.find(key);
                found != m_font_files.end() && found->second.bytes->size() == size &&
                (size == 0 || std::memcmp(found->second.bytes->data(), view, size) == 0)) {
                found->second.last_use = ++m_font_file_clock;
                fonts = found->second.fonts;
                return S_OK;
            }
            fresh = copied.empty()
                ? std::make_shared<const std::vector<u8>>(
                      reinterpret_cast<const u8*>(view),
                      reinterpret_cast<const u8*>(view) + size)
                : std::make_shared<const std::vector<u8>>(std::move(copied));
            return S_OK;
        }, ~u64{0}, true);
    if ((hr & 0x80000000u) != 0) return {hr, {}};
    if (fresh) {
        fonts = parse(fresh);
        remember_font_file(std::move(key), std::move(fresh), fonts, 0, 0);
    }
    return {hr, std::move(fonts)};
}

std::optional<std::string> DWriteManager::loader_key(
    GuestAddr loader, GuestAddr key, u32 key_size) const {
    std::string result(sizeof(u64) + key_size, '\0');
    const u64 loader_value = loader;
    std::memcpy(result.data(), &loader_value, sizeof(loader_value));
    if (key_size != 0 &&
        (key == 0 || m_memory.read_bytes(
                         key, reinterpret_cast<Byte*>(result.data() + sizeof(u64)),
                         key_size).is_err())) {
        return std::nullopt;
    }
    return result;
}

void DWriteManager::remember_font_file(
    std::string key, std::shared_ptr<const std::vector<u8>> bytes,
    ParsedFonts fonts, u64 file_size, u64 last_write_time) {
    // Host memory only; parsed faces of live collections hold these bytes
    // anyway. Bounded so released collections do not pin fonts forever.
    constexpr usize BUDGET = 256 * 1024 * 1024;
    constexpr usize MAX_ENTRIES = 1024;
    if (!bytes || bytes->size() > BUDGET) return;
    if (auto old = m_font_files.find(key); old != m_font_files.end()) {
        m_font_file_bytes -= old->second.bytes->size();
        m_font_files.erase(old);
    }
    while (!m_font_files.empty() &&
           (m_font_file_bytes + bytes->size() > BUDGET ||
            m_font_files.size() >= MAX_ENTRIES)) {
        auto oldest = std::min_element(
            m_font_files.begin(), m_font_files.end(),
            [](const auto& a, const auto& b) {
                return a.second.last_use < b.second.last_use;
            });
        m_font_file_bytes -= oldest->second.bytes->size();
        m_font_files.erase(oldest);
    }
    m_font_file_bytes += bytes->size();
    m_font_files[std::move(key)] = {std::move(bytes), std::move(fonts), file_size,
                                    last_write_time, ++m_font_file_clock};
}

GuestAddr DWriteManager::acquire_stream_mapping(
    const std::u16string& path, const std::string& utf8_path,
    u64 file_size, u64 last_write_time, usize& mapped_size) {
    if (auto current = m_current_stream_mapping.find(path);
        current != m_current_stream_mapping.end()) {
        const GuestAddr address = current->second;
        auto& entry = m_stream_mappings.at(address);
        GuestAddr base = 0;
        usize region_size = 0;
        MemoryProtection protection = MemoryProtection::None;
        // Still the read-only mapping this manager created, describing the
        // same file version.
        const bool intact =
            m_memory.query_region(address, base, region_size, protection) &&
            base == address && region_size >= entry.mapped_size &&
            protection == MemoryProtection::Read;
        if (intact && entry.file_size == file_size &&
            entry.last_write_time == last_write_time) {
            if (entry.streams++ == 0) m_idle_stream_bytes -= entry.mapped_size;
            entry.last_use = ++m_font_file_clock;
            mapped_size = entry.mapped_size;
            return address;
        }
        entry.current = false;
        m_current_stream_mapping.erase(current);
        if (entry.streams == 0) {
            m_idle_stream_bytes -= entry.mapped_size;
            unmap_stream_mapping(address);
        }
    }

    const usize size = static_cast<usize>(file_size);
    mapped_size = align_up(std::max<usize>(size, 1), GUEST_PAGE_SIZE);
    const GuestAddr data = m_memory.find_free_gap(0x6C000000ull, mapped_size);
    if (data == 0 || m_memory.map_region(
            data, mapped_size, MemoryProtection::ReadWrite).is_err()) {
        return 0;
    }
    bool loaded = size == 0;
    if (!loaded) {
        Byte* host = m_memory.host_ptr_mut(data, size);
        auto opened = m_vfs->open(
            utf8_path, capsule::AccessMode::Read, capsule::ShareMode::Read,
            capsule::CreationDisposition::OpenExisting);
        if (host != nullptr && opened.is_ok()) {
            usize offset = 0;
            while (offset < size) {
                auto read = m_vfs->read(
                    opened.value(),
                    std::span<u8>(reinterpret_cast<u8*>(host) + offset, size - offset));
                if (read.is_err() || read.value() == 0) break;
                offset += read.value();
            }
            loaded = offset == size;
        }
        if (opened.is_ok()) (void)m_vfs->close(opened.value());
        if (host == nullptr) {
            auto bytes = read_vfs_file(path);
            loaded = bytes && bytes->size() == size &&
                m_memory.write_bytes(data, reinterpret_cast<const Byte*>(bytes->data()),
                                     size).is_ok();
        }
    }
    if (!loaded || m_memory.protect_region(
            data, mapped_size, MemoryProtection::Read).is_err()) {
        (void)m_memory.unmap_region(data, mapped_size);
        return 0;
    }
    m_stream_mappings[data] = {path, mapped_size, file_size, last_write_time,
                               1, ++m_font_file_clock, true};
    m_current_stream_mapping[path] = data;
    return data;
}

void DWriteManager::release_stream_mapping(GuestAddr address) {
    auto found = m_stream_mappings.find(address);
    if (found == m_stream_mappings.end() || found->second.streams == 0) return;
    if (--found->second.streams != 0) return;
    if (!found->second.current) {
        unmap_stream_mapping(address);
        return;
    }
    // Guest address space is the scarce resource on x86.
    const usize budget = (m_is_64bit ? 128u : 32u) * 1024 * 1024;
    m_idle_stream_bytes += found->second.mapped_size;
    while (m_idle_stream_bytes > budget) {
        GuestAddr oldest = 0;
        u64 oldest_use = std::numeric_limits<u64>::max();
        for (const auto& [candidate, entry] : m_stream_mappings) {
            if (entry.streams == 0 && entry.current && entry.last_use < oldest_use) {
                oldest = candidate;
                oldest_use = entry.last_use;
            }
        }
        if (oldest == 0) break;
        auto& entry = m_stream_mappings.at(oldest);
        m_idle_stream_bytes -= entry.mapped_size;
        m_current_stream_mapping.erase(entry.path);
        unmap_stream_mapping(oldest);
    }
}

void DWriteManager::unmap_stream_mapping(GuestAddr address) {
    auto found = m_stream_mappings.find(address);
    if (found == m_stream_mappings.end()) return;
    (void)m_memory.unmap_region(address, found->second.mapped_size);
    m_stream_mappings.erase(found);
}

std::optional<std::u16string> DWriteManager::publish_font_file(
    const dwrite::MatchedFont& font) {
    if (m_vfs == nullptr) return std::nullopt;
    std::lock_guard lock(m_api_mutex);
    // GetFiles may revisit the same font through many faces. Cache publication
    // by immutable loader data or host file identity/version, within this VFS.
    const auto source_version = [&]() -> std::string {
        if (font.source_data) return "immutable";
        struct stat info {};
        if (font.source_path.empty() || ::stat(font.source_path.c_str(), &info) != 0)
            return {};
        return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) +
            ":" + std::to_string(info.st_size) + ":" +
            std::to_string(info.st_mtimespec.tv_sec) + ":" +
            std::to_string(info.st_mtimespec.tv_nsec) + ":" +
            std::to_string(info.st_ctimespec.tv_sec) + ":" +
            std::to_string(info.st_ctimespec.tv_nsec);
    };
    const std::string version = source_version();
    const std::string key = font.source_path + ":" +
        std::to_string(reinterpret_cast<uintptr_t>(font.source_data.get()));
    if (auto cached = m_published_fonts.find(key);
        !version.empty() && cached != m_published_fonts.end() &&
        cached->second.source_version == version &&
        (!font.source_data || cached->second.source_data.lock() == font.source_data)) {
        const auto& path = cached->second.guest_path;
        const auto existing = m_vfs->stat(utf16_to_utf8(
            reinterpret_cast<const u16*>(path.data()), static_cast<int>(path.size())));
        if (existing.is_ok() && existing.value().size == cached->second.size)
            return path;
    }
    // Loader faces already own immutable bytes; copying them only to hash
    // and write them back out is wasted work.
    std::optional<std::vector<u8>> loaded;
    const std::vector<u8>* data = nullptr;
    if (m_font_reuse && font.source_data) {
        data = font.source_data.get();
    } else {
        loaded = dwrite::font_file_data(font);
        if (loaded) data = &*loaded;
    }
    if (data == nullptr || data->empty()) return std::nullopt;

    u64 hash = 1469598103934665603ull;
    for (const u8 byte : *data) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::string extension =
        std::filesystem::path(font.source_path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    if (extension != ".ttf" && extension != ".ttc" &&
        extension != ".otf") {
        extension = ".font";
    }
    std::ostringstream name;
    name << "C:\\Windows\\Fonts\\Vitreo-" << std::hex
         << std::setw(16) << std::setfill('0') << hash << extension;
    const std::string guest_path = name.str();

    const auto published_path = [&]() {
        const auto wide = utf8_to_utf16(guest_path);
        std::u16string path(reinterpret_cast<const char16_t*>(wide.data()), wide.size());
        if (!version.empty() && source_version() == version) {
            // Bound metadata without retaining potentially large loader blobs.
            if (m_published_fonts.size() >= 256) m_published_fonts.clear();
            m_published_fonts[key] = {version, font.source_data, path, data->size()};
        }
        return path;
    };
    auto existing = m_vfs->stat(guest_path);
    if (existing.is_ok() && existing.value().size == data->size()) {
        return published_path();
    }
    auto opened = m_vfs->open(
        guest_path, capsule::AccessMode::Write, capsule::ShareMode::Read,
        capsule::CreationDisposition::CreateAlways);
    if (opened.is_err()) return std::nullopt;
    const capsule::FileHandle handle = opened.value();
    usize offset = 0;
    while (offset < data->size()) {
        auto written = m_vfs->write(
            handle, std::span<const u8>(*data).subspan(offset));
        if (written.is_err() || written.value() == 0) {
            (void)m_vfs->close(handle);
            return std::nullopt;
        }
        offset += written.value();
    }
    (void)m_vfs->flush(handle);
    (void)m_vfs->close(handle);
    return published_path();
}

GuestAddr DWriteManager::create_local_font_file(
    const std::u16string& path) {
    if (path.empty()) return 0;
    if (m_local_loader == 0) {
        m_local_loader = create_object(ObjectType::LocalFontFileLoader);
        if (m_local_loader == 0) return 0;
    }
    const usize byte_size = (path.size() + 1) * sizeof(char16_t);
    const usize mapping_size = align_up(byte_size, GUEST_PAGE_SIZE);
    const GuestAddr key = m_memory.find_free_gap(0x69000000ull, mapping_size);
    if (key == 0 || m_memory.map_region(
            key, mapping_size, MemoryProtection::ReadWrite).is_err()) {
        return 0;
    }
    if (m_memory.write_bytes(
            key, reinterpret_cast<const Byte*>(path.c_str()),
            byte_size).is_err() ||
        m_memory.protect_region(
            key, mapping_size, MemoryProtection::Read).is_err()) {
        (void)m_memory.unmap_region(key, mapping_size);
        return 0;
    }
    const GuestAddr file = create_object(ObjectType::FontFile);
    if (file == 0) {
        (void)m_memory.unmap_region(key, mapping_size);
        return 0;
    }
    auto& object = m_objects.at(file);
    object.local_font_path = path;
    object.key_address = key;
    object.key_size = static_cast<u32>(byte_size);
    object.key_mapping_size = mapping_size;
    ++m_objects.at(m_local_loader).ref_count;
    return file;
}

} // namespace vitreo::winapi
