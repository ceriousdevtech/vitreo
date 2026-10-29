// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d10_forwarding.cpp
/// @brief The per-call D3D10 path: the facet lookups, d3d10_forward and
///        its per-thread scratch arena, the ID3D10Device immediate-context
///        methods, and the buffer and texture facets with Map / Unmap.
///        Kept in one file so the small helpers stay inlinable into their
///        per-draw callers. Split out of d3d10_device.cpp as verbatim
///        code motion.

#include "winapi/graphics/d3d10/d3d10.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d10/d3d10_device_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace vitreo::winapi {
namespace {

/// The getters among them: while off they report an empty state.
[[nodiscard]] constexpr bool d3d10_getter_slot(usize slot) {
    return slot >= 39 && slot <= 62;
}

void write_float_bits(std::array<u8, 24>& record, usize at, float value) {
    std::memcpy(record.data() + at, &value, sizeof(value));
}

} // namespace

// -----------------------------------------------------------------------------
// Facets
// -----------------------------------------------------------------------------

bool D3D11Manager::d3d10_kind_of(GuestAddr owner, D3D10Kind& kind) const {
    if (owner == 0) return false;
    if (m_buffers.contains(owner)) {
        kind = D3D10Kind::Buffer;
    } else if (m_textures.contains(owner)) {
        kind = D3D10Kind::Texture;
    } else if (m_shader_resource_views.contains(owner)) {
        kind = D3D10Kind::ShaderResourceView;
    } else if (m_render_target_views.contains(owner)) {
        kind = D3D10Kind::RenderTargetView;
    } else if (m_depth_stencil_views.contains(owner)) {
        kind = D3D10Kind::DepthStencilView;
    } else if (m_blend_states.contains(owner)) {
        kind = D3D10Kind::BlendState;
    } else if (m_depth_stencil_states.contains(owner)) {
        kind = D3D10Kind::DepthStencilState;
    } else if (m_rasterizer_states.contains(owner)) {
        kind = D3D10Kind::RasterizerState;
    } else if (m_sampler_states.contains(owner)) {
        kind = D3D10Kind::SamplerState;
    } else if (m_input_layouts.contains(owner) ||
               m_vertex_shaders.contains(owner) ||
               m_geometry_shaders.contains(owner) ||
               m_pixel_shaders.contains(owner)) {
        kind = D3D10Kind::DeviceChild;
    } else if (m_queries.contains(owner)) {
        kind = D3D10Kind::Query;
    } else {
        return false;
    }
    return true;
}

GuestAddr D3D11Manager::d3d10_vtable_for(D3D10Kind kind) const {
    switch (kind) {
    case D3D10Kind::Buffer: return m_d3d10_buffer_vtable;
    case D3D10Kind::Texture: return m_d3d10_texture_vtable;
    case D3D10Kind::ShaderResourceView:
        return m_d3d10_shader_resource_view_vtable;
    case D3D10Kind::RenderTargetView: return m_d3d10_render_target_view_vtable;
    case D3D10Kind::DepthStencilView: return m_d3d10_depth_stencil_view_vtable;
    case D3D10Kind::BlendState: return m_d3d10_blend_state_vtable;
    case D3D10Kind::DepthStencilState:
        return m_d3d10_depth_stencil_state_vtable;
    case D3D10Kind::RasterizerState: return m_d3d10_rasterizer_state_vtable;
    case D3D10Kind::SamplerState: return m_d3d10_sampler_state_vtable;
    case D3D10Kind::DeviceChild: return m_d3d10_device_child_vtable;
    case D3D10Kind::Query: return m_d3d10_query_vtable;
    }
    return 0;
}

