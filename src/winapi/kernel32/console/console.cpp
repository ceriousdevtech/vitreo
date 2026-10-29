/// @file console.cpp
/// @brief Console I/O implementation (WIN32-032)

#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "core/sync/ntsync.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "core/cpu/state/cpu_state.hpp"

#include <unistd.h>
#include <sys/ioctl.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// Default I/O Callbacks
// =============================================================================

namespace {

void default_stdout_write(std::string_view text) {
    ::write(STDOUT_FILENO, text.data(), text.size());
}

void default_stderr_write(std::string_view text) {
    ::write(STDERR_FILENO, text.data(), text.size());
}

std::string default_stdin_read() {
    constexpr usize BUF_SIZE = 4096;
    char buf[BUF_SIZE];
    auto n = ::read(STDIN_FILENO, buf, BUF_SIZE);
    if (n <= 0) {
        return {};
    }
    return std::string(buf, static_cast<usize>(n));
}

} // anonymous namespace

// =============================================================================
// Constructor
// =============================================================================

ConsoleManager::ConsoleManager(HandleTable& handles, bool attached)
    : m_handles(handles)
    , m_attached(attached)
    , m_output_cb(default_stdout_write)
    , m_error_cb(default_stderr_write)
    , m_input_cb(default_stdin_read)
{
    if (!attached) {
        return;
    }
    // Pre-allocate the three standard handles
    m_stdin_handle  = m_handles.allocate(HandleType::ConsoleInput, nullptr);
    m_stdout_handle = m_handles.allocate(HandleType::ConsoleOutput, nullptr);
    m_stderr_handle = m_handles.allocate(HandleType::ConsoleOutput, nullptr);
    m_std_slots[0] = m_stdin_handle;
    m_std_slots[1] = m_stdout_handle;
    m_std_slots[2] = m_stderr_handle;

    // Set default console modes. STDOUT and STDERR share one screen-buffer
    // mode entry (Learn console-handles: both initially name the active
    // screen buffer).
    m_console_modes[m_stdin_handle]  = DEFAULT_INPUT_MODE;
    m_console_modes[m_stdout_handle] = DEFAULT_OUTPUT_MODE;
}

// =============================================================================
// Callback Configuration
// =============================================================================

void ConsoleManager::set_output_callback(OutputCallback cb) {
    m_output_cb = cb ? std::move(cb) : OutputCallback(default_stdout_write);
}

void ConsoleManager::set_error_callback(OutputCallback cb) {
    m_error_cb = cb ? std::move(cb) : OutputCallback(default_stderr_write);
}

void ConsoleManager::set_input_callback(InputCallback cb) {
    m_input_cb = cb ? std::move(cb) : InputCallback(default_stdin_read);
}

void ConsoleManager::set_sync_manager(SyncManager* sync) {
    m_sync_manager = sync;
    if (!sync || m_stdin_handle == NULL_HANDLE || m_stdin_wait_nt_id != 0) {
        return;
    }
    // Manual-reset: signaled while unread input exists. Start nonsignaled;
    // WriteConsoleInput / host reads arm it. A zero-timeout Wait that finds
    // host stdin ready will still see TIMEOUT until input is queued — matches
    // a quiet console; interactive guests that Wait then Read still progress
    // once input arrives.
    m_stdin_wait_nt_id =
        ntsync::global().create_event(/*manual_reset=*/true,
                                      /*initial_state=*/false);
    sync->register_waitable(m_stdin_handle, m_stdin_wait_nt_id);
}

void ConsoleManager::note_input_available_locked(bool available) {
    if (m_stdin_wait_nt_id == 0) return;
    if (available) {
        ntsync::global().set_event(m_stdin_wait_nt_id);
    } else {
        ntsync::global().reset_event(m_stdin_wait_nt_id);
    }
}

// =============================================================================
// get_std_handle
// =============================================================================

namespace {
int std_slot_index(u32 n_std_handle) {
    switch (n_std_handle) {
        case STD_INPUT_HANDLE:  return 0;
        case STD_OUTPUT_HANDLE: return 1;
        case STD_ERROR_HANDLE:  return 2;
        default:                return -1;
    }
}
} // namespace

Handle ConsoleManager::get_std_handle(u32 n_std_handle) const {
    // Learn GetStdHandle: returns the redirected handle set by SetStdHandle
    // or inherited through STARTF_USESTDHANDLES. Windows 11
    // (process_contract_probe.expected): an unknown nStdHandle returns
    // INVALID_HANDLE_VALUE with ERROR_INVALID_HANDLE, and a slot that was set
    // to INVALID_HANDLE_VALUE reports it with the same last error.
    const int slot = std_slot_index(n_std_handle);
    if (slot < 0) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return INVALID_HANDLE;
    }
    Handle h;
    {
        std::lock_guard lock(m_std_mutex);
        h = m_std_slots[slot];
    }
    if (h == INVALID_HANDLE) set_last_error(error_codes::ERROR_INVALID_HANDLE);
    return h;
}

bool ConsoleManager::set_std_handle(u32 n_std_handle, Handle handle) {
    const int slot = std_slot_index(n_std_handle);
    if (slot < 0) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    std::lock_guard lock(m_std_mutex);
    m_std_slots[slot] = handle;
    return true;
}

u32 ConsoleManager::console_handle_role(Handle h) const noexcept {
    if (!m_attached || h == NULL_HANDLE) return 0;
    if (h == m_stdin_handle) return 1;
    if (h == m_stdout_handle || h == m_stderr_handle) return 2;
    return 0;
}

// =============================================================================
// write_console_a
// =============================================================================

