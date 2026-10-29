// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file processor_topology.hpp
/// @brief The processors Windows reports, derived from the host hardware
/// profile (platform::host_hardware()).
///
/// Every kernel32 surface that counts or masks processors uses these, so
/// GetSystemInfo, the processor counts, affinity masks, GetLogicalProcessor-
/// Information(Ex), CPU sets and NUMA all describe the same processors.
/// Processor N is the host's logical CPU N; all are in group 0.

#include <vitreo/types.hpp>

#include <optional>
#include <vector>

namespace vitreo::winapi {

class Marshaler;

namespace processor_topology {

/// Logical processors in the single processor group: the host's, at most 64.
[[nodiscard]] u32 processor_count() noexcept;

/// One bit per logical processor (the system affinity mask).
[[nodiscard]] u64 processor_mask() noexcept;

/// A DWORD_PTR / KAFFINITY mask as the calling guest's ABI holds it: a 32-bit
/// guest's is 32 bits wide, so it sees at most 32 processors.
[[nodiscard]] u64 guest_affinity(const Marshaler& m, u64 mask) noexcept;

/// GROUP_AFFINITY { KAFFINITY Mask; WORD Group; WORD Reserved[3]; }:
/// 12 bytes on x86 (Group at 4), 16 on x64 (Group at 8).
[[nodiscard]] u32 group_affinity_size(const Marshaler& m) noexcept;
[[nodiscard]] bool write_group_affinity(Marshaler& m, GuestAddr at, u64 mask, u16 group);

struct GroupAffinity {
    u64 mask = 0;
    u16 group = 0;
    bool reserved_zero = true;
};
[[nodiscard]] bool read_group_affinity(Marshaler& m, GuestAddr at, GroupAffinity& out);

/// PROCESSOR_CACHE_TYPE.
enum class CacheType : u8 { Unified = 0, Instruction = 1, Data = 2 };

/// One cache, as GetLogicalProcessorInformation(Ex) describes it.
struct Cache {
    u8 level = 0;
    CacheType type = CacheType::Unified;
    u8 associativity = 0;           ///< platform::reported_cache_ways
    u16 line_size = 0;
    u32 size = 0;
    u64 mask = 0;                   ///< the processors sharing it
    u32 first_processor = 0;        ///< the lowest of them
};

/// The guest-visible processor topology: one package, one NUMA node, one
/// group; a core per logical processor (Apple silicon has no SMT); a module
/// per cluster of cores sharing an L2.
struct Topology {
    u32 count = 0;
    u64 mask = 0;
    std::vector<u8> efficiency_class;   ///< per processor: 0 = most efficient class
    std::vector<u64> module_mask;       ///< per processor: its cluster's processors
    std::vector<u32> max_mhz;           ///< per processor: its class's highest clock
    std::vector<u64> last_level_cache;  ///< per processor: processors sharing its LLC
    /// Every cache, in the order Windows lists them: after the first
    /// processor that has it, data before instruction, inner before outer.
    std::vector<Cache> caches;
};

/// Built from platform::host_hardware(); rebuilt when a test installs a
/// different profile.
[[nodiscard]] const Topology& topology();

/// GetLogicalProcessorInformation's SYSTEM_LOGICAL_PROCESSOR_INFORMATION
/// array (24-byte records on x86, 32 on x64).
[[nodiscard]] std::vector<u8> logical_processor_information(bool wide);

/// GetLogicalProcessorInformationEx's records for @p relationship, or
/// nothing for a relationship Windows rejects (see processor_topology.cpp).
[[nodiscard]] std::optional<std::vector<u8>> logical_processor_information_ex(u32 relationship, bool wide);

/// GetSystemCpuSetInformation's SYSTEM_CPU_SET_INFORMATION array (32 bytes
/// each, the same on both ABIs).
[[nodiscard]] std::vector<u8> cpu_set_information();

} // namespace processor_topology

} // namespace vitreo::winapi
