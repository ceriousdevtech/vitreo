// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file vcruntime_interpose_test_support.hpp
/// @brief Shared pieces of the vcruntime140 interposer tests: a synthetic PE
///        image builder (image layout == file layout) and in-tree guest
///        reference bodies for the mem* functions. Nothing here is derived
///        from Microsoft bytes: the bodies are written for these tests.

#include "core/pe/parser/pe_structures.hpp"
#include "winapi/msvcrt/memory/vcruntime_interpose.hpp"

#include <vitreo/types.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace vcrt_test {

using vitreo::u8;
using vitreo::u16;
using vitreo::u32;
using vitreo::u64;
using vitreo::usize;
using vitreo::GuestAddr;

inline constexpr u32 kImageSize = 0x3000;
inline constexpr u32 kTextRva = 0x1000;
inline constexpr u32 kExportRva = 0x2800;

struct Image {
    std::vector<u8> bytes;
    std::vector<std::pair<std::string, u32>> rvas;  ///< export -> RVA
    [[nodiscard]] u32 rva(const std::string& name) const {
        for (const auto& [n, r] : rvas) if (n == name) return r;
        return 0;
    }
};

/// A DLL whose export directory names itself @p dir_name and exports each
/// of @p funcs (name, code). SectionAlignment == FileAlignment == 0x1000, so
/// the file bytes are also the mapped image.
inline Image build_image(bool x64, const char* dir_name,
                         std::vector<std::pair<std::string, std::vector<u8>>> funcs) {
    namespace pe = vitreo::pe;
    Image img;
    img.bytes.assign(kImageSize, 0);
    u8* d = img.bytes.data();
    constexpr u32 pe_offset = 0x80;
    auto* dos = reinterpret_cast<pe::DosHeader*>(d);
    dos->e_magic = pe::DOS_MAGIC;
    dos->e_lfanew = pe_offset;
    const u32 sig = pe::PE_SIGNATURE;
    std::memcpy(d + pe_offset, &sig, 4);
    auto* coff = reinterpret_cast<pe::CoffHeader*>(d + pe_offset + 4);
    coff->Machine = static_cast<u16>(x64 ? pe::MachineType::AMD64 : pe::MachineType::I386);
    coff->NumberOfSections = 1;
    coff->SizeOfOptionalHeader =
        static_cast<u16>(x64 ? sizeof(pe::OptionalHeader64) : sizeof(pe::OptionalHeader32));
    coff->Characteristics = pe::CoffCharacteristics::EXECUTABLE_IMAGE | pe::CoffCharacteristics::DLL;
    const usize opt = pe_offset + 4 + sizeof(pe::CoffHeader);
    if (x64) {
        auto* o = reinterpret_cast<pe::OptionalHeader64*>(d + opt);
        o->Magic = pe::PE32PLUS_MAGIC;
        o->ImageBase = 0x180000000ull;
        o->SectionAlignment = 0x1000;
        o->FileAlignment = 0x1000;
        o->SizeOfImage = kImageSize;
        o->SizeOfHeaders = 0x1000;
        o->NumberOfRvaAndSizes = pe::DIR_COUNT;
        o->DataDirectory[pe::DIR_EXPORT] = {kExportRva, 0x800};
    } else {
        auto* o = reinterpret_cast<pe::OptionalHeader32*>(d + opt);
        o->Magic = pe::PE32_MAGIC;
        o->ImageBase = 0x10000000u;
        o->SectionAlignment = 0x1000;
        o->FileAlignment = 0x1000;
        o->SizeOfImage = kImageSize;
        o->SizeOfHeaders = 0x1000;
        o->NumberOfRvaAndSizes = pe::DIR_COUNT;
        o->DataDirectory[pe::DIR_EXPORT] = {kExportRva, 0x800};
    }
    auto* sec = reinterpret_cast<pe::SectionHeader*>(d + opt + coff->SizeOfOptionalHeader);
    std::memcpy(sec->Name, ".text\0\0\0", 8);
    sec->VirtualSize = 0x2000;
    sec->VirtualAddress = kTextRva;
    sec->SizeOfRawData = 0x2000;
    sec->PointerToRawData = 0x1000;
    sec->Characteristics = pe::SCN_CNT_CODE | pe::SCN_MEM_EXECUTE | pe::SCN_MEM_READ;

    // Code, 64-byte aligned.
    u32 at = kTextRva;
    for (auto& [name, code] : funcs) {
        std::memcpy(d + at, code.data(), code.size());
        img.rvas.emplace_back(name, at);
        at += static_cast<u32>((code.size() + 63) & ~usize{63});
    }
    // Export directory: names sorted (the loader binary-searches them).
    std::vector<std::pair<std::string, u32>> sorted = img.rvas;
    std::sort(sorted.begin(), sorted.end());
    const u32 n = static_cast<u32>(sorted.size());
    const u32 functions = kExportRva + 0x40;
    const u32 names = functions + 4 * n;
    const u32 ordinals = names + 4 * n;
    u32 strings = ordinals + 2 * n + 0x10;
    pe::ExportDirectory dir{};
    dir.Base = 1;
    dir.NumberOfFunctions = n;
    dir.NumberOfNames = n;
    dir.AddressOfFunctions = functions;
    dir.AddressOfNames = names;
    dir.AddressOfNameOrdinals = ordinals;
    for (u32 i = 0; i < n; ++i) {
        std::memcpy(d + functions + 4 * i, &sorted[i].second, 4);
        std::memcpy(d + names + 4 * i, &strings, 4);
        const u16 ord = static_cast<u16>(i);
        std::memcpy(d + ordinals + 2 * i, &ord, 2);
        std::memcpy(d + strings, sorted[i].first.c_str(), sorted[i].first.size() + 1);
        strings += static_cast<u32>(sorted[i].first.size() + 1);
    }
    dir.Name = strings;
    std::memcpy(d + strings, dir_name, std::strlen(dir_name) + 1);
    std::memcpy(d + kExportRva, &dir, sizeof(dir));
    return img;
}