bool ConsoleManager::write_console_a(Handle h, GuestAddr buffer, u32 chars_to_write,
                                     GuestAddr chars_written_addr, Marshaler& m) {
    if (!is_output_handle(h)) {
        set_last_error(read_handle_error(h));
        return false;
    }

    if (chars_to_write == 0) {
        // Write 0 chars is valid — just report 0 written
        if (chars_written_addr != 0) {
            auto wr = m.write_u32(chars_written_addr, 0);
            if (wr.is_err()) { return false; }
        }
        return true;
    }

    // Read ANSI bytes from guest memory
    std::vector<Byte> buf(chars_to_write);
    auto read_res = m.memory().read_bytes(buffer, buf.data(), chars_to_write);
    if (read_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    std::string_view text(reinterpret_cast<const char*>(buf.data()), chars_to_write);
    dispatch_output(h, text);

    // Write chars_written count
    if (chars_written_addr != 0) {
        auto wr = m.write_u32(chars_written_addr, chars_to_write);
        if (wr.is_err()) { return false; }
    }

    return true;
}

// =============================================================================
// write_console_w
// =============================================================================

bool ConsoleManager::write_console_w(Handle h, GuestAddr buffer, u32 chars_to_write,
                                     GuestAddr chars_written_addr, Marshaler& m) {
    if (!is_output_handle(h)) {
        set_last_error(read_handle_error(h));
        return false;
    }

    if (chars_to_write == 0) {
        if (chars_written_addr != 0) {
            auto wr = m.write_u32(chars_written_addr, 0);
            if (wr.is_err()) { return false; }
        }
        return true;
    }

    // Read UTF-16LE code units and convert to well-formed UTF-8 (surrogate
    // pairs decode to a single non-BMP code point — not CESU-8).
    std::vector<u16> units(chars_to_write);
    for (u32 i = 0; i < chars_to_write; ++i) {
        auto word_res = m.memory().read_u16(buffer + static_cast<GuestAddr>(i) * 2);
        if (word_res.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return false;
        }
        units[i] = word_res.value();
    }
    const std::string utf8 = utf16_to_utf8(
        units.data(), static_cast<int>(units.size()));
    // Lone/invalid surrogates yield an empty conversion; still report the
    // requested count written (Windows writes the units to the buffer).

    dispatch_output(h, utf8);

    // Report the number of wide characters written (not bytes)
    if (chars_written_addr != 0) {
        auto wr = m.write_u32(chars_written_addr, chars_to_write);
        if (wr.is_err()) { return false; }
    }

    return true;
}

// =============================================================================
// read_console_a
// =============================================================================

bool ConsoleManager::read_console_a(Handle h, GuestAddr buffer, u32 chars_to_read,
                                    GuestAddr chars_read_addr, Marshaler& m) {
    // Same contract as ReadConsoleW; characters are delivered in the console
    // input code page (Learn ReadConsole: the A form uses "8-bit characters
    // from the console's current code page").
    if (!is_input_handle(h)) {
        set_last_error(read_handle_error(h));
        return false;
    }
    const std::vector<u16> text = read_text(chars_to_read, 0);
    std::string bytes(text.size() * 2, '\0');
    u32 n = 0;
    if (!text.empty()) {
        const auto r = codepage::wide_to_multi_byte(
            codepage::kOemCodePage, 0, text.data(), text.size(),
            reinterpret_cast<u8*>(bytes.data()), bytes.size(), nullptr, false);
        n = std::min<u32>(r.count, chars_to_read);
    }
    if (n > 0 && m.memory().write_bytes(buffer,
            reinterpret_cast<const Byte*>(bytes.data()), n).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }
    if (chars_read_addr != 0 && m.write_u32(chars_read_addr, n).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }
    return true;
}

u32 ConsoleManager::read_handle_error(Handle h) const noexcept {
    // Windows 11 (process_contract_probe.expected): ReadConsole /
    // ReadConsoleInput / WriteConsole on a valid handle that is not a console
    // handle (a pipe) fail with ERROR_INVALID_FUNCTION; an invalid handle, or
    // any handle once no console is attached, with ERROR_INVALID_HANDLE.
    if (h != NULL_HANDLE && m_handles.is_valid(h)) {
        const HandleType t = m_handles.get_type(h);
        if (t != HandleType::ConsoleInput && t != HandleType::ConsoleOutput) {
            return error_codes::ERROR_INVALID_FUNCTION;
        }
    }
    return error_codes::ERROR_INVALID_HANDLE;
}

bool ConsoleManager::add_ctrl_handler(GuestAddr routine) {
    std::lock_guard lock(m_ctrl_mutex);
    m_ctrl_handlers.push_back(routine);
    return true;
}

bool ConsoleManager::remove_ctrl_handler(GuestAddr routine) {
    // Removes the most recent registration of the routine; removing one that
    // is not registered fails (ERROR_INVALID_PARAMETER, probe "remove again").
    std::lock_guard lock(m_ctrl_mutex);
    for (auto it = m_ctrl_handlers.rbegin(); it != m_ctrl_handlers.rend(); ++it) {
        if (*it == routine) {
            m_ctrl_handlers.erase(std::next(it).base());
            return true;
        }
    }
    return false;
}

std::vector<GuestAddr> ConsoleManager::ctrl_handlers() const {
    std::lock_guard lock(m_ctrl_mutex);
    return {m_ctrl_handlers.rbegin(), m_ctrl_handlers.rend()};
}

void ConsoleManager::detach() {
    // FreeConsole: the process no longer has a console. Its console handles
    // stop working (GetConsoleMode/WriteConsole -> ERROR_INVALID_HANDLE) but
    // the std-handle slots keep their values, and the control-handler list
    // is reset by the caller.
    m_attached = false;
    {
        std::lock_guard lock(m_ctrl_mutex);
        m_ctrl_handlers.clear();
    }
    m_ignore_ctrl_c.store(false);
}

bool ConsoleManager::set_ctrl_handler(GuestAddr handler, bool add) {
    std::lock_guard lock(m_ctrl_mutex);
    if (handler == 0) {
        // NULL HandlerRoutine: Add=TRUE ignores Ctrl+C; Add=FALSE restores it.
        m_ignore_ctrl_c = add;
        return true;
    }
    if (add) {
        m_ctrl_handlers.push_back(handler);
        return true;
    }
    auto it = std::find(m_ctrl_handlers.begin(), m_ctrl_handlers.end(), handler);
    if (it == m_ctrl_handlers.end()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    m_ctrl_handlers.erase(it);
    return true;
}

void ConsoleManager::clear_ctrl_handlers() {
    std::lock_guard lock(m_ctrl_mutex);
    m_ctrl_handlers.clear();
    m_ignore_ctrl_c = false;
}

// =============================================================================
// get_console_mode / set_console_mode
// =============================================================================

bool ConsoleManager::get_console_mode(Handle h, GuestAddr mode_addr, Marshaler& m) {
    if (!m_attached) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    if (mode_addr == 0) {
        // Windows 11 (process_contract_probe nullmode child): writing through
        // NULL lpMode raises STATUS_ACCESS_VIOLATION (exit=c0000005, no result
        // line). Mirror that via pending_raise so unhandled SEH terminates.
        if (auto* cpu = m.cpu()) {
            auto& fault = cpu->pending_raise;
            fault.active = true;
            fault.from_helper = false;
            fault.code = cpu::STATUS_ACCESS_VIOLATION;
            fault.flags = 0;
            fault.num_args = 2;
            fault.args[0] = 1; // write
            fault.args[1] = 0; // address
        } else {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        }
        return false;
    }
    auto it = m_console_modes.find(mode_key(h));
    if (it == m_console_modes.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }

    auto wr = m.write_u32(mode_addr, it->second);
    if (wr.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }

    return true;
}

bool ConsoleManager::set_console_mode(Handle h, u32 mode) {
    if (!m_attached) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    const Handle key = mode_key(h);
    auto it = m_console_modes.find(key);
    if (it == m_console_modes.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }

    // Validate against the handle's type (stderr aliases to the stdout mode
    // key but remains ConsoleOutput for flag checks).
    const Handle type_h = canonical(h);
    const HandleType type = m_handles.get_type(type_h);
    if (type == HandleType::ConsoleInput) {
        if ((mode & ~kValidInputModeMask) != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return false;
        }
        // Learn: ENABLE_ECHO_INPUT requires ENABLE_LINE_INPUT.
        if ((mode & ENABLE_ECHO_INPUT) != 0 &&
            (mode & ENABLE_LINE_INPUT) == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return false;
        }
    } else if (type == HandleType::ConsoleOutput) {
        if ((mode & ~kValidOutputModeMask) != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return false;
        }
    } else {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }

    it->second = mode;
    return true;
}

bool ConsoleManager::set_console_title_w(GuestAddr title_addr, Marshaler& m) {
    if (!m_attached) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    if (title_addr == 0) {
        set_last_error(error_codes::ERROR_INVALID_ACCESS);
        return false;
    }

    constexpr usize MAX_TITLE_CHARS = 65535;
    std::vector<u16> title;
    title.reserve(64);
    bool terminated = false;
    for (usize i = 0; i < MAX_TITLE_CHARS; ++i) {
        auto code_unit = m.memory().read_u16(
            title_addr + static_cast<GuestAddr>(i * sizeof(u16)));
        if (code_unit.is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }
        if (code_unit.value() == 0) {
            terminated = true;
            break;
        }
        title.push_back(code_unit.value());
    }
    if (!terminated) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    const usize title_size = title.size();
    {
        std::lock_guard lock(m_title_mutex);
        m_title = std::move(title);
    }
    VITREO_TRACE("[console] SetConsoleTitleW(chars=%zu) -> TRUE\n",
                 title_size);
    return true;
}

u32 ConsoleManager::get_console_title_w(GuestAddr buffer_addr,
                                        u32 buffer_chars,
                                        Marshaler& m) {
    if (!m_attached) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return 0;
    }
    if (buffer_chars == 0) {
        return 0;
    }
    if (buffer_addr == 0) {
        set_last_error(error_codes::ERROR_INVALID_ACCESS);
        return 0;
    }

    std::vector<u16> title;
    {
        std::lock_guard lock(m_title_mutex);
        title = m_title;
    }

    const usize copied = std::min<usize>(
        title.size(), static_cast<usize>(buffer_chars - 1));
    for (usize i = 0; i < copied; ++i) {
        if (m.memory().write_u16(
                buffer_addr + static_cast<GuestAddr>(i * sizeof(u16)),
                title[i]).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return 0;
        }
    }
    if (m.memory().write_u16(
            buffer_addr + static_cast<GuestAddr>(copied * sizeof(u16)),
            0).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return 0;
    }
    return static_cast<u32>(copied);
}

// =============================================================================
// Private Helpers
// =============================================================================

Handle ConsoleManager::canonical(Handle h) const {
    std::lock_guard<std::mutex> lock(m_alias_mutex);
    const auto it = m_aliases.find(h);
    // A recycled handle value of another type is not an alias any more.
    if (it == m_aliases.end() ||
        m_handles.get_type(h) != m_handles.get_type(it->second)) {
        return h;
    }
    return it->second;
}

Handle ConsoleManager::mode_key(Handle h) const {
    h = canonical(h);
    // Active screen buffer: stdout and stderr share one mode (Learn
    // console-handles; process_contract_probe sharedmode child).
    if (h == m_stderr_handle) {
        return m_stdout_handle;
    }
    return h;
}

Handle ConsoleManager::duplicate_handle(Handle source, u32 flags, u32 access) {
    const Handle c = canonical(source);
    if (c == NULL_HANDLE ||
        (c != m_stdin_handle && c != m_stdout_handle && c != m_stderr_handle)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return NULL_HANDLE;
    }
    const Handle dup = m_handles.allocate(m_handles.get_type(c), nullptr,
                                          flags, access);
    if (dup == NULL_HANDLE) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return NULL_HANDLE;
    }
    std::lock_guard<std::mutex> lock(m_alias_mutex);
    m_aliases[dup] = c;
    return dup;
}

