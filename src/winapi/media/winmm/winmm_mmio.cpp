/// @file winmm_mmio.cpp
/// @brief mmio*: the buffered multimedia file I/O engine.
///
/// Sources: Microsoft Learn (mmioOpen, mmioRead, mmioWrite, mmioSeek,
/// mmioGetInfo, mmioSetInfo, mmioAdvance, mmioSetBuffer, mmioFlush,
/// mmioDescend, mmioAscend, mmioCreateChunk, mmioInstallIOProc,
/// mmioSendMessage, mmioRename, mmioStringToFOURCC, MMIOINFO, MMCKINFO,
/// MMIOPROC, "Performing Buffered File I/O"), mmiscapi.h (winmm_mmio_abi.hpp,
/// audited), and everything Learn leaves open measured on Windows by
/// tools/oracle_probe/winmm_mmio_probe (.expected = x64, .x86.expected =
/// x86; probe line names are cited below).
///
/// Model. An HMMIO is the guest address of a process-heap MMIOINFO, the one
/// the I/O procedure receives (probe p_log info_is_h=1). The engine keeps the
/// buffer state there: pchBuffer/cchBuffer, pchNext (the current position),
/// pchEndRead (the valid bytes), pchEndWrite (pchBuffer + cchBuffer, even
/// unbuffered: s_get_neg), lBufOffset (the file offset of pchBuffer) and
/// lDiskOffset (where the I/O procedure's file pointer is). Seeks are lazy:
/// lBufOffset moves, the I/O procedure is only told to seek before it next
/// reads or writes (c_get3, p_log). Memory files (FOURCC_MEM) are their
/// buffer: lBufOffset stays 0 and nothing reaches an I/O procedure.

#include "winapi/media/winmm/winmm_internal.hpp"
#include "winapi/media/winmm/winmm_mmio_abi.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/user32/messages/user32_case.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/win32_full_path.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {

using namespace mmio_sdk;

namespace {

/// MMIOINFO in host form. Pointers keep the guest's width (masked to 32 bits
/// for an x86 guest), offsets are LONGs.
struct Info {
    u32 flags{0};
    u32 fcc{0};
    u64 proc{0};
    u32 err{0};
    u64 task{0};
    i32 cch{0};
    u64 buf{0};
    u64 next{0};
    u64 endr{0};
    u64 endw{0};
    i32 bufoff{0};
    i32 diskoff{0};
    u32 adw[3]{0, 0, 0};
    u32 res1{0};
    u32 res2{0};
    u64 hmmio{0};
};

[[nodiscard]] i32 rel(u64 p, u64 base) noexcept {
    return static_cast<i32>(static_cast<i64>(p - base));
}
[[nodiscard]] u64 ptr_add(u64 base, i64 delta, bool x64) noexcept {
    const u64 v = base + static_cast<u64>(delta);
    return x64 ? v : (v & 0xFFFFFFFFull);
}

bool load_info(const GuestMemory& mem, GuestAddr at, bool x64, Info& o) {
    const MmioInfoLayout& l = mmioinfo_layout(x64);
    std::array<Byte, 128> raw{};
    if (at == 0 || mem.read_bytes(at, raw.data(), l.size).is_err()) return false;
    const auto r32 = [&](u32 off) { u32 v; std::memcpy(&v, raw.data() + off, 4); return v; };
    const auto rp = [&](u32 off) -> u64 {
        if (!x64) return r32(off);
        u64 v;
        std::memcpy(&v, raw.data() + off, 8);
        return v;
    };
    o.flags = r32(l.off_dwFlags);
    o.fcc = r32(l.off_fccIOProc);
    o.proc = rp(l.off_pIOProc);
    o.err = r32(l.off_wErrorRet);
    o.task = rp(l.off_htask);
    o.cch = static_cast<i32>(r32(l.off_cchBuffer));
    o.buf = rp(l.off_pchBuffer);
    o.next = rp(l.off_pchNext);
    o.endr = rp(l.off_pchEndRead);
    o.endw = rp(l.off_pchEndWrite);
    o.bufoff = static_cast<i32>(r32(l.off_lBufOffset));
    o.diskoff = static_cast<i32>(r32(l.off_lDiskOffset));
    for (u32 i = 0; i < 3; ++i) o.adw[i] = r32(l.off_adwInfo + 4 * i);
    o.res1 = r32(l.off_dwReserved1);
    o.res2 = r32(l.off_dwReserved2);
    o.hmmio = rp(l.off_hmmio);
    return true;
}

bool store_info(GuestMemory& mem, GuestAddr at, bool x64, const Info& o) {
    const MmioInfoLayout& l = mmioinfo_layout(x64);
    std::array<Byte, 128> raw{};
    const auto w32 = [&](u32 off, u32 v) { std::memcpy(raw.data() + off, &v, 4); };
    const auto wp = [&](u32 off, u64 v) {
        if (x64) std::memcpy(raw.data() + off, &v, 8);
        else w32(off, static_cast<u32>(v));
    };
    w32(l.off_dwFlags, o.flags);
    w32(l.off_fccIOProc, o.fcc);
    wp(l.off_pIOProc, o.proc);
    w32(l.off_wErrorRet, o.err);
    wp(l.off_htask, o.task);
    w32(l.off_cchBuffer, static_cast<u32>(o.cch));
    wp(l.off_pchBuffer, o.buf);
    wp(l.off_pchNext, o.next);
    wp(l.off_pchEndRead, o.endr);
    wp(l.off_pchEndWrite, o.endw);
    w32(l.off_lBufOffset, static_cast<u32>(o.bufoff));
    w32(l.off_lDiskOffset, static_cast<u32>(o.diskoff));
    for (u32 i = 0; i < 3; ++i) w32(l.off_adwInfo + 4 * i, o.adw[i]);
    w32(l.off_dwReserved1, o.res1);
    w32(l.off_dwReserved2, o.res2);
    wp(l.off_hmmio, o.hmmio);
    return at != 0 && !mem.write_bytes(at, raw.data(), l.size).is_err();
}

/// Guest ANSI bytes (CP_ACP) to the UTF-8 the VFS takes.
std::string acp_to_utf8(std::string_view acp) {
    if (acp.empty()) return {};
    std::vector<u16> wide(acp.size() * 2 + 1);
    const auto r = codepage::multi_byte_to_wide(
        codepage::kCpAcp, 0, reinterpret_cast<const u8*>(acp.data()), acp.size(),
        wide.data(), wide.size());
    if (!r.ok()) return std::string(acp);
    return utf16_to_utf8(wide.data(), static_cast<int>(r.count));
}

/// UTF-8 to CP_ACP bytes with the '?' default character (WideCharToMultiByte
/// CP_ACP, NULL lpDefaultChar).
std::string utf8_to_acp_bytes(std::string_view utf8) {
    const std::vector<u16> wide = utf8_to_utf16(utf8);
    if (wide.empty()) return {};
    std::string out(wide.size() * 4 + 1, '\0');
    const auto r = codepage::wide_to_multi_byte(
        codepage::kCpAcp, 0, wide.data(), wide.size(),
        reinterpret_cast<u8*>(out.data()), out.size(), nullptr, false);
    out.resize(r.ok() ? r.count : 0);
    return out;
}

/// mmioStringToFOURCC on CP_ACP bytes: the first four characters, blank
/// padded; MMIO_TOUPPER uppercases them in the ANSI code page (probe
/// fourcc_hi_up: 0xE9 -> 0xC9).
u32 fourcc_from_acp(std::string_view s, u32 flags) {
    u32 out = 0;
    for (u32 i = 0; i < 4; ++i) {
        u8 c = i < s.size() && s[i] != '\0' ? static_cast<u8>(s[i]) : static_cast<u8>(' ');
        if (i < s.size() && s[i] == '\0') s = s.substr(0, i);
        if ((flags & MMIO_TOUPPER) != 0) {
            if (codepage::ansi_code_page() == 1252) c = user32_case::upper_ansi(c);
            else if (c >= 'a' && c <= 'z') c = static_cast<u8>(c - 'a' + 'A');
        }
        out |= static_cast<u32>(c) << (8 * i);
    }
    return out;
}

/// The I/O procedure a "file.ext+element" name selects: the extension of the
/// part before the first '+' (Learn mmioOpen; probe pp_open_dots uses the
/// last extension, pp_open_noext fails).
std::optional<u32> plus_extension_fourcc(std::string_view acp_name) {
    const usize plus = acp_name.find('+');
    if (plus == std::string_view::npos) return std::nullopt;
    const std::string_view head = acp_name.substr(0, plus);
    const usize sep = head.find_last_of("\\/:");
    const usize dot = head.find_last_of('.');
    if (dot == std::string_view::npos || (sep != std::string_view::npos && dot < sep)) {
        return 0u;
    }
    return fourcc_from_acp(head.substr(dot + 1), MMIO_TOUPPER);
}

capsule::ShareMode share_for(u32 flags) {
    // MMIO_COMPAT shares both ways (probe o_compat_rw, o_denynone_compat).
    switch (flags & MMIO_SHAREMODE) {
    case MMIO_EXCLUSIVE: return capsule::ShareMode::None;
    case MMIO_DENYWRITE: return capsule::ShareMode::Read;
    case MMIO_DENYREAD: return capsule::ShareMode::Write;
    default: return capsule::ShareMode::ReadWrite;
    }
}

constexpr i64 kProcError = -1;

} // namespace

