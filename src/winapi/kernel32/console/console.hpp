#pragma once

/// @file console.hpp
/// @brief Console I/O emulation (WIN32-032)
///
/// Implements GetStdHandle, WriteConsoleA/W, ReadConsoleA,
/// GetConsoleMode, and SetConsoleMode for Win32 guest processes.

#include <vitreo/common.hpp>
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/console/pseudo_console.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <utility>
#include <vector>

namespace vitreo::winapi {

class SyncManager;

// =============================================================================
// Standard Handle Constants (matches Windows)
// =============================================================================

constexpr u32 STD_INPUT_HANDLE  = static_cast<u32>(-10);
constexpr u32 STD_OUTPUT_HANDLE = static_cast<u32>(-11);
constexpr u32 STD_ERROR_HANDLE  = static_cast<u32>(-12);

// =============================================================================
// Console Mode Flags (matches Windows)
// =============================================================================

/// Input mode flags
constexpr u32 ENABLE_PROCESSED_INPUT        = 0x0001;
constexpr u32 ENABLE_LINE_INPUT             = 0x0002;
constexpr u32 ENABLE_ECHO_INPUT             = 0x0004;
constexpr u32 ENABLE_WINDOW_INPUT           = 0x0008;
constexpr u32 ENABLE_MOUSE_INPUT            = 0x0010;
constexpr u32 ENABLE_INSERT_MODE            = 0x0020;
constexpr u32 ENABLE_QUICK_EDIT_MODE        = 0x0040;
constexpr u32 ENABLE_EXTENDED_FLAGS         = 0x0080;
constexpr u32 ENABLE_AUTO_POSITION          = 0x0100;
constexpr u32 ENABLE_VIRTUAL_TERMINAL_INPUT = 0x0200;

/// Output mode flags
constexpr u32 ENABLE_PROCESSED_OUTPUT            = 0x0001;
constexpr u32 ENABLE_WRAP_AT_EOL                 = 0x0002;
constexpr u32 ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004;
constexpr u32 DISABLE_NEWLINE_AUTO_RETURN        = 0x0008;
constexpr u32 ENABLE_LVB_GRID_WORLDWIDE          = 0x0010;

/// Learn SetConsoleMode: default input enables all modes except WINDOW_INPUT
/// and VIRTUAL_TERMINAL_INPUT (process_contract_probe.expected mode=1f7).
constexpr u32 DEFAULT_INPUT_MODE =
    ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
    ENABLE_MOUSE_INPUT | ENABLE_INSERT_MODE | ENABLE_QUICK_EDIT_MODE |
    ENABLE_EXTENDED_FLAGS | ENABLE_AUTO_POSITION;
/// Default output mode measured on Windows 11 (process_contract_probe
/// sharedmode: initial out mode=1f). Includes VT processing and the
/// DISABLE_NEWLINE_AUTO_RETURN / LVB bits conhost enables by default.
constexpr u32 DEFAULT_OUTPUT_MODE =
    ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL |
    ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN |
    ENABLE_LVB_GRID_WORLDWIDE;

constexpr u32 kValidInputModeMask =
    ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
    ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_INSERT_MODE |
    ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS | ENABLE_AUTO_POSITION |
    ENABLE_VIRTUAL_TERMINAL_INPUT;
constexpr u32 kValidOutputModeMask =
    ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL |
    ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN |
    ENABLE_LVB_GRID_WORLDWIDE;

// =============================================================================
// ConsoleManager Class
// =============================================================================

/// Manages console I/O for a guest process
///
/// Provides:
/// - Standard handle allocation (stdin, stdout, stderr)
/// - WriteConsoleA / WriteConsoleW output
/// - ReadConsoleA input
/// - Console mode get/set
///
/// For testability, output and input are routed through configurable callbacks.
/// Defaults write to STDOUT_FILENO/STDERR_FILENO and read from STDIN_FILENO.
///
/// Usage:
/// @code
///     HandleTable handles;
///     ConsoleManager cm(handles);
///     Handle h = cm.get_std_handle(STD_OUTPUT_HANDLE);
///     // ... write to console via cm.write_console_a(...)
/// @endcode
class ConsoleManager {
public:
    /// DuplicateHandle of a console input/output handle: a new handle to the
    /// same console object (Learn lists Console input/output among the
    /// duplicable types). Returns NULL_HANDLE (last error set) if `source` is
    /// not one of this console's handles.
    [[nodiscard]] Handle duplicate_handle(Handle source, u32 flags, u32 access);

