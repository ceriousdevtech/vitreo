// SPDX-License-Identifier: LGPL-2.1-only

/// @file copy_file_engine.cpp
/// @brief One copy engine for CopyFileA/W, CopyFileExA/W and CopyFile2.
///
/// The three entry points differ only in how they report progress and how
/// they spell the result (BOOL + last error vs HRESULT). Everything Learn
/// leaves open — callback order, chunking, what an aborted copy leaves on
/// disk, which dwCopyFlags bits each entry point rejects — was measured by
/// tools/oracle_probe/s4_copyfile_probe.c on Windows 11 (x86 and x64 give
/// identical output; s4_copyfile_probe.expected).
///
/// Learn: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-copyfile2
///        https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-copyfileexw
///        https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-copyfile2_message

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"

#include <atomic>
#include <cstdio>
#include <span>
#include <vector>

namespace vitreo::winapi {

namespace {

// winbase.h dwCopyFlags.
constexpr u32 kCopyFileFailIfExists = 0x00000001;
constexpr u32 kCopyFileDirectory = 0x00000080;
constexpr u32 kCopyFileDisablePreAllocation = 0x04000000;

// CopyFileEx rejects every bit outside this mask with ERROR_INVALID_PARAMETER
// before touching either file (s4_copyfile_probe cfx_flag sweep): FAIL_IF_EXISTS,
// RESTARTABLE, OPEN_SOURCE_FOR_WRITE, ALLOW_DECRYPTED_DESTINATION, DIRECTORY,
// COPY_SYMLINK, NO_BUFFERING, REQUEST_SECURITY_PRIVILEGES,
// SKIP_ALTERNATE_STREAMS, NO_OFFLOAD, OPEN_AND_COPY_REPARSE_POINT,
// IGNORE_EDP_BLOCK, IGNORE_SOURCE_ENCRYPTION, DONT_REQUEST_DEST_WRITE_DAC,
// DISABLE_PRE_ALLOCATION, REQUEST_COMPRESSED_TRAFFIC. (RESUME_FROM_PAUSE
// alone is rejected too.)
constexpr u32 kCopyFileExValidFlags = 0x16E4B88Fu;
// CopyFile2 accepts every single bit except 0x40000000, which is
// E_INVALIDARG with the last error untouched (cf2_flag sweep). Bit 0x00100000
// (undocumented) completes with S_OK and a zero-length destination.
constexpr u32 kCopyFile2RejectedFlags = 0x40000000u;
constexpr u32 kCopyFile2NoDataFlag = 0x00100000u;

// Both engines move 1 MiB per chunk; CopyFile2 keeps up to three chunks in
// flight, so CHUNK_STARTED runs ahead of CHUNK_FINISHED (cf2_seq_big:
// "3111212224", cf2_seq_huge).
constexpr u64 kChunkBytes = 1024 * 1024;
constexpr u64 kCopyFile2Window = 3;

// COPYFILE2_MESSAGE_TYPE / _ACTION / _COPY_PHASE (winbase.h).
constexpr u32 kMsgChunkStarted = 1, kMsgChunkFinished = 2;
constexpr u32 kMsgStreamStarted = 3, kMsgStreamFinished = 4, kMsgError = 6;
constexpr u32 kActContinue = 0, kActCancel = 1, kActStop = 2, kActQuiet = 3,
              kActPause = 4;
constexpr u32 kPhasePrepareSource = 1, kPhasePrepareDest = 2;
// LPPROGRESS_ROUTINE reasons and results.
constexpr u32 kReasonChunkFinished = 0, kReasonStreamSwitch = 1;

constexpr u32 kGenericRead = 0x80000000u, kGenericWrite = 0x40000000u;
constexpr u32 kShareRead = 0x1;
constexpr u32 kCreateNew = 1, kCreateAlways = 2, kOpenExisting = 3;
constexpr u32 kAttrReadOnly = 0x1, kAttrHidden = 0x2, kAttrSystem = 0x4,
              kAttrArchive = 0x20;

/// What an aborted copy leaves behind (s4_copyfile_probe *_at_*):
/// CANCEL always deletes the destination; STOP keeps it when it comes from a
/// stream-level message and deletes it from a chunk-level one.
enum class Leftover { Delete, Keep };

void log_pause_once() {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED kernel32!CopyFile2: COPYFILE2_PROGRESS_PAUSE "
            "restart state (the partial destination is kept, but no restart "
            "metadata is written, so COPY_FILE_RESUME_FROM_PAUSE recopies)\n");
    }
}

} // namespace

