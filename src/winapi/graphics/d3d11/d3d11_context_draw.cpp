// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_context_draw.cpp
/// @brief ID3D11DeviceContext draws and dispatches, and their census.
///
/// Slots 12, 13, 20, 21 and 38-42.
/// impl_ContextMethod (d3d11_context.cpp) routes exactly these slots here.
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

namespace {

void note_draw(const char* api, u32 count, u32 instances, u64 index_bytes) {
    // A draw census for a [d3d11-draw] line every 5 s: seven strcmp and a
    // clock read on every draw. With the WP-R1 cuts on (com_direct) it is a
    // VITREO_D3D11_CENSUS diagnostic like the per-slot census.
    if (com_direct::active() && !d3d11_census_enabled()) return;
    struct Slot {
        const char* api;
        std::atomic<u64> zero{0};
        std::atomic<u64> live{0};
        std::atomic<u64> zero_with_indices{0};
    };
    static Slot slots[] = {
        {"DrawIndexed"}, {"Draw"}, {"DrawIndexedInstanced"}, {"DrawInstanced"},
        {"DrawIndexedInstancedIndirect"}, {"DrawInstancedIndirect"},
        {"Dispatch"},
    };
    Slot* slot = nullptr;
    for (Slot& candidate : slots) {
        if (std::strcmp(candidate.api, api) == 0) slot = &candidate;
    }
    if (slot == nullptr) return;
    if (count == 0 || instances == 0) {
        slot->zero.fetch_add(1, std::memory_order_relaxed);
        if (index_bytes != 0) {
            slot->zero_with_indices.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        slot->live.fetch_add(1, std::memory_order_relaxed);
    }
    static auto last = std::chrono::steady_clock::now();
    static std::atomic<int> printing{0};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) return;
    if (printing.exchange(1) != 0) return;
    last = now;
    for (Slot& candidate : slots) {
        const u64 zero = candidate.zero.exchange(0);
        const u64 live = candidate.live.exchange(0);
        const u64 with_ib = candidate.zero_with_indices.exchange(0);
        if (zero == 0 && live == 0) continue;
        std::fprintf(stderr,
                     "[d3d11-draw] %s live=%llu zero=%llu zero_but_index_buffer=%llu\n",
                     candidate.api, static_cast<unsigned long long>(live),
                     static_cast<unsigned long long>(zero),
                     static_cast<unsigned long long>(with_ib));
    }
    printing.store(0);
}

} // namespace

