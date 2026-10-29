// SPDX-License-Identifier: LGPL-2.1-only
#include "platform/host_hardware.hpp"

#include <vitreo/virtual_tsc.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <pthread.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <optional>

namespace vitreo::platform {

namespace host_hardware_detail {

std::vector<u32> decode_dvfs_mhz(std::span<const u8> table) {
    std::vector<u32> raw;
    for (usize at = 0; at + 8 <= table.size(); at += 8) {
        u32 value = 0;
        std::memcpy(&value, table.data() + at, 4);
        if (value != 0) raw.push_back(value);
    }
    if (raw.empty()) return {};
    const u32 largest = *std::max_element(raw.begin(), raw.end());
    const u32 per_mhz = largest < 10'000'000u ? 1'000u : 1'000'000u;
    std::vector<u32> mhz;
    mhz.reserve(raw.size());
    for (const u32 value : raw) mhz.push_back((value + per_mhz / 2) / per_mhz);
    return mhz;
}

std::string product_family(std::string_view product_name) {
    std::string_view family = product_name.substr(0, product_name.find(" ("));
    while (!family.empty() && family.back() == ' ') family.remove_suffix(1);
    return std::string(family);
}

HostChassis chassis_of(std::string_view family) {
    if (family.starts_with("MacBook")) return HostChassis::Notebook;
    if (family.starts_with("iMac")) return HostChassis::AllInOne;
    if (family == "Mac mini") return HostChassis::MiniPc;
    if (family == "Mac Studio") return HostChassis::Desktop;
    if (family == "Mac Pro") return HostChassis::Tower;
    return HostChassis::Unknown;
}

std::string device_tree_string(std::span<const u8> bytes) {
    const auto end = std::find(bytes.begin(), bytes.end(), u8{0});
    return std::string(bytes.begin(), end);
}

std::string firmware_date(std::string_view stamp) {
    // weekday month day time zone year, separated by spaces.
    static constexpr std::string_view kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::vector<std::string_view> fields;
    while (!stamp.empty()) {
        const usize space = stamp.find(' ');
        if (space != 0) fields.push_back(stamp.substr(0, space));
        if (space == std::string_view::npos) break;
        stamp.remove_prefix(space + 1);
    }
    if (fields.size() != 6) return {};
    const auto month = std::find(std::begin(kMonths), std::end(kMonths), fields[1]);
    if (month == std::end(kMonths)) return {};
    const auto digits = [](std::string_view v) {
        return !v.empty() && std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    if (!digits(fields[2]) || fields[2].size() > 2 || !digits(fields[5]) || fields[5].size() != 4) return {};
    char text[16];
    std::snprintf(text, sizeof(text), "%02d/%02d/%.4s", static_cast<int>(month - std::begin(kMonths)) + 1,
                  std::atoi(std::string(fields[2]).c_str()), fields[5].data());
    return text;
}

} // namespace host_hardware_detail

namespace {

using namespace host_hardware_detail;

std::optional<u64> sysctl_u64(const char* name) {
    u64 value = 0;
    usize size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) return std::nullopt;
    if (size == sizeof(u32)) {
        u32 narrow = 0;
        std::memcpy(&narrow, &value, sizeof(narrow));
        return narrow;
    }
    return size == sizeof(u64) ? std::optional<u64>(value) : std::nullopt;
}

u32 sysctl_u32(const char* name) {
    return static_cast<u32>(sysctl_u64(name).value_or(0));
}

std::string sysctl_string(const char* name) {
    usize size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
    std::string text(size, '\0');
    if (sysctlbyname(name, text.data(), &size, nullptr, 0) != 0) return {};
    text.resize(std::strlen(text.c_str()));
    return text;
}

/// Owns one IOKit object reference.
class IoObject {
public:
    explicit IoObject(io_object_t object = IO_OBJECT_NULL) : m_object(object) {}
    ~IoObject() { if (m_object != IO_OBJECT_NULL) IOObjectRelease(m_object); }
    IoObject(const IoObject&) = delete;
    IoObject& operator=(const IoObject&) = delete;
    [[nodiscard]] io_object_t get() const { return m_object; }
    [[nodiscard]] explicit operator bool() const { return m_object != IO_OBJECT_NULL; }

private:
    io_object_t m_object;
};

/// Owns one Core Foundation reference.
class CfObject {
public:
    explicit CfObject(CFTypeRef object) : m_object(object) {}
    ~CfObject() { if (m_object != nullptr) CFRelease(m_object); }
    CfObject(const CfObject&) = delete;
    CfObject& operator=(const CfObject&) = delete;
    [[nodiscard]] CFTypeRef get() const { return m_object; }

private:
    CFTypeRef m_object;
};

IoObject registry_path(const char* path) {
    return IoObject(IORegistryEntryFromPath(kIOMainPortDefault, path));
}

IoObject matching_service(const char* io_class) {
    return IoObject(IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(io_class)));
}

CfObject property(io_object_t entry, const char* key) {
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, name, kCFAllocatorDefault, 0);
    CFRelease(name);
    return CfObject(value);
}

