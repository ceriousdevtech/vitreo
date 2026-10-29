// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/kernel32/system/processor_topology.hpp"

#include "platform/host_hardware.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace vitreo::winapi::processor_topology {

u32 processor_count() noexcept {
    const usize host = platform::host_hardware().cpu.processors.size();
    return static_cast<u32>(std::clamp<usize>(host, 1, 64));
}

u64 processor_mask() noexcept {
    const u32 count = processor_count();
    return count >= 64 ? ~0ull : ((1ull << count) - 1);
}

u64 guest_affinity(const Marshaler& m, u64 mask) noexcept {
    return m.cpu_is_64bit() ? mask : (mask & 0xFFFFFFFFull);
}

u32 group_affinity_size(const Marshaler& m) noexcept {
    return m.cpu_is_64bit() ? 16u : 12u;
}

bool write_group_affinity(Marshaler& m, GuestAddr at, u64 mask, u16 group) {
    const bool wide = m.cpu_is_64bit();
    const u32 group_at = wide ? 8u : 4u;
    bool ok = wide ? m.write_u64(at, mask).is_ok() : m.write_u32(at, static_cast<u32>(mask)).is_ok();
    ok = ok && m.write_u16(at + group_at, group).is_ok();
    for (u32 i = 0; ok && i < 3; ++i) ok = m.write_u16(at + group_at + 2 + 2 * i, 0).is_ok();
    return ok;
}

bool read_group_affinity(Marshaler& m, GuestAddr at, GroupAffinity& out) {
    const bool wide = m.cpu_is_64bit();
    const u32 group_at = wide ? 8u : 4u;
    if (wide) {
        const auto mask = m.read_u64(at);
        if (mask.is_err()) return false;
        out.mask = mask.value();
    } else {
        const auto mask = m.read_u32(at);
        if (mask.is_err()) return false;
        out.mask = mask.value();
    }
    const auto group = m.read_u16(at + group_at);
    if (group.is_err()) return false;
    out.group = group.value();
    out.reserved_zero = true;
    for (u32 i = 0; i < 3; ++i) {
        const auto reserved = m.read_u16(at + group_at + 2 + 2 * i);
        if (reserved.is_err()) return false;
        if (reserved.value() != 0) out.reserved_zero = false;
    }
    return true;
}

} // namespace vitreo::winapi::processor_topology