GuestAddr D3D11Manager::d3d10_facet_of(GuestAddr owner) {
    D3D10Kind kind{};
    if (owner == 0 || !m_d3d10_ready || !d3d10_kind_of(owner, kind)) {
        return 0;
    }
    const GuestAddr facet = owner + kD3D10FacetOffset;
    if (!m_d3d10_facets.contains(facet)) {
        // Created on first use: most D3D11 objects are never asked for one.
        if (!write_pointer(facet, d3d10_vtable_for(kind))) return 0;
        m_d3d10_facets.emplace(facet, owner);
    }
    return facet;
}

GuestAddr D3D11Manager::d3d10_owner_of(GuestAddr iface) const {
    const auto it = m_d3d10_facets.find(iface);
    return it != m_d3d10_facets.end() ? it->second : iface;
}

// -----------------------------------------------------------------------------
// Forwarding
// -----------------------------------------------------------------------------

GuestAddr D3D11Manager::d3d10_scratch(usize bytes) {
    // 64 KiB per guest thread: the largest request is a 64-entry stream-out
    // declaration (1.5 KiB); frames and arrays are far smaller.
    constexpr usize kArenaBytes = 0x10000;
    bytes = std::max<usize>((bytes + 15u) & ~usize{15}, 16u);
    D3D10Scratch& arena = m_d3d10_scratch[std::this_thread::get_id()];
    if (arena.base == 0) {
        arena.base = m_memory.map_first_fit(0x10000000ull, kArenaBytes,
                                            MemoryProtection::ReadWrite);
        if (arena.base == 0) return 0;
    }
    if (bytes > kArenaBytes - arena.used) return 0;
    const GuestAddr address = arena.base + arena.used;
    arena.used += bytes;
    // Callers leave fields they do not set at zero.
    static const std::array<u8, 256> zero{};
    for (usize done = 0; done < bytes; done += zero.size()) {
        (void)m_memory.write_bytes(address + done, zero.data(),
                                   std::min(zero.size(), bytes - done));
    }
    return address;
}

usize D3D11Manager::d3d10_scratch_mark() {
    return m_d3d10_scratch[std::this_thread::get_id()].used;
}

void D3D11Manager::d3d10_scratch_release(usize mark) {
    m_d3d10_scratch[std::this_thread::get_id()].used = mark;
}

u64 D3D11Manager::d3d10_forward(MethodDispatch method, usize slot,
                                GuestAddr self,
                                std::initializer_list<u64> args) {
    // The frame the D3D11 thunk would have seen (Marshaler::arg): on x64
    // `this` and three arguments in RCX, RDX, R8, R9 -- a float in the XMM
    // register of its position -- the rest at RSP + 0x28 + 8(n-4); on x86
    // everything at ESP + 4 + 4n.
    const usize mark = d3d10_scratch_mark();
    const usize frame_bytes =
        m_is_64bit ? 0x28 + args.size() * 8 + 16 : 8 + args.size() * 4 + 16;
    const GuestAddr frame = d3d10_scratch(frame_bytes);
    if (frame == 0) {
        d3d11_diag("D3D10: no scratch memory for a forwarded call (slot %zu)",
                   slot);
        return kEOutOfMemory;
    }
    cpu::CpuState cpu{};
    if (m_is_64bit) {
        cpu.init_long_mode();
        cpu.gpr.rsp = frame;
        cpu.gpr.rcx = self;
        const std::array<u64*, 3> registers{&cpu.gpr.rdx, &cpu.gpr.r8,
                                            &cpu.gpr.r9};
        usize index = 0;
        for (const u64 value : args) {
            if (index < registers.size()) {
                *registers[index] = value;
                cpu.sse.xmm[index + 1].u32_[0] = static_cast<u32>(value);
            } else {
                (void)m_memory.write_u64(frame + 0x28 + (index - 3) * 8,
                                         value);
            }
            ++index;
        }
    } else {
        cpu.init_protected_mode();
        cpu.gpr.rsp = frame;
        (void)m_memory.write_u32(frame + 4, static_cast<u32>(self));
        usize index = 0;
        for (const u64 value : args) {
            (void)m_memory.write_u32(frame + 8 + index * 4,
                                     static_cast<u32>(value));
            ++index;
        }
    }
    Marshaler m(m_memory);
    m.attach_cpu(cpu);
    ++m_d3d10_forwarding;
    (this->*method)(m, slot);
    --m_d3d10_forwarding;
    d3d10_scratch_release(mark);
    return cpu.gpr.rax;
}

