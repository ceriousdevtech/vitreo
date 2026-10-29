/// @file module.cpp
/// @brief Implementation of module management APIs (WIN32-033)

#include "module.hpp"
#include "module_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/imports/api_set_resolver.hpp"
#include "capsule/vfs/filesystem.hpp"
#include <vitreo/result.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>
#include <unordered_set>

#include <vitreo/diagnostics.hpp>

/// @file setup_dll_tls.cpp
/// @brief ModuleManager::setup_dll_tls
///
/// Closed method cut from module.cpp.

namespace vitreo::winapi {

void ModuleManager::setup_dll_tls(GuestAddr image_base,
                                  u32 bitness,
                                  GuestAddr tls_dir_va,
                                  const char* dbg_path) {
    (void)image_base; // image base already baked into tls_dir_va

    if (!m_memory || tls_dir_va == 0 || m_main_teb == 0) return;
    std::lock_guard<std::mutex> lock(m_tls_mutex);

    // ---- 1. Read IMAGE_TLS_DIRECTORY{32,64} ------------------------------
    u64 raw_start = 0, raw_end = 0, index_va = 0;
    u32 zero_fill = 0;
    if (bitness == 64) {
        auto a = m_memory->read_u64(tls_dir_va + 0);
        auto b = m_memory->read_u64(tls_dir_va + 8);
        auto c = m_memory->read_u64(tls_dir_va + 16);
        auto d = m_memory->read_u32(tls_dir_va + 32);
        if (a.is_err() || b.is_err() || c.is_err()) return;
        raw_start = a.value();
        raw_end   = b.value();
        index_va  = c.value();
        zero_fill = d.is_ok() ? d.value() : 0;
    } else {
        auto a = m_memory->read_u32(tls_dir_va + 0);
        auto b = m_memory->read_u32(tls_dir_va + 4);
        auto c = m_memory->read_u32(tls_dir_va + 8);
        auto d = m_memory->read_u32(tls_dir_va + 16);
        if (a.is_err() || b.is_err() || c.is_err()) return;
        raw_start = a.value();
        raw_end   = b.value();
        index_va  = c.value();
        zero_fill = d.is_ok() ? d.value() : 0;
    }
    if (index_va == 0) return; // nowhere to publish the slot index

    u64 template_size = (raw_end > raw_start) ? (raw_end - raw_start) : 0;
    u64 block_size    = template_size + zero_fill;
    if (block_size == 0) block_size = 256;
    // Whole host pages, as for every per-thread runtime structure (see
    // round_up_host_page). The slot frees every block it owns with this one
    // size, including blocks register_thread_teb backfills later.
    u64 alloc_size    = round_up_host_page(block_size);

    // ---- 2. Allocate the slot index -------------------------------------
    // m_tls_mutex is already held for the whole of this function; taking it
    // again here would deadlock on the non-recursive mutex.
    //
    // Implicit TLS indices have no fixed ceiling (the 64/1088 limit belongs
    // to TlsAlloc). A process may load several hundred DLLs that each have a
    // TLS directory. When an index passes the vectors' capacity, every
    // thread's ThreadLocalStoragePointer vector grows first.
    u32 slot_index = 0;
    bool from_free_list = false;
    if (!m_free_tls_indices.empty()) {
        slot_index = m_free_tls_indices.back();
        m_free_tls_indices.pop_back();
        from_free_list = true;
    } else {
        slot_index = m_next_tls_index++;
    }
    if (slot_index >= m_tls_array_capacity) {
        u32 capacity = m_tls_array_capacity;
        while (capacity <= slot_index) capacity *= 2;
        m_tls_array_capacity = capacity;
        for (ThreadTeb& thread : m_thread_tebs) {
            if (thread.tls_capacity >= capacity) continue;
            if (!grow_thread_tls_array(thread, capacity)) {
                fprintf(stderr,
                        "[module] setup_dll_tls: could not grow TLS vector of "
                        "teb=0x%llx to %u entries for \"%s\"\n",
                        static_cast<unsigned long long>(thread.teb_addr),
                        capacity, dbg_path ? dbg_path : "<unknown>");
            }
        }
    }
    const auto return_slot_index = [&]() {
        if (from_free_list) {
            m_free_tls_indices.push_back(slot_index);
        } else {
            --m_next_tls_index;
        }
    };

    // ---- 3. Snapshot the template once ----------------------------------
    std::vector<u8> tls_template(static_cast<usize>(template_size));
    if (template_size > 0) {
        auto rd = m_memory->read_bytes(static_cast<GuestAddr>(raw_start),
                                       tls_template.data(),
                                       static_cast<usize>(template_size));
        if (rd.is_err()) {
            return_slot_index();
            return;
        }
    }

    // ---- 4. Allocate and publish one private block per existing thread ---
    usize seeded_threads = 0;
    GuestAddr first_buf_addr = 0;
    std::vector<GuestAddr> owned_blocks;
    for (const ThreadTeb& thread : m_thread_tebs) {
        if (thread.bitness != bitness) continue;
        if (slot_index >= thread.tls_capacity) continue;  // growth failed, logged above

        GuestAddr tls_array_addr = 0;
        if (bitness == 64) {
            auto r = m_memory->read_u64(thread.teb_addr + 0x58);
            if (r.is_err() || r.value() == 0) continue;
            tls_array_addr = static_cast<GuestAddr>(r.value());
        } else {
            auto r = m_memory->read_u32(thread.teb_addr + 0x2C);
            if (r.is_err() || r.value() == 0) continue;
            tls_array_addr = static_cast<GuestAddr>(r.value());
        }

        const GuestAddr search_start = round_up_host_page(m_next_tls_data);
        const GuestAddr buf_addr = m_memory->find_free_gap(
            search_start, static_cast<usize>(alloc_size), kLinearHostPageSize);
        if (buf_addr == 0 ||
            m_memory->map_region(
                buf_addr, static_cast<usize>(alloc_size),
                MemoryProtection::ReadWrite).is_err()) {
            fprintf(
                stderr,
                "[module] setup_dll_tls: failed to map TLS data for "
                "\"%s\" teb=0x%llx\n",
                dbg_path ? dbg_path : "<unknown>",
                static_cast<unsigned long long>(thread.teb_addr));
            continue;
        }
        m_next_tls_data = buf_addr + alloc_size;
        if (template_size > 0) {
            (void)m_memory->write_bytes(
                buf_addr, tls_template.data(),
                static_cast<usize>(template_size));
        }

        Result<void> publish = bitness == 64
            ? m_memory->write_u64(
                tls_array_addr + slot_index * 8,
                static_cast<u64>(buf_addr))
            : m_memory->write_u32(
                tls_array_addr + slot_index * 4,
                static_cast<u32>(buf_addr));
        if (publish.is_err()) {
            (void)m_memory->unmap_region(
                buf_addr, static_cast<usize>(alloc_size));
            continue;
        }
        if (first_buf_addr == 0) first_buf_addr = buf_addr;
        owned_blocks.push_back(buf_addr);
        ++seeded_threads;
    }

    // ---- 5. Publish the process-wide slot index --------------------------
    (void)m_memory->write_u32(static_cast<GuestAddr>(index_va), slot_index);

    // Remember this slot so worker threads can replay the template-copy
    // dance into their own TLS arrays. Without this, any DLL TLS with
    // slot > 0 is NULL on non-main threads and faults inside
    // `__declspec(thread)` accessors.
    m_dll_tls_slots.push_back(DllTlsSlot{
        slot_index,
        static_cast<GuestAddr>(raw_start),
        template_size,
        zero_fill,
        bitness,
        dbg_path ? std::string(dbg_path) : std::string(),
        std::move(owned_blocks),
        static_cast<usize>(alloc_size)
    });

    fprintf(stderr,
            "[module] setup_dll_tls: \"%s\" slot=%u idx_va=0x%llx "
            "first_data=0x%llx threads=%zu tmpl=%llu zerofill=%u\n",
            dbg_path ? dbg_path : "<unknown>",
            slot_index,
            static_cast<unsigned long long>(index_va),
            static_cast<unsigned long long>(first_buf_addr),
            seeded_threads,
            static_cast<unsigned long long>(template_size),
            zero_fill);
}

} // namespace vitreo::winapi