/// The mmio engine, with access to WinmmManager's mmio state.
struct MmioOps {
    using Handle = WinmmManager::MmioHandle;
    using Kind = WinmmManager::MmioProcKind;

    WinmmManager& mgr;
    GuestAddr h;
    Handle& st;
    Info in;

    [[nodiscard]] GuestMemory& mem() { return mgr.m_memory; }
    [[nodiscard]] bool x64() const { return st.is_64bit; }
    [[nodiscard]] bool buffered() const { return in.buf != 0 && in.cch > 0; }
    [[nodiscard]] i32 pos() const { return in.bufoff + rel(in.next, in.buf); }
    [[nodiscard]] i32 buffer_end_offset() const { return in.bufoff + rel(in.endr, in.buf); }
    void commit() { (void)store_info(mem(), h, x64(), in); }

    // ---- guest memory helpers ----------------------------------------------
    static bool copy_guest(GuestMemory& m, GuestAddr dst, GuestAddr src, usize n) {
        std::vector<Byte> tmp(std::min<usize>(n, 1u << 20));
        while (n != 0) {
            const usize chunk = std::min(n, tmp.size());
            if (m.read_bytes(src, tmp.data(), chunk).is_err()) return false;
            if (m.write_bytes(dst, tmp.data(), chunk).is_err()) return false;
            src += chunk;
            dst += chunk;
            n -= chunk;
        }
        return true;
    }

    // ---- the built-in DOS I/O procedure ------------------------------------
    // MMIOINFO.adwInfo[0] holds the kernel32 file HANDLE (Learn mmioOpen:
    // "adwInfo[0] ... file handle"; probe c_get a0_nz=1).
    static capsule::FileHandle dos_file(WinmmManager& mgr, const Info& i) {
        if (!mgr.m_mmio_services.vfs_handle_for) return capsule::INVALID_FILE_HANDLE;
        return mgr.m_mmio_services.vfs_handle_for(i.adw[0]);
    }

    /// Opens @p path for MMIOM_OPEN. Every failure is MMIOERR_FILENOTFOUND
    /// (probe o_missing_dir, o_create_nodir, o_denywrite: err=101).
    static u32 dos_open(WinmmManager& mgr, Info& i, const std::string& path) {
        const u32 rw = i.flags & MMIO_RWMODE;
        const capsule::AccessMode access =
            rw == MMIO_READ ? capsule::AccessMode::Read
            : rw == MMIO_WRITE ? capsule::AccessMode::Write
                               : capsule::AccessMode::ReadWrite;
        const u32 desired = rw == MMIO_READ ? GENERIC_READ
                            : rw == MMIO_WRITE ? GENERIC_WRITE
                                               : GENERIC_READ | GENERIC_WRITE;
        if (path.empty()) return MMIOERR_FILENOTFOUND;
        auto opened = mgr.m_vfs.open(
            path, access, share_for(i.flags),
            (i.flags & MMIO_CREATE) != 0 ? capsule::CreationDisposition::CreateAlways
                                         : capsule::CreationDisposition::OpenExisting);
        if (opened.is_err()) return MMIOERR_FILENOTFOUND;
        if (!mgr.m_mmio_services.adopt_file) {
            (void)mgr.m_vfs.close(opened.value());
            WINMM_UNIMPLEMENTED("mmioOpen: no kernel32 file-handle service for the DOS IOProc");
            return MMIOERR_CANNOTOPEN;
        }
        const auto full = capsule::win32_full_path(path, mgr.m_vfs.cwd());
        const u32 handle = mgr.m_mmio_services.adopt_file(opened.value(), full ? full->path : path, desired);
        if (handle == 0) {
            (void)mgr.m_vfs.close(opened.value());
            return MMIOERR_TOOMANYOPENFILES;
        }
        i.adw[0] = handle;
        i.diskoff = 0;
        return 0;
    }

    static i64 dos_proc(WinmmManager& mgr, Info& i, u32 msg, u64 l1, u64 l2) {
        GuestMemory& m = mgr.m_memory;
        switch (msg) {
        case MMIOM_OPEN: {
            if (l1 == 0) {
                // NULL name: adwInfo[0] is an open file HANDLE; the file
                // pointer is where I/O starts (probe fh_get bufoff=3).
                const capsule::FileHandle f = dos_file(mgr, i);
                if (f != capsule::INVALID_FILE_HANDLE) {
                    auto p = mgr.m_vfs.seek(f, 0, capsule::SeekOrigin::Current);
                    if (p.is_ok()) {
                        i.diskoff = static_cast<i32>(p.value());
                        i.bufoff = i.diskoff;
                    }
                }
                return 0;
            }
            Marshaler mm(m);
            auto name = mm.read_ansi_string(static_cast<GuestAddr>(l1), 32768);
            if (name.is_err()) return MMIOERR_FILENOTFOUND;
            return dos_open(mgr, i, acp_to_utf8(name.value()));
        }
        case MMIOM_CLOSE:
            // MMIO_FHOPEN keeps the HANDLE open (probe fh_still_open).
            if ((static_cast<u32>(l1) & MMIO_FHOPEN) == 0 && mgr.m_mmio_services.close_file) {
                mgr.m_mmio_services.close_file(i.adw[0]);
            }
            return 0;
        case MMIOM_READ: {
            const capsule::FileHandle f = dos_file(mgr, i);
            const i32 want = static_cast<i32>(l2);
            if (f == capsule::INVALID_FILE_HANDLE || want < 0) return kProcError;
            std::vector<u8> tmp(std::min<usize>(static_cast<usize>(want), 1u << 20));
            i64 total = 0;
            GuestAddr dst = static_cast<GuestAddr>(l1);
            while (total < want) {
                const usize chunk = std::min<usize>(tmp.size(), static_cast<usize>(want - total));
                auto r = mgr.m_vfs.read(f, std::span<u8>(tmp.data(), chunk));
                if (r.is_err()) {
                    if (total == 0) return kProcError;
                    break;
                }
                if (r.value() == 0) break;
                if (m.write_bytes(dst, reinterpret_cast<const Byte*>(tmp.data()), r.value()).is_err()) {
                    return kProcError;
                }
                dst += r.value();
                total += static_cast<i64>(r.value());
            }
            i.diskoff += static_cast<i32>(total);
            return total;
        }
        case MMIOM_WRITE:
        case MMIOM_WRITEFLUSH: {
            const capsule::FileHandle f = dos_file(mgr, i);
            const i32 want = static_cast<i32>(l2);
            if (f == capsule::INVALID_FILE_HANDLE || want < 0) return kProcError;
            std::vector<u8> tmp(std::min<usize>(static_cast<usize>(want), 1u << 20));
            i64 total = 0;
            GuestAddr src = static_cast<GuestAddr>(l1);
            // WriteFile of nothing still checks the handle's access.
            if (want == 0 && mgr.m_vfs.write(f, std::span<const u8>()).is_err()) return kProcError;
            while (total < want) {
                const usize chunk = std::min<usize>(tmp.size(), static_cast<usize>(want - total));
                if (m.read_bytes(src, reinterpret_cast<Byte*>(tmp.data()), chunk).is_err()) {
                    return kProcError;
                }
                auto r = mgr.m_vfs.write(f, std::span<const u8>(tmp.data(), chunk));
                if (r.is_err() || r.value() == 0) {
                    if (total == 0) return kProcError;
                    break;
                }
                src += r.value();
                total += static_cast<i64>(r.value());
            }
            i.diskoff += static_cast<i32>(total);
            return total;
        }
        case MMIOM_SEEK: {
            const capsule::FileHandle f = dos_file(mgr, i);
            const i32 off = static_cast<i32>(l1);
            const u32 origin = static_cast<u32>(l2);
            if (f == capsule::INVALID_FILE_HANDLE || origin > SEEK_END_VALUE) return kProcError;
            i64 base = 0;
            if (origin == SEEK_CUR_VALUE) {
                auto cur = mgr.m_vfs.seek(f, 0, capsule::SeekOrigin::Current);
                if (cur.is_err()) return kProcError;
                base = static_cast<i64>(cur.value());
            } else if (origin == SEEK_END_VALUE) {
                auto end = mgr.m_vfs.seek(f, 0, capsule::SeekOrigin::End);
                if (end.is_err()) return kProcError;
                base = static_cast<i64>(end.value());
            }
            // SetFilePointer: a negative result is an error, the pointer stays.
            const i64 target = base + off;
            if (target < 0 || target > 0x7FFFFFFF) return kProcError;
            auto r = mgr.m_vfs.seek(f, target, capsule::SeekOrigin::Begin);
            if (r.is_err()) return kProcError;
            i.diskoff = static_cast<i32>(r.value());
            return i.diskoff;
        }
        case MMIOM_RENAME: {
            Marshaler mm(m);
            auto from = mm.read_ansi_string(static_cast<GuestAddr>(l1), 32768);
            auto to = mm.read_ansi_string(static_cast<GuestAddr>(l2), 32768);
            if (from.is_err() || to.is_err()) return MMIOERR_FILENOTFOUND;
            return dos_rename(mgr, acp_to_utf8(from.value()), acp_to_utf8(to.value()));
        }
        default:
            // Unknown messages answer 0 (probe o_send_user, o_send_low).
            return 0;
        }
    }