GuestAddr D3D11Manager::d3d10_objects_in(GuestAddr array, u32 count) {
    if (array == 0) return 0;
    // Past the largest register space the D3D11 implementation refuses the
    // count before it reads a pointer.
    if (count > d3d11::SHADER_RESOURCE_SLOTS) return array;
    const usize pointer = m_is_64bit ? 8u : 4u;
    const GuestAddr copy = d3d10_scratch(std::max<usize>(count, 1) * pointer);
    if (copy == 0) return array;
    for (u32 i = 0; i < count; ++i) {
        GuestAddr value = 0;
        if (!read_pointer(array + i * pointer, value)) return array;
        (void)write_pointer(copy + i * pointer, d3d10_owner_of(value));
    }
    return copy;
}

void D3D11Manager::d3d10_objects_out(GuestAddr array, u32 count) {
    if (array == 0) return;
    const usize pointer = m_is_64bit ? 8u : 4u;
    count = std::min<u32>(count, d3d11::SHADER_RESOURCE_SLOTS);
    for (u32 i = 0; i < count; ++i) {
        GuestAddr value = 0;
        if (!read_pointer(array + i * pointer, value) || value == 0) {
            continue;
        }
        if (const GuestAddr facet = d3d10_facet_of(value)) {
            (void)write_pointer(array + i * pointer, facet);
        }
    }
}

bool D3D11Manager::d3d11_context_slot_disabled_by_d3d10(usize slot) {
    // Learn, CreateDeviceContextState remarks: every ID3D11DeviceContext
    // method from VSSetConstantBuffers (7) to FinishCommandList (114) except
    // Map, Unmap, Begin, End, GetData and GetContextFlags. The D3D11.1
    // methods (115+) are not in its table.
    if (slot < 7 || slot > 114) return false;
    return slot != 14 && slot != 15 && slot != 27 && slot != 28 &&
           slot != 29 && slot != 113;
}

void D3D11Manager::d3d10_run_on_empty_state(
    GuestAddr context, const std::function<void()>& call) {
    ContextState saved = std::move(m_context_states[context]);
    m_context_states[context] = ContextState{};
    ++m_d3d10_forwarding;
    call();
    --m_d3d10_forwarding;
    // A getter of an empty state takes no reference: nothing to release.
    m_context_states[context] = std::move(saved);
}

// -----------------------------------------------------------------------------
// ID3D10Device1: the immediate-context methods
// -----------------------------------------------------------------------------

