/// @file tls_processor.cpp
/// @brief TLS directory processing implementation (PE-006)

#include "core/pe/loader/tls_processor.hpp"

namespace vitreo::pe {

// =============================================================================
// Construction
// =============================================================================

TlsProcessor::TlsProcessor(GuestMemory& memory)
    : m_memory(memory) {}

// =============================================================================
// Parse
// =============================================================================

VResult<TlsInfo> TlsProcessor::parse(GuestAddr /*image_base*/,
                                       const TlsDirectory& dir) {
    TlsInfo info{};

    // ---- Template data ----
    if (dir.raw_data_end < dir.raw_data_start) {
        return Result<TlsInfo, Error>::err(Error::InvalidArgument);
    }

    const u64 template_size = dir.raw_data_end - dir.raw_data_start;
    if (template_size > 0) {
        info.template_data.resize(static_cast<usize>(template_size));
        auto read_res = m_memory.read_bytes(
            dir.raw_data_start,
            info.template_data.data(),
            static_cast<usize>(template_size));
        if (!read_res.is_ok()) {
            return Result<TlsInfo, Error>::err(read_res.error());
        }
    }

    // ---- TLS index ----
    info.tls_index = m_next_tls_index++;
    if (dir.address_of_index != 0) {
        auto write_res = m_memory.write_u32(dir.address_of_index, info.tls_index);
        if (!write_res.is_ok()) {
            return Result<TlsInfo, Error>::err(write_res.error());
        }
    }

    // ---- Callbacks ----
    info.zero_fill_size = dir.zero_fill_size;
    m_pending_callbacks.clear();

    if (dir.address_of_callbacks != 0) {
        GuestAddr cb_ptr = dir.address_of_callbacks;
        for (;;) {
            auto val = m_memory.read_u64(cb_ptr);
            if (!val.is_ok()) {
                break; // Bad address terminates the list
            }
            if (val.value() == 0) {
                break; // Null terminator
            }
            m_pending_callbacks.push_back(val.value());
            info.callbacks.push_back(val.value());
            cb_ptr += 8;
        }
    }

    return Result<TlsInfo, Error>::ok(std::move(info));
}

// =============================================================================
// Accessors
// =============================================================================

const std::vector<GuestAddr>& TlsProcessor::pending_callbacks() const noexcept {
    return m_pending_callbacks;
}

// =============================================================================
// Thread TLS Allocation
// =============================================================================

VResult<GuestAddr> TlsProcessor::allocate_thread_tls(const TlsInfo& info) {
    const usize total_size = info.template_data.size()
                           + static_cast<usize>(info.zero_fill_size);

    // Need at least one page even for zero-sized TLS
    const usize alloc_size = (total_size == 0) ? HOST_PAGE_SIZE
                                               : align_up<usize>(total_size, HOST_PAGE_SIZE);

    const GuestAddr alloc_addr = m_next_tls_alloc;
    auto map_res = m_memory.map_region(alloc_addr, alloc_size,
                                       MemoryProtection::ReadWrite);
    if (!map_res.is_ok()) {
        return Result<GuestAddr, Error>::err(map_res.error());
    }

    // Write template data (zero fill is implicit — mapped memory is zeroed)
    if (!info.template_data.empty()) {
        auto wr = m_memory.write_bytes(alloc_addr,
                                       info.template_data.data(),
                                       info.template_data.size());
        if (!wr.is_ok()) {
            return Result<GuestAddr, Error>::err(wr.error());
        }
    }

    m_next_tls_alloc += alloc_size;
    return Result<GuestAddr, Error>::ok(alloc_addr);
}

} // namespace vitreo::pe