    /// MoveFile semantics: an existing target fails (probe o_rename_onto);
    /// every failure is MMIOERR_FILENOTFOUND (o_rename_missing).
    static u32 dos_rename(WinmmManager& mgr, const std::string& from, const std::string& to) {
        if (from.empty() || to.empty() || mgr.m_vfs.stat(to).is_ok()) return MMIOERR_FILENOTFOUND;
        return mgr.m_vfs.rename(from, to).is_ok() ? 0u : MMIOERR_FILENOTFOUND;
    }

    /// The built-in MEM procedure: a memory file has no backing store, so
    /// anything that would move data to or from one fails.
    static i64 mem_proc(u32 msg) {
        switch (msg) {
        case MMIOM_OPEN:
        case MMIOM_CLOSE: return 0;
        case MMIOM_READ:
        case MMIOM_WRITE:
        case MMIOM_WRITEFLUSH:
        case MMIOM_SEEK: return kProcError;
        default: return 0;
        }
    }

    static i64 guest_call(WinmmManager& mgr, GuestAddr proc, GuestAddr info,
                          u32 msg, u64 l1, u64 l2, bool is_64bit) {
        if (!mgr.m_mmio_services.call) {
            WINMM_UNIMPLEMENTED("mmio: custom MMIOPROC without a guest-call service");
            return kProcError;
        }
        const u64 mask = is_64bit ? ~0ull : 0xFFFFFFFFull;
        const u64 args[4] = {info, msg, l1 & mask, l2 & mask};
        const u64 r = mgr.m_mmio_services.call(proc, args, 4, is_64bit);
        return is_64bit ? static_cast<i64>(r) : static_cast<i64>(static_cast<i32>(static_cast<u32>(r)));
    }

    /// Send @p msg to this file's I/O procedure. A guest procedure sees the
    /// MMIOINFO at the HMMIO address and may change it (lDiskOffset,
    /// adwInfo), so the engine's copy is written out first and read back.
    i64 proc(u32 msg, u64 l1, u64 l2) {
        switch (st.kind) {
        case Kind::Dos: return dos_proc(mgr, in, msg, l1, l2);
        case Kind::Mem: return mem_proc(msg);
        case Kind::Custom: {
            commit();
            const i64 r = guest_call(mgr, static_cast<GuestAddr>(in.proc), h, msg, l1, l2, x64());
            (void)load_info(mem(), h, x64(), in);
            return r;
        }
        }
        return kProcError;
    }

    // ---- buffer engine -----------------------------------------------------
    bool disk_seek(i32 target) {
        if (in.diskoff == target) return true;
        return proc(MMIOM_SEEK, static_cast<u64>(static_cast<i64>(target)), SEEK_SET_VALUE) != kProcError;
    }

    /// Write a dirty buffer [pchBuffer, pchEndRead) at lBufOffset. The
    /// buffer is kept (probe w_after_flush); mmioFlush and mmioClose send
    /// MMIOM_WRITEFLUSH, the rest MMIOM_WRITE (pw_log).
    u32 flush(u32 msg) {
        if (st.kind == Kind::Mem || (in.flags & MMIO_DIRTY) == 0) return 0;
        // A dirty buffer is written even when it holds nothing: a read-only
        // file then fails the close (ro_close after ro_setinfo_dirty).
        const i32 n = std::max(rel(in.endr, in.buf), 0);
        if (in.buf != 0) {
            if (!disk_seek(in.bufoff)) return MMIOERR_CANNOTWRITE;
            if (proc(msg, in.buf, static_cast<u64>(n)) != n) return MMIOERR_CANNOTWRITE;
        }
        in.flags &= ~MMIO_DIRTY;
        return 0;
    }

    /// Empty the buffer at the current position.
    void empty_at(i32 offset) {
        in.bufoff = offset;
        in.next = in.buf;
        in.endr = in.buf;
    }

    /// mmioAdvance(MMIO_READ): write a dirty buffer, then refill from the
    /// current position (am_adv_mid: lBufOffset = old offset + pchNext).
    u32 fill() {
        if (const u32 e = flush(MMIOM_WRITE); e != 0) return e;
        empty_at(pos());
        if (!disk_seek(in.bufoff)) return MMIOERR_CANNOTREAD;
        const i64 r = proc(MMIOM_READ, in.buf, static_cast<u64>(in.cch));
        if (r < 0) return MMIOERR_CANNOTREAD;
        in.endr = ptr_add(in.buf, r, x64());
        return 0;
    }

    /// Move the current position. Inside [lBufOffset, end of valid data] it
    /// is a pointer move; elsewhere the buffer is written and emptied at the
    /// new offset and the disk is not touched (r_get3, s_get2, c_get3).
    bool seek_to(i64 target) {
        const i32 t = static_cast<i32>(target);
        if (st.kind == Kind::Mem) {
            if (target < 0 || target > rel(in.endr, in.buf)) return false;
            in.next = ptr_add(in.buf, target, x64());
            return true;
        }
        if (t >= in.bufoff && t <= buffer_end_offset()) {
            in.next = ptr_add(in.buf, t - in.bufoff, x64());
            return true;
        }
        if (flush(MMIOM_WRITE) != 0) return false;
        empty_at(t);
        return true;
    }

    /// The file's end: the I/O procedure's end, or further when buffered
    /// data not yet written extends it (w_seek_end on a dirty buffer = 50).
    i64 file_end() {
        if (st.kind == Kind::Mem) return rel(in.endr, in.buf);
        const i64 disk = proc(MMIOM_SEEK, 0, SEEK_END_VALUE);
        if (disk == kProcError) return kProcError;
        return std::max<i64>(disk, buffer_end_offset());
    }

    bool grow_mem(i32 new_cch) {
        if (!st.owns_buffer || new_cch <= in.cch || !mgr.m_mmio_services.realloc) return false;
        const GuestAddr nb = mgr.m_mmio_services.realloc(static_cast<GuestAddr>(in.buf), static_cast<u64>(new_cch));
        if (nb == 0) return false;
        const i32 next = rel(in.next, in.buf), endr = rel(in.endr, in.buf);
        in.buf = nb;
        in.cch = new_cch;
        in.next = ptr_add(nb, next, x64());
        in.endr = ptr_add(nb, endr, x64());
        in.endw = ptr_add(nb, new_cch, x64());
        return true;
    }

    /// mmioRead's engine: @p dst is guest memory. -1 on failure.
    i64 read(GuestAddr dst, i32 cch) {
        if (st.kind == Kind::Mem) {
            const i32 n = std::clamp(rel(in.endr, in.next), 0, cch);
            if (n > 0 && !copy_guest(mem(), dst, static_cast<GuestAddr>(in.next), static_cast<usize>(n))) return -1;
            in.next = ptr_add(in.next, n, x64());
            return n;
        }
        if (!buffered()) {
            if (!disk_seek(in.bufoff)) return -1;
            const i64 r = proc(MMIOM_READ, dst, static_cast<u64>(cch));
            if (r < 0) return -1;
            in.bufoff += static_cast<i32>(r);
            return r;
        }
        i64 total = 0;
        i32 remaining = cch;
        for (;;) {
            const i32 take = std::clamp(rel(in.endr, in.next), 0, remaining);
            if (take > 0) {
                if (!copy_guest(mem(), dst + static_cast<GuestAddr>(total), static_cast<GuestAddr>(in.next),
                                static_cast<usize>(take))) {
                    return -1;
                }
                in.next = ptr_add(in.next, take, x64());
                remaining -= take;
                total += take;
            }
            if (remaining == 0) break;
            if (remaining > in.cch) {
                // Larger than the buffer: straight from the file into the
                // caller's memory, buffer left empty after it (s_read20,
                // s_read40; s_read16 still goes through the buffer).
                if (flush(MMIOM_WRITE) != 0) return -1;
                empty_at(pos());
                if (!disk_seek(in.bufoff)) return -1;
                const i64 r = proc(MMIOM_READ, dst + static_cast<GuestAddr>(total), static_cast<u64>(remaining));
                if (r < 0) return -1;
                in.bufoff += static_cast<i32>(r);
                total += r;
                break;
            }
            if (fill() != 0) return -1;
            if (in.endr == in.next) break;  // end of file
        }
        return total;
    }

