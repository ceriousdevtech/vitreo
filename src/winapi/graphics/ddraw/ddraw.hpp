#pragma once

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/ddraw/d3d7_state.hpp"

#include <array>
#include <unordered_map>
#include <memory>
#include <functional>
#include <vector>

namespace vitreo { class GuestMemory; }
namespace vitreo::graphics {
class IndexedColorTable;
class CpuSurface;
class PresentationState;
class ISurfacePresenter;
}

namespace vitreo::winapi {

class Marshaler;

namespace ddraw {
constexpr u32 DD_OK = 0;
constexpr u32 E_NOINTERFACE = 0x80004002u;
// ddraw.h defines DDERR_UNSUPPORTED as E_NOTIMPL and DDERR_INVALIDPARAMS as
// E_INVALIDARG, so these two spellings are the same HRESULT a guest sees.
constexpr u32 E_NOTIMPL = 0x80004001u;
constexpr u32 E_INVALIDARG = 0x80070057u;
constexpr u32 DDERR_INVALIDPARAMS = E_INVALIDARG;
constexpr u32 CLASS_E_NOAGGREGATION = 0x80040110u;

// DirectDraw-specific codes are MAKE_DDHRESULT(n) == 0x88760000 | n.
constexpr u32 DDERR_INVALIDDIRECTDRAWGUID = 0x88760231u;  // MAKE_DDHRESULT(561)
constexpr u32 DDERR_INVALIDOBJECT = 0x88760082u;          // MAKE_DDHRESULT(130)
constexpr u32 DDERR_SURFACEBUSY = 0x887601AEu;            // MAKE_DDHRESULT(430)
constexpr u32 DDERR_NOTFLIPPABLE = 0x88760246u;           // MAKE_DDHRESULT(582)
constexpr u32 DDERR_NOTFOUND = 0x887601A8u;               // MAKE_DDHRESULT(424)
constexpr u32 DDERR_ALREADYINITIALIZED = 0x88760005u;     // MAKE_DDHRESULT(5)
constexpr u32 DDERR_INVALIDCLIPLIST = 0x8876006Eu;        // MAKE_DDHRESULT(110)
constexpr u32 DDERR_CLIPPERISUSINGHWND = 0x8876009Au;     // MAKE_DDHRESULT(154)
constexpr u32 DDERR_NOCLIPLIST = 0x887600CDu;             // MAKE_DDHRESULT(205)
constexpr u32 DDERR_NOHWND = 0x88760239u;                 // MAKE_DDHRESULT(569)
constexpr u32 DDERR_REGIONTOOSMALL = 0x88760247u;         // MAKE_DDHRESULT(583)

// Direct3D codes are MAKE_D3DHRESULT(n) == 0x88760000 | n, the same facility
// DirectDraw uses, so a caller testing FAILED() cannot tell them apart.
constexpr u32 D3DERR_SCENE_IN_SCENE = 0x88760307u;      // MAKE_D3DHRESULT(775)
constexpr u32 D3DERR_SCENE_NOT_IN_SCENE = 0x88760308u;  // MAKE_D3DHRESULT(776)
constexpr u32 D3DERR_ZBUFFER_NOTPRESENT = 0x8876030Cu;  // MAKE_D3DHRESULT(780)
constexpr u32 D3DERR_STENCILBUFFER_NOTPRESENT =
    0x8876030Du;                                        // MAKE_D3DHRESULT(781)

constexpr u32 DDSCL_FULLSCREEN = 0x00000001;
constexpr u32 DDSCL_ALLOWREBOOT = 0x00000002;
constexpr u32 DDSCL_NORMAL = 0x00000008;
constexpr u32 DDSCL_EXCLUSIVE = 0x00000010;
} // namespace ddraw

class DirectDrawManager {
public:
    explicit DirectDrawManager(graphics::PresentationState& presentation)
        : m_presentation(presentation) {}

    void set_memory(GuestMemory* memory, bool is_64bit = false) noexcept {
        m_memory = memory; m_is_64bit = is_64bit;
    }
    void set_thunk_registry(ThunkRegistry* registry) noexcept { m_thunks = registry; }
    void set_surface_presenter(graphics::ISurfacePresenter* presenter) noexcept {
        m_surface_presenter = presenter;
    }

    /// Callback invoked when the guest enters/leaves an exclusive full-screen
    /// cooperative level, so the host windowing layer can size the target
    /// window to the display. Keeps ddraw decoupled from Cocoa. Args:
    /// (hwnd, enable).
    using FullscreenFn = std::function<void(u32, bool)>;
    void set_fullscreen_callback(FullscreenFn cb) { m_fullscreen_cb = std::move(cb); }