    /// Callback types for I/O redirection
    using OutputCallback = std::function<void(std::string_view)>;
    using InputCallback  = std::function<std::string()>;

    /// Construct a ConsoleManager
    /// @param handles Reference to the process handle table
    explicit ConsoleManager(HandleTable& handles, bool attached = true);

    /// No copy (contains references)
    ConsoleManager(const ConsoleManager&) = delete;
    ConsoleManager& operator=(const ConsoleManager&) = delete;

    /// No move (contains references)
    ConsoleManager(ConsoleManager&&) = delete;
    ConsoleManager& operator=(ConsoleManager&&) = delete;

    /// Destructor
    ~ConsoleManager() = default;

    // -------------------------------------------------------------------------
    // Callback Configuration
    // -------------------------------------------------------------------------

    /// Set callback for stdout output (default: write to STDOUT_FILENO)
    void set_output_callback(OutputCallback cb);

    /// Set callback for stderr output (default: write to STDERR_FILENO)
    void set_error_callback(OutputCallback cb);

    /// Set callback for stdin input (default: read from STDIN_FILENO)
    void set_input_callback(InputCallback cb);

    /// Wire SyncManager so STD_INPUT_HANDLE is waitable (Learn WaitFor*).
    void set_sync_manager(class SyncManager* sync);

    // -------------------------------------------------------------------------
    // Win32 API Implementations
    // -------------------------------------------------------------------------

    /// GetStdHandle — retrieve a standard device handle
    /// @param n_std_handle One of STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE
    /// @return Corresponding handle, or INVALID_HANDLE for unknown values
    /// GetStdHandle: the handle currently stored in the slot (SetStdHandle
    /// or the STARTUPINFO handles the creator passed), INVALID_HANDLE +
    /// ERROR_INVALID_HANDLE for an unknown nStdHandle or a slot holding
    /// INVALID_HANDLE_VALUE (process_contract_probe.expected).
    [[nodiscard]] Handle get_std_handle(u32 n_std_handle) const;
    /// SetStdHandle: any handle value (NULL and INVALID_HANDLE_VALUE included)
    /// replaces the slot; an unknown nStdHandle fails with ERROR_INVALID_HANDLE.
    bool set_std_handle(u32 n_std_handle, Handle handle);
    /// The value in std slot @p index (0 input, 1 output, 2 error) for host
    /// code: unlike get_std_handle it never touches the guest's last error.
    [[nodiscard]] Handle std_slot(u32 index) const {
        if (index > 2) return NULL_HANDLE;
        std::lock_guard lock(m_std_mutex);
        return m_std_slots[index];
    }
    /// Which console object @p h names: 0 none, 1 console input, 2 output.
    [[nodiscard]] u32 console_handle_role(Handle h) const noexcept;
    /// The console input / output (stdout, stderr) handles of this process's
    /// own console, independent of any redirection; NULL when unattached.
    [[nodiscard]] Handle console_input_handle() const noexcept { return m_stdin_handle; }
    [[nodiscard]] Handle console_output_handle(bool error_stream) const noexcept {
        return error_stream ? m_stderr_handle : m_stdout_handle;
    }

    /// WriteConsoleA — write ANSI text to console output
    /// @param h Console output handle (stdout or stderr)
    /// @param buffer Guest address of the character buffer
    /// @param chars_to_write Number of characters to write
    /// @param chars_written_addr Guest address to receive the number of characters written (u32)
    /// @param m Marshaler for guest memory access
    /// @return true on success, false on failure (sets last error)
    bool write_console_a(Handle h, GuestAddr buffer, u32 chars_to_write,
                         GuestAddr chars_written_addr, Marshaler& m);