/// Guest-visible state of one copy.
struct Kernel32ExtManager::CopyFileRun {
    Marshaler& m;
    const CopyFileRequest& req;
    bool is_64bit = false;
    Handle src = INVALID_HANDLE;
    Handle dst = INVALID_HANDLE;
    u64 size = 0;
    u64 done = 0;
    bool quiet = false;           // callback returned QUIET
    GuestAddr page = 0;           // COPYFILE2_MESSAGE scratch
    bool page_from_vmm = false;
};

GuestAddr Kernel32ExtManager::copy_file_scratch_alloc(Marshaler& m, bool& from_vmm) {
    from_vmm = false;
    if (m_virtual_memory_manager != nullptr) {
        const u64 a = m_virtual_memory_manager->virtual_alloc(
            0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (a != 0) {
            from_vmm = true;
            return static_cast<GuestAddr>(a);
        }
    }
    constexpr GuestAddr kMin = 0x5A400000;
    const GuestAddr base = m.memory().find_free_gap(kMin, 0x1000);
    if (base == 0 ||
        m.memory().map_region(base, 0x1000, MemoryProtection::ReadWrite).is_err()) {
        return 0;
    }
    return base;
}

void Kernel32ExtManager::copy_file_scratch_free(Marshaler& m, GuestAddr page,
                                                bool from_vmm) {
    if (page == 0) return;
    if (from_vmm && m_virtual_memory_manager != nullptr) {
        (void)m_virtual_memory_manager->virtual_free(page, 0, MEM_RELEASE);
    } else {
        (void)m.memory().unmap_region(page, 0x1000);
    }
}

namespace {

/// Build one COPYFILE2_MESSAGE at @p at. Offsets follow winbase.h: Type and
/// dwPadding, then the Info union at +8. Handles are pointer-sized, and every
/// ULARGE_INTEGER is 8-aligned, so on x86 the 64-bit fields start at +24
/// instead of +32 (the probe's layout dump lists each offset for both).
struct Cf2Msg {
    u32 type = 0;
    u32 stream = 0;
    u32 dword2 = 0;               // ChunkFinished.dwFlags / Error.hrFailure
    u64 chunk = 0, chunk_size = 0, stream_size = 0, stream_done = 0;
    u64 total = 0, total_done = 0;
    u32 phase = 0;
};

void write_cf2_message(Marshaler& m, GuestAddr at, bool x64, Handle hs, Handle hd,
                       const Cf2Msg& g) {
    auto& mem = m.memory();
    for (u32 i = 0; i < 0x60; i += 4) (void)mem.write_u32(at + i, 0);
    (void)mem.write_u32(at + 0, g.type);
    const GuestAddr info = at + 8;
    auto put_handles = [&](GuestAddr p) {
        if (x64) {
            (void)mem.write_u64(p, static_cast<u64>(hs));
            (void)mem.write_u64(p + 8, static_cast<u64>(hd));
        } else {
            (void)mem.write_u32(p, static_cast<u32>(hs));
            (void)mem.write_u32(p + 4, static_cast<u32>(hd));
        }
    };
    // First 8-aligned ULARGE_INTEGER after the two handles.
    const GuestAddr q = info + (x64 ? 24 : 16);
    switch (g.type) {
    case kMsgChunkStarted:
        (void)mem.write_u32(info, g.stream);
        put_handles(info + 8);
        (void)mem.write_u64(q + 0, g.chunk);
        (void)mem.write_u64(q + 8, g.chunk_size);
        (void)mem.write_u64(q + 16, g.stream_size);
        (void)mem.write_u64(q + 24, g.total);
        break;
    case kMsgChunkFinished:
        (void)mem.write_u32(info, g.stream);
        (void)mem.write_u32(info + 4, g.dword2);
        put_handles(info + 8);
        (void)mem.write_u64(q + 0, g.chunk);
        (void)mem.write_u64(q + 8, g.chunk_size);
        (void)mem.write_u64(q + 16, g.stream_size);
        (void)mem.write_u64(q + 24, g.stream_done);
        (void)mem.write_u64(q + 32, g.total);
        (void)mem.write_u64(q + 40, g.total_done);
        break;
    case kMsgStreamStarted:
        (void)mem.write_u32(info, g.stream);
        put_handles(info + 8);
        (void)mem.write_u64(q + 0, g.stream_size);
        (void)mem.write_u64(q + 8, g.total);
        break;
    case kMsgStreamFinished:
        (void)mem.write_u32(info, g.stream);
        put_handles(info + 8);
        (void)mem.write_u64(q + 0, g.stream_size);
        (void)mem.write_u64(q + 8, g.stream_done);
        (void)mem.write_u64(q + 16, g.total);
        (void)mem.write_u64(q + 24, g.total_done);
        break;
    case kMsgError:
        // No pointers: identical on both bitnesses.
        (void)mem.write_u32(info + 0, g.phase);
        (void)mem.write_u32(info + 4, g.stream);
        (void)mem.write_u32(info + 8, g.dword2);
        (void)mem.write_u64(info + 16, g.chunk);
        (void)mem.write_u64(info + 24, g.stream_size);
        (void)mem.write_u64(info + 32, g.stream_done);
        (void)mem.write_u64(info + 40, g.total);
        (void)mem.write_u64(info + 48, g.total_done);
        break;
    default:
        break;
    }
}

constexpr u32 hresult_from_win32(u32 e) {
    return e == 0 ? 0u : (0x80070000u | (e & 0xFFFFu));
}

} // namespace

u32 Kernel32ExtManager::copy_file_callback(CopyFileRun& run, u32 kind,
                                           u64 chunk, u64 chunk_size) {
    const CopyFileRequest& r = run.req;
    if (r.routine == 0 || run.quiet || !m_guest_call) return kActContinue;
    if (r.api == CopyFileApi::CopyFile2) {
        if (run.page == 0) {
            run.page = copy_file_scratch_alloc(run.m, run.page_from_vmm);
            if (run.page == 0) return kActContinue;
        }
        Cf2Msg g;
        g.type = kind;
        g.stream = 1;
        g.chunk = chunk;
        g.chunk_size = chunk_size;
        g.stream_size = run.size;
        g.stream_done = run.done;
        g.total = run.size;
        g.total_done = run.done;
        write_cf2_message(run.m, run.page, run.is_64bit, run.src, run.dst, g);
        const u64 args[2] = {run.page, r.context};
        const u32 act = static_cast<u32>(
            m_guest_call(r.routine, args, 2, run.is_64bit));
        if (act == kActQuiet) run.quiet = true;
        return act;
    }
    // LPPROGRESS_ROUTINE(TotalFileSize, TotalBytesTransferred, StreamSize,
    //   StreamBytesTransferred, dwStreamNumber, dwCallbackReason,
    //   hSourceFile, hDestinationFile, lpData). The four LARGE_INTEGERs are
    //   by value: one register/slot each on x64, two stdcall slots on x86.
    const u32 reason = kind;
    const u64 hs = static_cast<u64>(run.src), hd = static_cast<u64>(run.dst);
    u32 act = kActContinue;
    if (run.is_64bit) {
        const u64 args[9] = {run.size, run.done, run.size, run.done, 1,
                             reason, hs, hd, r.context};
        act = static_cast<u32>(m_guest_call(r.routine, args, 9, true));
    } else {
        const u64 args[13] = {
            run.size & 0xFFFFFFFFu, run.size >> 32,
            run.done & 0xFFFFFFFFu, run.done >> 32,
            run.size & 0xFFFFFFFFu, run.size >> 32,
            run.done & 0xFFFFFFFFu, run.done >> 32,
            1, reason, hs & 0xFFFFFFFFu, hd & 0xFFFFFFFFu,
            r.context & 0xFFFFFFFFu};
        act = static_cast<u32>(m_guest_call(r.routine, args, 13, false));
    }
    if (act == kActQuiet) run.quiet = true;
    return act;
}

u32 Kernel32ExtManager::run_copy_file(Marshaler& m, const CopyFileRequest& r) {
    auto& mem = m.memory();
    auto cancelled = [&]() {
        return r.cancel != 0 && mem.read_u32(r.cancel).value_or(0) != 0;
    };
    if (m_file_manager == nullptr) {
        return error_codes::ERROR_NOT_SUPPORTED;
    }

    CopyFileRun run{m, r};
    run.is_64bit = m.cpu_is_64bit();

    // pbCancel/pfCancel already TRUE: nothing is opened, no callback runs
    // (cf2_pfcancel_true / cfx_pbcancel_true).
    if (cancelled()) return error_codes::ERROR_REQUEST_ABORTED;

    auto fail_prepare = [&](u32 phase, u32 err) {
        if (r.api == CopyFileApi::CopyFile2 && r.routine != 0 && m_guest_call) {
            // cf2_seq_missing_src / _fail_if_exists: one ERROR message whose
            // hrFailure is the failure; the callback's answer changes nothing.
            if (run.page == 0) run.page = copy_file_scratch_alloc(m, run.page_from_vmm);
            if (run.page != 0) {
                Cf2Msg g;
                g.type = kMsgError;
                g.phase = phase;
                g.stream = phase == kPhasePrepareSource ? 0 : 1;
                g.dword2 = hresult_from_win32(err);
                g.stream_size = phase == kPhasePrepareSource ? 0 : run.size;
                g.total = g.stream_size;
                write_cf2_message(m, run.page, run.is_64bit, run.src, run.dst, g);
                const u64 args[2] = {run.page, r.context};
                (void)m_guest_call(r.routine, args, 2, run.is_64bit);
            }
        }
        return err;
    };
    auto cleanup = [&](bool delete_dst) {
        if (run.src != INVALID_HANDLE) (void)m_file_manager->close_file(run.src);
        if (run.dst != INVALID_HANDLE) (void)m_file_manager->close_file(run.dst);
        if (delete_dst) (void)m_vfs.remove(r.dst);
        copy_file_scratch_free(m, run.page, run.page_from_vmm);
        run.page = 0;
    };

    // COPY_FILE_DIRECTORY with a directory source copies the directory itself,
    // never its contents; an existing destination directory is fine
    // (cfx_directory_flag_dir_src / _existing_dst / _nonempty_src).
    if ((r.flags & kCopyFileDirectory) != 0 && r.api != CopyFileApi::CopyFile) {
        auto st = m_vfs.stat(r.src);
        if (st.is_ok() && capsule::has_attribute(st.value().attributes,
                                                 capsule::FileAttributes::Directory)) {
            auto dst_st = m_vfs.stat(r.dst);
            if (dst_st.is_ok()) {
                if (capsule::has_attribute(dst_st.value().attributes,
                                           capsule::FileAttributes::Directory)) {
                    return 0;
                }
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true)) {
                    std::fprintf(stderr,
                        "[vitreo] UNIMPLEMENTED kernel32!CopyFileEx: "
                        "COPY_FILE_DIRECTORY onto an existing file\n");
                }
                return error_codes::ERROR_NOT_SUPPORTED;
            }
            auto made = m_vfs.create_dir(r.dst);
            if (made.is_err()) return vfs_error_to_win32(made.error());
            (void)m_vfs.set_attributes(
                r.dst, static_cast<u32>(st.value().attributes));
            return 0;
        }
    }

    // --- prepare source -------------------------------------------------------
    run.src = m_file_manager->create_file(r.src, kGenericRead, kShareRead,
                                          kOpenExisting, 0);
    if (run.src == INVALID_HANDLE) {
        const u32 err = get_last_error();
        cleanup(false);
        return fail_prepare(kPhasePrepareSource, err);
    }
    const auto src_info = m_vfs.stat(r.src);
    const u32 src_attrs =
        src_info.is_ok() ? static_cast<u32>(src_info.value().attributes) : 0u;
    run.size = m_file_manager->get_file_size64(run.src).value_or(0);

    // --- prepare destination --------------------------------------------------
    // CREATE_ALWAYS with the source's HIDDEN/SYSTEM bits: an existing
    // destination that has either bit the source lacks is ERROR_ACCESS_DENIED
    // (cfw_dst_hidden / _system / cfw_hidden_over_hidden), as is a READONLY
    // one (cfw_ro_over_ro). FAIL_IF_EXISTS is CREATE_NEW → ERROR_FILE_EXISTS.
    const bool fail_if_exists = (r.flags & kCopyFileFailIfExists) != 0;
    if (!fail_if_exists) {
        if (auto dst_info = m_vfs.stat(r.dst); dst_info.is_ok() &&
            (static_cast<u32>(dst_info.value().attributes) & kAttrReadOnly) != 0 &&
            !capsule::has_attribute(dst_info.value().attributes,
                                    capsule::FileAttributes::Directory)) {
            cleanup(false);
            return fail_prepare(kPhasePrepareDest, error_codes::ERROR_ACCESS_DENIED);
        }
    }
    run.dst = m_file_manager->create_file(
        r.dst, kGenericWrite, 0, fail_if_exists ? kCreateNew : kCreateAlways,
        src_attrs & (kAttrHidden | kAttrSystem));
    if (run.dst == INVALID_HANDLE) {
        const u32 err = get_last_error();
        cleanup(false);
        return fail_prepare(kPhasePrepareDest, err);
    }
    const auto vsrc = m_file_manager->vfs_handle_for(run.src);
    const auto vdst = m_file_manager->vfs_handle_for(run.dst);
    if (vsrc == capsule::INVALID_FILE_HANDLE || vdst == capsule::INVALID_FILE_HANDLE) {
        cleanup(true);
        return error_codes::ERROR_INVALID_HANDLE;
    }
    const bool copy_data =
        !(r.api == CopyFileApi::CopyFile2 && (r.flags & kCopyFile2NoDataFlag) != 0);
    // The destination is extended to its final size before the first
    // callback: STOP at STREAM_STARTED leaves a full-size file behind
    // (cf2_stop_at_stream_started dst=300005, cfx_stop_at_stream).
    if (copy_data && (r.flags & kCopyFileDisablePreAllocation) == 0 && run.size != 0) {
        (void)m_vfs.truncate(vdst, run.size);
    }

    auto finish_abort = [&](u32 act, bool stream_level) -> u32 {
        if (act == kActPause) {
            log_pause_once();
            (void)m_vfs.truncate(vdst, run.done);
            cleanup(false);
            return error_codes::ERROR_REQUEST_PAUSED;
        }
        const Leftover left = (act == kActStop && stream_level)
            ? Leftover::Keep : Leftover::Delete;
        cleanup(left == Leftover::Delete);
        return error_codes::ERROR_REQUEST_ABORTED;
    };
    auto is_abort = [](u32 act) {
        return act == kActCancel || act == kActStop || act == kActPause;
    };
    // CopyFile2 reports STREAM_FINISHED once more after an abort from any
    // earlier message (cf2_cancel_at_chunk_finished seq=311124, sdone=done).
    auto cf2_abort = [&](u32 act, bool stream_level) -> u32 {
        (void)copy_file_callback(run, kMsgStreamFinished, 0, 0);
        return finish_abort(act, stream_level);
    };

    // --- stream start -------------------------------------------------------
    const bool cf2 = r.api == CopyFileApi::CopyFile2;
    {
        const u32 act = copy_file_callback(
            run, cf2 ? kMsgStreamStarted : kReasonStreamSwitch, 0, 0);
        if (is_abort(act) && !(act == kActPause && !cf2)) {
            return cf2 ? cf2_abort(act, true) : finish_abort(act, true);
        }
    }

    // --- chunks ---------------------------------------------------------------
    const u64 data_size = copy_data ? run.size : 0;
    const u64 chunks = (data_size + kChunkBytes - 1) / kChunkBytes;
    std::vector<u8> buf(static_cast<usize>(
        std::min<u64>(kChunkBytes, std::max<u64>(data_size, 1))));
    auto chunk_len = [&](u64 n) {
        return std::min<u64>(kChunkBytes, data_size - n * kChunkBytes);
    };
    auto copy_chunk = [&](u64 n) -> u32 {
        const u64 len = chunk_len(n);
        u64 moved = 0;
        while (moved < len) {
            auto rd = m_vfs.read(vsrc, std::span<u8>(buf.data(),
                                 static_cast<usize>(len - moved)));
            if (rd.is_err()) return vfs_error_to_win32(rd.error());
            if (rd.value() == 0) break;
            auto wr = m_vfs.write(vdst, std::span<const u8>(buf.data(), rd.value()));
            if (wr.is_err()) return vfs_error_to_win32(wr.error());
            moved += rd.value();
        }
        run.done += moved;
        return 0;
    };
    (void)m_vfs.seek(vdst, 0, capsule::SeekOrigin::Begin);
    u64 started = 0, finished = 0;
    while (finished < chunks) {
        if (cf2) {
            while (started < chunks && started - finished < kCopyFile2Window) {
                // ChunkStarted.uliChunkSize is the stream's chunk size, not
                // the bytes left: 1 MiB even for a 5-byte tail, the file size
                // for a one-chunk file (cf2_seq_big chunk=3, cf2_seq_small).
                const u32 act = copy_file_callback(run, kMsgChunkStarted, started,
                                                   std::min(kChunkBytes, data_size));
                ++started;
                if (is_abort(act)) return cf2_abort(act, false);
            }
        }
        if (cancelled()) {
            cleanup(true);
            return error_codes::ERROR_REQUEST_ABORTED;
        }
        if (const u32 err = copy_chunk(finished); err != 0) {
            cleanup(true);
            return err;
        }
        const u32 act = copy_file_callback(
            run, cf2 ? kMsgChunkFinished : kReasonChunkFinished, finished,
            chunk_len(finished));
        ++finished;
        if (is_abort(act) && !(act == kActPause && !cf2)) {
            if (cf2 && act == kActPause) {
                while (finished < started) {
                    if (copy_chunk(finished) != 0) break;
                    ++finished;
                }
            }
            return cf2 ? cf2_abort(act, false) : finish_abort(act, false);
        }
    }
    if (!copy_data) (void)m_vfs.truncate(vdst, 0);

    // --- stream end (CopyFile2 only; PAUSE here is ignored) --------------------
    if (cf2) {
        const u32 act = copy_file_callback(run, kMsgStreamFinished, 0, 0);
        if (act == kActCancel || act == kActStop) return finish_abort(act, true);
    }

    // Keep the source's last-write time at full FILETIME resolution.
    if (src_info.is_ok()) {
        const u64 write_ft = src_info.value().last_write_time;
        (void)m_vfs.set_file_times_ft(vdst, nullptr, nullptr, &write_ft);
    }
    cleanup(false);
    // The copy carries the source attributes and gains ARCHIVE
    // (cf2_src_hidden_ro_attrs a=23, files_ns_probe CopyFileW_attrs).
    (void)m_vfs.set_attributes(r.dst, src_attrs | kAttrArchive);
    return 0;
}

