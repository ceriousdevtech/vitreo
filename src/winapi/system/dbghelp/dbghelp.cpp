// SPDX-License-Identifier: LGPL-2.1-only

/// @file dbghelp.cpp
/// @brief dbghelp.dll exports (symbols, stack walking, minidumps).
///
/// Moved out of the CLI's DLL registry: dbghelp is a Windows DLL like any
/// other, so it belongs beside the modules it is contract-equivalent to, where
/// its failure codes can be covered by a focused test.

#include "winapi/system/dbghelp/dbghelp.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/system/dbghelp/msvc_demangle.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <mutex>
#include <string>

namespace vitreo::winapi {

namespace {

// ---------------------------------------------------------------------------
// Symbol handler state: SymInitialize opens a session per process handle and
// records its search path; SymCleanup ends it (tools/oracle_probe/
// dbghelp_searchpath_probe.expected).
// ---------------------------------------------------------------------------
struct SymbolSessions {
    std::mutex mutex;
    std::map<u64, std::u16string> search_path; ///< keyed by process handle
    std::map<u64, bool> invaded;               ///< SymInitialize fInvadeProcess
    std::map<u64, std::set<u64>> loaded;       ///< SymLoadModule bases
    std::function<std::optional<std::string>(std::string_view)> env;
    DbghelpModuleLookup modules;
    /// SymSetOptions/SymGetOptions: process-global, not per session (Learn);
    /// measured initial value SYMOPT_UNDNAME (2).
    u32 options{2};
};

SymbolSessions& sym_sessions() {
    static auto* sessions = new SymbolSessions();
    return *sessions;
}

u64 process_key(u64 handle, bool is64) {
    // GetCurrentProcess() is -1 at the guest's pointer width.
    return is64 ? handle : (handle & 0xFFFFFFFFull);
}

/// Default path when the caller passes none: "." plus _NT_SYMBOL_PATH when
/// it is set (probe get_default / get_env).
std::u16string default_search_path() {
    std::u16string path = u".";
    auto& s = sym_sessions();
    if (s.env) {
        if (auto v = s.env("_NT_SYMBOL_PATH"); v && !v->empty()) {
            path += u';';
            for (unsigned char c : *v) path.push_back(static_cast<char16_t>(c));
        }
    }
    return path;
}

std::u16string read_guest_text(Marshaler& m, GuestAddr p, bool wide) {
    std::u16string out;
    for (GuestAddr a = p; p != 0 && out.size() < 32768; a += wide ? 2 : 1) {
        const u16 ch = wide ? m.read_u16(a).value_or(0) : m.read_u8(a).value_or(0);
        if (ch == 0) break;
        out.push_back(static_cast<char16_t>(ch));
    }
    return out;
}

void sym_initialize(Marshaler& m, bool wide) {
    const u64 key = process_key(m.arg(0), m.cpu_is_64bit());
    const GuestAddr user_path = m.arg_ptr(1);
    std::u16string path = user_path != 0 ? read_guest_text(m, user_path, wide)
                                         : default_search_path();
    const bool invade = m.arg32(2) != 0;
    std::lock_guard lock(sym_sessions().mutex);
    auto& s = sym_sessions();
    // Measured (tools/oracle_probe/s7_dbghelp_probe.expected): a second
    // SymInitialize for the same process succeeds but leaves
    // ERROR_INVALID_HANDLE; fInvadeProcess leaves ERROR_SUCCESS; a
    // non-invading default-path init leaves ERROR_ENVVAR_NOT_FOUND from the
    // missing _NT_SYMBOL_PATH lookup.
    const bool again = s.search_path.contains(key);
    s.search_path[key] = std::move(path);
    // fInvadeProcess: "enumerates the loaded modules for the process" into
    // the handler, so address lookups cover them.
    s.invaded[key] = invade;
    s.loaded[key].clear();
    if (again) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
    } else if (invade) {
        set_last_error(error_codes::ERROR_SUCCESS);
    } else if (user_path == 0 && !(s.env && s.env("_NT_SYMBOL_PATH"))) {
        set_last_error(error_codes::ERROR_ENVVAR_NOT_FOUND);
    }
    m.set_return(1);
}

void sym_set_search_path(Marshaler& m, bool wide) {
    const u64 key = process_key(m.arg(0), m.cpu_is_64bit());
    const GuestAddr p = m.arg_ptr(1);
    std::u16string path = p != 0 ? read_guest_text(m, p, wide) : default_search_path();
    std::lock_guard lock(sym_sessions().mutex);
    auto it = sym_sessions().search_path.find(key);
    if (it == sym_sessions().search_path.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    it->second = std::move(path);
    m.set_return(1);
}

void sym_get_search_path(Marshaler& m, bool wide) {
    const u64 key = process_key(m.arg(0), m.cpu_is_64bit());
    const GuestAddr buf = m.arg_ptr(1);
    const u32 size = m.arg32(2);
    std::u16string path;
    {
        std::lock_guard lock(sym_sessions().mutex);
        auto it = sym_sessions().search_path.find(key);
        if (it == sym_sessions().search_path.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        path = it->second;
    }
    // A buffer that cannot hold the path receives an empty string and the
    // call still succeeds (probe get_short5).
    if (buf != 0 && size != 0) {
        if (path.size() + 1 > size) {
            if (wide) (void)m.write_u16(buf, 0); else (void)m.write_u8(buf, 0);
        } else {
            for (usize i = 0; i < path.size(); ++i) {
                if (wide) (void)m.write_u16(buf + i * 2, path[i]);
                else (void)m.write_u8(buf + i, static_cast<u8>(path[i]));
            }
            if (wide) (void)m.write_u16(buf + path.size() * 2, 0);
            else (void)m.write_u8(buf + path.size(), 0);
        }
    }
    set_last_error(error_codes::ERROR_SUCCESS); // probe: gle=0 on success
    m.set_return(1);
}

/// The minidump pair, shared by dbghelp.dll and dbgcore.dll.
///
/// Since Windows 8.1 these two live in **dbgcore.dll**, and dbghelp.dll
/// exports them as forwarders to it (confirmed by reading the export table of
/// a real C:\Windows\System32\dbgcore.dll: exactly two named exports,
/// MiniDumpWriteDump and MiniDumpReadDumpStream, and nothing else). A guest
/// may therefore reach either name through either DLL, so one definition feeds
/// both tables and they cannot drift apart.
void add_minidump_exports(DllExports& db) {
    // BOOL MiniDumpWriteDump(HANDLE hProcess, DWORD ProcessId, HANDLE hFile,
    //   MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
    //   PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION)
    // x86 stdcall: 4 + 4 + 4 + 4 + 4 + 4 + 4 = 28.
    //
    // Vitreo writes no minidump. FALSE is the honest answer, and the caller
    // has normally created the destination file already — which is why a
    // crash directory fills up with zero-byte .dmp files. What was missing is
    // the error code: Learn says to call GetLastError on failure, and a
    // reporter that reads zero there cannot tell a failed dump from a written
    // one. ERROR_CALL_NOT_IMPLEMENTED is what Windows reports for an export
    // that exists but is not implemented on this platform.
    //
    // Emitting a synthetic MINIDUMP_HEADER instead would be worse than
    // failing: every consumer treats a well-formed dump as a genuine crash
    // record, so a header with no thread, module, or memory streams would
    // turn a visible gap into silently corrupt crash reports.
    db.entries.push_back({"MiniDumpWriteDump", 0,
        [](Marshaler& m){
            // Learn: on failure "the last error will be an HRESULT value".
            // A bad hFile is HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE)
            // (measured, s7_dbghelp_probe); otherwise the dump cannot be
            // produced: HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED).
            const u64 file = m.arg(2);
            std::function<bool(u64)> is_open;
            {
                std::lock_guard lock(sym_sessions().mutex);
                is_open = sym_sessions().modules.is_open_handle;
            }
            if (is_open && !is_open(m.cpu_is_64bit() ? file : (file & 0xFFFFFFFFull))) {
                set_last_error(0x80070006u);
                m.set_return(0);
                return;
            }
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED dbghelp!MiniDumpWriteDump: "
                                     "minidump streams\n");
            });
            set_last_error(0x80070078u);
            m.set_return(0);
        }, 28});
    // BOOL MiniDumpReadDumpStream(PVOID BaseOfDump, ULONG StreamNumber,
    //   PMINIDUMP_DIRECTORY* Dir, PVOID* StreamPointer, ULONG* StreamSize)
    // x86 stdcall: 4 + 4 + 4 + 4 + 4 = 20.
    //
    // This one reads a dump the CALLER mapped, so it does not depend on
    // Vitreo having written anything -- but Vitreo parses no minidump format,
    // so it fails. Learn: FALSE on failure with GetLastError set. The out
    // parameters are cleared first, because a caller that ignores the return
    // value would otherwise dereference whatever the stack held.
    db.entries.push_back({"MiniDumpReadDumpStream", 0,
        [](Marshaler& m){
            (void)m.arg(0);
            (void)m.arg(1);
            for (u32 i = 2; i < 5; ++i) {
                const GuestAddr out = m.arg_ptr(i);
                if (out != 0) (void)m.write_ptr(out, 0);
            }
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            m.set_return(0);
        }, 20});
}

}  // namespace