    /// mmioWrite's engine: @p src is guest memory. -1 on failure.
    i64 write(GuestAddr src, i32 cch) {
        if (st.kind == Kind::Mem) {
            // All or nothing: a write that does not fit grows an expandable
            // file to max(cchBuffer + adwInfo[0], needed) (mg_w10b, me_get1)
            // or fails untouched (mw_write70, mf_write3).
            const i64 needed = static_cast<i64>(rel(in.next, in.buf)) + cch;
            if (needed > in.cch) {
                if (!st.expandable) return -1;
                const i64 grown = std::max<i64>(static_cast<i64>(in.cch) + in.adw[0], needed);
                if (grown > 0x7FFFFFFF || !grow_mem(static_cast<i32>(grown))) return -1;
            }
            if (cch > 0 && !copy_guest(mem(), static_cast<GuestAddr>(in.next), src, static_cast<usize>(cch))) return -1;
            in.next = ptr_add(in.next, cch, x64());
            if (rel(in.next, in.endr) > 0) in.endr = in.next;
            in.flags |= MMIO_DIRTY;
            return cch;
        }
        if (!buffered()) {
            if (!disk_seek(in.bufoff)) return -1;
            const i64 r = proc(MMIOM_WRITE, src, static_cast<u64>(cch));
            if (r < 0) return -1;
            in.bufoff += static_cast<i32>(r);
            return r;
        }
        // Through the buffer, a buffer-full at a time; a full buffer is
        // written and emptied (sw_get20, s_getw20 on a read-only file).
        i64 total = 0;
        i32 remaining = cch;
        for (;;) {
            const i32 take = std::clamp(rel(in.endw, in.next), 0, remaining);
            if (take > 0) {
                if (!copy_guest(mem(), static_cast<GuestAddr>(in.next), src + static_cast<GuestAddr>(total),
                                static_cast<usize>(take))) {
                    return -1;
                }
                in.next = ptr_add(in.next, take, x64());
                if (rel(in.next, in.endr) > 0) in.endr = in.next;
                in.flags |= MMIO_DIRTY;
                remaining -= take;
                total += take;
            }
            if (remaining == 0) break;
            if (flush(MMIOM_WRITE) != 0) return -1;
            empty_at(pos());
        }
        return total;
    }

    /// Write host bytes (chunk headers, sizes, the pad byte) through the
    /// engine; the I/O procedure takes guest memory, so they are staged in a
    /// process-heap block.
    i64 write_host(const u8* bytes, i32 n) {
        if (!mgr.m_mmio_services.alloc) return -1;
        const GuestAddr tmp = mgr.m_mmio_services.alloc(static_cast<u64>(n));
        if (tmp == 0) return -1;
        i64 r = -1;
        if (!mem().write_bytes(tmp, reinterpret_cast<const Byte*>(bytes), static_cast<usize>(n)).is_err()) {
            r = write(tmp, n);
        }
        if (mgr.m_mmio_services.free) mgr.m_mmio_services.free(tmp);
        return r;
    }

    void free_buffer() {
        if (st.owns_buffer && in.buf != 0 && mgr.m_mmio_services.free) {
            mgr.m_mmio_services.free(static_cast<GuestAddr>(in.buf));
        }
        st.owns_buffer = false;
    }
};

namespace {

/// Look up an HMMIO under m_mmio_mutex.
template <typename Map>
auto* find_handle(Map& handles, GuestAddr h) {
    auto it = handles.find(h);
    return it == handles.end() ? nullptr : &it->second;
}

} // namespace

void WinmmManager::set_mmio_services(MmioServices services) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    m_mmio_services = std::move(services);
}

// HMMIO mmioOpenA/W(LPSTR/LPWSTR pszFileName, LPMMIOINFO pmmioinfo, DWORD fdwOpen)
void WinmmManager::impl_mmioOpen(Marshaler& m, bool wide) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const bool x64 = m.cpu_is_64bit();
    const MmioInfoLayout& layout = mmioinfo_layout(x64);
    const GuestAddr name_ptr = m.arg_ptr(0);
    const GuestAddr info_ptr = m.arg_ptr(1);
    u32 flags = m.arg32(2);

    // mmioOpen writes nothing to the caller's MMIOINFO but wErrorRet
    // (probe o_garbage_after, o_missing_info).
    const auto set_err = [&](u32 e) {
        if (info_ptr != 0) (void)m.write_u32(info_ptr + layout.off_wErrorRet, e);
    };
    Info caller{};
    if (info_ptr != 0 && !load_info(m_memory, info_ptr, x64, caller)) {
        m.set_return(0);
        return;
    }

    // The name as CP_ACP bytes (what an IOProc receives) and as UTF-8.
    std::optional<std::string> name_acp;
    std::optional<std::string> name_utf8;
    if (name_ptr != 0) {
        if (wide) {
            auto w = m.read_wide_string(name_ptr, 32768);
            if (w.is_ok()) {
                name_utf8 = w.value();
                name_acp = utf8_to_acp_bytes(w.value());
            }
        } else {
            auto a = m.read_ansi_string(name_ptr, 32768);
            if (a.is_ok()) {
                name_acp = a.value();
                name_utf8 = acp_to_utf8(a.value());
            }
        }
        if (!name_utf8) {
            set_err(MMIOERR_FILENOTFOUND);
            m.set_return(0);
            return;
        }
    }

    // MMIO_PARSE: qualify the name in place and return TRUE; the file is
    // not touched (Learn mmioOpen; probe o_parse*: GetFullPathName form,
    // case kept, "+" not special, "" fails).
    if ((flags & MMIO_PARSE) != 0) {
        const auto full = name_utf8 && !name_utf8->empty()
            ? capsule::win32_full_path(*name_utf8, m_vfs.cwd())
            : std::nullopt;
        // Learn: szFilename must be at least 128 characters long.
        const bool fits = full && (wide ? utf8_to_utf16(full->path).size()
                                        : utf8_to_acp_bytes(full->path).size()) < 128;
        if (!fits) {
            set_err(MMIOERR_FILENOTFOUND);
            m.set_return(0);
            return;
        }
        const bool ok = wide ? m.write_wide_string(name_ptr, full->path).is_ok()
                             : m.write_ansi_string(name_ptr, utf8_to_acp_bytes(full->path)).is_ok();
        set_err(ok ? 0u : MMIOERR_FILENOTFOUND);
        m.set_return(ok ? 1u : 0u);
        return;
    }
    // MMIO_DELETE: TRUE, or FALSE with MMIOERR_FILENOTFOUND (o_delete*).
    if ((flags & MMIO_DELETE) != 0) {
        const bool ok = name_utf8 && !name_utf8->empty() && m_vfs.remove(*name_utf8).is_ok();
        set_err(ok ? 0u : MMIOERR_FILENOTFOUND);
        m.set_return(ok ? 1u : 0u);
        return;
    }
    // MMIO_EXIST: TRUE when the file exists. The name is not written back
    // (the probe passes a read-only literal and Windows does not fault).
    if ((flags & MMIO_EXIST) != 0) {
        bool ok = false;
        if (name_utf8 && !name_utf8->empty()) {
            auto info = m_vfs.stat(*name_utf8);
            ok = info.is_ok() &&
                 (info.value().attributes & capsule::FileAttributes::Directory) == capsule::FileAttributes::None;
        }
        set_err(ok ? 0u : MMIOERR_FILENOTFOUND);
        m.set_return(ok ? 1u : 0u);
        return;
    }
    if ((flags & MMIO_GETTEMP) != 0) {
        WINMM_UNIMPLEMENTED("mmioOpen: MMIO_GETTEMP");
        set_err(MMIOERR_FILENOTFOUND);
        m.set_return(0);
        return;
    }

    // Choose the I/O procedure: pIOProc, else fccIOProc, else the extension
    // before a '+', else DOS (Learn mmioOpen; probe pd_both: pIOProc wins).
    MmioHandle st{};
    st.is_64bit = x64;
    Info in = caller;
    GuestAddr proc_addr = 0;
    const auto resolve = [&](u32 fcc) -> bool {
        for (auto it = m_mmio_procs.rbegin(); it != m_mmio_procs.rend(); ++it) {
            if (it->fcc == fcc) {
                proc_addr = it->proc;
                st.kind = MmioProcKind::Custom;
                return true;
            }
        }
        if (fcc == FOURCC_DOS) { st.kind = MmioProcKind::Dos; return true; }
        if (fcc == FOURCC_MEM) { st.kind = MmioProcKind::Mem; return true; }
        return false;
    };
    if (caller.proc != 0) {
        proc_addr = static_cast<GuestAddr>(caller.proc);
        st.kind = MmioProcKind::Custom;
    } else if (caller.fcc != 0) {
        if (!resolve(caller.fcc)) {  // p_unknown_fcc
            set_err(MMIOERR_FILENOTFOUND);
            m.set_return(0);
            return;
        }
    } else if (auto ext = name_acp ? plus_extension_fourcc(*name_acp) : std::nullopt) {
        if (*ext == 0 || !resolve(*ext)) {  // pp_open_noext, pp_open_unknown
            set_err(MMIOERR_FILENOTFOUND);
            m.set_return(0);
            return;
        }
        in.fcc = *ext;
    } else {
        // An installed FOURCC_DOS procedure replaces the built-in one
        // (probe p_open_dos_overridden).
        (void)resolve(FOURCC_DOS);
        in.fcc = FOURCC_DOS;
    }
    // A guest that found a built-in procedure and passed it back gets it.
    if (st.kind == MmioProcKind::Custom && proc_addr != 0) {
        if (proc_addr == m_mmio_dos_proc) st.kind = MmioProcKind::Dos;
        else if (proc_addr == m_mmio_mem_proc) st.kind = MmioProcKind::Mem;
    }
    if (st.kind == MmioProcKind::Dos) {
        if (in.fcc == 0 && caller.proc == 0) in.fcc = FOURCC_DOS;
        in.proc = proc_addr != 0 ? proc_addr : m_mmio_dos_proc;
    } else if (st.kind == MmioProcKind::Mem) {
        in.proc = proc_addr != 0 ? proc_addr : m_mmio_mem_proc;
    } else {
        in.proc = proc_addr;
    }

    if (!m_mmio_services.alloc) {
        WINMM_UNIMPLEMENTED("mmioOpen: no process-heap service for the MMIOINFO");
        set_err(MMIOERR_OUTOFMEMORY);
        m.set_return(0);
        return;
    }
    const GuestAddr h = m_mmio_services.alloc(layout.size);
    if (h == 0) {
        set_err(MMIOERR_OUTOFMEMORY);
        m.set_return(0);
        return;
    }
    in.err = 0;
    in.bufoff = 0;
    in.diskoff = 0;
    in.hmmio = h;
    const auto alloc_buffer = [&](i32 size) -> bool {
        const GuestAddr b = m_mmio_services.alloc(static_cast<u64>(size));
        if (b == 0) return false;
        in.buf = b;
        st.owns_buffer = true;
        return true;
    };
    bool buffer_ok = true;
    if (st.kind == MmioProcKind::Mem) {
        // The buffer is the file: a caller buffer holds cchBuffer bytes of
        // content unless MMIO_CREATE empties it (m_get0, mc_get0); NULL with
        // cchBuffer > 0 is an internal, expandable file growing by
        // adwInfo[0] (me v=0/1); NULL, 0 and MMIO_ALLOCBUF the default
        // buffer, which does not grow (me v=2/3).
        if (caller.buf != 0) {
            in.cch = caller.cch;
        } else if (caller.cch > 0) {
            buffer_ok = alloc_buffer(caller.cch);
            in.cch = caller.cch;
            flags |= MMIO_ALLOCBUF;
            st.expandable = caller.adw[0] != 0;
        } else if ((flags & MMIO_ALLOCBUF) != 0) {
            in.cch = static_cast<i32>(MMIO_DEFAULTBUFFER);
            buffer_ok = alloc_buffer(in.cch);
        } else {
            in.buf = 0;
            in.cch = caller.cch;
        }
        in.next = in.buf;
        in.endw = ptr_add(in.buf, in.cch, x64);
        in.endr = (flags & MMIO_CREATE) != 0 || in.buf == 0 ? in.buf : in.endw;
    } else {
        if (caller.buf != 0) {
            // A caller buffer is used as is; MMIO_ALLOCBUF is dropped
            // (s_user_alloc flags=0).
            in.cch = caller.cch;
            flags &= ~MMIO_ALLOCBUF;
        } else if ((flags & MMIO_ALLOCBUF) != 0) {
            in.cch = caller.cch == 0 ? static_cast<i32>(MMIO_DEFAULTBUFFER) : caller.cch;
            if (in.cch < 0) {
                in.buf = 0;  // s_get_neg: unbuffered, cchBuffer kept
                flags &= ~MMIO_ALLOCBUF;
            } else {
                buffer_ok = alloc_buffer(in.cch);
            }
        } else {
            in.buf = 0;
            in.cch = caller.cch;
        }
        in.next = in.buf;
        in.endr = in.buf;
        in.endw = ptr_add(in.buf, in.cch, x64);
    }
    in.flags = flags;
    if (!buffer_ok) {
        m_mmio_services.free(h);
        set_err(MMIOERR_OUTOFMEMORY);
        m.set_return(0);
        return;
    }

    auto [slot, inserted] = m_mmio_handles.emplace(h, st);
    (void)inserted;
    MmioOps ops{*this, h, slot->second, in};
    u32 error = 0;
    if (st.kind == MmioProcKind::Dos) {
        if (name_utf8) {
            error = MmioOps::dos_open(*this, ops.in, *name_utf8);
        } else {
            (void)MmioOps::dos_proc(*this, ops.in, MMIOM_OPEN, 0, 0);
        }
    } else if (st.kind == MmioProcKind::Custom) {
        // MMIOM_OPEN(lParam1 = the name in ANSI, lParam2 = 0) (p_log;
        // pw_openw: mmioOpenW hands an ANSI procedure ANSI).
        GuestAddr name_arg = 0;
        GuestAddr temp = 0;
        if (name_ptr != 0) {
            if (wide) {
                temp = m_mmio_services.alloc(name_acp->size() + 1);
                if (temp != 0 && m.write_ansi_string(temp, *name_acp).is_ok()) name_arg = temp;
            } else {
                name_arg = name_ptr;
            }
        }
        const i64 r = ops.proc(MMIOM_OPEN, name_arg, 0);
        if (temp != 0) m_mmio_services.free(temp);
        error = static_cast<u32>(r);
    }
    if (error != 0) {
        ops.free_buffer();
        m_mmio_handles.erase(h);
        m_mmio_services.free(h);
        set_err(error);
        winmm_diag("mmioOpen -> error %u\n", error);
        m.set_return(0);
        return;
    }
    ops.commit();
    set_err(0);
    winmm_diag("mmioOpen kind=%d flags=0x%x -> 0x%llx\n", static_cast<int>(st.kind), flags,
               static_cast<unsigned long long>(h));
    m.set_return(h);
}

