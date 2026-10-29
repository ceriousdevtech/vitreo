// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: vitreo_sysprog.dll, the host side of the system
// programs the runtime provides as executables (module/system_programs.hpp).

#include "dll_registry_internal.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/module/system_programs.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_customaction.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_service.hpp"
#include "winapi/system/msi/msi_ui.hpp"
#include "winapi/system/sysprog/regsvr32.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace vitreo::cli {

namespace {

/// regsvr32's guest operations, each a call of the guest-visible export.
class GuestRegsvr32Host final : public winapi::sysprog::Regsvr32Host {
public:
    GuestRegsvr32Host(RuntimeContext& ctx, GuestMemory& memory, bool is64) : ctx_(ctx), memory_(memory), is64_(is64) {
        scratch_ = static_cast<GuestAddr>(ctx_.vmm.virtual_alloc(0, kScratch, 0x3000, 0x04));
    }
    ~GuestRegsvr32Host() override {
        if (scratch_ != 0) (void)ctx_.vmm.virtual_free_internal(scratch_);
    }

    [[nodiscard]] bool usable() const { return scratch_ != 0 && static_cast<bool>(ctx_.system_program_call); }

    void ole_initialize() override { (void)call("ole32.dll", "OleInitialize", {0}); }
    void ole_uninitialize() override { (void)call("ole32.dll", "OleUninitialize", {}); }
    u64 load_library(const std::u16string& path) override {
        constexpr u64 LOAD_WITH_ALTERED_SEARCH_PATH = 0x8;
        const Mark mark(*this);
        return call("kernel32.dll", "LoadLibraryExW", {wide(path), 0, LOAD_WITH_ALTERED_SEARCH_PATH});
    }
    u64 get_proc_address(u64 module, const char* name) override {
        const Mark mark(*this);
        return call("kernel32.dll", "GetProcAddress", {module, ansi(name)});
    }
    void free_library(u64 module) override { (void)call("kernel32.dll", "FreeLibrary", {module}); }
    u32 call_register(u64 entry) override { return static_cast<u32>(invoke(entry, {})); }
    u32 call_install(u64 entry, bool install, const std::u16string& command_line) override {
        const Mark mark(*this);
        return static_cast<u32>(invoke(entry, {install ? 1u : 0u, wide(command_line)}));
    }
    [[nodiscard]] bool is_64bit() const override { return is64_; }
    u16 image_machine(const std::u16string& path) override {
        // CreateFileW(GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE,
        // OPEN_EXISTING); the DOS header's e_lfanew, then "PE\0\0" and the
        // IMAGE_FILE_HEADER Machine that follows it.
        constexpr u64 GENERIC_READ = 0x80000000ull, SHARE_RW = 0x3, OPEN_EXISTING = 3, FILE_BEGIN = 0;
        const Mark mark(*this);
        const GuestAddr name = wide(path);
        const GuestAddr buf = take(64);
        const GuestAddr got = take(4);
        if (name == 0 || buf == 0 || got == 0) return 0;
        const u64 file = call("kernel32.dll", "CreateFileW", {name, GENERIC_READ, SHARE_RW, 0, OPEN_EXISTING, 0, 0});
        if (file == 0 || file == (is64_ ? ~0ull : 0xFFFFFFFFull)) return 0;
        auto read = [&](u32 offset, u32 bytes) {
            return (call("kernel32.dll", "SetFilePointer", {file, offset, 0, FILE_BEGIN}) & 0xFFFFFFFFull) == offset &&
                   call("kernel32.dll", "ReadFile", {file, buf, bytes, got, 0}) != 0 &&
                   memory_.read_u32(got).value_or(0) == bytes;
        };
        u16 machine = 0;
        if (read(0, 64) && memory_.read_u16(buf).value_or(0) == 0x5A4D /* "MZ" */) {
            const u32 lfanew = memory_.read_u32(buf + 0x3C).value_or(0);
            if (read(lfanew, 6) && memory_.read_u32(buf).value_or(0) == 0x00004550 /* "PE\0\0" */) {
                machine = memory_.read_u16(buf + 4).value_or(0);
            }
        }
        (void)call("kernel32.dll", "CloseHandle", {file});
        return machine;
    }
    std::optional<u32> run_other_regsvr32(const std::u16string& command_line) override {
        constexpr u32 kMaxPath = 260;
        constexpr u64 INFINITE = 0xFFFFFFFF;
        const u32 si_size = is64_ ? 104 : 68;  // sizeof(STARTUPINFOW)
        const u32 pi_size = is64_ ? 24 : 16;   // sizeof(PROCESS_INFORMATION)
        const Mark mark(*this);
        const GuestAddr dir = take(kMaxPath * 2);
        if (dir == 0) return std::nullopt;
        const u64 len = call("kernel32.dll", "GetWindowsDirectoryW", {dir, kMaxPath});
        if (len == 0 || len >= kMaxPath) return std::nullopt;
        std::u16string app;
        for (u64 i = 0; i < len; ++i) app.push_back(static_cast<char16_t>(memory_.read_u16(dir + i * 2).value_or(0)));
        // From a 32-bit process System32 is redirected: Sysnative names the
        // native directory (WOW64 file-system redirection).
        app += is64_ ? u"\\SysWOW64\\regsvr32.exe" : u"\\Sysnative\\regsvr32.exe";
        const GuestAddr app_at = wide(app);
        const GuestAddr cmd_at = wide(command_line);  // CreateProcessW may write to it
        const GuestAddr si = take(si_size);
        const GuestAddr pi = take(pi_size);
        const GuestAddr code = take(4);
        if (app_at == 0 || cmd_at == 0 || si == 0 || pi == 0 || code == 0) return std::nullopt;
        for (u32 i = 0; i < si_size; i += 4) (void)memory_.write_u32(si + i, 0);
        (void)memory_.write_u32(si, si_size);
        if (call("kernel32.dll", "CreateProcessW", {app_at, cmd_at, 0, 0, 0, 0, 0, 0, si, pi}) == 0) return std::nullopt;
        const u64 process = read_ptr(pi);
        (void)call("kernel32.dll", "CloseHandle", {read_ptr(pi + (is64_ ? 8 : 4))});
        (void)call("kernel32.dll", "WaitForSingleObject", {process, INFINITE});
        std::optional<u32> exit_code;
        if (call("kernel32.dll", "GetExitCodeProcess", {process, code}) != 0) exit_code = memory_.read_u32(code).value_or(0);
        (void)call("kernel32.dll", "CloseHandle", {process});
        return exit_code;
    }

private:
    static constexpr u32 kScratch = 0x10000;