std::vector<u8> data_property(io_object_t entry, const char* key) {
    const CfObject value = property(entry, key);
    if (value.get() == nullptr || CFGetTypeID(value.get()) != CFDataGetTypeID()) return {};
    const auto data = static_cast<CFDataRef>(value.get());
    const u8* bytes = CFDataGetBytePtr(data);
    return std::vector<u8>(bytes, bytes + CFDataGetLength(data));
}

std::string string_property(io_object_t entry, const char* key) {
    const CfObject value = property(entry, key);
    if (value.get() == nullptr) return {};
    if (CFGetTypeID(value.get()) == CFDataGetTypeID()) {
        const auto data = static_cast<CFDataRef>(value.get());
        return device_tree_string({CFDataGetBytePtr(data), static_cast<usize>(CFDataGetLength(data))});
    }
    if (CFGetTypeID(value.get()) != CFStringGetTypeID()) return {};
    const auto text = static_cast<CFStringRef>(value.get());
    const CFIndex capacity =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<usize>(capacity), '\0');
    if (!CFStringGetCString(text, out.data(), capacity, kCFStringEncodingUTF8)) return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

/// A u32 property stored either as a CFNumber or as little-endian data.
std::optional<u32> u32_property(io_object_t entry, const char* key) {
    const CfObject value = property(entry, key);
    if (value.get() == nullptr) return std::nullopt;
    if (CFGetTypeID(value.get()) == CFNumberGetTypeID()) {
        i64 number = 0;
        if (!CFNumberGetValue(static_cast<CFNumberRef>(value.get()), kCFNumberSInt64Type, &number)) {
            return std::nullopt;
        }
        return static_cast<u32>(number);
    }
    if (CFGetTypeID(value.get()) == CFDataGetTypeID() &&
        CFDataGetLength(static_cast<CFDataRef>(value.get())) >= 4) {
        u32 number = 0;
        std::memcpy(&number, CFDataGetBytePtr(static_cast<CFDataRef>(value.get())), 4);
        return number;
    }
    return std::nullopt;
}

/// Apple silicon: one class per performance level, each logical CPU placed
/// by its device-tree node.
bool read_apple_silicon_cpu(HostCpu& cpu) {
    const u32 levels = sysctl_u32("hw.nperflevels");
    if (levels == 0) return false;
    for (u32 level = 0; level < levels; ++level) {
        const std::string prefix = "hw.perflevel" + std::to_string(level) + ".";
        auto field = [&](const char* name) { return sysctl_u64((prefix + name).c_str()).value_or(0); };
        HostCoreClass core_class;
        core_class.name = sysctl_string((prefix + "name").c_str());
        core_class.efficiency = levels > 1 && core_class.name == "Efficiency";
        core_class.physical_cores = static_cast<u32>(field("physicalcpu"));
        core_class.logical_processors = static_cast<u32>(field("logicalcpu"));
        core_class.l1i_bytes = field("l1icachesize");
        core_class.l1d_bytes = field("l1dcachesize");
        core_class.l2_bytes = field("l2cachesize");
        core_class.cores_per_l2 = static_cast<u32>(field("cpusperl2"));
        cpu.core_classes.push_back(std::move(core_class));
    }

    // cluster-type names the class ("P"/"E"); cluster-id groups an L2.
    const IoObject cpus = registry_path("IODeviceTree:/cpus");
    io_iterator_t children = IO_OBJECT_NULL;
    if (!cpus || IORegistryEntryGetChildIterator(cpus.get(), kIODeviceTreePlane, &children) != KERN_SUCCESS) {
        return false;
    }
    const IoObject iterator(children);
    std::vector<std::optional<HostLogicalProcessor>> placed;
    while (true) {
        const IoObject node(IOIteratorNext(iterator.get()));
        if (!node) break;
        const auto id = u32_property(node.get(), "logical-cpu-id");
        const std::string type = string_property(node.get(), "cluster-type");
        if (!id || type.empty() || *id >= 1024) continue;
        const bool efficiency = type == "E";
        const auto match = std::find_if(cpu.core_classes.begin(), cpu.core_classes.end(),
            [&](const HostCoreClass& c) { return c.efficiency == efficiency; });
        if (match == cpu.core_classes.end()) continue;
        if (placed.size() <= *id) placed.resize(*id + 1);
        placed[*id] = HostLogicalProcessor{
            static_cast<u32>(match - cpu.core_classes.begin()),
            u32_property(node.get(), "cluster-id").value_or(0)};
    }
    for (const auto& processor : placed) {
        if (!processor) return false;  // a gap: the numbering is not what macOS schedules on
        cpu.processors.push_back(*processor);
    }
    return !cpu.processors.empty();
}