DllExports create_dbgcore_thunks() {
    // dbgcore.dll is exactly these two exports and nothing else -- measured
    // from the real DLL, not assumed. UE loads it on its exit/crash-report
    // path right after dbghelp, and a LoadLibrary that returns NULL there
    // makes the reporter abandon setup.
    DllExports db;
    add_minidump_exports(db);
    return db;
}

void set_dbghelp_module_lookup(DbghelpModuleLookup lookup) {
    std::lock_guard lock(sym_sessions().mutex);
    sym_sessions().modules = std::move(lookup);
}

namespace {
/// Base of the module holding @p address when that module is known to the
/// session for @p key (invaded process or SymLoadModule); 0 otherwise, with
/// the last error set as Windows' symbol handler does.
/// The base of the session's module containing @p address; nullopt when
/// there is no session (ERROR_INVALID_HANDLE, set here), 0 when no module the
/// session knows covers it (the caller picks the error: measured 0 /
/// ERROR_SUCCESS for SymGetModuleBase64, ERROR_MOD_NOT_FOUND for symbol
/// lookups, ERROR_INVALID_ADDRESS for SymFunctionTableAccess64).
std::optional<u64> session_module_base(u64 key, u64 address) {
    auto& s = sym_sessions();
    std::lock_guard lock(s.mutex);
    if (!s.search_path.contains(key)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return std::nullopt;
    }
    const u64 base = s.modules.module_base ? s.modules.module_base(address) : 0;
    const bool known = base != 0 && (s.invaded[key] || s.loaded[key].contains(base));
    return known ? base : 0;
}

void report_no_symbols(const char* api) {
    static std::mutex once_mutex;
    static std::set<std::string> reported;
    std::lock_guard lock(once_mutex);
    if (reported.insert(api).second) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED dbghelp!%s: symbol lookup "
                             "(no PDB or export-table symbols)\n", api);
    }
}

