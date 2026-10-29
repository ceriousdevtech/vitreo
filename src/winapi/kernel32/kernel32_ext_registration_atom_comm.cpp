// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext_registration_atom_comm.cpp
/// @brief Extended kernel32 export table, part 9 of 9: atom tables, comm ports, activation contexts.
///
/// A contiguous slice of the table that create_kernel32_ext_thunks builds
/// (see kernel32_ext_registration.cpp). The slice boundaries follow the
/// table's registration order, which is load-bearing: a name registered
/// twice resolves last-wins, and thunk addresses are assigned in
/// registration order. Do not reorder entries or call the parts out of
/// order.

#include "winapi/kernel32/kernel32_ext_registration_parts.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/thread/context64.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/thread/restore_context64.hpp"

#include "winapi/kernel32/synchronization/slist.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <cctype>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

namespace vitreo::winapi {

void append_atom_comm_exports(Kernel32ExtManager& mgr, DllExports& exports) {
    // ---- Atom tables --------------------------------------------------------
    //
    // Windows keeps a per-process local table and a system-wide global table,
    // both mapping a string to a 16-bit ATOM. Vitreo's capsule is the process
    // boundary, so both live here; the global table is still kept separate
    // because Add/Find/Delete are distinct entry points and a name registered
    // in one must not be visible through the other.
    //
    // Integer atoms (values below 0xC000, produced by MAKEINTATOM) are not
    // stored: the string form "#nnnnn" round-trips through GetAtomName as
    // Windows specifies.
    struct AtomTable {
        std::mutex lock;
        std::unordered_map<std::string, u16> by_name;   ///< upper-cased key
        std::unordered_map<u16, std::string> by_atom;   ///< original casing
        std::unordered_map<u16, u32> refs;
        u16 next_atom{0xC000};
    };
    static AtomTable s_local_atoms;
    static AtomTable s_global_atoms;

    const auto upper_key = [](std::string_view name) {
        std::string key(name);
        for (char& c : key) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return key;
    };

    // An ATOM argument below MAXINTATOM is an integer atom, and a string
    // pointer below 0x10000 is MAKEINTATOM(n) rather than a real pointer.
    const auto integer_atom_name = [](u64 value) -> std::optional<std::string> {
        if (value >= 0xC000) return std::nullopt;
        return "#" + std::to_string(value & 0xFFFF);
    };

    const auto add_atom = [upper_key, integer_atom_name](
                              Marshaler& m, AtomTable& table, bool wide) {
        const u64 raw = m.arg(0);
        if (auto integer = integer_atom_name(raw); integer && raw != 0) {
            // MAKEINTATOM(n): nothing is stored, the atom is the value.
            m.set_return(raw & 0xFFFF);
            return;
        }
        auto name = wide ? m.read_wide_string(m.arg_ptr(0))
                         : m.read_ansi_string(m.arg_ptr(0));
        if (name.is_err() || name.value().empty()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        const std::string key = upper_key(name.value());

        std::lock_guard guard(table.lock);
        auto found = table.by_name.find(key);
        if (found != table.by_name.end()) {
            ++table.refs[found->second];
            m.set_return(found->second);
            return;
        }
        if (table.next_atom == 0xFFFF) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            m.set_return(0);
            return;
        }
        const u16 atom = table.next_atom++;
        table.by_name[key] = atom;
        table.by_atom[atom] = name.value();
        table.refs[atom] = 1;
        m.set_return(atom);
    };

    const auto find_atom = [upper_key, integer_atom_name](
                               Marshaler& m, AtomTable& table, bool wide) {
        const u64 raw = m.arg(0);
        if (auto integer = integer_atom_name(raw); integer && raw != 0) {
            m.set_return(raw & 0xFFFF);
            return;
        }
        auto name = wide ? m.read_wide_string(m.arg_ptr(0))
                         : m.read_ansi_string(m.arg_ptr(0));
        if (name.is_err() || name.value().empty()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        std::lock_guard guard(table.lock);
        auto found = table.by_name.find(upper_key(name.value()));
        if (found == table.by_name.end()) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            m.set_return(0);
            return;
        }
        m.set_return(found->second);
    };

    const auto delete_atom = [](Marshaler& m, AtomTable& table) {
        const auto atom = static_cast<u16>(m.arg(0) & 0xFFFF);
        if (atom < 0xC000) {  // integer atoms are never stored
            m.set_return(0);
            return;
        }
        std::lock_guard guard(table.lock);
        auto ref = table.refs.find(atom);
        if (ref == table.refs.end()) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            m.set_return(atom);
            return;
        }
        if (--ref->second == 0) {
            auto named = table.by_atom.find(atom);
            if (named != table.by_atom.end()) {
                std::string key(named->second);
                for (char& c : key) {
                    c = static_cast<char>(
                        std::toupper(static_cast<unsigned char>(c)));
                }
                table.by_name.erase(key);
                table.by_atom.erase(named);
            }
            table.refs.erase(ref);
        }
        m.set_return(0);
    };