/// The DVFS tables pmgr publishes. By the layout every Apple silicon
/// generation so far uses, voltage-states1 is the Efficiency cluster,
/// voltage-states5 the Performance cluster and voltage-states9 the GPU. That
/// layout is only accepted when it agrees with the hardware: the Efficiency
/// maximum below the Performance maximum, and the GPU table distinct from both.
void read_dvfs(HostCpu& cpu, HostGpu& gpu) {
    const IoObject pmgr = registry_path("IODeviceTree:/arm-io/pmgr");
    if (!pmgr) return;
    auto table = [&](const char* name) {
        std::vector<u8> bytes = data_property(pmgr.get(), (std::string(name) + "-sram").c_str());
        if (bytes.empty()) bytes = data_property(pmgr.get(), name);
        return decode_dvfs_mhz(bytes);
    };
    const auto efficiency = std::find_if(cpu.core_classes.begin(), cpu.core_classes.end(),
        [](const HostCoreClass& c) { return c.efficiency; });
    const auto performance = std::find_if(cpu.core_classes.begin(), cpu.core_classes.end(),
        [](const HostCoreClass& c) { return !c.efficiency; });
    if (efficiency == cpu.core_classes.end() || performance == cpu.core_classes.end()) return;
    const std::vector<u32> e_states = table("voltage-states1");
    const std::vector<u32> p_states = table("voltage-states5");
    const std::vector<u32> gpu_states = table("voltage-states9");
    if (e_states.empty() || p_states.empty()) return;
    const auto [e_min, e_max] = std::minmax_element(e_states.begin(), e_states.end());
    const auto [p_min, p_max] = std::minmax_element(p_states.begin(), p_states.end());
    if (*e_max >= *p_max) return;
    efficiency->min_mhz = *e_min;
    efficiency->max_mhz = *e_max;
    performance->min_mhz = *p_min;
    performance->max_mhz = *p_max;
    if (!gpu_states.empty() && gpu_states != e_states && gpu_states != p_states) {
        const auto [g_min, g_max] = std::minmax_element(gpu_states.begin(), gpu_states.end());
        gpu.min_mhz = *g_min;
        gpu.max_mhz = *g_max;
    }
}

/// Intel Macs: one class; hw.cacheconfig gives how many logical CPUs share
/// each cache level (index 1 = L1, 2 = L2).
void read_intel_cpu(HostCpu& cpu) {
    HostCoreClass core_class;
    core_class.name = "Performance";
    core_class.physical_cores = sysctl_u32("hw.physicalcpu");
    core_class.logical_processors = sysctl_u32("hw.logicalcpu");
    core_class.l1i_bytes = sysctl_u64("hw.l1icachesize").value_or(0);
    core_class.l1d_bytes = sysctl_u64("hw.l1dcachesize").value_or(0);
    core_class.l2_bytes = sysctl_u64("hw.l2cachesize").value_or(0);
    core_class.min_mhz = static_cast<u32>(sysctl_u64("hw.cpufrequency_min").value_or(0) / 1'000'000);
    core_class.max_mhz = static_cast<u32>(sysctl_u64("hw.cpufrequency_max").value_or(0) / 1'000'000);
    u64 sharing[10]{};
    usize size = sizeof(sharing);
    const u32 threads_per_core = core_class.physical_cores == 0
        ? 1 : std::max(1u, core_class.logical_processors / core_class.physical_cores);
    u32 l2_sharing = 0;
    if (sysctlbyname("hw.cacheconfig", sharing, &size, nullptr, 0) == 0 && size >= 3 * sizeof(u64)) {
        l2_sharing = static_cast<u32>(sharing[2]);
        core_class.cores_per_l2 = std::max(1u, l2_sharing / threads_per_core);
    }
    cpu.l3_bytes = sysctl_u64("hw.l3cachesize").value_or(0);
    for (u32 i = 0; i < core_class.logical_processors; ++i) {
        cpu.processors.push_back({0, l2_sharing == 0 ? 0 : i / l2_sharing});
    }
    cpu.core_classes.push_back(std::move(core_class));
}