/// SymLoadModule(64): register a module the process has loaded at
/// @p base. Windows returns the base, or 0 with ERROR_SUCCESS when the module
/// is already loaded (Learn SymLoadModule64).
u64 sym_load_module(u64 key, u64 base, bool named) {
    auto& s = sym_sessions();
    std::lock_guard lock(s.mutex);
    if (!s.search_path.contains(key)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return 0;
    }
    const u64 actual = s.modules.module_base ? s.modules.module_base(base) : 0;
    // Measured: a module the session already covers, or a base with no
    // image file/name to load symbols from, is 0 with ERROR_SUCCESS.
    if (s.invaded[key] || !named || (actual == base && s.loaded[key].contains(base))) {
        set_last_error(error_codes::ERROR_SUCCESS);
        return 0;
    }
    if (base == 0 || actual != base) {
        // Loading an image file that is not mapped in the process would need
        // a PE reader for the symbol handler; not built.
        report_no_symbols("SymLoadModule64");
        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
        return 0;
    }
    s.loaded[key].insert(base);
    return base;
}
} // namespace

void set_dbghelp_environment_lookup(
    std::function<std::optional<std::string>(std::string_view)> lookup) {
    std::lock_guard lock(sym_sessions().mutex);
    sym_sessions().env = std::move(lookup);
}