    /// Reports the host display's size, used as the current display mode when
    /// the guest has not called SetDisplayMode. Args: (out_width, out_height).
    using DisplaySizeFn = std::function<void(u32&, u32&)>;
    void set_display_size_callback(DisplaySizeFn cb) {
        m_display_size_cb = std::move(cb);
    }

    /// Notifies user32/gdi when SetDisplayMode changes the logical exclusive
    /// resolution (or when exclusive mode is left). Args:
    /// (hwnd, width, height, bits_per_pixel); width/height 0 restores physical.
    using DisplayModeFn = std::function<void(u32, u32, u32, u32)>;
    void set_display_mode_callback(DisplayModeFn cb) {
        m_display_mode_cb = std::move(cb);
    }

    /// Invokes a guest function pointer, used for the DirectDraw enumeration
    /// callbacks. Signature mirrors run_guest_callback.
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }

    /// Resolves a window's client area in screen coordinates, which is the
    /// region an HWND-attached IDirectDrawClipper clips to. Returns false when
    /// the handle names no window this process owns. Args:
    /// (hwnd, out_left, out_top, out_right, out_bottom).
    using ClientRegionFn =
        std::function<bool(u32, i32&, i32&, i32&, i32&)>;
    void set_client_region_callback(ClientRegionFn cb) {
        m_client_region_cb = std::move(cb);
    }

    [[nodiscard]] u32 enum_display_modes(GuestAddr object, u32 flags,
                                         GuestAddr descriptor, GuestAddr context,
                                         GuestAddr callback);
    [[nodiscard]] u32 get_display_mode(GuestAddr object, GuestAddr descriptor);
    [[nodiscard]] u32 get_caps(GuestAddr object, GuestAddr driver_caps,
                               GuestAddr hel_caps);
    [[nodiscard]] u32 get_vertical_blank_status(GuestAddr object,
                                                GuestAddr status);
    [[nodiscard]] u32 wait_for_vertical_blank(GuestAddr object, u32 flags,
                                              GuestAddr event);

    /// DirectDrawCreateEx: same object as direct_draw_create but handed back
    /// through the IDirectDraw7 vtable. SimCity 4 asks for IDirectDraw7 by IID
    /// and aborts with "Could not initialize Direct Draw" when refused.
    [[nodiscard]] u32 direct_draw_create_ex(GuestAddr guid, GuestAddr output,
                                            GuestAddr iid, GuestAddr outer);

    // ---- IDirect3D7 ----
    // On Windows IDirect3D7 lives in ddraw.dll and is obtained by
    // QueryInterface on an IDirectDraw7. Direct3D 7 is a fixed-function
    // pipeline, so its device state maps onto the same FFPipelineState the
    // shared MSL generator already consumes for D3D9 fixed-function.
    [[nodiscard]] u32 d3d7_enum_devices(GuestAddr object, GuestAddr callback,
                                        GuestAddr context);
    [[nodiscard]] u32 d3d7_create_device(GuestAddr object, GuestAddr clsid,
                                         GuestAddr surface, GuestAddr out_device);
    [[nodiscard]] u32 d3d7_create_vertex_buffer(GuestAddr object, GuestAddr desc,
                                                GuestAddr out_buffer, u32 flags);
    [[nodiscard]] u32 d3d7_enum_z_buffer_formats(GuestAddr object,
                                                 GuestAddr clsid,
                                                 GuestAddr callback,
                                                 GuestAddr context);
    [[nodiscard]] u32 d3d7_evict_managed_textures(GuestAddr object);

