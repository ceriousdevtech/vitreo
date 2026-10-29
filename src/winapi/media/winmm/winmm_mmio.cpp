/// @file winmm_mmio.cpp
/// @brief mmio* read-only RIFF/WAVE file I/O.
///
/// MmioFile and m_mmio_files are declared in winmm.hpp. Split out of
/// winmm.cpp.

#include "winapi/media/winmm/winmm_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include <algorithm>
#include <cstring>

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// Multimedia file I/O — read-only RIFF/WAVE path
// ---------------------------------------------------------------------------
namespace {
constexpr u32 MMIO_WRITE = 0x00000001;
constexpr u32 MMIO_READWRITE = 0x00000002;
constexpr u32 MMIO_CREATE = 0x00001000;
constexpr u32 MMIO_FINDCHUNK = 0x00000010;
constexpr u32 MMIO_FINDRIFF = 0x00000020;
constexpr u32 MMIO_FINDLIST = 0x00000040;
constexpr u32 MMIOERR_FILENOTFOUND = 257;
constexpr u32 MMIOERR_CANNOTOPEN = 259;
constexpr u32 MMIOERR_CHUNKNOTFOUND = 265;
constexpr u32 FOURCC_RIFF = 0x46464952; // "RIFF"
constexpr u32 FOURCC_LIST = 0x5453494C; // "LIST"

u32 read_le_u32(const std::vector<u8>& bytes, usize offset) {
    return static_cast<u32>(bytes[offset]) |
           (static_cast<u32>(bytes[offset + 1]) << 8) |
           (static_cast<u32>(bytes[offset + 2]) << 16) |
           (static_cast<u32>(bytes[offset + 3]) << 24);
}
}

void WinmmManager::impl_mmioOpenA(Marshaler& m) {
    const GuestAddr filename_ptr = m.arg_ptr(0);
    const GuestAddr info_ptr = m.arg_ptr(1);
    const u32 flags = static_cast<u32>(m.arg(2));
    const auto set_open_error = [&](u32 error) {
        // MMIOINFO.wErrorRet is a UINT at offset 12 in the Win32 layout.
        if (info_ptr != 0) (void)m.write_u32(info_ptr + 12, error);
    };

    if (filename_ptr == 0 ||
        (flags & (MMIO_WRITE | MMIO_READWRITE | MMIO_CREATE)) != 0) {
        set_open_error(MMIOERR_CANNOTOPEN);
        m.set_return(0);
        return;
    }
    const auto filename_result = m.read_ansi_string(filename_ptr, 32768);
    if (filename_result.is_err() || filename_result.value().empty()) {
        set_open_error(MMIOERR_CANNOTOPEN);
        m.set_return(0);
        return;
    }
    const std::string filename = filename_result.value();

    auto stat_result = m_vfs.stat(filename);
    if (stat_result.is_err() ||
        stat_result.value().size > 256ULL * 1024ULL * 1024ULL) {
        winmm_diag("mmioOpenA path=\"%s\" -> not found\n", filename.c_str());
        set_open_error(MMIOERR_FILENOTFOUND);
        m.set_return(0);
        return;
    }

    auto open_result = m_vfs.open(
        filename, capsule::AccessMode::Read, capsule::ShareMode::Read,
        capsule::CreationDisposition::OpenExisting);
    if (open_result.is_err()) {
        set_open_error(MMIOERR_CANNOTOPEN);
        m.set_return(0);
        return;
    }
    const capsule::FileHandle file_handle = open_result.value();
    std::vector<u8> bytes(static_cast<usize>(stat_result.value().size));
    usize total = 0;
    bool read_ok = true;
    while (total < bytes.size()) {
        auto read_result = m_vfs.read(
            file_handle, std::span<u8>(bytes).subspan(total));
        if (read_result.is_err()) {
            read_ok = false;
            break;
        }
        if (read_result.value() == 0) break;
        total += read_result.value();
    }
    (void)m_vfs.close(file_handle);
    if (!read_ok) {
        set_open_error(MMIOERR_CANNOTOPEN);
        m.set_return(0);
        return;
    }
    bytes.resize(total);

    u32 handle = m_next_mmio_handle++;
    if (handle == 0) handle = m_next_mmio_handle++;
    m_mmio_files.emplace(
        handle, MmioFile{std::move(bytes), 0, filename});
    set_open_error(0);
    winmm_diag("mmioOpenA path=\"%s\" -> 0x%x bytes=%zu\n",
               filename.c_str(), handle,
               m_mmio_files.at(handle).bytes.size());
    m.set_return(handle);
}

void WinmmManager::impl_mmioClose(Marshaler& m) {
    const u32 handle = static_cast<u32>(m.arg(0));
    (void)m.arg(1);
    const bool removed = m_mmio_files.erase(handle) != 0;
    winmm_diag("mmioClose handle=0x%x -> %s\n", handle,
               removed ? "ok" : "invalid");
    m.set_return(removed ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE);
}

