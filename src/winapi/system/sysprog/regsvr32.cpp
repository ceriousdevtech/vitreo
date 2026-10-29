// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/sysprog/regsvr32.hpp"

#include "core/pe/parser/pe_structures.hpp"

#include <cstdio>

namespace vitreo::winapi::sysprog {

namespace {

[[nodiscard]] bool failed(u32 hr) { return (hr & 0x80000000u) != 0; }

[[nodiscard]] char16_t lower(char16_t c) {
    return (c >= u'A' && c <= u'Z') ? static_cast<char16_t>(c - u'A' + u'a') : c;
}

}  // namespace

std::u16string command_line_arguments(const std::u16string& command_line) {
    usize i = 0;
    const usize n = command_line.size();
    if (n != 0 && command_line[0] == u'"') {
        ++i;
        while (i < n && command_line[i] != u'"') ++i;
        if (i < n) ++i;  // closing quote
    } else {
        while (i < n && command_line[i] != u' ' && command_line[i] != u'\t') ++i;
    }
    return command_line.substr(i);
}

u32 run_regsvr32(const std::u16string& command_line, const std::vector<std::u16string>& argv, Regsvr32Host& host) {
    bool silent = false, unregister = false, no_register = false, install = false;
    std::u16string install_cmd;
    std::vector<std::u16string> dlls;
    for (usize i = 1; i < argv.size(); ++i) {
        const std::u16string& arg = argv[i];
        if (arg.size() >= 2 && (arg[0] == u'/' || arg[0] == u'-')) {
            const char16_t sw = lower(arg[1]);
            if (sw == u'i' && (arg.size() == 2 || arg[2] == u':')) {
                install = true;
                install_cmd = arg.size() > 3 ? arg.substr(3) : std::u16string{};
                continue;
            }
            if (arg.size() != 2) return REGSVR32_FAIL_ARGS;
            switch (sw) {
                case u's': silent = true; continue;
                case u'u': unregister = true; continue;
                case u'n': no_register = true; continue;
                default: return REGSVR32_FAIL_ARGS;
            }
        }
        dlls.push_back(arg);
    }
    if (dlls.empty() || (no_register && !install)) return REGSVR32_FAIL_ARGS;
    if (!silent) {
        // Windows reports each result in a message box; the exit code and the
        // calls are the same.
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED regsvr32.exe: result message box without /s\n");
    }

    host.ole_initialize();
    u32 code = REGSVR32_OK;
    for (const auto& dll : dlls) {
        const u64 module = host.load_library(dll);
        if (module == 0) {
            // A PE image of the other machine: the other regsvr32 does the
            // whole command (header comment, regsvr32_bitness probe).
            const auto other = static_cast<u16>(host.is_64bit() ? pe::MachineType::I386 : pe::MachineType::AMD64);
            if (host.image_machine(dll) == other) {
                const auto child = host.run_other_regsvr32(command_line_arguments(command_line));
                // Not measurable on Windows (the other regsvr32 always starts
                // there): the DLL was not loaded, exit code 3.
                code = child ? *child : REGSVR32_FAIL_LOAD;
                break;
            }
            code = REGSVR32_FAIL_LOAD;
            break;
        }
        const char* reg_name = unregister ? "DllUnregisterServer" : "DllRegisterServer";
        const u64 reg = no_register ? 0 : host.get_proc_address(module, reg_name);
        const u64 inst = install ? host.get_proc_address(module, "DllInstall") : 0;
        if ((!no_register && reg == 0) || (install && inst == 0)) {
            host.free_library(module);
            code = REGSVR32_FAIL_ENTRY;
            break;
        }
        bool ok = true;
        if (unregister) {
            if (install) ok = !failed(host.call_install(inst, false, install_cmd));
            if (ok && !no_register) ok = !failed(host.call_register(reg));
        } else {
            if (!no_register) ok = !failed(host.call_register(reg));
            if (ok && install) ok = !failed(host.call_install(inst, true, install_cmd));
        }
        host.free_library(module);
        if (!ok) {
            code = REGSVR32_FAIL_REG;
            break;
        }
    }
    host.ole_uninitialize();
    return code;
}

}  // namespace vitreo::winapi::sysprog