HostHardwareProfile read_host() {
    HostHardwareProfile profile;
    HostCpu& cpu = profile.cpu;
    cpu.brand = sysctl_string("machdep.cpu.brand_string");
    cpu.cache_line_bytes = sysctl_u32("hw.cachelinesize");
    cpu.timebase_hz = sysctl_u64("hw.tbfrequency").value_or(0);
    if (read_apple_silicon_cpu(cpu)) {
        read_dvfs(cpu, profile.gpu);
    } else {
        cpu.core_classes.clear();
        cpu.processors.clear();
        read_intel_cpu(cpu);
    }

    HostMemory& memory = profile.memory;
    memory.total_bytes = sysctl_u64("hw.memsize").value_or(0);
    if (const IoObject chosen = registry_path("IODeviceTree:/chosen")) {
        memory.type = string_property(chosen.get(), "dram-type");
        memory.manufacturer = string_property(chosen.get(), "dram-vendor");
        memory.ecc = u32_property(chosen.get(), "dram-ecc").value_or(0) != 0;
        std::string firmware = string_property(chosen.get(), "system-firmware-version");
        if (firmware.starts_with("iBoot-")) firmware.erase(0, 6);
        profile.machine.firmware_version = std::move(firmware);
    }

    HostMachine& machine = profile.machine;
    machine.model_identifier = sysctl_string("hw.model");
    if (const IoObject root = registry_path("IODeviceTree:/")) {
        machine.manufacturer = string_property(root.get(), "manufacturer");
        machine.board_id = string_property(root.get(), "target-type");
        machine.firmware_date = firmware_date(string_property(root.get(), "time-stamp"));
        const std::string model_number = string_property(root.get(), "model-number");
        if (!model_number.empty()) {
            machine.part_number = model_number + string_property(root.get(), "region-info");
        }
    }
    if (const IoObject product = registry_path("IODeviceTree:/product")) {
        machine.product_name = string_property(product.get(), "product-name");
    }
    if (const IoObject platform = matching_service("IOPlatformExpertDevice")) {
        if (machine.manufacturer.empty()) machine.manufacturer = string_property(platform.get(), "manufacturer");
        machine.serial_number = string_property(platform.get(), "IOPlatformSerialNumber");
        machine.hardware_uuid = string_property(platform.get(), "IOPlatformUUID");
    }
    machine.product_family = product_family(machine.product_name);
    machine.chassis = chassis_of(machine.product_family);
    if (!machine.firmware_version.empty()) machine.firmware_vendor = machine.manufacturer;

    if (const IoObject accelerator = matching_service("AGXAccelerator")) {
        profile.gpu.name = string_property(accelerator.get(), "model");
        profile.gpu.core_count = u32_property(accelerator.get(), "gpu-core-count").value_or(0);
    }
    return profile;
}

std::atomic<const HostHardwareProfile*> g_override{nullptr};

const HostHardwareProfile& real_host() {
    static const HostHardwareProfile host = read_host();
    return host;
}

} // namespace

const HostHardwareProfile& host_hardware() {
    if (const HostHardwareProfile* profile = g_override.load(std::memory_order_acquire)) return *profile;
    return real_host();
}

u32 reported_cache_ways(u32 level, u64 size_bytes, u32 line_bytes) noexcept {
    if (size_bytes == 0 || line_bytes == 0 || size_bytes % line_bytes != 0) return 0;
    const u64 lines = size_bytes / line_bytes;
    for (u64 ways = level <= 1 ? 8 : 16; ways <= lines; ++ways) {
        if (lines % ways == 0) return static_cast<u32>(ways);
    }
    return static_cast<u32>(lines);  // smaller than one set of the usual shape
}

u64 host_max_core_hz() noexcept {
    u64 mhz = 0;
    for (const HostCoreClass& core_class : real_host().cpu.core_classes) {
        mhz = std::max<u64>(mhz, core_class.max_mhz);
    }
    return mhz * 1'000'000ULL;
}

} // namespace vitreo::platform

namespace vitreo {

u64 host_max_core_hz_for_tsc() noexcept {
    return platform::host_max_core_hz();
}

} // namespace vitreo

namespace vitreo::platform {

u32 current_host_processor() noexcept {
    size_t number = 0;
    if (pthread_cpu_number_np(&number) != 0) return 0;
    return static_cast<u32>(number);
}

namespace {
thread_local std::shared_ptr<const ProcessorAffinity> t_affinity;
} // namespace

void bind_current_thread_affinity(std::shared_ptr<const ProcessorAffinity> affinity) noexcept {
    t_affinity = std::move(affinity);
}

u32 current_processor() noexcept {
    const u32 count = static_cast<u32>(host_hardware().cpu.processors.size());
    u32 processor = current_host_processor();
    if (count != 0 && processor >= count) processor = 0;
    const u64 allowed = t_affinity != nullptr ? t_affinity->load(std::memory_order_relaxed) : 0;
    if (allowed == 0 || (processor < 64 && ((allowed >> processor) & 1u) != 0)) return processor;
    return static_cast<u32>(__builtin_ctzll(allowed));
}

void set_host_hardware_for_testing(const HostHardwareProfile* profile) {
    g_override.store(profile, std::memory_order_release);
}

} // namespace vitreo::platform
