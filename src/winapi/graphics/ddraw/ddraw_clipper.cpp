// SPDX-License-Identifier: LGPL-2.1-only

/// @file ddraw_clipper.cpp
/// @brief IDirectDrawClipper: creation, lifetime, clip list and window region.

#include "winapi/graphics/ddraw/ddraw.hpp"
#include "winapi/graphics/ddraw/ddraw_internal.hpp"
#include "core/memory/guest_memory.hpp"

#include <array>
#include <utility>
#include <vector>

namespace vitreo::winapi {

using namespace ddraw_internal;

// =============================================================================
// IDirectDrawClipper
// =============================================================================
//
// A clipper carries the region a Blt is confined to. It is described either by
// an explicit RGNDATA rectangle list (SetClipList) or by a window whose client
// area supplies the region (SetHWnd); the two are mutually exclusive, which is
// why SetClipList is refused while an HWND is attached.

DirectDrawManager::ClipperObject* DirectDrawManager::find_clipper(
    GuestAddr clipper) {
    auto it = m_clippers.find(clipper);
    return it == m_clippers.end() ? nullptr : &it->second;
}

u32 DirectDrawManager::new_clipper(GuestAddr owner, u32 flags,
                                   GuestAddr output, GuestAddr outer) {
    if (!m_memory || output == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(output, 0);
    if (outer != 0) return ddraw::CLASS_E_NOAGGREGATION;
    // dwFlags is documented as reserved and must be zero.
    if (flags != 0) return ddraw::DDERR_INVALIDPARAMS;
    if (!ensure_storage()) return 0x8007000Eu;

    const GuestAddr clipper = m_next_object;
    m_next_object += 0x20;
    if (m_memory->write_u32(clipper,
                            static_cast<u32>(m_clipper_vtable)).is_err() ||
        m_memory->write_u32(output, static_cast<u32>(clipper)).is_err()) {
        return 0x8007000Eu;
    }
    ClipperObject state;
    state.owner = owner;
    m_clippers.emplace(clipper, std::move(state));
    if (owner != 0) (void)add_ref(owner);
    VITREO_TRACE("[ddraw] clipper created owner=0x%llx -> clipper=0x%llx\n",
                 static_cast<unsigned long long>(owner),
                 static_cast<unsigned long long>(clipper));
    return ddraw::DD_OK;
}

u32 DirectDrawManager::direct_draw_create_clipper(u32 flags, GuestAddr output,
                                                  GuestAddr outer) {
    // The standalone export builds a clipper with no owning IDirectDraw; the
    // caller associates one later with IDirectDrawClipper::Initialize.
    return new_clipper(0, flags, output, outer);
}

u32 DirectDrawManager::create_clipper(GuestAddr object, u32 flags,
                                      GuestAddr output, GuestAddr outer) {
    if (!m_memory || output == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(output, 0);
    if (!find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    // A clipper from IDirectDraw::CreateClipper is already associated, so
    // Initialize on it is DDERR_ALREADYINITIALIZED.
    return new_clipper(object, flags, output, outer);
}

u32 DirectDrawManager::clipper_query_interface(GuestAddr clipper,
                                               GuestAddr iid,
                                               GuestAddr output) {
    if (!m_memory || output == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(output, 0);
    if (!find_clipper(clipper)) return ddraw::DDERR_INVALIDOBJECT;
    Guid requested{};
    if (iid == 0 || m_memory->read_bytes(
            iid, reinterpret_cast<Byte*>(&requested), sizeof(requested)).is_err()) {
        return ddraw::E_INVALIDARG;
    }
    if (!guid_equal(requested, IID_IUNKNOWN) &&
        !guid_equal(requested, IID_IDIRECTDRAWCLIPPER)) {
        return ddraw::E_NOINTERFACE;
    }
    // IDirectDrawClipper was never versioned, so IUnknown and the clipper IID
    // are the same vtable.
    (void)m_memory->write_u32(output, static_cast<u32>(clipper));
    return clipper_add_ref(clipper);
}

u32 DirectDrawManager::clipper_add_ref(GuestAddr clipper) {
    ClipperObject* state = find_clipper(clipper);
    if (!state) return 0;
    return ++state->ref_count;
}

u32 DirectDrawManager::clipper_release(GuestAddr clipper) {
    ClipperObject* state = find_clipper(clipper);
    if (!state) return 0;
    if (state->ref_count > 1) return --state->ref_count;
    const GuestAddr owner = state->owner;
    m_clippers.erase(clipper);
    // A surface still naming this clipper must stop clipping against freed
    // state; DirectDraw expects the caller to detach first, but a dangling
    // reference here would silently clip the wrong region.
    for (auto& entry : m_surfaces) {
        if (entry.second.clipper == clipper) entry.second.clipper = 0;
    }
    if (owner != 0) (void)release(owner);
    return 0;
}

u32 DirectDrawManager::clipper_initialize(GuestAddr clipper, GuestAddr object,
                                          u32 flags) {
    ClipperObject* state = find_clipper(clipper);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (flags != 0) return ddraw::DDERR_INVALIDPARAMS;
    if (state->owner != 0) return ddraw::DDERR_ALREADYINITIALIZED;
    // A null IDirectDraw leaves the clipper unowned, which is legal: it can be
    // attached to a surface from any DirectDraw object.
    if (object != 0) {
        if (!find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
        state->owner = object;
        (void)add_ref(object);
    }
    return ddraw::DD_OK;
}

u32 DirectDrawManager::clipper_set_hwnd(GuestAddr clipper, u32 flags,
                                        u32 hwnd) {
    ClipperObject* state = find_clipper(clipper);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (flags != 0) return ddraw::DDERR_INVALIDPARAMS;
    if (hwnd != 0) {
        // Attaching to a window this process does not own would leave the
        // clipper with a region it can never resolve.
        i32 left = 0;
        i32 top = 0;
        i32 right = 0;
        i32 bottom = 0;
        if (!m_client_region_cb ||
            !m_client_region_cb(hwnd, left, top, right, bottom)) {
            VITREO_TRACE("[ddraw] Clipper::SetHWnd hwnd=0x%x unresolvable\n",
                         hwnd);
            return ddraw::DDERR_INVALIDPARAMS;
        }
    }
    // Attaching a window discards any explicit list; the two describe the
    // region exclusively.
    state->hwnd = hwnd;
    state->has_clip_list = false;
    state->clip_list.clear();
    state->clip_list_changed = true;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::clipper_get_hwnd(GuestAddr clipper,
                                        GuestAddr out_hwnd) {
    ClipperObject* state = find_clipper(clipper);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (out_hwnd == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(out_hwnd, state->hwnd);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::clipper_set_clip_list(GuestAddr clipper,
                                             GuestAddr clip_list, u32 flags) {
    ClipperObject* state = find_clipper(clipper);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (flags != 0) return ddraw::DDERR_INVALIDPARAMS;
    if (state->hwnd != 0) return ddraw::DDERR_CLIPPERISUSINGHWND;
    if (clip_list == 0) {
        // A null list deletes the clip list.
        state->has_clip_list = false;
        state->clip_list.clear();
        state->clip_list_changed = true;
        return ddraw::DD_OK;
    }

    // RGNDATAHEADER { DWORD dwSize, iType, nCount, nRgnSize; RECT rcBound }
    // followed by nCount RECTs.
    constexpr u32 RGNDATAHEADER_SIZE = 32;
    constexpr u32 RDH_RECTANGLES = 1;
    const u32 header_size = m_memory->read_u32(clip_list).value_or(0);
    const u32 type = m_memory->read_u32(clip_list + 4).value_or(0);
    const u32 count = m_memory->read_u32(clip_list + 8).value_or(0);
    if (header_size != RGNDATAHEADER_SIZE || type != RDH_RECTANGLES) {
        return ddraw::DDERR_INVALIDCLIPLIST;
    }

    std::vector<std::array<i32, 4>> rects;
    rects.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const GuestAddr entry = clip_list + RGNDATAHEADER_SIZE + i * 16;
        const i32 left = static_cast<i32>(m_memory->read_u32(entry).value_or(0));
        const i32 top =
            static_cast<i32>(m_memory->read_u32(entry + 4).value_or(0));
        const i32 right =
            static_cast<i32>(m_memory->read_u32(entry + 8).value_or(0));
        const i32 bottom =
            static_cast<i32>(m_memory->read_u32(entry + 12).value_or(0));
        // An empty or inverted rectangle is not a valid region member.
        if (right <= left || bottom <= top) return ddraw::DDERR_INVALIDCLIPLIST;
        rects.push_back({left, top, right, bottom});
    }
    state->clip_list = std::move(rects);
    state->has_clip_list = true;
    state->clip_list_changed = true;
    return ddraw::DD_OK;
}

bool DirectDrawManager::clipper_region(const ClipperObject& state,
                                       std::vector<std::array<i32, 4>>& out) {
    out.clear();
    if (state.hwnd != 0) {
        // Vitreo composites each guest top-level window into its own host
        // view, so within the guest's own coordinate space the window is not
        // occluded by another guest window — the host compositor resolves
        // overlap after the blit. The client rectangle is therefore the whole
        // visible region this layer can describe.
        i32 left = 0;
        i32 top = 0;
        i32 right = 0;
        i32 bottom = 0;
        if (!m_client_region_cb ||
            !m_client_region_cb(state.hwnd, left, top, right, bottom)) {
            return false;
        }
        if (right <= left || bottom <= top) return false;
        out.push_back({left, top, right, bottom});
        return true;
    }
    if (!state.has_clip_list) return false;
    out = state.clip_list;
    return true;
}

u32 DirectDrawManager::clipper_get_clip_list(GuestAddr clipper, GuestAddr rect,
                                             GuestAddr clip_list,
                                             GuestAddr out_size) {
    ClipperObject* state = find_clipper(clipper);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (out_size == 0) return ddraw::E_INVALIDARG;

    std::vector<std::array<i32, 4>> rects;
    if (!clipper_region(*state, rects)) return ddraw::DDERR_NOCLIPLIST;

    // An optional bounding rectangle restricts the returned region to the
    // part of it the caller cares about.
    if (rect != 0) {
        const i32 clip_left =
            static_cast<i32>(m_memory->read_u32(rect).value_or(0));
        const i32 clip_top =
            static_cast<i32>(m_memory->read_u32(rect + 4).value_or(0));
        const i32 clip_right =
            static_cast<i32>(m_memory->read_u32(rect + 8).value_or(0));
        const i32 clip_bottom =
            static_cast<i32>(m_memory->read_u32(rect + 12).value_or(0));
        std::vector<std::array<i32, 4>> intersected;
        for (const auto& r : rects) {
            const i32 left = r[0] > clip_left ? r[0] : clip_left;
            const i32 top = r[1] > clip_top ? r[1] : clip_top;
            const i32 right = r[2] < clip_right ? r[2] : clip_right;
            const i32 bottom = r[3] < clip_bottom ? r[3] : clip_bottom;
            if (right > left && bottom > top) {
                intersected.push_back({left, top, right, bottom});
            }
        }
        rects = std::move(intersected);
    }

    constexpr u32 RGNDATAHEADER_SIZE = 32;
    constexpr u32 RDH_RECTANGLES = 1;
    const u32 rect_bytes = static_cast<u32>(rects.size()) * 16;
    const u32 required = RGNDATAHEADER_SIZE + rect_bytes;
    if (clip_list == 0) {
        // The size-query form: report how large a buffer the caller needs.
        (void)m_memory->write_u32(out_size, required);
        return ddraw::DD_OK;
    }
    const u32 available = m_memory->read_u32(out_size).value_or(0);
    if (available < required) return ddraw::DDERR_REGIONTOOSMALL;

    i32 bound_left = 0;
    i32 bound_top = 0;
    i32 bound_right = 0;
    i32 bound_bottom = 0;
    for (usize i = 0; i < rects.size(); ++i) {
        if (i == 0) {
            bound_left = rects[i][0];
            bound_top = rects[i][1];
            bound_right = rects[i][2];
            bound_bottom = rects[i][3];
            continue;
        }
        if (rects[i][0] < bound_left) bound_left = rects[i][0];
        if (rects[i][1] < bound_top) bound_top = rects[i][1];
        if (rects[i][2] > bound_right) bound_right = rects[i][2];
        if (rects[i][3] > bound_bottom) bound_bottom = rects[i][3];
    }
    (void)m_memory->write_u32(clip_list, RGNDATAHEADER_SIZE);
    (void)m_memory->write_u32(clip_list + 4, RDH_RECTANGLES);
    (void)m_memory->write_u32(clip_list + 8, static_cast<u32>(rects.size()));
    (void)m_memory->write_u32(clip_list + 12, rect_bytes);
    (void)m_memory->write_u32(clip_list + 16, static_cast<u32>(bound_left));
    (void)m_memory->write_u32(clip_list + 20, static_cast<u32>(bound_top));
    (void)m_memory->write_u32(clip_list + 24, static_cast<u32>(bound_right));
    (void)m_memory->write_u32(clip_list + 28, static_cast<u32>(bound_bottom));
    for (usize i = 0; i < rects.size(); ++i) {
        const GuestAddr entry =
            clip_list + RGNDATAHEADER_SIZE + static_cast<GuestAddr>(i) * 16;
        (void)m_memory->write_u32(entry, static_cast<u32>(rects[i][0]));
        (void)m_memory->write_u32(entry + 4, static_cast<u32>(rects[i][1]));
        (void)m_memory->write_u32(entry + 8, static_cast<u32>(rects[i][2]));
        (void)m_memory->write_u32(entry + 12, static_cast<u32>(rects[i][3]));
    }
    (void)m_memory->write_u32(out_size, required);
    // The change flag reports movement since the last time the list was read.
    state->clip_list_changed = false;
    return ddraw::DD_OK;
}

u32 DirectDrawManager::clipper_is_clip_list_changed(GuestAddr clipper,
                                                    GuestAddr out_changed) {
    ClipperObject* state = find_clipper(clipper);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (out_changed == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(out_changed, state->clip_list_changed ? 1u : 0u);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::surface_set_clipper(GuestAddr surface,
                                           GuestAddr clipper) {
    SurfaceObject* state = find_surface(surface);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    // A null clipper detaches whatever was attached.
    if (clipper != 0 && find_clipper(clipper) == nullptr) {
        return ddraw::DDERR_INVALIDOBJECT;
    }
    const GuestAddr previous = state->clipper;
    if (previous == clipper) return ddraw::DD_OK;
    if (clipper != 0) (void)clipper_add_ref(clipper);
    state->clipper = clipper;
    if (previous != 0) (void)clipper_release(previous);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::surface_get_clipper(GuestAddr surface,
                                           GuestAddr out_clipper) {
    SurfaceObject* state = find_surface(surface);
    if (!m_memory || !state) return ddraw::DDERR_INVALIDOBJECT;
    if (out_clipper == 0) return ddraw::E_INVALIDARG;
    // MAKE_DDHRESULT(182) — the surface has no clipper attached.
    constexpr u32 DDERR_NOCLIPPERATTACHED = 0x887600B6u;
    if (state->clipper == 0) return DDERR_NOCLIPPERATTACHED;
    (void)m_memory->write_u32(out_clipper, static_cast<u32>(state->clipper));
    (void)clipper_add_ref(state->clipper);
    return ddraw::DD_OK;
}

} // namespace vitreo::winapi