    // ---- IDirect3DDevice7 ----
    // A device is a distinct COM object with its own 49-slot vtable, created
    // by IDirect3D7::CreateDevice against a DirectDraw surface that becomes
    // its render target. Everything it is told is kept in a d3d7::DeviceState
    // so `d3d7::state_to_ff()` can describe the draw to the shared MSL
    // generator; see d3d7_state.hpp for why that is a remap and not a second
    // fixed-function implementation.
    [[nodiscard]] u32 device_query_interface(GuestAddr device, GuestAddr iid,
                                             GuestAddr output);
    [[nodiscard]] u32 device_add_ref(GuestAddr device);
    [[nodiscard]] u32 device_release(GuestAddr device);
    [[nodiscard]] u32 device_get_caps(GuestAddr device, GuestAddr out_desc);
    [[nodiscard]] u32 device_enum_texture_formats(GuestAddr device,
                                                  GuestAddr callback,
                                                  GuestAddr context);
    [[nodiscard]] u32 device_begin_scene(GuestAddr device);
    [[nodiscard]] u32 device_end_scene(GuestAddr device);
    [[nodiscard]] u32 device_get_direct3d(GuestAddr device, GuestAddr out_d3d);
    [[nodiscard]] u32 device_set_render_target(GuestAddr device,
                                               GuestAddr surface, u32 flags);
    [[nodiscard]] u32 device_get_render_target(GuestAddr device,
                                               GuestAddr out_surface);
    [[nodiscard]] u32 device_clear(GuestAddr device, u32 count,
                                   GuestAddr rects, u32 flags, u32 color,
                                   u32 depth_bits, u32 stencil);
    [[nodiscard]] u32 device_set_transform(GuestAddr device, u32 which,
                                           GuestAddr matrix);
    [[nodiscard]] u32 device_get_transform(GuestAddr device, u32 which,
                                           GuestAddr matrix);
    [[nodiscard]] u32 device_multiply_transform(GuestAddr device, u32 which,
                                                GuestAddr matrix);
    [[nodiscard]] u32 device_set_viewport(GuestAddr device, GuestAddr viewport);
    [[nodiscard]] u32 device_get_viewport(GuestAddr device, GuestAddr viewport);
    [[nodiscard]] u32 device_set_material(GuestAddr device, GuestAddr material);
    [[nodiscard]] u32 device_get_material(GuestAddr device, GuestAddr material);
    [[nodiscard]] u32 device_set_light(GuestAddr device, u32 index,
                                       GuestAddr light);
    [[nodiscard]] u32 device_get_light(GuestAddr device, u32 index,
                                       GuestAddr light);
    [[nodiscard]] u32 device_light_enable(GuestAddr device, u32 index,
                                          u32 enable);
    [[nodiscard]] u32 device_get_light_enable(GuestAddr device, u32 index,
                                              GuestAddr out_enable);
    [[nodiscard]] u32 device_set_render_state(GuestAddr device, u32 which,
                                              u32 value);
    [[nodiscard]] u32 device_get_render_state(GuestAddr device, u32 which,
                                              GuestAddr out_value);
    [[nodiscard]] u32 device_set_texture_stage_state(GuestAddr device,
                                                     u32 stage, u32 which,
                                                     u32 value);
    [[nodiscard]] u32 device_get_texture_stage_state(GuestAddr device,
                                                     u32 stage, u32 which,
                                                     GuestAddr out_value);
    [[nodiscard]] u32 device_set_texture(GuestAddr device, u32 stage,
                                         GuestAddr surface);
    [[nodiscard]] u32 device_get_texture(GuestAddr device, u32 stage,
                                         GuestAddr out_surface);
    [[nodiscard]] u32 device_set_clip_status(GuestAddr device, GuestAddr status);
    [[nodiscard]] u32 device_get_clip_status(GuestAddr device, GuestAddr status);
    [[nodiscard]] u32 device_set_clip_plane(GuestAddr device, u32 index,
                                            GuestAddr plane);
    [[nodiscard]] u32 device_get_clip_plane(GuestAddr device, u32 index,
                                            GuestAddr plane);
    [[nodiscard]] u32 device_validate_device(GuestAddr device,
                                             GuestAddr out_passes);
    [[nodiscard]] u32 device_pre_load(GuestAddr device, GuestAddr surface);
    [[nodiscard]] u32 device_get_info(GuestAddr device, u32 info_id,
                                      GuestAddr buffer, u32 size);
    [[nodiscard]] u32 device_draw_primitive(GuestAddr device, u32 primitive_type,
                                            u32 fvf, GuestAddr vertices,
                                            u32 vertex_count, u32 flags);
    [[nodiscard]] u32 device_draw_indexed_primitive(
        GuestAddr device, u32 primitive_type, u32 fvf, GuestAddr vertices,
        u32 vertex_count, GuestAddr indices, u32 index_count, u32 flags);