bool D3D11Manager::d3d10_context_method(Marshaler& m, usize slot,
                                        Device& device) {
    const GuestAddr context = device.context_interface;
    // Off while a D3D11 state is active (measured: a setter changes
    // nothing, a getter reports the empty state).
    if (!device.d3d10_active && !d3d10_getter_slot(slot)) {
        if (slot == 97) {
            if (m.arg_ptr(1) != 0) (void)m_memory.write_u32(m.arg_ptr(1), 0);
            if (m.arg_ptr(2) != 0) (void)m_memory.write_u32(m.arg_ptr(2), 0);
        }
        m.set_return(0);
        return true;
    }
    const usize mark = d3d10_scratch_mark();
    const auto ctx = [&](usize d3d11_slot, std::initializer_list<u64> args) {
        return d3d10_forward(&D3D11Manager::impl_ContextMethod, d3d11_slot,
                             context, args);
    };
    const auto a = [&](usize index) { return m.arg(index); };
    const auto obj = [&](usize index) -> u64 {
        return d3d10_owner_of(m.arg_ptr(index));
    };
    const auto objects = [&](usize array, usize count) -> u64 {
        return d3d10_objects_in(m.arg_ptr(array), m.arg32(count));
    };
    const auto run = [&] {
        switch (slot) {
        // -- Setters ------------------------------------------------------
        case 3: ctx(7, {a(1), a(2), objects(3, 2)}); break;
        case 4: ctx(8, {a(1), a(2), objects(3, 2)}); break;
        case 5: ctx(9, {obj(1), 0, 0}); break;
        case 6: ctx(10, {a(1), a(2), objects(3, 2)}); break;
        case 7: ctx(11, {obj(1), 0, 0}); break;
        case 8: ctx(12, {a(1), a(2), a(3)}); break;
        case 9: ctx(13, {a(1), a(2)}); break;
        case 10: ctx(16, {a(1), a(2), objects(3, 2)}); break;
        case 11: ctx(17, {obj(1)}); break;
        case 12: ctx(18, {a(1), a(2), objects(3, 2), a(4), a(5)}); break;
        case 13: ctx(19, {obj(1), a(2), a(3)}); break;
        case 14: ctx(20, {a(1), a(2), a(3), a(4), a(5)}); break;
        case 15: ctx(21, {a(1), a(2), a(3), a(4)}); break;
        case 16: ctx(22, {a(1), a(2), objects(3, 2)}); break;
        case 17: ctx(23, {obj(1), 0, 0}); break;
        case 18: ctx(24, {a(1)}); break;
        case 19: ctx(25, {a(1), a(2), objects(3, 2)}); break;
        case 20: ctx(26, {a(1), a(2), objects(3, 2)}); break;
        case 21: ctx(30, {obj(1), a(2)}); break;
        case 22: ctx(31, {a(1), a(2), objects(3, 2)}); break;
        case 23: ctx(32, {a(1), a(2), objects(3, 2)}); break;
        case 24: ctx(33, {a(1), objects(2, 1), obj(3)}); break;
        case 25: ctx(35, {obj(1), a(2), a(3)}); break;
        case 26: ctx(36, {obj(1), a(2)}); break;
        case 27: {
            // SOSetTargets(NumBuffers, ppSOTargets, pOffsets).
            const u32 count = m.arg32(1);
            ctx(37, {a(1), objects(2, 1), a(3)});
            if (count <= 4) {
                m_context_states[context].d3d10_stream_out_count = count;
            }
            break;
        }
        case 28: ctx(38, {}); break;
        case 29: ctx(43, {obj(1)}); break;
        case 30: {
            // RSSetViewports(NumViewports, const D3D10_VIEWPORT*): integer
            // origin and size, float depth -- to D3D11's six floats.
            const u32 count = m.arg32(1);
            const GuestAddr in = m.arg_ptr(2);
            GuestAddr converted = in;
            if (in != 0 && count <= 16) {
                converted = d3d10_scratch(std::max<u32>(count, 1) * 24u);
                for (u32 i = 0; i < count && converted != 0; ++i) {
                    std::array<u8, 24> record{};
                    if (m_memory.read_bytes(in + i * 24u, record.data(), 24)
                            .is_err()) {
                        break;
                    }
                    i32 x = 0, y = 0;
                    u32 w = 0, h = 0;
                    std::memcpy(&x, record.data() + 0, 4);
                    std::memcpy(&y, record.data() + 4, 4);
                    std::memcpy(&w, record.data() + 8, 4);
                    std::memcpy(&h, record.data() + 12, 4);
                    write_float_bits(record, 0, static_cast<float>(x));
                    write_float_bits(record, 4, static_cast<float>(y));
                    write_float_bits(record, 8, static_cast<float>(w));
                    write_float_bits(record, 12, static_cast<float>(h));
                    (void)m_memory.write_bytes(converted + i * 24u,
                                               record.data(), 24);
                }
                if (converted == 0) converted = in;
            }
            ctx(44, {a(1), converted});
            break;
        }
        case 31: ctx(45, {a(1), a(2)}); break;
        case 32:
            ctx(46, {obj(1), a(2), a(3), a(4), a(5), obj(6), a(7), a(8)});
            break;
        case 33: ctx(47, {obj(1), obj(2)}); break;
        case 34: ctx(48, {obj(1), a(2), a(3), a(4), a(5), a(6)}); break;
        case 35: ctx(50, {obj(1), a(2)}); break;
        case 36: {
            // ClearDepthStencilView(pDSV, ClearFlags, Depth, Stencil): the
            // float travels in its own register / stack slot.
            const float depth = m.arg_f32(3);
            u32 bits = 0;
            std::memcpy(&bits, &depth, sizeof(bits));
            ctx(53, {obj(1), a(2), bits, a(4)});
            break;
        }
        case 37: ctx(54, {obj(1)}); break;
        case 38: ctx(57, {obj(1), a(2), obj(3), a(4), a(5)}); break;
        // -- Getters: the D3D11 answer, objects turned into facets ---------
        case 39: ctx(72, {a(1), a(2), a(3)});
            d3d10_objects_out(m.arg_ptr(3), m.arg32(2)); break;
        case 40: ctx(73, {a(1), a(2), a(3)});
            d3d10_objects_out(m.arg_ptr(3), m.arg32(2)); break;
        case 41: ctx(74, {a(1), 0, 0});
            d3d10_objects_out(m.arg_ptr(1), 1); break;
        case 42: ctx(75, {a(1), a(2), a(3)});
            d3d10_objects_out(m.arg_ptr(3), m.arg32(2)); break;
        case 43: ctx(76, {a(1), 0, 0});
            d3d10_objects_out(m.arg_ptr(1), 1); break;
        case 44: ctx(77, {a(1), a(2), a(3)});
            d3d10_objects_out(m.arg_ptr(3), m.arg32(2)); break;
        case 45: ctx(78, {a(1)});
            d3d10_objects_out(m.arg_ptr(1), 1); break;
        case 46: ctx(79, {a(1), a(2), a(3), a(4), a(5)});
            d3d10_objects_out(m.arg_ptr(3), m.arg32(2)); break;
        case 47: ctx(80, {a(1), a(2), a(3)});
            d3d10_objects_out(m.arg_ptr(1), 1); break;
        case 48: ctx(81, {a(1), a(2), a(3)});
            d3d10_objects_out(m.arg_ptr(3), m.arg32(2)); break;
        case 49: ctx(82, {a(1), 0, 0});
            d3d10_objects_out(m.arg_ptr(1), 1); break;
        case 50: ctx(83, {a(1)}); break;
        case 51: ctx(84, {a(1), a(2), a(3)});
            d3d10_objects_out(m.arg_ptr(3), m.arg32(2)); break;
        case 52: ctx(85, {a(1), a(2), a(3)});
            d3d10_objects_out(m.arg_ptr(3), m.arg32(2)); break;
        case 53: ctx(86, {a(1), a(2)});
            d3d10_objects_out(m.arg_ptr(1), 1); break;
        case 54: ctx(87, {a(1), a(2), a(3)});
            d3d10_objects_out(m.arg_ptr(3), m.arg32(2)); break;
        case 55: ctx(88, {a(1), a(2), a(3)});
            d3d10_objects_out(m.arg_ptr(3), m.arg32(2)); break;
        case 56: ctx(89, {a(1), a(2), a(3)});
            d3d10_objects_out(m.arg_ptr(2), m.arg32(1));
            d3d10_objects_out(m.arg_ptr(3), 1); break;
        case 57: ctx(91, {a(1), a(2), a(3)});
            d3d10_objects_out(m.arg_ptr(1), 1); break;
        case 58: ctx(92, {a(1), a(2)});
            d3d10_objects_out(m.arg_ptr(1), 1); break;
        case 59: {
            // SOGetTargets(NumBuffers, ppSOTargets, pOffsets). D3D11 keeps
            // no offsets; D3D10's answer is 0xFFFFFFFF for each slot the
            // last SOSetTargets covered and 0 beyond (measured).
            const u32 count = m.arg32(1);
            ctx(93, {a(1), a(2)});
            d3d10_objects_out(m.arg_ptr(2), count);
            const GuestAddr offsets = m.arg_ptr(3);
            if (offsets != 0 && count <= 4) {
                const u32 set = m_context_states[context]
                                    .d3d10_stream_out_count;
                for (u32 i = 0; i < count; ++i) {
                    (void)m_memory.write_u32(offsets + i * 4u,
                                             i < set ? 0xFFFFFFFFu : 0u);
                }
            }
            break;
        }
        case 60: ctx(94, {a(1)});
            d3d10_objects_out(m.arg_ptr(1), 1); break;
        case 61: {
            // RSGetViewports(pNumViewports, D3D10_VIEWPORT*): D3D11's floats
            // back to integers.
            const GuestAddr count_address = m.arg_ptr(1);
            const GuestAddr out = m.arg_ptr(2);
            const auto requested = count_address != 0
                                       ? m_memory.read_u32(count_address)
                                       : VResult<u32>::ok(0);
            if (out == 0 || requested.is_err()) {
                ctx(95, {a(1), a(2)});
                break;
            }
            const u32 wanted = std::min<u32>(requested.value(), 16u);
            const GuestAddr floats =
                d3d10_scratch(std::max<u32>(wanted, 1) * 24u);
            if (floats == 0) break;
            ctx(95, {a(1), floats});
            for (u32 i = 0; i < wanted; ++i) {
                std::array<u8, 24> record{};
                if (m_memory.read_bytes(floats + i * 24u, record.data(), 24)
                        .is_err()) {
                    break;
                }
                float value[4]{};
                std::memcpy(value, record.data(), sizeof(value));
                const i32 x = static_cast<i32>(value[0]);
                const i32 y = static_cast<i32>(value[1]);
                const u32 w = static_cast<u32>(std::max(value[2], 0.0f));
                const u32 h = static_cast<u32>(std::max(value[3], 0.0f));
                std::memcpy(record.data() + 0, &x, 4);
                std::memcpy(record.data() + 4, &y, 4);
                std::memcpy(record.data() + 8, &w, 4);
                std::memcpy(record.data() + 12, &h, 4);
                (void)m_memory.write_bytes(out + i * 24u, record.data(), 24);
            }
            break;
        }
        case 62: ctx(96, {a(1), a(2)}); break;
        // -- The rest ------------------------------------------------------
        case 69: ctx(110, {}); break;  // ClearState
        case 70: ctx(111, {}); break;  // Flush
        case 96:
            // SetTextFilterSize: the runtime keeps nothing (measured:
            // GetTextFilterSize reads 0x0 after setting 7x3).
            break;
        case 97:
            // GetTextFilterSize(pWidth, pHeight): 0 x 0 (measured).
            if (m.arg_ptr(1) != 0) (void)m_memory.write_u32(m.arg_ptr(1), 0);
            if (m.arg_ptr(2) != 0) (void)m_memory.write_u32(m.arg_ptr(2), 0);
            break;
        default:
            break;
        }
    };
    if (device.d3d10_active) {
        run();
    } else {
        d3d10_run_on_empty_state(context, run);
    }
    d3d10_scratch_release(mark);
    m.set_return(0);
    return true;
}

