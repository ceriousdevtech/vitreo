#pragma once

/// @file tls_processor.hpp
/// @brief TLS directory processing (PE-006)

#include <vitreo/common.hpp>
#include "core/memory/guest_memory.hpp"

#include <vector>

namespace vitreo::pe {

// =============================================================================
// TLS Directory Structures
// =============================================================================

/// Parsed TLS directory info (mirrors IMAGE_TLS_DIRECTORY64 layout)
struct TlsDirectory {
    u64 raw_data_start{0};
    u64 raw_data_end{0};
    u64 address_of_index{0};
    u64 address_of_callbacks{0};
    u32 zero_fill_size{0};
    u32 characteristics{0};
};

/// Processed TLS information ready for thread allocation
struct TlsInfo {
    u32 tls_index{0};
    std::vector<u8> template_data;
    u32 zero_fill_size{0};
    std::vector<GuestAddr> callbacks;
};

// =============================================================================
// TlsProcessor Class
// =============================================================================

/// Processes TLS directories from PE images
///
/// Handles parsing TLS directory entries, extracting template data and
/// callback addresses, and allocating per-thread TLS storage.
class TlsProcessor {
public:
    explicit TlsProcessor(GuestMemory& memory);

    TlsProcessor(const TlsProcessor&) = delete;
    TlsProcessor& operator=(const TlsProcessor&) = delete;
    ~TlsProcessor() = default;

    /// Parse TLS directory and extract template data + callbacks
    /// @param image_base Guest base address of the PE image
    /// @param dir Parsed TLS directory descriptor
    /// @return Processed TLS info or error
    [[nodiscard]] VResult<TlsInfo> parse(GuestAddr image_base,
                                          const TlsDirectory& dir);

    /// Get pending callback addresses (populated after parse)
    [[nodiscard]] const std::vector<GuestAddr>& pending_callbacks() const noexcept;

    /// Allocate TLS storage for a new thread (copies template + zero fill)
    /// @param info TLS info from a previous parse call
    /// @return Guest address of the allocated TLS block or error
    [[nodiscard]] VResult<GuestAddr> allocate_thread_tls(const TlsInfo& info);

private:
    GuestMemory& m_memory;
    std::vector<GuestAddr> m_pending_callbacks;
    u32 m_next_tls_index{0};
    static constexpr GuestAddr TLS_STORAGE_BASE = 0x00800000;
    GuestAddr m_next_tls_alloc{TLS_STORAGE_BASE};
};

} // namespace vitreo::pe