// MMRESULT mmioClose(HMMIO, UINT fuClose)
void WinmmManager::impl_mmioClose(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const u32 fu = m.arg32(1);
    MmioHandle* st = find_handle(m_mmio_handles, h);
    if (st == nullptr) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    MmioOps ops{*this, h, *st, {}};
    (void)load_info(m_memory, h, st->is_64bit, ops.in);
    // A buffer that cannot be written fails the close and the file stays
    // open (ro_write_after_close: getinfo=0, close2=106).
    if (const u32 e = ops.flush(MMIOM_WRITEFLUSH); e != 0) {
        ops.commit();
        m.set_return(e);
        return;
    }
    const i64 r = ops.proc(MMIOM_CLOSE, fu, 0);
    ops.free_buffer();
    m_mmio_handles.erase(h);
    if (m_mmio_services.free) m_mmio_services.free(h);
    m.set_return(static_cast<u32>(r));
}

// LONG mmioRead(HMMIO, HPSTR pch, LONG cch)
void WinmmManager::impl_mmioRead(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const GuestAddr dst = m.arg_ptr(1);
    const i32 cch = static_cast<i32>(m.arg32(2));
    MmioHandle* st = find_handle(m_mmio_handles, h);
    // NULL buffer -1 (risky 0x15); a negative count is -1 unbuffered (0x1d)
    // and never returns buffered on Windows (0x1c): -1 here as well.
    if (st == nullptr || dst == 0 || cch < 0) {
        m.set_return(0xFFFFFFFFu);
        return;
    }
    if (cch == 0) {
        m.set_return(0);
        return;
    }
    MmioOps ops{*this, h, *st, {}};
    (void)load_info(m_memory, h, st->is_64bit, ops.in);
    const i64 r = ops.read(dst, cch);
    ops.commit();
    m.set_return(static_cast<u32>(static_cast<i32>(r)));
}

// LONG mmioWrite(HMMIO, const char* pch, LONG cch)
void WinmmManager::impl_mmioWrite(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const GuestAddr src = m.arg_ptr(1);
    const i32 cch = static_cast<i32>(m.arg32(2));
    MmioHandle* st = find_handle(m_mmio_handles, h);
    if (st == nullptr || cch < 0 || (src == 0 && cch != 0)) {  // risky 0x1e
        m.set_return(0xFFFFFFFFu);
        return;
    }
    if (cch == 0) {
        m.set_return(0);
        return;
    }
    MmioOps ops{*this, h, *st, {}};
    (void)load_info(m_memory, h, st->is_64bit, ops.in);
    const i64 r = ops.write(src, cch);
    ops.commit();
    m.set_return(static_cast<u32>(static_cast<i32>(r)));
}

// LONG mmioSeek(HMMIO, LONG lOffset, int iOrigin)
void WinmmManager::impl_mmioSeek(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const i32 offset = static_cast<i32>(m.arg32(1));
    const u32 origin = m.arg32(2);
    MmioHandle* st = find_handle(m_mmio_handles, h);
    if (st == nullptr) {
        m.set_return(0xFFFFFFFFu);
        return;
    }
    MmioOps ops{*this, h, *st, {}};
    (void)load_info(m_memory, h, st->is_64bit, ops.in);
    // SEEK_END counts back from the end: end - lOffset (r_seek_endm4 = 104
    // on a 100-byte file, se_buf_p4 = 96, mg_seek_endp = 68). Any other
    // origin is position 0 (seek_origin3_7, seek_origin_neg). Negative
    // positions are accepted on disk files (r_seek_cur_neg_big) and fail at
    // the next transfer (se_read_neg); a memory file stays in [0, end]
    // (m_seek_100, mg_seek_neg).
    const i64 current = st->kind == MmioProcKind::Mem ? rel(ops.in.next, ops.in.buf) : ops.pos();
    i64 target = 0;
    if (origin == SEEK_SET_VALUE) {
        target = offset;
    } else if (origin == SEEK_CUR_VALUE) {
        target = current + offset;
    } else if (origin == SEEK_END_VALUE) {
        const i64 end = ops.file_end();
        if (end == kProcError) {
            ops.commit();
            m.set_return(0xFFFFFFFFu);
            return;
        }
        target = end - offset;
    }
    target = static_cast<i32>(target);
    const bool ok = ops.seek_to(target);
    ops.commit();
    m.set_return(ok ? static_cast<u32>(static_cast<i32>(target)) : 0xFFFFFFFFu);
}

