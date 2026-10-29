// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_map_buffer.cpp
/// @brief The buffer half of ID3D11DeviceContext::Map / Unmap.
///
/// map_buffer and unmap_buffer, and note_uav_buffer_writes, which marks the
/// mappable DEFAULT buffers a draw's or dispatch's UAVs wrote as newer on the
/// GPU.
///
/// Split out of d3d11_context.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_texel_encode.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <string>
#include <mutex>
#include <set>
#include <vector>

namespace vitreo::winapi {

// ID3D11DeviceContext::Map of a buffer, shared with the IDXGISurface facet a
// buffer answers. Returns the HRESULT; on S_OK @p data is the CPU view and
// @p pitch both of D3D11_MAPPED_SUBRESOURCE's pitches.
u32 D3D11Manager::map_buffer(GuestAddr resource, u32 subresource,
                             u32 map_type, u32 map_flags,
                             GuestAddr context_owner, GuestAddr& data,
                             u32& pitch) {
    auto buffer = m_buffers.find(resource);
    const bool staging =
        buffer != m_buffers.end() &&
        buffer->second.desc.usage == D3D11_USAGE_STAGING;
    const bool dynamic =
        buffer != m_buffers.end() &&
        buffer->second.desc.usage == D3D11_USAGE_DYNAMIC;
    const u32 cpu = buffer != m_buffers.end()
                        ? buffer->second.desc.cpu_access_flags
                        : 0;
    const bool staging_map_ok =
        staging &&
        ((map_type == D3D11_MAP_READ &&
          (cpu & D3D11_CPU_ACCESS_READ) != 0) ||
         (map_type == D3D11_MAP_WRITE &&
          (cpu & D3D11_CPU_ACCESS_WRITE) != 0) ||
         (map_type == D3D11_MAP_READ_WRITE &&
          (cpu & D3D11_CPU_ACCESS_READ) != 0 &&
          (cpu & D3D11_CPU_ACCESS_WRITE) != 0));
    // Learn (D3D11_MAP): "The Direct3D 11.1 runtime ... enables mapping
    // dynamic constant buffers and shader resource views (SRVs) of dynamic
    // buffers with D3D11_MAP_WRITE_NO_OVERWRITE" on a device reporting
    // MapNoOverwriteOnDynamicConstantBuffer / MapNoOverwriteOnDynamicBufferSRV,
    // which this one does -- so every dynamic buffer (vertex, index,
    // constant, SRV: the binds DYNAMIC allows) takes it, as measured on a
    // first map with no DISCARD before it (d3d11_fl111_probe "nooverwrite").
    // Unmap writes only the changed bytes in place (MetalBuffer::update): the
    // application promised the GPU is not reading them.
    const bool dynamic_map_ok =
        dynamic &&
        cpu == D3D11_CPU_ACCESS_WRITE &&
        (map_type == D3D11_MAP_WRITE_DISCARD ||
         map_type == D3D11_MAP_WRITE_NO_OVERWRITE);
    // MapOnDefaultBuffers (Learn D3D11_FEATURE_DATA_D3D11_OPTIONS1): a
    // DEFAULT buffer created with CPU access. Measured (d3d11_capsmall_probe
    // "mapdef"): READ needs CPU_ACCESS_READ; WRITE and WRITE_NO_OVERWRITE
    // CPU_ACCESS_WRITE; READ_WRITE both; WRITE_DISCARD never; DO_NOT_WAIT
    // accompanies any of them; the mapping is the buffer's own memory, so
    // every map shows the current contents.
    const bool default_map_ok =
        buffer != m_buffers.end() &&
        buffer->second.desc.usage == D3D11_USAGE_DEFAULT && cpu != 0 &&
        ((map_type == D3D11_MAP_READ && (cpu & D3D11_CPU_ACCESS_READ) != 0) ||
         ((map_type == D3D11_MAP_WRITE ||
           map_type == D3D11_MAP_WRITE_NO_OVERWRITE) &&
          (cpu & D3D11_CPU_ACCESS_WRITE) != 0) ||
         (map_type == D3D11_MAP_READ_WRITE &&
          cpu == (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE)));
    // Learn (D3D11_MAP_FLAG): DO_NOT_WAIT is the one flag, and it
    // cannot accompany WRITE_DISCARD or WRITE_NO_OVERWRITE -- but a
    // mappable DEFAULT buffer takes it with WRITE_NO_OVERWRITE (measured).
    // Waiting is always a valid outcome of it (the GPU may simply be done),
    // so it changes nothing here.
    const bool flags_ok =
        map_flags == 0 ||
        (map_flags == D3D11_MAP_FLAG_DO_NOT_WAIT &&
         map_type != D3D11_MAP_WRITE_DISCARD &&
         (map_type != D3D11_MAP_WRITE_NO_OVERWRITE || default_map_ok));
    if (buffer == m_buffers.end() ||
        buffer->second.device_owner != context_owner ||
        subresource != 0 || !flags_ok ||
        (!staging_map_ok && !dynamic_map_ok && !default_map_ok)) {
        // Say which condition rejected it. "invalid" alone forces a whole
        // extra run to narrow down, and a Map failure on a constant buffer
        // is fatal to UE, so each retry costs a full application launch.
        d3d11_diag(
            "ID3D11DeviceContext::Map invalid buffer=0x%llx sub=%u "
            "type=%u flags=0x%x known=%d owner_ok=%d usage=%u cpu=0x%x "
            "bind=0x%x already_mapped=%d",
            static_cast<unsigned long long>(resource), subresource,
            map_type, map_flags, buffer != m_buffers.end(),
            buffer != m_buffers.end() &&
                buffer->second.device_owner == context_owner,
            buffer != m_buffers.end() ? buffer->second.desc.usage : 0u,
            buffer != m_buffers.end()
                ? buffer->second.desc.cpu_access_flags : 0u,
            buffer != m_buffers.end() ? buffer->second.desc.bind_flags : 0u,
            buffer != m_buffers.end() && buffer->second.mapped);
        return d3d11::E_INVALIDARG;
    }
    // Measured (d3d11_share_ers_probe "BUF STAGING SH map="): a STAGING
    // buffer opened from a shared handle does not Map (E_OUTOFMEMORY); only
    // its creator maps the shared memory.
    if (staging && buffer->second.shared_opening) {
        return d3d11::E_OUTOFMEMORY;
    }

    // Learn (ID3D11DeviceContext::Map, D3D11_MAP_FLAG): Map blocks until
    // the GPU is done with the resource, and with DO_NOT_WAIT returns
    // DXGI_ERROR_WAS_STILL_DRAWING instead while that work is pending.
    // "That work" is the last submission that copied into or out of this
    // buffer (note_gpu_use) -- never the rest of the frame queued after
    // it. Waiting for the whole queue here made every UE readback
    // serialize the render thread with the GPU. WRITE_DISCARD and
    // WRITE_NO_OVERWRITE promise not to touch in-use data and never wait.
    // A use the backend could not place on its timeline falls back to a
    // full wait, and DO_NOT_WAIT cannot poll it, so it waits (a valid
    // outcome of DO_NOT_WAIT: the GPU may simply be done).
    // A mappable DEFAULT buffer the GPU has written (a UAV, a copy, a clear)
    // is the one exception: its mapping must show those writes -- WARP maps
    // the buffer's own memory -- so even WRITE_NO_OVERWRITE waits for them
    // then (a wait is a valid outcome of any Map).
    const bool map_waits_for_gpu =
        map_type == D3D11_MAP_READ || map_type == D3D11_MAP_READ_WRITE ||
        map_type == D3D11_MAP_WRITE ||
        (default_map_ok && buffer->second.gpu_newer);
    // A shared buffer's memory is written by other devices and processes
    // too: what they published is complete, and its contents are the
    // backend's, not this opening's shadow.
    if (map_waits_for_gpu && buffer->second.backend &&
        buffer->second.backend->shared_handle() != 0) {
        m_backend.acquire_shared_writes();
        buffer->second.gpu_newer = true;
        if (buffer->second.gpu_use_gen == 0) {
            buffer->second.gpu_use_gen = kGpuUseUntracked;
        }
    }
    bool gpu_synchronized = true;
    const u64 use_gen = buffer->second.gpu_use_gen;
    if (map_waits_for_gpu && use_gen != 0) {
        const bool untracked = use_gen == kGpuUseUntracked;
        if (!untracked && !m_backend.generation_complete(use_gen)) {
            if (map_flags == D3D11_MAP_FLAG_DO_NOT_WAIT) {
                // Hand over a recording that still holds the work, so a
                // caller polling with DO_NOT_WAIT sees it retire.
                if (use_gen >= m_backend.recording_generation()) {
                    (void)m_backend.flush_completion();
                }
                return kDxgiErrorWasStillDrawing;
            }
            gpu_synchronized = m_backend.wait_generation(use_gen);
        } else if (untracked) {
            gpu_synchronized = m_backend.wait_idle();
        }
        if (gpu_synchronized) buffer->second.gpu_use_gen = 0;
    }

    bool allocated_mapping = false;
    if (buffer->second.mapped_address == 0) {
        // Whole host pages, host-page aligned: a mapped dynamic buffer is
        // written by the guest every frame, and a host page it only partly
        // fills would route every one of those stores through the checked
        // helper. See round_up_host_page.
        const usize mapped_size = static_cast<usize>(round_up_host_page(
            static_cast<u64>(buffer->second.desc.byte_width)));
        Error place_error = Error::OutOfMemory;
        GuestAddr address = m_memory.map_first_fit(
            m_next_map_hint, mapped_size, MemoryProtection::ReadWrite,
            true, kLinearHostPageSize, &place_error);
        const GuestAddr hint_result = address;
        if (address == 0) {
            address = m_memory.map_first_fit(
                0x10000000ull, mapped_size, MemoryProtection::ReadWrite,
                true, kLinearHostPageSize, &place_error);
        }
        const auto mapped = address == 0
                                ? VoidResult::err(place_error)
                                : VoidResult::ok();
        if (mapped.is_err()) {
            std::fprintf(stderr,
                         "[d3d11] Map buffer=0x%llx bytes=%u -> "
                         "E_OUTOFMEMORY: hint=0x%llx gap_from_hint=0x%llx "
                         "gap=0x%llx map_region=%s\n",
                         static_cast<unsigned long long>(resource),
                         buffer->second.desc.byte_width,
                         static_cast<unsigned long long>(m_next_map_hint),
                         static_cast<unsigned long long>(hint_result),
                         static_cast<unsigned long long>(address),
                         error_to_string(mapped.error()));
            return 0x8007000Eu;
        }
        buffer->second.mapped_address = address;
        buffer->second.mapped_size = mapped_size;
        m_next_map_hint = address + mapped_size;
        allocated_mapping = true;
    }

    // D3D11_MAP_WRITE_DISCARD invalidates the entire previous buffer and
    // returns undefined contents (Microsoft Learn). Do not clear the
    // shadow or copy it into guest memory: dynamic upload rings use this
    // path specifically to avoid preserving old data. Unmap reads the
    // guest's replacement contents and publishes them to the backend.
    // The mapped guest pages persist across Unmap. After each successful
    // Unmap they already contain the current shadow bytes, so copying a
    // large dynamic buffer back into those pages on every
    // WRITE_NO_OVERWRITE map is redundant. Seed only a newly allocated
    // mapping (for example when the resource had initial data).
    // A read of GPU-written contents: the wait above retired the work
    // that wrote it, so pull the buffer back into the shadow -- and into
    // the mapped pages, which otherwise keep the last CPU-side bytes.
    // So does a write-only map: its unwritten bytes must stay what the GPU
    // left (measured "exposed words"), and Unmap publishes only the bytes
    // that differ from the shadow -- which must therefore be current.
    bool refreshed = false;
    if (map_waits_for_gpu && map_type != D3D11_MAP_WRITE_DISCARD &&
        buffer->second.gpu_newer && buffer->second.backend) {
        if (gpu_synchronized &&
            buffer->second.backend->read(0, buffer->second.shadow.data(),
                                         buffer->second.shadow.size())) {
            buffer->second.gpu_newer = false;
            ++buffer->second.shadow_version;
            refreshed = true;
        } else {
            d3d11_diag("ID3D11DeviceContext::Map buffer=0x%llx: GPU "
                       "contents could not be read back; the CPU copy "
                       "is stale",
                       static_cast<unsigned long long>(resource));
        }
    }
    const bool expose_previous_contents =
        (allocated_mapping || refreshed ||
         buffer->second.mapped_pages_version !=
             buffer->second.shadow_version) &&
        map_type != D3D11_MAP_WRITE_DISCARD;
    // D3D11_MAPPED_SUBRESOURCE for a buffer: Learn calls both pitches the
    // "physical size (in bytes) of the data". Measured on Windows for
    // every usage and bind class, x86 and x64
    // (d3d11_buffer_map_pitch_probe.expected): RowPitch == DepthPitch ==
    // ByteWidth rounded up to 4 (6 -> 8, 1001 -> 1004). Callers check
    // RowPitch >= the bytes they write; UE's D3D11 RHI asserts on it.
    const u32 buffer_pitch =
        (buffer->second.desc.byte_width + 3u) & ~3u;
    if (expose_previous_contents &&
        m_memory.write_bytes(
            buffer->second.mapped_address,
            buffer->second.shadow.data(),
            buffer->second.shadow.size()).is_err()) {
        return d3d11::E_INVALIDARG;
    }
    if (expose_previous_contents) {
        buffer->second.mapped_pages_version = buffer->second.shadow_version;
    }
    data = buffer->second.mapped_address;
    pitch = buffer_pitch;
    // Measured (a06 probe M7): a Map of a buffer that is already mapped
    // succeeds with the same pointer. A DISCARD stays in force for the
    // one Unmap that publishes both.
    const bool was_discard =
        buffer->second.mapped &&
        buffer->second.mapped_type == D3D11_MAP_WRITE_DISCARD;
    buffer->second.mapped = true;
    buffer->second.mapped_type =
        was_discard ? D3D11_MAP_WRITE_DISCARD : map_type;
    d3d11_diag(
        "ID3D11DeviceContext::Map buffer=0x%llx "
        "type=%u -> guest=0x%llx bytes=%u",
        static_cast<unsigned long long>(resource), map_type,
        static_cast<unsigned long long>(
            buffer->second.mapped_address),
        buffer->second.desc.byte_width);
    return d3d11::S_OK;
}

void D3D11Manager::note_uav_buffer_writes(GuestAddr context, bool compute) {
    const auto state = m_context_states.find(context);
    if (state == m_context_states.end()) return;
    const auto note = [&](GuestAddr view_address) {
        if (view_address == 0) return;
        const auto view = m_unordered_access_views.find(view_address);
        if (view == m_unordered_access_views.end() || view->second.buffer == 0) {
            return;
        }
        const auto buffer = m_buffers.find(view->second.buffer);
        if (buffer == m_buffers.end() ||
            buffer->second.desc.usage != D3D11_USAGE_DEFAULT ||
            buffer->second.desc.cpu_access_flags == 0) {
            return;
        }
        buffer->second.gpu_newer = true;
        note_gpu_use(buffer->second);
    };
    if (compute) {
        for (const GuestAddr view :
             state->second.compute_unordered_access_views) {
            note(view);
        }
    } else {
        for (const GuestAddr view :
             state->second.output_merger_unordered_access_views) {
            note(view);
        }
    }
}

// ID3D11DeviceContext::Unmap of a buffer (see map_buffer).
void D3D11Manager::unmap_buffer(GuestAddr resource, u32 subresource) {
    auto buffer = m_buffers.find(resource);
    if (buffer == m_buffers.end() || subresource != 0 ||
        !buffer->second.mapped) {
        d3d11_diag(
            "ID3D11DeviceContext::Unmap invalid "
            "resource=0x%llx subresource=%u",
            static_cast<unsigned long long>(resource), subresource);
        return;
    }
    static const bool timing_enabled =
        environment_flag_enabled("VITREO_D3D11_UNMAP_TIMING");
    const auto timing_start = timing_enabled ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    const u32 mapped_type = buffer->second.mapped_type;
    const usize byte_width = buffer->second.shadow.size();
    const u8* mapped = m_memory.host_ptr(
        buffer->second.mapped_address, byte_width);
    bool read = mapped != nullptr;
    if (!read) {
        // A protection split may prevent a contiguous host view even
        // though the complete guest range remains readable.
        buffer->second.map_readback.resize(byte_width);
        read = m_memory.read_bytes(
            buffer->second.mapped_address,
            buffer->second.map_readback.data(), byte_width).is_ok();
        if (read) mapped = buffer->second.map_readback.data();
    }
    bool updated = read && buffer->second.backend != nullptr;
    const bool discard =
        buffer->second.mapped_type == D3D11_MAP_WRITE_DISCARD;
    // A mappable DEFAULT buffer mapped READ_WRITE / WRITE: Map did not wait
    // for the draws and dispatches still READING it (only for its writers),
    // so its new bytes go in ordered like UpdateSubresource's -- the backend
    // renames, keeping every other byte, or queues a copy -- never in place
    // under that work. One span, first to last changed byte.
    const bool ordered_default =
        buffer->second.desc.usage == D3D11_USAGE_DEFAULT &&
        buffer->second.mapped_type != D3D11_MAP_WRITE_NO_OVERWRITE;
    if (updated && discard) {
        updated = buffer->second.backend->update(
            0, mapped, byte_width, graphics::BufferWrite::Discard);
    } else if (updated && ordered_default) {
        usize first = 0;
        while (first < byte_width &&
               buffer->second.shadow[first] == mapped[first]) {
            ++first;
        }
        usize last = byte_width;
        while (last > first && buffer->second.shadow[last - 1] == mapped[last - 1]) {
            --last;
        }
        if (first < last) {
            updated = buffer->second.backend->update(
                first, mapped + first, last - first,
                graphics::BufferWrite::Ordered);
            if (updated) {
                std::memcpy(buffer->second.shadow.data() + first,
                            mapped + first, last - first);
            }
        }
    } else if (updated) {
        // WRITE_NO_OVERWRITE goes in place by the application's promise; a
        // STAGING buffer's WRITE / READ_WRITE (Map waited for its copies) is
        // ordered like any plain write.
        const graphics::BufferWrite mode =
            mapped_type == D3D11_MAP_WRITE_NO_OVERWRITE
                ? graphics::BufferWrite::NoOverwrite
                : graphics::BufferWrite::Ordered;
        static const bool wide_compare =
            std::getenv("VITREO_D3D11_WIDE_COMPARE") == nullptr ||
            environment_flag_enabled("VITREO_D3D11_WIDE_COMPARE");
        usize offset = 0;
        while (offset < buffer->second.shadow.size()) {
            // NO_OVERWRITE often changes only a small part of a large
            // buffer. Skip equal spans with the platform's vectorized
            // comparison, then retain exact byte-granular dirty ranges:
            // uploading unchanged bytes could touch in-flight GPU data.
            for (const usize compare_span :
                 {wide_compare ? usize{65536} : usize{4096}, usize{256}}) {
                while (buffer->second.shadow.size() - offset >= compare_span &&
                       std::memcmp(buffer->second.shadow.data() + offset,
                                   mapped + offset, compare_span) == 0) {
                    offset += compare_span;
                }
            }
            while (offset < buffer->second.shadow.size() &&
                   buffer->second.shadow[offset] ==
                      mapped[offset]) {
                ++offset;
            }
            const usize first = offset;
            while (offset < buffer->second.shadow.size() &&
                   buffer->second.shadow[offset] !=
                      mapped[offset]) {
                ++offset;
            }
            if (first != offset) {
                updated = buffer->second.backend->update(
                    first, mapped + first, offset - first, mode);
                std::memcpy(buffer->second.shadow.data() + first,
                            mapped + first, offset - first);
                if (!updated) break;
            }
        }
    }
    if (read && (!updated || discard)) {
        std::memcpy(buffer->second.shadow.data(), mapped, byte_width);
    }
    if (read) {
        ++buffer->second.shadow_version;
        buffer->second.mapped_pages_version = buffer->second.shadow_version;
    }
    buffer->second.mapped = false;
    buffer->second.mapped_type = 0;
    const char* unmap_status = "failed";
    if (buffer->second.desc.usage == D3D11_USAGE_STAGING) {
        unmap_status = read ? "ok" : "failed";
    } else if (updated) {
        unmap_status = "updated";
    }
    d3d11_diag(
        "ID3D11DeviceContext::Unmap buffer=0x%llx bytes=%u -> %s",
        static_cast<unsigned long long>(resource),
        buffer->second.desc.byte_width, unmap_status);
    if (timing_enabled) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - timing_start).count();
        static std::atomic<u32> reports{0};
        if (elapsed >= 100 && reports.fetch_add(1, std::memory_order_relaxed) < 64) {
            std::fprintf(stderr,
                "[d3d11-unmap-time] pid=%d type=%u bytes=%zu us=%lld status=%s\n",
                static_cast<int>(::getpid()), mapped_type, byte_width,
                static_cast<long long>(elapsed), unmap_status);
        }
    }
}

} // namespace vitreo::winapi
