// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file crypt32_api_harness.hpp
/// @brief Calls crypt32 exports through their registered thunks at either
///        pointer width, the way a guest does.
///
/// Used by the oracle-replay tests: each replays a tools/oracle_probe/crypt_*
/// probe against Vitreo and compares with the Windows .expected output.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/system/crypt32/crypt32.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/memory/global_local.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <cstring>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace vitreo::winapi::test {

class CryptApi {
public:
    explicit CryptApi(bool is64) : m_is64(is64) {
        m_base = is64 ? 0x200100000ULL : 0x00100000ULL;
        EXPECT_TRUE(m_memory.map_region(m_base, kDataSize + kStackSize,
                                        MemoryProtection::ReadWrite).is_ok());
        if (is64) m_cpu.init_long_mode(); else m_cpu.init_protected_mode();
        m_vmm = std::make_unique<VirtualMemoryManager>(m_memory);
        m_handles = std::make_unique<HandleTable>();
        m_heap = std::make_unique<HeapManager>(*m_vmm, m_memory, *m_handles);
        m_local = std::make_unique<GlobalLocalManager>(*m_heap);
        m_local->set_guest_64bit(is64);
        m_mgr.set_local_memory(m_local.get());
        m_exports = create_crypt32_thunks(m_mgr);
        m_next = m_base;
    }

    [[nodiscard]] bool is64() const { return m_is64; }
    [[nodiscard]] u32 ptr_size() const { return m_is64 ? 8u : 4u; }
    [[nodiscard]] GuestMemory& memory() { return m_memory; }
    [[nodiscard]] Crypt32Manager& manager() { return m_mgr; }
    [[nodiscard]] cpu::CpuState& cpu() { return m_cpu; }
    /// LocalSize() of a LocalAlloc'd block (0 when it is not one).
    [[nodiscard]] u64 local_size(GuestAddr block) { return m_local->global_size(block); }