void D3D11Manager::context_draw_method(Marshaler& m, usize slot,
                                       GuestAddr self) {
    switch (slot) {
    case 12: {
        // DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation).
        d3d11_diag(
            "ID3D11DeviceContext::DrawIndexed indices=%u start_index=%u "
            "base_vertex=%d",
            static_cast<u32>(m.arg(1)), static_cast<u32>(m.arg(2)),
            static_cast<i32>(m.arg(3)));
        if (static_cast<u32>(m.arg(1)) == 0 &&
            d3d11_skip_stats_enabled()) {
            static std::atomic<u32> seen{0};
            const u32 n = seen.fetch_add(1);
            if (n >= 30000 && n < 30040) {
                const auto& st = m_context_states[self];
                u64 ib_size = 0;
                if (auto b = m_buffers.find(st.index_buffer); b != m_buffers.end()) {
                    ib_size = b->second.desc.byte_width;
                }
                std::fprintf(stderr,
                             "[d3d11-zero-site] ret=0x%llx start=%u base=%d ib=0x%llx "
                             "ib_bytes=%llu ib_fmt=%u ib_off=%u vs=0x%llx\n",
                             static_cast<unsigned long long>(m.guest_return_address()),
                             static_cast<u32>(m.arg(2)), static_cast<i32>(m.arg(3)),
                             static_cast<unsigned long long>(st.index_buffer),
                             static_cast<unsigned long long>(ib_size),
                             st.index_buffer_format, st.index_buffer_offset,
                             static_cast<unsigned long long>(st.vertex_shader));
            }
        }
        note_draw("DrawIndexed", static_cast<u32>(m.arg(1)), 1, 0);
        m_draw_may_release_lock = true;
        execute_draw(self, true, static_cast<u32>(m.arg(1)),
                     static_cast<u32>(m.arg(2)),
                     static_cast<i32>(m.arg(3)), 1, 0);
        m.set_return(0);
        return;
    }
    case 13: {
        // Draw(VertexCount, StartVertexLocation).
        d3d11_diag("ID3D11DeviceContext::Draw vertices=%u start_vertex=%u",
                   static_cast<u32>(m.arg(1)), static_cast<u32>(m.arg(2)));
        note_draw("Draw", static_cast<u32>(m.arg(1)), 1, 0);
        m_draw_may_release_lock = true;
        execute_draw(self, false, static_cast<u32>(m.arg(1)),
                     static_cast<u32>(m.arg(2)), 0, 1, 0);
        m.set_return(0);
        return;
    }
    case 20: {
        // DrawIndexedInstanced(IndexCountPerInstance, InstanceCount,
        // StartIndexLocation, BaseVertexLocation, StartInstanceLocation).
        d3d11_diag(
            "ID3D11DeviceContext::DrawIndexedInstanced indices=%u "
            "instances=%u start_index=%u base_vertex=%d first_instance=%u",
            static_cast<u32>(m.arg(1)), static_cast<u32>(m.arg(2)),
            static_cast<u32>(m.arg(3)), static_cast<i32>(m.arg(4)),
            static_cast<u32>(m.arg(5)));
        if (static_cast<u32>(m.arg(1)) == 0 &&
            d3d11_skip_stats_enabled()) {
            static std::atomic<u32> seen{0};
            const u32 n = seen.fetch_add(1);
            {
                const auto& st = m_context_states[self];
                static std::mutex dump_mu;
                static std::set<GuestAddr> dumped;
                std::lock_guard<std::mutex> dl(dump_mu);
                auto dump = [&](GuestAddr addr, const std::vector<u8>& code,
                                const char* kind) {
                    if (addr == 0 || !dumped.insert(addr).second) return;
                    char path[160];
                    std::snprintf(path, sizeof(path), "/tmp/vitreo-dxbc/%llx.%s.dxbc",
                                  static_cast<unsigned long long>(addr), kind);
                    if (FILE* f = std::fopen(path, "wb")) {
                        std::fwrite(code.data(), 1, code.size(), f);
                        std::fclose(f);
                    }
                };
                if (auto v = m_vertex_shaders.find(st.vertex_shader);
                    v != m_vertex_shaders.end()) {
                    dump(st.vertex_shader, v->second.bytecode, "vs");
                }
                if (auto p = m_pixel_shaders.find(st.pixel_shader);
                    p != m_pixel_shaders.end()) {
                    dump(st.pixel_shader, p->second.bytecode, "ps");
                }
            }
            if (n < 16) {
                const auto& st = m_context_states[self];
                u64 ib_size = 0;
                if (auto b = m_buffers.find(st.index_buffer); b != m_buffers.end()) {
                    ib_size = b->second.desc.byte_width;
                }
                u32 range[8]{};
                u32 sub[22]{};
                u64 sub_ptr = 0;
                if (m.has_cpu() && m.cpu()->gpr.r14 != 0) {
                    for (u32 i = 0; i < 8; ++i) {
                        const auto w = m_memory.read_u32(m.cpu()->gpr.r14 + i * 4u);
                        if (w.is_ok()) range[i] = w.value();
                    }
                    // The call has pushed the return address. The caller stored a
                    // 0x58-byte record at 0xf8(%rsp) before the call.
                    const auto saved = m_memory.read_u64(m.cpu()->gpr.rsp + 0x100);
                    if (saved.is_ok()) {
                        sub_ptr = saved.value();
                        for (u32 i = 0; i < 22; ++i) {
                            const auto w = m_memory.read_u32(sub_ptr + i * 4u);
                            if (w.is_ok()) sub[i] = w.value();
                        }
                    }
                }
                std::fprintf(stderr,
                             "[d3d11-zeroi-site] ret=0x%llx inst=%u start=%u base=%d "
                             "first=%u ib=0x%llx ib_bytes=%llu ib_fmt=%u vs=0x%llx "
                             "range=%u,%u,%u,%u,%u,%u,%u,%u sub=0x%llx "
                             "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                             static_cast<unsigned long long>(m.guest_return_address()),
                             static_cast<u32>(m.arg(2)), static_cast<u32>(m.arg(3)),
                             static_cast<i32>(m.arg(4)), static_cast<u32>(m.arg(5)),
                             static_cast<unsigned long long>(st.index_buffer),
                             static_cast<unsigned long long>(ib_size),
                             st.index_buffer_format,
                             static_cast<unsigned long long>(st.vertex_shader),
                             range[0], range[1], range[2], range[3],
                             range[4], range[5], range[6], range[7],
                             static_cast<unsigned long long>(sub_ptr),
                             sub[0], sub[1], sub[2], sub[3], sub[4], sub[5],
                             sub[6], sub[7], sub[8], sub[9], sub[10], sub[11]);
            }
        }
        if (static_cast<u32>(m.arg(1)) != 0 &&
            d3d11_skip_stats_enabled()) {
            const auto& st = m_context_states[self];
            static std::atomic<u32> color_seen{0};
            static std::atomic<u64> null_rt{0};
            static std::atomic<u64> color_rt{0};
            if (st.render_targets[0] == 0) {
                null_rt.fetch_add(1);
            } else {
                color_rt.fetch_add(1);
                const u32 n = color_seen.fetch_add(1);
                if (n < 8) {
                    u64 ib_size = 0;
                    if (auto b = m_buffers.find(st.index_buffer); b != m_buffers.end()) {
                        ib_size = b->second.desc.byte_width;
                    }
                    std::fprintf(stderr,
                                 "[d3d11-livei] count=%u inst=%u start=%u base=%d "
                                 "first=%u ib_bytes=%llu rt=0x%llx dsv=0x%llx vs=0x%llx\n",
                                 static_cast<u32>(m.arg(1)), static_cast<u32>(m.arg(2)),
                                 static_cast<u32>(m.arg(3)), static_cast<i32>(m.arg(4)),
                                 static_cast<u32>(m.arg(5)),
                                 static_cast<unsigned long long>(ib_size),
                                 static_cast<unsigned long long>(st.render_targets[0]),
                                 static_cast<unsigned long long>(st.depth_stencil_view),
                                 static_cast<unsigned long long>(st.vertex_shader));
                }
            }
            const u64 nrt = null_rt.load();
            const u64 crt = color_rt.load();
            if ((nrt + crt) % 100000 == 0) {
                std::fprintf(stderr, "[d3d11-livei-rt] null=%llu color=%llu\n",
                             static_cast<unsigned long long>(nrt),
                             static_cast<unsigned long long>(crt));
            }
        }
        note_draw("DrawIndexedInstanced", static_cast<u32>(m.arg(1)),
                  static_cast<u32>(m.arg(2)), 0);
        m_draw_may_release_lock = true;
        execute_draw(self, true, static_cast<u32>(m.arg(1)),
                     static_cast<u32>(m.arg(3)),
                     static_cast<i32>(m.arg(4)),
                     static_cast<u32>(m.arg(2)),
                     static_cast<u32>(m.arg(5)));
        m.set_return(0);
        return;
    }
    case 21: {
        // DrawInstanced(VertexCountPerInstance, InstanceCount,
        // StartVertexLocation, StartInstanceLocation).
        d3d11_diag(
            "ID3D11DeviceContext::DrawInstanced vertices=%u instances=%u "
            "start_vertex=%u first_instance=%u",
            static_cast<u32>(m.arg(1)), static_cast<u32>(m.arg(2)),
            static_cast<u32>(m.arg(3)), static_cast<u32>(m.arg(4)));
        note_draw("DrawInstanced", static_cast<u32>(m.arg(1)),
                  static_cast<u32>(m.arg(2)), 0);
        m_draw_may_release_lock = true;
        execute_draw(self, false, static_cast<u32>(m.arg(1)),
                     static_cast<u32>(m.arg(3)), 0,
                     static_cast<u32>(m.arg(2)),
                     static_cast<u32>(m.arg(4)));
        m.set_return(0);
        return;
    }
    case 38:
        // DrawAuto: draws what a previous stream-out wrote, counted from the
        // IA slot-0 buffer's filled size.
        context_draw_auto(self);
        m.set_return(0);
        return;
    case 39: {
        // DrawIndexedInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs).
        //
        // Learn: the buffer must have been created with
        // D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS and holds the same argument
        // list as DrawIndexedInstanced — IndexCountPerInstance, InstanceCount,
        // StartIndexLocation, BaseVertexLocation, StartInstanceLocation
        // (D3D12_DRAW_INDEXED_ARGUMENTS names that layout explicitly). The
        // counts stay on the GPU; nothing here reads them.
        const GuestAddr args = m.arg_ptr(1);
        const u32 offset = static_cast<u32>(m.arg(2));
        d3d11_diag("ID3D11DeviceContext::DrawIndexedInstancedIndirect "
                   "args=0x%llx offset=%u",
                   static_cast<unsigned long long>(args), offset);
        note_draw("DrawIndexedInstancedIndirect", 1, 1, 0);
        m_draw_may_release_lock = true;
        execute_draw(self, true, 0, 0, 0, 1, 0, args, offset);
        m.set_return(0);
        return;
    }
    case 40: {
        // DrawInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs).
        // Argument list of DrawInstanced: VertexCountPerInstance,
        // InstanceCount, StartVertexLocation, StartInstanceLocation.
        const GuestAddr args = m.arg_ptr(1);
        const u32 offset = static_cast<u32>(m.arg(2));
        d3d11_diag("ID3D11DeviceContext::DrawInstancedIndirect "
                   "args=0x%llx offset=%u",
                   static_cast<unsigned long long>(args), offset);
        note_draw("DrawInstancedIndirect", 1, 1, 0);
        m_draw_may_release_lock = true;
        execute_draw(self, false, 0, 0, 0, 1, 0, args, offset);
        m.set_return(0);
        return;
    }
    case 41: {
        note_draw("Dispatch", static_cast<u32>(m.arg(1)),
                  static_cast<u32>(m.arg(2)) == 0 ||
                          static_cast<u32>(m.arg(3)) == 0
                      ? 0u
                      : 1u,
                  0);
        execute_dispatch(self, static_cast<u32>(m.arg(1)),
                         static_cast<u32>(m.arg(2)),
                         static_cast<u32>(m.arg(3)));
        m.set_return(0);
        return;
    }
    case 42: {
        // DispatchIndirect(pBufferForArgs, AlignedByteOffsetForArgs). Learn:
        // the buffer "must be loaded with data that matches the argument list
        // for Dispatch" — three UINT threadgroup counts — and must carry
        // D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS.
        const GuestAddr args = m.arg_ptr(1);
        const u32 offset = static_cast<u32>(m.arg(2));
        d3d11_diag("ID3D11DeviceContext::DispatchIndirect args=0x%llx "
                   "offset=%u",
                   static_cast<unsigned long long>(args), offset);
        execute_dispatch(self, 0, 0, 0, args, offset);
        m.set_return(0);
        return;
    }
    }
}

} // namespace vitreo::winapi