    // ---- IDirectDrawClipper ----
    // A clipper is an independent COM object: DirectDrawCreateClipper builds
    // one with no owning IDirectDraw, and IDirectDraw::CreateClipper builds
    // one already bound to that object. Either way it holds a clip region —
    // an explicit RGNDATA rect list, or an HWND whose client area supplies the
    // region — that a surface Blt intersects its destination against.
    [[nodiscard]] u32 direct_draw_create_clipper(u32 flags, GuestAddr output,
                                                 GuestAddr outer);
    [[nodiscard]] u32 create_clipper(GuestAddr object, u32 flags,
                                     GuestAddr output, GuestAddr outer);
    [[nodiscard]] u32 clipper_query_interface(GuestAddr clipper, GuestAddr iid,
                                              GuestAddr output);
    [[nodiscard]] u32 clipper_add_ref(GuestAddr clipper);
    [[nodiscard]] u32 clipper_release(GuestAddr clipper);
    [[nodiscard]] u32 clipper_get_clip_list(GuestAddr clipper, GuestAddr rect,
                                            GuestAddr clip_list,
                                            GuestAddr out_size);
    [[nodiscard]] u32 clipper_get_hwnd(GuestAddr clipper, GuestAddr out_hwnd);
    [[nodiscard]] u32 clipper_initialize(GuestAddr clipper, GuestAddr object,
                                         u32 flags);
    [[nodiscard]] u32 clipper_is_clip_list_changed(GuestAddr clipper,
                                                   GuestAddr out_changed);
    [[nodiscard]] u32 clipper_set_clip_list(GuestAddr clipper,
                                            GuestAddr clip_list, u32 flags);
    [[nodiscard]] u32 clipper_set_hwnd(GuestAddr clipper, u32 flags, u32 hwnd);

    [[nodiscard]] u32 surface_set_clipper(GuestAddr surface, GuestAddr clipper);
    [[nodiscard]] u32 surface_get_clipper(GuestAddr surface,
                                          GuestAddr out_clipper);

    // ---- IDirectDraw7-only methods ----
    [[nodiscard]] u32 get_surface_from_dc(GuestAddr object, u32 hdc,
                                          GuestAddr out_surface);
    [[nodiscard]] u32 restore_all_surfaces(GuestAddr object);
    [[nodiscard]] u32 test_cooperative_level(GuestAddr object);
    [[nodiscard]] u32 get_device_identifier(GuestAddr object, GuestAddr out_id,
                                            u32 flags);
    [[nodiscard]] u32 start_mode_test(GuestAddr object, GuestAddr modes,
                                      u32 num_modes, u32 flags);
    [[nodiscard]] u32 evaluate_mode(GuestAddr object, u32 flags,
                                    GuestAddr out_timeout);

