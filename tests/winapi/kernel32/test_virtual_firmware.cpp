/// @file test_virtual_firmware.cpp
/// @brief The SMBIOS table describes the host (DMTF DSP0134 3.6.0), and the
/// firmware-table APIs follow tools/oracle_probe/firmware_table_probe.expected.

#include <gtest/gtest.h>

#include "capsule/registry/registry.hpp"
#include "platform/host_hardware_fixtures.hpp"
#include "winapi/kernel32/system/hardware_description.hpp"
#include "winapi/kernel32/system/virtual_firmware.hpp"

#include <cstring>
#include <map>
#include <string>
#include <variant>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

struct Parsed {
    u8 type = 0;
    std::vector<u8> formatted;
    std::vector<std::string> strings;
    u16 u16_at(usize at) const { u16 v = 0; std::memcpy(&v, formatted.data() + at, 2); return v; }
    u32 u32_at(usize at) const { u32 v = 0; std::memcpy(&v, formatted.data() + at, 4); return v; }
    u64 u64_at(usize at) const { u64 v = 0; std::memcpy(&v, formatted.data() + at, 8); return v; }
    std::string str(usize at) const {
        const u8 n = formatted[at];
        return n == 0 || n > strings.size() ? std::string() : strings[n - 1];
    }
};

std::vector<Parsed> parse(std::span<const u8> table) {
    std::vector<Parsed> out;
    usize at = 0;
    while (at + 4 <= table.size()) {
        Parsed p;
        p.type = table[at];
        const u8 length = table[at + 1];
        p.formatted.assign(table.begin() + at, table.begin() + at + length);
        usize s = at + length;
        if (table[s] == 0 && table[s + 1] == 0) {
            s += 2;
        } else {
            while (table[s] != 0) {
                const usize end = std::find(table.begin() + s, table.end(), u8{0}) - table.begin();
                p.strings.emplace_back(table.begin() + s, table.begin() + end);
                s = end + 1;
            }
            s += 1;
        }
        out.push_back(std::move(p));
        if (out.back().type == 127) break;
        at = s;
    }
    return out;
}

class VirtualFirmwareTest : public ::testing::Test {
protected:
    const Parsed& one(u8 type) {
        static Parsed none;
        for (const Parsed& p : structures) if (p.type == type) return p;
        ADD_FAILURE() << "no type " << int(type);
        return none;
    }
    test::ScopedHostHardware host{test::m4_pro_profile()};
    std::vector<Parsed> structures = parse(virtual_firmware::smbios_table());
};

TEST_F(VirtualFirmwareTest, StructureSetAndLengths) {
    // Lengths per DSP0134 3.6.0: 0 1Ah, 1 1Bh, 2 0Fh, 3 16h, 4 32h, 7 1Bh,
    // 16 17h, 17 5Ch, 19 1Fh, 32 0Bh, 127 04h.
    std::string order;
    const std::map<u8, u8> lengths{{0, 0x1A}, {1, 0x1B}, {2, 0x0F}, {3, 0x16}, {4, 0x32}, {7, 0x1B},
                                   {16, 0x17}, {17, 0x5C}, {19, 0x1F}, {32, 0x0B}, {127, 0x04}};
    for (const Parsed& p : structures) {
        order += std::to_string(p.type) + " ";
        EXPECT_EQ(p.formatted[1], lengths.at(p.type)) << int(p.type);
    }
    EXPECT_EQ(order, "0 1 2 3 4 7 7 16 17 19 32 127 ");  // L1 and L2; no L3 on Apple silicon
}