DllExports create_dbghelp_thunks() {
    DllExports db;
    db.entries.push_back({"SymInitialize", 0,
        [](Marshaler& m){ sym_initialize(m, false); }, 12});
    db.entries.push_back({"SymInitializeW", 0,
        [](Marshaler& m){ sym_initialize(m, true); }, 12});
    db.entries.push_back({"SymCleanup", 0,
        [](Marshaler& m){
            const u64 key = process_key(m.arg(0), m.cpu_is_64bit());
            std::lock_guard lock(sym_sessions().mutex);
            sym_sessions().invaded.erase(key);
            sym_sessions().loaded.erase(key);
            if (sym_sessions().search_path.erase(key) == 0) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            set_last_error(error_codes::ERROR_SUCCESS);  // measured
            m.set_return(1);
        }, 4});
    // SymSetOptions returns the options now in effect, and SymGetOptions
    // reports them (Learn); SYMOPT_SECURE, once set, cannot be cleared.
    db.entries.push_back({"SymSetOptions", 0,
        [](Marshaler& m){
            constexpr u32 SYMOPT_SECURE = 0x00040000u;
            std::lock_guard lock(sym_sessions().mutex);
            u32& options = sym_sessions().options;
            options = m.arg32(0) | (options & SYMOPT_SECURE);
            m.set_return(options);
        }, 4});
    db.entries.push_back({"SymGetOptions", 0,
        [](Marshaler& m){
            std::lock_guard lock(sym_sessions().mutex);
            m.set_return(sym_sessions().options);
        }, 0});
    db.entries.push_back({"SymSetSearchPath", 0,
        [](Marshaler& m){ sym_set_search_path(m, false); }, 8});
    db.entries.push_back({"SymSetSearchPathW", 0,
        [](Marshaler& m){ sym_set_search_path(m, true); }, 8});
    db.entries.push_back({"SymGetSearchPath", 0,
        [](Marshaler& m){ sym_get_search_path(m, false); }, 12});
    db.entries.push_back({"SymGetSearchPathW", 0,
        [](Marshaler& m){ sym_get_search_path(m, true); }, 12});
    // BOOL SymFromAddr(HANDLE, DWORD64 Address, PDWORD64, PSYMBOL_INFO):
    // x86 stdcall 4 + 8 + 4 + 4 = 20. No symbol source is built; the module
    // check still answers ERROR_MOD_NOT_FOUND / ERROR_INVALID_HANDLE exactly.
    db.entries.push_back({"SymFromAddr", 0,
        [](Marshaler& m){
            const bool is64 = m.cpu_is_64bit();
            const u64 key = process_key(m.arg(0), is64);
            const u64 address = is64 ? m.arg(1) : (m.arg32(1) | (u64{m.arg32(2)} << 32));
            const auto base = session_module_base(key, address);
            if (base) {
                if (*base != 0) report_no_symbols("SymFromAddr");
                set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
            }
            m.set_return(0);
        }, 20});
    // Line lookup without a PDB reports ERROR_INVALID_ADDRESS (487) on
    // Windows -- "the address is not in any line table" -- rather than a
    // module error.
    // BOOL SymGetLineFromAddr64(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64):
    // x86 stdcall 4 + 8 + 4 + 4 = 20.
    db.entries.push_back({"SymGetLineFromAddr64", 0,
        [](Marshaler& m){
            const u64 key = process_key(m.arg(0), m.cpu_is_64bit());
            bool session = false;
            {
                std::lock_guard lock(sym_sessions().mutex);
                session = sym_sessions().search_path.contains(key);
            }
            // Measured: no session is ERROR_INVALID_HANDLE.
            set_last_error(session ? 487u /* ERROR_INVALID_ADDRESS */
                                   : error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
        }, 20});
    // PVOID SymFunctionTableAccess64(HANDLE, DWORD64 AddrBase): x86 12.
    // x64: the IMAGE_RUNTIME_FUNCTION_ENTRY from the module's exception
    // directory; x86 FPO data comes only from a PDB, so NULL there.
    db.entries.push_back({"SymFunctionTableAccess64", 0,
        [](Marshaler& m){
            const bool is64 = m.cpu_is_64bit();
            const u64 key = process_key(m.arg(0), is64);
            const u64 address = is64 ? m.arg(1) : (m.arg32(1) | (u64{m.arg32(2)} << 32));
            const auto base = session_module_base(key, address);
            u64 entry = 0;
            if (base && *base != 0 && is64) {
                std::lock_guard lock(sym_sessions().mutex);
                auto& lookup = sym_sessions().modules.runtime_function;
                entry = lookup ? lookup(address) : 0;
            }
            // Measured: found -> ERROR_SUCCESS; no entry (always on x86,
            // which has FPO data only in PDBs) -> ERROR_INVALID_ADDRESS.
            if (base) set_last_error(entry != 0 ? error_codes::ERROR_SUCCESS : 487u);
            m.set_return(entry);
        }, 12});
    // DWORD64 SymGetModuleBase64(HANDLE, DWORD64): x86 12, EDX:EAX return.
    db.entries.push_back({"SymGetModuleBase64", 0,
        [](Marshaler& m){
            const bool is64 = m.cpu_is_64bit();
            const u64 key = process_key(m.arg(0), is64);
            const u64 address = is64 ? m.arg(1) : (m.arg32(1) | (u64{m.arg32(2)} << 32));
            const auto base = session_module_base(key, address);
            // Measured: found or not, a live session leaves ERROR_SUCCESS.
            if (base) set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return_i64(base.value_or(0));
        }, 12});
    // BOOL SymGetModuleInfo64(HANDLE, DWORD64, PIMAGEHLP_MODULE64)
    // No symbol database → FALSE (same honesty as SymFromAddr).
    db.entries.push_back({"SymGetModuleInfo64", 0,
        [](Marshaler& m){
            (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
            m.set_return(0);
        }, 16});
    // BOOL SymEnumerateSymbols64(HANDLE, ULONG64 BaseOfDll,
    //                            PSYM_ENUMSYMBOLS_CALLBACK64, PVOID)
    // x86 stdcall: 4 + 8 + 4 + 4 = 20. Vitreo ships no symbol database, so
    // there is nothing to enumerate and Windows' answer is FALSE with
    // ERROR_MOD_NOT_FOUND -- but the export must EXIST. Callers resolve
    // dbghelp entry points one by one and abandon crash-handler setup the
    // moment one comes back NULL, which leaves the debug mutex
    // uninitialised and fails startup before anything is drawn.
    db.entries.push_back({"SymEnumerateSymbols64", 0,
        [](Marshaler& m){
            for (u32 i = 0; i < 5; ++i) (void)m.arg(i);
            set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
            m.set_return(0);
        }, 20});
    db.entries.push_back({"StackWalk64", 0,
        [](Marshaler& m){ for (u32 i = 0; i < 9; ++i) (void)m.arg(i); m.set_return(0); }, 36});
    // BOOL MiniDumpWriteDump(HANDLE hProcess, DWORD ProcessId, HANDLE hFile,
    //   MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
    //   PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION)
    // x86 stdcall: 4 + 4 + 4 + 4 + 4 + 4 + 4 = 28.
    //
    // Vitreo writes no minidump. FALSE is the honest answer, and the caller
    // has normally created the destination file already — which is why a
    // crash directory fills up with zero-byte .dmp files. What was missing is
    // the error code: Learn says to call GetLastError on failure, and a
    // reporter that reads zero there cannot tell a failed dump from a written
    // one. ERROR_CALL_NOT_IMPLEMENTED is what Windows reports for an export
    // that exists but is not implemented on this platform.
    //
    // Emitting a synthetic MINIDUMP_HEADER instead would be worse than
    // failing: every consumer treats a well-formed dump as a genuine crash
    // record, so a header with no thread, module, or memory streams would
    // turn a visible gap into silently corrupt crash reports.
    add_minidump_exports(db);
    // SymLoadModule64(HANDLE, HANDLE, PCSTR, PCSTR, DWORD64, DWORD)
    // x86 stdcall: 4+4+4+4+8+4 = 28. No symbols → return 0 (failure).
    db.entries.push_back({"SymLoadModule64", 0,
        [](Marshaler& m){
            const bool is64 = m.cpu_is_64bit();
            const u64 key = process_key(m.arg(0), is64);
            // x86: BaseOfDll (DWORD64) occupies stack slots 4-5.
            const u64 base = is64 ? m.arg(4) : (m.arg32(4) | (u64{m.arg32(5)} << 32));
            const bool named = m.arg_ptr(1) != 0 || m.arg_ptr(2) != 0 || m.arg_ptr(3) != 0;
            m.set_return_i64(sym_load_module(key, base, named));
        }, 28});
    // DWORD SymLoadModule(HANDLE, HANDLE, PCSTR, PCSTR, DWORD BaseOfDll,
    // DWORD SizeOfDll): the 32-bit form, x86 stdcall 24.
    db.entries.push_back({"SymLoadModule", 0,
        [](Marshaler& m){
            const u64 key = process_key(m.arg(0), m.cpu_is_64bit());
            const bool named = m.arg_ptr(1) != 0 || m.arg_ptr(2) != 0 || m.arg_ptr(3) != 0;
            m.set_return(static_cast<u32>(sym_load_module(key, m.arg32(4), named)));
        }, 24});
    // BOOL SymGetSymFromAddr(HANDLE, DWORD, PDWORD, PIMAGEHLP_SYMBOL): the
    // 32-bit form of SymGetSymFromAddr64, x86 stdcall 16.
    db.entries.push_back({"SymGetSymFromAddr", 0,
        [](Marshaler& m){
            const u64 key = process_key(m.arg(0), m.cpu_is_64bit());
            const auto base = session_module_base(key, m.arg32(1));
            if (base) {
                if (*base != 0) report_no_symbols("SymGetSymFromAddr");
                set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
            }
            m.set_return(0);
        }, 16});
    // SymGetSymFromAddr64(HANDLE, DWORD64, PDWORD64, PIMAGEHLP_SYMBOL64)
    // x86 stdcall: 4+8+4+4 = 20.
    // Vitreo ships no symbol database, so this legitimately fails -- but
    // the error code matters: Windows reports ERROR_MOD_NOT_FOUND (126)
    // when no module owns the address, and callers print that distinctly
    // from a genuine lookup failure.
    db.entries.push_back({"SymGetSymFromAddr64", 0,
        [](Marshaler& m){
            const bool is64 = m.cpu_is_64bit();
            const u64 key = process_key(m.arg(0), is64);
            const u64 address = is64 ? m.arg(1) : (m.arg32(1) | (u64{m.arg32(2)} << 32));
            const auto base = session_module_base(key, address);
            if (base) {
                if (*base != 0) report_no_symbols("SymGetSymFromAddr64");
                set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
            }
            m.set_return(0);
        }, 20});
    // DWORD UnDecorateSymbolName(PCSTR DecoratedName, PSTR out,
    //                            DWORD cch, DWORD flags) — stdcall=16.
    //
    // Returns the character count written, EXCLUDING the terminator; 0
    // means failure. This was a stub returning 0 for everything, which
    // blanks out every frame of a guest-generated stack trace.
    db.entries.push_back({"UnDecorateSymbolName", 0,
        [](Marshaler& m){
            const GuestAddr in = m.arg_ptr(0);
            const GuestAddr out = m.arg_ptr(1);
            const u32 cch = static_cast<u32>(m.arg(2));
            const u32 flags = static_cast<u32>(m.arg(3));
            if (in == 0 || out == 0 || cch == 0) {
                m.set_return(0);
                return;
            }
            auto src = m.read_ansi_string(in, 4096);
            if (src.is_err()) { m.set_return(0); return; }
            const std::string text = msvc_undecorate(src.value(), flags);
            if (text.empty()) { m.set_return(0); return; }
            const u32 n = std::min<u32>(static_cast<u32>(text.size()), cch - 1);
            (void)m.memory().write_bytes(
                out, reinterpret_cast<const Byte*>(text.data()), n);
            (void)m.write_u8(out + n, 0);
            m.set_return(n);
        }, 16});
    return db;
}

}  // namespace vitreo::winapi
