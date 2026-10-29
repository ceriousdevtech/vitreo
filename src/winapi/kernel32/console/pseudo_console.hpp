// SPDX-License-Identifier: LGPL-2.1-only

/// @file pseudo_console.hpp
/// @brief ConPTY: CreatePseudoConsole / ResizePseudoConsole / ClosePseudoConsole.
///
/// A pseudoconsole is a console device whose screen is a PIPE rather than a
/// window. The creator hands it two pipe ends:
///
///   hInput  — the pseudoconsole READS from this. The creator writes
///             keystrokes into the other end.
///   hOutput — the pseudoconsole WRITES to this, VT-encoded. The creator reads
///             the rendered stream from the other end.
///
/// A child launched with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE gets that console
/// as its own, so its WriteConsole/cursor/erase calls become VT in the pipe
/// and its stdin comes from the pipe. That is exactly what node-pty and every
/// other terminal front-end on Windows is built on.
///
/// HPCON is opaque on Windows (a pointer into conhost's bookkeeping). It is an
/// opaque id here, minted from a counter with a distinctive high pattern so a
/// value misused as a kernel Handle fails loudly rather than aliasing a real
/// one.

#pragma once

#include <vitreo/common.hpp>
#include "winapi/kernel32/handle.hpp"

#include <mutex>
#include <optional>
#include <unordered_map>

namespace vitreo::winapi {

/// Opaque HPCON value.
using Hpcon = u64;

inline constexpr Hpcon NULL_HPCON = 0;

class PseudoConsoleManager {
public:
    struct PseudoConsole {
        Handle input{NULL_HANDLE};    ///< pcon reads from here
        Handle output{NULL_HANDLE};   ///< pcon writes VT here
        i16 cols{80};
        i16 rows{25};
        bool inherit_cursor{false};
    };

    /// CreatePseudoConsole. `cols`/`rows` must both be > 0; Windows returns
    /// E_INVALIDARG for a zero dimension, and a zero-sized console would make
    /// every subsequent coordinate clamp to nothing.
    /// @return NULL_HPCON on failure.
    [[nodiscard]] Hpcon create(i16 cols, i16 rows, Handle input, Handle output,
                               u32 flags);

    /// ResizePseudoConsole. @return false when the handle is unknown or the
    /// size is degenerate.
    [[nodiscard]] bool resize(Hpcon pcon, i16 cols, i16 rows);

    /// ClosePseudoConsole. Returns false when the handle was not open, which
    /// callers ignore (the API is void) but tests check.
    bool close(Hpcon pcon);

    /// Look up a live pseudoconsole.
    [[nodiscard]] std::optional<PseudoConsole> find(Hpcon pcon) const;

    [[nodiscard]] usize live_count() const;

private:
    mutable std::mutex m_mutex;
    /// Minted with a high tag so an HPCON accidentally passed to CloseHandle
    /// or WaitForSingleObject cannot collide with a real kernel handle.
    static constexpr Hpcon kTag = 0x7000'0000ull;
    Hpcon m_next{kTag + 1};
    std::unordered_map<Hpcon, PseudoConsole> m_consoles;
};

} // namespace vitreo::winapi
