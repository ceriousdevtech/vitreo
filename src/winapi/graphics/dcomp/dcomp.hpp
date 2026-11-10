// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dcomp.hpp
/// @brief DirectComposition device, target, and visual graph contracts.

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_swap_chain.hpp"
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::graphics {
class GraphicsBackendDevice;
}

#include <array>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

class D3D11Manager;
class Marshaler;

namespace dcomp {

constexpr u32 S_OK = 0;
constexpr u32 E_NOTIMPL = 0x80004001u;
constexpr u32 E_NOINTERFACE = 0x80004002u;
constexpr u32 E_POINTER = 0x80004003u;
constexpr u32 E_INVALIDARG = 0x80070057u;
// dcomp.h facility 0x898. WINDOW_ALREADY_COMPOSED and ACCESS_DENIED are the
// ends of the block; the two surface-draw codes sit between them
// (DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED / _NOT_BEING_RENDERED).
constexpr u32 DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED = 0x88980800u;
constexpr u32 DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED = 0x88980801u;
constexpr u32 DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED = 0x88980802u;
constexpr u32 DCOMPOSITION_ERROR_ACCESS_DENIED = 0x88980803u;
constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
constexpr u32 DXGI_ERROR_INVALID_CALL = 0x887A0001u;

constexpr usize DEVICE_METHOD_COUNT = 27;
constexpr usize DEVICE2_METHOD_COUNT = 24;
constexpr usize DESKTOP_DEVICE_METHOD_COUNT = 27;
constexpr usize DEVICE3_METHOD_COUNT = 37;
constexpr usize TARGET_METHOD_COUNT = 4;
constexpr usize VISUAL2_METHOD_COUNT = 22;
constexpr usize TRANSLATE_TRANSFORM_METHOD_COUNT = 7;
constexpr usize SCALE_TRANSFORM_METHOD_COUNT = 11;
constexpr usize ROTATE_TRANSFORM_METHOD_COUNT = 9;
constexpr usize SKEW_TRANSFORM_METHOD_COUNT = 11;
constexpr usize MATRIX_TRANSFORM_METHOD_COUNT = 6;
constexpr usize RECTANGLE_CLIP_METHOD_COUNT = 27;
// IDCompositionSurface (dcomp.h): IUnknown + BeginDraw, EndDraw,
// SuspendDraw, ResumeDraw, Scroll. IDCompositionVirtualSurface appends
// Resize and Trim. Both objects share this table; Resize and Trim fail
// on a surface that is not virtual.
constexpr usize SURFACE_METHOD_COUNT = 10;
// IDXGISurface1, the update object BeginDraw returns for a DXGI surface
// IID. GetDC/ReleaseDC are present so the vtable is long enough; they fail
// because the surface is not GDI-compatible.
constexpr usize UPDATE_OBJECT_METHOD_COUNT = 13;

} // namespace dcomp

class DcompManager {
public:
    enum class WindowOwnership {
        Invalid,
        CurrentProcess,
        OtherProcess,
    };
    using WindowOwnershipResolver =
        std::function<WindowOwnership(u64 hwnd)>;

    struct MethodThunks {
        std::array<GuestAddr, dcomp::DEVICE_METHOD_COUNT> device{};
        std::array<GuestAddr, dcomp::DEVICE2_METHOD_COUNT> device2{};
        std::array<GuestAddr, dcomp::DESKTOP_DEVICE_METHOD_COUNT>
            desktop_device{};
        std::array<GuestAddr, dcomp::DEVICE3_METHOD_COUNT> device3{};
        std::array<GuestAddr, dcomp::TARGET_METHOD_COUNT> target{};
        std::array<GuestAddr, dcomp::VISUAL2_METHOD_COUNT> visual2{};
        std::array<GuestAddr, dcomp::TRANSLATE_TRANSFORM_METHOD_COUNT>
            translate_transform{};
        std::array<GuestAddr, dcomp::SCALE_TRANSFORM_METHOD_COUNT>
            scale_transform{};
        std::array<GuestAddr, dcomp::ROTATE_TRANSFORM_METHOD_COUNT>
            rotate_transform{};
        std::array<GuestAddr, dcomp::SKEW_TRANSFORM_METHOD_COUNT>
            skew_transform{};
        std::array<GuestAddr, dcomp::MATRIX_TRANSFORM_METHOD_COUNT>
            matrix_transform{};
        std::array<GuestAddr, dcomp::RECTANGLE_CLIP_METHOD_COUNT>
            rectangle_clip{};
        std::array<GuestAddr, dcomp::SURFACE_METHOD_COUNT> surface{};
        std::array<GuestAddr, dcomp::UPDATE_OBJECT_METHOD_COUNT>
            update_object{};
    };