// =============================================================================
// Entry points
// =============================================================================

namespace {

struct CopyPaths {
    bool ok = false;
    std::string src, dst;
};

CopyPaths read_copy_paths(Marshaler& m, bool wide) {
    auto s = wide ? m.read_wide_string(m.arg_ptr(0)) : m.read_ansi_string(m.arg_ptr(0));
    auto d = wide ? m.read_wide_string(m.arg_ptr(1)) : m.read_ansi_string(m.arg_ptr(1));
    if (s.is_err() || d.is_err()) return {};
    return {true, s.value(), d.value()};
}

} // namespace

void Kernel32ExtManager::copy_file_ex_entry(Marshaler& m, bool wide) {
    // BOOL CopyFileEx(LPCTSTR lpExistingFileName, LPCTSTR lpNewFileName,
    //   LPPROGRESS_ROUTINE lpProgressRoutine, LPVOID lpData, LPBOOL pbCancel,
    //   DWORD dwCopyFlags)
    const u32 flags = m.arg32(5);
    if ((flags & ~kCopyFileExValidFlags) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto paths = read_copy_paths(m, wide);
    if (!paths.ok) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    CopyFileRequest r;
    r.api = CopyFileApi::CopyFileEx;
    r.src = std::move(paths.src);
    r.dst = std::move(paths.dst);
    r.flags = flags;
    r.routine = m.arg_ptr(2);
    r.context = m.arg_ptr(3);
    r.cancel = m.arg_ptr(4);
    const u32 err = run_copy_file(m, r);
    // Success sets ERROR_SUCCESS (last_error_on_success_probe CopyFileExW_overwrite).
    set_last_error(err);
    m.set_return(err == 0 ? 1u : 0u);
}

void Kernel32ExtManager::impl_CopyFileExA(Marshaler& m) { copy_file_ex_entry(m, false); }
void Kernel32ExtManager::impl_CopyFileExW(Marshaler& m) { copy_file_ex_entry(m, true); }

void Kernel32ExtManager::impl_CopyFile2(Marshaler& m) {
    // HRESULT CopyFile2(PCWSTR pwszExistingFileName, PCWSTR pwszNewFileName,
    //   COPYFILE2_EXTENDED_PARAMETERS* pExtendedParameters)
    constexpr u32 kEInvalidArg = 0x80070057u;
    const bool x64 = m.cpu_is_64bit();
    // sizeof(COPYFILE2_EXTENDED_PARAMETERS): DWORD, DWORD, three pointers.
    // sizeof(..._V2) adds DWORD dwCopyFlagsV2, ULONG ioDesiredSize,
    // ULONG ioDesiredRate and eight pointer-sized slots (winbase.h).
    const u32 v1_size = x64 ? 32u : 20u;
    const u32 v2_size = x64 ? 112u : 64u;
    CopyFileRequest r;
    r.api = CopyFileApi::CopyFile2;
    const GuestAddr ext = m.arg_ptr(2);
    if (ext != 0) {
        const u32 dw_size = m.memory().read_u32(ext).value_or(0);
        // Any other dwSize is E_INVALIDARG with the last error untouched
        // (cf2_dwsize_0 / _small / _big).
        if (dw_size != v1_size && dw_size != v2_size) {
            m.set_return(kEInvalidArg);
            return;
        }
        r.flags = m.memory().read_u32(ext + 4).value_or(0);
        if (x64) {
            r.cancel = m.memory().read_u64(ext + 8).value_or(0);
            r.routine = m.memory().read_u64(ext + 16).value_or(0);
            r.context = m.memory().read_u64(ext + 24).value_or(0);
        } else {
            r.cancel = m.memory().read_u32(ext + 8).value_or(0);
            r.routine = m.memory().read_u32(ext + 12).value_or(0);
            r.context = m.memory().read_u32(ext + 16).value_or(0);
        }
        if ((r.flags & kCopyFile2RejectedFlags) != 0) {
            m.set_return(kEInvalidArg);
            return;
        }
        if (dw_size == v2_size) {
            // pProgressRoutineOld (Windows 11): the old-style routine takes
            // precedence over pProgressRoutine (winbase.h comment).
            const GuestAddr old_routine = x64
                ? m.memory().read_u64(ext + 48).value_or(0)
                : m.memory().read_u32(ext + 32).value_or(0);
            if (old_routine != 0) {
                r.api = CopyFileApi::CopyFileEx;
                r.routine = old_routine;
            }
        }
    }
    // A NULL source is ERROR_PATH_NOT_FOUND (cf2_null_src).
    if (m.arg_ptr(0) == 0) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        m.set_return(hresult_from_win32(error_codes::ERROR_PATH_NOT_FOUND));
        return;
    }
    auto paths = read_copy_paths(m, true);
    if (!paths.ok) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(kEInvalidArg);
        return;
    }
    r.src = std::move(paths.src);
    r.dst = std::move(paths.dst);
    const u32 err = run_copy_file(m, r);
    // The Win32 code is also the thread's last error, 0 on success (cf2_* gle).
    set_last_error(err);
    m.set_return(hresult_from_win32(err));
}

} // namespace vitreo::winapi
