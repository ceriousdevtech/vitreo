// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dcomp.hpp
/// @brief DirectComposition device, target, and visual graph contracts.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <array>
#include <functional>
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
constexpr u32 DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED = 0x88980800u;
constexpr u32 DCOMPOSITION_ERROR_ACCESS_DENIED = 0x88980803u;

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
    };

    DcompManager(GuestMemory& memory, bool is_64bit,
                 D3D11Manager& d3d11_manager);

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
    void unsupported_device_method(Marshaler& m, DeviceInterface interface,
                                   usize slot);

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
    GuestAddr m_next_object{0};
    bool m_storage_ready{false};
    std::unordered_map<GuestAddr, Device> m_devices;
    std::unordered_map<GuestAddr, DeviceInterfaceEntry> m_device_interfaces;
    std::unordered_map<GuestAddr, Target> m_targets;
    std::unordered_map<GuestAddr, Visual> m_visuals;
    std::unordered_map<GuestAddr, Transform> m_transforms;
    std::unordered_map<GuestAddr, RectangleClip> m_rectangle_clips;
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

} // namespace vitreo::winapi