    [[nodiscard]] u32 direct_draw_create(GuestAddr guid, GuestAddr output,
                                         GuestAddr outer);
    [[nodiscard]] u32 query_interface(GuestAddr object, GuestAddr iid,
                                      GuestAddr output);
    [[nodiscard]] u32 add_ref(GuestAddr object);
    [[nodiscard]] u32 release(GuestAddr object);
    [[nodiscard]] u32 set_cooperative_level(GuestAddr object, u32 hwnd, u32 flags);
    /// IDirectDraw::RestoreDisplayMode. Returns the display to the mode in
    /// force before SetDisplayMode, which a guest calls while tearing down.
    [[nodiscard]] u32 restore_display_mode(GuestAddr object);
    [[nodiscard]] u32 set_display_mode(GuestAddr object, u32 width, u32 height,
                                       u32 bits_per_pixel);
    [[nodiscard]] u32 create_palette(GuestAddr object, u32 flags,
                                     GuestAddr entries, GuestAddr output,
                                     GuestAddr outer);
    [[nodiscard]] u32 palette_query_interface(GuestAddr palette, GuestAddr iid,
                                              GuestAddr output);
    [[nodiscard]] u32 palette_add_ref(GuestAddr palette);
    [[nodiscard]] u32 palette_release(GuestAddr palette);
    [[nodiscard]] u32 palette_get_caps(GuestAddr palette, GuestAddr caps);
    [[nodiscard]] u32 palette_get_entries(GuestAddr palette, u32 flags,
                                          u32 start, u32 count,
                                          GuestAddr entries);
    [[nodiscard]] u32 palette_set_entries(GuestAddr palette, u32 flags,
                                          u32 start, u32 count,
                                          GuestAddr entries);
    [[nodiscard]] u32 create_surface(GuestAddr object, GuestAddr descriptor,
                                     GuestAddr output, GuestAddr outer);
    [[nodiscard]] u32 surface_query_interface(GuestAddr surface, GuestAddr iid,
                                              GuestAddr output);
    [[nodiscard]] u32 surface_add_ref(GuestAddr surface);
    [[nodiscard]] u32 surface_release(GuestAddr surface);
    [[nodiscard]] u32 surface_set_palette(GuestAddr surface, GuestAddr palette);
    [[nodiscard]] u32 surface_is_lost(GuestAddr surface);
    [[nodiscard]] u32 surface_restore(GuestAddr surface);
    /// IDirectDrawSurface::GetAttachedSurface. Selects an attached surface by
    /// capability bits; only the DDSCAPS_BACKBUFFER link of a flipping chain
    /// is attachable here, which is what a fullscreen guest asks for.
    /// IDirectDrawSurface::GetPixelFormat. Fills a standalone DDPIXELFORMAT.
    [[nodiscard]] u32 surface_get_pixel_format(GuestAddr surface,
                                               GuestAddr format);
    [[nodiscard]] u32 surface_get_attached_surface(GuestAddr surface,
                                                   GuestAddr caps,
                                                   GuestAddr output);
    /// IDirectDrawSurface::Flip. Rotates the chain so the current back buffer
    /// becomes visible, then presents it.
    [[nodiscard]] u32 surface_flip(GuestAddr surface,
                                   GuestAddr override_surface, u32 flags);
    [[nodiscard]] u32 surface_lock(GuestAddr surface, GuestAddr rectangle,
                                   GuestAddr descriptor, u32 flags,
                                   GuestAddr event);
    [[nodiscard]] u32 surface_unlock(GuestAddr surface, GuestAddr pixels);
    [[nodiscard]] u32 surface_get_surface_desc(GuestAddr surface,
                                                GuestAddr descriptor);
    [[nodiscard]] u32 surface_blt(GuestAddr dest, GuestAddr dest_rect,
                                  GuestAddr src, GuestAddr src_rect, u32 flags,
                                  GuestAddr fx);
    [[nodiscard]] u32 surface_blt_fast(GuestAddr dest, u32 dest_x, u32 dest_y,
                                       GuestAddr src, GuestAddr src_rect,
                                       u32 flags);
    [[nodiscard]] u32 surface_set_color_key(GuestAddr surface, u32 flags,
                                            GuestAddr color_key);

    [[nodiscard]] u32 cooperative_flags(GuestAddr object) const;
    [[nodiscard]] u32 display_width(GuestAddr object) const;
    [[nodiscard]] u32 display_height(GuestAddr object) const;
    [[nodiscard]] u32 display_bpp(GuestAddr object) const;

private:
    struct Object {
        u32 ref_count{1};
        u32 hwnd{0};
        u32 cooperative_flags{0};
    };

    struct PaletteObject {
        u32 ref_count{1};
        u32 caps{0};
        std::shared_ptr<graphics::IndexedColorTable> colors;
    };

    struct SurfaceObject {
        u32 ref_count{1};
        u32 caps{0};
        GuestAddr pixels{0};
        u32 pixel_bytes{0};
        /// Page-rounded size passed to map_region / unmap_region.
        u32 mapped_bytes{0};
        bool locked{false};
        GuestAddr locked_ptr{0};
        /// DDCKEY_SRCBLT color key for DDBLT_KEYSRC / DDBLTFAST_SRCCOLORKEY.
        bool has_src_color_key{false};
        u32 src_color_key_low{0};
        u32 src_color_key_high{0};
        /// IDirectDrawClipper attached by SetClipper (0 = none). A Blt whose
        /// destination carries one is confined to that clipper's region.
        GuestAddr clipper{0};
        std::shared_ptr<graphics::CpuSurface> surface;
        /// Complex flipping chain links. A primary created with
        /// DDSCAPS_COMPLEX|DDSCAPS_FLIP owns a ring of back buffers;
        /// `back_buffer` is the next surface in flip order and `front_buffer`
        /// names the primary that owns the chain (0 on the primary itself).
        /// GetAttachedSurface walks the first, Flip rotates along it.
        GuestAddr back_buffer{0};
        GuestAddr front_buffer{0};
    };

    /// An IDirect3DDevice7 instance. `owner` is the IDirectDraw7/IDirect3D7
    /// object it was created from, which GetDirect3D hands back.
    struct DeviceObject {
        u32 ref_count{1};
        GuestAddr owner{0};
        /// Which documented device GUID this was created from, reported back
        /// through GetCaps.
        u32 kind{0};
        d3d7::DeviceState state;
    };