    /// WriteConsoleW — write wide (UTF-16LE) text to console output
    /// @param h Console output handle (stdout or stderr)
    /// @param buffer Guest address of the wide character buffer
    /// @param chars_to_write Number of wide characters to write
    /// @param chars_written_addr Guest address to receive the number of characters written (u32)
    /// @param m Marshaler for guest memory access
    /// @return true on success, false on failure (sets last error)
    bool write_console_w(Handle h, GuestAddr buffer, u32 chars_to_write,
                         GuestAddr chars_written_addr, Marshaler& m);

    /// ReadConsoleA — read ANSI text from console input
    /// @param h Console input handle
    /// @param buffer Guest address of the destination buffer
    /// @param chars_to_read Maximum number of characters to read
    /// @param chars_read_addr Guest address to receive the number of characters read (u32)
    /// @param m Marshaler for guest memory access
    /// @return true on success, false on failure (sets last error)
    bool read_console_a(Handle h, GuestAddr buffer, u32 chars_to_read,
                        GuestAddr chars_read_addr, Marshaler& m);

    /// GetConsoleMode — retrieve the current console mode
    /// @param h Console handle
    /// @param mode_addr Guest address to receive the mode flags (u32)
    /// @param m Marshaler for guest memory access
    /// @return true on success, false on failure (sets last error)
    bool get_console_mode(Handle h, GuestAddr mode_addr, Marshaler& m);

    /// SetConsoleMode — set the console mode
    /// @param h Console handle
    /// @param mode New console mode flags
    /// @return true on success, false on failure (sets last error)
    bool set_console_mode(Handle h, u32 mode);

    /// SetConsoleTitleW — retain the console's UTF-16 title.
    bool set_console_title_w(GuestAddr title_addr, Marshaler& m);

    /// GetConsoleTitleW — copy the retained title and return UTF-16 code units
    /// copied, excluding the null terminator.
    u32 get_console_title_w(GuestAddr buffer_addr, u32 buffer_chars,
                            Marshaler& m);

    // ---------------------------------------------------------------
    // Screen buffer  (CONSOLE_SCREEN_BUFFER_INFO and friends)
    //
    // The console used to hold no screen state at all: cursor APIs did not
    // exist, GetConsoleScreenBufferInfo synthesised a fresh answer from the
    // host tty's winsize on every call, and SetConsoleTextAttribute was a
    // no-op returning TRUE. Anything that drives a console -- a progress bar
    // that rewrites a line, an installer, node-pty -- needs the state to
    // actually persist, and needs a fill to be observable afterwards.
    // ---------------------------------------------------------------

    /// One cell of the screen buffer.
    struct Cell {
        u16 ch{u' '};      ///< UTF-16 code unit
        u16 attr{0x07};    ///< FOREGROUND_RED|GREEN|BLUE
    };

    /// CONSOLE_CURSOR_INFO.
    struct CursorInfo {
        u32  size{25};       ///< percent of cell filled, 1..100
        bool visible{true};
    };

    /// Move the cursor. Clamped to the buffer, as Windows does; an
    /// out-of-range coordinate is ERROR_INVALID_PARAMETER there, so the
    /// caller checks the bounds and this only ever receives valid ones.
    void set_cursor_position(i16 x, i16 y);
    [[nodiscard]] std::pair<i16, i16> cursor_position() const;

    [[nodiscard]] CursorInfo cursor_info() const;
    void set_cursor_info(const CursorInfo& info);

    /// Buffer dimensions, in cells.
    [[nodiscard]] std::pair<i16, i16> buffer_size() const;
    void set_buffer_size(i16 cols, i16 rows);

    [[nodiscard]] u16 text_attributes() const;
    void set_text_attributes(u16 attr);