    DcompManager(GuestMemory& memory, bool is_64bit,
                 D3D11Manager& d3d11_manager);
    ~DcompManager();

    void set_method_thunks(MethodThunks thunks);
    void set_window_ownership_resolver(WindowOwnershipResolver resolver);

    void impl_DCompositionCreateDevice(Marshaler& m);
    void impl_DCompositionCreateDevice2(Marshaler& m);
    void impl_DCompositionCreateDevice3(Marshaler& m);
    void impl_DCompositionCreateSurfaceHandle(Marshaler& m);
    void impl_PublicUnsupported(Marshaler& m, const char* name,
                                usize output_arg = static_cast<usize>(-1));

    enum class DeviceInterface {
        Legacy,
        Device2,
        Desktop,
        Device3,
    };
    void impl_DeviceMethod(Marshaler& m, DeviceInterface interface,
                           usize slot);
    void impl_TargetMethod(Marshaler& m, usize slot);
    void impl_VisualMethod(Marshaler& m, usize slot);
    void impl_SurfaceMethod(Marshaler& m, usize slot);
    void impl_UpdateObjectMethod(Marshaler& m, usize slot);
    void impl_TransformMethod(Marshaler& m, usize slot);
    void impl_RectangleClipMethod(Marshaler& m, usize slot);

private:
    using GuidBytes = std::array<u8, 16>;

    struct Device {
        GuestAddr legacy_interface{0};
        GuestAddr device2_interface{0};
        GuestAddr desktop_interface{0};
        GuestAddr device3_interface{0};
        GuestAddr rendering_device{0};
        u32 ref_count{1};
        u64 commit_id{0};
        // The surface in SurfaceDraw::Drawing, or 0 when none is active.
        // A suspended surface does not hold this.
        GuestAddr drawing_surface{0};
    };

    struct Target {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        GuestAddr hwnd{0};
        GuestAddr root{0};
        u32 ref_count{1};
        bool topmost{false};
    };

    struct Visual {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        GuestAddr parent{0};
        GuestAddr root_target{0};
        GuestAddr content{0};
        GuestAddr transform{0};
        GuestAddr transform_parent{0};
        GuestAddr clip{0};
        u32 ref_count{1};
        std::array<float, 6> matrix{1.0f, 0.0f, 0.0f,
                                    1.0f, 0.0f, 0.0f};
        std::array<float, 4> clip_rect{};
        bool has_matrix{false};
        bool has_clip_rect{false};
        float offset_x{0.0f};
        float offset_y{0.0f};
        u32 bitmap_interpolation_mode{0};
        u32 border_mode{0};
        u32 composite_mode{0};
        u32 opacity_mode{0};
        u32 back_face_visibility{0};
        std::vector<GuestAddr> children;
    };

    enum class TransformKind {
        Translate,
        Scale,
        Rotate,
        Skew,
        Matrix,
        Group,
    };

    struct Transform {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        TransformKind kind{TransformKind::Matrix};
        u32 ref_count{1};
        // Translate: x/y. Scale: x/y/center x/y. Rotate: angle/center x/y.
        // Skew: angle x/y/center x/y. Matrix: six affine elements.
        std::array<float, 6> values{1.0f, 0.0f, 0.0f,
                                    1.0f, 0.0f, 0.0f};
        std::vector<GuestAddr> group;
    };

    struct RectangleClip {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        // left, top, right, bottom, then the x/y radius of each corner.
        std::array<float, 12> values{};
    };

    // Learn IDCompositionSurface: one draw is active (not suspended) per
    // device. Idle has no open update. Drawing is between BeginDraw and
    // SuspendDraw/EndDraw. Suspended still owns an unpublished update.
    enum class SurfaceDraw : u8 { Idle, Drawing, Suspended };

