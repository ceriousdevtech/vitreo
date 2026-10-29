// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ntdll_forwarded.cpp
/// @brief kernel32 exports Windows forwards to ntdll that no other kernel32
///        family owns. See kernel32_ntdll_forwarded.hpp.

#include "winapi/kernel32/kernel32_ntdll_forwarded.hpp"

#include "core/memory/guest_memory.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "winapi/common/c_specific_handler.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/vectored_exception.hpp"
#include "winapi/system/ntdll/ntdll_ext.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <strings.h>

namespace vitreo::winapi {

namespace {

/// KUSER_SHARED_DATA.Cookie (+0x330), the per-boot value the system-pointer
/// encoding uses (k32_ntdll_forwarder_probe reads it from the guest).
constexpr GuestAddr kSharedUserCookie = pe::SHARED_USER_DATA_ADDRESS + 0x330;

u64 rotate_right(u64 value, u32 count, u32 bits) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    value &= mask;
    count &= bits - 1;
    if (count == 0) return value;
    return ((value >> count) | (value << (bits - count))) & mask;
}

/// k32_ntdll_forwarder_probe, x64 and x86: Encode(p) is
/// ror(p ^ Cookie, Cookie mod pointer-bits) with the 32-bit Cookie
/// zero-extended, and Decode is its inverse whatever its input.
void encode_system_pointer(Marshaler& m, bool decode) {
    const u32 bits = m.cpu_is_64bit() ? 64u : 32u;
    const u64 cookie = m.read_u32(kSharedUserCookie).value_or(0);
    const u32 shift = static_cast<u32>(cookie) & (bits - 1);
    const u64 value = m.arg(0);
    m.set_return(decode ? rotate_right(value, bits - shift, bits) ^ cookie
                        : rotate_right(value ^ cookie, shift, bits));
}

std::mutex g_icx64_mutex;

constexpr u32 kStatusSuccess = 0;
constexpr u32 kStatusInvalidParameter = 0xC000000Du;
constexpr u32 kStatusDllNotFound = 0xC0000135u;
constexpr u32 kStatusDelayLoadFailed = 0xC0000412u;
constexpr u32 kDelayloadGpaFailure = 4;  // DELAYLOAD_GPA_FAILURE (delayloadhandler.h)

/// Reads a guest value, raising the access violation the guest's own read
/// would have taken when the address is not readable.
struct GuestReader {
    Marshaler& m;
    bool failed = false;
    u64 ptr(GuestAddr at) {
        if (failed) return 0;
        auto v = m.read_ptr(at);
        if (v.is_ok()) return v.value();
        failed = true;
        (void)m.raise_access_violation(false, at);
        return 0;
    }
    u32 u32_at(GuestAddr at) {
        if (failed) return 0;
        auto v = m.read_u32(at);
        if (v.is_ok()) return v.value();
        failed = true;
        (void)m.raise_access_violation(false, at);
        return 0;
    }
};

/// One delay-load import, as delayload_probe measures ResolveDelayLoadedAPI
/// (x64 and x86):
///  - the parent must be a loaded module's base, the descriptor RVA-based
///    (Attributes bit 0), else NULL with nothing touched;
///  - an IAT entry that no longer points into the parent image is already
///    resolved and is returned as it is;
///  - otherwise the DLL comes from the descriptor's module-handle slot or
///    is loaded (the slot written), the import is found by name or ordinal
///    through the INT entry at the IAT entry's index, written to the IAT
///    and returned;
///  - on failure the DLL hook runs with DELAYLOAD_GPA_FAILURE and a
///    DELAYLOAD_INFO whose LastError is 126 (DLL), 127 (name) or 182
///    (ordinal); its non-NULL return is the result, else the system hook's
///    (DllName, ProcName or ordinal). Only an ordinal missing from a loaded
///    DLL writes a non-NULL result to the IAT. Nothing is cached, and the
///    thread's last error is untouched throughout.
u64 resolve_delay_import(Marshaler& m, ModuleManager& modules, Kernel32ExtManager& k32,
                         GuestAddr parent, GuestAddr descriptor, u64 dll_hook,
                         u64 system_hook, GuestAddr thunk) {
    const bool is64 = m.cpu_is_64bit();
    const u32 ptr = is64 ? 8u : 4u;
    const auto module = modules.module_for_address(parent);
    if (module.info == nullptr || module.rva != 0) return 0;

    GuestReader r{m};
    const u32 nt = r.u32_at(parent + 0x3C);
    const u32 size_of_image = r.u32_at(parent + nt + 0x50);  // same offset in PE32 / PE32+
    const u64 current = r.ptr(thunk);
    if (r.failed) return 0;
    if (current < parent || current >= parent + size_of_image) return current;

    const u32 attributes = r.u32_at(descriptor);
    const u32 dll_rva = r.u32_at(descriptor + 4);
    const u32 handle_rva = r.u32_at(descriptor + 8);
    const u32 iat_rva = r.u32_at(descriptor + 12);
    const u32 int_rva = r.u32_at(descriptor + 16);
    if (r.failed || (attributes & 1u) == 0) return 0;
    const u64 index = (thunk - (parent + iat_rva)) / ptr;
    const u64 entry = r.ptr(parent + int_rva + index * ptr);
    if (r.failed) return 0;
    const bool by_ordinal = is64 ? (entry >> 63) != 0 : (entry & 0x80000000u) != 0;
    const u16 ordinal = static_cast<u16>(entry & 0xFFFFu);
    const GuestAddr name_addr = parent + static_cast<u32>(entry) + 2;  // IMAGE_IMPORT_BY_NAME.Name
    const GuestAddr dll_name_addr = parent + dll_rva;
    const std::string dll_name = m.read_ansi_string(dll_name_addr).value_or(std::string{});

    const u32 saved_error = get_last_error();
    const GuestAddr handle_slot = parent + handle_rva;
    u64 handle = r.ptr(handle_slot);
    if (r.failed) return 0;
    if (handle == 0 && !dll_name.empty()) {
        handle = modules.load_library(dll_name);
        if (handle != 0) (void)m.write_ptr(handle_slot, handle);
    }
    u64 proc = 0;
    u32 error = 0;
    if (handle == 0) {
        error = error_codes::ERROR_MOD_NOT_FOUND;
    } else {
        if (by_ordinal) {
            proc = modules.get_proc_address_by_ordinal(static_cast<GuestAddr>(handle), ordinal);
        } else {
            const std::string name = m.read_ansi_string(name_addr).value_or(std::string{});
            proc = modules.get_proc_address(static_cast<GuestAddr>(handle), name);
        }
        if (proc == 0) error = by_ordinal ? 182u /* ERROR_INVALID_ORDINAL */
                                          : error_codes::ERROR_PROC_NOT_FOUND;
    }
    set_last_error(saved_error);
    if (proc != 0) {
        (void)m.write_ptr(thunk, proc);
        return proc;
    }

    const auto& guest_call = k32.guest_call();
    if ((dll_hook != 0 || system_hook != 0) && !guest_call) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!ResolveDelayLoadedAPI: "
                                 "failure hook with no guest-call path -> NULL\n");
        }
        return 0;
    }
    cpu::CpuState* cpu = m.cpu();
    const u64 saved_sp = cpu->sp();
    // DELAYLOAD_INFO (delayloadhandler.h): 72 bytes on x64, 36 on x86 --
    // the Size delayload_probe reads. Built below the caller's stack and
    // passed to the hook; the hook runs below it.
    const GuestAddr info = (saved_sp - 0x100) & ~u64{0xF};
    const u32 info_size = is64 ? 72u : 36u;
    const u8 zeros[72] = {};
    (void)m.memory().write_bytes(info, zeros, info_size);
    (void)m.write_u32(info, info_size);
    (void)m.write_ptr(info + ptr * 1, descriptor);
    (void)m.write_ptr(info + ptr * 2, thunk);
    (void)m.write_ptr(info + ptr * 3, dll_name_addr);
    (void)m.write_u32(info + ptr * 4, by_ordinal ? 0u : 1u);
    (void)m.write_ptr(info + ptr * 5, by_ordinal ? ordinal : name_addr);
    (void)m.write_ptr(info + ptr * 6, handle);
    (void)m.write_u32(info + ptr * 8, error);

    u64 result = 0;
    if (dll_hook != 0) {
        cpu->set_sp(info);
        const u64 args[2] = {kDelayloadGpaFailure, info};
        result = guest_call(static_cast<GuestAddr>(dll_hook), args, 2, is64);
        if (!is64) result &= 0xFFFFFFFFu;
    }
    if (result == 0 && system_hook != 0) {
        cpu->set_sp(info);
        const u64 args[2] = {dll_name_addr, by_ordinal ? ordinal : name_addr};
        result = guest_call(static_cast<GuestAddr>(system_hook), args, 2, is64);
        if (!is64) result &= 0xFFFFFFFFu;
    }
    cpu->set_sp(saved_sp);
    set_last_error(saved_error);
    if (result != 0 && error == 182u) (void)m.write_ptr(thunk, result);
    return result;
}