    /// An IDirectDrawClipper. Exactly one of `hwnd` and `clip_list` describes
    /// the region: SetHWnd clears an explicit list and SetClipList is refused
    /// while an HWND is attached, which is the documented exclusivity.
    struct ClipperObject {
        u32 ref_count{1};
        /// IDirectDraw this clipper belongs to; 0 until Initialize for one
        /// built by DirectDrawCreateClipper.
        GuestAddr owner{0};
        u32 hwnd{0};
        bool has_clip_list{false};
        /// Rectangles in the order SetClipList supplied them.
        std::vector<std::array<i32, 4>> clip_list;
        /// Cleared by GetClipList, which is what IsClipListChanged reports
        /// against.
        bool clip_list_changed{false};
    };

    [[nodiscard]] bool ensure_storage();
    [[nodiscard]] ClipperObject* find_clipper(GuestAddr clipper);
    /// Builds the clipper object and writes it to @p output. Shared by the
    /// DirectDrawCreateClipper export and IDirectDraw::CreateClipper.
    [[nodiscard]] u32 new_clipper(GuestAddr owner, u32 flags, GuestAddr output,
                                  GuestAddr outer);
    /// Current clip rectangles in screen coordinates, or false when the
    /// clipper has no region to report.
    [[nodiscard]] bool clipper_region(const ClipperObject& state,
                                      std::vector<std::array<i32, 4>>& out);
    [[nodiscard]] DeviceObject* find_device(GuestAddr device);
    /// Writes the D3DDEVICEDESC7 this implementation can serve into @p out.
    [[nodiscard]] bool write_device_desc(GuestAddr out, u32 device_kind);
    [[nodiscard]] Object* find_object(GuestAddr object);
    [[nodiscard]] const Object* find_object(GuestAddr object) const;
    [[nodiscard]] PaletteObject* find_palette(GuestAddr palette);
    [[nodiscard]] SurfaceObject* find_surface(GuestAddr surface);
    void present_surface(GuestAddr surface, SurfaceObject& state);

    graphics::PresentationState& m_presentation;
    graphics::ISurfacePresenter* m_surface_presenter{nullptr};
    FullscreenFn m_fullscreen_cb;
    DisplaySizeFn m_display_size_cb;
    DisplayModeFn m_display_mode_cb;
    GuestCall m_guest_call;
    ClientRegionFn m_client_region_cb;
    u32 m_fullscreen_hwnd{0};  ///< hwnd currently driven full-screen (0 = none)
    [[nodiscard]] VoidResult write_pointer(GuestAddr address, GuestAddr value);
    bool m_is_64bit{false};
    GuestMemory* m_memory{nullptr};
    ThunkRegistry* m_thunks{nullptr};
    GuestAddr m_region_base{0};
    GuestAddr m_vtable{0};
    GuestAddr m_vtable2{0};  ///< IDirectDraw2
    GuestAddr m_vtable7{0};  ///< IDirectDraw7
    GuestAddr m_d3d7_vtable{0};  ///< IDirect3D7, reached via QueryInterface
    GuestAddr m_d3d7_device_vtable{0};  ///< IDirect3DDevice7
    GuestAddr m_clipper_vtable{0};  ///< IDirectDrawClipper
    GuestAddr m_palette_vtable{0};
    GuestAddr m_surface_vtable{0};
    GuestAddr m_next_object{0};
    std::unordered_map<GuestAddr, Object> m_objects;
    /// Interface aliases: a second COM pointer onto an existing object, keyed
    /// alias -> owner. QueryInterface must hand back a distinct pointer whose
    /// vtable is the requested interface while leaving the original pointer
    /// usable; a D3D7 caller holds IDirectDraw7 and IDirect3D7 at the same
    /// time and calls both. find_object resolves an alias to its owner so the
    /// two pointers share one state.
    std::unordered_map<GuestAddr, GuestAddr> m_interface_aliases;
    /// Returns an IDirect3D7 pointer onto `object`, creating it once.
    [[nodiscard]] GuestAddr d3d7_interface_for(GuestAddr object);
    std::unordered_map<GuestAddr, PaletteObject> m_palettes;
    std::unordered_map<GuestAddr, SurfaceObject> m_surfaces;
    std::unordered_map<GuestAddr, DeviceObject> m_devices;
    std::unordered_map<GuestAddr, ClipperObject> m_clippers;
};

[[nodiscard]] DllExports create_ddraw_thunks(DirectDrawManager& manager);

} // namespace vitreo::winapi