// -----------------------------------------------------------------------------
// The device-child facets
// -----------------------------------------------------------------------------

void D3D11Manager::d3d10_child_method(Marshaler& m, usize slot,
                                      GuestAddr owner, MethodDispatch method) {
    switch (slot) {
    case 0:
        // The object's own QueryInterface, which answers the D3D10 IIDs
        // with this facet: one identity, one count.
        m.set_return(d3d10_forward(method, 0, owner, {m.arg(1), m.arg(2)}));
        return;
    case 1:
        m.set_return(retain_object(owner));
        return;
    case 2:
        m.set_return(release_object(owner));
        return;
    case 3: {
        // GetDevice(ID3D10Device**): the device's D3D10 facet with a
        // reference; NULL while the device does not answer ID3D10Device
        // (measured, a D3D11 device before CreateDeviceContextState).
        const GuestAddr out = m.arg_ptr(1);
        const GuestAddr facet = d3d10_device_facet(device_owner_of(owner));
        if (out != 0) {
            if (facet != 0) (void)add_ref(facet);
            (void)m.write_ptr(out, facet);
        }
        m.set_return(0);
        return;
    }
    case 4:  // GetPrivateData: one store with the D3D11 object (measured).
    case 5:  // SetPrivateData
        m.set_return(
            d3d10_forward(method, slot, owner, {m.arg(1), m.arg(2), m.arg(3)}));
        return;
    case 6:  // SetPrivateDataInterface
        m.set_return(d3d10_forward(method, slot, owner, {m.arg(1), m.arg(2)}));
        return;
    default:
        m.set_return(0x80004001u);
        return;
    }
}

