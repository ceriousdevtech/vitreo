#pragma once
/// @file teb_setup.hpp
/// @brief Thread Environment Block (TEB) and Process Environment Block (PEB) setup

#include <vitreo/common.hpp>
#include <string>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::cpu {
struct CpuState;
}

namespace vitreo::pe {

struct TebModuleInfo {
    u64 base_address{0};
    std::string path;
    /// LDR_DATA_TABLE_ENTRY::SizeOfImage. A guest resolving an address to its
    /// module walks the loader list and tests
    /// `DllBase <= addr < DllBase + SizeOfImage`; a zero here makes every such
    /// lookup miss.
    u64 size_of_image{0};
    /// LDR_DATA_TABLE_ENTRY::EntryPoint (DllMain / image entry).
    u64 entry_point{0};
    /// True for the process image. Windows lists the executable in the load-
    /// and memory-order lists but never in InInitializationOrderModuleList,
    /// which holds only DLLs that have been initialized (measured against a
    /// real Windows loader).
    bool is_executable{false};
};

/// Primary-process layout used by `vitreo run`. CreateProcess metadata exposed
/// to a parent must stay in lockstep with this deterministic address.
///
/// 0x7FFE0000 is KUSER_SHARED_DATA on every supported Windows bitness.
/// Putting the TEB there makes ntdll's lock-free GetTickCount /
/// GetSystemTimeAsFileTime / QPC-frequency reads see TEB.Self and PEB
/// pointers as the clock.
inline constexpr u64 PRIMARY_TEB_ADDRESS = 0x7FFD0000;
inline constexpr u64 PRIMARY_PEB_ADDRESS = PRIMARY_TEB_ADDRESS + 0x2000;
inline constexpr u64 SHARED_USER_DATA_ADDRESS = 0x7FFE0000;
inline constexpr u64 SHARED_USER_DATA_SIZE = 0x1000;

/// Configuration for TEB/PEB setup
struct TebConfig {
    u64 stack_base;        ///< Top of stack region
    u64 stack_bottom;      ///< Bottom of stack region
    /// Base of the whole stack allocation (TEB DeallocationStack, x64
    /// +0x1478). May sit below stack_bottom. Zero leaves it unwritten.
    u64 deallocation_stack{0};
    u64 image_base;        ///< PE image base address
    u32 process_id = 1;    ///< Synthetic PID
    u32 thread_id = 1;     ///< Synthetic TID
    bool is_64bit = true;  ///< true for PE32+, false for PE32
    /// UTF-8 Windows image path for PEB ProcessParameters.ImagePathName.
    std::string image_path;
    /// UTF-8 command line copied into the PEB when command_line_va is 0.
    std::string command_line;
    /// Guest VA of the UTF-16 GetCommandLineW buffer. When non-zero,
    /// ProcessParameters.CommandLine.Buffer aliases it (Windows contract).
    u64 command_line_va = 0;
    u16 command_line_bytes = 0;
    /// Modules already mapped by the process loader, in loader order.
    std::vector<TebModuleInfo> loaded_modules;
};

/// Result of TEB/PEB setup
struct TebInfo {
    u64 teb_address;  ///< Address where TEB was placed
    u64 peb_address;  ///< Address where PEB was placed
};

/// Publish a mapped module in PEB_LDR_DATA::InMemoryOrderModuleList.
///
/// Windows exposes every loaded module through this list while its loader
/// initialization runs. The operation is idempotent for an existing base.
/// @return false when the PEB/Ldr projection is unavailable or guest memory
///         cannot hold another loader entry.
[[nodiscard]] bool append_peb_loader_module(GuestMemory& memory,
                                            u64 peb_address,
                                            bool is_64bit,
                                            const TebModuleInfo& module);

/// Remove a module from PEB_LDR_DATA::InMemoryOrderModuleList.
/// @return false when the PEB/Ldr projection is unavailable or the base is
///         not present.
[[nodiscard]] bool remove_peb_loader_module(GuestMemory& memory,
                                            u64 peb_address,
                                            bool is_64bit,
                                            u64 base_address);

/// Set up Thread Environment Block (TEB) and Process Environment Block (PEB)
/// in guest memory and configure the CPU segment register.
///
/// For 64-bit: TEB accessed via GS segment, placed at stack_base
/// For 32-bit: TEB accessed via FS segment, placed at stack_base
///
/// @param memory Guest memory to allocate TEB/PEB in
/// @param cpu CPU state (GS/FS base will be set)
/// @param config TEB configuration
/// @return TebInfo on success, or error on failure
[[nodiscard]] VResult<TebInfo> setup_teb(GuestMemory& memory,
                                         cpu::CpuState& cpu,
                                         const TebConfig& config);

} // namespace vitreo::pe
