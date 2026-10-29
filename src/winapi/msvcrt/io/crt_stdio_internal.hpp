#pragma once
/// @file crt_stdio_internal.hpp
/// @brief Private shared surface of msvcrt's stdio implementation.
///
/// These were file-local (an anonymous namespace) while the whole export table
/// lived in crt_stdio_registration.cpp. The table is split across several
/// translation units now, so the state they share is declared here, in
/// vitreo::winapi::crt_stdio_internal. Private to msvcrt's stdio: not an API.
///
/// Three things live here:
///  - the stdio lock domain (one table lock plus per-descriptor locks);
///  - the FILE stream, standard-descriptor and low-io descriptor state, and
///    the per-DLL arena that owns it;
///  - the stream primitives every export family reads and writes through.
///
/// The lock classes, the host-call wrapper and the arena accessors stay inline
/// so the fgetc/ungetc fast path costs what it did when they were file-local;
/// the thread_locals are constinit for the same reason.

#include "winapi/kernel32/handle.hpp"
#include "winapi/msvcrt/io/crt_stdio.hpp"
#include "winapi/msvcrt/io/crt_stdio_buffer.hpp"
#include "capsule/vfs/vfs.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {
class CrtMemoryManager;
class FileManager;
class Marshaler;
}

namespace vitreo::winapi::crt_stdio_internal {

// ---- lock domain -----------------------------------------------------------

// Standard descriptors keep their own lock while external I/O releases all
// nested table-lock levels. Lock order is descriptor, then shared tables.
extern std::recursive_mutex s_stdio_state_mutex;
std::recursive_mutex& standard_descriptor_mutex(i32 index);
std::recursive_mutex& lowio_descriptor_mutex(i32 index);
extern constinit thread_local unsigned s_stdio_lock_depth;

/// Descriptor-table size of the CRT flavor being executed.
i32 lowio_limit();

class StdioStateLock {
public:
    StdioStateLock() { s_stdio_state_mutex.lock(); ++s_stdio_lock_depth; }
    ~StdioStateLock() { --s_stdio_lock_depth; s_stdio_state_mutex.unlock(); }
    StdioStateLock(const StdioStateLock&) = delete;
    StdioStateLock& operator=(const StdioStateLock&) = delete;
};

class StdioStateSuspension {
public:
    StdioStateSuspension() : m_depth(s_stdio_lock_depth) {
        s_stdio_lock_depth = 0;
        for (unsigned i = 0; i < m_depth; ++i) s_stdio_state_mutex.unlock();
    }
    ~StdioStateSuspension() {
        for (unsigned i = 0; i < m_depth; ++i) s_stdio_state_mutex.lock();
        s_stdio_lock_depth = m_depth;
    }
    StdioStateSuspension(const StdioStateSuspension&) = delete;
    StdioStateSuspension& operator=(const StdioStateSuspension&) = delete;
private:
    unsigned m_depth;
};

template <typename Operation>
decltype(auto) stdio_host_call(Operation&& operation) {
    StdioStateSuspension unlocked;
    return std::forward<Operation>(operation)();
}

std::unique_lock<std::recursive_mutex> lock_standard_descriptor(i32 index);

std::unique_lock<std::recursive_mutex> lock_lowio_descriptor(i32 index);

// ---- FILE streams ----------------------------------------------------------

/// A VFS-backed CRT stream, tracked by its guest-visible UCRT FILE object. The
/// object lives in mapped CRT memory because _get_stream_buffer_pointers
/// exposes addresses of three cells inside it.
///
/// The VFS handle is deliberately *not* named `handle`: read-ahead leaves the
/// VFS position past the stream's logical position, so anything that reads,
/// writes, seeks or reports the position must go through `stream_handle`.
/// Naming the raw field separately makes the compiler reject a direct use.
struct VfsFile {
    capsule::FileHandle raw_handle{};
    crt_stdio_buffer::Layout layout{
        crt_stdio_buffer::Layout::LegacyIobuf32};
    bool text_mode{false};
    /// fopen `, ccs=` / `_setmode(_O_U8TEXT/_O_U16TEXT)` stream encoding.
    /// Default text is ACP (CP_ACP); binary leaves encoding None.
    enum class Encoding : u8 { None = 0, Acp, Utf8, Utf16Le };
    Encoding encoding{Encoding::None};
    /// "a"/"a+": Learn fopen -- every write happens at end-of-file; the
    /// pointer "is always moved back to the end of the file before any write
    /// operation" (09_crt.md HIGH 13).
    bool append{false};
    bool pending_cr{false};
    std::vector<u8> read_ahead;
    usize read_ahead_pos{0};
    bool has_read_character{false};
    bool pushback_valid{false};
    u8 pushback_character{0};
    /// C's EOF indicator: set only when a READ actually hits end-of-file, and
    /// cleared by fseek/rewind/clearerr. feof() used to probe by reading a byte
    /// instead, so it reported EOF purely from POSITION -- fseek(f,0,SEEK_END)
    /// then feof() returned 1 where Windows returns 0, which ends a read loop
    /// one iteration early.
    bool eof_flag{false};
    /// C's error indicator (ferror); set on I/O failure / wrong direction and
    /// cleared by clearerr/rewind/successful fseek (Learn ferror/clearerr).
    bool error_flag{false};
    /// fopen access mode so fwrite/fgetc can refuse the wrong direction.
    capsule::AccessMode access{capsule::AccessMode::Read};
    /// Optional binary-mode buffer and synchronization sidecar inside the
    /// synthetic 32-bit FILE.  Text streams retain the host path because a
    /// CRLF-translated buffer needs a raw/logical offset map for exact seeks.
    /// When >= 0, this stream wraps host stdin/stdout/stderr (_fdopen of 0..2).
    i32 host_std_fd{-1};
    GuestMemory* guest_memory{nullptr};
    u32 guest_stream{0};
    u32 guest_buffer{0};
};

u32* guest_stream_cell(VfsFile& vf, GuestAddr offset);

class GuestStreamLock {
public:
    explicit GuestStreamLock(VfsFile& vf) : m_vf(vf) {
        u32* cell = guest_stream_cell(vf, crt_stdio_buffer::kLock);
        if (cell == nullptr || vf.guest_buffer == 0) return;
        std::atomic_ref<u32> lock(*cell);
        u32 expected = 0;
        usize spins = 0;
        while (!lock.compare_exchange_weak(
            expected, 0xFFFFFFFFu, std::memory_order_acquire,
            std::memory_order_relaxed)) {
            expected = 0;
            if ((++spins & 0x3FFu) == 0) std::this_thread::yield();
        }
        m_held = true;
    }