/// x64 __chkstk (k32_ntdll_misc_forwarder_probe): RAX bytes are about to be
/// allocated below the caller's RSP (here RSP + 8, past the return
/// address; an allocation larger than RSP probes down to 0). Every page
/// from just under TEB.StackLimit down to the target page is read, so a
/// stack that ends there faults on its guard -- a read access violation
/// the dispatcher turns into STATUS_STACK_OVERFLOW (2 parameters, read, an
/// address inside the stack). No register changes, RAX included.
void chkstk(Marshaler& m) {
    cpu::CpuState* cpu = m.cpu();
    const u64 rax = cpu->gpr.rax;
    const u64 caller_sp = cpu->gpr.rsp + 8;
    const u64 target = caller_sp >= rax ? caller_sp - rax : 0;
    const GuestAddr teb = static_cast<GuestAddr>(cpu->seg.gs_desc.base);
    const u64 limit = m.read_u64(teb + 0x10).value_or(0);
    if (target < limit) {
        const u64 target_page = target & ~u64{0xFFF};
        for (u64 page = limit - 0x1000;; page -= 0x1000) {
            if (m.memory().read_u8(page).is_err()) {
                (void)m.raise_access_violation(false, page);
                break;
            }
            if (page <= target_page) break;
        }
    }
    m.set_return(rax);
}

}  // namespace

