// SPDX-License-Identifier: LGPL-2.1-only

/// @file module_helpers.cpp
/// @brief Shared ModuleManager helpers promoted for multi-TU splits.

#include <cstring>
#include <limits>
#include <mutex>
#include "module_internal.hpp"
#include "capsule/vfs/win32_full_path.hpp"
#include "winapi/common/synthetic_version_resource.hpp"
#include "winapi/system/ntdll/ntdll_system_services.hpp"

#include "core/pe/loader/load_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vitreo/diagnostics.hpp>

namespace vitreo::winapi {

/// Opt-in: skip PE ImageBase and load into the dynamic DLL arena (with
/// relocations). Used to reproduce address-space layouts where preferred-base
/// mapping is unavailable. A preferred-base collision lands the image
/// elsewhere; callers that parse the relocated image must still see a
/// consistent layout.
[[nodiscard]] bool force_dll_reloc() noexcept {
    static const bool enabled =
        (std::getenv("VITREO_FORCE_DLL_RELOC") != nullptr);
    return enabled;
}

[[nodiscard]] bool module_diag_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_MODULE_DIAG");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

[[nodiscard]] bool looks_like_path(std::string_view s) {
    return s.find('\\') != std::string_view::npos ||
           s.find('/') != std::string_view::npos ||
           s.find(':') != std::string_view::npos;
}

[[nodiscard]] bool is_absolute_windows_path(std::string_view s) {
    // Drive-absolute (C:\...), UNC (\\server\share), or root-absolute (\foo).
    if (s.size() >= 2) {
        const unsigned char drive = static_cast<unsigned char>(s[0]);
        if (((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z')) &&
            s[1] == ':') {
            return true;
        }
        if (s[0] == '\\' && s[1] == '\\') {
            return true;
        }
    }
    return !s.empty() && (s[0] == '\\' || s[0] == '/');
}

[[nodiscard]] std::string normalize_path_key(std::string_view path) {
    // The loader names a module by its full path: "/", repeated separators,
    // "." / ".." (even through a missing directory) and trailing dots and
    // spaces all reach the module of the canonical spelling, x86 and x64
    // (tools/oracle_probe/loader_path_canon_probe.c, loader_path_canon_probe
    // {32,64}.expected). Drive-absolute and UNC paths do not depend on the
    // current directory; LoadLibrary canonicalizes the rest against it.
    const bool drive_absolute = path.size() >= 3 && path[1] == ':' &&
                                (path[2] == '\\' || path[2] == '/') &&
                                std::isalpha(static_cast<unsigned char>(path[0]));
    const bool unc = path.size() >= 2 && (path[0] == '\\' || path[0] == '/') &&
                     (path[1] == '\\' || path[1] == '/');
    std::string out(path);
    if (drive_absolute || unc) {
        if (auto full = capsule::win32_full_path(path, "C:\\")) out = std::move(full->path);
    }
    // Normalize slashes and lowercase for stable lookup.
    for (char& c : out) {
        if (c == '/') c = '\\';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

[[nodiscard]] bool is_system_dll_path(std::string_view path) {
    // Conservative: only treat these well-known directories as "system".
    // This preserves the invariant that plugin DLLs under Temp must be PE-backed.
    std::string p = normalize_path_key(path);
    return p.rfind("c:\\windows\\system32\\", 0) == 0 ||
           p.rfind("c:\\windows\\syswow64\\", 0) == 0;
}

[[nodiscard]] std::string parent_windows_dir(std::string_view win_path) {
    // Strip trailing filename component from a Windows-style path.
    // Returns "" if there is no separator.
    auto last_sep = win_path.find_last_of("\\/");
    if (last_sep == std::string_view::npos) {
        return {};
    }
    return std::string(win_path.substr(0, last_sep));
}

[[nodiscard]] SyntheticPeImage build_synthetic_pe_image(
    GuestAddr image_base,
    bool is_64bit,
    std::string_view dll_name,
    ThunkRegistry& registry,
    bool register_aliases
) {
    constexpr usize image_size = SYNTHETIC_MODULE_SLOT_SIZE;
    // The last page is the .rsrc section: one RT_VERSION resource, the
    // VS_VERSIONINFO GetFileVersionInfo reports for this module. The image is
    // also the file served at the module's path, so a process that parses
    // that file sees the version the API returns.
    constexpr usize resource_rva = image_size - 0x1000;
    constexpr usize nt_offset = 0x80;
    constexpr usize optional_offset = nt_offset + 4 + 20;
    const usize optional_size = is_64bit ? 0xF0 : 0xE0;
    const usize section_offset = optional_offset + optional_size;

    std::vector<Byte> image(image_size, 0);
    u32 ntdll_exception_rva = 0;
    u32 ntdll_exception_size = 0;
    ParsedExports published_exports;
    usize code_rva = 0x2000;
    auto put16 = [&image](usize offset, u16 value) {
        image[offset + 0] = static_cast<Byte>(value & 0xFFu);
        image[offset + 1] = static_cast<Byte>((value >> 8) & 0xFFu);
    };
    auto put32 = [&image](usize offset, u32 value) {
        for (usize i = 0; i < 4; ++i) {
            image[offset + i] =
                static_cast<Byte>((value >> (i * 8)) & 0xFFu);
        }
    };
    auto put64 = [&image](usize offset, u64 value) {
        for (usize i = 0; i < 8; ++i) {
            image[offset + i] =
                static_cast<Byte>((value >> (i * 8)) & 0xFFu);
        }
    };
    auto put_string = [&image](usize offset, std::string_view value) {
        std::copy(value.begin(), value.end(),
                  image.begin() + static_cast<std::ptrdiff_t>(offset));
        image[offset + value.size()] = 0;
    };

    // DOS header and NT signature.
    put16(0x00, 0x5A4D); // IMAGE_DOS_SIGNATURE ("MZ")
    put32(0x3C, static_cast<u32>(nt_offset));
    put32(nt_offset, 0x00004550); // IMAGE_NT_SIGNATURE ("PE\0\0")

    // IMAGE_FILE_HEADER: .edata, .text and .rsrc (the count is set below,
    // once it is known whether the export image left room for .rsrc).
    put16(nt_offset + 4, is_64bit ? 0x8664 : 0x014C);
    put16(nt_offset + 6, 2);
    put16(nt_offset + 20, static_cast<u16>(optional_size));
    put16(nt_offset + 22, is_64bit ? 0x2022 : 0x2102);

    // IMAGE_OPTIONAL_HEADER32/64.
    put16(optional_offset + 0x00, is_64bit ? 0x020B : 0x010B);
    put32(optional_offset + 0x10, 0);      // AddressOfEntryPoint
    put32(optional_offset + 0x14, 0x1000); // BaseOfCode
    if (is_64bit) {
        put64(optional_offset + 0x18, image_base);
    } else {
        put32(optional_offset + 0x18, 0x2000); // BaseOfData
        put32(optional_offset + 0x1C, static_cast<u32>(image_base));
    }
    put32(optional_offset + 0x20, 0x1000); // SectionAlignment
    put32(optional_offset + 0x24, 0x0200); // FileAlignment
    put16(optional_offset + 0x28, 6);      // MajorOperatingSystemVersion
    put16(optional_offset + 0x30, 6);      // MajorSubsystemVersion
    put32(optional_offset + 0x38, static_cast<u32>(image_size));
    put32(optional_offset + 0x3C, 0x1000); // SizeOfHeaders
    put16(optional_offset + 0x44, 3);      // IMAGE_SUBSYSTEM_WINDOWS_CUI
    put16(optional_offset + 0x46, 0x8160); // NX, ASLR, high entropy (x64)
    if (is_64bit) {
        put64(optional_offset + 0x48, 0x100000);
        put64(optional_offset + 0x50, 0x1000);
        put64(optional_offset + 0x58, 0x100000);
        put64(optional_offset + 0x60, 0x1000);
        put32(optional_offset + 0x6C, 16); // NumberOfRvaAndSizes
    } else {
        put32(optional_offset + 0x48, 0x100000);
        put32(optional_offset + 0x4C, 0x1000);
        put32(optional_offset + 0x50, 0x100000);
        put32(optional_offset + 0x54, 0x1000);
        put32(optional_offset + 0x5C, 16); // NumberOfRvaAndSizes
    }

    // Materialize a real IMAGE_EXPORT_DIRECTORY. Some Windows components do
    // not call GetProcAddress; they parse the module's PE export table
    // directly. Each EAT
    // entry therefore points at a tiny in-image jump veneer which transfers
    // control to the corresponding Vitreo thunk.
    struct SyntheticExport {
        std::string name;
        u16 requested_ordinal{0};
        u16 ordinal{0};
        GuestAddr thunk{0};
        u16 stdcall_bytes{0};
    };
    std::string normalized_dll(dll_name);
    std::transform(
        normalized_dll.begin(), normalized_dll.end(), normalized_dll.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::vector<SyntheticExport> exports;
    for (const auto& info : registry.enumerate_exports()) {
        if (info.dll_name != normalized_dll || info.function_name.empty()) {
            continue;
        }
        exports.push_back(SyntheticExport{
            info.function_name, info.ordinal, 0, info.thunk_address,
            info.stdcall_bytes});
    }
    std::sort(
        exports.begin(), exports.end(),
        [](const SyntheticExport& lhs, const SyntheticExport& rhs) {
            return lhs.name < rhs.name;
        });

    // ntdll's system services, as a guest parsing the export table sees them
    // (ntdll_service_layout_probe): NtXxx and ZwXxx name one stub; the stubs
    // are contiguous, ascending, one per service; and the number each loads
    // into EAX is its rank. Direct-syscall resolvers rely on exactly that --
    // Hell's Gate reads the number, Halo's Gate steps to a neighbour, and
    // SysWhispers2 sorts the Zw* exports by address and takes the rank -- so
    // a service's number here is its index in name order, and its stub is
    // that many stubs into .text. Nt* precedes Zw* in name order, so a Zw*
    // export finds its Nt* twin already numbered and shares its stub.
    std::vector<std::optional<usize>> native_alias_target(exports.size());
    std::vector<std::optional<u32>> service_number(exports.size());
    u32 service_count = 0;
    if (normalized_dll == "ntdll.dll") {
        std::unordered_map<std::string, usize> nt_by_suffix;
        for (usize i = 0; i < exports.size(); ++i) {
            const auto& entry = exports[i];
            if (!is_ntdll_system_service(entry.name)) continue;
            if (entry.name.starts_with("Zw")) {
                const auto target = nt_by_suffix.find(entry.name.substr(2));
                if (target != nt_by_suffix.end() &&
                    exports[target->second].stdcall_bytes ==
                        entry.stdcall_bytes) {
                    native_alias_target[i] = target->second;
                    continue;
                }
            } else {
                nt_by_suffix.emplace(entry.name.substr(2), i);
            }
            service_number[i] = service_count++;
        }
    }

    // Preserve unique, non-zero registered ordinals. Most Vitreo exports are
    // name-only (ordinal zero), so assign those the lowest unused ordinals.
    std::unordered_set<u16> used_ordinals;
    for (auto& entry : exports) {
        if (entry.requested_ordinal != 0 &&
            used_ordinals.insert(entry.requested_ordinal).second) {
            entry.ordinal = entry.requested_ordinal;
        }
    }
    u32 next_ordinal = 1;
    for (auto& entry : exports) {
        if (entry.ordinal != 0) continue;
        while (next_ordinal <= 0xFFFFu &&
               used_ordinals.contains(static_cast<u16>(next_ordinal))) {
            ++next_ordinal;
        }
        if (next_ordinal > 0xFFFFu) {
            exports.clear();
            break;
        }
        entry.ordinal = static_cast<u16>(next_ordinal);
        used_ordinals.insert(entry.ordinal);
        ++next_ordinal;
    }

    if (!exports.empty()) {
        const auto [min_it, max_it] = std::minmax_element(
            exports.begin(), exports.end(),
            [](const SyntheticExport& lhs, const SyntheticExport& rhs) {
                return lhs.ordinal < rhs.ordinal;
            });
        const u32 ordinal_base = min_it->ordinal;
        const u32 function_count =
            static_cast<u32>(max_it->ordinal) - ordinal_base + 1;
        const u32 export_rva = 0x1000;
        usize cursor = export_rva + sizeof(pe::ExportDirectory);
        const u32 functions_rva = static_cast<u32>(cursor);
        cursor += static_cast<usize>(function_count) * sizeof(u32);
        const u32 names_rva = static_cast<u32>(cursor);
        cursor += exports.size() * sizeof(u32);
        const u32 ordinals_rva = static_cast<u32>(cursor);
        cursor += exports.size() * sizeof(u16);

        const u32 dll_name_rva = static_cast<u32>(cursor);
        cursor += normalized_dll.size() + 1;
        std::vector<u32> name_rvas;
        name_rvas.reserve(exports.size());
        for (const auto& entry : exports) {
            name_rvas.push_back(static_cast<u32>(cursor));
            cursor += entry.name.size() + 1;
        }

        const usize export_data_end = cursor;
        // Export metadata is data, even though the native-service veneers
        // below are executable. Keep them on separate pages so native64's
        // instruction translation never decodes and mutates export strings.
        cursor = (cursor + 0xFFFu) & ~usize{0xFFFu};
        code_rva = cursor;
        // A sandbox may parse ntdll's PE exports directly and validate Nt*/Zw*
        // targets as x64 native-service entry points before replacing them in
        // the suspended child. Normal import and GetProcAddress resolution
        // still returns registered host-dispatch thunks; only these directly
        // parsed in-image veneers need the Windows-observable service-entry
        // shape.
        const bool ntdll_image = normalized_dll == "ntdll.dll";
        // Service stubs first, at Windows' strides (32 bytes x64, 16 bytes
        // WoW64, ntdll_service_layout_probe), in service-number order; then a
        // jump veneer for every other export. A Zw* twin shares its Nt* stub.
        const usize service_stride = is_64bit ? 32u : 16u;
        const usize veneer_size = is_64bit ? 14u : 7u;
        std::vector<usize> stub_rvas(exports.size(), 0);
        usize veneer_cursor = cursor + service_count * service_stride;
        for (usize i = 0; i < exports.size(); ++i) {
            if (service_number[i].has_value()) {
                stub_rvas[i] = cursor + *service_number[i] * service_stride;
            } else if (!native_alias_target[i].has_value()) {
                stub_rvas[i] = veneer_cursor;
                veneer_cursor += veneer_size;
            }
        }
        for (usize i = 0; i < exports.size(); ++i) {
            if (native_alias_target[i].has_value()) {
                stub_rvas[i] = stub_rvas[*native_alias_target[i]];
            }
        }
        const usize stubs_end = veneer_cursor;
        std::vector<GuestAddr> service_thunks(service_count, 0);
        if (stubs_end <= resource_rva) {
            const usize data_directory =
                optional_offset + (is_64bit ? 0x70u : 0x60u);
            put32(data_directory + 0, export_rva);
            // Keep jump veneers outside the directory range. A function RVA
            // inside that range denotes a forwarded export on Windows.
            put32(data_directory + 4,
                  static_cast<u32>(export_data_end - export_rva));

            pe::ExportDirectory directory{};
            directory.Name = dll_name_rva;
            directory.Base = ordinal_base;
            directory.NumberOfFunctions = function_count;
            directory.NumberOfNames = static_cast<u32>(exports.size());
            directory.AddressOfFunctions = functions_rva;
            directory.AddressOfNames = names_rva;
            directory.AddressOfNameOrdinals = ordinals_rva;
            std::memcpy(
                image.data() + export_rva, &directory, sizeof(directory));

            put_string(dll_name_rva, normalized_dll);
            u32 query_process_rva = 0;
            for (usize i = 0; i < exports.size(); ++i) {
                const auto& entry = exports[i];
                const u32 ordinal_index =
                    static_cast<u32>(entry.ordinal) - ordinal_base;
                const u32 stub_rva = static_cast<u32>(stub_rvas[i]);
                const GuestAddr export_address = image_base + stub_rva;
                put32(functions_rva + ordinal_index * sizeof(u32), stub_rva);
                put32(names_rva + i * sizeof(u32), name_rvas[i]);
                put16(ordinals_rva + i * sizeof(u16),
                      static_cast<u16>(ordinal_index));
                put_string(name_rvas[i], entry.name);
                published_exports.by_name.emplace(entry.name, export_address);
                published_exports.by_ordinal.emplace(entry.ordinal,
                                                      export_address);
                if (module_diag_enabled() && ntdll_image) {
                    std::fprintf(stderr,
                                 "[module-diag] ntdll export=%s rva=0x%x ordinal=%u\n",
                                 entry.name.c_str(), stub_rva, entry.ordinal);
                }
                if (native_alias_target[i].has_value()) continue;
                if (entry.name == "NtQueryInformationProcess") {
                    query_process_rva = stub_rva;
                }

                const bool native_service = service_number[i].has_value();
                if (native_service) {
                    service_thunks[*service_number[i]] = entry.thunk;
                }
                const bool debug_breakpoint =
                    ntdll_image &&
                    (entry.name == "DbgBreakPoint" ||
                     entry.name == "DbgUserBreakPoint");
                if (debug_breakpoint) {
                    // These ntdll routines are executable breakpoint entry
                    // points, not host-dispatch veneers. A direct call must
                    // execute INT3 and, if exception handling continues,
                    // return to its caller.
                    image[stub_rva + 0] = 0xCC;
                    image[stub_rva + 1] = 0xC3;
                } else if (native_service && is_64bit) {
                    // Windows x64 ntdll service stub, all 32 bytes of it
                    // (ntdll_service_layout_probe):
                    //   mov r10, rcx
                    //   mov eax, service_number
                    //   test byte ptr [SharedUserData+0x308], 1
                    //   jne  +3
                    //   syscall
                    //   ret
                    //   int  2Eh
                    //   ret
                    //   nop dword ptr [rax+rax+0]
                    // Direct PE parsers use this full shape to distinguish an
                    // unmodified service entry from a detour. The trailing
                    // `ret` sits at +0x17.
                    const u32 number = *service_number[i];
                    image[stub_rva + 0] = 0x4C;
                    image[stub_rva + 1] = 0x8B;
                    image[stub_rva + 2] = 0xD1;
                    image[stub_rva + 3] = 0xB8;
                    put32(stub_rva + 4, number);
                    image[stub_rva + 8] = 0xF6;
                    image[stub_rva + 9] = 0x04;
                    image[stub_rva + 10] = 0x25;
                    put32(stub_rva + 11, 0x7FFE0308u);
                    image[stub_rva + 15] = 0x01;
                    image[stub_rva + 16] = 0x75;
                    image[stub_rva + 17] = 0x03;
                    image[stub_rva + 18] = 0x0F;
                    image[stub_rva + 19] = 0x05;
                    image[stub_rva + 20] = 0xC3;
                    image[stub_rva + 21] = 0xCD;
                    image[stub_rva + 22] = 0x2E;
                    image[stub_rva + 23] = 0xC3;
                    static constexpr Byte kNopDword[] = {
                        0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
                    std::copy(std::begin(kNopDword), std::end(kNopDword),
                              image.begin() + static_cast<std::ptrdiff_t>(
                                                  stub_rva + 24));
                    // The `syscall` enters the service EAX names, as it does
                    // on Windows, wherever it is reached from: a guest may
                    // jump to it with its own number (an indirect syscall).
                    // A decoding backend dispatches it by EAX. native64 runs
                    // these bytes on the real CPU, where a `syscall` is a
                    // silent no-op, so on that host
                    // KUSER_SHARED_DATA.SystemCall selects `int 2Eh`, which
                    // traps and is dispatched from the fault path by this
                    // stub's address.
                    if (register_aliases && !registry.register_dispatch_alias(
                            image_base + stub_rva + 21,
                            entry.thunk)) {
                        std::fprintf(
                            stderr,
                            "[module] failed to register int-2E service "
                            "dispatch alias name=\"%s\" export=\"%s\" "
                            "address=0x%llx thunk=0x%llx\n",
                            normalized_dll.c_str(), entry.name.c_str(),
                            static_cast<unsigned long long>(
                                image_base + stub_rva + 21),
                            static_cast<unsigned long long>(
                                entry.thunk));
                    }
                } else if (native_service) {
                    // mov eax, service_number; mov edx, esp; sysenter; ret n,
                    // padded with NOPs to Windows' 16-byte WoW64 stride.
                    // KiFastSystemCall contract (Intel SDM SYSENTER does not
                    // save EIP; the instruction after SYSENTER returns). The
                    // SYSENTER is dispatched by EAX, like the x64 `syscall`.
                    std::fill_n(image.begin() +
                                    static_cast<std::ptrdiff_t>(stub_rva),
                                service_stride, Byte{0x90});
                    image[stub_rva + 0] = 0xB8;
                    put32(stub_rva + 1, *service_number[i]);
                    image[stub_rva + 5] = 0x89;
                    image[stub_rva + 6] = 0xE2;
                    image[stub_rva + 7] = 0x0F;
                    image[stub_rva + 8] = 0x34;
                    if (entry.stdcall_bytes == 0) {
                        image[stub_rva + 9] = 0xC3;
                    } else {
                        image[stub_rva + 9] = 0xC2;
                        put16(stub_rva + 10, entry.stdcall_bytes);
                    }
                } else if (is_64bit) {
                    // jmp qword ptr [rip+0]; dq target. To the export's
                    // guest implementation when it has one
                    // (guest_accel.hpp), so the import links through
                    // ordinary guest code. Every register reaches the export
                    // as its caller left it, as a jump to the export's own
                    // code does on Windows: __chkstk takes its size in RAX
                    // and changes no register at all
                    // (k32_ntdll_misc_forwarder_probe), which a
                    // `mov rax, imm64; jmp rax` veneer could not keep.
                    image[stub_rva + 0] = 0xFF;
                    image[stub_rva + 1] = 0x25;
                    put32(stub_rva + 2, 0);
                    put64(stub_rva + 6, registry.guest_entry(entry.thunk));
                } else {
                    // mov eax, imm32; jmp eax
                    image[stub_rva + 0] = 0xB8;
                    put32(stub_rva + 1, static_cast<u32>(entry.thunk));
                    image[stub_rva + 5] = 0xFF;
                    image[stub_rva + 6] = 0xE0;
                }
            }
            // The stubs' SYSCALL / SYSENTER / INT 2Eh enter the service EAX
            // names; publish the table that number indexes.
            if (ntdll_image && register_aliases) {
                registry.publish_native_services(std::move(service_thunks));
            }
            // Windows ntdll publishes a RUNTIME_FUNCTION for every service
            // veneer. Without one, an exception whose PC is inside a stub is
            // not a leaf the unwinder can step out of, and the process
            // terminates. One leaf entry covers the whole veneer range: none
            // of these stubs allocate a frame, so the return address is [RSP].
            if (ntdll_image && is_64bit && stubs_end + 16 <= resource_rva) {
                const u32 unwind_rva = static_cast<u32>(stubs_end);
                image[unwind_rva + 0] = 0x01;
                image[unwind_rva + 1] = 0;
                image[unwind_rva + 2] = 0;
                image[unwind_rva + 3] = 0;
                const u32 rf_rva = unwind_rva + 4;
                put32(rf_rva + 0, static_cast<u32>(code_rva));
                put32(rf_rva + 4, static_cast<u32>(stubs_end));
                put32(rf_rva + 8, unwind_rva);
                put32(data_directory + 24, rf_rva);
                put32(data_directory + 28, 12);
                ntdll_exception_rva = rf_rva;
                ntdll_exception_size = 12;
            }
            if (module_diag_enabled()) {
                std::fprintf(
                    stderr,
                    "[module-diag] synthetic name=\"%s\" base=0x%llx "
                    "exports=%zu ordinal_base=%u functions=%u "
                    "directory=0x%x+0x%zx NtQueryInformationProcess=0x%x\n",
                    normalized_dll.c_str(),
                    static_cast<unsigned long long>(image_base),
                    exports.size(), ordinal_base, function_count, export_rva,
                    export_data_end - export_rva, query_process_rva);
            }
        } else {
            std::fprintf(
                stderr,
                "[module] synthetic export image overflow name=\"%s\" "
                "exports=%zu required=0x%zx slot=0x%zx\n",
                normalized_dll.c_str(), exports.size(), stubs_end,
                image.size());
        }
    }

    static constexpr char export_section_name[] = ".edata";
    std::copy(std::begin(export_section_name),
              std::end(export_section_name) - 1,
              image.begin() + static_cast<std::ptrdiff_t>(section_offset));
    put32(section_offset + 0x08,
          static_cast<u32>(code_rva - 0x1000)); // VirtualSize
    put32(section_offset + 0x0C, 0x1000);     // VirtualAddress
    put32(section_offset + 0x10,
          static_cast<u32>(code_rva - 0x1000)); // SizeOfRawData
    put32(section_offset + 0x14, 0x1000);       // PointerToRawData
    put32(section_offset + 0x24, 0x40000040); // initialized data | read

    static constexpr char code_section_name[] = ".text";
    const usize code_section_offset = section_offset + 40;
    std::copy(std::begin(code_section_name), std::end(code_section_name) - 1,
              image.begin() +
                  static_cast<std::ptrdiff_t>(code_section_offset));
    // .text ends where .rsrc begins; an export image too large to leave that
    // page (none is: ucrtbase, the largest, ends near 0xB100) keeps the old
    // two-section layout with .text to the end of the slot.
    const bool with_resource = code_rva <= resource_rva;
    const usize code_end = with_resource ? resource_rva : image_size;
    put32(code_section_offset + 0x08,
          static_cast<u32>(code_end - code_rva)); // VirtualSize
    put32(code_section_offset + 0x0C,
          static_cast<u32>(code_rva)); // VirtualAddress
    put32(code_section_offset + 0x10,
          static_cast<u32>(code_end - code_rva)); // SizeOfRawData
    put32(code_section_offset + 0x14,
          static_cast<u32>(code_rva)); // PointerToRawData
    put32(code_section_offset + 0x24, 0x60000020); // code | execute | read

    // PE format, "The .rsrc Section": a three-level directory tree (type,
    // name, language) whose offsets are relative to the section start, ending
    // in an IMAGE_RESOURCE_DATA_ENTRY whose OffsetToData is an RVA.
    // RT_VERSION (16), VS_VERSION_INFO (1), language 0x0409 -- the language
    // of the block's own Translation entry.
    usize resource_initialized = 0;
    if (with_resource) {
        const std::vector<u8> version =
            make_synthetic_system_version_resource(normalized_dll);
        // Each directory is a 16-byte header and one 8-byte entry.
        constexpr usize kRoot = 0x00, kNameDir = 0x18, kLangDir = 0x30;
        constexpr usize kDataEntry = 0x48, kData = 0x58;
        if (!version.empty() && kData + version.size() <= 0x1000) {
            constexpr u32 kSubdirectory = 0x80000000u;
            const auto directory = [&](usize at, u32 id, u32 target) {
                put16(resource_rva + at + 14, 1);   // NumberOfIdEntries
                put32(resource_rva + at + 16, id);  // entry: Id
                put32(resource_rva + at + 20, target);
            };
            directory(kRoot, 16, kSubdirectory | kNameDir);   // RT_VERSION
            directory(kNameDir, 1, kSubdirectory | kLangDir); // VS_VERSION_INFO
            directory(kLangDir, 0x0409, kDataEntry);          // leaf
            put32(resource_rva + kDataEntry,
                  static_cast<u32>(resource_rva + kData));    // OffsetToData (RVA)
            put32(resource_rva + kDataEntry + 4,
                  static_cast<u32>(version.size()));          // Size
            std::copy(version.begin(), version.end(),
                      image.begin() +
                          static_cast<std::ptrdiff_t>(resource_rva + kData));
            const usize resource_size = kData + version.size();
            const usize data_directory =
                optional_offset + (is_64bit ? 0x70u : 0x60u);
            put32(data_directory + 16, static_cast<u32>(resource_rva));
            put32(data_directory + 20, static_cast<u32>(resource_size));

            static constexpr char resource_section_name[] = ".rsrc";
            const usize resource_section_offset = code_section_offset + 40;
            std::copy(std::begin(resource_section_name),
                      std::end(resource_section_name) - 1,
                      image.begin() + static_cast<std::ptrdiff_t>(
                                          resource_section_offset));
            put32(resource_section_offset + 0x08,
                  static_cast<u32>(resource_size));           // VirtualSize
            put32(resource_section_offset + 0x0C,
                  static_cast<u32>(resource_rva));            // VirtualAddress
            put32(resource_section_offset + 0x10,
                  static_cast<u32>(image_size - resource_rva)); // SizeOfRawData
            put32(resource_section_offset + 0x14,
                  static_cast<u32>(resource_rva));            // PointerToRawData
            put32(resource_section_offset + 0x24,
                  0x40000040); // initialized data | read
            put16(nt_offset + 6, 3);
            resource_initialized = image_size - resource_rva;
        }
    }
    // The optional-header aggregates describe the section table, not merely
    // loader allocation. Keep them coherent with the file-backed sections so
    // a guest parsing the on-disk PE observes a valid image.
    put32(optional_offset + 0x04,
          static_cast<u32>(code_end - code_rva)); // SizeOfCode
    put32(optional_offset + 0x08,
          static_cast<u32>(code_rva - 0x1000 +
                           resource_initialized)); // SizeOfInitializedData
    put32(optional_offset + 0x14, static_cast<u32>(code_rva)); // BaseOfCode

    // IMAGE_OPTIONAL_HEADER.CheckSum is the folded sum of every 16-bit word
    // in the file (with this field treated as zero), plus the file length.
    // CheckSumMappedFile exposes it and system-image integrity consumers may
    // verify it directly from the bytes returned by NtReadFile.
    const usize checksum_offset = optional_offset + 0x40;
    u64 checksum = 0;
    for (usize offset = 0; offset < image.size(); offset += 2) {
        u16 word = 0;
        if (offset != checksum_offset && offset != checksum_offset + 2) {
            word = image[offset];
            if (offset + 1 < image.size()) {
                word |= static_cast<u16>(image[offset + 1]) << 8;
            }
        }
        checksum += word;
        checksum = (checksum & 0xFFFFu) + (checksum >> 16);
    }
    checksum = (checksum & 0xFFFFu) + (checksum >> 16);
    checksum += image.size();
    put32(checksum_offset, static_cast<u32>(checksum));
    return SyntheticPeImage{
        std::move(image), std::move(published_exports), code_rva,
        code_end - code_rva, ntdll_exception_rva, ntdll_exception_size};
}

[[nodiscard]] GuestAddr well_known_synthetic_base(
    std::string_view dll_base,
    bool is_64bit,
    GuestAddr x64_synthetic_arena
) {
    if (dll_base == "kernel32") {
        return is_64bit ? x64_synthetic_arena : KERNEL32_BASE;
    }
    if (dll_base == "ntdll") {
        return is_64bit ? x64_synthetic_arena + 0x10000 : NTDLL_BASE;
    }
    if (dll_base == "user32") {
        return is_64bit ? x64_synthetic_arena + 0x20000 : USER32_BASE;
    }
    return 0;
}

[[nodiscard]] Result<std::vector<u8>, capsule::VfsError> module_read_all_bytes(
    capsule::Vfs& vfs,
    std::string_view win_path
) {
    auto h = vfs.open(win_path,
                      capsule::AccessMode::Read,
                      capsule::ShareMode::Read,
                      capsule::CreationDisposition::OpenExisting);
    if (h.is_err()) {
        return Result<std::vector<u8>, capsule::VfsError>::err(h.error());
    }

    std::vector<u8> data;
    // Best-effort reserve (avoids dependence on size being stable at stat-time).
    if (auto st = vfs.stat(win_path); st.is_ok()) {
        data.reserve(static_cast<usize>(st.value().size));
    }

    std::array<u8, 64 * 1024> buf{};
    for (;;) {
        auto chunk = vfs.read(h.value(), std::span<u8>(buf.data(), buf.size()));
        if (chunk.is_err()) {
            (void)vfs.close(h.value());
            return Result<std::vector<u8>, capsule::VfsError>::err(chunk.error());
        }
        if (chunk.value() == 0) {
            break;
        }
        data.insert(data.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(chunk.value()));
    }

    (void)vfs.close(h.value());
    return Result<std::vector<u8>, capsule::VfsError>::ok(std::move(data));
}

[[nodiscard]] ParsedExports parse_exports_from_file(
    std::span<const u8> pe_data,
    const pe::ParsedPe& pe,
    GuestAddr image_base
) {
    ParsedExports out{};
    if (!pe.has_exports()) {
        return out;
    }

    const u32 exp_rva = pe.data_directories[pe::DIR_EXPORT].VirtualAddress;
    auto exp_off_res = pe.rva_to_file_offset(exp_rva);
    if (exp_off_res.is_err()) {
        return out;
    }
    const u32 exp_off = exp_off_res.value();
    if (exp_off + sizeof(pe::ExportDirectory) > pe_data.size()) {
        return out;
    }

    pe::ExportDirectory dir{};
    std::memcpy(&dir, pe_data.data() + exp_off, sizeof(dir));
    if (dir.NumberOfFunctions == 0 || dir.AddressOfFunctions == 0) {
        return out;
    }

    // Ordinal table: fill all function RVAs
    auto eat_off_res = pe.rva_to_file_offset(dir.AddressOfFunctions);
    if (eat_off_res.is_err()) {
        return out;
    }
    const u32 eat_off = eat_off_res.value();
    const usize eat_bytes = static_cast<usize>(dir.NumberOfFunctions) * sizeof(u32);
    if (static_cast<usize>(eat_off) + eat_bytes > pe_data.size()) {
        return out;
    }
    // An EAT entry whose RVA lands inside the export directory itself is a
    // forwarder: the "address" is really an ASCII "TargetDll.ExportName"
    // string (Microsoft Learn, PE Format / Export Address Table). Handing the
    // guest image_base + that RVA hands it the string, and calling it faults
    // with STATUS_ILLEGAL_INSTRUCTION somewhere in .rdata.
    const u32 exp_size = pe.data_directories[pe::DIR_EXPORT].Size;
    const auto read_forwarder = [&](u32 rva) -> std::string {
        auto off_res = pe.rva_to_file_offset(rva);
        if (off_res.is_err()) {
            return {};
        }
        std::string text;
        for (usize p = off_res.value(); p < pe_data.size(); ++p) {
            const char c = static_cast<char>(pe_data[p]);
            if (c == '\0') {
                break;
            }
            text.push_back(c);
        }
        return text;
    };

    for (u32 i = 0; i < dir.NumberOfFunctions; ++i) {
        u32 func_rva = 0;
        std::memcpy(&func_rva, pe_data.data() + eat_off + i * sizeof(u32), sizeof(u32));
        if (func_rva == 0) {
            // A hole in the ordinal space: no export with this ordinal.
            continue;
        }
        const u16 ordinal = static_cast<u16>(dir.Base + i);
        if (func_rva >= exp_rva && func_rva < exp_rva + exp_size) {
            std::string target = read_forwarder(func_rva);
            if (!target.empty()) {
                out.forwarders_by_ordinal[ordinal] = std::move(target);
                continue;
            }
        }
        out.by_ordinal[ordinal] = static_cast<GuestAddr>(image_base + func_rva);
    }

    // Name table: map names to ordinals and RVAs
    if (dir.NumberOfNames == 0 || dir.AddressOfNames == 0 || dir.AddressOfNameOrdinals == 0) {
        return out;
    }

    auto names_off_res = pe.rva_to_file_offset(dir.AddressOfNames);
    auto ords_off_res  = pe.rva_to_file_offset(dir.AddressOfNameOrdinals);
    if (names_off_res.is_err() || ords_off_res.is_err()) {
        return out;
    }
    const u32 names_off = names_off_res.value();
    const u32 ords_off  = ords_off_res.value();
    const usize names_bytes = static_cast<usize>(dir.NumberOfNames) * sizeof(u32);
    const usize ords_bytes  = static_cast<usize>(dir.NumberOfNames) * sizeof(u16);
    if (static_cast<usize>(names_off) + names_bytes > pe_data.size() ||
        static_cast<usize>(ords_off) + ords_bytes > pe_data.size()) {
        return out;
    }

    for (u32 i = 0; i < dir.NumberOfNames; ++i) {
        u32 name_rva = 0;
        u16 ord_index = 0;
        std::memcpy(&name_rva, pe_data.data() + names_off + i * sizeof(u32), sizeof(u32));
        std::memcpy(&ord_index, pe_data.data() + ords_off + i * sizeof(u16), sizeof(u16));

        auto name_off_res = pe.rva_to_file_offset(name_rva);
        if (name_off_res.is_err()) {
            continue;
        }
        const u32 name_off = name_off_res.value();
        if (name_off >= pe_data.size()) {
            continue;
        }
        std::string func_name;
        for (usize p = name_off; p < pe_data.size(); ++p) {
            char c = static_cast<char>(pe_data[p]);
            if (c == '\0') {
                break;
            }
            func_name.push_back(c);
        }
        if (func_name.empty()) {
            continue;
        }

        const u16 ordinal = static_cast<u16>(dir.Base + ord_index);
        auto it = out.by_ordinal.find(ordinal);
        if (it != out.by_ordinal.end()) {
            out.by_name.emplace(std::move(func_name), it->second);
            continue;
        }
        auto fwd = out.forwarders_by_ordinal.find(ordinal);
        if (fwd != out.forwarders_by_ordinal.end()) {
            out.forwarders_by_name.emplace(std::move(func_name), fwd->second);
        }
    }

    return out;
}

void patch_guard_cf_load_config(GuestMemory& memory,
                                ThunkRegistry& registry,
                                const pe::ParsedPe& pe,
                                GuestAddr image_base,
                                const char* dbg_path) {
    if (pe.num_data_directories <= pe::DIR_LOAD_CONFIG) {
        return;
    }

    const auto& dir = pe.data_directories[pe::DIR_LOAD_CONFIG];
    if (dir.VirtualAddress == 0 || dir.Size < sizeof(u32)) {
        return;
    }

    auto check_thunk = registry.get_thunk_address(
        "vitreo-cfg.dll", "__guard_check_icall");
    auto dispatch_thunk = registry.get_thunk_address(
        "vitreo-cfg.dll", "__guard_dispatch_icall");
    if (check_thunk.is_err() || dispatch_thunk.is_err()) {
        return;
    }
    // Guest-code stubs first; see the identical choice in run.cpp. Every
    // guarded indirect call in a CFG image reaches this pointer, so it must
    // not be a host thunk when it can be a guest `ret`.
    const auto stubs = vitreo::pe::install_guard_cf_stubs(memory);
    const GuestAddr check_target =
        stubs.check != 0 ? stubs.check : check_thunk.value();
    const GuestAddr dispatch_target =
        stubs.dispatch != 0 ? stubs.dispatch : dispatch_thunk.value();
    const bool patched = vitreo::pe::initialize_load_config_guard_cf(
        memory, pe, image_base, check_target, dispatch_target);
    VITREO_TRACE("[module] GuardCF %s for \"%s\": check=0x%llx dispatch=0x%llx\n",
        patched ? "initialized" : "initialization failed",
        dbg_path ? dbg_path : "<unknown>",
        static_cast<unsigned long long>(check_target),
        static_cast<unsigned long long>(dispatch_target));
}

[[nodiscard]] GuestAddr lookup_runtime_function_in_table(GuestMemory& memory,
                                                         GuestAddr base,
                                                         u32 exception_rva,
                                                         u32 exception_size,
                                                         GuestAddr control_pc) {
    if (exception_rva == 0 || exception_size < 12 || control_pc < base) {
        return 0;
    }

    const u32 control_rva = static_cast<u32>(control_pc - base);
    const GuestAddr table_addr = base + exception_rva;
    const u32 entry_count = exception_size / 12;

    // Microsoft PE/COFF, "The .pdata Section": the RUNTIME_FUNCTION records of
    // an x64 image are sorted in ascending BeginAddress order precisely so the
    // OS can binary-search them, and Windows resolves a ControlPc that no entry
    // covers (a leaf function) as "no entry" rather than by rescanning. Match
    // that: binary search, and report a miss as a miss.
    //
    // The table is a contiguous run of 12-byte records inside the image, so
    // resolve it to a host pointer once instead of paying two guest reads per
    // probe. If it does not resolve as one readable span (an image mapped
    // across region boundaries, a partially committed view), fall back to the
    // per-probe guest reads, which are correct either way.
    const u64 table_bytes = static_cast<u64>(entry_count) * 12;
    const u8* host_table = nullptr;
    if (table_bytes <= std::numeric_limits<usize>::max()) {
        const auto access =
            memory.probe_access(table_addr, static_cast<usize>(table_bytes));
        if (access.host_ptr != nullptr && access.readable) {
            host_table = access.host_ptr;
        }
    }

    auto entry_begin_end = [&](u32 index, u32& begin, u32& end) -> bool {
        if (host_table != nullptr) {
            std::memcpy(&begin, host_table + static_cast<usize>(index) * 12, 4);
            std::memcpy(&end, host_table + static_cast<usize>(index) * 12 + 4, 4);
            return true;
        }
        const GuestAddr entry_addr =
            table_addr + static_cast<GuestAddr>(index) * 12;
        auto b = memory.read_u32(entry_addr + 0);
        auto e = memory.read_u32(entry_addr + 4);
        if (b.is_err() || e.is_err()) return false;
        begin = b.value();
        end = e.value();
        return true;
    };

    u32 lo = 0;
    u32 hi = entry_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        u32 begin = 0;
        u32 end = 0;
        if (!entry_begin_end(mid, begin, end)) {
            return 0;
        }
        if (control_rva < begin) {
            hi = mid;
        } else if (control_rva >= end) {
            lo = mid + 1;
        } else {
            return table_addr + static_cast<GuestAddr>(mid) * 12;
        }
    }

    return 0;
}

} // namespace vitreo::winapi