    /// Returns the scratch taken during one operation when it ends.
    class Mark {
    public:
        explicit Mark(GuestRegsvr32Host& host) : host_(host), cursor_(host.cursor_) {}
        ~Mark() { host_.cursor_ = cursor_; }
        Mark(const Mark&) = delete;
        Mark& operator=(const Mark&) = delete;

    private:
        GuestRegsvr32Host& host_;
        u32 cursor_;
    };

    [[nodiscard]] u64 read_ptr(GuestAddr at) const {
        return is64_ ? memory_.read_u64(at).value_or(0) : memory_.read_u32(at).value_or(0);
    }
    u64 invoke(u64 fn, std::vector<u64> args) {
        const u64 r = ctx_.system_program_call(static_cast<GuestAddr>(fn), args.data(), args.size(), is64_);
        return is64_ ? r : (r & 0xFFFFFFFFull);
    }
    u64 call(const char* dll, const char* name, std::vector<u64> args) {
        auto fn = ctx_.thunk_registry.get_thunk_address(dll, name);
        if (fn.is_err()) return 0;
        return invoke(fn.value(), std::move(args));
    }
    GuestAddr wide(const std::u16string& text) {
        const GuestAddr at = take(static_cast<u32>((text.size() + 1) * 2));
        for (usize i = 0; at != 0 && i <= text.size(); ++i) {
            (void)memory_.write_u16(at + i * 2, i < text.size() ? static_cast<u16>(text[i]) : 0);
        }
        return at;
    }
    GuestAddr ansi(const char* text) {
        const std::string s(text);
        const GuestAddr at = take(static_cast<u32>(s.size() + 1));
        for (usize i = 0; at != 0 && i <= s.size(); ++i) {
            (void)memory_.write_u8(at + i, i < s.size() ? static_cast<u8>(s[i]) : 0);
        }
        return at;
    }
    GuestAddr take(u32 bytes) {
        const u32 aligned = (bytes + 15u) & ~15u;
        if (scratch_ == 0 || cursor_ + aligned > kScratch) return 0;
        const GuestAddr at = scratch_ + cursor_;
        cursor_ += aligned;
        return at;
    }

    RuntimeContext& ctx_;
    GuestMemory& memory_;
    bool is64_;
    GuestAddr scratch_ = 0;
    u32 cursor_ = 0;
};

/// msiexec.exe's custom-action host (msi_customaction.hpp): every operation
/// is a call of the guest-visible kernel32 export, made from whichever guest
/// thread runs the engine or a custom action (nested MsiDoAction calls come
/// from custom-action threads). Each operation takes its own guest buffer,
/// so concurrent and nested calls never share scratch memory.
class GuestMsiexecHost final : public winapi::msi::CustomActionProcess {
public:
    GuestMsiexecHost(RuntimeContext& ctx, GuestMemory& memory, bool is64) : ctx_(ctx), memory_(memory), is64_(is64) {}

    [[nodiscard]] bool usable() const { return static_cast<bool>(ctx_.system_program_call); }
    [[nodiscard]] bool is_64bit() const override { return is64_; }

