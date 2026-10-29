#include "virtual_firmware.hpp"

#include "core/cpu/cpuid_synth.hpp"
#include "platform/host_hardware.hpp"
#include "winapi/kernel32/system/processor_topology.hpp"

#include <vitreo/virtual_tsc.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>

namespace vitreo::winapi::virtual_firmware {
namespace {

constexpr usize kSmbiosTableOffset = 0x100;
constexpr usize kLegacyEntryOffset = 0x20;

// Structure handles: the type in the high byte, so they read like the table.
constexpr u16 kHandleBios = 0x0000, kHandleSystem = 0x0100, kHandleBoard = 0x0200,
              kHandleChassis = 0x0300, kHandleProcessor = 0x0400, kHandleL1 = 0x0700,
              kHandleL2 = 0x0701, kHandleL3 = 0x0702, kHandleArray = 0x1000, kHandleDevice = 0x1100,
              kHandleMapped = 0x1300, kHandleBoot = 0x2000, kHandleEnd = 0x7F00;

/// One SMBIOS structure: the formatted area, then its string set (DSP0134
/// 6.1.3: 1-based string numbers, 0 = no string, each string NUL-terminated
/// and the set ended by one more NUL; no strings at all is a double NUL).
class Structure {
public:
    Structure(u8 type, u8 length, u16 handle) : m_bytes(length, 0) {
        m_bytes[0] = type;
        m_bytes[1] = length;
        u16_at(2, handle);
    }
    void u8_at(usize at, u8 value) { m_bytes[at] = value; }
    void u16_at(usize at, u16 value) { std::memcpy(m_bytes.data() + at, &value, 2); }
    void u32_at(usize at, u32 value) { std::memcpy(m_bytes.data() + at, &value, 4); }
    void u64_at(usize at, u64 value) { std::memcpy(m_bytes.data() + at, &value, 8); }
    void bytes_at(usize at, std::span<const u8> value) { std::memcpy(m_bytes.data() + at, value.data(), value.size()); }
    /// A string field: its number, or 0 when the value is unknown.
    void string_at(usize at, std::string_view value) {
        if (value.empty()) return;
        m_strings.emplace_back(value);
        m_bytes[at] = static_cast<u8>(m_strings.size());
    }
    void append_to(std::vector<u8>& table) const {
        table.insert(table.end(), m_bytes.begin(), m_bytes.end());
        for (const std::string& value : m_strings) {
            table.insert(table.end(), value.begin(), value.end());
            table.push_back(0);
        }
        if (m_strings.empty()) table.push_back(0);
        table.push_back(0);
    }
    [[nodiscard]] usize size() const {
        usize strings = 0;
        for (const std::string& value : m_strings) strings += value.size() + 1;
        return m_bytes.size() + (m_strings.empty() ? 2 : strings + 1);
    }

private:
    std::vector<u8> m_bytes;
    std::vector<std::string> m_strings;
};

[[nodiscard]] u8 enclosure_type(platform::HostChassis chassis) {
    // DSP0134 7.4.1 System Enclosure or Chassis Types.
    switch (chassis) {
        case platform::HostChassis::Notebook: return 0x0A;
        case platform::HostChassis::AllInOne: return 0x0D;
        case platform::HostChassis::MiniPc: return 0x23;
        case platform::HostChassis::Desktop: return 0x03;
        case platform::HostChassis::Tower: return 0x07;
        case platform::HostChassis::Unknown: break;
    }
    return 0x02;
}

/// The Type 1 UUID in SMBIOS wire format (DSP0134 7.2.1): time_low,
/// time_mid and time_hi_and_version little-endian, the rest in order. All
/// zero ("not present") when the host has none.
[[nodiscard]] std::array<u8, 16> smbios_uuid(std::string_view text) {
    std::array<u8, 16> uuid{};
    std::array<u8, 16> big{};
    usize nibble = 0;
    for (const char c : text) {
        if (c == '-') continue;
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        if (v < 0 || nibble >= 32) return {};
        big[nibble / 2] = static_cast<u8>((big[nibble / 2] << 4) | v);
        ++nibble;
    }
    if (nibble != 32) return {};
    const usize order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (usize i = 0; i < 16; ++i) uuid[i] = big[order[i]];
    return uuid;
}

/// SMBIOS Type 17 Memory Type (DSP0134 7.18.2) of the host's DRAM type.
[[nodiscard]] u8 memory_type(std::string_view type) {
    if (type == "LPDDR5") return 0x23;
    if (type == "DDR5") return 0x22;
    if (type == "LPDDR4" || type == "LPDDR4X") return 0x1E;
    if (type == "DDR4") return 0x1A;
    return 0x02;  // Unknown
}

/// Type 7 Associativity (DSP0134 7.8.5) of a way count.
[[nodiscard]] u8 associativity_code(u32 ways) {
    switch (ways) {
        case 8: return 0x07;
        case 12: return 0x09;
        case 16: return 0x08;
        default: return 0x02;  // Unknown
    }
}

void append_cache(std::vector<u8>& table, u16& largest, u16 handle, u8 level, u64 bytes, u32 ways) {
    // DSP0134 7.8 Cache Information, 3.1 length. Sizes are per processor
    // package: the sum over every core / cluster of the host. 1K granularity
    // up to 32767 KB, 64K beyond (bit 15); the DWORD "2" fields (bit 31) for
    // the same value.
    Structure cache(7, 0x1B, handle);
    cache.string_at(0x04, level == 1 ? "L1 Cache" : level == 2 ? "L2 Cache" : "L3 Cache");
    // Level, not socketed, internal, enabled; operational mode unknown (11b).
    cache.u16_at(0x05, static_cast<u16>((level - 1) | 0x80 | (0x3 << 8)));
    const u64 kb = bytes / 1024;
    const u16 size16 = kb <= 0x7FFF ? static_cast<u16>(kb) : static_cast<u16>(0x8000 | std::min<u64>(kb / 64, 0x7FFF));
    cache.u16_at(0x07, size16);
    cache.u16_at(0x09, size16);
    cache.u16_at(0x0B, 0x0002);  // SRAM type: Unknown
    cache.u16_at(0x0D, 0x0002);
    cache.u8_at(0x0F, 0);        // speed unknown
    cache.u8_at(0x10, 0x02);     // error correction: Unknown
    cache.u8_at(0x11, 0x05);     // Unified: instruction and data together at L1, unified beyond
    cache.u8_at(0x12, associativity_code(ways));
    const u32 size32 = kb <= 0x7FFFFFFF ? static_cast<u32>(kb) : static_cast<u32>(0x80000000u | (kb / 64));
    cache.u32_at(0x13, size32);
    cache.u32_at(0x17, size32);
    cache.append_to(table);
    largest = std::max(largest, static_cast<u16>(cache.size()));
}

struct Built {
    std::vector<u8> table;
    std::vector<u8> bios;
    u16 largest = 0;
    u16 count = 0;
};

Built build(const platform::HostHardwareProfile& host) {
    Built out;
    std::vector<u8>& table = out.table;
    auto add = [&](const Structure& s) {
        s.append_to(table);
        out.largest = std::max(out.largest, static_cast<u16>(s.size()));
        ++out.count;
    };
    const Identity id = identity();
    const auto& topology = processor_topology::topology();

    // Type 0 BIOS Information (3.1+ length 1Ah).
    {
        Structure bios(0, 0x1A, kHandleBios);
        bios.string_at(0x04, id.bios_vendor);
        bios.string_at(0x05, id.bios_version);
        bios.u16_at(0x06, 0x0000);   // no legacy BIOS segment (not a PC BIOS)
        bios.string_at(0x08, id.bios_release_date);
        bios.u8_at(0x09, 0x00);
        bios.u64_at(0x0A, 0x08);     // bit 3: BIOS Characteristics are not supported
        bios.u8_at(0x14, id.bios_major);
        bios.u8_at(0x15, id.bios_minor);
        bios.u8_at(0x16, id.ec_major);
        bios.u8_at(0x17, id.ec_minor);
        bios.u16_at(0x18, 0x0000);
        add(bios);
    }
    // Type 1 System Information (2.4+ length 1Bh).
    {
        Structure system(1, 0x1B, kHandleSystem);
        system.string_at(0x04, id.system_manufacturer);
        system.string_at(0x05, id.system_product);
        system.string_at(0x06, id.system_version);
        system.string_at(0x07, id.system_serial);
        system.bytes_at(0x08, smbios_uuid(host.machine.hardware_uuid));
        system.u8_at(0x18, 0x06);    // wake-up: Power Switch
        system.string_at(0x19, id.system_sku);
        system.string_at(0x1A, id.system_family);
        add(system);
    }
    // Type 2 Baseboard (no contained objects: length 0Fh).
    {
        Structure board(2, 0x0F, kHandleBoard);
        board.string_at(0x04, id.board_manufacturer);
        board.string_at(0x05, id.board_product);
        board.string_at(0x06, id.board_version);
        board.u8_at(0x09, 0x09);     // hosting board, replaceable
        board.u16_at(0x0B, kHandleChassis);
        board.u8_at(0x0D, 0x0A);     // Motherboard
        board.u8_at(0x0E, 0);
        add(board);
    }
    // Type 3 System Enclosure (2.7+, no contained elements: length 16h).
    {
        Structure chassis(3, 0x16, kHandleChassis);
        chassis.string_at(0x04, id.system_manufacturer);
        chassis.u8_at(0x05, id.enclosure_type);
        chassis.string_at(0x07, id.system_serial);
        chassis.u8_at(0x09, 0x03);   // boot-up state: Safe
        chassis.u8_at(0x0A, 0x03);   // power supply state: Safe
        chassis.u8_at(0x0B, 0x03);   // thermal state: Safe
        chassis.u8_at(0x0C, 0x02);   // security status: Unknown
        add(chassis);
    }
    // Type 7 caches (per package totals), referenced by the processor.
    u64 level_bytes[4]{};
    u32 level_ways[4]{};
    for (const auto& cache : topology.caches) {
        if (cache.level >= 1 && cache.level <= 3) {
            level_bytes[cache.level] += cache.size;
            level_ways[cache.level] = std::max<u32>(level_ways[cache.level], cache.associativity);
        }
    }
    // Type 4 Processor Information (3.6 length 32h).
    {
        u32 r[4]{};
        cpu::ops::synth_cpuid_on(0, 1, 0, true, r[0], r[1], r[2], r[3]);
        const u32 signature = r[0];
        const u32 features = r[3];
        u32 max_mhz = 0;
        for (const auto& core_class : host.cpu.core_classes) max_mhz = std::max(max_mhz, core_class.max_mhz);
        const u32 tsc_mhz = static_cast<u32>(guest_tsc_hz() / 1'000'000);
        const u16 count = static_cast<u16>(topology.count);
        Structure cpu(4, 0x32, kHandleProcessor);
        cpu.string_at(0x04, "SoC");
        cpu.u8_at(0x05, 0x03);                         // Central Processor
        cpu.u8_at(0x06, 0x01);                         // family: Other (an Apple core, not an x86 family)
        cpu.string_at(0x07, host.machine.manufacturer);
        cpu.u32_at(0x08, signature);                   // x86 Processor ID: CPUID 1 EAX, EDX
        cpu.u32_at(0x0C, features);
        cpu.string_at(0x10, host.cpu.brand);
        cpu.u8_at(0x11, 0x00);                         // no legacy voltage; the actual one is not exposed
        cpu.u16_at(0x12, static_cast<u16>(host.cpu.timebase_hz / 1'000'000));  // reference clock
        cpu.u16_at(0x14, static_cast<u16>(max_mhz != 0 ? max_mhz : tsc_mhz));
        cpu.u16_at(0x16, static_cast<u16>(tsc_mhz));
        cpu.u8_at(0x18, 0x41);                         // socket populated, CPU enabled
        cpu.u8_at(0x19, 0x06);                         // upgrade: None (soldered SoC)
        cpu.u16_at(0x1A, level_bytes[1] != 0 ? kHandleL1 : 0xFFFF);
        cpu.u16_at(0x1C, level_bytes[2] != 0 ? kHandleL2 : 0xFFFF);
        cpu.u16_at(0x1E, level_bytes[3] != 0 ? kHandleL3 : 0xFFFF);
        cpu.u8_at(0x23, static_cast<u8>(std::min<u16>(count, 0xFF)));
        cpu.u8_at(0x24, static_cast<u8>(std::min<u16>(count, 0xFF)));
        cpu.u8_at(0x25, static_cast<u8>(std::min<u16>(count, 0xFF)));
        // 64-bit capable, multi-core, power/performance control.
        cpu.u16_at(0x26, static_cast<u16>((1u << 2) | (count > 1 ? 1u << 3 : 0u) | (1u << 7)));
        cpu.u16_at(0x28, 0x0001);                      // Processor Family 2 = Family
        cpu.u16_at(0x2A, count);
        cpu.u16_at(0x2C, count);
        cpu.u16_at(0x2E, count);
        cpu.u16_at(0x30, count);                       // Thread Enabled
        add(cpu);
    }
    for (u8 level = 1; level <= 3; ++level) {
        if (level_bytes[level] == 0) continue;
        const u16 handle = level == 1 ? kHandleL1 : level == 2 ? kHandleL2 : kHandleL3;
        append_cache(table, out.largest, handle, level, level_bytes[level], level_ways[level]);
        ++out.count;
    }
    // Type 16 Physical Memory Array (2.7+ length 17h).
    const u64 memory = host.memory.total_bytes;
    const u64 memory_kb = memory / 1024;
    {
        Structure array(16, 0x17, kHandleArray);
        array.u8_at(0x04, 0x03);   // system board
        array.u8_at(0x05, 0x03);   // system memory
        // The host says whether its DRAM has ECC, not which kind: Other.
        array.u8_at(0x06, host.memory.ecc ? 0x01 : 0x03);
        if (memory_kb >= 0x80000000ull) {
            array.u32_at(0x07, 0x80000000u);
            array.u64_at(0x0F, memory);
        } else {
            array.u32_at(0x07, static_cast<u32>(memory_kb));
        }
        array.u16_at(0x0B, 0xFFFE);  // no error information
        array.u16_at(0x0D, 1);
        add(array);
    }
    // Type 17 Memory Device (3.3+ length 5Ch): the unified memory, one device.
    {
        Structure device(17, 0x5C, kHandleDevice);
        device.u16_at(0x04, kHandleArray);
        device.u16_at(0x06, 0xFFFE);
        device.u16_at(0x08, 0xFFFF);   // total width unknown
        device.u16_at(0x0A, 0xFFFF);   // data width unknown
        const u64 mb = memory / (1024 * 1024);
        if (mb >= 0x7FFF) {            // 32 GB - 1 MB and above: Extended Size
            device.u16_at(0x0C, 0x7FFF);
            device.u32_at(0x1C, static_cast<u32>(std::min<u64>(mb, 0x7FFFFFFF)));
        } else {
            device.u16_at(0x0C, static_cast<u16>(mb));
        }
        device.u8_at(0x0E, 0x0B);      // Row of chips (soldered)
        device.string_at(0x10, "Unified Memory");
        device.u8_at(0x12, memory_type(host.memory.type));
        device.u16_at(0x13, 0x4080);   // synchronous, unbuffered (DDR SDRAM, soldered)
        device.u16_at(0x15, 0);        // speed: not exposed by macOS
        device.string_at(0x17, host.memory.manufacturer);
        device.u8_at(0x28, 0x03);      // DRAM
        device.u16_at(0x29, 0x0008);   // volatile memory
        device.u64_at(0x3C, memory);   // volatile size
        device.u64_at(0x4C, ~0ull);    // logical size unknown
        add(device);
    }
    // Type 19 Memory Array Mapped Address (2.7+ length 1Fh).
    {
        Structure mapped(19, 0x1F, kHandleMapped);
        if (memory_kb != 0 && memory_kb - 1 < 0xFFFFFFFFull) {
            mapped.u32_at(0x04, 0);
            mapped.u32_at(0x08, static_cast<u32>(memory_kb - 1));
        } else {
            mapped.u32_at(0x04, 0xFFFFFFFFu);
            mapped.u32_at(0x08, 0xFFFFFFFFu);
            mapped.u64_at(0x0F, 0);
            mapped.u64_at(0x17, memory == 0 ? 0 : memory - 1);
        }
        mapped.u16_at(0x0C, kHandleArray);
        mapped.u8_at(0x0E, 1);
        add(mapped);
    }
    // Type 32 System Boot Information: no errors.
    add(Structure(32, 0x0B, kHandleBoot));
    add(Structure(127, 0x04, kHandleEnd));

    // The legacy window: _SM3_ (DSP0134 5.2.2) at 0 and the 2.1-style _SM_
    // (5.2.1) at 20h, both pointing at the table at 100h.
    out.bios.assign(kBiosSize, 0);
    std::copy(table.begin(), table.end(), out.bios.begin() + kSmbiosTableOffset);
    const u32 table_address = static_cast<u32>(kBiosBase + kSmbiosTableOffset);
    const auto checksum = [](const u8* bytes, usize size) {
        u8 sum = 0;
        for (usize i = 0; i < size; ++i) sum = static_cast<u8>(sum + bytes[i]);
        return static_cast<u8>(0u - sum);
    };
    {
        std::array<u8, 0x18> entry{};
        std::copy_n("_SM3_", 5, entry.begin());
        entry[6] = 0x18;
        entry[7] = kSmbiosMajor;
        entry[8] = kSmbiosMinor;
        entry[9] = kSmbiosDocrev;
        entry[10] = 0x01;  // entry point revision: SMBIOS 3.0 format
        const u32 maximum = static_cast<u32>(table.size());
        std::memcpy(entry.data() + 0x0C, &maximum, 4);
        const u64 address = table_address;
        std::memcpy(entry.data() + 0x10, &address, 8);
        entry[5] = checksum(entry.data(), entry.size());
        std::copy(entry.begin(), entry.end(), out.bios.begin());
    }
    {
        std::array<u8, 0x1F> entry{};
        std::copy_n("_SM_", 4, entry.begin());
        entry[5] = 0x1F;
        entry[6] = kSmbiosMajor;
        entry[7] = kSmbiosMinor;
        std::memcpy(entry.data() + 8, &out.largest, 2);
        std::copy_n("_DMI_", 5, entry.begin() + 0x10);
        const u16 length = static_cast<u16>(table.size());
        std::memcpy(entry.data() + 0x16, &length, 2);
        std::memcpy(entry.data() + 0x18, &table_address, 4);
        std::memcpy(entry.data() + 0x1C, &out.count, 2);
        entry[0x1E] = static_cast<u8>((kSmbiosMajor << 4) | kSmbiosMinor);  // BCD revision
        entry[0x15] = checksum(entry.data() + 0x10, entry.size() - 0x10);
        entry[4] = checksum(entry.data(), entry.size());
        std::copy(entry.begin(), entry.end(), out.bios.begin() + kLegacyEntryOffset);
    }
    return out;
}

const Built& built() {
    static std::mutex lock;
    static const platform::HostHardwareProfile* source = nullptr;
    static Built cached;
    std::lock_guard guard(lock);
    const platform::HostHardwareProfile& host = platform::host_hardware();
    if (source != &host) {
        cached = build(host);
        source = &host;
    }
    return cached;
}

} // namespace

Identity identity() {
    const platform::HostHardwareProfile& host = platform::host_hardware();
    const platform::HostMachine& machine = host.machine;
    Identity id;
    id.bios_vendor = machine.firmware_vendor;
    id.bios_version = machine.firmware_version;
    id.bios_release_date = machine.firmware_date;
    // Apple's firmware version (e.g. 13822.81.10) does not fit the BYTE
    // System BIOS Major/Minor Release fields, so both are 0FFh, the value
    // DSP0134 7.1 gives when the system does not use them.
    id.system_manufacturer = machine.manufacturer;
    id.system_product = machine.model_identifier;
    id.system_serial = machine.serial_number;
    id.system_sku = machine.part_number;
    id.system_family = machine.product_family;
    id.board_manufacturer = machine.manufacturer;
    id.board_product = machine.board_id;
    id.enclosure_type = enclosure_type(machine.chassis);
    return id;
}

std::span<const u8> smbios_table() {
    return built().table;
}

std::span<const u8> bios_window() {
    return built().bios;
}

} // namespace vitreo::winapi::virtual_firmware
