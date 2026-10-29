// SPDX-License-Identifier: LGPL-2.1-only

/// @file console_screen.cpp
/// @brief Console screen buffer and input queue state.
///
/// The console layer was a pass-through to the host terminal with no state of
/// its own: no cursor, no attributes, no cell contents, no input queue. Every
/// API that reads or writes those was either absent (registered as a
/// missing-export stub returning 0xFFFFFFFF) or a no-op returning TRUE.
///
/// That is fine for a program that only ever writes lines. It is not fine for
/// anything that DRIVES a console -- a progress display that rewrites a line
/// in place, an installer, or a pseudo-console consumer such as node-pty --
/// because those position the cursor, erase spans, and read input records, and
/// then expect what they wrote to still be there.

#include "winapi/kernel32/console/console.hpp"

#include "winapi/kernel32/locale/string_conv.hpp"
#include <algorithm>
#include <poll.h>
#include <unistd.h>
#include <string_view>

namespace vitreo::winapi {

void ConsoleManager::reflow_cells_locked(i16 cols, i16 rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    std::vector<Cell> next(static_cast<usize>(cols) * static_cast<usize>(rows));
    // Preserve the overlapping region so a resize does not blank the screen.
    // Windows keeps the buffer contents across SetConsoleScreenBufferSize;
    // only cells outside the new extent are lost.
    // m_buf_cols/m_buf_rows describe the LOGICAL size and are initialised to
    // 80x25 before any cells exist, so the copy bound has to come from the
    // vector itself. Trusting the dimensions on the first reflow reads off the
    // end of an empty buffer.
    const bool have_old =
        m_cells.size() == static_cast<usize>(m_buf_cols) *
                          static_cast<usize>(m_buf_rows) &&
        !m_cells.empty();
    const i16 copy_cols = have_old ? std::min(cols, m_buf_cols) : 0;
    const i16 copy_rows = have_old ? std::min(rows, m_buf_rows) : 0;
    for (i16 y = 0; y < copy_rows; ++y) {
        for (i16 x = 0; x < copy_cols; ++x) {
            next[static_cast<usize>(y) * cols + x] =
                m_cells[static_cast<usize>(y) * m_buf_cols + x];
        }
    }
    m_cells.swap(next);
    m_buf_cols = cols;
    m_buf_rows = rows;
    m_cursor_x = std::min<i16>(m_cursor_x, static_cast<i16>(cols - 1));
    m_cursor_y = std::min<i16>(m_cursor_y, static_cast<i16>(rows - 1));
}

void ConsoleManager::set_cursor_position(i16 x, i16 y) {
    std::lock_guard<std::mutex> lock(m_screen_mutex);
    if (m_cells.empty()) reflow_cells_locked(m_buf_cols, m_buf_rows);
    m_cursor_x = std::clamp<i16>(x, 0, static_cast<i16>(m_buf_cols - 1));
    m_cursor_y = std::clamp<i16>(y, 0, static_cast<i16>(m_buf_rows - 1));
}

std::pair<i16, i16> ConsoleManager::cursor_position() const {
    std::lock_guard<std::mutex> lock(m_screen_mutex);
    return {m_cursor_x, m_cursor_y};
}

ConsoleManager::CursorInfo ConsoleManager::cursor_info() const {
    std::lock_guard<std::mutex> lock(m_screen_mutex);
    return m_cursor_info;
}

void ConsoleManager::set_cursor_info(const CursorInfo& info) {
    std::lock_guard<std::mutex> lock(m_screen_mutex);
    m_cursor_info = info;
}

std::pair<i16, i16> ConsoleManager::buffer_size() const {
    std::lock_guard<std::mutex> lock(m_screen_mutex);
    return {m_buf_cols, m_buf_rows};
}

void ConsoleManager::set_buffer_size(i16 cols, i16 rows) {
    std::lock_guard<std::mutex> lock(m_screen_mutex);
    reflow_cells_locked(cols, rows);
}

u16 ConsoleManager::text_attributes() const {
    std::lock_guard<std::mutex> lock(m_screen_mutex);
    return m_attributes;
}

void ConsoleManager::set_text_attributes(u16 attr) {
    std::lock_guard<std::mutex> lock(m_screen_mutex);
    m_attributes = attr;
}

namespace {
/// Shared body of the two fills. `apply` writes one cell.
template <typename Apply>
u32 fill_cells(std::vector<ConsoleManager::Cell>& cells, i16 cols, i16 rows,
               i16 x, i16 y, u32 count, Apply apply) {
    if (cols < 1 || rows < 1) return 0;
    if (x < 0 || y < 0 || x >= cols || y >= rows) return 0;
    // A fill runs from the start coordinate and WRAPS onto the following
    // rows; it is not clipped to the starting row. It stops at the end of
    // the buffer rather than scrolling.
    usize index = static_cast<usize>(y) * static_cast<usize>(cols) +
                  static_cast<usize>(x);
    const usize total = static_cast<usize>(cols) * static_cast<usize>(rows);
    u32 written = 0;
    while (written < count && index < total) {
        apply(cells[index]);
        ++index;
        ++written;
    }
    return written;
}
} // namespace

u32 ConsoleManager::fill_output_character(i16 x, i16 y, u16 ch, u32 count) {
    std::lock_guard<std::mutex> lock(m_screen_mutex);
    if (m_cells.empty()) reflow_cells_locked(m_buf_cols, m_buf_rows);
    return fill_cells(m_cells, m_buf_cols, m_buf_rows, x, y, count,
                      [ch](Cell& c) { c.ch = ch; });
}

u32 ConsoleManager::fill_output_attribute(i16 x, i16 y, u16 attr, u32 count) {
    std::lock_guard<std::mutex> lock(m_screen_mutex);
    if (m_cells.empty()) reflow_cells_locked(m_buf_cols, m_buf_rows);
    return fill_cells(m_cells, m_buf_cols, m_buf_rows, x, y, count,
                      [attr](Cell& c) { c.attr = attr; });
}

void ConsoleManager::write_host_output(std::string_view bytes) {
    if (bytes.empty()) return;
    dispatch_output(m_stdout_handle, bytes);
}

namespace {

/// Key record for one typed UTF-16 unit, US layout (winuser.h VK_* values).
ConsoleManager::InputRecord key_record_for_unit(u16 unit, bool down) {
    constexpr u32 SHIFT_PRESSED = 0x0010;
    constexpr u32 LEFT_CTRL_PRESSED = 0x0008;
    constexpr u16 VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D,
                  VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PACKET = 0xE7;
    ConsoleManager::InputRecord r{};
    r.event_type = 1;  // KEY_EVENT
    r.key_down = down;
    r.repeat = 1;
    r.unicode_char = unit;
    if (unit >= 'a' && unit <= 'z') {
        r.virtual_key = static_cast<u16>(unit - 'a' + 'A');
    } else if (unit >= 'A' && unit <= 'Z') {
        r.virtual_key = unit;
        r.control_key_state = SHIFT_PRESSED;
    } else if (unit >= '0' && unit <= '9') {
        r.virtual_key = unit;
    } else if (unit == ' ') {
        r.virtual_key = VK_SPACE;
    } else if (unit == '\r' || unit == '\n') {
        r.virtual_key = VK_RETURN;
        r.unicode_char = '\r';
    } else if (unit == '\t') {
        r.virtual_key = VK_TAB;
    } else if (unit == 0x08 || unit == 0x7F) {
        r.virtual_key = VK_BACK;
        r.unicode_char = 0x08;
    } else if (unit == 0x1B) {
        r.virtual_key = VK_ESCAPE;
    } else if (unit >= 1 && unit <= 26) {
        r.virtual_key = static_cast<u16>('A' + unit - 1);
        r.control_key_state = LEFT_CTRL_PRESSED;
    } else {
        // A character with no key on the US layout arrives as VK_PACKET, the
        // virtual key Windows uses for Unicode input.
        r.virtual_key = VK_PACKET;
    }
    return r;
}

bool default_input_ready() {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    return ::poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP)) != 0;
}

} // namespace