    u64 load_library_from_bytes(const std::vector<u8>& bytes, std::u16string& temp_path) override {
        if (!write_temp_file(bytes, {}, temp_path)) return 0;
        return load_library(temp_path);
    }
    u64 load_library(const std::u16string& path) override {
        constexpr u64 LOAD_WITH_ALTERED_SEARCH_PATH = 0x8;
        Scratch s(*this, bytes_of(path));
        return call("kernel32.dll", "LoadLibraryExW", {s.wide(path), 0, LOAD_WITH_ALTERED_SEARCH_PATH});
    }
    u64 get_proc_address(u64 module, const std::string& name) override {
        Scratch s(*this, static_cast<u32>(name.size() + 1));
        return call("kernel32.dll", "GetProcAddress", {module, s.ansi(name)});
    }
    void free_library(u64 module) override { (void)call("kernel32.dll", "FreeLibrary", {module}); }
    u32 call_dll_entry(u64 entry, u32 install_handle) override {
        const u64 thread = start_dll_entry(entry, install_handle);
        return thread != 0 ? wait_and_close(thread) : winapi::MSI_ERROR_INSTALL_FAILURE;
    }
    bool run_process(const std::u16string& application, const std::u16string& command_line,
                     const std::u16string& directory, bool wait, u32& exit_code) override {
        const u64 process = start_process(application, command_line, directory);
        if (process == 0) return false;
        exit_code = 0;
        if (wait) exit_code = wait_and_close(process);
        else close_handle(process);
        return true;
    }

