// SPDX-License-Identifier: LGPL-2.1-only

/// @file cabinet_fdi.cpp
/// @brief FDI implementation over Vitreo's MSCF reader.

#include "winapi/system/cabinet/cabinet_fdi.hpp"

#include "core/compression/cabinet.hpp"

#include <vitreo/common.hpp>

#include <algorithm>
#include <cstring>

namespace vitreo::winapi {

namespace {

// Verified against MSVC for both bitnesses (tests/winapi/cabinet/
// oracle_fdi_layout.txt). FDICABINETINFO and ERF are pointer-free and so are
// identical on x86 and x64; FDINOTIFICATION is not.
constexpr u32 kCabinetInfoSize = 24u;
constexpr u32 kCabInfo_cbCabinet = 0u;
constexpr u32 kCabInfo_cFolders  = 4u;
constexpr u32 kCabInfo_cFiles    = 6u;
constexpr u32 kCabInfo_setID     = 8u;
constexpr u32 kCabInfo_iCabinet  = 10u;
constexpr u32 kCabInfo_fReserve  = 12u;
constexpr u32 kCabInfo_hasprev   = 16u;
constexpr u32 kCabInfo_hasnext   = 20u;

constexpr u32 kErf_erfOper = 0u;
constexpr u32 kErf_erfType = 4u;
constexpr u32 kErf_fError  = 8u;

/// FDINOTIFICATION field offsets. INT_PTR and the three char* widen on x64,
/// so every field after `cb` moves.
struct NotifyLayout {
    u32 size, cb, psz1, psz2, psz3, pv, hf, date, time, attribs, setID,
        iCabinet, iFolder, fdie;
};
constexpr NotifyLayout kNotify32{40, 0, 4, 8, 12, 16, 20, 24, 26, 28, 30, 32,
                                 34, 36};
constexpr NotifyLayout kNotify64{64, 0, 8, 16, 24, 32, 40, 48, 50, 52, 54, 56,
                                 58, 60};

[[nodiscard]] const NotifyLayout& notify_layout(bool is_64bit) {
    return is_64bit ? kNotify64 : kNotify32;
}

// The oflag/pmode FDI itself passes when opening a cabinet for reading.
constexpr i32 kOpenReadBinary = 0x8000;  // _O_BINARY | _O_RDONLY
constexpr i32 kPmodeReadWrite = 0x0180;  // _S_IREAD | _S_IWRITE

constexpr i32 kSeekSet = 0;
constexpr i32 kSeekEnd = 2;

/// Bounce-buffer size for callback-driven I/O.
constexpr u32 kIoChunk = 64u * 1024u;

} // namespace

void CabinetManager::set_scratch_base(GuestAddr base) noexcept {
    m_base = base;
    // The I/O bounce buffer is carved off the front and never recycled; the
    // remainder is the ring used for notification records and strings.
    m_io_buffer = base;
    m_cursor    = base + kIoChunk;
    m_next_hfdi = base + kIoChunk;
}

GuestAddr CabinetManager::scratch_alloc(u32 bytes) {
    if (m_base == 0) return 0;
    const GuestAddr limit = m_base + CabinetManager::CABINET_SCRATCH_SIZE;
    const u32 aligned = (bytes + 15u) & ~15u;
    if (m_cursor + aligned >= limit) {
        // Wrap: records and strings are consumed within a single callback, so
        // reuse is safe once the ring has been walked.
        m_cursor = m_base + kIoChunk + 0x1000u;
    }
    const GuestAddr out = m_cursor;
    m_cursor += aligned;
    return out;
}

GuestAddr CabinetManager::write_scratch_ansi(Marshaler& m,
                                             std::string_view text) {
    const GuestAddr addr = scratch_alloc(static_cast<u32>(text.size()) + 1u);
    if (addr == 0) return 0;
    for (usize i = 0; i < text.size(); ++i) {
        if (m.write_u8(addr + i, static_cast<u8>(text[i])).is_err()) return 0;
    }
    if (m.write_u8(addr + text.size(), 0).is_err()) return 0;
    return addr;
}

CabinetManager::Session* CabinetManager::find(GuestAddr hfdi) {
    auto it = m_sessions.find(hfdi);
    return it == m_sessions.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Guest callback wrappers
// ---------------------------------------------------------------------------

i64 CabinetManager::call_open(const Session& s, GuestAddr path, i32 oflag,
                              i32 pmode) {
    if (!m_guest_call || s.open == 0) return -1;
    const u64 args[3] = {static_cast<u64>(path), static_cast<u64>(oflag),
                         static_cast<u64>(pmode)};
    const u64 raw = m_guest_call(s.open, args, 3, s.is_64bit);
    return s.is_64bit ? static_cast<i64>(raw)
                      : static_cast<i64>(static_cast<i32>(raw));
}

u32 CabinetManager::call_read(const Session& s, i64 hf, GuestAddr buffer,
                              u32 count) {
    if (!m_guest_call || s.read == 0) return 0;
    const u64 args[3] = {static_cast<u64>(hf), static_cast<u64>(buffer),
                         static_cast<u64>(count)};
    return static_cast<u32>(m_guest_call(s.read, args, 3, s.is_64bit));
}

u32 CabinetManager::call_write(const Session& s, i64 hf, GuestAddr buffer,
                               u32 count) {
    if (!m_guest_call || s.write == 0) return 0;
    const u64 args[3] = {static_cast<u64>(hf), static_cast<u64>(buffer),
                         static_cast<u64>(count)};
    return static_cast<u32>(m_guest_call(s.write, args, 3, s.is_64bit));
}

i32 CabinetManager::call_close(const Session& s, i64 hf) {
    if (!m_guest_call || s.close == 0) return -1;
    const u64 args[1] = {static_cast<u64>(hf)};
    return static_cast<i32>(m_guest_call(s.close, args, 1, s.is_64bit));
}

i64 CabinetManager::call_seek(const Session& s, i64 hf, i64 distance,
                              i32 origin) {
    if (!m_guest_call || s.seek == 0) return -1;
    // PFNSEEK takes a `long` distance, which is 32-bit in both Windows
    // bitnesses, so a cabinet above 2 GiB cannot be addressed through this
    // interface at all — that is the documented limit, not a shortcut here.
    const u64 args[3] = {static_cast<u64>(hf),
                         static_cast<u64>(static_cast<i32>(distance)),
                         static_cast<u64>(origin)};
    const u64 raw = m_guest_call(s.seek, args, 3, s.is_64bit);
    return static_cast<i64>(static_cast<i32>(raw));
}

i64 CabinetManager::call_notify(const Session& s, GuestAddr notify,
                                FdiNotification type, GuestAddr record) {
    if (!m_guest_call || notify == 0) return -1;
    const u64 args[2] = {static_cast<u64>(type), static_cast<u64>(record)};
    const u64 raw = m_guest_call(notify, args, 2, s.is_64bit);
    const i64 result = s.is_64bit ? static_cast<i64>(raw)
                                  : static_cast<i64>(static_cast<i32>(raw));
    VITREO_TRACE("[cabinet] notify %u -> %lld\n", static_cast<u32>(type),
                 static_cast<long long>(result));
    return result;
}

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

bool CabinetManager::fail(const Session& s, Marshaler& m, FdiError error) {
    if (s.erf != 0) {
        (void)m.write_u32(s.erf + kErf_erfOper, static_cast<u32>(error));
        (void)m.write_u32(s.erf + kErf_erfType, 0);
        (void)m.write_u32(s.erf + kErf_fError, 1);
    }
    VITREO_TRACE("[cabinet] FDI error %d\n", static_cast<int>(error));
    return false;
}

void CabinetManager::clear_error(const Session& s, Marshaler& m) {
    if (s.erf == 0) return;
    (void)m.write_u32(s.erf + kErf_erfOper, 0);
    (void)m.write_u32(s.erf + kErf_erfType, 0);
    (void)m.write_u32(s.erf + kErf_fError, 0);
}

// ---------------------------------------------------------------------------
// Reading a cabinet through the caller's callbacks
// ---------------------------------------------------------------------------

std::optional<std::vector<u8>> CabinetManager::read_cabinet_image(
    Marshaler& m, const Session& s, i64 hf) {
    // SEEK_END is a hint, not the container length. Callers commonly present a
    // cabinet that lives at an offset inside a larger file — WiX Burn's
    // attached container is inside the bootstrapper's own image — and their
    // seek handler then reports the outer size. Real FDI never sizes the file
    // this way; it walks the CFHEADER. So treat the hint as an upper bound
    // only and stop when the caller's read callback reports EOF.
    const i64 hint = call_seek(s, hf, 0, kSeekEnd);
    if (call_seek(s, hf, 0, kSeekSet) < 0) return std::nullopt;

    std::vector<u8> image;
    if (hint > 0) image.reserve(static_cast<usize>(hint));
    while (true) {
        const u32 got = call_read(s, hf, m_io_buffer, kIoChunk);
        if (got == 0 || got > kIoChunk) break;
        for (u32 i = 0; i < got; ++i) {
            auto byte = m.read_u8(m_io_buffer + i);
            if (byte.is_err()) return std::nullopt;
            image.push_back(byte.value());
        }
        if (hint > 0 && image.size() >= static_cast<usize>(hint)) break;
    }
    if (image.empty()) return std::nullopt;
    VITREO_TRACE("[cabinet] image %zu bytes (hint %lld), magic %02x %02x %02x %02x\n",
                 image.size(), static_cast<long long>(hint),
                 image[0], image.size() > 1 ? image[1] : 0,
                 image.size() > 2 ? image[2] : 0,
                 image.size() > 3 ? image[3] : 0);
    return image;
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

void CabinetManager::impl_FDICreate(Marshaler& m) {
    // HFDI FDICreate(PFNALLOC, PFNFREE, PFNOPEN, PFNREAD, PFNWRITE, PFNCLOSE,
    //                PFNSEEK, int cpuType, PERF perf)
    Session s;
    s.alloc    = m.arg_ptr(0);
    s.free     = m.arg_ptr(1);
    s.open     = m.arg_ptr(2);
    s.read     = m.arg_ptr(3);
    s.write    = m.arg_ptr(4);
    s.close    = m.arg_ptr(5);
    s.seek     = m.arg_ptr(6);
    // arg 7 is cpuType, which is advisory on every modern platform.
    s.erf      = m.arg_ptr(8);
    s.is_64bit = m.cpu_is_64bit();

    if (s.open == 0 || s.read == 0 || s.seek == 0 || s.close == 0) {
        // Without I/O there is nothing FDI can do; report the allocation-style
        // failure FDICreate uses for a bad context rather than a null crash.
        if (s.erf != 0) (void)fail(s, m, FdiError::AllocFail);
        m.set_return(0);
        return;
    }

    const GuestAddr hfdi = m_next_hfdi;
    // The handle is opaque to callers but must still be a distinct, valid
    // pointer, so hand out one aligned slot per session.
    m_next_hfdi += 16u;
    m_sessions.emplace(hfdi, s);
    clear_error(s, m);
    VITREO_TRACE("[cabinet] FDICreate -> 0x%llx\n",
                 static_cast<unsigned long long>(hfdi));
    m.set_return(static_cast<u64>(hfdi));
}

void CabinetManager::impl_FDIDestroy(Marshaler& m) {
    const GuestAddr hfdi = m.arg_ptr(0);
    const bool known = m_sessions.erase(hfdi) != 0;
    VITREO_TRACE("[cabinet] FDIDestroy(0x%llx) -> %d\n",
                 static_cast<unsigned long long>(hfdi), known ? 1 : 0);
    m.set_return(known ? 1u : 0u);
}

void CabinetManager::impl_FDIIsCabinet(Marshaler& m) {
    // BOOL FDIIsCabinet(HFDI, INT_PTR hf, PFDICABINETINFO)
    const GuestAddr hfdi = m.arg_ptr(0);
    Session* s = find(hfdi);
    if (s == nullptr) { m.set_return(0); return; }
    const i64 hf = m.cpu_is_64bit() ? static_cast<i64>(m.arg(1))
                                    : static_cast<i64>(m.arg32(1));
    const GuestAddr info = m.arg_ptr(2);

    auto image = read_cabinet_image(m, *s, hf);
    if (!image.has_value()) {
        m.set_return(fail(*s, m, FdiError::NotACabinet) ? 1u : 0u);
        return;
    }
    auto parsed = compression::cabinet::Cabinet::parse(
        std::span<const u8>(image->data(), image->size()));
    if (parsed.is_err()) {
        m.set_return(fail(*s, m, FdiError::NotACabinet) ? 1u : 0u);
        return;
    }
    const auto& cab = parsed.value();
    const auto& hdr = cab.header();

    if (info != 0) {
        (void)m.write_u32(info + kCabInfo_cbCabinet, hdr.cabinet_size);
        (void)m.write_u16(info + kCabInfo_cFolders, hdr.folder_count);
        (void)m.write_u16(info + kCabInfo_cFiles, hdr.file_count);
        (void)m.write_u16(info + kCabInfo_setID, hdr.set_id);
        (void)m.write_u16(info + kCabInfo_iCabinet, hdr.cabinet_index);
        (void)m.write_u32(
            info + kCabInfo_fReserve,
            (hdr.flags & compression::cabinet::kFlagReservePresent) ? 1u : 0u);
        (void)m.write_u32(
            info + kCabInfo_hasprev,
            (hdr.flags & compression::cabinet::kFlagPrevCabinet) ? 1u : 0u);
        (void)m.write_u32(
            info + kCabInfo_hasnext,
            (hdr.flags & compression::cabinet::kFlagNextCabinet) ? 1u : 0u);
    }
    clear_error(*s, m);
    VITREO_TRACE("[cabinet] FDIIsCabinet: %u folder(s), %u file(s), set %u\n",
                 hdr.folder_count, hdr.file_count, hdr.set_id);
    m.set_return(1u);
}

void CabinetManager::impl_FDICopy(Marshaler& m) {
    // BOOL FDICopy(HFDI, LPSTR pszCabinet, LPSTR pszCabPath, INT flags,
    //              PFNFDINOTIFY, PFNFDIDECRYPT, void *pvUser)
    const GuestAddr hfdi = m.arg_ptr(0);
    Session* s = find(hfdi);
    if (s == nullptr) { m.set_return(0); return; }

    auto name_res = m.read_ansi_string(m.arg_ptr(1));
    auto path_res = m.read_ansi_string(m.arg_ptr(2));
    const std::string cab_name = name_res.is_ok() ? name_res.value()
                                                  : std::string{};
    const std::string cab_path = path_res.is_ok() ? path_res.value()
                                                  : std::string{};
    const GuestAddr notify   = m.arg_ptr(4);
    const GuestAddr decrypt  = m.arg_ptr(5);
    const GuestAddr pv_user  = m.arg_ptr(6);

    if (decrypt != 0) {
        // Encrypted cabinets need the caller's PFNFDIDECRYPT to be driven per
        // data block. Refusing is the honest answer; extracting the ciphertext
        // as if it were file content would corrupt every output silently.
        VITREO_TRACE("[cabinet] FDICopy: PFNFDIDECRYPT is not supported\n");
        m.set_return(fail(*s, m, FdiError::BadComprType) ? 1u : 0u);
        return;
    }

    const std::string full = cab_path + cab_name;
    const GuestAddr full_addr = write_scratch_ansi(m, full);
    if (full_addr == 0) {
        m.set_return(fail(*s, m, FdiError::AllocFail) ? 1u : 0u);
        return;
    }
    const i64 hf = call_open(*s, full_addr, kOpenReadBinary, kPmodeReadWrite);
    if (hf < 0) {
        VITREO_TRACE("[cabinet] FDICopy: cannot open '%s'\n", full.c_str());
        m.set_return(fail(*s, m, FdiError::CabinetNotFound) ? 1u : 0u);
        return;
    }

    auto image = read_cabinet_image(m, *s, hf);
    if (!image.has_value()) {
        (void)call_close(*s, hf);
        m.set_return(fail(*s, m, FdiError::CorruptCabinet) ? 1u : 0u);
        return;
    }
    auto parsed = compression::cabinet::Cabinet::parse(
        std::span<const u8>(image->data(), image->size()));
    if (parsed.is_err()) {
        (void)call_close(*s, hf);
        m.set_return(fail(*s, m, FdiError::NotACabinet) ? 1u : 0u);
        return;
    }
    const auto& cab = parsed.value();
    const auto& hdr = cab.header();
    const auto& layout = notify_layout(s->is_64bit);

    VITREO_TRACE("[cabinet] FDICopy('%s'): %u file(s)\n", full.c_str(),
                 hdr.file_count);

    // Every notification shares one record; FDI reuses its own the same way and
    // callers are documented not to retain it past the call.
    const GuestAddr record = scratch_alloc(layout.size);
    if (record == 0) {
        (void)call_close(*s, hf);
        m.set_return(fail(*s, m, FdiError::AllocFail) ? 1u : 0u);
        return;
    }
    const auto reset_record = [&]() {
        for (u32 i = 0; i < layout.size; i += 4) {
            (void)m.write_u32(record + i, 0);
        }
        (void)m.write_ptr(record + layout.pv, pv_user);
    };

    // fdintCABINET_INFO first, describing the container as a whole.
    reset_record();
    const GuestAddr next_cab = write_scratch_ansi(m, hdr.next_cabinet);
    const GuestAddr next_disk = write_scratch_ansi(m, hdr.next_disk);
    const GuestAddr path_copy = write_scratch_ansi(m, cab_path);
    (void)m.write_ptr(record + layout.psz1, next_cab);
    (void)m.write_ptr(record + layout.psz2, next_disk);
    (void)m.write_ptr(record + layout.psz3, path_copy);
    (void)m.write_u16(record + layout.setID, hdr.set_id);
    (void)m.write_u16(record + layout.iCabinet, hdr.cabinet_index);
    if (call_notify(*s, notify, FdiNotification::CabinetInfo, record) == -1) {
        (void)call_close(*s, hf);
        m.set_return(fail(*s, m, FdiError::UserAbort) ? 1u : 0u);
        return;
    }

    // A folder is the unit of compression, so decode each one once and slice
    // every file out of it. Extracting file-by-file would re-decompress the
    // whole folder per file.
    std::vector<u8> folder_bytes;
    usize           folder_loaded = static_cast<usize>(-1);
    bool            aborted = false;

    for (const auto& file : cab.files()) {
        if (file.spans_cabinets()) {
            // Multi-cabinet sets need fdintNEXT_CABINET and a caller that can
            // supply the continuation. Report the file as partial rather than
            // writing a truncated copy.
            reset_record();
            const GuestAddr name_addr = write_scratch_ansi(m, file.name);
            (void)m.write_ptr(record + layout.psz1, name_addr);
            (void)call_notify(*s, notify, FdiNotification::PartialFile, record);
            continue;
        }

        reset_record();
        const GuestAddr name_addr = write_scratch_ansi(m, file.name);
        (void)m.write_u32(record + layout.cb, file.size);
        (void)m.write_ptr(record + layout.psz1, name_addr);
        (void)m.write_u16(record + layout.date, file.date);
        (void)m.write_u16(record + layout.time, file.time);
        (void)m.write_u16(record + layout.attribs, file.attributes);
        (void)m.write_u16(record + layout.iFolder, file.folder_index);
        (void)m.write_u16(record + layout.setID, hdr.set_id);
        (void)m.write_u16(record + layout.iCabinet, hdr.cabinet_index);

        VITREO_TRACE("[cabinet] file '%s' size=%u folder=%u\n",
                     file.name.c_str(), file.size, file.folder_index);
        const i64 target = call_notify(*s, notify, FdiNotification::CopyFile,
                                       record);
        if (target == -1) { aborted = true; break; }
        if (target == 0) continue;  // Caller declined this file.

        if (folder_loaded != file.folder_index) {
            auto folder = cab.extract_folder(file.folder_index);
            if (folder.is_err()) {
                (void)call_close(*s, hf);
                m.set_return(fail(*s, m, FdiError::CorruptCabinet) ? 1u : 0u);
                return;
            }
            folder_bytes = std::move(folder.value());
            folder_loaded = file.folder_index;
        }
        const usize begin = file.folder_offset;
        if (begin > folder_bytes.size() ||
            begin + file.size > folder_bytes.size()) {
            (void)call_close(*s, hf);
            m.set_return(fail(*s, m, FdiError::CorruptCabinet) ? 1u : 0u);
            return;
        }

        usize written = 0;
        while (written < file.size) {
            const u32 chunk = static_cast<u32>(
                std::min<usize>(kIoChunk, file.size - written));
            for (u32 i = 0; i < chunk; ++i) {
                (void)m.write_u8(m_io_buffer + i,
                                 folder_bytes[begin + written + i]);
            }
            const u32 put = call_write(*s, target, m_io_buffer, chunk);
            if (put != chunk) {
                (void)call_close(*s, hf);
                m.set_return(fail(*s, m, FdiError::TargetFile) ? 1u : 0u);
                return;
            }
            written += chunk;
        }

        // fdintCLOSE_FILE_INFO hands the handle back so the caller closes it
        // and applies the timestamp and attributes.
        reset_record();
        (void)m.write_u32(record + layout.cb, 1u);
        (void)m.write_ptr(record + layout.psz1, name_addr);
        (void)m.write_ptr(record + layout.hf, static_cast<GuestAddr>(target));
        (void)m.write_u16(record + layout.date, file.date);
        (void)m.write_u16(record + layout.time, file.time);
        (void)m.write_u16(record + layout.attribs, file.attributes);
        (void)m.write_u16(record + layout.iFolder, file.folder_index);
        (void)m.write_u16(record + layout.setID, hdr.set_id);
        (void)m.write_u16(record + layout.iCabinet, hdr.cabinet_index);
        if (call_notify(*s, notify, FdiNotification::CloseFileInfo, record) ==
            -1) {
            aborted = true;
            break;
        }
    }

    (void)call_close(*s, hf);
    if (aborted) {
        m.set_return(fail(*s, m, FdiError::UserAbort) ? 1u : 0u);
        return;
    }
    clear_error(*s, m);
    m.set_return(1u);
}

void CabinetManager::impl_FDITruncateCabinet(Marshaler& m) {
    // Truncating rewrites the container, which this reader does not do. Fail
    // honestly rather than reporting a truncation that never happened.
    const GuestAddr hfdi = m.arg_ptr(0);
    if (Session* s = find(hfdi); s != nullptr) {
        (void)fail(*s, m, FdiError::NotACabinet);
    }
    VITREO_TRACE("[cabinet] FDITruncateCabinet is not implemented\n");
    m.set_return(0u);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

DllExports create_cabinet_thunks(CabinetManager& mgr) {
    DllExports exports;
    // Ordinals are contractual here: callers routinely bind cabinet.dll by
    // ordinal rather than by name (WiX Burn's delay-load stub asks for
    // ordinal 20), so a name-only table resolves to nothing. These match the
    // export table of the real cabinet.dll on Windows.
    constexpr u16 kOrdFDICreate         = 20;
    constexpr u16 kOrdFDIIsCabinet      = 21;
    constexpr u16 kOrdFDICopy           = 22;
    constexpr u16 kOrdFDIDestroy        = 23;
    constexpr u16 kOrdFDITruncateCabinet = 24;

    // Every FDI entry point is DIAMONDAPI, i.e. __cdecl: the caller cleans the
    // stack, so the stdcall pop count is zero on x86.
    exports.entries.push_back(ExportEntry{
        "FDICreate", kOrdFDICreate,
        [&mgr](Marshaler& m) { mgr.impl_FDICreate(m); }, 0});
    exports.entries.push_back(ExportEntry{
        "FDIIsCabinet", kOrdFDIIsCabinet,
        [&mgr](Marshaler& m) { mgr.impl_FDIIsCabinet(m); }, 0});
    exports.entries.push_back(ExportEntry{
        "FDICopy", kOrdFDICopy,
        [&mgr](Marshaler& m) { mgr.impl_FDICopy(m); }, 0});
    exports.entries.push_back(ExportEntry{
        "FDIDestroy", kOrdFDIDestroy,
        [&mgr](Marshaler& m) { mgr.impl_FDIDestroy(m); }, 0});
    exports.entries.push_back(ExportEntry{
        "FDITruncateCabinet", kOrdFDITruncateCabinet,
        [&mgr](Marshaler& m) { mgr.impl_FDITruncateCabinet(m); }, 0});
    return exports;
}

} // namespace vitreo::winapi