std::vector<u16> ConsoleManager::pull_host_text() {
    std::lock_guard<std::mutex> host_lock(m_host_read_mutex);
    std::string bytes = m_input_cb ? m_input_cb() : std::string{};
    if (bytes.empty()) {
        std::lock_guard<std::mutex> lock(m_input_mutex);
        m_host_input_eof = true;
        m_input_cv.notify_all();
        return {};
    }
    bytes.insert(0, m_pending_utf8);
    m_pending_utf8.clear();
    // Keep an incomplete trailing UTF-8 sequence for the next chunk.
    usize cut = bytes.size();
    for (usize back = 1; back <= 3 && back <= bytes.size(); ++back) {
        const u8 c = static_cast<u8>(bytes[bytes.size() - back]);
        if ((c & 0xC0) == 0x80) continue;          // continuation byte
        const usize need = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
                         : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (need > back) cut = bytes.size() - back;
        break;
    }
    m_pending_utf8 = bytes.substr(cut);
    bytes.resize(cut);
    return utf8_to_utf16(bytes);
}

void ConsoleManager::queue_text_records_locked(const std::vector<u16>& text) {
    for (const u16 unit : text) {
        m_input_queue.push_back(key_record_for_unit(unit, true));
        m_input_queue.push_back(key_record_for_unit(unit, false));
    }
    if (!text.empty()) {
        note_input_available_locked(true);
        m_input_cv.notify_all();
    }
}