    bool write_temp_file(const std::vector<u8>& bytes, const std::u16string& directory,
                         std::u16string& path) override {
        constexpr u32 kMaxPath = 260;
        Scratch s(*this, 4 * kMaxPath * 2 + 16 + static_cast<u32>(bytes.size()));
        GuestAddr dir = 0;
        if (directory.empty()) {
            dir = s.take(kMaxPath * 2);
            if (dir == 0 || call("kernel32.dll", "GetTempPathW", {kMaxPath, dir}) == 0) return false;
        } else {
            dir = s.wide(directory);
        }
        const GuestAddr name = s.take(kMaxPath * 2);
        if (name == 0 || call("kernel32.dll", "GetTempFileNameW", {dir, s.wide(u"MSI"), 0, name}) == 0) return false;
        path = s.read_wide(name);
        constexpr u64 GENERIC_WRITE = 0x40000000, CREATE_ALWAYS = 2, FILE_ATTRIBUTE_NORMAL = 0x80;
        const u64 file = call("kernel32.dll", "CreateFileW", {name, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0});
        if (file == invalid_handle()) return false;
        const GuestAddr data = s.take(static_cast<u32>(bytes.size()));
        const GuestAddr written = s.take(4);
        bool ok = data != 0 && written != 0 &&
                  memory_.write_bytes(data, reinterpret_cast<const Byte*>(bytes.data()), bytes.size()).is_ok() &&
                  call("kernel32.dll", "WriteFile", {file, data, bytes.size(), written, 0}) != 0 &&
                  memory_.read_u32(written).value_or(0) == bytes.size();
        (void)call("kernel32.dll", "CloseHandle", {file});
        if (!ok) delete_file(path);
        return ok;
    }
    void delete_file(const std::u16string& path) override {
        Scratch s(*this, bytes_of(path));
        (void)call("kernel32.dll", "DeleteFileW", {s.wide(path)});
    }
    std::optional<u16> image_machine(const std::u16string& path) override {
        constexpr u32 kHead = 0x1000;
        Scratch s(*this, bytes_of(path) + kHead + 16);
        constexpr u64 GENERIC_READ = 0x80000000, FILE_SHARE_READ = 1, OPEN_EXISTING = 3;
        const u64 file = call("kernel32.dll", "CreateFileW", {s.wide(path), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0});
        if (file == invalid_handle()) return std::nullopt;
        const GuestAddr data = s.take(kHead);
        const GuestAddr got = s.take(4);
        const bool ok = data != 0 && got != 0 && call("kernel32.dll", "ReadFile", {file, data, kHead, got, 0}) != 0;
        (void)call("kernel32.dll", "CloseHandle", {file});
        if (!ok) return std::nullopt;
        std::vector<u8> head(memory_.read_u32(got).value_or(0));
        if (head.empty() || !memory_.read_bytes(data, reinterpret_cast<Byte*>(head.data()), head.size()).is_ok()) {
            return std::nullopt;
        }
        if (head.size() < 0x40 || head[0] != 'M' || head[1] != 'Z') return std::nullopt;
        const u32 nt = head[0x3C] | (head[0x3D] << 8) | (head[0x3E] << 16) | (static_cast<u32>(head[0x3F]) << 24);
        if (nt > head.size() - 6 || head[nt] != 'P' || head[nt + 1] != 'E') return std::nullopt;
        return static_cast<u16>(head[nt + 4] | (head[nt + 5] << 8));
    }
    u64 start_dll_entry(u64 entry, u32 install_handle) override {
        // `UINT __stdcall fn(MSIHANDLE)` has LPTHREAD_START_ROUTINE's shape on
        // both ABIs; the custom action runs on its own thread as on Windows.
        const u64 thread = call("kernel32.dll", "CreateThread", {0, 0, entry, install_handle, 0, 0});
        if (thread != 0) remember(thread, false);
        return thread;
    }
    u64 start_process(const std::u16string& application, const std::u16string& command_line,
                      const std::u16string& directory) override {
        const u32 si_size = is64_ ? 104 : 68;   // sizeof(STARTUPINFOW)
        const u32 pi_size = is64_ ? 24 : 16;    // sizeof(PROCESS_INFORMATION)
        Scratch s(*this, bytes_of(application) + bytes_of(command_line) + bytes_of(directory) + si_size + pi_size + 64);
        const GuestAddr si = s.take(si_size);
        const GuestAddr pi = s.take(pi_size);
        if (si == 0 || pi == 0) return 0;
        (void)memory_.write_u32(si, si_size);
        const GuestAddr app = application.empty() ? 0 : s.wide(application);
        const GuestAddr cmd = s.wide(command_line);   // CreateProcessW may write to it
        const GuestAddr cwd = directory.empty() ? 0 : s.wide(directory);
        if (call("kernel32.dll", "CreateProcessW", {app, cmd, 0, 0, 0, 0, 0, cwd, si, pi}) == 0) return 0;
        const u64 process = read_ptr(pi);
        (void)call("kernel32.dll", "CloseHandle", {read_ptr(pi + (is64_ ? 8 : 4))});
        remember(process, true);
        return process;
    }
    u32 wait_and_close(u64 handle) override {
        // The engine thread is also the installer UI's thread here (on
        // Windows the custom action runs in another process and the client UI
        // thread stays free), so it waits pumping its message queue:
        // MsiProcessMessage from the custom action may drive windows this
        // thread owns.
        constexpr u64 INFINITE = 0xFFFFFFFF, QS_ALLINPUT = 0x04FF, PM_REMOVE = 1;
        constexpr u64 WAIT_OBJECT_0 = 0;
        {
            const u32 msg_size = is64_ ? 48 : 28;  // sizeof(MSG)
            Scratch s(*this, 8 + msg_size);
            const GuestAddr handles = s.take(8);
            const GuestAddr msg = s.take(msg_size);
            if (handles != 0 && msg != 0 && has("user32.dll", "MsgWaitForMultipleObjects") &&
                has("user32.dll", "PeekMessageW") && (is64_ ? memory_.write_u64(handles, handle).is_ok() : memory_.write_u32(handles, static_cast<u32>(handle)).is_ok())) {
                for (;;) {
                    const u64 r = call("user32.dll", "MsgWaitForMultipleObjects", {1, handles, 0, INFINITE, QS_ALLINPUT});
                    if (r != WAIT_OBJECT_0 + 1) break;  // signaled, or failed
                    while (call("user32.dll", "PeekMessageW", {msg, 0, 0, 0, PM_REMOVE}) != 0) {
                        (void)call("user32.dll", "TranslateMessage", {msg});
                        (void)call("user32.dll", "DispatchMessageW", {msg});
                    }
                }
            } else {
                (void)call("kernel32.dll", "WaitForSingleObject", {handle, INFINITE});
            }
        }
        bool process = false;
        {
            std::lock_guard lock(mutex_);
            process = processes_.erase(handle) != 0;
        }
        Scratch s(*this, 4);
        const GuestAddr code = s.take(4);
        u32 exit_code = winapi::MSI_ERROR_INSTALL_FAILURE;
        if (code != 0 && call("kernel32.dll", process ? "GetExitCodeProcess" : "GetExitCodeThread", {handle, code}) != 0) {
            exit_code = memory_.read_u32(code).value_or(exit_code);
        }
        (void)call("kernel32.dll", "CloseHandle", {handle});
        return exit_code;
    }
    GuestMemory& guest_memory() override { return memory_; }
    GuestAddr alloc_scratch(u32 bytes) override {
        constexpr u32 kCommitReserve = 0x3000, kReadWrite = 0x04;
        return static_cast<GuestAddr>(ctx_.vmm.virtual_alloc(0, bytes, kCommitReserve, kReadWrite));
    }
    void free_scratch(GuestAddr block) override {
        if (block != 0) (void)ctx_.vmm.virtual_free_internal(block);
    }
    bool start_custom_action_server(bool x64, const std::u16string& token) override {
        // Learn "64-Bit Custom Actions": the 64-bit server is System32's
        // msiexec, the 32-bit one SysWOW64's. A 32-bit process names the
        // native directory Sysnative ("File System Redirector"); SysWOW64 is
        // never redirected, so neither depends on the calling thread's
        // Wow64DisableWow64FsRedirection state.
        const std::u16string windows = windows_directory();
        if (windows.empty()) return false;
        const char16_t* dir = x64 ? (is64_ ? u"\\System32" : u"\\Sysnative") : u"\\SysWOW64";
        const std::u16string app = windows + dir + u"\\msiexec.exe";
        const u64 process = start_process(app, u"\"" + app + u"\" -Embedding " + token, {});
        if (process == 0) return false;
        close_handle(process);
        return true;
    }
    void pump_messages() override {
        if (!has("user32.dll", "PeekMessageW")) return;
        constexpr u64 PM_REMOVE = 1;
        const u32 msg_size = is64_ ? 48 : 28;  // sizeof(MSG)
        Scratch s(*this, msg_size);
        const GuestAddr msg = s.take(msg_size);
        if (msg == 0) return;
        while (call("user32.dll", "PeekMessageW", {msg, 0, 0, 0, PM_REMOVE}) != 0) {
            (void)call("user32.dll", "TranslateMessage", {msg});
            (void)call("user32.dll", "DispatchMessageW", {msg});
        }
    }