void WinmmManager::impl_mmioDescend(Marshaler& m) {
    const u32 handle = static_cast<u32>(m.arg(0));
    const GuestAddr chunk_ptr = m.arg_ptr(1);
    const GuestAddr parent_ptr = m.arg_ptr(2);
    const u32 flags = static_cast<u32>(m.arg(3));
    auto file_it = m_mmio_files.find(handle);
    if (file_it == m_mmio_files.end() || chunk_ptr == 0) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    auto& file = file_it->second;
    const u32 requested_id = m.read_u32(chunk_ptr).value_or(0);
    const u32 requested_type = m.read_u32(chunk_ptr + 8).value_or(0);

    usize search_pos = file.cursor;
    usize search_end = file.bytes.size();
    if (parent_ptr != 0) {
        const u32 parent_id = m.read_u32(parent_ptr).value_or(0);
        const u32 parent_size = m.read_u32(parent_ptr + 4).value_or(0);
        const u32 parent_data = m.read_u32(parent_ptr + 12).value_or(0);
        search_pos = std::max<usize>(search_pos, parent_data);
        const u64 payload_size =
            (parent_id == FOURCC_RIFF || parent_id == FOURCC_LIST)
                ? (parent_size >= 4 ? parent_size - 4 : 0)
                : parent_size;
        search_end = static_cast<usize>(
            std::min<u64>(file.bytes.size(),
                          static_cast<u64>(parent_data) + payload_size));
    }

    bool found = false;
    u32 found_id = 0;
    u32 found_size = 0;
    u32 found_type = 0;
    u32 found_data = 0;
    while (search_pos + 8 <= search_end) {
        const u32 id = read_le_u32(file.bytes, search_pos);
        const u32 size = read_le_u32(file.bytes, search_pos + 4);
        const u64 chunk_end =
            static_cast<u64>(search_pos) + 8ULL + size + (size & 1U);
        if (chunk_end > file.bytes.size()) break;

        const bool container = id == FOURCC_RIFF || id == FOURCC_LIST;
        const u32 type =
            container && size >= 4 && search_pos + 12 <= file.bytes.size()
                ? read_le_u32(file.bytes, search_pos + 8)
                : 0;
        bool match = false;
        if ((flags & MMIO_FINDRIFF) != 0) {
            match = id == FOURCC_RIFF && type == requested_type;
        } else if ((flags & MMIO_FINDLIST) != 0) {
            match = id == FOURCC_LIST && type == requested_type;
        } else if ((flags & MMIO_FINDCHUNK) != 0) {
            match = id == requested_id;
        } else {
            match = true;
        }
        if (match) {
            found = true;
            found_id = id;
            found_size = size;
            found_type = type;
            found_data = static_cast<u32>(
                search_pos + (container ? 12 : 8));
            break;
        }
        search_pos = static_cast<usize>(chunk_end);
    }

    if (!found) {
        winmm_diag("mmioDescend handle=0x%x flags=0x%x -> not found\n",
                   handle, flags);
        m.set_return(MMIOERR_CHUNKNOTFOUND);
        return;
    }
    (void)m.write_u32(chunk_ptr, found_id);
    (void)m.write_u32(chunk_ptr + 4, found_size);
    (void)m.write_u32(chunk_ptr + 8, found_type);
    (void)m.write_u32(chunk_ptr + 12, found_data);
    (void)m.write_u32(chunk_ptr + 16, 0);
    file.cursor = found_data;
    winmm_diag(
        "mmioDescend handle=0x%x id=0x%08x type=0x%08x size=%u data=%u\n",
        handle, found_id, found_type, found_size, found_data);
    m.set_return(MMSYSERR_NOERROR);
}

void WinmmManager::impl_mmioRead(Marshaler& m) {
    const u32 handle = static_cast<u32>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const i32 requested = static_cast<i32>(m.arg(2));
    auto file_it = m_mmio_files.find(handle);
    if (file_it == m_mmio_files.end() || buffer == 0 || requested < 0) {
        m.set_return(static_cast<u32>(-1));
        return;
    }
    auto& file = file_it->second;
    const usize available = file.cursor < file.bytes.size()
        ? file.bytes.size() - file.cursor
        : 0;
    const usize count = std::min<usize>(
        available, static_cast<usize>(requested));
    if (count != 0 &&
        m_memory.write_bytes(
            buffer,
            reinterpret_cast<const Byte*>(
                file.bytes.data() + file.cursor),
            count).is_err()) {
        m.set_return(static_cast<u32>(-1));
        return;
    }
    file.cursor += count;
    winmm_diag("mmioRead handle=0x%x requested=%d -> %zu\n",
               handle, requested, count);
    m.set_return(static_cast<u32>(count));
}

void WinmmManager::impl_mmioAscend(Marshaler& m) {
    const u32 handle = static_cast<u32>(m.arg(0));
    const GuestAddr chunk_ptr = m.arg_ptr(1);
    (void)m.arg(2);
    auto file_it = m_mmio_files.find(handle);
    if (file_it == m_mmio_files.end() || chunk_ptr == 0) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    const u32 id = m.read_u32(chunk_ptr).value_or(0);
    const u32 size = m.read_u32(chunk_ptr + 4).value_or(0);
    const u32 data = m.read_u32(chunk_ptr + 12).value_or(0);
    const u64 header = data >= (id == FOURCC_RIFF || id == FOURCC_LIST ? 12U : 8U)
        ? data - (id == FOURCC_RIFF || id == FOURCC_LIST ? 12U : 8U)
        : 0;
    const u64 end = header + 8ULL + size + (size & 1U);
    file_it->second.cursor = static_cast<usize>(
        std::min<u64>(end, file_it->second.bytes.size()));
    winmm_diag("mmioAscend handle=0x%x -> offset=%zu\n",
               handle, file_it->second.cursor);
    m.set_return(MMSYSERR_NOERROR);
}

} // namespace vitreo::winapi