void D3D11Manager::d3d10_map(Marshaler& m, GuestAddr owner, bool texture) {
    // ID3D10Buffer::Map(MapType, MapFlags, void** ppData);
    // ID3D10Texture1D::Map(Subresource, MapType, MapFlags, void** ppData);
    // ID3D10Texture2D::Map(..., D3D10_MAPPED_TEXTURE2D* {pData, RowPitch});
    // ID3D10Texture3D::Map(..., D3D10_MAPPED_TEXTURE3D* {pData, RowPitch,
    // DepthPitch}). All are the immediate context's Map of the resource.
    const u32 subresource = texture ? m.arg32(1) : 0u;
    const usize first = texture ? 2u : 1u;
    const u32 map_type = m.arg32(first);
    const u32 map_flags = m.arg32(first + 1);
    const GuestAddr out = m.arg_ptr(first + 2);
    const auto device = m_devices.find(device_owner_of(owner));
    if (device == m_devices.end() || out == 0) {
        m.set_return(kEInvalidArg);
        return;
    }
    const usize mark = d3d10_scratch_mark();
    const usize pointer = m_is_64bit ? 8u : 4u;
    const GuestAddr mapped = d3d10_scratch(pointer + 8u);
    if (mapped == 0) {
        m.set_return(kEOutOfMemory);
        return;
    }
    const u32 hr = static_cast<u32>(d3d10_forward(
        &D3D11Manager::impl_ContextMethod, 14,
        device->second.context_interface,
        {owner, subresource, map_type, map_flags, mapped}));
    GuestAddr data = 0;
    (void)read_pointer(mapped, data);
    const auto row = m_memory.read_u32(mapped + pointer);
    const auto depth = m_memory.read_u32(mapped + pointer + 4);
    enum class Shape { Pointer, Texture2D, Texture3D } shape = Shape::Pointer;
    if (texture) {
        const Texture2D& t = m_textures.at(owner);
        shape = t.linear   ? Shape::Pointer
                : t.volume ? Shape::Texture3D
                           : Shape::Texture2D;
    }
    // On failure the pointer reads NULL (measured).
    (void)write_pointer(out, hr == kSOk ? data : 0);
    if (shape != Shape::Pointer) {
        (void)m_memory.write_u32(out + pointer,
                                 hr == kSOk && row.is_ok() ? row.value() : 0);
    }
    if (shape == Shape::Texture3D) {
        (void)m_memory.write_u32(
            out + pointer + 4,
            hr == kSOk && depth.is_ok() ? depth.value() : 0);
    }
    d3d10_scratch_release(mark);
    m.set_return(hr);
}