    // -- the installer service (msiexec /V): advapi32 service-process calls --

    /// StartServiceCtrlDispatcherW with one SERVICE_TABLE_ENTRYW; blocks
    /// until the service stops. 0 on success, else GetLastError.
    u32 run_service_dispatcher(const std::u16string& name, GuestAddr service_main) {
        const u32 ptr = is64_ ? 8 : 4;
        Scratch s(*this, bytes_of(name) + 4 * ptr + 16);
        const GuestAddr table = s.take(4 * ptr);
        const GuestAddr wname = s.wide(name);
        if (table == 0 || wname == 0) return 8;  // ERROR_NOT_ENOUGH_MEMORY
        write_ptr(table, wname);
        write_ptr(table + ptr, service_main);
        write_ptr(table + 2 * ptr, 0);
        write_ptr(table + 3 * ptr, 0);
        if (call("advapi32.dll", "StartServiceCtrlDispatcherW", {table}) != 0) return 0;
        return static_cast<u32>(call("kernel32.dll", "GetLastError", {}));
    }
    /// RegisterServiceCtrlHandlerExW.
    u64 register_service_handler(const std::u16string& name, GuestAddr handler) {
        Scratch s(*this, bytes_of(name));
        return call("advapi32.dll", "RegisterServiceCtrlHandlerExW", {s.wide(name), handler, 0});
    }
    /// SetServiceStatus with a SERVICE_STATUS (seven DWORDs on both ABIs).
    void set_service_status(u64 handle, u32 state, u32 accepted, u32 exit_code = 0) {
        Scratch s(*this, 28);
        const GuestAddr st = s.take(28);
        if (st == 0) return;
        const u32 fields[7] = {0x10 /* SERVICE_WIN32_OWN_PROCESS */, state, accepted, exit_code, 0, 0, 0};
        for (u32 i = 0; i < 7; ++i) (void)memory_.write_u32(st + i * 4, fields[i]);
        (void)call("advapi32.dll", "SetServiceStatus", {handle, st});
    }
    /// CreateMutexW(NULL, TRUE, name): owned by the calling thread.
    u64 create_owned_mutex(const std::u16string& name) {
        Scratch s(*this, bytes_of(name));
        return call("kernel32.dll", "CreateMutexW", {0, 1, s.wide(name)});
    }
    void release_mutex(u64 mutex) {
        (void)call("kernel32.dll", "ReleaseMutex", {mutex});
        (void)call("kernel32.dll", "CloseHandle", {mutex});
    }