    const auto get_atom_name = [integer_atom_name](
                                   Marshaler& m, AtomTable& table, bool wide) {
        const auto atom = static_cast<u16>(m.arg(0) & 0xFFFF);
        const GuestAddr buffer = m.arg_ptr(1);
        const auto buffer_chars = static_cast<u32>(m.arg(2));
        if (buffer == 0 || buffer_chars == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }

        std::string name;
        if (auto integer = integer_atom_name(atom); integer) {
            name = *integer;
        } else {
            std::lock_guard guard(table.lock);
            auto found = table.by_atom.find(atom);
            if (found == table.by_atom.end()) {
                set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
                m.set_return(0);
                return;
            }
            name = found->second;
        }

        // GetAtomName truncates rather than failing, and returns the number of
        // characters actually copied (excluding the terminator).
        const u32 copied = name.size() >= buffer_chars
                               ? buffer_chars - 1
                               : static_cast<u32>(name.size());
        if (wide) {
            for (u32 i = 0; i < copied; ++i) {
                (void)m.write_u16(buffer + i * 2,
                                  static_cast<u8>(name[i]));
            }
            (void)m.write_u16(buffer + copied * 2, 0);
        } else {
            for (u32 i = 0; i < copied; ++i) {
                (void)m.write_u8(buffer + i, static_cast<u8>(name[i]));
            }
            (void)m.write_u8(buffer + copied, 0);
        }
        m.set_return(copied);
    };

    exports.entries.push_back({"AddAtomA", 0,
        [add_atom](Marshaler& m) { add_atom(m, s_local_atoms, false); }, 4});
    exports.entries.push_back({"AddAtomW", 0,
        [add_atom](Marshaler& m) { add_atom(m, s_local_atoms, true); }, 4});
    exports.entries.push_back({"GlobalAddAtomA", 0,
        [add_atom](Marshaler& m) { add_atom(m, s_global_atoms, false); }, 4});
    exports.entries.push_back({"GlobalAddAtomW", 0,
        [add_atom](Marshaler& m) { add_atom(m, s_global_atoms, true); }, 4});

    exports.entries.push_back({"FindAtomA", 0,
        [find_atom](Marshaler& m) { find_atom(m, s_local_atoms, false); }, 4});
    exports.entries.push_back({"FindAtomW", 0,
        [find_atom](Marshaler& m) { find_atom(m, s_local_atoms, true); }, 4});
    exports.entries.push_back({"GlobalFindAtomA", 0,
        [find_atom](Marshaler& m) { find_atom(m, s_global_atoms, false); }, 4});
    exports.entries.push_back({"GlobalFindAtomW", 0,
        [find_atom](Marshaler& m) { find_atom(m, s_global_atoms, true); }, 4});

    exports.entries.push_back({"DeleteAtom", 0,
        [delete_atom](Marshaler& m) { delete_atom(m, s_local_atoms); }, 4});
    exports.entries.push_back({"GlobalDeleteAtom", 0,
        [delete_atom](Marshaler& m) { delete_atom(m, s_global_atoms); }, 4});

    exports.entries.push_back({"GetAtomNameA", 0,
        [get_atom_name](Marshaler& m) {
            get_atom_name(m, s_local_atoms, false);
        }, 12});
    exports.entries.push_back({"GetAtomNameW", 0,
        [get_atom_name](Marshaler& m) {
            get_atom_name(m, s_local_atoms, true);
        }, 12});
    exports.entries.push_back({"GlobalGetAtomNameA", 0,
        [get_atom_name](Marshaler& m) {
            get_atom_name(m, s_global_atoms, false);
        }, 12});
    exports.entries.push_back({"GlobalGetAtomNameW", 0,
        [get_atom_name](Marshaler& m) {
            get_atom_name(m, s_global_atoms, true);
        }, 12});

    // BOOL InitAtomTable(DWORD nSize) — the hash-bucket count is an internal
    // detail of Vitreo's table, so there is nothing to size, but the call
    // must succeed because guests treat failure as fatal.
    exports.entries.push_back({"InitAtomTable", 0, [](Marshaler& m) {
        (void)m.arg(0);
        m.set_return(1);
    }, 4});

    // ---- BOOL IsDBCSLeadByteEx(UINT CodePage, BYTE TestChar) ----------------
    //
    // Vitreo's CRT and console run single-byte code pages (1252 and the
    // OEM 437/850 family), where no byte is a lead byte. The DBCS code pages
    // are reported honestly rather than guessed at.
    exports.entries.push_back({"IsDBCSLeadByteEx", 0, [](Marshaler& m) {
        const auto code_page = static_cast<u32>(m.arg(0));
        const auto test_char = static_cast<u8>(m.arg(1) & 0xFF);
        bool is_lead = false;
        switch (code_page) {
        case 932:  // Shift-JIS
            is_lead = (test_char >= 0x81 && test_char <= 0x9F) ||
                      (test_char >= 0xE0 && test_char <= 0xFC);
            break;
        case 936:  // GBK
        case 949:  // Korean
        case 950:  // Big5
            is_lead = test_char >= 0x81 && test_char <= 0xFE;
            break;
        default:
            is_lead = false;  // single-byte code page
            break;
        }
        m.set_return(is_lead ? 1u : 0u);
    }, 8});