    /// FillConsoleOutputCharacter / FillConsoleOutputAttribute. Fills `count`
    /// cells starting at (x, y), wrapping to following rows exactly as the
    /// console does, and stops at the end of the buffer.
    /// @return cells actually written.
    u32 fill_output_character(i16 x, i16 y, u16 ch, u32 count);
    u32 fill_output_attribute(i16 x, i16 y, u16 attr, u32 count);

    // ---------------------------------------------------------------
    // Input queue  (INPUT_RECORD)
    // ---------------------------------------------------------------

    /// A queued INPUT_RECORD, held in its decoded form. Only the fields the
    /// console APIs below actually round-trip are kept; a record type Vitreo
    /// does not model is never queued rather than queued wrong.
    struct InputRecord {
        u16 event_type{1};      ///< KEY_EVENT
        bool key_down{true};
        u16 repeat{1};
        u16 virtual_key{0};
        u16 virtual_scan{0};
        u16 unicode_char{0};
        u32 control_key_state{0};
    };

    /// Writes `bytes` to a kernel handle (a pipe). Supplied by the runtime,
    /// which owns the FileManager; ConsoleManager must not depend on it.
    using HandleWriter = std::function<bool(Handle, std::string_view)>;
    void set_handle_writer(HandleWriter w) { m_handle_writer = std::move(w); }

    /// Adopt a pseudoconsole as THIS process's console.
    ///
    /// Called in a child launched with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE.
    /// Console output then goes to `output` as VT instead of the host stdout,
    /// which is the whole point: the parent reads the rendered stream from the
    /// other end of that pipe. Passing NULL_HANDLE detaches.
    void attach_pseudo_console(Handle input, Handle output);

    /// True when this process's console is a pseudoconsole.
    [[nodiscard]] bool is_pseudo_console_attached() const noexcept {
        return m_pcon_output != NULL_HANDLE;
    }
    [[nodiscard]] Handle pseudo_console_input() const noexcept {
        return m_pcon_input;
    }

    /// The process-wide ConPTY registry. A pseudoconsole is a console whose
    /// screen is a pipe; see pseudo_console.hpp.
    [[nodiscard]] PseudoConsoleManager& pseudo_consoles() noexcept {
        return m_pseudo_consoles;
    }

    /// Write raw bytes to whatever the console's stdout is wired to, without
    /// touching screen-buffer state. Used to mirror cursor/erase operations to
    /// a host terminal as VT so a real tty follows what the guest believes the
    /// screen looks like.
    void write_host_output(std::string_view bytes);

    /// GetNumberOfConsoleInputEvents.
    [[nodiscard]] u32 pending_input_events() const;

    /// ReadConsoleInput (removes) / PeekConsoleInput (leaves in place).
    /// @return records produced, at most `max_records`.
    u32 read_input(std::vector<InputRecord>& out, u32 max_records, bool peek);

    /// WriteConsoleInput — append to the queue.
    u32 write_input(const std::vector<InputRecord>& records);

