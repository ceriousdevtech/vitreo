// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_context_constant_buffers.cpp
/// @brief ID3D11DeviceContext constant-buffer setters and getters.
///
/// Slots 7, 16, 22, 62, 66 and 71 (*SetConstantBuffers) and 119-130
/// (ID3D11DeviceContext1 *SetConstantBuffers1 / *GetConstantBuffers1).
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

void D3D11Manager::context_constant_buffer_method(Marshaler& m, usize slot,
                                                  GuestAddr self) {
    switch (slot) {
    // -----------------------------------------------------------------------
    // ID3D11DeviceContext1 constant buffers with an offset window
    // -----------------------------------------------------------------------
    //
    // Slots 119-124 are *SetConstantBuffers1 and 125-130 are *GetConstantBuffers1,
    // from the SDK's ID3D11DeviceContext1 vtable. Learn
    // (ID3D11DeviceContext1::VSSetConstantBuffers1): pFirstConstant and
    // pNumConstants are measured in CONSTANTS of 16 bytes, and each entry in
    // pFirstConstant "must be a multiple of 16", so a slot's byte offset is
    // always a multiple of 256. A null pFirstConstant means the whole buffer,
    // which is what the D3D11.0 entry point always means.
    //
    // A caller binds sub-ranges of one large constant buffer this way when the
    // device exposes ID3D11DeviceContext1, so a guest that gets the interface and
    // finds these unimplemented reads its uniforms from the wrong offset.
    case 119:   // VSSetConstantBuffers1
    case 120:   // HSSetConstantBuffers1
    case 121:   // DSSetConstantBuffers1
    case 122:   // GSSetConstantBuffers1
    case 123:   // PSSetConstantBuffers1
    case 124: { // CSSetConstantBuffers1
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr buffers = m.arg_ptr(3);
        const GuestAddr first_constants = m.arg_ptr(4);
        const GuestAddr num_constants = m.arg_ptr(5);
        auto& state = m_context_states[self];
        std::array<GuestAddr, d3d11::CONSTANT_BUFFER_SLOTS>* bound = nullptr;
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS>* first = nullptr;
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS>* window = nullptr;
        if (slot == 120) {
            bound = &state.hull_constant_buffers;
            first = &state.hull_constant_first;
            window = &state.hull_constant_count;
        } else if (slot == 121) {
            bound = &state.domain_constant_buffers;
            first = &state.domain_constant_first;
            window = &state.domain_constant_count;
        } else if (slot == 119) {
            bound = &state.vertex_constant_buffers;
            first = &state.vertex_constant_first;
            window = &state.vertex_constant_count;
        } else if (slot == 122) {
            bound = &state.geometry_constant_buffers;
            first = &state.geometry_constant_first;
            window = &state.geometry_constant_count;
        } else if (slot == 123) {
            bound = &state.pixel_constant_buffers;
            first = &state.pixel_constant_first;
            window = &state.pixel_constant_count;
        } else {
            bound = &state.compute_constant_buffers;
            first = &state.compute_constant_first;
            window = &state.compute_constant_count;
        }
        // Learn (VSSetConstantBuffers1): each FirstConstant and NumConstants
        // "must be a multiple of 16", NumConstants is "in the range
        // [0..4096]", and "If either pFirstConstant or pNumConstants is NULL,
        // the other parameter must also be NULL". Any violation, in any entry
        // and whatever its buffer, drops the WHOLE call (measured: a bad
        // second entry, or a NULL buffer with first 8, leaves every slot as
        // it was).
        if ((first_constants == 0) != (num_constants == 0)) {
            d3d11_diag("ID3D11DeviceContext1 slot %zu: exactly one of "
                       "pFirstConstant/pNumConstants is NULL -> call dropped",
                       slot);
            m.set_return(0);
            return;
        }
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        const u32 in_range =
            start < bound->size()
                ? std::min<u32>(count, static_cast<u32>(bound->size()) - start)
                : 0u;
        std::array<GuestAddr, d3d11::CONSTANT_BUFFER_SLOTS> next{};
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS> next_first{};
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS> next_count =
            d3d11::kWholeConstantWindows;
        for (u32 i = 0; i < in_range; ++i) {
            if (first_constants == 0) continue; // whole buffer
            const auto first_constant =
                m_memory.read_u32(first_constants + i * sizeof(u32));
            const auto constant_count =
                m_memory.read_u32(num_constants + i * sizeof(u32));
            if (first_constant.is_err() || constant_count.is_err() ||
                (first_constant.value() % 16u) != 0 ||
                (constant_count.value() % 16u) != 0 ||
                constant_count.value() > d3d11::WHOLE_CONSTANT_WINDOW) {
                d3d11_diag("ID3D11DeviceContext1 slot %zu: window %u+%u is not "
                           "a multiple of 16 within 4096 -> call dropped",
                           slot, first_constant.value_or(0u),
                           constant_count.value_or(0u));
                m.set_return(0);
                return;
            }
            next_first[i] = first_constant.value();
            next_count[i] = constant_count.value();
        }
        for (u32 i = 0; i < in_range; ++i) {
            GuestAddr buf = 0;
            if (buffers != 0) {
                (void)read_pointer(buffers + i * ptr_size, buf);
            }
            if (buf != 0) {
                auto found = m_buffers.find(buf);
                if (found == m_buffers.end()) {
                    buf = 0;
                } else {
                    // Learn makes the constant buffer "the intersection of
                    // the actual memory allocation for the buffer and the
                    // window", with reads outside it returning 0: the draw
                    // binds from FirstConstant and the translated shader
                    // bounds every cb# read by the window
                    // (fill_constant_buffer_limits).
                    ++found->second.ref_count;
                }
            }
            next[i] = buf;
        }
        for (u32 i = 0; i < in_range; ++i) {
            GuestAddr& target = (*bound)[start + i];
            const GuestAddr previous = target;
            target = next[i];
            // A NULL buffer reads back as the whole-buffer window (measured:
            // "vs set1(NULL,16,16): - first 0 num 4096").
            (*first)[start + i] = next[i] != 0 ? next_first[i] : 0u;
            (*window)[start + i] = next[i] != 0
                ? next_count[i] : d3d11::WHOLE_CONSTANT_WINDOW;
            if (previous != 0) (void)release_buffer(previous);
        }
        m.set_return(0);
        return;
    }
    case 125:   // VSGetConstantBuffers1
    case 126:   // HSGetConstantBuffers1
    case 127:   // DSGetConstantBuffers1
    case 128:   // GSGetConstantBuffers1
    case 129:   // PSGetConstantBuffers1
    case 130: { // CSGetConstantBuffers1
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr buffers_out = m.arg_ptr(3);
        const GuestAddr first_out = m.arg_ptr(4);
        const GuestAddr num_out = m.arg_ptr(5);
        auto& state = m_context_states[self];
        const std::array<GuestAddr, d3d11::CONSTANT_BUFFER_SLOTS>* bound =
            nullptr;
        const std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS>* first = nullptr;
        const std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS>* window = nullptr;
        if (slot == 126) {
            bound = &state.hull_constant_buffers;
            first = &state.hull_constant_first;
            window = &state.hull_constant_count;
        } else if (slot == 127) {
            bound = &state.domain_constant_buffers;
            first = &state.domain_constant_first;
            window = &state.domain_constant_count;
        } else if (slot == 125) {
            bound = &state.vertex_constant_buffers;
            first = &state.vertex_constant_first;
            window = &state.vertex_constant_count;
        } else if (slot == 128) {
            bound = &state.geometry_constant_buffers;
            first = &state.geometry_constant_first;
            window = &state.geometry_constant_count;
        } else if (slot == 129) {
            bound = &state.pixel_constant_buffers;
            first = &state.pixel_constant_first;
            window = &state.pixel_constant_count;
        } else if (slot == 130) {
            bound = &state.compute_constant_buffers;
            first = &state.compute_constant_first;
            window = &state.compute_constant_count;
        }
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        for (u32 i = 0; i < count; ++i) {
            const usize index = static_cast<usize>(start) + i;
            const GuestAddr value =
                (bound != nullptr && index < bound->size()) ? (*bound)[index]
                                                            : 0;
            if (buffers_out != 0) {
                if (value != 0) (void)retain_object(value);
                (void)m.write_ptr(
                    buffers_out + static_cast<GuestAddr>(i) * ptr_size, value);
            }
            if (first_out != 0) {
                (void)m_memory.write_u32(
                    first_out + i * sizeof(u32),
                    (first != nullptr && index < first->size())
                        ? (*first)[index] : 0u);
            }
            if (num_out != 0) {
                (void)m_memory.write_u32(
                    num_out + i * sizeof(u32),
                    (window != nullptr && index < window->size())
                        ? (*window)[index] : d3d11::WHOLE_CONSTANT_WINDOW);
            }
        }
        m.set_return(0);
        return;
    }
    case 7:     // VSSetConstantBuffers
    case 22:    // GSSetConstantBuffers
    case 62:    // HSSetConstantBuffers
    case 66: {  // DSSetConstantBuffers
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr buffers = m.arg_ptr(3);
        auto& state = m_context_states[self];
        auto& stage_buffers = slot == 22   ? state.geometry_constant_buffers
                              : slot == 62 ? state.hull_constant_buffers
                              : slot == 66 ? state.domain_constant_buffers
                                           : state.vertex_constant_buffers;
        auto& stage_first = slot == 22   ? state.geometry_constant_first
                            : slot == 62 ? state.hull_constant_first
                            : slot == 66 ? state.domain_constant_first
                                         : state.vertex_constant_first;
        auto& stage_count = slot == 22   ? state.geometry_constant_count
                            : slot == 62 ? state.hull_constant_count
                            : slot == 66 ? state.domain_constant_count
                                         : state.vertex_constant_count;
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        for (u32 i = 0; i < count && start + i < stage_buffers.size();
             ++i) {
            GuestAddr buf = 0;
            if (buffers != 0) {
                (void)read_pointer(buffers + i * ptr_size, buf);
            }
            if (buf != 0) {
                auto next = m_buffers.find(buf);
                if (next == m_buffers.end()) buf = 0;
                else ++next->second.ref_count;
            }
            // D3D11.0 binds the whole buffer, so this resets any window a
            // previous *SetConstantBuffers1 left on the slot.
            stage_first[start + i] = 0;
            stage_count[start + i] = d3d11::WHOLE_CONSTANT_WINDOW;
            GuestAddr& bound = stage_buffers[start + i];
            const GuestAddr previous = bound;
            bound = buf;
            if (previous != 0) (void)release_buffer(previous);
        }
        m.set_return(0);
        return;
    }
    case 16: {
        // PSSetConstantBuffers
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr buffers = m.arg_ptr(3);
        auto& state = m_context_states[self];
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        for (u32 i = 0; i < count && start + i < state.pixel_constant_buffers.size();
             ++i) {
            GuestAddr buf = 0;
            if (buffers != 0) {
                (void)read_pointer(buffers + i * ptr_size, buf);
            }
            if (buf != 0) {
                auto next = m_buffers.find(buf);
                if (next == m_buffers.end()) buf = 0;
                else ++next->second.ref_count;
            }
            // D3D11.0 binds the whole buffer, so this resets any window a
            // previous *SetConstantBuffers1 left on the slot.
            state.pixel_constant_first[start + i] = 0;
            state.pixel_constant_count[start + i] = d3d11::WHOLE_CONSTANT_WINDOW;
            const GuestAddr previous = state.pixel_constant_buffers[start + i];
            state.pixel_constant_buffers[start + i] = buf;
            if (previous != 0) (void)release_buffer(previous);
        }
        m.set_return(0);
        return;
    }
    case 71: {
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr buffers = m.arg_ptr(3);
        auto& state = m_context_states[self];
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        for (u32 i = 0; i < count && start + i < state.compute_constant_buffers.size(); ++i) {
            GuestAddr next_buffer = 0;
            if (buffers != 0) (void)read_pointer(buffers + i * ptr_size, next_buffer);
            if (next_buffer != 0) {
                auto next = m_buffers.find(next_buffer);
                if (next == m_buffers.end()) next_buffer = 0;
                else ++next->second.ref_count;
            }
            const GuestAddr previous = state.compute_constant_buffers[start + i];
            state.compute_constant_buffers[start + i] = next_buffer;
            // D3D11.0 binds the whole buffer, so this resets any window a
            // previous *SetConstantBuffers1 left on the slot.
            state.compute_constant_first[start + i] = 0;
            state.compute_constant_count[start + i] =
                d3d11::WHOLE_CONSTANT_WINDOW;
            if (previous != 0) (void)release_buffer(previous);
        }
        m.set_return(0);
        return;
    }
    }
}

} // namespace vitreo::winapi
