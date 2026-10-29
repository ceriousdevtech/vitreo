#pragma once

#include "core/memory/memory_region.hpp"

#include <vitreo/common.hpp>

#include <span>
#include <string>
#include <vector>

namespace vitreo::checkpoint {

inline constexpr u32 kFormatVersion = 1;
inline constexpr u32 kMinimumReaderVersion = 1;
inline constexpr usize kGuestPageSize = 4096;

enum class GuestArchitecture : u8 {
    X86 = 1,
    X64 = 2,
};

struct DeterminismMetadata {
    u64 virtual_time_ns{0};
    u64 rng_seed{0};
    u64 retired_instructions{0};
};

struct MemoryPage {
    GuestAddr address{0};
    MemoryProtection protection{MemoryProtection::None};
    bool has_contents{false};
    std::vector<u8> contents;
};

struct ThreadState {
    u32 guest_thread_id{0};
    u32 cpu_state_abi_size{0};
    std::vector<u8> cpu_state;
};

struct ModuleMetadata {
    GuestAddr base{0};
    u64 image_size{0};
    std::string guest_path;
    std::string identity;
};

/// Provider-defined handle payload. Version 1 intentionally writes an empty,
/// incomplete table: host resources cannot be reconstructed safely merely by
/// copying guest handle values. Future providers can add independently
/// versioned payloads without changing the checkpoint container.
struct HandleProviderState {
    std::string provider;
    u32 provider_version{0};
    std::vector<u8> payload;
};

struct Checkpoint {
    u32 format_version{kFormatVersion};
    GuestArchitecture architecture{GuestArchitecture::X64};
    DeterminismMetadata determinism;
    std::vector<MemoryPage> pages;
    std::vector<ThreadState> threads;
    std::vector<ModuleMetadata> modules;
    bool handles_complete{false};
    std::vector<HandleProviderState> handle_providers;
};

[[nodiscard]] bool encode(const Checkpoint& checkpoint,
                          std::vector<u8>& bytes,
                          std::string& error);
[[nodiscard]] bool decode(std::span<const u8> bytes,
                          Checkpoint& checkpoint,
                          std::string& error);
[[nodiscard]] bool save_file(const std::string& path,
                             const Checkpoint& checkpoint,
                             std::string& error);
[[nodiscard]] bool load_file(const std::string& path,
                             Checkpoint& checkpoint,
                             std::string& error);

} // namespace vitreo::checkpoint
