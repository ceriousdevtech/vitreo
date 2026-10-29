// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file system_programs.hpp
/// @brief Windows system programs the runtime provides as executables.
///
/// Some %windir%\System32 programs (msiexec.exe, regsvr32.exe) are launched by installers
/// and must run as their own process of the directory's bitness: a 32-bit
/// installer turns WOW64 redirection off to run the native 64-bit
/// regsvr32.exe on a 64-bit DLL. Each is a generated PE, served at
/// System32\<name> (x64) and SysWOW64\<name> (x86) like the built-in DLLs,
/// whose entry point is
///     ExitProcess(vitreo_sysprog.dll!RunSystemProgram())
/// The host side of RunSystemProgram picks the program from the process's
/// image name. vitreo_sysprog.dll is resolvable by import only: it is not a
/// file in any directory.

#include <vitreo/common.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

inline constexpr std::string_view kSystemProgramDll = "vitreo_sysprog.dll";
inline constexpr std::string_view kSystemProgramEntry = "RunSystemProgram";

/// True for a file name (no directory) that is a runtime-provided program.
[[nodiscard]] bool is_system_program(std::string_view leaf) noexcept;

/// The runtime-provided program file names, as listed in System32/SysWOW64.
[[nodiscard]] std::vector<std::string> system_program_names();

/// The executable image served for a system program.
[[nodiscard]] std::vector<u8> build_system_program_image(bool is_64bit);

}  // namespace vitreo::winapi