    exports.entries.push_back({"IsDBCSLeadByte", 0, [](Marshaler& m) {
        (void)m.arg(0);
        m.set_return(0);  // ANSI code page is single-byte
    }, 4});

    // Communications family (comm_port.cpp; tools/oracle_probe/comm_probe).
    exports.entries.push_back(ExportEntry{ "ClearCommBreak", 0,
        [&mgr](Marshaler& m){ mgr.impl_ClearCommBreak(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "SetCommBreak", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetCommBreak(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "GetCommMask", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetCommMask(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "SetCommMask", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetCommMask(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "GetCommProperties", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetCommProperties(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "GetCommConfig", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetCommConfig(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "SetCommConfig", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetCommConfig(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "SetupComm", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetupComm(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "TransmitCommChar", 0,
        [&mgr](Marshaler& m){ mgr.impl_TransmitCommChar(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "WaitCommEvent", 0,
        [&mgr](Marshaler& m){ mgr.impl_WaitCommEvent(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "GetDefaultCommConfigA", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetDefaultCommConfigA(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "GetDefaultCommConfigW", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetDefaultCommConfigW(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "SetDefaultCommConfigA", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetDefaultCommConfigA(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "SetDefaultCommConfigW", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetDefaultCommConfigW(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "GetCommPorts", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetCommPorts(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "OpenCommPort", 0,
        [&mgr](Marshaler& m){ mgr.impl_OpenCommPort(m); }, 12 });

    // CreateActCtxA/W (1 arg), ActivateActCtx (2), DeactivateActCtx (2),
    // ReleaseActCtx (1), GetCurrentActCtx (1). kernel32.dll since XP.
    exports.entries.push_back(ExportEntry{
        "CreateActCtxA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateActCtxA(m); }, 4});
    exports.entries.push_back(ExportEntry{
        "CreateActCtxW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateActCtxW(m); }, 4});
    exports.entries.push_back(ExportEntry{
        "ActivateActCtx", 0,
        [&mgr](Marshaler& m) { mgr.impl_ActivateActCtx(m); }, 8});
    exports.entries.push_back(ExportEntry{
        "DeactivateActCtx", 0,
        [&mgr](Marshaler& m) { mgr.impl_DeactivateActCtx(m); }, 8});
    exports.entries.push_back(ExportEntry{
        "ReleaseActCtx", 0,
        [&mgr](Marshaler& m) { mgr.impl_ReleaseActCtx(m); }, 4});
    exports.entries.push_back(ExportEntry{
        "GetCurrentActCtx", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCurrentActCtx(m); }, 4});

    // BOOL CancelSynchronousIo(HANDLE hThread)
    // https://learn.microsoft.com/en-us/windows/win32/fileio/cancelsynchronousio-func
    exports.entries.push_back({"CancelSynchronousIo", 0, [&mgr](Marshaler& m) {
        mgr.impl_CancelSynchronousIo(m);
    }, 4});

    // BOOL SetSystemPowerState(BOOL fSuspend, BOOL fForce)
    // https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setsystempowerstate
    // Requires SE_SHUTDOWN_NAME. Vitreo does not grant that privilege, so the
    // suspend/hibernate request fails without touching host power state.
    exports.entries.push_back({"SetSystemPowerState", 0, [](Marshaler& m) {
        (void)m.arg(0);
        (void)m.arg(1);
        set_last_error(error_codes::ERROR_PRIVILEGE_NOT_HELD);
        m.set_return(0);
    }, 8});

    // BOOL GetNumaHighestNodeNumber(PULONG HighestNodeNumber)
    // https://learn.microsoft.com/en-us/windows/win32/api/systemtopologyapi/nf-systemtopologyapi-getnumahighestnodenumber
    // A single-node machine reports 0. The highest node number is not the
    // node count.
    exports.entries.push_back({"GetNumaHighestNodeNumber", 0, [](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        (void)m.write_u32(out, 0);
        m.set_return(1);
    }, 4});

    // RtlRestoreContext (Learn winnt.h): restore a CONTEXT, applying the
    // STATUS_LONGJUMP / STATUS_UNWIND_CONSOLIDATE ExceptionRecord forms.
    exports.entries.push_back(ExportEntry{"RtlRestoreContext", 0,
        [&mgr](Marshaler& m) { rtl_restore_context(m, mgr.module_manager()); },
        0});
}

} // namespace vitreo::winapi