// ---------------------------------------------------------------------------
// Guest reference bodies (string-instruction loops), x86 __cdecl and x64.
// ---------------------------------------------------------------------------

/// memmove(dst, src, n) -> dst: backward (STD; REP MOVSB) only when dst
/// lies inside (src, src+n); memcpy uses the same body (overlap-safe).
inline std::vector<u8> ref_memmove(bool x64) {
    if (!x64) {
        return {0x56, 0x57,                    // push esi; push edi
                0x8B, 0x7C, 0x24, 0x0C,        // mov edi,[esp+0Ch]
                0x8B, 0x74, 0x24, 0x10,        // mov esi,[esp+10h]
                0x8B, 0x4C, 0x24, 0x14,        // mov ecx,[esp+14h]
                0x89, 0xF8,                    // mov eax,edi
                0x39, 0xF7,                    // cmp edi,esi
                0x76, 0x15,                    // jbe fwd
                0x8D, 0x14, 0x0E,              // lea edx,[esi+ecx]
                0x39, 0xD7,                    // cmp edi,edx
                0x73, 0x0E,                    // jae fwd
                0x8D, 0x74, 0x0E, 0xFF,        // lea esi,[esi+ecx-1]
                0x8D, 0x7C, 0x0F, 0xFF,        // lea edi,[edi+ecx-1]
                0xFD, 0xF3, 0xA4, 0xFC,        // std; rep movsb; cld
                0xEB, 0x02,                    // jmp done
                0xF3, 0xA4,                    // fwd: rep movsb
                0x5F, 0x5E, 0xC3};             // done: pop edi; pop esi; ret
    }
    return {0x57, 0x56,                        // push rdi; push rsi
            0x48, 0x89, 0xCF,                  // mov rdi,rcx
            0x48, 0x89, 0xD6,                  // mov rsi,rdx
            0x48, 0x89, 0xC8,                  // mov rax,rcx
            0x4C, 0x89, 0xC1,                  // mov rcx,r8
            0x48, 0x39, 0xF7,                  // cmp rdi,rsi
            0x76, 0x19,                        // jbe fwd
            0x48, 0x8D, 0x14, 0x0E,            // lea rdx,[rsi+rcx]
            0x48, 0x39, 0xD7,                  // cmp rdi,rdx
            0x73, 0x10,                        // jae fwd
            0x48, 0x8D, 0x74, 0x0E, 0xFF,      // lea rsi,[rsi+rcx-1]
            0x48, 0x8D, 0x7C, 0x0F, 0xFF,      // lea rdi,[rdi+rcx-1]
            0xFD, 0xF3, 0xA4, 0xFC,            // std; rep movsb; cld
            0xEB, 0x02,                        // jmp done
            0xF3, 0xA4,                        // fwd: rep movsb
            0x5E, 0x5F, 0xC3};                 // done: pop rsi; pop rdi; ret
}

