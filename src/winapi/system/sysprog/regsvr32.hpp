// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file regsvr32.hpp
/// @brief regsvr32.exe, the Windows program that calls a DLL's
///        DllRegisterServer / DllUnregisterServer / DllInstall.
///
/// Contract: Learn "regsvr32" (switches /s /u /n /i[:cmdline]) and
/// tests/winapi/kernel32/oracle/regsvr32 (both bitnesses, Windows 11 26200):
/// switches may start with '/' or '-'; each named DLL is loaded, called and
/// freed in turn on an OleInitialize'd thread; /i calls DllRegisterServer
/// then DllInstall(TRUE, cmdline), /u /i calls DllInstall(FALSE, cmdline)
/// then DllUnregisterServer, /n /i calls DllInstall alone, and a bare /i
/// passes an empty command line. Exit codes: 0 success, 1 bad arguments (no
/// DLL, /n without /i, an unknown switch), 3 the DLL did not load, 4 an
/// entry point is missing, 5 an entry point returned a failure HRESULT.
///
/// Bitness hand-off (tools/oracle_probe/regsvr32_bitness, both directions):
/// after the arguments are accepted, a named file that does not load and is
/// a PE image of the other machine (I386 in the 64-bit regsvr32, AMD64 in the
/// 32-bit one) is handed to the other regsvr32.exe (SysWOW64 from 64-bit,
/// the native System32 via Sysnative from 32-bit), started with this
/// process's command line minus its program-name token, verbatim (leading
/// whitespace included). This process waits for it and exits with its exit
/// code, processing nothing further: DLLs named before the hand-off were
/// already done here, the child redoes the whole list. A file that is not a
/// PE image or does not exist stays exit code 3 with no child.
///
/// The program logic is here; the process it runs in supplies the guest
/// operations through Regsvr32Host.

#include <vitreo/common.hpp>

#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi::sysprog {

constexpr u32 REGSVR32_OK = 0;
constexpr u32 REGSVR32_FAIL_ARGS = 1;
constexpr u32 REGSVR32_FAIL_LOAD = 3;
constexpr u32 REGSVR32_FAIL_ENTRY = 4;
constexpr u32 REGSVR32_FAIL_REG = 5;

class Regsvr32Host {
public:
    virtual ~Regsvr32Host() = default;
    virtual void ole_initialize() = 0;
    virtual void ole_uninitialize() = 0;
    /// LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH); 0 on failure.
    virtual u64 load_library(const std::u16string& path) = 0;
    virtual u64 get_proc_address(u64 module, const char* name) = 0;
    virtual void free_library(u64 module) = 0;
    /// HRESULT of `HRESULT STDAPI entry(void)`.
    virtual u32 call_register(u64 entry) = 0;
    /// HRESULT of `HRESULT STDAPI DllInstall(BOOL, PCWSTR)`.
    virtual u32 call_install(u64 entry, bool install, const std::u16string& command_line) = 0;
    /// Whether this regsvr32 is the 64-bit (System32) one.
    [[nodiscard]] virtual bool is_64bit() const = 0;
    /// IMAGE_FILE_HEADER.Machine of the PE image at @p path (opened as
    /// CreateFileW would), or 0 when it cannot be read or is not a PE image.
    virtual u16 image_machine(const std::u16string& path) = 0;
    /// Run the other bitness's regsvr32.exe with @p command_line as its
    /// lpCommandLine and wait for it: its exit code, or nullopt when it could
    /// not be started.
    virtual std::optional<u32> run_other_regsvr32(const std::u16string& command_line) = 0;
};

/// @p command_line minus its program-name token (CommandLineToArgvW's argv[0]
/// rule: a leading quote runs to the next quote, otherwise to whitespace),
/// verbatim: what regsvr32 passes to the other bitness's regsvr32.
[[nodiscard]] std::u16string command_line_arguments(const std::u16string& command_line);

/// Run regsvr32 with @p argv (argv[0] is the program), parsed from
/// @p command_line (GetCommandLineW). Returns the exit code.
[[nodiscard]] u32 run_regsvr32(const std::u16string& command_line, const std::vector<std::u16string>& argv,
                               Regsvr32Host& host);

}  // namespace vitreo::winapi::sysprog