TEST_F(VirtualFirmwareTest, FirmwareSystemBoardAndEnclosure) {
    const Parsed& bios = one(0);
    EXPECT_EQ(bios.str(0x04), "Apple Inc.");
    EXPECT_EQ(bios.str(0x05), "13822.81.10");
    EXPECT_EQ(bios.str(0x08), "01/28/2026");
    EXPECT_EQ(bios.formatted[0x14], 0xFFu);  // release fields not usable for 13822.81.10
    EXPECT_EQ(bios.formatted[0x15], 0xFFu);
    const Parsed& system = one(1);
    EXPECT_EQ(system.str(0x04), "Apple Inc.");
    EXPECT_EQ(system.str(0x05), "Mac16,8");
    EXPECT_EQ(system.str(0x06), "");
    EXPECT_EQ(system.str(0x07), "TESTSERIAL");
    EXPECT_EQ(system.str(0x19), "MX2J3LL/A");
    EXPECT_EQ(system.str(0x1A), "MacBook Pro");
    EXPECT_EQ(system.formatted[0x18], 0x06u);  // power switch
    // UUID wire format (7.2.1): the first three fields little-endian.
    const u8 uuid[16] = {0x67, 0x45, 0x23, 0x01, 0xAB, 0x89, 0xEF, 0xCD,
                         0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    EXPECT_EQ(std::memcmp(system.formatted.data() + 0x08, uuid, 16), 0);
    const Parsed& board = one(2);
    EXPECT_EQ(board.str(0x05), "J614s");
    EXPECT_EQ(board.u16_at(0x0B), 0x0300u);  // its chassis
    EXPECT_EQ(board.formatted[0x0D], 0x0Au);  // motherboard
    const Parsed& chassis = one(3);
    EXPECT_EQ(chassis.formatted[0x05], 0x0Au);  // Notebook
    EXPECT_EQ(chassis.str(0x07), "TESTSERIAL");
}

TEST_F(VirtualFirmwareTest, ProcessorAndCaches) {
    const Parsed& cpu = one(4);
    EXPECT_EQ(cpu.formatted[0x05], 0x03u);         // central processor
    EXPECT_EQ(cpu.str(0x07), "Apple Inc.");
    EXPECT_EQ(cpu.str(0x10), "Apple M4 Pro");
    EXPECT_EQ(cpu.u32_at(0x08), 0x000306C3u);      // CPUID 1 EAX, as the guest sees it
    EXPECT_EQ(cpu.u16_at(0x12), 24u);              // 24 MHz reference clock
    EXPECT_EQ(cpu.u16_at(0x14), 4512u);            // P-core maximum
    EXPECT_EQ(cpu.formatted[0x18], 0x41u);
    EXPECT_EQ(cpu.formatted[0x19], 0x06u);         // upgrade: none
    EXPECT_EQ(cpu.formatted[0x23], 14u);
    EXPECT_EQ(cpu.u16_at(0x2A), 14u);
    EXPECT_EQ(cpu.u16_at(0x30), 14u);              // thread enabled
    EXPECT_EQ(cpu.u16_at(0x1A), 0x0700u);
    EXPECT_EQ(cpu.u16_at(0x1C), 0x0701u);
    EXPECT_EQ(cpu.u16_at(0x1E), 0xFFFFu);          // no L3
    std::vector<const Parsed*> caches;
    for (const Parsed& p : structures) if (p.type == 7) caches.push_back(&p);
    ASSERT_EQ(caches.size(), 2u);
    // L1: 10 x (128K + 192K) + 4 x (64K + 128K) = 3968 KB, 8-way.
    EXPECT_EQ(caches[0]->str(0x04), "L1 Cache");
    EXPECT_EQ(caches[0]->u16_at(0x05) & 0x7, 0u);
    EXPECT_EQ(caches[0]->u16_at(0x07), 3968u);
    EXPECT_EQ(caches[0]->u32_at(0x13), 3968u);
    EXPECT_EQ(caches[0]->formatted[0x12], 0x07u);
    // L2: 2 x 16 MB + 4 MB = 36864 KB, above 32767 KB so 64K granularity.
    EXPECT_EQ(caches[1]->u16_at(0x05) & 0x7, 1u);
    EXPECT_EQ(caches[1]->u16_at(0x07), 0x8000u | (36864u / 64));
    EXPECT_EQ(caches[1]->u32_at(0x13), 36864u);
    EXPECT_EQ(caches[1]->formatted[0x12], 0x08u);   // 16-way
}

TEST_F(VirtualFirmwareTest, Memory) {
    const Parsed& array = one(16);
    EXPECT_EQ(array.formatted[0x04], 0x03u);
    EXPECT_EQ(array.formatted[0x05], 0x03u);
    EXPECT_EQ(array.formatted[0x06], 0x01u);       // ECC present, kind not exposed: Other
    EXPECT_EQ(array.u32_at(0x07), 24u * 1024 * 1024);
    EXPECT_EQ(array.u16_at(0x0D), 1u);
    const Parsed& device = one(17);
    EXPECT_EQ(device.u16_at(0x04), 0x1000u);
    EXPECT_EQ(device.u16_at(0x0C), 24u * 1024);    // MB, below the Extended Size threshold
    EXPECT_EQ(device.formatted[0x0E], 0x0Bu);      // row of chips
    EXPECT_EQ(device.formatted[0x12], 0x23u);      // LPDDR5
    EXPECT_EQ(device.str(0x17), "Micron");
    EXPECT_EQ(device.u16_at(0x15), 0u);            // speed unknown
    EXPECT_EQ(device.formatted[0x28], 0x03u);      // DRAM
    EXPECT_EQ(device.u64_at(0x3C), 24ull << 30);   // volatile size
    const Parsed& mapped = one(19);
    EXPECT_EQ(mapped.u32_at(0x04), 0u);
    EXPECT_EQ(mapped.u32_at(0x08), 24u * 1024 * 1024 - 1);
}

TEST_F(VirtualFirmwareTest, EntryPointsInTheBiosWindow) {
    const auto window = virtual_firmware::bios_window();
    ASSERT_EQ(window.size(), virtual_firmware::kBiosSize);
    auto sum = [&](usize at, usize n) { u8 s = 0; for (usize i = 0; i < n; ++i) s += window[at + i]; return s; };
    // _SM3_ (5.2.2): 18h bytes, version 3.6, revision 1, table at F0100h.
    EXPECT_EQ(std::memcmp(window.data(), "_SM3_", 5), 0);
    EXPECT_EQ(window[6], 0x18u);
    EXPECT_EQ(window[7], 3u);
    EXPECT_EQ(window[8], 6u);
    EXPECT_EQ(window[10], 1u);
    EXPECT_EQ(sum(0, 0x18), 0u);
    u64 address = 0;
    std::memcpy(&address, window.data() + 0x10, 8);
    EXPECT_EQ(address, virtual_firmware::kBiosBase + 0x100);
    const auto table = virtual_firmware::smbios_table();
    EXPECT_EQ(std::memcmp(window.data() + 0x100, table.data(), table.size()), 0);
    // _SM_ at 20h with its _DMI_ intermediate anchor.
    EXPECT_EQ(std::memcmp(window.data() + 0x20, "_SM_", 4), 0);
    EXPECT_EQ(std::memcmp(window.data() + 0x30, "_DMI_", 5), 0);
    EXPECT_EQ(sum(0x20, 0x1F), 0u);
    EXPECT_EQ(sum(0x30, 0x0F), 0u);
}

TEST_F(VirtualFirmwareTest, BiosRegistryMirrorsTheTable) {
    capsule::Registry reg;
    winapi::publish_hardware_description(reg);
    auto bios = reg.open_key(capsule::HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", false);
    ASSERT_TRUE(bios.is_ok());
    auto text = [&](const wchar_t* name) {
        auto v = reg.query_value(bios.value(), name);
        EXPECT_TRUE(v.is_ok()) << name;
        return v.is_ok() ? std::get<std::wstring>(v.value().data) : std::wstring();
    };
    auto dword = [&](const wchar_t* name) {
        auto v = reg.query_value(bios.value(), name);
        EXPECT_TRUE(v.is_ok()) << name;
        return v.is_ok() ? std::get<u32>(v.value().data) : 0u;
    };
    EXPECT_EQ(text(L"BIOSVendor"), L"Apple Inc.");
    EXPECT_EQ(text(L"BIOSVersion"), L"13822.81.10");
    EXPECT_EQ(text(L"BIOSReleaseDate"), L"01/28/2026");
    EXPECT_EQ(text(L"SystemManufacturer"), L"Apple Inc.");
    EXPECT_EQ(text(L"SystemProductName"), L"Mac16,8");
    EXPECT_EQ(text(L"SystemFamily"), L"MacBook Pro");
    EXPECT_EQ(text(L"SystemSKU"), L"MX2J3LL/A");
    EXPECT_EQ(text(L"SystemVersion"), L"");
    EXPECT_EQ(text(L"BaseBoardManufacturer"), L"Apple Inc.");
    EXPECT_EQ(text(L"BaseBoardProduct"), L"J614s");
    EXPECT_EQ(dword(L"EnclosureType"), 0x0Au);
    EXPECT_EQ(dword(L"BiosMajorRelease"), 0xFFu);
    EXPECT_EQ(dword(L"ECFirmwareMajorRelease"), 0xFFu);
    // No serial number in this key (measured).
    EXPECT_TRUE(reg.query_value(bios.value(), L"SystemSerialNumber").is_err());
    reg.close_key(bios.value());
    auto system = reg.open_key(capsule::HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System", false);
    ASSERT_TRUE(system.is_ok());
    auto id = reg.query_value(system.value(), L"Identifier");
    ASSERT_TRUE(id.is_ok());
    EXPECT_EQ(std::get<std::wstring>(id.value().data), L"AT/AT COMPATIBLE");
    auto versions = reg.query_value(system.value(), L"SystemBiosVersion");
    ASSERT_TRUE(versions.is_ok());
    EXPECT_EQ(versions.value().type, capsule::RegType::MultiSz);
    EXPECT_EQ(std::get<std::vector<std::wstring>>(versions.value().data),
              (std::vector<std::wstring>{L"13822.81.10"}));
    reg.close_key(system.value());
}

} // namespace
