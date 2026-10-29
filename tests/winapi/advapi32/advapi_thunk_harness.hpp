// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file advapi_thunk_harness.hpp
/// @brief Shared harness for advapi32 contract tests that call the exported
///        thunks at either bitness and replay tools/oracle_probe .expected
///        files (Windows' own output).

#include <gtest/gtest.h>
#include <vitreo/common.hpp>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/kernel32/memory/memory.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi::advapi_test {

constexpr GuestAddr kData = 0x100000;
constexpr GuestAddr kStack = 0x180000;

inline std::vector<std::string> read_expected(const char* name) {
    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + name);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

/// Value of ` key=<hex>` in a probe line.
inline std::optional<u64> field(const std::string& line, const std::string& key) {
    const std::string pat = " " + key + "=";
    const usize at = line.find(pat);
    if (at == std::string::npos) return std::nullopt;
    return std::strtoull(line.c_str() + at + pat.size(), nullptr, 16);
}

/// Value of ` key="<text>"` (the text runs to the last quote of the line).
inline std::optional<std::string> quoted(const std::string& line, const std::string& key) {
    const std::string pat = " " + key + "=\"";
    const usize at = line.find(pat);
    if (at == std::string::npos) return std::nullopt;
    const usize start = at + pat.size();
    const usize end = line.find('"', start);
    return line.substr(start, end - start);
}

/// Value of ` key=<token>` up to the next space.
inline std::optional<std::string> token(const std::string& line, const std::string& key) {
    const std::string pat = " " + key + "=";
    const usize at = line.find(pat);
    if (at == std::string::npos) return std::nullopt;
    const usize start = at + pat.size();
    const usize end = line.find(' ', start);
    return line.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

/// The corpus string of `sd "<text>" ok=...` (quotes inside are literal).
inline std::string corpus_text(const std::string& line, const char* prefix) {
    const usize start = std::strlen(prefix);
    const usize end = line.rfind("\" ok=");
    std::string raw = line.substr(start, end - start);
    std::string out;
    for (usize i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            const char c = raw[++i];
            out.push_back(c == 't' ? '\t' : c == 'r' ? '\r' : c == 'n' ? '\n' : c);
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

/// Win64 / x86 stdcall calls into create_advapi32_security_thunks' exports.
class AdvapiThunkHarness : public ::testing::TestWithParam<bool> {
protected:
    GuestMemory memory;
    HandleTable handles;
    VirtualMemoryManager vmm{memory};
    HeapManager heap_mgr{vmm, memory, handles};
    GlobalLocalManager local{heap_mgr};
    SecurityStubs stubs{handles, &local};
    cpu::CpuState cpu;
    DllExports exports;
    bool is64 = true;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kData, 0x40000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStack, 0x2000, MemoryProtection::ReadWrite).is_ok());
        is64 = GetParam();
        if (is64) cpu.init_long_mode();
        else cpu.init_protected_mode();
        exports = create_advapi32_security_thunks(stubs, nullptr);
    }

    /// "<probe>.expected" for x64, "<probe>.x86.expected" for x86.
    std::string expected_file(const char* probe) const {
        return std::string(probe) + (is64 ? ".expected" : ".x86.expected");
    }

    u64 call(const char* name, std::initializer_list<u64> args) {
        const ExportEntry* entry = nullptr;
        for (const auto& e : exports.entries) {
            if (e.name == name) entry = &e;
        }
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return ~0ull;
        cpu.gpr.rsp = kStack + 0x800;
        if (is64) {
            u64 regs[4]{};
            usize i = 0;
            for (u64 a : args) {
                if (i < 4) regs[i] = a;
                else (void)memory.write_u64(cpu.gpr.rsp + 0x28 + (i - 4) * 8, a);
                ++i;
            }
            cpu.gpr.rcx = regs[0];
            cpu.gpr.rdx = regs[1];
            cpu.gpr.r8 = regs[2];
            cpu.gpr.r9 = regs[3];
        } else {
            usize i = 0;
            for (u64 a : args) {
                (void)memory.write_u32(cpu.gpr.rsp + 4 + i * 4, static_cast<u32>(a));
                ++i;
            }
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return is64 ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFull);
    }
    u32 stdcall_bytes(const char* name) {
        for (const auto& e : exports.entries) {
            if (e.name == name) return e.stdcall_bytes;
        }
        return ~0u;
    }
    u64 read_ptr(GuestAddr at) {
        return is64 ? memory.read_u64(at).value() : memory.read_u32(at).value();
    }
    void write_ptr(GuestAddr at, u64 v) {
        if (is64) (void)memory.write_u64(at, v);
        else (void)memory.write_u32(at, static_cast<u32>(v));
    }
    GuestAddr wstr(GuestAddr at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) {
            (void)memory.write_u16(at + i * 2, static_cast<u8>(s[i]));
        }
        (void)memory.write_u16(at + s.size() * 2, 0);
        return at;
    }
    GuestAddr astr(GuestAddr at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) (void)memory.write_u8(at + i, static_cast<u8>(s[i]));
        (void)memory.write_u8(at + s.size(), 0);
        return at;
    }
    std::string read_w(GuestAddr p) {
        std::string s;
        for (; memory.read_u16(p).value() != 0; p += 2) {
            s.push_back(static_cast<char>(memory.read_u16(p).value()));
        }
        return s;
    }
    std::string hex_bytes(GuestAddr p, u32 n) {
        std::string out;
        static const char* d = "0123456789abcdef";
        for (u32 i = 0; i < n; ++i) {
            const u8 b = memory.read_u8(p + i).value();
            out.push_back(d[b >> 4]);
            out.push_back(d[b & 15]);
        }
        return out;
    }

};

} // namespace vitreo::winapi::advapi_test