    /// ReadConsoleA/W body: up to @p max_units UTF-16 units of typed text
    /// (queued KEY_EVENT key-down characters first, then host input), keeping
    /// the unread remainder for the next read. A control character c < 32
    /// whose bit is set in @p wakeup_mask ends the read after it
    /// (CONSOLE_READCONSOLE_CONTROL.dwCtrlWakeupMask). Empty on host EOF.
    std::vector<u16> read_text(u32 max_units, u32 wakeup_mask);
    /// Host-input readiness probe for the non-blocking paths (Peek /
    /// GetNumberOfConsoleInputEvents). Default: poll(STDIN, 0).
    using InputReadyCallback = std::function<bool()>;
    void set_input_ready_callback(InputReadyCallback cb) { m_input_ready_cb = std::move(cb); }
    [[nodiscard]] bool is_attached() const noexcept { return m_attached; }
    /// GetConsoleCP / SetConsoleCP: the console input code page (default
    /// OEM-US 437; process_contract_probe.expected: 0x1b5).
    [[nodiscard]] u32 input_code_page() const noexcept { return m_input_cp.load(); }
    void set_input_code_page(u32 cp) noexcept { m_input_cp.store(cp); }
    /// SetConsoleCtrlHandler list (most recently added first) and the
    /// "ignore CTRL+C" process attribute. Reset by FreeConsole.
    bool add_ctrl_handler(GuestAddr routine);
    bool remove_ctrl_handler(GuestAddr routine);
    void set_ignore_ctrl_c(bool ignore) noexcept { m_ignore_ctrl_c.store(ignore); }
    [[nodiscard]] bool ignore_ctrl_c() const noexcept { return m_ignore_ctrl_c.load(); }
    [[nodiscard]] std::vector<GuestAddr> ctrl_handlers() const;
    [[nodiscard]] bool is_console_input(Handle h) const noexcept { return is_input_handle(h); }
    [[nodiscard]] bool is_console_output(Handle h) const noexcept { return is_output_handle(h); }
    /// FreeConsole: detach. The console handles stop naming a console
    /// (ERROR_INVALID_HANDLE); the std slots keep their values
    /// (process_contract_probe.expected "console state").
    void detach();

    /// SetConsoleCtrlHandler — register/remove a guest HandlerRoutine, or
    /// (handler==0) set/clear the ignore-Ctrl+C process attribute. Removing a
    /// handler that was never added fails with ERROR_INVALID_PARAMETER
    /// (process_contract_probe.expected "== SetConsoleCtrlHandler").
    [[nodiscard]] bool set_ctrl_handler(GuestAddr handler, bool add);
    /// FreeConsole resets the control-handler list (Learn).
    void clear_ctrl_handlers();

    /// Last error for a console read/write on a handle that is not this
    /// console's: ERROR_INVALID_FUNCTION for a live non-console handle,
    /// ERROR_INVALID_HANDLE otherwise.
    [[nodiscard]] u32 read_handle_error(Handle h) const noexcept;

private:
    HandleTable& m_handles;
    bool m_attached{false};

    mutable std::mutex m_screen_mutex;
    i16 m_buf_cols{80};
    i16 m_buf_rows{25};
    i16 m_cursor_x{0};
    i16 m_cursor_y{0};
    u16 m_attributes{0x07};
    CursorInfo m_cursor_info{};
    std::vector<Cell> m_cells;      ///< m_buf_cols * m_buf_rows, row-major

    PseudoConsoleManager m_pseudo_consoles;

    /// Set in a child whose console IS a pseudoconsole (see
    /// attach_pseudo_console). Output is routed to m_pcon_output instead of
    /// the host callbacks.
    Handle m_pcon_input{NULL_HANDLE};
    Handle m_pcon_output{NULL_HANDLE};
    HandleWriter m_handle_writer;

    mutable std::mutex m_input_mutex;
    std::deque<InputRecord> m_input_queue;
    std::condition_variable m_input_cv;
    std::atomic<u32> m_input_cp{437};
    std::atomic<bool> m_ignore_ctrl_c{false};
    mutable std::mutex m_ctrl_mutex;
    std::vector<GuestAddr> m_ctrl_handlers;   ///< registration order
    std::deque<u16> m_pending_text;   ///< typed text not yet returned by ReadConsole
    std::string m_pending_utf8;       ///< trailing partial UTF-8 sequence from the host
    bool m_host_input_eof{false};
    std::mutex m_host_read_mutex;     ///< serializes host reads
    InputReadyCallback m_input_ready_cb;
    /// Read one chunk of host input (call WITHOUT m_input_mutex) and decode
    /// it to UTF-16; sets m_host_input_eof on end of input.
    std::vector<u16> pull_host_text();
    /// Append records for typed text (key down + key up per unit). Locked.
    void queue_text_records_locked(const std::vector<u16>& text);

    /// Re-fit m_cells to the current dimensions, preserving overlap.
    /// Caller holds m_screen_mutex.
    void reflow_cells_locked(i16 cols, i16 rows);