DllExports make_kernel32_ntdll_forwarded_exports(bool guest_is_64bit,
                                                  ModuleManager& modules,
                                                  Kernel32ExtManager& k32) {
    DllExports exports;

    // NTSTATUS LogUnexpectedCodepath(const <12-byte codepath record>*)
    // (ntdll reads three ULONGs from it). k32_ntdll_misc_forwarder_probe:
    // STATUS_SUCCESS, the record and the last error untouched. On Windows
    // the record goes to the system's telemetry, which has no counterpart
    // here; Vitreo's trace log records it instead.
    exports.entries.push_back({"LogUnexpectedCodepath", 0, [](Marshaler& m) {
        const GuestAddr record = m.arg_ptr(0);
        GuestReader r{m};
        const u32 a = r.u32_at(record);
        const u32 b = r.u32_at(record + 4);
        const u32 c = r.u32_at(record + 8);
        if (r.failed) return;
        VITREO_TRACE("[k32] LogUnexpectedCodepath(%08x %08x %08x)\n", a, b, c);
        (void)a; (void)b; (void)c;
        m.set_return(kStatusSuccess);
    }, 4});

    // PVOID ResolveDelayLoadedAPI(PVOID ParentModuleBase,
    //     PCIMAGE_DELAYLOAD_DESCRIPTOR DelayloadDescriptor,
    //     PDELAYLOAD_FAILURE_DLL_CALLBACK FailureDllHook,
    //     PDELAYLOAD_FAILURE_SYSTEM_ROUTINE FailureSystemHook,
    //     PIMAGE_THUNK_DATA ThunkAddress, ULONG Flags)
    // Learn; Flags is reserved, and a nonzero one returns NULL
    // (delayload_probe flags_1). The rest is resolve_delay_import.
    exports.entries.push_back({"ResolveDelayLoadedAPI", 0, [&modules, &k32](Marshaler& m) {
        if (static_cast<u32>(m.arg(5)) != 0) {
            m.set_return(0);
            return;
        }
        m.set_return(resolve_delay_import(m, modules, k32, m.arg_ptr(0), m.arg_ptr(1),
                                          m.arg_ptr(2), m.arg_ptr(3), m.arg_ptr(4)));
    }, 24});

    // NTSTATUS ResolveDelayLoadsFromDll(PVOID ParentBase, LPCSTR TargetDllName,
    //                                   ULONG Flags)
    // Learn: STATUS_SUCCESS or STATUS_DLL_NOT_FOUND. delayload_probe (a
    // real delay-import directory, x64 and x86): a nonzero Flags is
    // STATUS_INVALID_PARAMETER; the descriptor is the one whose DllName
    // equals TargetDllName ignoring case (no implied ".dll"), none (or no
    // delay-import directory) is STATUS_DLL_NOT_FOUND; every import of it is
    // resolved as ResolveDelayLoadedAPI without hooks does, continuing past
    // failures, and any failure -- a DLL that does not load, a missing
    // export, a parent that is not a loaded module -- is
    // STATUS_DELAY_LOAD_FAILED. The last error is untouched.
    exports.entries.push_back({"ResolveDelayLoadsFromDll", 0, [&modules, &k32](Marshaler& m) {
        const GuestAddr parent = m.arg_ptr(0);
        const GuestAddr target_name = m.arg_ptr(1);
        if (static_cast<u32>(m.arg(2)) != 0) {
            m.set_return(kStatusInvalidParameter);
            return;
        }
        const bool is64 = m.cpu_is_64bit();
        const u32 ptr = is64 ? 8u : 4u;
        auto target = m.read_ansi_string(target_name);
        if (!target.is_ok()) {
            (void)m.raise_access_violation(false, target_name);
            return;
        }
        GuestReader r{m};
        const GuestAddr nt = parent + r.u32_at(parent + 0x3C);
        const u16 magic = static_cast<u16>(r.u32_at(nt + 0x18));
        // OptionalHeader.NumberOfRvaAndSizes and DataDirectory: +0x6C/+0x70
        // in PE32+, +0x5C/+0x60 in PE32; the delay-import entry is index 13.
        const u32 dirs = magic == 0x20B ? 0x70u : 0x60u;
        const u32 count = r.u32_at(nt + 0x18 + dirs - 4);
        const u32 delay_rva = count > 13 ? r.u32_at(nt + 0x18 + dirs + 13 * 8) : 0;
        if (r.failed) return;
        GuestAddr found = 0;
        for (u32 i = 0; delay_rva != 0 && i < 4096; ++i) {
            const GuestAddr d = parent + delay_rva + i * 32;
            const u32 dll_rva = r.u32_at(d + 4);
            if (r.failed) return;
            if (dll_rva == 0) break;
            const auto name = m.read_ansi_string(parent + dll_rva);
            if (name.is_ok() && ::strcasecmp(name.value().c_str(), target.value().c_str()) == 0) {
                found = d;
                break;
            }
        }
        if (found == 0) {
            m.set_return(kStatusDllNotFound);
            return;
        }
        const u32 iat_rva = r.u32_at(found + 12);
        const u32 int_rva = r.u32_at(found + 16);
        if (r.failed) return;
        bool failed = false;
        for (u32 k = 0; k < 0x10000; ++k) {
            if (r.ptr(parent + int_rva + k * ptr) == 0 || r.failed) break;
            if (resolve_delay_import(m, modules, k32, parent, found, 0, 0,
                                     parent + iat_rva + k * ptr) == 0) {
                failed = true;
            }
            if (m.cpu()->pending_raise.active) return;
        }
        if (r.failed) return;
        m.set_return(failed ? kStatusDelayLoadFailed : kStatusSuccess);
    }, 12});

    // PVOID AddVectoredContinueHandler(ULONG First,
    //                                  PVECTORED_EXCEPTION_HANDLER Handler)
    // ULONG RemoveVectoredContinueHandler(PVOID Handle)
    // Learn; the list rules are in vectored_exception.hpp.
    exports.entries.push_back({"AddVectoredContinueHandler", 0, [](Marshaler& m) {
        m.set_return(add_vectored_continue_handler(static_cast<u32>(m.arg(0)),
                                                   m.arg_ptr(1)));
    }, 8});
    exports.entries.push_back({"RemoveVectoredContinueHandler", 0, [](Marshaler& m) {
        m.set_return(remove_vectored_continue_handler(m.arg_ptr(0)) ? 1u : 0u);
    }, 4});

    // PVOID EncodeSystemPointer(PVOID Ptr) / PVOID DecodeSystemPointer(PVOID Ptr)
    exports.entries.push_back({"EncodeSystemPointer", 0,
        [](Marshaler& m) { encode_system_pointer(m, false); }, 4});
    exports.entries.push_back({"DecodeSystemPointer", 0,
        [](Marshaler& m) { encode_system_pointer(m, true); }, 4});

    // VOID InitOnceInitialize(PINIT_ONCE InitOnce): INIT_ONCE_STATIC_INIT,
    // one pointer of zero; the last error is untouched
    // (k32_ntdll_forwarder_probe init_once).
    exports.entries.push_back({"InitOnceInitialize", 0, [](Marshaler& m) {
        const GuestAddr init_once = m.arg_ptr(0);
        if (m.write_ptr(init_once, 0).is_err()) {
            (void)m.raise_access_violation(true, init_once);
        }
    }, 4});

    // VOID RtlZeroMemory(PVOID, SIZE_T): kernel32 exports ntdll's on both
    // bitnesses; RtlMoveMemory too (a forwarder on x86, kernel32's own
    // export on x64).
    exports.entries.push_back({"RtlZeroMemory", 0, impl_RtlZeroMemory, 8});
    exports.entries.push_back({"RtlMoveMemory", 0, impl_RtlMoveMemory, 12});

    if (guest_is_64bit) {
        // BOOLEAN RtlIsEcCode(ULONG64 CodePointer). Learn: TRUE for ARM64EC
        // code. A Vitreo process is an x64 process with no ARM64EC code, as
        // on x64 Windows (k32_ntdll_forwarder_probe is_ec_code: FALSE for
        // image code, ntdll code, heap and NULL).
        exports.entries.push_back({"RtlIsEcCode", 0,
            [](Marshaler& m) { (void)m.arg(0); m.set_return(0); }, 8});
        // The SEH scope-table handler ntdll already exports.
        exports.entries.push_back(make_c_specific_handler_export());
        // VOID __misaligned_access(void): returns with every register as it
        // was (k32_ntdll_misc_forwarder_probe); RAX goes back unchanged.
        exports.entries.push_back({"__misaligned_access", 0,
            [](Marshaler& m) { m.set_return(m.cpu()->gpr.rax); }, 0});
        exports.entries.push_back({"__chkstk", 0, chkstk, 0});
    } else {
        // LONGLONG InterlockedCompareExchange64(LONGLONG volatile* Destination,
        //                                       LONGLONG Exchange,
        //                                       LONGLONG Comparand)
        // Learn; x86 kernel32 only (x64 has it as an intrinsic). Stdcall:
        // the 64-bit values take two stack slots each, the result EDX:EAX.
        exports.entries.push_back({"InterlockedCompareExchange64", 0, [](Marshaler& m) {
            const GuestAddr dest = m.arg_ptr(0);
            const u64 exchange = (m.arg(2) << 32) | (m.arg(1) & 0xFFFFFFFFu);
            const u64 comparand = (m.arg(4) << 32) | (m.arg(3) & 0xFFFFFFFFu);
            if (auto* hp = m.memory().host_ptr_mut(dest, 8);
                hp != nullptr && (dest & 7) == 0) {
                u64 expected = comparand;
                (void)__atomic_compare_exchange_n(
                    reinterpret_cast<u64*>(hp), &expected, exchange,
                    /*weak=*/false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                m.set_return_i64(expected);
                return;
            }
            std::lock_guard<std::mutex> lock(g_icx64_mutex);
            const auto old = m.read_u64(dest);
            if (!old.is_ok()) {
                (void)m.raise_access_violation(false, dest);
                return;
            }
            if (old.value() == comparand && m.write_u64(dest, exchange).is_err()) {
                (void)m.raise_access_violation(true, dest);
                return;
            }
            m.set_return_i64(old.value());
        }, 20});
    }
    return exports;
}

}  // namespace vitreo::winapi