bool ConsoleManager::is_output_handle(Handle h) const noexcept {
    h = canonical(h);
    return m_attached && h != NULL_HANDLE &&
           (h == m_stdout_handle || h == m_stderr_handle);
}

bool ConsoleManager::is_input_handle(Handle h) const noexcept {
    h = canonical(h);
    return m_attached && h != NULL_HANDLE && h == m_stdin_handle;
}

void ConsoleManager::dispatch_output(Handle h, std::string_view text) {
    // When this process's console IS a pseudoconsole, everything the console
    // produces -- text, cursor moves, erases, colour -- belongs in the
    // pseudoconsole's output pipe, because that is where the creating process
    // reads its terminal from. Sending it to the host stdout instead is why a
    // ConPTY front-end would see an empty stream and wait forever.
    //
    // stderr is NOT split out here: a pseudoconsole is one screen, and
    // conhost interleaves both streams into the single VT output. Splitting
    // them would reorder a program's output relative to its own diagnostics.
    if (m_pcon_output != NULL_HANDLE && m_handle_writer) {
        (void)m_handle_writer(m_pcon_output, text);
        return;
    }
    if (canonical(h) == m_stderr_handle) {
        m_error_cb(text);
    } else {
        m_output_cb(text);
    }
}

void ConsoleManager::attach_pseudo_console(Handle input, Handle output) {
    m_pcon_input = input;
    m_pcon_output = output;
    VITREO_TRACE("[conpty] console attached to pseudoconsole in=0x%x out=0x%x\n",
                 input, output);
}