/// memset(dst, c, n) -> dst.
inline std::vector<u8> ref_memset(bool x64) {
    if (!x64) {
        return {0x57,                          // push edi
                0x8B, 0x7C, 0x24, 0x08,        // mov edi,[esp+8]
                0x8B, 0x44, 0x24, 0x0C,        // mov eax,[esp+0Ch]
                0x8B, 0x4C, 0x24, 0x10,        // mov ecx,[esp+10h]
                0xF3, 0xAA,                    // rep stosb
                0x8B, 0x44, 0x24, 0x08,        // mov eax,[esp+8]
                0x5F, 0xC3};                   // pop edi; ret
    }
    return {0x57,                              // push rdi
            0x48, 0x89, 0xCF,                  // mov rdi,rcx
            0x49, 0x89, 0xC9,                  // mov r9,rcx
            0x89, 0xD0,                        // mov eax,edx
            0x4C, 0x89, 0xC1,                  // mov rcx,r8
            0xF3, 0xAA,                        // rep stosb
            0x4C, 0x89, 0xC8,                  // mov rax,r9
            0x5F, 0xC3};                       // pop rdi; ret
}

/// memcmp(a, b, n) -> -1/0/+1 (REPE CMPSB, then SBB/OR on the carry).
inline std::vector<u8> ref_memcmp(bool x64) {
    if (!x64) {
        return {0x56, 0x57,                    // push esi; push edi
                0x8B, 0x74, 0x24, 0x0C,        // mov esi,[esp+0Ch]
                0x8B, 0x7C, 0x24, 0x10,        // mov edi,[esp+10h]
                0x8B, 0x4C, 0x24, 0x14,        // mov ecx,[esp+14h]
                0x31, 0xC0,                    // xor eax,eax
                0x85, 0xC9,                    // test ecx,ecx
                0x74, 0x09,                    // jz done
                0xF3, 0xA6,                    // repe cmpsb
                0x74, 0x05,                    // je done
                0x19, 0xC0,                    // sbb eax,eax
                0x83, 0xC8, 0x01,              // or eax,1
                0x5F, 0x5E, 0xC3};             // done: pop edi; pop esi; ret
    }
    return {0x56, 0x57,                        // push rsi; push rdi
            0x48, 0x89, 0xCE,                  // mov rsi,rcx
            0x48, 0x89, 0xD7,                  // mov rdi,rdx
            0x4C, 0x89, 0xC1,                  // mov rcx,r8
            0x31, 0xC0,                        // xor eax,eax
            0x48, 0x85, 0xC9,                  // test rcx,rcx
            0x74, 0x09,                        // jz done
            0xF3, 0xA6,                        // repe cmpsb
            0x74, 0x05,                        // je done
            0x19, 0xC0,                        // sbb eax,eax
            0x83, 0xC8, 0x01,                  // or eax,1
            0x5F, 0x5E, 0xC3};                 // done: pop rdi; pop rsi; ret
}

/// memchr(p, c, n) -> address of the first byte == (u8)c, or NULL.
inline std::vector<u8> ref_memchr(bool x64) {
    if (!x64) {
        return {0x57,                          // push edi
                0x8B, 0x7C, 0x24, 0x08,        // mov edi,[esp+8]
                0x8B, 0x44, 0x24, 0x0C,        // mov eax,[esp+0Ch]
                0x8B, 0x4C, 0x24, 0x10,        // mov ecx,[esp+10h]
                0x85, 0xC9,                    // test ecx,ecx
                0x74, 0x09,                    // jz nf
                0xF2, 0xAE,                    // repne scasb
                0x75, 0x05,                    // jne nf
                0x8D, 0x47, 0xFF,              // lea eax,[edi-1]
                0x5F, 0xC3,                    // pop edi; ret
                0x31, 0xC0,                    // nf: xor eax,eax
                0x5F, 0xC3};                   // pop edi; ret
    }
    return {0x57,                              // push rdi
            0x48, 0x89, 0xCF,                  // mov rdi,rcx
            0x89, 0xD0,                        // mov eax,edx
            0x4C, 0x89, 0xC1,                  // mov rcx,r8
            0x48, 0x85, 0xC9,                  // test rcx,rcx
            0x74, 0x0A,                        // jz nf
            0xF2, 0xAE,                        // repne scasb
            0x75, 0x06,                        // jne nf
            0x48, 0x8D, 0x47, 0xFF,            // lea rax,[rdi-1]
            0x5F, 0xC3,                        // pop rdi; ret
            0x31, 0xC0,                        // nf: xor eax,eax
            0x5F, 0xC3};                       // pop rdi; ret
}

/// `ret` (stands in for the str*/wcs* bodies, which stay un-interposed).
inline std::vector<u8> ref_ret() { return {0xC3}; }

/// The five sized functions plus strchr, as exports of a reference image.
inline std::vector<std::pair<std::string, std::vector<u8>>> reference_exports(bool x64) {
    return {{"memcpy", ref_memmove(x64)}, {"memmove", ref_memmove(x64)},
            {"memset", ref_memset(x64)},  {"memcmp", ref_memcmp(x64)},
            {"memchr", ref_memchr(x64)},  {"strchr", ref_ret()}};
}

}  // namespace vcrt_test