    struct Surface {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        u32 width{0};
        u32 height{0};
        u32 format{0};
        u32 alpha{0};
        u32 bpp{0};
        u32 row_pitch{0};
        SurfaceDraw draw{SurfaceDraw::Idle};
        bool has_drawn{false};
        bool initialized{false};
        bool mapped{false};
        i32 origin_x{0};
        i32 origin_y{0};
        u32 update_w{0};
        u32 update_h{0};
        u32 update_pitch{0};
        GuestAddr update_object{0};
        GuestAddr pixels{0};
        usize pixels_mapped{0};
        GuestAddr presenter_hwnd{0};
        void* presenter_view{nullptr};
        std::vector<u8> published;
        std::unique_ptr<graphics::BackendTexture2D> texture;
        std::unique_ptr<graphics::BackendSwapChain> presenter;
        // Learn CreateVirtualSurface: width/height is the logical surface.
        // Realized pixels are only what BeginDraw allocated and Trim or
        // Resize has not discarded. One rectangle is `texture`, placed at
        // content_x/content_y. Several rectangles are `pieces`: Learn Trim
        // says pixels outside the kept rectangles are no longer used for
        // texturing, and it does not make that hole a transparent texel.
        // present_blocked means the realization cannot be shown.
        bool virtual_surface{false};
        bool present_blocked{false};
        i32 content_x{0};
        i32 content_y{0};
        u32 content_w{0};
        u32 content_h{0};
        struct RealizedRect {
            i32 x{0};
            i32 y{0};
            u32 width{0};
            u32 height{0};
            std::vector<u8> pixels;
        };
        std::vector<RealizedRect> realized;
        // One composition layer per realized rectangle. The hole between
        // them is not uploaded.
        struct PresentedPiece {
            i32 x{0};
            i32 y{0};
            u32 width{0};
            u32 height{0};
            GuestAddr presenter_hwnd{0};
            void* presenter_view{nullptr};
            std::unique_ptr<graphics::BackendTexture2D> texture;
            std::unique_ptr<graphics::BackendSwapChain> presenter;
        };
        std::vector<PresentedPiece> pieces;
    };

    struct IntRect {
        i32 left{0};
        i32 top{0};
        i32 right{0};
        i32 bottom{0};
    };

    // The DXGI surface BeginDraw hands back. It describes the allocated
    // update rectangle, not a cached pointer the caller may keep past EndDraw.
    struct UpdateObject {
        GuestAddr interface_address{0};
        GuestAddr surface{0};
        u32 ref_count{1};
        bool live{true};
        u32 width{0};
        u32 height{0};
        u32 format{0};
    };

    struct DeviceInterfaceEntry {
        GuestAddr owner{0};
        DeviceInterface kind{DeviceInterface::Legacy};
    };

    [[nodiscard]] bool ensure_storage();
    [[nodiscard]] bool write_pointer(GuestAddr address, GuestAddr value);
    [[nodiscard]] bool read_guid(GuestAddr address, GuidBytes& guid) const;
    [[nodiscard]] GuestAddr create_device(GuestAddr rendering_device,
                                          DeviceInterface requested);
    [[nodiscard]] GuestAddr create_visual(GuestAddr device_owner);
    [[nodiscard]] GuestAddr create_target(GuestAddr device_owner,
                                          GuestAddr hwnd, bool topmost);
    [[nodiscard]] GuestAddr create_transform(GuestAddr device_owner,
                                             TransformKind kind);
    [[nodiscard]] GuestAddr create_transform_group(
        GuestAddr device_owner, const std::vector<GuestAddr>& transforms);
    [[nodiscard]] GuestAddr create_rectangle_clip(GuestAddr device_owner);
    [[nodiscard]] u32 query_device_interface(Marshaler& m,
                                             GuestAddr interface_address);
    [[nodiscard]] u32 add_ref_device(GuestAddr interface_address);
    [[nodiscard]] u32 release_device(GuestAddr interface_address);
    [[nodiscard]] u32 release_visual(GuestAddr address);
    [[nodiscard]] u32 release_target(GuestAddr address);
    [[nodiscard]] u32 release_transform(GuestAddr address);
    [[nodiscard]] u32 release_rectangle_clip(GuestAddr address);
    void release_device_owner(GuestAddr owner);
    void detach_visual(GuestAddr visual);
    [[nodiscard]] bool bind_visual_tree(GuestAddr visual, GuestAddr hwnd,
                                        bool above_window_children,
                                        std::vector<GuestAddr>& visiting);
    [[nodiscard]] bool transform_matrix(GuestAddr transform,
                                        std::array<float, 6>& matrix,
                                        std::vector<GuestAddr>& visiting) const;
    [[nodiscard]] bool read_float_array(GuestAddr address, float* values,
                                        usize count) const;
    [[nodiscard]] float scalar_arg(Marshaler& m, usize index) const;
    void create_device_entry(Marshaler& m, DeviceInterface requested,
                             const char* api);
    /// D3D11 device / DXGI device, or a D3D12 device or command queue.
    /// Takes the reference the composition device holds until Release.
    [[nodiscard]] bool retain_rendering_device(GuestAddr address);
    void release_rendering_device(GuestAddr address);
    void unsupported_device_method(Marshaler& m, DeviceInterface interface,
                                   usize slot);
    /// A D3D11 swap chain, or a D3D12 swap chain in the same guest.
    [[nodiscard]] bool content_is_swap_chain(GuestAddr content) const;
    [[nodiscard]] bool retain_visual_content(GuestAddr content);
    void release_visual_content(GuestAddr content);
    [[nodiscard]] bool bind_visual_content(
        GuestAddr content, GuestAddr hwnd,
        const graphics::BackendCompositionLayerDesc& layer);
    void create_composition_surface(Marshaler& m, GuestAddr owner,
                                    bool virtual_surface);
    [[nodiscard]] bool read_surface_rect(GuestAddr address,
                                         IntRect& rect) const;
    [[nodiscard]] bool rect_inside_surface(const Surface& surface,
                                           const IntRect& rect) const;
    [[nodiscard]] bool ensure_update_pixels(Surface& surface, u32 bytes);
    void blit_realized(Surface& surface, i32 x, i32 y, u32 width, u32 height,
                       const u8* source, u32 source_pitch);
    [[nodiscard]] bool scroll_realized(Surface& surface, IntRect source,
                                       IntRect destination, i32 dx, i32 dy);
    void retain_realized(Surface& surface, const std::vector<IntRect>& keep);
    void scroll_published(Surface& surface, IntRect source,
                          IntRect destination, i32 dx, i32 dy);
    [[nodiscard]] bool compose_scroll(const Surface& surface,
                                      const IntRect* scroll,
                                      const IntRect* clip, i32 dx, i32 dy,
                                      IntRect& source,
                                      IntRect& destination) const;
    /// S_OK stores the realization. E_OUTOFMEMORY when a rectangle does not
    /// fit in one texture. E_NOTIMPL when the backend refuses an upload.
    /// Several rectangles are one texture each; the hole is not a texel.
    [[nodiscard]] u32 publish_virtual_surface(Surface& surface);
    void end_virtual_draw(Marshaler& m, Surface& surface, Device& device);
    void scroll_surface(Marshaler& m, Surface& surface);
    void resize_virtual_surface(Marshaler& m, Surface& surface);
    void trim_virtual_surface(Marshaler& m, Surface& surface);
    [[nodiscard]] bool bind_virtual_surface(
        Surface& surface, GuestAddr hwnd, void* view,
        const graphics::BackendCompositionLayerDesc& layer);
    [[nodiscard]] u32 release_surface(GuestAddr address);
    [[nodiscard]] bool bind_composition_surface(
        GuestAddr content, GuestAddr hwnd,
        const graphics::BackendCompositionLayerDesc& layer);
    [[nodiscard]] graphics::GraphicsBackendDevice* graphics_backend();
    [[nodiscard]] void* presentation_view_for(GuestAddr hwnd);
    [[nodiscard]] bool rendering_device_answers(GuestAddr device,
                                               const GuidBytes& iid);