// MMRESULT mmioGetInfo(HMMIO, LPMMIOINFO, UINT wFlags)
void WinmmManager::impl_mmioGetInfo(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const GuestAddr info = m.arg_ptr(1);
    (void)m.arg32(2);  // reserved; a nonzero value is ignored (r_getinfo_flag1)
    MmioHandle* st = find_handle(m_mmio_handles, h);
    if (st == nullptr) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    if (info == 0) {  // risky 0x12
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    Info in{};
    (void)load_info(m_memory, h, st->is_64bit, in);
    in.hmmio = h;
    m.set_return(store_info(m_memory, info, st->is_64bit, in) ? MMSYSERR_NOERROR : MMSYSERR_INVALPARAM);
}

// MMRESULT mmioSetInfo(HMMIO, LPCMMIOINFO, UINT wFlags)
void WinmmManager::impl_mmioSetInfo(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const GuestAddr info = m.arg_ptr(1);
    (void)m.arg32(2);
    MmioHandle* st = find_handle(m_mmio_handles, h);
    if (st == nullptr) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    if (info == 0) {  // risky 0x16
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    MmioOps ops{*this, h, *st, {}};
    (void)load_info(m_memory, h, st->is_64bit, ops.in);
    Info caller{};
    if (!load_info(m_memory, info, st->is_64bit, caller)) {
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    // Learn: pchNext and MMIO_DIRTY are taken back; data written past
    // pchEndRead extends it, dirty or not (w_get1, w_nodirty1).
    if (ops.in.buf != 0) {
        ops.in.next = caller.next;
        if (rel(ops.in.next, ops.in.endr) > 0) ops.in.endr = ops.in.next;
        ops.in.flags |= caller.flags & MMIO_DIRTY;
    }
    ops.commit();
    m.set_return(MMSYSERR_NOERROR);
}

// MMRESULT mmioSetBuffer(HMMIO, LPSTR pchBuffer, LONG cchBuffer, UINT fuBuffer)
void WinmmManager::impl_mmioSetBuffer(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const GuestAddr pch = m.arg_ptr(1);
    const i32 cch = static_cast<i32>(m.arg32(2));
    (void)m.arg32(3);  // reserved; ignored (u_setbuf_null0_flag)
    MmioHandle* st = find_handle(m_mmio_handles, h);
    if (st == nullptr) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    MmioOps ops{*this, h, *st, {}};
    (void)load_info(m_memory, h, st->is_64bit, ops.in);
    if (const u32 e = ops.flush(MMIOM_WRITE); e != 0) {
        ops.commit();
        m.set_return(e);
        return;
    }
    const bool mem = st->kind == MmioProcKind::Mem;
    const i32 position = ops.pos();
    ops.free_buffer();
    u32 result = MMSYSERR_NOERROR;
    if (pch != 0) {
        ops.in.buf = pch;
        ops.in.flags &= ~MMIO_ALLOCBUF;
    } else if (cch > 0) {
        const GuestAddr b = m_mmio_services.alloc ? m_mmio_services.alloc(static_cast<u64>(cch)) : 0;
        if (b == 0) {
            ops.in.buf = 0;
            ops.in.flags &= ~MMIO_ALLOCBUF;
            result = MMIOERR_OUTOFMEMORY;
        } else {
            ops.in.buf = b;
            st->owns_buffer = true;
            ops.in.flags |= MMIO_ALLOCBUF;
        }
    } else {
        // (NULL, 0) and (NULL, negative) leave the file unbuffered with that
        // cchBuffer (u_get6, u_get7).
        ops.in.buf = 0;
        ops.in.flags &= ~MMIO_ALLOCBUF;
    }
    ops.in.cch = result == MMSYSERR_NOERROR ? cch : 0;
    ops.in.next = ops.in.buf;
    ops.in.endr = ops.in.buf;
    ops.in.endw = ptr_add(ops.in.buf, ops.in.cch, st->is_64bit);
    // The new buffer starts at the current position; a memory file's stays
    // at 0 (u_get4 bufoff=b, m_get3 bufoff=0).
    if (!mem) ops.in.bufoff = position;
    st->expandable = false;
    ops.commit();
    m.set_return(result);
}

// MMRESULT mmioFlush(HMMIO, UINT fuFlush)
void WinmmManager::impl_mmioFlush(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const u32 fu = m.arg32(1);
    MmioHandle* st = find_handle(m_mmio_handles, h);
    if (st == nullptr) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    MmioOps ops{*this, h, *st, {}};
    (void)load_info(m_memory, h, st->is_64bit, ops.in);
    // Unbuffered and memory files have nothing to flush (c_flush,
    // mg_after_flush keeps a memory file's buffer and MMIO_DIRTY).
    if (st->kind == MmioProcKind::Mem || ops.in.buf == 0) {
        m.set_return(MMSYSERR_NOERROR);
        return;
    }
    if (const u32 e = ops.flush(MMIOM_WRITEFLUSH); e != 0) {
        ops.commit();
        m.set_return(e);
        return;
    }
    if ((fu & MMIO_EMPTYBUF) != 0) ops.empty_at(ops.pos());  // r_get_after_empty
    ops.commit();
    m.set_return(MMSYSERR_NOERROR);
}

// MMRESULT mmioAdvance(HMMIO, LPMMIOINFO, UINT fuAdvance)
void WinmmManager::impl_mmioAdvance(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const GuestAddr info = m.arg_ptr(1);
    const u32 fu = m.arg32(2);
    MmioHandle* st = find_handle(m_mmio_handles, h);
    if (st == nullptr) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    MmioOps ops{*this, h, *st, {}};
    (void)load_info(m_memory, h, st->is_64bit, ops.in);
    if (ops.in.buf == 0) {  // u_adv, c_advance_r_unbuf
        m.set_return(MMIOERR_UNBUFFERED);
        return;
    }
    // The caller's pchNext and MMIO_DIRTY are taken first, as by
    // mmioSetInfo (w_adv_mid wrote up to the caller's pchNext). A NULL
    // MMIOINFO advances the internal state (risky 0x13 returns 0).
    if (info != 0) {
        Info caller{};
        if (load_info(m_memory, info, st->is_64bit, caller)) {
            ops.in.next = caller.next;
            if (rel(ops.in.next, ops.in.endr) > 0) ops.in.endr = ops.in.next;
            ops.in.flags |= caller.flags & MMIO_DIRTY;
        }
    }
    u32 error = 0;
    if (st->kind == MmioProcKind::Mem) {
        // A memory file cannot be read further (m_adv_eof, mg_adv_read) and
        // grows by adwInfo[0] when expandable (mg_adv_76, mg_adv_full), else
        // MMIOERR_CANNOTEXPAND (mw_adv_w, me v=2).
        if ((fu & MMIO_WRITE) == 0) {
            error = MMIOERR_CANNOTREAD;
        } else if (!st->expandable ||
                   !ops.grow_mem(static_cast<i32>(std::min<i64>(
                       static_cast<i64>(ops.in.cch) + ops.in.adw[0], 0x7FFFFFFF)))) {
            error = MMIOERR_CANNOTEXPAND;
        }
    } else if ((fu & MMIO_WRITE) != 0) {
        // Write a dirty buffer, then start an empty one at the current
        // position (aw_after, am_adv_w_clean; ro_adv_write: a clean buffer
        // on a read-only file succeeds).
        error = ops.flush(MMIOM_WRITE);
        if (error == 0) ops.empty_at(ops.pos());
    } else {
        // At the end of the file the buffer comes back empty with
        // pchNext == pchEndRead and MMSYSERR_NOERROR (r_adv_eof).
        error = ops.fill();
    }
    ops.commit();
    if (error == 0 && info != 0) {
        Info out = ops.in;
        out.hmmio = h;
        (void)store_info(m_memory, info, st->is_64bit, out);
    }
    m.set_return(error);
}

// LRESULT mmioSendMessage(HMMIO, UINT uMsg, LPARAM lParam1, LPARAM lParam2)
void WinmmManager::impl_mmioSendMessage(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const u32 msg = m.arg32(1);
    const bool x64 = m.cpu_is_64bit();
    const u64 l1 = x64 ? m.arg(2) : static_cast<u64>(static_cast<i64>(static_cast<i32>(m.arg32(2))));
    const u64 l2 = x64 ? m.arg(3) : static_cast<u64>(static_cast<i64>(static_cast<i32>(m.arg32(3))));
    MmioHandle* st = find_handle(m_mmio_handles, h);
    if (st == nullptr) {  // risky 0xc: 0
        m.set_return(0);
        return;
    }
    // Every message goes to the I/O procedure, the MMIOM_* ones included
    // (o_send_seek moves the DOS file pointer, not the mmio position).
    MmioOps ops{*this, h, *st, {}};
    (void)load_info(m_memory, h, st->is_64bit, ops.in);
    const i64 r = ops.proc(msg, l1, l2);
    ops.commit();
    m.set_return(static_cast<u64>(r));
}

// MMRESULT mmioDescend(HMMIO, LPMMCKINFO, const MMCKINFO* pmmckiParent, UINT fuDescend)
void WinmmManager::impl_mmioDescend(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const GuestAddr ck = m.arg_ptr(1);
    const GuestAddr parent = m.arg_ptr(2);
    const u32 fu = m.arg32(3);
    MmioHandle* st = find_handle(m_mmio_handles, h);
    // Windows faults on a bad HMMIO (risky 9); the handle error is reported.
    if (st == nullptr) {
        m.set_return(MMSYSERR_INVALHANDLE);
        return;
    }
    if (ck == 0) {  // risky 0x14
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    const MmckInfoLayout& ckl = mmckinfo_layout;
    MmioOps ops{*this, h, *st, {}};
    (void)load_info(m_memory, h, st->is_64bit, ops.in);
    const auto r32 = [&](GuestAddr a) { return m.read_u32(a).value_or(0); };
    // What to look for (Learn mmioDescend). A zero id or form type matches
    // anything (d_findchunk_id0, d_findriff_type0); flags are tested
    // FINDCHUNK, FINDRIFF, FINDLIST in that order (d_bad_flags 0x70 =
    // FINDCHUNK with ckid 0; d_flag_riff_list 0x60 = FINDRIFF).
    const u32 want_id = r32(ck + ckl.off_ckid);
    const u32 want_type = r32(ck + ckl.off_fccType);
    u32 search_id = 0;
    u32 search_type = 0;
    if ((fu & MMIO_FINDCHUNK) != 0) {
        search_id = want_id;
    } else if ((fu & MMIO_FINDRIFF) != 0) {
        search_id = FOURCC_RIFF;
        search_type = want_type;
    } else if ((fu & MMIO_FINDLIST) != 0) {
        search_id = FOURCC_LIST;
        search_type = want_type;
    }
    i64 parent_data = 0;
    i64 parent_end = 0;
    if (parent != 0) {
        parent_data = r32(parent + ckl.off_dwDataOffset);
        parent_end = parent_data + r32(parent + ckl.off_cksize);
    }
    // A chunk running past the end of the file is cut to it (d_partial,
    // d_odd_skip, cb_riff_no_type size=0).
    const i64 file_end = ops.file_end();
    u32 result = MMIOERR_CHUNKNOTFOUND;
    for (;;) {
        const i64 header = st->kind == MmioProcKind::Mem ? rel(ops.in.next, ops.in.buf) : ops.pos();
        // The header is read straight into the caller's MMCKINFO; a short
        // read leaves what it got there (cb_eof_header id=0).
        if (ops.read(ck, 8) != 8) break;
        const u32 id = r32(ck + ckl.off_ckid);
        i64 size = r32(ck + ckl.off_cksize);
        const i64 data = header + 8;
        (void)m.write_u32(ck + ckl.off_dwDataOffset, static_cast<u32>(data));
        if (file_end != kProcError && data + size > file_end) {
            size = file_end > data ? file_end - data : 0;
            (void)m.write_u32(ck + ckl.off_cksize, static_cast<u32>(size));
        }
        // A chunk that starts before the parent's data is outside it
        // (d_parent_after_pos), checked before the form type is read.
        if (parent != 0 && header < parent_data) break;
        const bool container = id == FOURCC_RIFF || id == FOURCC_LIST;
        // The form type is read only for a chunk whose id is the one looked
        // for (d_fmt_after_data and d_flag_riff_list keep fccType 0 past a
        // LIST; d_wrong_riff shows the RIFF's WAVE).
        const bool id_match = (fu & MMIO_FINDCHUNK) != 0
            ? search_id == 0 || id == search_id
            : (fu & (MMIO_FINDRIFF | MMIO_FINDLIST)) != 0 ? id == search_id : true;
        u32 type = 0;
        if (id_match && container) {
            if (ops.read(ck + ckl.off_fccType, 4) != 4) break;
            type = r32(ck + ckl.off_fccType);
        }
        const bool match = id_match &&
            ((fu & MMIO_FINDCHUNK) != 0 || (fu & (MMIO_FINDRIFF | MMIO_FINDLIST)) == 0 ||
             search_type == 0 || type == search_type);
        if (match) {
            if (!container) (void)m.write_u32(ck + ckl.off_fccType, 0);
            (void)m.write_u32(ck + ckl.off_dwFlags, 0);
            result = MMSYSERR_NOERROR;
            break;
        }
        // A non-matching chunk that ends past the parent ends the search,
        // positioned after its header (d_missing_in_list pos=0x52).
        if (parent != 0 && data + size > parent_end) break;
        if (!ops.seek_to(data + size + (size & 1))) break;
    }
    ops.commit();
    m.set_return(result);
}

// MMRESULT mmioAscend(HMMIO, LPMMCKINFO, UINT fuAscend)
void WinmmManager::impl_mmioAscend(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const GuestAddr ck = m.arg_ptr(1);
    const u32 fu = m.arg32(2);
    MmioHandle* st = find_handle(m_mmio_handles, h);
    if (st == nullptr) {  // risky 0xa
        m.set_return(MMIOERR_CANNOTSEEK);
        return;
    }
    if (ck == 0) {  // risky 0x17
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    if (fu != 0) {  // k_ascend_flag
        m.set_return(MMSYSERR_INVALFLAG);
        return;
    }
    const MmckInfoLayout& ckl = mmckinfo_layout;
    MmioOps ops{*this, h, *st, {}};
    (void)load_info(m_memory, h, st->is_64bit, ops.in);
    const i64 data = m.read_u32(ck + ckl.off_dwDataOffset).value_or(0);
    i64 size = m.read_u32(ck + ckl.off_cksize).value_or(0);
    const u32 flags = m.read_u32(ck + ckl.off_dwFlags).value_or(0);
    u32 result = MMSYSERR_NOERROR;
    if ((flags & MMIO_DIRTY) != 0) {
        // A chunk written here: its size is the distance from its data to
        // the current position, patched into the header, and an odd size
        // gets a zero pad byte (k_nam_ascend, pad_file; pad_clean_file: a
        // clean chunk gets none). dwFlags is left as it is.
        const i64 current = st->kind == MmioProcKind::Mem ? rel(ops.in.next, ops.in.buf) : ops.pos();
        size = current - data;
        (void)m.write_u32(ck + ckl.off_cksize, static_cast<u32>(size));
        const u32 le = static_cast<u32>(size);
        const u8 bytes[4] = {static_cast<u8>(le), static_cast<u8>(le >> 8), static_cast<u8>(le >> 16),
                             static_cast<u8>(le >> 24)};
        if (!ops.seek_to(data - 4)) {
            result = MMIOERR_CANNOTSEEK;
        } else if (ops.write_host(bytes, 4) != 4) {
            result = MMIOERR_CANNOTWRITE;
        } else if (!ops.seek_to(current)) {
            result = MMIOERR_CANNOTSEEK;
        } else if ((size & 1) != 0) {
            const u8 pad = 0;
            if (ops.write_host(&pad, 1) != 1) result = MMIOERR_CANNOTWRITE;
        }
    }
    if (result == MMSYSERR_NOERROR && !ops.seek_to(data + size + (size & 1))) result = MMIOERR_CANNOTSEEK;
    ops.commit();
    m.set_return(result);
}

// MMRESULT mmioCreateChunk(HMMIO, LPMMCKINFO, UINT fuCreate)
void WinmmManager::impl_mmioCreateChunk(Marshaler& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const GuestAddr h = m.arg_ptr(0);
    const GuestAddr ck = m.arg_ptr(1);
    const u32 fu = m.arg32(2);
    MmioHandle* st = find_handle(m_mmio_handles, h);
    if (st == nullptr) {  // risky 0xb
        m.set_return(MMIOERR_CANNOTSEEK);
        return;
    }
    if (ck == 0) {  // risky 0x18
        m.set_return(MMSYSERR_INVALPARAM);
        return;
    }
    if ((fu & ~(MMIO_CREATERIFF | MMIO_CREATELIST)) != 0) {  // k_flag_bad
        m.set_return(MMSYSERR_INVALFLAG);
        return;
    }
    const MmckInfoLayout& ckl = mmckinfo_layout;
    MmioOps ops{*this, h, *st, {}};
    (void)load_info(m_memory, h, st->is_64bit, ops.in);
    u32 id = m.read_u32(ck + ckl.off_ckid).value_or(0);
    const u32 size = m.read_u32(ck + ckl.off_cksize).value_or(0);
    const u32 type = m.read_u32(ck + ckl.off_fccType).value_or(0);
    // MMIO_CREATERIFF wins over MMIO_CREATELIST (cc_both).
    if ((fu & MMIO_CREATERIFF) != 0) id = FOURCC_RIFF;
    else if ((fu & MMIO_CREATELIST) != 0) id = FOURCC_LIST;
    (void)m.write_u32(ck + ckl.off_ckid, id);
    // dwDataOffset follows the 8-byte header at the current position, with
    // no alignment (cc_odd_pos data=9), and is set even when the header
    // cannot be written (k_ro_create data=8).
    const i64 current = st->kind == MmioProcKind::Mem ? rel(ops.in.next, ops.in.buf) : ops.pos();
    (void)m.write_u32(ck + ckl.off_dwDataOffset, static_cast<u32>(current + 8));
    u8 header[12];
    std::memcpy(header, &id, 4);
    std::memcpy(header + 4, &size, 4);
    std::memcpy(header + 8, &type, 4);
    const i32 len = fu != 0 ? 12 : 8;
    if (ops.write_host(header, len) != len) {
        ops.commit();
        m.set_return(MMIOERR_CANNOTWRITE);
        return;
    }
    (void)m.write_u32(ck + ckl.off_dwFlags, MMIO_DIRTY);
    ops.commit();
    m.set_return(MMSYSERR_NOERROR);
}

// FOURCC mmioStringToFOURCCA/W(LPCSTR/LPCWSTR sz, UINT uFlags)
void WinmmManager::impl_mmioStringToFOURCC(Marshaler& m, bool wide) {
    const GuestAddr sz = m.arg_ptr(0);
    const u32 flags = m.arg32(1);
    if (sz == 0) {  // risky 0x19
        m.set_return(0);
        return;
    }
    std::string acp;
    if (wide) {
        // The W form converts to the ANSI code page first ('?' for what it
        // cannot hold: fourcc_w_hi Zhe -> 0x3F).
        auto w = m.read_wide_string(sz, 4);
        if (w.is_ok()) acp = utf8_to_acp_bytes(w.value());
    } else {
        auto a = m.read_ansi_string(sz, 4);
        if (a.is_ok()) acp = a.value();
    }
    m.set_return(fourcc_from_acp(acp, flags));
}

// LPMMIOPROC mmioInstallIOProcA/W(FOURCC, LPMMIOPROC, DWORD dwFlags)
void WinmmManager::impl_mmioInstallIOProc(Marshaler& m, bool /*wide*/) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const u32 fcc = m.arg32(0);
    const GuestAddr proc = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    // Measured table (probe it1..it14): MMIO_INSTALLPROC installs a non-NULL
    // procedure for a nonzero FOURCC, newest first, whatever else is set,
    // except that MMIO_UNICODEPROC is refused (it10). REMOVE and FIND take
    // pIOProc NULL, as Learn says, and act on the newest entry; a non-NULL
    // pIOProc gets NULL (it1 findp/rm). FIND falls back to the built-in DOS
    // and MEM procedures, which cannot be removed (it14, p_remove_dos).
    GuestAddr result = 0;
    if ((flags & MMIO_INSTALLPROC) != 0) {
        if (proc != 0 && fcc != 0 && (flags & MMIO_UNICODEPROC) == 0) {
            m_mmio_procs.push_back({fcc, proc});
            result = proc;
        }
    } else if (proc == 0 && fcc != 0) {
        auto newest = std::find_if(m_mmio_procs.rbegin(), m_mmio_procs.rend(),
                                   [fcc](const MmioIoProcEntry& e) { return e.fcc == fcc; });
        if ((flags & MMIO_REMOVEPROC) != 0) {
            if (newest != m_mmio_procs.rend()) {
                result = newest->proc;
                m_mmio_procs.erase(std::next(newest).base());
            }
        } else if ((flags & MMIO_FINDPROC) != 0) {
            if (newest != m_mmio_procs.rend()) result = newest->proc;
            else if (fcc == FOURCC_DOS) result = m_mmio_dos_proc;
            else if (fcc == FOURCC_MEM) result = m_mmio_mem_proc;
        }
    }
    m.set_return(result);
}

// MMRESULT mmioRenameA/W(LPCSTR/LPCWSTR old, LPCSTR/LPCWSTR new, LPCMMIOINFO, DWORD)
void WinmmManager::impl_mmioRename(Marshaler& m, bool wide) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const bool x64 = m.cpu_is_64bit();
    const GuestAddr from_ptr = m.arg_ptr(0);
    const GuestAddr to_ptr = m.arg_ptr(1);
    const GuestAddr info_ptr = m.arg_ptr(2);
    (void)m.arg32(3);  // ignored (o_rename_flag)
    const auto read_name = [&](GuestAddr p) -> std::optional<std::string> {
        if (p == 0) return std::nullopt;
        if (wide) {
            auto w = m.read_wide_string(p, 32768);
            return w.is_ok() ? std::optional<std::string>(w.value()) : std::nullopt;
        }
        auto a = m.read_ansi_string(p, 32768);
        return a.is_ok() ? std::optional<std::string>(acp_to_utf8(a.value())) : std::nullopt;
    };
    const auto from = read_name(from_ptr);
    const auto to = read_name(to_ptr);
    Info caller{};
    if (info_ptr != 0) (void)load_info(m_memory, info_ptr, x64, caller);
    GuestAddr proc = static_cast<GuestAddr>(caller.proc);
    if (proc == 0 && caller.fcc != 0 && caller.fcc != FOURCC_DOS) {
        for (auto it = m_mmio_procs.rbegin(); it != m_mmio_procs.rend(); ++it) {
            if (it->fcc == caller.fcc) {
                proc = it->proc;
                break;
            }
        }
        if (proc == 0) {
            m.set_return(MMIOERR_FILENOTFOUND);
            return;
        }
    }
    if (proc == 0 || proc == m_mmio_dos_proc) {
        m.set_return(from && to ? MmioOps::dos_rename(*this, *from, *to) : MMIOERR_FILENOTFOUND);
        return;
    }
    // A custom procedure gets MMIOM_RENAME(old, new) with a scratch
    // MMIOINFO that is no open file's (p_rename_log info_is_h=0), the names
    // in ANSI.
    if (!m_mmio_services.alloc || !m_mmio_services.free) {
        WINMM_UNIMPLEMENTED("mmioRename: no process-heap service");
        m.set_return(MMIOERR_OUTOFMEMORY);
        return;
    }
    const MmioInfoLayout& layout = mmioinfo_layout(x64);
    const GuestAddr scratch = m_mmio_services.alloc(layout.size);
    GuestAddr a_from = 0, a_to = 0;
    if (wide) {
        if (from) {
            const std::string s = utf8_to_acp_bytes(*from);
            a_from = m_mmio_services.alloc(s.size() + 1);
            if (a_from != 0) (void)m.write_ansi_string(a_from, s);
        }
        if (to) {
            const std::string s = utf8_to_acp_bytes(*to);
            a_to = m_mmio_services.alloc(s.size() + 1);
            if (a_to != 0) (void)m.write_ansi_string(a_to, s);
        }
    } else {
        a_from = from_ptr;
        a_to = to_ptr;
    }
    u64 result = MMIOERR_OUTOFMEMORY;
    if (scratch != 0) {
        Info tmp = caller;
        tmp.proc = proc;
        tmp.hmmio = 0;
        (void)store_info(m_memory, scratch, x64, tmp);
        result = static_cast<u64>(MmioOps::guest_call(*this, proc, scratch, MMIOM_RENAME, a_from, a_to, x64));
        m_mmio_services.free(scratch);
    }
    if (wide) {
        if (a_from != 0) m_mmio_services.free(a_from);
        if (a_to != 0) m_mmio_services.free(a_to);
    }
    m.set_return(static_cast<u32>(result));
}

// The built-in procedures as MMIOPROCs: LRESULT (LPSTR lpmmioinfo, UINT
// uMsg, LPARAM lParam1, LPARAM lParam2), on whatever MMIOINFO the caller
// passes (an open file's, or a custom procedure's own that chains to DOS).
void WinmmManager::impl_mmioBuiltinIOProc(Marshaler& m, bool mem) {
    std::lock_guard<std::recursive_mutex> lock(m_mmio_mutex);
    const bool x64 = m.cpu_is_64bit();
    const GuestAddr info = m.arg_ptr(0);
    const u32 msg = m.arg32(1);
    const u64 l1 = x64 ? m.arg(2) : static_cast<u64>(static_cast<i64>(static_cast<i32>(m.arg32(2))));
    const u64 l2 = x64 ? m.arg(3) : static_cast<u64>(static_cast<i64>(static_cast<i32>(m.arg32(3))));
    Info in{};
    if (info == 0 || !load_info(m_memory, info, x64, in)) {
        m.set_return(static_cast<u64>(kProcError));
        return;
    }
    const i64 r = mem ? MmioOps::mem_proc(msg) : MmioOps::dos_proc(*this, in, msg, l1, l2);
    (void)store_info(m_memory, info, x64, in);
    m.set_return(static_cast<u64>(r));
}

} // namespace vitreo::winapi
