// SPDX-License-Identifier: LGPL-2.1-only

/// @file cabinet_fdi.hpp
/// @brief cabinet.dll's File Decompression Interface (FDI).
///
/// FDI is inverted relative to most of the Win32 surface: the caller hands
/// FDICreate a table of function pointers (alloc/free/open/read/write/close/
/// seek), and FDICopy then drives *the caller* — reading the cabinet through
/// its I/O callbacks and asking a notification callback what to do with each
/// file. This component therefore calls back into guest code rather than only
/// being called from it.
///
/// Using the caller's I/O callbacks is a contract requirement, not a
/// formality: callers routinely point them at a cabinet embedded at an offset
/// inside an already-open file (WiX Burn reads its container out of its own
/// image that way), so reading the path directly off the VFS would work for
/// standalone .cab files and silently fail for embedded ones.
///
/// Structure layouts and the DIAMONDAPI (__cdecl) convention are taken from the
/// Windows SDK's fdi.h and verified against MSVC for both bitnesses; see
/// tests/winapi/cabinet/oracle_fdi_layout.txt.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "vitreo/types.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// FDIERROR (fdi.h). Reported through the ERF the caller supplied to
/// FDICreate.
enum class FdiError : i32 {
    None = 0,
    CabinetNotFound,
    NotACabinet,
    UnknownCabinetVersion,
    CorruptCabinet,
    AllocFail,
    BadComprType,
    MdiFail,
    TargetFile,
    ReserveMismatch,
    WrongCabinet,
    UserAbort,
    Eof,
};

/// FDINOTIFICATIONTYPE (fdi.h).
enum class FdiNotification : u32 {
    CabinetInfo    = 0,
    PartialFile    = 1,
    CopyFile       = 2,
    CloseFileInfo  = 3,
    NextCabinet    = 4,
    Enumerate      = 5,
};

class CabinetManager {
public:
    /// Guest scratch for HFDI blocks, notification records, and the I/O
    /// bounce buffer the caller's read/write callbacks operate on.
    static constexpr GuestAddr CABINET_SCRATCH_BASE = 0x5F800000u;
    static constexpr u32       CABINET_SCRATCH_SIZE = 0x00200000u; // 2 MiB

    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;

    void set_thunk_registry(ThunkRegistry* thunks) noexcept {
        m_thunks = thunks;
    }
    void set_scratch_base(GuestAddr base) noexcept;
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }

    // -- exports --------------------------------------------------------------
    void impl_FDICreate(Marshaler& m);
    void impl_FDIIsCabinet(Marshaler& m);
    void impl_FDICopy(Marshaler& m);
    void impl_FDIDestroy(Marshaler& m);
    void impl_FDITruncateCabinet(Marshaler& m);

private:
    /// One FDICreate context: the caller's callback table plus its ERF.
    struct Session {
        GuestAddr alloc = 0;
        GuestAddr free  = 0;
        GuestAddr open  = 0;
        GuestAddr read  = 0;
        GuestAddr write = 0;
        GuestAddr close = 0;
        GuestAddr seek  = 0;
        GuestAddr erf   = 0;  ///< PERF handed to FDICreate.
        bool      is_64bit = false;
    };

    [[nodiscard]] Session* find(GuestAddr hfdi);

    // -- guest callback wrappers ---------------------------------------------
    [[nodiscard]] i64 call_open(const Session& s, GuestAddr path, i32 oflag,
                                i32 pmode);
    [[nodiscard]] u32 call_read(const Session& s, i64 hf, GuestAddr buffer,
                                u32 count);
    [[nodiscard]] u32 call_write(const Session& s, i64 hf, GuestAddr buffer,
                                 u32 count);
    [[nodiscard]] i32 call_close(const Session& s, i64 hf);
    [[nodiscard]] i64 call_seek(const Session& s, i64 hf, i64 distance,
                                i32 origin);
    [[nodiscard]] i64 call_notify(const Session& s, GuestAddr notify,
                                  FdiNotification type, GuestAddr record);

    /// Pull an entire cabinet image through the caller's read/seek callbacks.
    [[nodiscard]] std::optional<std::vector<u8>> read_cabinet_image(
        Marshaler& m, const Session& s, i64 hf);

    /// Record @p error in the session's ERF and return false.
    bool fail(const Session& s, Marshaler& m, FdiError error);
    void clear_error(const Session& s, Marshaler& m);

    /// Scratch allocation. The arena is a ring: notification records and
    /// strings live only for the duration of one callback.
    [[nodiscard]] GuestAddr scratch_alloc(u32 bytes);
    [[nodiscard]] GuestAddr write_scratch_ansi(Marshaler& m,
                                               std::string_view text);

    ThunkRegistry* m_thunks   = nullptr;
    GuestCall      m_guest_call;
    GuestAddr      m_base     = 0;
    GuestAddr      m_cursor   = 0;
    GuestAddr      m_io_buffer = 0;
    GuestAddr      m_next_hfdi = 0;

    std::unordered_map<GuestAddr, Session> m_sessions;
};

/// Build the cabinet.dll export table.
[[nodiscard]] DllExports create_cabinet_thunks(CabinetManager& mgr);

} // namespace vitreo::winapi
