// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: main-image TLS directory and the primary thread TLS array

#include "run_internal.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/mapper/section_mapper.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <limits>
#include <vector>

namespace vitreo::cli {

std::optional<RunResult> setup_main_image_tls(
    const RunConfig& config, GuestMemory& memory, const pe::ParsedPe& pe,
    const pe::MappedImage& image, const pe::TebInfo& teb_info,
    u64& pe_tls_data_start, u32& pe_tls_data_size,
    u64& pe_tls_callbacks_addr) {
    {
        const auto& tls_dir = pe.data_directories[pe::DIR_TLS];
        if (tls_dir.VirtualAddress != 0 && tls_dir.Size != 0) {
            u64 tls_dir_va = image.base_address + tls_dir.VirtualAddress;

            if (pe.is_64bit) {
                auto raw_data_start = memory.read_u64(tls_dir_va + 0);
                auto raw_data_end   = memory.read_u64(tls_dir_va + 8);
                auto index_addr     = memory.read_u64(tls_dir_va + 16);
                auto callbacks_addr = memory.read_u64(tls_dir_va + 24);
                auto zero_fill      = memory.read_u32(tls_dir_va + 32);

                if (raw_data_start.is_err() || raw_data_end.is_err() ||
                    index_addr.is_err() || callbacks_addr.is_err() ||
                    zero_fill.is_err() || index_addr.value() == 0 ||
                    raw_data_end.value() < raw_data_start.value()) {
                    return RunResult{1, 0, "Invalid main-image TLS directory", false};
                }
                {
                    u64 tls_data_start = raw_data_start.value();
                    u64 tls_data_end   = raw_data_end.value();
                    u64 tls_index_va   = index_addr.value();
                    u64 tls_callbacks  = callbacks_addr.is_ok() ? callbacks_addr.value() : 0;
                    u64 tls_data_span = tls_data_end - tls_data_start;
                    const u64 tls_total_size = tls_data_span + zero_fill.value();
                    if (tls_total_size < tls_data_span ||
                        tls_data_span > std::numeric_limits<u32>::max() ||
                        tls_total_size > std::numeric_limits<u32>::max() - 0xFFFu) {
                        return RunResult{1, 0, "Oversized main-image TLS", false};
                    }
                    u32 tls_data_size  = static_cast<u32>(tls_data_span);

                    pe_tls_data_start = tls_data_start;
                    pe_tls_data_size = tls_data_size;
                    pe_tls_callbacks_addr = tls_callbacks;

                    u64 tls_array_addr = teb_info.peb_address + 0x1000;
                    constexpr u64 TLS_SLOTS = 64;
                    auto tls_arr_result = memory.map_region(tls_array_addr, TLS_SLOTS * 8,
                                                             MemoryProtection::ReadWrite);
                    if (tls_arr_result.is_err()) {
                        tls_array_addr = memory.find_free_gap(
                            teb_info.peb_address + 0x1000, TLS_SLOTS * 8);
                        if (tls_array_addr == 0) {
                            return RunResult{1, 0,
                                "Unable to allocate main-image TLS array", false};
                        }
                        tls_arr_result = memory.map_region(tls_array_addr, TLS_SLOTS * 8,
                                                            MemoryProtection::ReadWrite);
                    }
                    if (tls_arr_result.is_err()) {
                        return RunResult{1, 0,
                            "Unable to allocate main-image TLS array", false};
                    }

                    {
                        u64 tls_data_alloc = (tls_array_addr + TLS_SLOTS * 8 + 0xFFFu) & ~0xFFFULL;
                        // Whole host pages: the main thread's thread_local
                        // data is among the hottest memory in the process.
                        // See round_up_host_page.
                        const u64 alloc_size = round_up_host_page(
                            std::max<u64>(tls_total_size, 256u));
                        tls_data_alloc = memory.find_free_gap(
                            tls_data_alloc, static_cast<usize>(alloc_size),
                            kLinearHostPageSize);
                        if (tls_data_alloc == 0 ||
                            memory.map_region(tls_data_alloc, alloc_size,
                                              MemoryProtection::ReadWrite)
                                .is_err()) {
                            return RunResult{
                                1, 0,
                                "Unable to allocate main-image TLS storage",
                                false};
                        }

                        if (tls_data_size > 0) {
                            std::vector<u8> tls_template(tls_data_size);
                            if (memory.read_bytes(tls_data_start, tls_template.data(), tls_data_size).is_err() ||
                                memory.write_bytes(tls_data_alloc, tls_template.data(), tls_data_size).is_err()) {
                                return RunResult{1, 0,
                                    "Unable to copy main-image TLS template", false};
                            }
                        }

                        if (memory.write_u64(tls_array_addr, tls_data_alloc).is_err() ||
                            memory.write_u32(tls_index_va, 0).is_err() ||
                            memory.write_u64(teb_info.teb_address + 0x58,
                                             tls_array_addr).is_err()) {
                            return RunResult{1, 0,
                                "Unable to publish main-image TLS", false};
                        }

                        if (config.verbose) {
                            VITREO_TRACE( "[vitreo] TLS64: array=0x%llx, data=0x%llx (%u bytes), "
                                         "_tls_index at 0x%llx, callbacks=0x%llx\n",
                                         static_cast<unsigned long long>(tls_array_addr),
                                         static_cast<unsigned long long>(tls_data_alloc),
                                         tls_data_size,
                                         static_cast<unsigned long long>(tls_index_va),
                                         static_cast<unsigned long long>(tls_callbacks));
                        }
                    }
                }
            } else {
                // Read IMAGE_TLS_DIRECTORY32 from mapped memory
                auto raw_data_start = memory.read_u32(tls_dir_va + 0);
                auto raw_data_end   = memory.read_u32(tls_dir_va + 4);
                auto index_addr     = memory.read_u32(tls_dir_va + 8);
                auto callbacks_addr = memory.read_u32(tls_dir_va + 12);

                if (raw_data_start.is_ok() && raw_data_end.is_ok() && index_addr.is_ok()) {
                    u32 tls_data_start = raw_data_start.value();
                    u32 tls_data_end   = raw_data_end.value();
                    u32 tls_index_va   = index_addr.value();
                    u32 tls_data_size  = tls_data_end - tls_data_start;

                    // Save for per-thread TLS allocation
                    pe_tls_data_start = tls_data_start;
                    pe_tls_data_size  = tls_data_size;
                    pe_tls_callbacks_addr = callbacks_addr.is_ok() ? callbacks_addr.value() : 0;

                    // Allocate TLS array (void* array, at least 64 slots)
                    // Place it right after PEB at a fixed address
                    u32 teb_addr = static_cast<u32>(teb_info.teb_address);
                    u32 tls_array_addr = static_cast<u32>(teb_info.peb_address) + 0x1000;
                    constexpr u32 TLS_SLOTS = 64;
                    auto tls_arr_result = memory.map_region(tls_array_addr, TLS_SLOTS * 4,
                                                             MemoryProtection::ReadWrite);
                    if (tls_arr_result.is_err()) {
                        // Try alternate address
                        tls_array_addr += 0x1000;
                        tls_arr_result = memory.map_region(tls_array_addr, TLS_SLOTS * 4,
                                                            MemoryProtection::ReadWrite);
                    }

                    if (tls_arr_result.is_ok()) {
                        // Allocate TLS data slot (copy of the template)
                        // Place data on a separate page to avoid overlap with TLS array
                        u32 tls_data_alloc = (tls_array_addr + TLS_SLOTS * 4 + 0xFFFu) & ~0xFFFu;
                        u32 alloc_size = (tls_data_size > 0) ? tls_data_size : 256;
                        alloc_size = (alloc_size + 0xFFF) & ~0xFFFu;  // page-align
                        auto tls_data_result = memory.map_region(
                            tls_data_alloc, alloc_size,
                            MemoryProtection::ReadWrite);
                        constexpr usize MAX_TLS_PLACEMENT_ATTEMPTS = 4096;
                        for (usize attempt = 1;
                             tls_data_result.is_err() &&
                             attempt < MAX_TLS_PLACEMENT_ATTEMPTS;
                             ++attempt) {
                            tls_data_alloc += 0x1000;
                            tls_data_result = memory.map_region(
                                tls_data_alloc, alloc_size,
                                MemoryProtection::ReadWrite);
                        }
                        if (tls_data_result.is_err()) {
                            return RunResult{
                                1, 0,
                                "Unable to allocate main-image TLS storage",
                                false};
                        }
                        
                        VITREO_TRACE( "[tls-debug] array=0x%08x  data_alloc=0x%08x  "
                                     "alloc_size=0x%x  map_ok=%d  data_size=%u\n",
                                     tls_array_addr, tls_data_alloc, alloc_size,
                                     tls_data_result.is_ok() ? 1 : 0, tls_data_size);

                        if (tls_data_size > 0) {
                            // Copy TLS template data from the mapped PE image
                            std::vector<u8> tls_template(tls_data_size);
                            auto rd = memory.read_bytes(tls_data_start, tls_template.data(), tls_data_size);
                            if (rd.is_ok()) {
                                (void)memory.write_bytes(tls_data_alloc, tls_template.data(), tls_data_size);
                                // Debug: verify TLS copy
                                if (tls_data_size > 0x484) {
                                    u32 val_src = 0, val_dst = 0;
                                    auto r1 = memory.read_u32(tls_data_start + 0x480);
                                    auto r2 = memory.read_u32(tls_data_alloc + 0x480);
                                    if (r1.is_ok()) val_src = r1.value();
                                    if (r2.is_ok()) val_dst = r2.value();
                                    VITREO_TRACE( "[tls-debug] src[0x480]=0x%08x  dst[0x480]=0x%08x  "
                                                 "src_addr=0x%08x  dst_addr=0x%08x  size=%u\n",
                                                 val_src, val_dst, tls_data_start + 0x480,
                                                 tls_data_alloc + 0x480, tls_data_size);
                                }
                            } else {
                                VITREO_TRACE( "[tls-debug] read_bytes FAILED from 0x%08x size=%u\n",
                                             tls_data_start, tls_data_size);
                            }
                        }

                        // TLS array[0] = pointer to this thread's TLS data
                        (void)memory.write_u32(tls_array_addr, tls_data_alloc);

                        // Write _tls_index = 0 at the PE's TLS index address
                        (void)memory.write_u32(tls_index_va, 0);

                        // Set TEB.ThreadLocalStoragePointer (offset 0x2C) to TLS array
                        (void)memory.write_u32(teb_addr + 0x2C, tls_array_addr);

                        if (config.verbose) {
                            VITREO_TRACE( "[vitreo] TLS: array=0x%08x, data=0x%08x (%u bytes), "
                                         "_tls_index at 0x%08x\n",
                                         tls_array_addr, tls_data_alloc, tls_data_size, tls_index_va);
                        }
                    }
                }
            }
        }
    }

    // A main image with NO TLS directory still needs a live
    // TEB.ThreadLocalStoragePointer. The block above only publishes one when
    // the EXE itself has an IMAGE_TLS_DIRECTORY, and teb_setup.cpp seeds the
    // field with 0 — so a DLL loaded later that DOES have static TLS reads a
    // null array from gs:[0x58] on the MAIN thread and faults at
    // `mov reg,[rcx+rdx*8]` with rcx = 0.
    //
    // Measured: a browser helper executable has no TLS directory but its
    // loaded library does, so the first UI navigation took an access
    // violation reading 0x8 and killed the renderer.
    // Worker threads already allocate this unconditionally — see the "Always
    // allocate a per-thread TLS array" path in guest_thread_context.cpp. This
    // gives the main thread the same guarantee.
    if (teb_info.teb_address != 0) {
        const u64 tls_ptr_field =
            teb_info.teb_address + (pe.is_64bit ? 0x58u : 0x2Cu);
        const auto existing = pe.is_64bit
            ? memory.read_u64(tls_ptr_field)
            : memory.read_u32(tls_ptr_field).map([](u32 v) {
                  return static_cast<u64>(v);
              });
        if (existing.is_ok() && existing.value() == 0) {
            constexpr u64 TLS_SLOTS = 64;
            const u64 slot_bytes =
                round_up_host_page(TLS_SLOTS * (pe.is_64bit ? 8u : 4u));
            u64 array_addr = memory.find_free_gap(
                teb_info.peb_address + 0x1000, slot_bytes, kLinearHostPageSize);
            if (array_addr != 0 &&
                memory.map_region(array_addr, slot_bytes,
                                  MemoryProtection::ReadWrite).is_ok()) {
                // Slots stay zero: a DLL's TLS callback fills its own index.
                // The array being *mapped* is what stops the null dereference.
                const bool published = pe.is_64bit
                    ? memory.write_u64(tls_ptr_field, array_addr).is_ok()
                    : memory.write_u32(tls_ptr_field,
                          static_cast<u32>(array_addr)).is_ok();
                if (published) {
                    VITREO_TRACE("[vitreo] main-thread TLS array (no EXE TLS "
                                 "directory) at 0x%llx\n",
                                 static_cast<unsigned long long>(array_addr));
                }
            }
        }
    }

    return std::nullopt;
}

} // namespace vitreo::cli