// =============================================================================
// Thunk Implementations
// =============================================================================

void impl_GetStdHandle(Marshaler& m, ConsoleManager& cm) {
    u32 n_std_handle = static_cast<u32>(m.arg(0));
    Handle h = cm.get_std_handle(n_std_handle);
    // A stored pseudo value such as INVALID_HANDLE_VALUE is (HANDLE)-1: all
    // 64 bits on x64 (process_contract_probe: ffffffffffffffff).
    const u64 value = m.cpu_is_64bit() && (h & 0x80000000u) != 0
        ? static_cast<u64>(static_cast<i64>(static_cast<i32>(h)))
        : static_cast<u64>(h);
    m.set_return(value);
}

void impl_WriteConsoleA(Marshaler& m, ConsoleManager& cm) {
    Handle handle              = static_cast<Handle>(m.arg(0));
    GuestAddr buffer           = m.arg_ptr(1);
    u32 chars_to_write         = static_cast<u32>(m.arg(2));
    GuestAddr chars_written_addr = m.arg_ptr(3);
    bool ok = cm.write_console_a(handle, buffer, chars_to_write, chars_written_addr, m);
    m.set_return(ok ? 1u : 0u);
}

void impl_WriteConsoleW(Marshaler& m, ConsoleManager& cm) {
    Handle handle              = static_cast<Handle>(m.arg(0));
    GuestAddr buffer           = m.arg_ptr(1);
    u32 chars_to_write         = static_cast<u32>(m.arg(2));
    GuestAddr chars_written_addr = m.arg_ptr(3);
    // Learn marks lpReserved "must be NULL"; Windows does not enforce it
    // (wc_emoji.exe: reserved=1 → TRUE err=dead).
    (void)m.arg_ptr(4);
    bool ok = cm.write_console_w(handle, buffer, chars_to_write, chars_written_addr, m);
    m.set_return(ok ? 1u : 0u);
}

void impl_ReadConsoleA(Marshaler& m, ConsoleManager& cm) {
    Handle handle            = static_cast<Handle>(m.arg(0));
    GuestAddr buffer         = m.arg_ptr(1);
    u32 chars_to_read        = static_cast<u32>(m.arg(2));
    GuestAddr chars_read_addr = m.arg_ptr(3);
    bool ok = cm.read_console_a(handle, buffer, chars_to_read, chars_read_addr, m);
    m.set_return(ok ? 1u : 0u);
}

void impl_GetConsoleMode(Marshaler& m, ConsoleManager& cm) {
    Handle handle      = static_cast<Handle>(m.arg(0));
    GuestAddr mode_addr = m.arg_ptr(1);
    bool ok = cm.get_console_mode(handle, mode_addr, m);
    m.set_return(ok ? 1u : 0u);
}

void impl_SetConsoleMode(Marshaler& m, ConsoleManager& cm) {
    Handle handle = static_cast<Handle>(m.arg(0));
    u32 mode      = static_cast<u32>(m.arg(1));
    bool ok = cm.set_console_mode(handle, mode);
    m.set_return(ok ? 1u : 0u);
}

void impl_SetConsoleTitleW(Marshaler& m, ConsoleManager& cm) {
    m.set_return(cm.set_console_title_w(m.arg_ptr(0), m) ? 1u : 0u);
}

void impl_GetConsoleTitleW(Marshaler& m, ConsoleManager& cm) {
    m.set_return(cm.get_console_title_w(
        m.arg_ptr(0), static_cast<u32>(m.arg(1)), m));
}

// =============================================================================
// make_console_exports
// =============================================================================