    GuestStreamLock(const GuestStreamLock&) = delete;
    GuestStreamLock& operator=(const GuestStreamLock&) = delete;

    ~GuestStreamLock() { release(); }

    void release() {
        if (!m_held) return;
        u32* cell = guest_stream_cell(m_vf, crt_stdio_buffer::kLock);
        if (cell != nullptr) {
            std::atomic_ref<u32>(*cell).store(0, std::memory_order_release);
        }
        m_held = false;
    }

private:
    VfsFile& m_vf;
    bool m_held{false};
};

void release_guest_stream_lock(VfsFile& vf);

usize pending_guest_buffer(VfsFile& vf);

void clear_guest_buffer_cells(VfsFile& vf);

/// VITREO_STDIO_DIAG=1: print the guest-visible FILE cells (no-ops otherwise).
void stdio_diag_cells(Marshaler& m, const char* who, GuestAddr stream,
                      GuestAddr ptr_cell, GuestAddr count_cell,
                      GuestAddr base_cell, u64 buffer, u64 capacity);

void stdio_diag_vfs(Marshaler& m, const char* who, u32 stream, VfsFile& vf);

/// Raw bytes already fetched from the VFS but not yet handed to the guest.
usize pending_read_ahead(const VfsFile& vf);

/// Drop any read-ahead, rewind the VFS position to the stream's logical
/// position, and return the handle for direct use.
capsule::FileHandle stream_handle(capsule::Vfs& vfs, VfsFile& vf);

/// The handle to WRITE through: stream_handle() plus the append-mode rule
/// that every write lands at end-of-file.
capsule::FileHandle stream_write_handle(capsule::Vfs& vfs, VfsFile& vf);

/// Block-buffered raw read for a stream.
usize stream_raw_read(capsule::Vfs& vfs, VfsFile& vf, std::span<u8> dst);

/// Consume bytes already staged in the guest-visible FILE buffer.
usize drain_guest_character_buffer(VfsFile& vf, std::span<u8> dst);

// ---- descriptors 0-2 and low-io descriptors --------------------------------

enum class StdKind : u8 { Unresolved, Console, Kernel, None, Closed };

// _setmode values; any other recorded mode is _O_WTEXT (0x10000), which is
// how the CRT records _O_U16TEXT too.
constexpr i32 kStdModeText    = 0x4000;   // _O_TEXT
constexpr i32 kStdModeBinary  = 0x8000;   // _O_BINARY
constexpr i32 kStdModeU8Text  = 0x40000;  // _O_U8TEXT

struct StdDescriptor {
    StdKind kind{StdKind::Unresolved};
    Handle handle{0};                    ///< the std HANDLE (Kernel kind)
    i32 translation_mode{kStdModeText};  ///< _setmode; text by default
    FILE* stream{nullptr};               ///< host FILE* over `handle`
    GuestAddr scratch{0};                ///< guest buffer ReadFile fills
    u8 lookahead{0};                     ///< byte read past a trailing CR
    bool lookahead_valid{false};
    FileManager* files{nullptr};
    CrtMemoryManager* memory{nullptr};
    i32 index{0};
    bool text_eof{false};                ///< Ctrl+Z seen in text mode
    std::optional<u16> unicode_lookahead; ///< a wide unit peeked on a pipe
    /// No handle: stream output the next fflush must fail on, and the
    /// stream's error indicator once it has.
    bool pending_output{false};
    bool stream_error{false};
    bool file_closed{false};             ///< fclose, independently of _close
};

struct LowIoFd {
    capsule::FileHandle handle{};
    bool text_mode{false};
    bool append{false};   ///< _O_APPEND: seek End before every write
    bool owns_handle{true};
    u32  aliased_file{0};   ///< the FILE* this fd came from, 0 if none
    /// Last `_setmode` / `_open` translation mode (`_O_TEXT`/`_O_BINARY`/
    /// `_O_U8TEXT`/`_O_U16TEXT`/`_O_WTEXT`).
    i32  translation_mode{0x4000};  // _O_TEXT
    bool temporary{false};  ///< `_O_TEMPORARY`: delete path on `_close`
    std::string delete_path;  ///< guest path for temporary delete
    bool pending_cr{false}; ///< text-mode: CR was last byte of prior read
    bool text_eof{false};   ///< text-mode: Ctrl+Z seen → further reads 0
    u8   pushback{0};       ///< one-byte pushback after CR peek
    bool pushback_valid{false};
    std::optional<u16> unicode_lookahead;
    bool noinherit{false};  ///< `_O_NOINHERIT`
    bool short_lived{false}; ///< `_O_SHORT_LIVED` (accepted; caching hint)
    /// The kernel32 HANDLE behind this descriptor (0 until one exists).
    /// A Windows descriptor is a thin layer over a HANDLE: _get_osfhandle
    /// returns it, _open_osfhandle starts from one, and _close releases it
    /// (Learn). Descriptors from _open get theirs on first request.
    Handle os_handle{0};
    /// A pipe or device from _open_osfhandle: no VFS file, so reads and
    /// writes go through kernel32 on `os_handle`.
    bool kernel_io{false};
    /// Guest path and access the descriptor was opened with, so the
    /// kernel32 handle it gets carries the same identity CreateFile gives.
    std::string path;
    u32 win32_access{0x80000000u | 0x40000000u};  // GENERIC_READ|WRITE
};

struct StdioArena {
    std::unordered_map<u32, VfsFile> files;
    std::unordered_map<i32, LowIoFd> descriptors;
    CrtStdioManager::GuestFgetc get_character;
    CrtStdioManager::GuestUngetc unget_character;
    std::array<StdDescriptor, 3> standard;
    std::array<std::recursive_mutex, 3> standard_mutexes;
    std::unordered_map<i32, std::unique_ptr<std::recursive_mutex>> lowio_mutexes;
    u64 owner{0};
    FileManager* file_manager{nullptr};
    CrtMemoryManager* memory_manager{nullptr};
    ~StdioArena() {
        for (auto& descriptor : standard)
            if (descriptor.stream != nullptr)
                (void)stdio_host_call([&] { return ::fclose(descriptor.stream); });
    }
};
extern constinit thread_local StdioArena* current_stdio_arena;
class StdioArenaScope {
public:
    explicit StdioArenaScope(StdioArena& arena) : previous(current_stdio_arena) {
        current_stdio_arena = &arena;
    }
    ~StdioArenaScope() { current_stdio_arena = previous; }
private:
    StdioArena* previous;
};
inline StdioArena& stdio_arena() { return *current_stdio_arena; }

/// Bytes a descriptor in @p mode puts on its HANDLE for @p in.
std::optional<std::vector<u8>> lowio_encode(i32 mode, std::span<const u8> in);

/// Classify descriptor @p index from the process's std handle.
StdDescriptor& std_descriptor(CrtStdioManager& mgr, i32 index);

/// _read on a Kernel descriptor.
i64 std_read(StdDescriptor& d, u8* dst, u32 count);

/// The host FILE* a standard stream writes and reads through.
FILE* std_host_file(CrtStdioManager& mgr, i32 index);

/// ferror for a standard stream; Console streams keep reporting no error.
bool std_stream_error(const StdDescriptor& d);

} // namespace vitreo::winapi::crt_stdio_internal
