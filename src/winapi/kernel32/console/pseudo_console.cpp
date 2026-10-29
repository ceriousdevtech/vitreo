// SPDX-License-Identifier: LGPL-2.1-only

/// @file pseudo_console.cpp
/// @brief ConPTY object lifetime. See pseudo_console.hpp.

#include "winapi/kernel32/console/pseudo_console.hpp"

namespace vitreo::winapi {

Hpcon PseudoConsoleManager::create(i16 cols, i16 rows, Handle input,
                                   Handle output, u32 flags) {
    // Windows rejects a zero dimension with E_INVALIDARG. Accepting one would
    // leave every cursor coordinate clamping into an empty buffer, which
    // presents much later as a terminal that renders nothing.
    if (cols <= 0 || rows <= 0) return NULL_HPCON;

    constexpr u32 kInheritCursor = 0x1;   // PSEUDOCONSOLE_INHERIT_CURSOR

    std::lock_guard<std::mutex> lock(m_mutex);
    const Hpcon pcon = m_next++;
    PseudoConsole pc{};
    pc.input = input;
    pc.output = output;
    pc.cols = cols;
    pc.rows = rows;
    pc.inherit_cursor = (flags & kInheritCursor) != 0;
    m_consoles.emplace(pcon, pc);
    return pcon;
}

bool PseudoConsoleManager::resize(Hpcon pcon, i16 cols, i16 rows) {
    if (cols <= 0 || rows <= 0) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_consoles.find(pcon);
    if (it == m_consoles.end()) return false;
    it->second.cols = cols;
    it->second.rows = rows;
    return true;
}

bool PseudoConsoleManager::close(Hpcon pcon) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_consoles.erase(pcon) != 0;
}

std::optional<PseudoConsoleManager::PseudoConsole>
PseudoConsoleManager::find(Hpcon pcon) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_consoles.find(pcon);
    if (it == m_consoles.end()) return std::nullopt;
    return it->second;
}

usize PseudoConsoleManager::live_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_consoles.size();
}

} // namespace vitreo::winapi