namespace vitreo::winapi::processor_topology {

namespace {

Topology build(const platform::HostHardwareProfile& host) {
    Topology t;
    t.count = processor_count();
    t.mask = processor_mask();
    const auto& cpu = host.cpu;
    const u32 classes = static_cast<u32>(cpu.core_classes.size());
    const u32 line = cpu.cache_line_bytes;
    auto class_of = [&](u32 p) -> u32 {
        return p < cpu.processors.size() ? std::min(cpu.processors[p].core_class, classes == 0 ? 0 : classes - 1) : 0;
    };
    auto cluster_key = [&](u32 p) -> std::pair<u32, u32> {
        return {class_of(p), p < cpu.processors.size() ? cpu.processors[p].cluster : 0};
    };
    // Learn (PROCESSOR_RELATIONSHIP): a higher efficiency class has greater
    // performance; it is nonzero only on a heterogeneous system. macOS lists
    // its classes fastest first.
    for (u32 p = 0; p < t.count; ++p) {
        t.efficiency_class.push_back(classes > 1 ? static_cast<u8>(classes - 1 - class_of(p)) : 0);
        u64 module = 0;
        for (u32 q = 0; q < t.count; ++q) {
            if (cluster_key(q) == cluster_key(p)) module |= 1ull << q;
        }
        t.module_mask.push_back(module);
        const platform::HostCoreClass* core = classes == 0 ? nullptr : &cpu.core_classes[class_of(p)];
        t.max_mhz.push_back(core != nullptr ? core->max_mhz : 0);
        t.last_level_cache.push_back(cpu.l3_bytes != 0 ? t.mask : module);
    }
    auto add = [&](u8 level, CacheType type, u64 size, u64 mask, u32 first) {
        const u32 ways = platform::reported_cache_ways(level, size, line);
        if (ways == 0 || ways > 0xFF || size > 0xFFFFFFFFull) return;
        t.caches.push_back({level, type, static_cast<u8>(ways), static_cast<u16>(line),
                            static_cast<u32>(size), mask, first});
    };
    for (u32 p = 0; p < t.count && classes != 0; ++p) {
        const platform::HostCoreClass& core = cpu.core_classes[class_of(p)];
        add(1, CacheType::Data, core.l1d_bytes, 1ull << p, p);
        add(1, CacheType::Instruction, core.l1i_bytes, 1ull << p, p);
        if (static_cast<u32>(__builtin_ctzll(t.module_mask[p])) == p) {
            add(2, CacheType::Unified, core.l2_bytes, t.module_mask[p], p);
        }
        if (p == 0) add(3, CacheType::Unified, cpu.l3_bytes, t.mask, 0);
    }
    return t;
}

struct Writer {
    std::vector<u8> bytes;
    void u8_at(usize o, u8 v) { bytes[o] = v; }
    void u16_at(usize o, u16 v) { std::memcpy(bytes.data() + o, &v, 2); }
    void u32_at(usize o, u32 v) { std::memcpy(bytes.data() + o, &v, 4); }
    void ptr_at(usize o, u64 v, bool wide) {
        if (wide) std::memcpy(bytes.data() + o, &v, 8);
        else { const u32 narrow = static_cast<u32>(v); std::memcpy(bytes.data() + o, &narrow, 4); }
    }
    usize grow(usize size) {
        const usize start = bytes.size();
        bytes.resize(start + size, 0);
        return start;
    }
    /// GROUP_AFFINITY at @p o: KAFFINITY Mask, then WORD Group.
    void group_affinity_at(usize o, u64 mask, bool wide) { ptr_at(o, mask, wide); }
};

// SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX records. Layouts from winnt.h, sizes
// measured (processor_topology_probe): processor/NUMA 48 (x64) / 44 (x86),
// cache 56 / 52, group 80 / 76. The GROUP_AFFINITY of a processor or NUMA
// record is at 32, of a cache record at 40.
constexpr u32 kRelationProcessorCore = 0, kRelationNumaNode = 1, kRelationCache = 2,
              kRelationProcessorPackage = 3, kRelationGroup = 4, kRelationProcessorDie = 5,
              kRelationNumaNodeEx = 6, kRelationProcessorModule = 7, kRelationAll = 0xFFFF;

void processor_record(Writer& w, u32 relationship, u8 efficiency, u64 mask, bool wide) {
    const usize size = wide ? 48 : 44;
    const usize o = w.grow(size);
    w.u32_at(o + 0, relationship);
    w.u32_at(o + 4, static_cast<u32>(size));
    w.u8_at(o + 8, 0);             // Flags: no SMT
    w.u8_at(o + 9, efficiency);
    w.u16_at(o + 30, 1);           // GroupCount
    w.group_affinity_at(o + 32, mask, wide);
}

void numa_record(Writer& w, u64 mask, bool wide) {
    const usize size = wide ? 48 : 44;
    const usize o = w.grow(size);
    w.u32_at(o + 0, kRelationNumaNode);  // NumaNodeEx queries report it the same way
    w.u32_at(o + 4, static_cast<u32>(size));
    w.u32_at(o + 8, 0);                  // NodeNumber
    w.u16_at(o + 30, 1);                 // GroupCount
    w.group_affinity_at(o + 32, mask, wide);
}

void cache_record(Writer& w, const Cache& c, bool wide) {
    const usize size = wide ? 56 : 52;
    const usize o = w.grow(size);
    w.u32_at(o + 0, kRelationCache);
    w.u32_at(o + 4, static_cast<u32>(size));
    w.u8_at(o + 8, c.level);
    w.u8_at(o + 9, c.associativity);
    w.u16_at(o + 10, c.line_size);
    w.u32_at(o + 12, c.size);
    w.u32_at(o + 16, static_cast<u32>(c.type));
    w.u16_at(o + 38, 1);                 // GroupCount
    w.group_affinity_at(o + 40, c.mask, wide);
}

void group_record(Writer& w, const Topology& t, bool wide) {
    const usize size = wide ? 80 : 76;
    const usize o = w.grow(size);
    w.u32_at(o + 0, kRelationGroup);
    w.u32_at(o + 4, static_cast<u32>(size));
    w.u16_at(o + 8, 1);                  // MaximumGroupCount
    w.u16_at(o + 10, 1);                 // ActiveGroupCount
    w.u8_at(o + 32, static_cast<u8>(t.count));  // MaximumProcessorCount
    w.u8_at(o + 33, static_cast<u8>(t.count));  // ActiveProcessorCount
    w.ptr_at(o + 72, t.mask, wide);      // ActiveProcessorMask
}

} // namespace

const Topology& topology() {
    static std::mutex lock;
    static const platform::HostHardwareProfile* source = nullptr;
    static Topology cached;
    std::lock_guard guard(lock);
    const platform::HostHardwareProfile& host = platform::host_hardware();
    if (source != &host) {
        cached = build(host);
        source = &host;
    }
    return cached;
}

std::vector<u8> logical_processor_information(bool wide) {
    // SYSTEM_LOGICAL_PROCESSOR_INFORMATION { ULONG_PTR ProcessorMask;
    //   LOGICAL_PROCESSOR_RELATIONSHIP Relationship; union (16 bytes) }:
    // Relationship at 4 / 8, the union at 8 / 16. Only cores, caches, the
    // package and the NUMA node appear (measured).
    const Topology& t = topology();
    const usize size = wide ? 32 : 24;
    const usize relation_at = wide ? 8 : 4;
    const usize union_at = wide ? 16 : 8;
    Writer w;
    auto record = [&](u64 mask, u32 relationship) {
        const usize o = w.grow(size);
        w.ptr_at(o, mask, wide);
        w.u32_at(o + relation_at, relationship);
        return o + union_at;
    };
    for (u32 p = 0; p < t.count; ++p) {
        record(1ull << p, kRelationProcessorCore);  // Flags 0: no SMT
        for (const Cache& c : t.caches) {
            if (c.first_processor != p) continue;
            const usize u = record(c.mask, kRelationCache);
            w.u8_at(u + 0, c.level);
            w.u8_at(u + 1, c.associativity);
            w.u16_at(u + 2, c.line_size);
            w.u32_at(u + 4, c.size);
            w.u32_at(u + 8, static_cast<u32>(c.type));
        }
    }
    record(t.mask, kRelationProcessorPackage);
    record(t.mask, kRelationNumaNode);  // NodeNumber 0
    return std::move(w.bytes);
}

std::optional<std::vector<u8>> logical_processor_information_ex(u32 relationship, bool wide) {
    // Measured order of RelationAll (processor_topology_probe): the package;
    // then for each core its record, its module's when the module starts
    // there, and the caches first seen on it; then the NUMA node and the
    // group. RelationProcessorDie is answered on its own but not listed under
    // RelationAll, and RelationNumaNodeEx returns a RelationNumaNode record.
    const Topology& t = topology();
    Writer w;
    switch (relationship) {
        case kRelationProcessorCore:
            for (u32 p = 0; p < t.count; ++p)
                processor_record(w, kRelationProcessorCore, t.efficiency_class[p], 1ull << p, wide);
            break;
        case kRelationNumaNode:
        case kRelationNumaNodeEx:
            numa_record(w, t.mask, wide);
            break;
        case kRelationCache:
            for (const Cache& c : t.caches) cache_record(w, c, wide);
            break;
        case kRelationProcessorPackage:
        case kRelationProcessorDie:
            processor_record(w, relationship, 0, t.mask, wide);
            break;
        case kRelationGroup:
            group_record(w, t, wide);
            break;
        case kRelationProcessorModule:
            for (u32 p = 0; p < t.count; ++p) {
                if (static_cast<u32>(__builtin_ctzll(t.module_mask[p])) == p)
                    processor_record(w, kRelationProcessorModule, 0, t.module_mask[p], wide);
            }
            break;
        case kRelationAll:
            processor_record(w, kRelationProcessorPackage, 0, t.mask, wide);
            for (u32 p = 0; p < t.count; ++p) {
                processor_record(w, kRelationProcessorCore, t.efficiency_class[p], 1ull << p, wide);
                if (static_cast<u32>(__builtin_ctzll(t.module_mask[p])) == p)
                    processor_record(w, kRelationProcessorModule, 0, t.module_mask[p], wide);
                for (const Cache& c : t.caches) {
                    if (c.first_processor == p) cache_record(w, c, wide);
                }
            }
            numa_record(w, t.mask, wide);
            group_record(w, t, wide);
            break;
        default:
            return std::nullopt;
    }
    return std::move(w.bytes);
}

std::vector<u8> cpu_set_information() {
    // SYSTEM_CPU_SET_INFORMATION (winnt.h; 32 bytes on both ABIs): Size,
    // Type (CpuSetInformation 0), Id at 8, Group at 12, LogicalProcessorIndex
    // 14, CoreIndex 15, LastLevelCacheIndex 16, NumaNodeIndex 17,
    // EfficiencyClass 18, AllFlags 19, SchedulingClass 20, AllocationTag 24.
    // Measured: Ids start at 256; CoreIndex and LastLevelCacheIndex name the
    // first logical processor of the core / of the processors sharing the
    // last-level cache.
    const Topology& t = topology();
    Writer w;
    for (u32 p = 0; p < t.count; ++p) {
        const usize o = w.grow(32);
        w.u32_at(o + 0, 32);
        w.u32_at(o + 4, 0);
        w.u32_at(o + 8, 256 + p);
        w.u16_at(o + 12, 0);
        w.u8_at(o + 14, static_cast<u8>(p));
        w.u8_at(o + 15, static_cast<u8>(p));
        w.u8_at(o + 16, static_cast<u8>(__builtin_ctzll(t.last_level_cache[p])));
        w.u8_at(o + 17, 0);
        w.u8_at(o + 18, t.efficiency_class[p]);
    }
    return std::move(w.bytes);
}

} // namespace vitreo::winapi::processor_topology