    /// GetWindowsDirectoryW.
    std::u16string windows_directory() {
        constexpr u32 kMaxPath = 260;
        Scratch s(*this, kMaxPath * 2);
        const GuestAddr buf = s.take(kMaxPath * 2);
        if (buf == 0 || call("kernel32.dll", "GetWindowsDirectoryW", {buf, kMaxPath}) == 0) return {};
        return s.read_wide(buf);
    }
    void close_handle(u64 handle) override {
        {
            std::lock_guard lock(mutex_);
            processes_.erase(handle);
        }
        (void)call("kernel32.dll", "CloseHandle", {handle});
    }

private:
    /// One operation's guest buffer (VirtualAlloc'd, freed when it ends).
    class Scratch {
    public:
        Scratch(GuestMsiexecHost& host, u32 bytes) : host_(host) {
            size_ = (bytes + 0x1000 + 0xFFF) & ~0xFFFu;
            base_ = static_cast<GuestAddr>(host_.ctx_.vmm.virtual_alloc(0, size_, 0x3000, 0x04));
        }
        ~Scratch() {
            if (base_ != 0) (void)host_.ctx_.vmm.virtual_free_internal(base_);
        }
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;
        GuestAddr take(u32 bytes) {
            const u32 aligned = (bytes + 15u) & ~15u;
            if (base_ == 0 || cursor_ + aligned > size_) return 0;
            const GuestAddr at = base_ + cursor_;
            cursor_ += aligned;
            return at;
        }
        GuestAddr wide(const std::u16string& text) {
            const GuestAddr at = take(static_cast<u32>((text.size() + 1) * 2));
            for (usize i = 0; at != 0 && i <= text.size(); ++i) {
                (void)host_.memory_.write_u16(at + i * 2, i < text.size() ? static_cast<u16>(text[i]) : 0);
            }
            return at;
        }
        GuestAddr ansi(const std::string& text) {
            const GuestAddr at = take(static_cast<u32>(text.size() + 1));
            for (usize i = 0; at != 0 && i <= text.size(); ++i) {
                (void)host_.memory_.write_u8(at + i, i < text.size() ? static_cast<u8>(text[i]) : 0);
            }
            return at;
        }
        std::u16string read_wide(GuestAddr at) const {
            std::u16string out;
            for (usize i = 0; i < 32768; ++i) {
                const u16 c = host_.memory_.read_u16(at + i * 2).value_or(0);
                if (c == 0) break;
                out.push_back(static_cast<char16_t>(c));
            }
            return out;
        }

    private:
        GuestMsiexecHost& host_;
        GuestAddr base_ = 0;
        u32 size_ = 0;
        u32 cursor_ = 0;
    };

    static u32 bytes_of(const std::u16string& text) { return static_cast<u32>((text.size() + 1) * 2); }
    [[nodiscard]] u64 invalid_handle() const { return is64_ ? ~0ull : 0xFFFFFFFFull; }
    [[nodiscard]] u64 read_ptr(GuestAddr at) const {
        return is64_ ? memory_.read_u64(at).value_or(0) : memory_.read_u32(at).value_or(0);
    }
    void write_ptr(GuestAddr at, u64 value) {
        if (is64_) (void)memory_.write_u64(at, value);
        else (void)memory_.write_u32(at, static_cast<u32>(value));
    }
    void remember(u64 handle, bool process) {
        std::lock_guard lock(mutex_);
        if (process) processes_.insert(handle);
    }
    [[nodiscard]] bool has(const char* dll, const char* name) {
        return ctx_.thunk_registry.get_thunk_address(dll, name).is_ok();
    }
    u64 call(const char* dll, const char* name, std::vector<u64> args) {
        auto fn = ctx_.thunk_registry.get_thunk_address(dll, name);
        if (fn.is_err()) return 0;
        u64 r = ctx_.system_program_call(static_cast<GuestAddr>(fn.value()), args.data(), args.size(), is64_);
        return is64_ ? r : (r & 0xFFFFFFFFull);
    }

    RuntimeContext& ctx_;
    GuestMemory& memory_;
    bool is64_;
    std::mutex mutex_;
    std::set<u64> processes_;
};

[[nodiscard]] bool same_name(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

[[nodiscard]] std::u16string to_u16(std::string_view utf8) {
    const auto units = winapi::utf8_to_utf16(utf8);
    return std::u16string(units.begin(), units.end());
}

[[nodiscard]] std::string leaf_of(std::string_view path) {
    const auto sep = path.find_last_of("\\/");
    return std::string(sep == std::string_view::npos ? path : path.substr(sep + 1));
}

/// The whitespace-separated arguments of a command line after the program
/// name (quotes group; enough for msiexec's -Embedding / /V forms).
std::vector<std::string> command_arguments(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (const char c : line) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (any) out.push_back(cur);
            cur.clear();
            any = false;
            continue;
        }
        cur.push_back(c);
        any = true;
    }
    if (any) out.push_back(cur);
    if (!out.empty()) out.erase(out.begin());
    return out;
}

/// The internal exports the installer service registers with the SCM.
constexpr std::string_view kMsiServiceMainExport = "VitreoMsiServiceMain";
constexpr std::string_view kMsiServiceHandlerExport = "VitreoMsiServiceHandler";
std::atomic<bool> g_msi_service_stop{false};
std::unique_ptr<GuestMsiexecHost> g_msi_process_host;

