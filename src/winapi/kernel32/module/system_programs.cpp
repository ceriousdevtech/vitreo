// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/kernel32/module/system_programs.hpp"

#include <algorithm>
#include <cctype>

namespace vitreo::winapi {

namespace {

constexpr std::string_view kPrograms[] = {"msiexec.exe", "regsvr32.exe"};

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

bool is_system_program(std::string_view leaf) noexcept {
    return std::any_of(std::begin(kPrograms), std::end(kPrograms),
                       [leaf](std::string_view p) { return iequals(p, leaf); });
}

std::vector<std::string> system_program_names() {
    return {std::begin(kPrograms), std::end(kPrograms)};
}

std::vector<u8> build_system_program_image(bool is_64bit) {
    // PE/COFF layout (Microsoft "PE Format"): headers in the first 0x400
    // bytes, .text at RVA 0x1000 and .idata at RVA 0x2000, one file page
    // each. No relocations: the image is linked at the default base of its
    // kind and nothing else is mapped in a new process before it.
    constexpr u32 kText = 0x1000, kIdata = 0x2000, kTextRaw = 0x400, kIdataRaw = 0x600;
    constexpr u32 kFileSize = 0x800, kImageSize = 0x3000;
    const u64 image_base = is_64bit ? 0x140000000ull : 0x400000ull;
    const u32 ptr = is_64bit ? 8 : 4;
    std::vector<u8> image(kFileSize, 0);
    const auto put16 = [&](usize at, u16 v) { image[at] = static_cast<u8>(v); image[at + 1] = static_cast<u8>(v >> 8); };
    const auto put32 = [&](usize at, u32 v) { for (int i = 0; i < 4; ++i) image[at + i] = static_cast<u8>(v >> (8 * i)); };
    const auto put64 = [&](usize at, u64 v) { for (int i = 0; i < 8; ++i) image[at + i] = static_cast<u8>(v >> (8 * i)); };
    const auto put_ptr = [&](usize at, u64 v) { if (is_64bit) put64(at, v); else put32(at, static_cast<u32>(v)); };
    const auto raw = [&](u32 rva) { return static_cast<usize>(rva - kIdata + kIdataRaw); };

    // .idata: two IMAGE_IMPORT_DESCRIPTORs and a null one, then for each DLL
    // its lookup table and IAT (one name, then 0), hint/names and DLL names.
    const u32 ilt0 = kIdata + 0x40, ilt1 = ilt0 + 2 * ptr;
    const u32 iat0 = ilt1 + 2 * ptr, iat1 = iat0 + 2 * ptr;
    const u32 name_run = iat1 + 2 * ptr;
    const std::string run_name(kSystemProgramEntry);
    const u32 name_exit = (name_run + 2 + static_cast<u32>(run_name.size()) + 1 + 1) & ~1u;
    const u32 dll0 = (name_exit + 2 + 12 + 1 + 1) & ~1u;   // "ExitProcess"
    const std::string sysprog(kSystemProgramDll);
    const u32 dll1 = dll0 + static_cast<u32>(sysprog.size()) + 1;
    put32(raw(kIdata) + 0, ilt0);
    put32(raw(kIdata) + 12, dll0);
    put32(raw(kIdata) + 16, iat0);
    put32(raw(kIdata) + 20, ilt1);
    put32(raw(kIdata) + 32, dll1);
    put32(raw(kIdata) + 36, iat1);
    put_ptr(raw(ilt0), name_run);
    put_ptr(raw(iat0), name_run);
    put_ptr(raw(ilt1), name_exit);
    put_ptr(raw(iat1), name_exit);
    std::copy(run_name.begin(), run_name.end(), image.begin() + static_cast<std::ptrdiff_t>(raw(name_run) + 2));
    const std::string exit_name = "ExitProcess";
    std::copy(exit_name.begin(), exit_name.end(), image.begin() + static_cast<std::ptrdiff_t>(raw(name_exit) + 2));
    std::copy(sysprog.begin(), sysprog.end(), image.begin() + static_cast<std::ptrdiff_t>(raw(dll0)));
    const std::string kernel32 = "kernel32.dll";
    std::copy(kernel32.begin(), kernel32.end(), image.begin() + static_cast<std::ptrdiff_t>(raw(dll1)));
    const u32 idata_used = dll1 + static_cast<u32>(kernel32.size()) + 1 - kIdata;

    // .text: ExitProcess(RunSystemProgram()).
    std::vector<u8> code;
    if (is_64bit) {
        code = {0x48, 0x83, 0xEC, 0x28,               // sub rsp, 28h
                0xFF, 0x15, 0, 0, 0, 0,                // call [rip+iat0]
                0x89, 0xC1,                            // mov ecx, eax
                0xFF, 0x15, 0, 0, 0, 0,                // call [rip+iat1]
                0xCC};
        const u32 d0 = iat0 - (kText + 10), d1 = iat1 - (kText + 18);
        for (int i = 0; i < 4; ++i) { code[6 + i] = static_cast<u8>(d0 >> (8 * i)); code[14 + i] = static_cast<u8>(d1 >> (8 * i)); }
    } else {
        code = {0xFF, 0x15, 0, 0, 0, 0,                // call [iat0]
                0x50,                                  // push eax
                0xFF, 0x15, 0, 0, 0, 0,                // call [iat1]
                0xCC};
        const u32 a0 = static_cast<u32>(image_base) + iat0, a1 = static_cast<u32>(image_base) + iat1;
        for (int i = 0; i < 4; ++i) { code[2 + i] = static_cast<u8>(a0 >> (8 * i)); code[9 + i] = static_cast<u8>(a1 >> (8 * i)); }
    }
    std::copy(code.begin(), code.end(), image.begin() + kTextRaw);

    // Headers.
    constexpr usize nt = 0x80;
    const usize opt = nt + 24;
    const u16 opt_size = is_64bit ? 0xF0 : 0xE0;
    put16(0x00, 0x5A4D);
    put32(0x3C, static_cast<u32>(nt));
    put32(nt, 0x00004550);
    put16(nt + 4, is_64bit ? 0x8664 : 0x014C);
    put16(nt + 6, 2);
    put16(nt + 20, opt_size);
    // EXECUTABLE_IMAGE | RELOCS_STRIPPED, plus LARGE_ADDRESS_AWARE (x64) or
    // 32BIT_MACHINE (x86).
    put16(nt + 22, is_64bit ? 0x0023 : 0x0103);
    put16(opt + 0x00, is_64bit ? 0x020B : 0x010B);
    put32(opt + 0x04, 0x200);           // SizeOfCode
    put32(opt + 0x08, 0x200);           // SizeOfInitializedData
    put32(opt + 0x10, kText);           // AddressOfEntryPoint
    put32(opt + 0x14, kText);           // BaseOfCode
    if (is_64bit) {
        put64(opt + 0x18, image_base);
    } else {
        put32(opt + 0x18, kIdata);      // BaseOfData
        put32(opt + 0x1C, static_cast<u32>(image_base));
    }
    put32(opt + 0x20, 0x1000);          // SectionAlignment
    put32(opt + 0x24, 0x200);           // FileAlignment
    put16(opt + 0x28, 6);               // MajorOperatingSystemVersion
    put16(opt + 0x30, 6);               // MajorSubsystemVersion
    put32(opt + 0x38, kImageSize);
    put32(opt + 0x3C, 0x400);           // SizeOfHeaders
    put16(opt + 0x44, 2);               // IMAGE_SUBSYSTEM_WINDOWS_GUI
    put16(opt + 0x46, 0x8100);          // NX_COMPAT | TERMINAL_SERVER_AWARE
    const usize dirs = opt + (is_64bit ? 0x70 : 0x60);
    if (is_64bit) {
        put64(opt + 0x48, 0x100000); put64(opt + 0x50, 0x1000);
        put64(opt + 0x58, 0x100000); put64(opt + 0x60, 0x1000);
        put32(opt + 0x6C, 16);
    } else {
        put32(opt + 0x48, 0x100000); put32(opt + 0x4C, 0x1000);
        put32(opt + 0x50, 0x100000); put32(opt + 0x54, 0x1000);
        put32(opt + 0x5C, 16);
    }
    put32(dirs + 8 * 1, kIdata);        // IMAGE_DIRECTORY_ENTRY_IMPORT
    put32(dirs + 8 * 1 + 4, 0x3C);
    put32(dirs + 8 * 12, iat0);         // IMAGE_DIRECTORY_ENTRY_IAT
    put32(dirs + 8 * 12 + 4, 4 * ptr);

    const usize sections = opt + opt_size;
    const auto section = [&](usize at, const char* name, u32 vsize, u32 rva, u32 rawp, u32 flags) {
        for (int i = 0; name[i] != 0; ++i) image[at + i] = static_cast<u8>(name[i]);
        put32(at + 8, vsize);
        put32(at + 12, rva);
        put32(at + 16, 0x200);
        put32(at + 20, rawp);
        put32(at + 36, flags);
    };
    section(sections, ".text", static_cast<u32>(code.size()), kText, kTextRaw, 0x60000020u);
    section(sections + 40, ".idata", idata_used, kIdata, kIdataRaw, 0xC0000040u);
    return image;
}

}  // namespace vitreo::winapi