    /// Pre-allocated standard handles
    Handle m_stdin_handle{NULL_HANDLE};
    Handle m_stdout_handle{NULL_HANDLE};
    Handle m_stderr_handle{NULL_HANDLE};
    mutable std::mutex m_std_mutex;
    /// Current GetStdHandle values, indexed STD_INPUT/OUTPUT/ERROR.
    Handle m_std_slots[3]{NULL_HANDLE, NULL_HANDLE, NULL_HANDLE};

    /// Waitable ntsync event for ConsoleInput (WaitForSingleObject on stdin).
    SyncManager* m_sync_manager{nullptr};
    u64 m_stdin_wait_nt_id{0};
    void note_input_available_locked(bool available);

    /// Per-handle console modes
    std::unordered_map<Handle, u32> m_console_modes;

    /// Duplicated handle -> the standard handle naming the same console
    /// object. Modes and routing are per object, so lookups canonicalise.
    mutable std::mutex m_alias_mutex;
    std::unordered_map<Handle, Handle> m_aliases;
    [[nodiscard]] Handle canonical(Handle h) const;

    /// Learn: STDOUT and STDERR initially name the same active screen buffer,
    /// so SetConsoleMode on either is visible through GetConsoleMode on the
    /// other. Map both (and aliases) onto the stdout mode key.
    [[nodiscard]] Handle mode_key(Handle h) const;

    /// I/O callbacks
    OutputCallback m_output_cb;
    OutputCallback m_error_cb;
    InputCallback  m_input_cb;

    mutable std::mutex m_title_mutex;
    std::vector<u16> m_title;

    /// Check whether a handle is the stdout or stderr handle
    [[nodiscard]] bool is_output_handle(Handle h) const noexcept;
    [[nodiscard]] bool is_input_handle(Handle h) const noexcept;

    /// Route output to the correct callback based on handle
    void dispatch_output(Handle h, std::string_view text);
};

// =============================================================================
// Thunk Implementations
// =============================================================================

/// GetStdHandle thunk
/// HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
void impl_GetStdHandle(Marshaler& m, ConsoleManager& cm);

/// WriteConsoleA thunk
/// BOOL WINAPI WriteConsoleA(HANDLE, const VOID*, DWORD, LPDWORD, LPVOID);
void impl_WriteConsoleA(Marshaler& m, ConsoleManager& cm);

/// WriteConsoleW thunk
/// BOOL WINAPI WriteConsoleW(HANDLE, const VOID*, DWORD, LPDWORD, LPVOID);
void impl_WriteConsoleW(Marshaler& m, ConsoleManager& cm);

/// ReadConsoleA thunk
/// BOOL WINAPI ReadConsoleA(HANDLE, LPVOID, DWORD, LPDWORD, LPVOID);
void impl_ReadConsoleA(Marshaler& m, ConsoleManager& cm);

/// GetConsoleMode thunk
/// BOOL WINAPI GetConsoleMode(HANDLE, LPDWORD);
void impl_GetConsoleMode(Marshaler& m, ConsoleManager& cm);

/// SetConsoleMode thunk
/// BOOL WINAPI SetConsoleMode(HANDLE, DWORD);
void impl_SetConsoleMode(Marshaler& m, ConsoleManager& cm);

/// SetConsoleTitleW thunk
/// BOOL WINAPI SetConsoleTitleW(LPCWSTR);
void impl_SetConsoleTitleW(Marshaler& m, ConsoleManager& cm);

/// GetConsoleTitleW thunk
/// DWORD WINAPI GetConsoleTitleW(LPWSTR, DWORD);
void impl_GetConsoleTitleW(Marshaler& m, ConsoleManager& cm);

/// Create DllExports entries for all console I/O functions
/// @param cm Reference to the ConsoleManager (captured by thunk lambdas)
/// @return DllExports containing the console API entries
DllExports make_console_exports(ConsoleManager& cm);

} // namespace vitreo::winapi