/// msiexec.exe's window operations for the internal UI (msi_ui.hpp
/// UiGuestApi): calls of the guest-visible user32 / gdi32 / kernel32
/// exports, with scratch memory from one runtime arena handed out and
/// returned in stack order.
class GuestUiApi final : public winapi::msi::UiGuestApi {
public:
    GuestUiApi(RuntimeContext& ctx, GuestMemory& memory, bool is64) : ctx_(ctx), memory_(memory), is64_(is64) {
        arena_ = static_cast<GuestAddr>(ctx_.vmm.virtual_alloc_internal(kArena, 0x04));
        auto proc = ctx_.thunk_registry.get_thunk_address(winapi::kSystemProgramDll, winapi::msi::kUiWindowProcExport, true);
        proc_ = proc.is_ok() ? proc.value() : 0;
    }
    ~GuestUiApi() override {
        if (arena_ != 0) (void)ctx_.vmm.virtual_free_internal(arena_);
    }
    [[nodiscard]] bool usable() const { return arena_ != 0 && proc_ != 0 && static_cast<bool>(ctx_.system_program_call); }

    u64 call(std::string_view dll, std::string_view function, const std::vector<u64>& args) override {
        auto fn = ctx_.thunk_registry.get_thunk_address(dll, function);
        if (fn.is_err()) return 0;
        std::vector<u64> a = args;
        const u64 r = ctx_.system_program_call(static_cast<GuestAddr>(fn.value()), a.data(), a.size(), is64_);
        return is64_ ? r : (r & 0xFFFFFFFFull);
    }
    GuestAddr alloc(u32 bytes) override {
        std::lock_guard lock(mutex_);
        const u32 size = (bytes + 15u) & ~15u;
        if (size == 0 || used_ + size > kArena) return 0;
        const GuestAddr at = arena_ + used_;
        used_ += size;
        blocks_.push_back({at, size, false});
        std::vector<u8> zero(size, 0);
        (void)memory_.write_bytes(at, reinterpret_cast<const Byte*>(zero.data()), zero.size());
        return at;
    }
    void release(GuestAddr address) override {
        std::lock_guard lock(mutex_);
        for (auto& b : blocks_)
            if (b.at == address) b.freed = true;
        while (!blocks_.empty() && blocks_.back().freed) {
            used_ -= blocks_.back().size;
            blocks_.pop_back();
        }
    }
    GuestMemory& memory() override { return memory_; }
    [[nodiscard]] bool is_64bit() const override { return is64_; }
    [[nodiscard]] GuestAddr window_proc() const override { return proc_; }

private:
    static constexpr u32 kArena = 0x100000;
    struct Block {
        GuestAddr at;
        u32 size;
        bool freed;
    };
    RuntimeContext& ctx_;
    GuestMemory& memory_;
    bool is64_;
    GuestAddr arena_{0};
    GuestAddr proc_{0};
    u32 used_{0};
    std::vector<Block> blocks_;
    std::mutex mutex_;
};

}  // namespace

void install_msi_process_host(RuntimeContext& ctx, GuestMemory& memory, bool is_64bit) {
    // Every process can be a Windows Installer client (MsiInstallProduct)
    // and start custom-action servers; msiexec.exe is also the service and
    // the servers themselves.
    g_msi_process_host = std::make_unique<GuestMsiexecHost>(ctx, memory, is_64bit);
    winapi::msi::set_process_custom_action_host(g_msi_process_host.get());
    winapi::msi::set_installer_service_connector(ctx.msi_mgr, winapi::msi::scm_service_connector());
}