void D3D11Manager::impl_D3D10BufferMethod(Marshaler& m, usize slot) {
    const GuestAddr owner = d3d10_owner_of(m.arg_ptr(0));
    if (!m_buffers.contains(owner) || owner == m.arg_ptr(0)) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    const MethodDispatch method = &D3D11Manager::impl_BufferMethod;
    switch (slot) {
    case 7:  // GetType
    case 9:  // GetEvictionPriority
        m.set_return(d3d10_forward(method, slot, owner, {m.arg(1)}));
        return;
    case 8:  // SetEvictionPriority
        m.set_return(d3d10_forward(method, slot, owner, {m.arg(1)}));
        return;
    case 10:  // Map(MapType, MapFlags, ppData)
        d3d10_map(m, owner, false);
        return;
    case 11: {  // Unmap()
        const auto device = m_devices.find(m_buffers.at(owner).device_owner);
        if (device != m_devices.end()) {
            (void)d3d10_forward(&D3D11Manager::impl_ContextMethod, 15,
                                device->second.context_interface, {owner, 0});
        }
        m.set_return(0);
        return;
    }
    case 12: {  // GetDesc(D3D10_BUFFER_DESC*): D3D11's, no stride.
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(0);
            return;
        }
        const usize mark = d3d10_scratch_mark();
        const GuestAddr desc = d3d10_scratch(24);
        if (desc != 0) {
            (void)d3d10_forward(method, 10, owner, {desc});
            std::array<u32, 6> words{};
            if (m_memory.read_bytes(desc, reinterpret_cast<u8*>(words.data()),
                                    sizeof(words)).is_ok()) {
                words[4] = misc_to_d3d10(words[4]);
                (void)m_memory.write_bytes(
                    out, reinterpret_cast<const u8*>(words.data()), 20);
            }
        }
        d3d10_scratch_release(mark);
        m.set_return(0);
        return;
    }
    default:
        d3d10_child_method(m, slot, owner, method);
        return;
    }
}

