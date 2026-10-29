#pragma once
/// @file kernel32_ext_internal.hpp
/// @brief Shared surface between kernel32_ext.cpp and the files split out of it.
///
/// Private to kernel32's extended surface. These were file-local helpers of
/// kernel32_ext.cpp; this header exists so the halves can still see each other,
/// not to publish them.
///
/// Both entries are here because a split cut across a caller and its callee.
/// `normalize_dir_path` is defined near the top of kernel32_ext.cpp and used by
/// the path APIs that moved to kernel32_environment.cpp; `get_host_disk_stats`
/// moved with those APIs and is still called from three places that stayed.

#include <vitreo/common.hpp>

#include <string>

namespace vitreo::winapi {

/// Trailing-backslash normalisation for a directory path, the form the Win32
/// path APIs hand back. Defined in kernel32_ext.cpp.
[[nodiscard]] std::string normalize_dir_path(std::string path);

/// Host filesystem capacity, reported to the guest as its disk. Defined in
/// kernel32_environment.cpp.
void get_host_disk_stats(u64& out_total_bytes, u64& out_free_bytes);

/// VITREO_IOCP_DIAG tracing. Timestamps each line and takes an exclusive lock on
/// stderr, so completions arriving on several threads at once stay readable
/// instead of interleaving mid-line. Defined in kernel32_ext.cpp.
void iocp_diag_log(const char* format, ...);

/// Host logical CPU count clamped to a 32-bit processor group. Used by
/// GetLogicalProcessorInformation* and the Wave 3 system-info stubs.
/// Defined in kernel32_ext.cpp.
[[nodiscard]] u32 host_logical_processor_count() noexcept;

} // namespace vitreo::winapi