void register_system_program_dll(RuntimeContext& ctx) {
    winapi::DllExports exports;
    // DWORD RunSystemProgram(void): the exit code of the program this process
    // was started as (the stub executable's entry point calls ExitProcess
    // with it).
    exports.entries.push_back(winapi::ExportEntry{
        std::string(winapi::kSystemProgramEntry), 0,
        [&ctx](winapi::Marshaler& m) {
            const std::string image = leaf_of(ctx.module_mgr.main_module_path());
            if (same_name(image, "msiexec.exe")) {
                auto* host = g_msi_process_host.get();
                if (host == nullptr || !host->usable()) {
                    m.set_return(winapi::MSI_ERROR_INSTALL_FAILURE);
                    return;
                }
                const std::string command_line = ctx.env_mgr.get_command_line_w_utf8();
                const std::vector<std::string> args = command_arguments(command_line);
                if (args.size() >= 2 && (same_name(args[0], "-Embedding") || same_name(args[0], "/Embedding"))) {
                    // A custom-action server (msi_remote.hpp): DLL custom
                    // actions of this process's bitness run here.
                    m.set_return(winapi::msi::run_custom_action_server(ctx.msi_mgr, args[1], *host));
                    return;
                }
                if (args.size() == 1 && (same_name(args[0], "/V") || same_name(args[0], "-V"))) {
                    // The Windows Installer service (msi_service.hpp): the
                    // SCM starts it as "msiserver"; run directly it fails to
                    // reach the service controller, as on Windows.
                    auto main = ctx.thunk_registry.get_thunk_address(winapi::kSystemProgramDll, kMsiServiceMainExport, true);
                    m.set_return(main.is_ok() ? host->run_service_dispatcher(u"msiserver", main.value())
                                              : winapi::MSI_ERROR_INSTALL_FAILURE);
                    return;
                }
                // A client (msiexec /i, /x, /f, /a, /j): the internal UI's
                // dialogs are this process's windows; the install runs in
                // the service.
                auto ui_api = std::make_shared<GuestUiApi>(ctx, m.memory(), m.cpu_is_64bit());
                if (ui_api->usable())
                    winapi::msi::install_internal_ui(ctx.msi_mgr, winapi::msi::make_user32_window_system(ui_api));
                const u32 code = ctx.msi_mgr.run_msiexec(command_line);
                winapi::msi::install_internal_ui(ctx.msi_mgr, nullptr);
                m.set_return(code);
                return;
            }
            if (same_name(image, "regsvr32.exe")) {
                GuestRegsvr32Host host(ctx, m.memory(), m.cpu_is_64bit());
                if (!host.usable()) {
                    m.set_return(winapi::sysprog::REGSVR32_FAIL_ARGS);
                    return;
                }
                std::vector<std::u16string> argv;
                const std::u16string command_line = to_u16(ctx.env_mgr.get_command_line_w_utf8());
                ctx.shell_mgr.command_line_to_argv(command_line, to_u16(ctx.module_mgr.main_module_path()), argv);
                m.set_return(winapi::sysprog::run_regsvr32(command_line, argv, host));
                return;
            }
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s: system program '%s'\n",
                         std::string(winapi::kSystemProgramDll).c_str(), image.c_str());
            m.set_return(1);
        },
        0});
    // The installer service's ServiceMain: VOID WINAPI (DWORD, LPWSTR*), 8
    // bytes on x86. Runs on the service thread the dispatcher creates until
    // the service stops (msi_service.hpp run_installer_service).
    winapi::ExportEntry service_main{std::string(kMsiServiceMainExport), 0, [&ctx](winapi::Marshaler& m) {
        GuestMsiexecHost* host = g_msi_process_host.get();
        auto handler = ctx.thunk_registry.get_thunk_address(winapi::kSystemProgramDll, kMsiServiceHandlerExport, true);
        if (host == nullptr || handler.is_err()) { m.set_return(0); return; }
        constexpr u32 kStopped = 1, kStartPending = 2, kRunning = 4;
        constexpr u32 kAcceptStop = 0x1, kAcceptShutdown = 0x4;
        const u64 status = host->register_service_handler(u"msiserver", handler.value());
        if (status == 0) { m.set_return(0); return; }
        host->set_service_status(status, kStartPending, 0);
        u64 execute_mutex = 0;
        // Windows stops an idle installer service after a while; ten minutes
        // without a client (open point, .docs/contracts/msi/process.md).
        constexpr u32 kIdleMs = 10 * 60 * 1000;
        winapi::msi::run_installer_service(
            ctx.msi_mgr, [&] { host->set_service_status(status, kRunning, kAcceptStop | kAcceptShutdown); },
            [] { return g_msi_service_stop.load(); },
            [&](bool begin) {
                // Learn "_MSIExecute Mutex": held while an installation runs.
                if (begin) execute_mutex = host->create_owned_mutex(u"Global\\_MSIExecute");
                else if (execute_mutex != 0) { host->release_mutex(execute_mutex); execute_mutex = 0; }
            },
            kIdleMs);
        host->set_service_status(status, kStopped, 0);
        m.set_return(0);
    }, 8};
    service_main.internal = true;
    exports.entries.push_back(std::move(service_main));
    // DWORD WINAPI HandlerEx(DWORD control, DWORD type, LPVOID data, LPVOID
    // context), 16 bytes on x86.
    winapi::ExportEntry service_handler{std::string(kMsiServiceHandlerExport), 0, [](winapi::Marshaler& m) {
        constexpr u32 kStop = 1, kInterrogate = 4, kShutdown = 5;
        const u32 control = m.arg32(0);
        if (control == kStop || control == kShutdown) {
            g_msi_service_stop.store(true);
            m.set_return(0);
        } else {
            m.set_return(control == kInterrogate ? 0u : 120u /* ERROR_CALL_NOT_IMPLEMENTED */);
        }
    }, 16};
    service_handler.internal = true;
    exports.entries.push_back(std::move(service_handler));
    // The internal UI's window procedure (msi_ui.hpp kUiWindowProcExport):
    // LRESULT CALLBACK (HWND, UINT, WPARAM, LPARAM), 16 bytes on x86.
    winapi::ExportEntry ui_proc{std::string(winapi::msi::kUiWindowProcExport), 0,
                                [](winapi::Marshaler& m) { winapi::msi::ui_window_proc(m); }, 16};
    ui_proc.internal = true;
    exports.entries.push_back(std::move(ui_proc));
    (void)ctx.thunk_registry.register_dll(winapi::kSystemProgramDll, std::move(exports));
}

}  // namespace vitreo::cli