DllExports make_console_exports(ConsoleManager& cm) {
    DllExports exports;
    exports.entries.reserve(10);

    // GetStdHandle (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetStdHandle", 0,
        [&cm](Marshaler& m) { impl_GetStdHandle(m, cm); },
        4
    });

    // WriteConsoleA (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "WriteConsoleA", 0,
        [&cm](Marshaler& m) { impl_WriteConsoleA(m, cm); },
        20
    });

    // WriteConsoleW (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "WriteConsoleW", 0,
        [&cm](Marshaler& m) { impl_WriteConsoleW(m, cm); },
        20
    });

    // ReadConsoleA (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "ReadConsoleA", 0,
        [&cm](Marshaler& m) { impl_ReadConsoleA(m, cm); },
        20
    });

    // GetConsoleMode (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetConsoleMode", 0,
        [&cm](Marshaler& m) { impl_GetConsoleMode(m, cm); },
        8
    });

    // SetConsoleMode (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "SetConsoleMode", 0,
        [&cm](Marshaler& m) { impl_SetConsoleMode(m, cm); },
        8
    });

    // SetConsoleTitleW (1 arg, stdcall_bytes=4). The arity is essential on
    // x86: zero-byte cleanup shifts the caller's nonvolatile register restores
    // by one stack slot.
    // Learn (SetConsoleTitleA / SetConsoleScreenBufferSize /
    // SetConsoleWindowInfo): all three configure a console Vitreo presents but
    // does not render into a resizable window. They report success because the
    // request is accepted and nothing observable contradicts it — a caller that
    // sees failure here typically retries or reports a console error rather
    // than simply continuing.
    exports.entries.push_back(ExportEntry{
        "SetConsoleTitleA", 0,
        [](Marshaler& m) { m.set_return(m.arg_ptr(0) != 0 ? 1u : 0u); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "SetConsoleScreenBufferSize", 0,
        [](Marshaler& m) { m.set_return(1); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "SetConsoleWindowInfo", 0,
        [](Marshaler& m) { m.set_return(1); },
        12
    });

    exports.entries.push_back(ExportEntry{
        "SetConsoleTitleW", 0,
        [&cm](Marshaler& m) { impl_SetConsoleTitleW(m, cm); },
        4
    });

    // GetConsoleTitleW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetConsoleTitleW", 0,
        [&cm](Marshaler& m) { impl_GetConsoleTitleW(m, cm); },
        8
    });

    // GetConsoleScreenBufferInfo (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetConsoleScreenBufferInfo", 0,
        [&cm](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
            // BOOL GetConsoleScreenBufferInfo(HANDLE hConsoleOutput,
            //   PCONSOLE_SCREEN_BUFFER_INFO lpConsoleScreenBufferInfo)
            GuestAddr info = m.arg_ptr(1);
            if (info != 0) {
                // Query real terminal dimensions via ioctl
                u16 cols = 80;
                u16 rows = 25;
                struct winsize ws;
                if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
                    cols = static_cast<u16>(ws.ws_col);
                    rows = static_cast<u16>(ws.ws_row);
                }
                // Scrollback buffer height (capped to i16 max)
                u16 buf_rows = static_cast<u16>(std::min(static_cast<int>(rows) * 50, 32767));

                VITREO_TRACE("[console] GetConsoleScreenBufferInfo: %ux%u (buf %u)\n", cols, rows, buf_rows);

                // CONSOLE_SCREEN_BUFFER_INFO: 22 bytes
                // Keep the buffer model in step with the tty, then report
                // the RETAINED cursor and attributes. Reporting a hardcoded
                // 0,0 cursor and 0x07 attributes made every save/restore
                // sequence read back something the guest had never set.
                cm.set_buffer_size(static_cast<i16>(cols),
                                   static_cast<i16>(buf_rows));
                const auto [cur_x, cur_y] = cm.cursor_position();
                (void)m.write_u16(info + 0,  cols);             // dwSize.X
                (void)m.write_u16(info + 2,  buf_rows);         // dwSize.Y (scrollback)
                (void)m.write_u16(info + 4,  static_cast<u16>(cur_x));
                (void)m.write_u16(info + 6,  static_cast<u16>(cur_y));
                (void)m.write_u16(info + 8,  cm.text_attributes());
                (void)m.write_u16(info + 10, 0);                // srWindow.Left
                (void)m.write_u16(info + 12, 0);                // srWindow.Top
                (void)m.write_u16(info + 14, cols - 1);         // srWindow.Right
                (void)m.write_u16(info + 16, rows - 1);         // srWindow.Bottom
                (void)m.write_u16(info + 18, cols);             // dwMaximumWindowSize.X
                (void)m.write_u16(info + 20, rows);             // dwMaximumWindowSize.Y
            }
            m.set_return(1u); // TRUE
        },
        8
    });

    // GetCurrentConsoleFont (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "GetCurrentConsoleFont", 0,
        [](Marshaler& m) {
            // BOOL GetCurrentConsoleFont(HANDLE hConsoleOutput,
            //   BOOL bMaximumWindow, PCONSOLE_FONT_INFO lpConsoleCurrentFont)
            if (m.arg(0) == 0) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            const GuestAddr info = m.arg_ptr(2);
            if (info == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            // CONSOLE_FONT_INFO { DWORD nFont; COORD dwFontSize; } = 8 bytes.
            // Vitreo renders no console font table, so the only honest values
            // are font index 0 and the classic 8x16 raster cell every Windows
            // console falls back to. `bMaximumWindow` selects the cell size at
            // the largest window; with one fixed cell the answer is the same.
            (void)m.write_u32(info + 0, 0);       // nFont
            (void)m.write_u16(info + 4, 8);       // dwFontSize.X
            (void)m.write_u16(info + 6, 16);      // dwFontSize.Y
            VITREO_TRACE("[console] GetCurrentConsoleFont -> nFont=0 8x16\n");
            m.set_return(1u); // TRUE
        },
        12
    });

    // SetConsoleTextAttribute (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "SetConsoleTextAttribute", 0,
        [&cm](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
            // BOOL SetConsoleTextAttribute(HANDLE hConsoleOutput, WORD wAttributes)
            // Retained, not discarded: GetConsoleScreenBufferInfo reports it
            // back, and a caller that saves the attributes, changes them and
            // restores them (every coloured console logger does) needs the
            // value it reads to be the value it wrote.
            const u16 attr = static_cast<u16>(m.arg(1) & 0xFFFF);
            cm.set_text_attributes(attr);
            // Mirror to a host tty as SGR. The console attribute packs
            // foreground in bits 0-3 and background in 4-7, each as
            // Blue|Green|Red|Intensity; ANSI orders its 3 bits Red|Green|Blue,
            // so the RGB bits have to be transposed rather than passed
            // through.
            auto ansi_of = [](u16 bits) {
                return ((bits & 0x4) >> 2) | (bits & 0x2) | ((bits & 0x1) << 2);
            };
            const u16 fg = attr & 0x7, bg = (attr >> 4) & 0x7;
            const bool fg_bright = (attr & 0x08) != 0;
            const bool bg_bright = (attr & 0x80) != 0;
            char vt[48];
            const int n = std::snprintf(
                vt, sizeof(vt), "\x1b[0;%d;%dm",
                (fg_bright ? 90 : 30) + static_cast<int>(ansi_of(fg)),
                (bg_bright ? 100 : 40) + static_cast<int>(ansi_of(bg)));
            if (n > 0) cm.write_host_output(std::string_view(vt, static_cast<usize>(n)));
            m.set_return(1u);
        },
        8
    });


    // ------------------------------------------------------------------
    // Cursor position / shape
    // ------------------------------------------------------------------

    // SetConsoleCursorPosition(HANDLE, COORD) — COORD is two SHORTs packed
    // into one 32-bit argument slot by value, low half X.
    exports.entries.push_back(ExportEntry{
        "SetConsoleCursorPosition", 0,
        [&cm](Marshaler& m) {
            if (m.arg(0) == 0) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0); return;
            }
            const u32 packed = static_cast<u32>(m.arg(1));
            const i16 x = static_cast<i16>(packed & 0xFFFF);
            const i16 y = static_cast<i16>((packed >> 16) & 0xFFFF);
            const auto [cols, rows] = cm.buffer_size();
            // Windows rejects a coordinate outside the buffer rather than
            // clamping; a caller that positions past the end has miscomputed
            // and must see that.
            if (x < 0 || y < 0 || x >= cols || y >= rows) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0); return;
            }
            cm.set_cursor_position(x, y);
            // Mirror to the host terminal so a real tty follows the guest.
            // CUP is 1-based; the console API is 0-based.
            char vt[32];
            const int n = std::snprintf(vt, sizeof(vt), "\x1b[%d;%dH",
                                        static_cast<int>(y) + 1,
                                        static_cast<int>(x) + 1);
            if (n > 0) cm.write_host_output(std::string_view(vt, static_cast<usize>(n)));
            VITREO_TRACE("[console] SetConsoleCursorPosition(%d,%d)\n", x, y);
            m.set_return(1u);
        },
        8
    });

    // GetConsoleCursorInfo(HANDLE, PCONSOLE_CURSOR_INFO)
    exports.entries.push_back(ExportEntry{
        "GetConsoleCursorInfo", 0,
        [&cm](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(1);
            // Windows 11: a non-console handle (a pipe) or no console fails
            // ERROR_INVALID_HANDLE (process_contract_probe.expected).
            if (!cm.is_console_input(static_cast<Handle>(m.arg(0)))) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0); return;
            }
            if (out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0); return;
            }
            const auto info = cm.cursor_info();
            // CONSOLE_CURSOR_INFO { DWORD dwSize; BOOL bVisible; }
            (void)m.write_u32(out + 0, info.size);
            (void)m.write_u32(out + 4, info.visible ? 1u : 0u);
            m.set_return(1u);
        },
        8
    });

    // SetConsoleCursorInfo(HANDLE, const CONSOLE_CURSOR_INFO*)
    exports.entries.push_back(ExportEntry{
        "SetConsoleCursorInfo", 0,
        [&cm](Marshaler& m) {
            const GuestAddr in = m.arg_ptr(1);
            if (m.arg(0) == 0 || in == 0) {
                set_last_error(m.arg(0) == 0 ? error_codes::ERROR_INVALID_HANDLE
                                             : error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0); return;
            }
            const auto size_r = m.read_u32(in + 0);
            const auto vis_r  = m.read_u32(in + 4);
            ConsoleManager::CursorInfo info{};
            info.size = size_r.is_ok() ? size_r.value() : 25u;
            info.visible = vis_r.is_ok() && vis_r.value() != 0;
            // dwSize is a percentage and must be 1..100; Windows fails
            // outside that range.
            if (info.size < 1 || info.size > 100) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0); return;
            }
            cm.set_cursor_info(info);
            cm.write_host_output(info.visible ? "\x1b[?25h" : "\x1b[?25l");
            VITREO_TRACE("[console] SetConsoleCursorInfo(size=%u visible=%d)\n",
                         info.size, info.visible ? 1 : 0);
            m.set_return(1u);
        },
        8
    });

    // ------------------------------------------------------------------
    // Screen-buffer fills
    // ------------------------------------------------------------------

    // FillConsoleOutputCharacterW(HANDLE, WCHAR, DWORD nLength, COORD,
    //                             LPDWORD lpNumberOfCharsWritten)
    const auto fill_char = [&cm](Marshaler& m, bool wide) {
        if (m.arg(0) == 0) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0); return;
        }
        const u16 ch = static_cast<u16>(m.arg(1) & 0xFFFF);
        const u32 count = static_cast<u32>(m.arg(2));
        const u32 packed = static_cast<u32>(m.arg(3));
        const i16 x = static_cast<i16>(packed & 0xFFFF);
        const i16 y = static_cast<i16>((packed >> 16) & 0xFFFF);
        const GuestAddr written_out = m.arg_ptr(4);
        const u32 written = cm.fill_output_character(x, y, ch, count);
        if (written_out != 0) (void)m.write_u32(written_out, written);
        // Mirror as: position, then erase `written` cells. ECH is the exact
        // analogue of a space fill and leaves the cursor where it was, which
        // is what the console API does. A non-space fill has no single VT
        // equivalent, so write the character out instead.
        if (written != 0) {
            char vt[32];
            int n = std::snprintf(vt, sizeof(vt), "\x1b[%d;%dH",
                                  static_cast<int>(y) + 1,
                                  static_cast<int>(x) + 1);
            if (n > 0) cm.write_host_output(std::string_view(vt, static_cast<usize>(n)));
            if (ch == u' ') {
                n = std::snprintf(vt, sizeof(vt), "\x1b[%uX", written);
                if (n > 0) cm.write_host_output(std::string_view(vt, static_cast<usize>(n)));
            } else if (ch < 0x80) {
                cm.write_host_output(std::string(written, static_cast<char>(ch)));
            }
            // Restore the cursor: a fill does not move it.
            const auto [cx, cy] = cm.cursor_position();
            n = std::snprintf(vt, sizeof(vt), "\x1b[%d;%dH",
                              static_cast<int>(cy) + 1, static_cast<int>(cx) + 1);
            if (n > 0) cm.write_host_output(std::string_view(vt, static_cast<usize>(n)));
        }
        VITREO_TRACE("[console] FillConsoleOutputCharacter%s(ch=0x%04x n=%u at %d,%d) -> %u\n",
                     wide ? "W" : "A", ch, count, x, y, written);
        m.set_return(1u);
    };
    exports.entries.push_back(ExportEntry{
        "FillConsoleOutputCharacterW", 0,
        [fill_char](Marshaler& m) { fill_char(m, true); }, 20 });
    exports.entries.push_back(ExportEntry{
        "FillConsoleOutputCharacterA", 0,
        [fill_char](Marshaler& m) { fill_char(m, false); }, 20 });

    // FillConsoleOutputAttribute(HANDLE, WORD, DWORD, COORD, LPDWORD)
    exports.entries.push_back(ExportEntry{
        "FillConsoleOutputAttribute", 0,
        [&cm](Marshaler& m) {
            if (m.arg(0) == 0) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0); return;
            }
            const u16 attr = static_cast<u16>(m.arg(1) & 0xFFFF);
            const u32 count = static_cast<u32>(m.arg(2));
            const u32 packed = static_cast<u32>(m.arg(3));
            const i16 x = static_cast<i16>(packed & 0xFFFF);
            const i16 y = static_cast<i16>((packed >> 16) & 0xFFFF);
            const GuestAddr written_out = m.arg_ptr(4);
            const u32 written = cm.fill_output_attribute(x, y, attr, count);
            if (written_out != 0) (void)m.write_u32(written_out, written);
            VITREO_TRACE("[console] FillConsoleOutputAttribute(attr=0x%04x n=%u) -> %u\n",
                         attr, count, written);
            m.set_return(1u);
        },
        20
    });

    // ------------------------------------------------------------------
    // Input queue
    // ------------------------------------------------------------------

    exports.entries.push_back(ExportEntry{
        "GetNumberOfConsoleInputEvents", 0,
        [&cm](Marshaler& m) {
            const Handle handle = static_cast<Handle>(m.arg(0));
            const GuestAddr out = m.arg_ptr(1);
            if (!cm.is_console_input(handle) || out == 0) {
                set_last_error(out == 0 ? error_codes::ERROR_INVALID_PARAMETER
                                        : error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0); return;
            }
            (void)m.write_u32(out, cm.pending_input_events());
            m.set_return(1u);
        },
        8
    });

    // INPUT_RECORD is 20 bytes on both bitnesses: WORD EventType, 2 bytes
    // padding, then the union. KEY_EVENT_RECORD is
    // { BOOL bKeyDown; WORD wRepeatCount; WORD wVirtualKeyCode;
    //   WORD wVirtualScanCode; union { WCHAR/CHAR } uChar; DWORD dwControlKeyState }
    constexpr u32 kInputRecordSize = 20;

    // Windows 11 (process_contract_probe.expected): ReadConsoleInput on a
    // pipe fails ERROR_INVALID_FUNCTION and zeroes *lpNumberOfEventsRead;
    // PeekConsoleInput fails ERROR_INVALID_HANDLE. Both fail with
    // ERROR_INVALID_HANDLE when there is no console.
    const auto read_input_records = [&cm](Marshaler& m, bool peek, bool ansi) {
        const Handle handle = static_cast<Handle>(m.arg(0));
        const GuestAddr buf = m.arg_ptr(1);
        const u32 max_records = static_cast<u32>(m.arg(2));
        const GuestAddr read_out = m.arg_ptr(3);
        if (!cm.is_console_input(handle)) {
            if (!peek && read_out != 0) (void)m.write_u32(read_out, 0);
            set_last_error(peek ? error_codes::ERROR_INVALID_HANDLE
                                : cm.read_handle_error(handle));
            m.set_return(0); return;
        }
        if (buf == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0); return;
        }
        std::vector<ConsoleManager::InputRecord> records;
        const u32 n = cm.read_input(records, max_records, peek);
        for (u32 i = 0; i < n; ++i) {
            const GuestAddr rec = buf + static_cast<GuestAddr>(i) * 20u;
            const ConsoleManager::InputRecord& r = records[i];
            (void)m.write_u16(rec + 0,  r.event_type);
            (void)m.write_u16(rec + 2,  0);              // padding
            (void)m.write_u32(rec + 4,  r.key_down ? 1u : 0u);
            (void)m.write_u16(rec + 8,  r.repeat);
            (void)m.write_u16(rec + 10, r.virtual_key);
            (void)m.write_u16(rec + 12, r.virtual_scan);
            if (ansi) {
                // KEY_EVENT_RECORD.uChar.AsciiChar: character in the console's
                // current input code page (GetConsoleCP), low byte of the union.
                u8 ch = 0;
                const u32 cp = cm.input_code_page();
                (void)codepage::wide_to_multi_byte(cp, 0,
                                                   &r.unicode_char, 1, &ch, 1,
                                                   nullptr, false);
                (void)m.write_u16(rec + 14, ch);
            } else {
                (void)m.write_u16(rec + 14, r.unicode_char);
            }
            (void)m.write_u32(rec + 16, r.control_key_state);
        }
        if (read_out != 0) (void)m.write_u32(read_out, n);
        m.set_return(1u);
    };
    exports.entries.push_back(ExportEntry{
        "ReadConsoleInputW", 0,
        [read_input_records](Marshaler& m) { read_input_records(m, false, false); }, 16 });
    exports.entries.push_back(ExportEntry{
        "ReadConsoleInputA", 0,
        [read_input_records](Marshaler& m) { read_input_records(m, false, true); }, 16 });
    exports.entries.push_back(ExportEntry{
        "PeekConsoleInputW", 0,
        [read_input_records](Marshaler& m) { read_input_records(m, true, false); }, 16 });
    exports.entries.push_back(ExportEntry{
        "PeekConsoleInputA", 0,
        [read_input_records](Marshaler& m) { read_input_records(m, true, true); }, 16 });

    const auto write_input_records = [&cm](Marshaler& m) {
        const GuestAddr buf = m.arg_ptr(1);
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr written_out = m.arg_ptr(3);
        if (m.arg(0) == 0 || buf == 0) {
            set_last_error(m.arg(0) == 0 ? error_codes::ERROR_INVALID_HANDLE
                                         : error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0); return;
        }
        std::vector<ConsoleManager::InputRecord> records;
        records.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            const GuestAddr rec = buf + static_cast<GuestAddr>(i) * 20u;
            ConsoleManager::InputRecord r{};
            const auto type_r = m.read_u16(rec + 0);
            r.event_type = type_r.is_ok() ? type_r.value() : 0;
            // Only KEY_EVENT is round-tripped. Queuing a record type the
            // console does not model would hand the reader back a record it
            // cannot interpret, which is worse than dropping it.
            if (r.event_type != 1) continue;
            const auto down_r = m.read_u32(rec + 4);
            r.key_down = down_r.is_ok() && down_r.value() != 0;
            const auto rep_r = m.read_u16(rec + 8);
            r.repeat = rep_r.is_ok() ? rep_r.value() : 1;
            const auto vk_r = m.read_u16(rec + 10);
            r.virtual_key = vk_r.is_ok() ? vk_r.value() : 0;
            const auto vs_r = m.read_u16(rec + 12);
            r.virtual_scan = vs_r.is_ok() ? vs_r.value() : 0;
            const auto ch_r = m.read_u16(rec + 14);
            r.unicode_char = ch_r.is_ok() ? ch_r.value() : 0;
            const auto cks_r = m.read_u32(rec + 16);
            r.control_key_state = cks_r.is_ok() ? cks_r.value() : 0;
            records.push_back(r);
        }
        const u32 n = cm.write_input(records);
        if (written_out != 0) (void)m.write_u32(written_out, n);
        VITREO_TRACE("[console] WriteConsoleInput(%u requested) -> %u queued\n",
                     count, n);
        m.set_return(1u);
    };
    exports.entries.push_back(ExportEntry{
        "WriteConsoleInputW", 0, write_input_records, 16 });
    exports.entries.push_back(ExportEntry{
        "WriteConsoleInputA", 0, write_input_records, 16 });
    (void)kInputRecordSize;


    // ------------------------------------------------------------------
    // ConPTY
    //
    // A pseudoconsole is a console whose screen is a pipe. The creator passes
    // the two pipe ends; a child launched with
    // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE then has that console as its own.
    // ------------------------------------------------------------------

    // HRESULT CreatePseudoConsole(COORD size, HANDLE hInput, HANDLE hOutput,
    //                             DWORD dwFlags, HPCON* phPC)
    //
    // COORD is passed BY VALUE and occupies one argument slot, two SHORTs
    // packed low-half-X. Reading it as two slots would shift every argument
    // after it -- the same class of defect as the SHELLEXECUTEINFOW layout.
    exports.entries.push_back(ExportEntry{
        "CreatePseudoConsole", 0,
        [&cm](Marshaler& m) {
            constexpr u64 kEInvalidArg = 0x80070057ull;   // E_INVALIDARG
            const u32 packed = static_cast<u32>(m.arg(0));
            const i16 cols = static_cast<i16>(packed & 0xFFFF);
            const i16 rows = static_cast<i16>((packed >> 16) & 0xFFFF);
            const Handle hin  = static_cast<Handle>(m.arg(1));
            const Handle hout = static_cast<Handle>(m.arg(2));
            const u32 flags   = static_cast<u32>(m.arg(3));
            const GuestAddr out = m.arg_ptr(4);
            if (out == 0) { m.set_return(kEInvalidArg); return; }

            const Hpcon pcon =
                cm.pseudo_consoles().create(cols, rows, hin, hout, flags);
            if (pcon == NULL_HPCON) {
                VITREO_TRACE("[conpty] CreatePseudoConsole(%dx%d) -> "
                             "E_INVALIDARG\n", cols, rows);
                m.set_return(kEInvalidArg);
                return;
            }
            (void)m.write_ptr(out, pcon);
            VITREO_TRACE("[conpty] CreatePseudoConsole(%dx%d in=0x%x out=0x%x "
                         "flags=0x%x) -> HPCON 0x%llx\n",
                         cols, rows, hin, hout, flags,
                         static_cast<unsigned long long>(pcon));
            m.set_return(0u);   // S_OK
        },
        20
    });

    // HRESULT ResizePseudoConsole(HPCON hPC, COORD size)
    exports.entries.push_back(ExportEntry{
        "ResizePseudoConsole", 0,
        [&cm](Marshaler& m) {
            constexpr u64 kEInvalidArg = 0x80070057ull;
            const Hpcon pcon = static_cast<Hpcon>(m.arg(0));
            const u32 packed = static_cast<u32>(m.arg(1));
            const i16 cols = static_cast<i16>(packed & 0xFFFF);
            const i16 rows = static_cast<i16>((packed >> 16) & 0xFFFF);
            if (!cm.pseudo_consoles().resize(pcon, cols, rows)) {
                VITREO_TRACE("[conpty] ResizePseudoConsole(0x%llx, %dx%d) -> "
                             "E_INVALIDARG\n",
                             static_cast<unsigned long long>(pcon), cols, rows);
                m.set_return(kEInvalidArg);
                return;
            }
            // The console's own buffer follows the pseudoconsole, so a guest
            // that resizes the terminal and then asks for the screen buffer
            // gets the size it just set.
            cm.set_buffer_size(cols, rows);
            VITREO_TRACE("[conpty] ResizePseudoConsole(0x%llx, %dx%d)\n",
                         static_cast<unsigned long long>(pcon), cols, rows);
            m.set_return(0u);
        },
        8
    });

    // void ClosePseudoConsole(HPCON hPC)
    exports.entries.push_back(ExportEntry{
        "ClosePseudoConsole", 0,
        [&cm](Marshaler& m) {
            const Hpcon pcon = static_cast<Hpcon>(m.arg(0));
            const bool closed = cm.pseudo_consoles().close(pcon);
            VITREO_TRACE("[conpty] ClosePseudoConsole(0x%llx) %s\n",
                         static_cast<unsigned long long>(pcon),
                         closed ? "closed" : "(not open)");
            m.set_return(0u);   // void
        },
        4
    });

    return exports;
}

} // namespace vitreo::winapi