u32 ConsoleManager::pending_input_events() const {
    // GetNumberOfConsoleInputEvents counts unread records, including host
    // keystrokes that are already available (pulled without blocking).
    auto* self = const_cast<ConsoleManager*>(this);
    bool pull = false;
    {
        std::lock_guard<std::mutex> lock(m_input_mutex);
        pull = m_input_queue.empty() && !m_host_input_eof;
    }
    if (pull && (m_input_ready_cb ? m_input_ready_cb() : default_input_ready())) {
        const auto text = self->pull_host_text();
        std::lock_guard<std::mutex> lock(m_input_mutex);
        self->queue_text_records_locked(text);
    }
    std::lock_guard<std::mutex> lock(m_input_mutex);
    return static_cast<u32>(m_input_queue.size());
}

u32 ConsoleManager::read_input(std::vector<InputRecord>& out, u32 max_records,
                               bool peek) {
    out.clear();
    if (max_records == 0) return 0;
    std::unique_lock<std::mutex> lock(m_input_mutex);
    if (m_input_queue.empty() && !m_host_input_eof &&
        (!peek || (m_input_ready_cb ? m_input_ready_cb() : default_input_ready()))) {
        lock.unlock();
        const auto text = pull_host_text();
        lock.lock();
        queue_text_records_locked(text);
    }
    if (!peek) {
        // ReadConsoleInput does not return until a record exists. Once the
        // host input has ended only WriteConsoleInput can supply one.
        while (m_input_queue.empty()) {
            if (!m_host_input_eof) {
                lock.unlock();
                const auto text = pull_host_text();
                lock.lock();
                queue_text_records_locked(text);
                continue;
            }
            m_input_cv.wait(lock);
        }
    }
    const u32 n = std::min<u32>(max_records,
                                static_cast<u32>(m_input_queue.size()));
    out.reserve(n);
    for (u32 i = 0; i < n; ++i) out.push_back(m_input_queue[i]);
    // PeekConsoleInput leaves the records queued; ReadConsoleInput consumes
    // them. Sharing one path keeps the two from drifting apart, which is how
    // a peek that silently consumed would present as dropped input.
    if (!peek) {
        m_input_queue.erase(m_input_queue.begin(),
                            m_input_queue.begin() + static_cast<isize>(n));
        note_input_available_locked(!m_input_queue.empty());
    }
    return n;
}

u32 ConsoleManager::write_input(const std::vector<InputRecord>& records) {
    std::lock_guard<std::mutex> lock(m_input_mutex);
    for (const InputRecord& r : records) m_input_queue.push_back(r);
    if (!records.empty()) note_input_available_locked(true);
    m_input_cv.notify_all();
    return static_cast<u32>(records.size());
}

std::vector<u16> ConsoleManager::read_text(u32 max_units, u32 wakeup_mask) {
    std::vector<u16> out;
    if (max_units == 0) return out;
    std::unique_lock<std::mutex> lock(m_input_mutex);
    // Characters typed as key events (WriteConsoleInput or keystrokes a
    // ReadConsoleInput caller did not consume) are read first.
    const auto push_text = [&](u16 unit) {
        if (unit == '\n' || unit == '\r') {
            m_pending_text.push_back('\r');
            m_pending_text.push_back('\n');
        } else {
            m_pending_text.push_back(unit);
        }
    };
    while (!m_input_queue.empty()) {
        const InputRecord r = m_input_queue.front();
        m_input_queue.pop_front();
        if (r.event_type == 1 && r.key_down && r.unicode_char != 0) {
            for (u16 i = 0; i < std::max<u16>(r.repeat, 1); ++i) push_text(r.unicode_char);
        }
    }
    if (m_pending_text.empty() && !m_host_input_eof) {
        lock.unlock();
        const auto text = pull_host_text();
        lock.lock();
        for (const u16 unit : text) push_text(unit);
    }
    // Line input returns one line per call; a wake-up control character
    // ends the read right after it.
    while (!m_pending_text.empty() && out.size() < max_units) {
        const u16 unit = m_pending_text.front();
        m_pending_text.pop_front();
        out.push_back(unit);
        if (unit == '\n') break;
        if (unit < 32 && (wakeup_mask & (1u << unit)) != 0) break;
    }
    return out;
}

} // namespace vitreo::winapi