    [[nodiscard]] const ExportEntry* find(const char* name) const {
        for (const auto& entry : m_exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    }

    /// Call an export with the guest calling convention; returns RAX/EAX.
    u64 call(const char* name, std::initializer_list<u64> args) {
        const ExportEntry* entry = find(name);
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return 0;
        const GuestAddr sp = m_base + kDataSize + kStackSize - 0x400;
        m_cpu.set_sp(sp);
        usize index = 0;
        for (const u64 value : args) {
            if (m_is64) {
                switch (index) {
                    case 0: m_cpu.gpr.rcx = value; break;
                    case 1: m_cpu.gpr.rdx = value; break;
                    case 2: m_cpu.gpr.r8 = value; break;
                    case 3: m_cpu.gpr.r9 = value; break;
                    default:
                        EXPECT_TRUE(m_memory.write_u64(sp + 8 + 8 * index, value).is_ok());
                }
            } else {
                EXPECT_TRUE(m_memory.write_u32(sp + 4 + 4 * index,
                                               static_cast<u32>(value)).is_ok());
            }
            ++index;
        }
        m_cpu.gpr.rax = 0xBADBADBADBADull;
        Marshaler marshaler(m_memory);
        marshaler.attach_cpu(m_cpu);
        entry->handler(marshaler);
        return m_is64 ? m_cpu.gpr.rax : static_cast<u32>(m_cpu.gpr.rax);
    }

    /// Bump-allocate guest bytes in the data region.
    GuestAddr alloc(usize size, usize align = 16) {
        m_next = (m_next + align - 1) / align * align;
        const GuestAddr at = m_next;
        m_next += size == 0 ? 1 : size;
        EXPECT_LE(m_next, m_base + kDataSize);
        for (usize i = 0; i < size; ++i) (void)m_memory.write_u8(at + i, 0);
        return at;
    }
    GuestAddr put(const void* data, usize size, usize align = 16) {
        const GuestAddr at = alloc(size, align);
        if (size) EXPECT_TRUE(m_memory.write_bytes(at, static_cast<const u8*>(data), size).is_ok());
        return at;
    }
    GuestAddr put_string(const std::string& text) {
        return put(text.c_str(), text.size() + 1, 1);
    }
    GuestAddr put_wstring(const std::u16string& text) {
        return put(text.c_str(), (text.size() + 1) * 2, 2);
    }
    /// CRYPT_DATA_BLOB {cbData; pbData} at the guest pointer width.
    GuestAddr put_blob(GuestAddr data, u32 size) {
        const GuestAddr at = alloc(m_is64 ? 16 : 8, 8);
        write_u32(at, size);
        write_ptr(at + ptr_size(), data);
        return at;
    }

    u32 u32_at(GuestAddr at) { return m_memory.read_u32(at).value_or(0xFEEEFEEE); }
    u64 ptr_at(GuestAddr at) {
        return m_is64 ? m_memory.read_u64(at).value_or(0) : m_memory.read_u32(at).value_or(0);
    }
    u8 u8_at(GuestAddr at) { return m_memory.read_u8(at).value_or(0xEE); }
    void write_u32(GuestAddr at, u32 v) { EXPECT_TRUE(m_memory.write_u32(at, v).is_ok()); }
    void write_ptr(GuestAddr at, u64 v) {
        if (m_is64) EXPECT_TRUE(m_memory.write_u64(at, v).is_ok());
        else EXPECT_TRUE(m_memory.write_u32(at, static_cast<u32>(v)).is_ok());
    }
    std::string cstring_at(GuestAddr at) {
        std::string out;
        for (;;) {
            const u8 c = u8_at(at++);
            if (c == 0 || out.size() > 4096) break;
            out.push_back(static_cast<char>(c));
        }
        return out;
    }
    std::string wstring_at(GuestAddr at) {
        std::string out;
        for (;;) {
            const u16 c = m_memory.read_u16(at).value_or(0);
            at += 2;
            if (c == 0 || out.size() > 4096) break;
            out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        return out;
    }
    std::vector<u8> bytes_at(GuestAddr at, usize n) {
        std::vector<u8> out(n);
        if (n) EXPECT_TRUE(m_memory.read_bytes(at, out.data(), n).is_ok());
        return out;
    }

private:
    static constexpr u64 kDataSize = 0x100000;
    static constexpr u64 kStackSize = 0x10000;
    bool m_is64;
    GuestAddr m_base;
    GuestAddr m_next;
    GuestMemory m_memory;
    cpu::CpuState m_cpu;
    std::unique_ptr<VirtualMemoryManager> m_vmm;
    std::unique_ptr<HandleTable> m_handles;
    std::unique_ptr<HeapManager> m_heap;
    std::unique_ptr<GlobalLocalManager> m_local;
    Crypt32Manager m_mgr;
    DllExports m_exports;
};

/// Output formatting identical to tools/oracle_probe/crypt_probe_util.h.
inline std::string hex(u64 value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(value));
    return buffer;
}
inline std::string bytes_hex(const std::vector<u8>& bytes) {
    std::string out;
    char buffer[4];
    for (const u8 b : bytes) {
        std::snprintf(buffer, sizeof(buffer), "%02x", b);
        out += buffer;
    }
    return out;
}

inline std::vector<std::string> read_expected(const std::string& name) {
    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + name);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

/// Compare replayed lines against the Windows transcript line by line, so a
/// failure names the first differing measurement.
inline void expect_transcript(const std::vector<std::string>& expected,
                              const std::vector<std::string>& actual) {
    ASSERT_FALSE(expected.empty()) << "missing .expected file";
    const usize n = std::min(expected.size(), actual.size());
    for (usize i = 0; i < n; ++i) {
        EXPECT_EQ(actual[i], expected[i]) << "line " << (i + 1);
    }
    EXPECT_EQ(actual.size(), expected.size());
}

} // namespace vitreo::winapi::test