void D3D11Manager::impl_D3D10TextureMethod(Marshaler& m, usize slot) {
    const GuestAddr owner = d3d10_owner_of(m.arg_ptr(0));
    const auto texture = m_textures.find(owner);
    if (texture == m_textures.end() || owner == m.arg_ptr(0)) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    const MethodDispatch method = &D3D11Manager::impl_Texture2DMethod;
    switch (slot) {
    case 7:
    case 8:
    case 9:
        m.set_return(d3d10_forward(method, slot, owner, {m.arg(1)}));
        return;
    case 10:  // Map(Subresource, MapType, MapFlags, out)
        d3d10_map(m, owner, true);
        return;
    case 11: {  // Unmap(Subresource)
        const auto device = m_devices.find(texture->second.device_owner);
        if (device != m_devices.end()) {
            (void)d3d10_forward(&D3D11Manager::impl_ContextMethod, 15,
                                device->second.context_interface,
                                {owner, m.arg(1)});
        }
        m.set_return(0);
        return;
    }
    case 12: {
        // GetDesc: D3D10_TEXTURE{1,2,3}D_DESC are D3D11's layouts; only the
        // misc flags are renumbered (their word is the last).
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(0);
            return;
        }
        const usize bytes = texture->second.linear   ? 32u
                            : texture->second.volume ? 36u
                                                     : 44u;
        const usize mark = d3d10_scratch_mark();
        const GuestAddr desc = d3d10_scratch(bytes);
        if (desc != 0) {
            (void)d3d10_forward(method, 10, owner, {desc});
            std::vector<u8> words(bytes);
            if (m_memory.read_bytes(desc, words.data(), bytes).is_ok()) {
                u32 misc = 0;
                std::memcpy(&misc, words.data() + bytes - 4, 4);
                misc = misc_to_d3d10(misc);
                std::memcpy(words.data() + bytes - 4, &misc, 4);
                (void)m_memory.write_bytes(out, words.data(), bytes);
            }
        }
        d3d10_scratch_release(mark);
        m.set_return(0);
        return;
    }
    default:
        d3d10_child_method(m, slot, owner, method);
        return;
    }
}

} // namespace vitreo::winapi