    GuestMemory& m_memory;
    bool m_is_64bit;
    D3D11Manager& m_d3d11;
    MethodThunks m_method_thunks{};
    WindowOwnershipResolver m_window_ownership_resolver;
    GuestAddr m_region_base{0};
    GuestAddr m_device_vtable{0};
    GuestAddr m_device2_vtable{0};
    GuestAddr m_desktop_vtable{0};
    GuestAddr m_device3_vtable{0};
    GuestAddr m_target_vtable{0};
    GuestAddr m_visual_vtable{0};
    GuestAddr m_translate_transform_vtable{0};
    GuestAddr m_scale_transform_vtable{0};
    GuestAddr m_rotate_transform_vtable{0};
    GuestAddr m_skew_transform_vtable{0};
    GuestAddr m_matrix_transform_vtable{0};
    GuestAddr m_rectangle_clip_vtable{0};
    GuestAddr m_surface_vtable{0};
    GuestAddr m_update_vtable{0};
    GuestAddr m_next_object{0};
    bool m_storage_ready{false};
    std::unordered_map<GuestAddr, Device> m_devices;
    std::unordered_map<GuestAddr, DeviceInterfaceEntry> m_device_interfaces;
    std::unordered_map<GuestAddr, Target> m_targets;
    std::unordered_map<GuestAddr, Visual> m_visuals;
    std::unordered_map<GuestAddr, Transform> m_transforms;
    std::unordered_map<GuestAddr, RectangleClip> m_rectangle_clips;
    std::unordered_map<GuestAddr, Surface> m_surfaces;
    std::unordered_map<GuestAddr, UpdateObject> m_update_objects;
    std::unordered_map<u64, GuestAddr> m_target_layers;
};

[[nodiscard]] DllExports create_dcomp_thunks(DcompManager& manager);
[[nodiscard]] std::string dcomp_device_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_device2_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_desktop_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_device3_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_target_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_visual_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_translate_transform_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_scale_transform_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_rotate_transform_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_skew_transform_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_matrix_transform_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_rectangle_clip_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_surface_method_export_name(usize slot);
[[nodiscard]] std::string dcomp_update_object_method_export_name(usize slot);

} // namespace vitreo::winapi
