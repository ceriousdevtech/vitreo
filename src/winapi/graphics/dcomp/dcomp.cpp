// SPDX-License-Identifier: LGPL-2.1-only
#include "dcomp.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_constants.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace vitreo::winapi {
namespace {

constexpr usize STORAGE_SIZE = 0x40000;
constexpr usize NO_OUTPUT = static_cast<usize>(-1);

constexpr std::array<u8, 16> IID_IUNKNOWN{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONDEVICE{
    0x3A, 0xA9, 0x7E, 0xC3, 0xAA, 0xE7, 0x0D, 0x45,
    0xB1, 0x6F, 0x97, 0x46, 0xCB, 0x04, 0x07, 0xF3};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONDEVICE2{
    0x8D, 0x46, 0xF6, 0x75, 0x8E, 0x1B, 0x7C, 0x44,
    0x9B, 0xC6, 0x75, 0xFE, 0xA8, 0x0B, 0x5B, 0x25};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONDESKTOPDEVICE{
    0xFE, 0x33, 0x46, 0x5F, 0x08, 0x1E, 0xB8, 0x4C,
    0x8C, 0x75, 0xCE, 0x24, 0x33, 0x3F, 0x56, 0x02};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONDEVICE3{
    0x06, 0xCB, 0x87, 0x09, 0x16, 0xF9, 0xBF, 0x48,
    0x8D, 0x35, 0xCE, 0x76, 0x41, 0x78, 0x1B, 0xD9};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONTARGET{
    0x4C, 0xD0, 0xCD, 0xEA, 0x7E, 0x11, 0x17, 0x4E,
    0x88, 0xF4, 0xD1, 0xB1, 0x2B, 0x0E, 0x3D, 0x89};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONVISUAL{
    0x9D, 0x05, 0x93, 0x4D, 0x7B, 0x09, 0x51, 0x46,
    0x9A, 0x60, 0xF0, 0xF2, 0x51, 0x16, 0xE2, 0xF3};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONVISUAL2{
    0x39, 0x16, 0xDE, 0xE8, 0x31, 0x43, 0x26, 0x4B,
    0xBC, 0x5F, 0x6A, 0x32, 0x1D, 0x34, 0x7A, 0x85};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONEFFECT{
    0x8F, 0xB0, 0x81, 0xEC, 0xCB, 0xBF, 0x8D, 0x4E,
    0xB1, 0x93, 0xA9, 0x15, 0x58, 0x79, 0x99, 0xE8};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONTRANSFORM3D{
    0x22, 0x57, 0x18, 0x71, 0x6B, 0x24, 0xF2, 0x41,
    0xAA, 0xD1, 0x04, 0x43, 0xF7, 0xF4, 0xBF, 0xC2};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONTRANSFORM{
    0xA7, 0xFA, 0x55, 0xFD, 0xE0, 0x37, 0x20, 0x4C,
    0x95, 0xD2, 0x9B, 0xE4, 0x5B, 0xC3, 0x3F, 0x55};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONTRANSLATETRANSFORM{
    0x22, 0x11, 0x79, 0x06, 0xF0, 0xC6, 0x7D, 0x41,
    0x83, 0x23, 0x26, 0x9E, 0x98, 0x7F, 0x59, 0x54};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONSCALETRANSFORM{
    0x14, 0xE9, 0xFD, 0x71, 0xEF, 0x40, 0xEF, 0x45,
    0xBD, 0x51, 0x68, 0xB0, 0x37, 0xC3, 0x39, 0xF9};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONROTATETRANSFORM{
    0x3C, 0xD8, 0x1E, 0x64, 0x96, 0xAE, 0xC5, 0x46,
    0x90, 0xDC, 0x32, 0x77, 0x4C, 0xC5, 0xC6, 0xD5};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONSKEWTRANSFORM{
    0x35, 0xA7, 0x7A, 0xE5, 0xDB, 0xDC, 0x72, 0x4C,
    0x9C, 0x61, 0x05, 0x91, 0xF5, 0x88, 0x89, 0xEE};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONMATRIXTRANSFORM{
    0x07, 0xFF, 0xCD, 0x16, 0x03, 0xC5, 0x9C, 0x41,
    0x83, 0xF2, 0x09, 0x65, 0xC7, 0xAF, 0x1F, 0xA6};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONCLIP{
    0x03, 0x37, 0xAC, 0x64, 0x3F, 0x9D, 0xEC, 0x45,
    0xA1, 0x09, 0x7C, 0xAC, 0x0E, 0x7A, 0x13, 0xA7};
constexpr std::array<u8, 16> IID_IDCOMPOSITIONRECTANGLECLIP{
    0x7D, 0xAD, 0x42, 0x98, 0xCF, 0xD9, 0x08, 0x49,
    0xAE, 0xD7, 0x48, 0xB5, 0x1D, 0xA5, 0xE7, 0xC2};
// dcomp.h IDCompositionSurface {bb8a4953-2c99-4f5a-96f5-4819027fa3ac}
constexpr std::array<u8, 16> IID_IDCOMPOSITIONSURFACE{
    0x53, 0x49, 0x8A, 0xBB, 0x99, 0x2C, 0x5A, 0x4F,
    0x96, 0xF5, 0x48, 0x19, 0x02, 0x7F, 0xA3, 0xAC};
// dcomp.h IDCompositionVirtualSurface {AE471C51-5F53-4A24-8D3E-D0C39C30B3F0}
constexpr std::array<u8, 16> IID_IDCOMPOSITIONVIRTUALSURFACE{
    0x51, 0x1C, 0x47, 0xAE, 0x53, 0x5F, 0x24, 0x4A,
    0x8D, 0x3E, 0xD0, 0xC3, 0x9C, 0x30, 0xB3, 0xF0};
// dxgi.h IDXGISurface {cafcb56c-6ac3-4889-bf47-9e23bbd260ec}
constexpr std::array<u8, 16> IID_IDXGISURFACE{
    0x6C, 0xB5, 0xFC, 0xCA, 0xC3, 0x6A, 0x89, 0x48,
    0xBF, 0x47, 0x9E, 0x23, 0xBB, 0xD2, 0x60, 0xEC};
// dxgi.h IDXGISurface1 {4ae63092-6327-4c1b-80ae-bfe12ea32b86}
constexpr std::array<u8, 16> IID_IDXGISURFACE1{
    0x92, 0x30, 0xE6, 0x4A, 0x27, 0x63, 0x1B, 0x4C,
    0x80, 0xAE, 0xBF, 0xE1, 0x2E, 0xA3, 0x2B, 0x86};
// d2d1_1.h ID2D1DeviceContext {e8f7fe7a-191c-466d-ad95-975678bda998}
constexpr std::array<u8, 16> IID_ID2D1DEVICECONTEXT{
    0x7A, 0xFE, 0xF7, 0xE8, 0x1C, 0x19, 0x6D, 0x46,
    0xAD, 0x95, 0x97, 0x56, 0x78, 0xBD, 0xA9, 0x98};

// dxgi1_2.h DXGI_ALPHA_MODE. UNSPECIFIED has the same effect as IGNORE
// (Learn CreateSurface). STRAIGHT is not a CreateSurface alpha mode.
constexpr u32 DXGI_ALPHA_MODE_UNSPECIFIED = 0;
constexpr u32 DXGI_ALPHA_MODE_PREMULTIPLIED = 1;
constexpr u32 DXGI_ALPHA_MODE_STRAIGHT = 2;
constexpr u32 DXGI_ALPHA_MODE_IGNORE = 3;
// dxgi.h DXGI_MAP flags (the D3D11 surface facet uses the same values).
constexpr u32 DXGI_MAP_READ = 1;
constexpr u32 DXGI_MAP_WRITE = 2;
constexpr u32 DXGI_MAP_DISCARD = 4;
// Learn CreateVirtualSurface / Resize: the edge fails when it exceeds this.
constexpr u32 kVirtualSurfaceMaxEdge = 16777216u;
// A realization larger than this is not stored as one texture. BeginDraw of
// a bigger update, and a solid bounding box past this limit, return
// E_OUTOFMEMORY. A non-rectangular realization is stored as one texture
// per realized rectangle.
constexpr u64 kRealizedByteLimit = 128ull * 1024ull * 1024ull;

constexpr std::array<u32, dcomp::SURFACE_METHOD_COUNT> SURFACE_BYTES{
    12, 4, 4, 20, 4, 4, 4, 20, 12, 12};
constexpr std::array<u32, dcomp::UPDATE_OBJECT_METHOD_COUNT> UPDATE_BYTES{
    12, 4, 4, 16, 12, 16, 12, 12, 8, 12, 4, 12, 8};

void unimplemented_once(bool& logged, const char* api, const char* detail) {
    if (logged) return;
    logged = true;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED dcomp!%s: %s\n", api, detail);
}

bool guid_equal(const std::array<u8, 16>& lhs,
                const std::array<u8, 16>& rhs) {
    return lhs == rhs;
}

// Learn IDCompositionVisual::SetClip: the clip restricts this visual and its
// subtree, and is transformed by OffsetX/OffsetY/Transform. Axis-aligned
// rectangle clips flatten through the 3x2 world matrix; rounded clips stay
// unsupported because they need a container layer.
using WorldClip = std::optional<std::array<float, 4>>;

std::array<float, 4> transform_rect_aabb(const std::array<float, 6>& matrix,
                                         const std::array<float, 4>& rect) {
    const float xs[4] = {rect[0], rect[2], rect[0], rect[2]};
    const float ys[4] = {rect[1], rect[1], rect[3], rect[3]};
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float x = xs[i] * matrix[0] + ys[i] * matrix[2] + matrix[4];
        const float y = xs[i] * matrix[1] + ys[i] * matrix[3] + matrix[5];
        if (i == 0) {
            min_x = max_x = x;
            min_y = max_y = y;
        } else {
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    }
    return {min_x, min_y, max_x, max_y};
}

std::array<float, 4> intersect_rect(const std::array<float, 4>& a,
                                    const std::array<float, 4>& b) {
    return {
        std::max(a[0], b[0]), std::max(a[1], b[1]),
        std::min(a[2], b[2]), std::min(a[3], b[3])};
}

bool invert_affine(const std::array<float, 6>& matrix,
                   std::array<float, 6>& inverse) {
    const float det = matrix[0] * matrix[3] - matrix[1] * matrix[2];
    if (!std::isfinite(det) || std::fabs(det) < 1.0e-12f) {
        return false;
    }
    const float inv_det = 1.0f / det;
    inverse[0] = matrix[3] * inv_det;
    inverse[1] = -matrix[1] * inv_det;
    inverse[2] = -matrix[2] * inv_det;
    inverse[3] = matrix[0] * inv_det;
    inverse[4] = -matrix[4] * inverse[0] - matrix[5] * inverse[2];
    inverse[5] = -matrix[4] * inverse[1] - matrix[5] * inverse[3];
    return std::all_of(inverse.begin(), inverse.end(),
                       [](float item) { return std::isfinite(item); });
}

void dcomp_diag(const char* format, ...) {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_DCOMP_DIAG");
        return value != nullptr && value[0] != '\0' &&
               std::strcmp(value, "0") != 0;
    }();
    if (!enabled) return;
    std::fprintf(stderr, "[dcomp-diag] ");
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

std::string method_export_name(const char* interface_name, usize slot) {
    return std::string("_DComp_") + interface_name + "_" +
           std::to_string(slot);
}

u32 device_stdcall_bytes(DcompManager::DeviceInterface interface,
                         usize slot) {
    if (slot == 0) return 12;
    if (slot == 1 || slot == 2 || slot == 3 || slot == 4) return 4;
    if (slot == 5) return 8;
    if (interface == DcompManager::DeviceInterface::Legacy) {
        constexpr std::array<u32, dcomp::DEVICE_METHOD_COUNT> bytes{
            12, 4, 4, 4, 4, 8, 16, 8, 24, 24, 12, 12, 8, 8,
            8, 8, 8, 16, 8, 8, 8, 8, 16, 8, 8, 8, 8};
        return bytes.at(slot);
    }
    if (interface == DcompManager::DeviceInterface::Device2) {
        constexpr std::array<u32, dcomp::DEVICE2_METHOD_COUNT> bytes{
            12, 4, 4, 4, 4, 8, 8, 12, 24, 24, 8, 8,
            8, 8, 8, 16, 8, 8, 8, 8, 16, 8, 8, 8};
        return bytes.at(slot);
    }
    if (interface == DcompManager::DeviceInterface::Desktop) {
        if (slot < dcomp::DEVICE2_METHOD_COUNT) {
            return device_stdcall_bytes(
                DcompManager::DeviceInterface::Device2, slot);
        }
        constexpr std::array<u32, 3> bytes{16, 12, 12};
        return bytes.at(slot - dcomp::DEVICE2_METHOD_COUNT);
    }
    if (slot < dcomp::DEVICE2_METHOD_COUNT) {
        return device_stdcall_bytes(DcompManager::DeviceInterface::Device2,
                                    slot);
    }
    return 8;
}

constexpr std::array<u32, dcomp::TARGET_METHOD_COUNT> TARGET_BYTES{
    12, 4, 4, 8};
constexpr std::array<u32, dcomp::VISUAL2_METHOD_COUNT> VISUAL_BYTES{
    12, 4, 4, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 16, 8, 4,
    8, 8, 8};
constexpr std::array<u32, dcomp::MATRIX_TRANSFORM_METHOD_COUNT>
    MATRIX_TRANSFORM_BYTES{12, 4, 4, 8, 16, 16};

} // namespace

std::string dcomp_device_method_export_name(usize slot) {
    return method_export_name("Device", slot);
}
std::string dcomp_device2_method_export_name(usize slot) {
    return method_export_name("Device2", slot);
}
std::string dcomp_desktop_method_export_name(usize slot) {
    return method_export_name("DesktopDevice", slot);
}
std::string dcomp_device3_method_export_name(usize slot) {
    return method_export_name("Device3", slot);
}
std::string dcomp_target_method_export_name(usize slot) {
    return method_export_name("Target", slot);
}
std::string dcomp_visual_method_export_name(usize slot) {
    return method_export_name("Visual2", slot);
}
std::string dcomp_translate_transform_method_export_name(usize slot) {
    return method_export_name("TranslateTransform", slot);
}
std::string dcomp_scale_transform_method_export_name(usize slot) {
    return method_export_name("ScaleTransform", slot);
}
std::string dcomp_rotate_transform_method_export_name(usize slot) {
    return method_export_name("RotateTransform", slot);
}
std::string dcomp_skew_transform_method_export_name(usize slot) {
    return method_export_name("SkewTransform", slot);
}
std::string dcomp_matrix_transform_method_export_name(usize slot) {
    return method_export_name("MatrixTransform", slot);
}
std::string dcomp_rectangle_clip_method_export_name(usize slot) {
    return method_export_name("RectangleClip", slot);
}
std::string dcomp_surface_method_export_name(usize slot) {
    return method_export_name("Surface", slot);
}
std::string dcomp_update_object_method_export_name(usize slot) {
    return method_export_name("SurfaceUpdate", slot);
}

DcompManager::DcompManager(GuestMemory& memory, bool is_64bit,
                           D3D11Manager& d3d11_manager)
    : m_memory(memory), m_is_64bit(is_64bit), m_d3d11(d3d11_manager) {}

DcompManager::~DcompManager() {
    for (auto& entry : m_surfaces) {
        auto& surface = entry.second;
        if (surface.pixels != 0) {
            (void)m_memory.unmap_region(surface.pixels, surface.pixels_mapped);
            surface.pixels = 0;
        }
    }
}

void DcompManager::set_method_thunks(MethodThunks thunks) {
    m_method_thunks = std::move(thunks);
}

void DcompManager::set_window_ownership_resolver(
    WindowOwnershipResolver resolver) {
    m_window_ownership_resolver = std::move(resolver);
}

bool DcompManager::write_pointer(GuestAddr address, GuestAddr value) {
    return m_is_64bit ? m_memory.write_u64(address, value).is_ok()
                      : m_memory.write_u32(address,
                                           static_cast<u32>(value)).is_ok();
}

bool DcompManager::read_guid(GuestAddr address, GuidBytes& guid) const {
    return address != 0 &&
           m_memory.read_bytes(address, guid.data(), guid.size()).is_ok();
}

bool DcompManager::ensure_storage() {
    if (m_storage_ready) return true;
    if (m_region_base != 0) return false;
    GuestAddr minimum = 0x65000000ull;
    while (minimum != 0) {
        const GuestAddr base = m_memory.find_free_gap(minimum, STORAGE_SIZE);
        if (base == 0) {
            if (m_is_64bit || minimum != 0x65000000ull) return false;
            minimum = 0x12000000ull;
            continue;
        }
        if (m_memory.map_region(base, STORAGE_SIZE,
                                MemoryProtection::ReadWrite).is_ok()) {
            m_region_base = base;
            m_device_vtable = base;
            m_device2_vtable = base + 0x100;
            m_desktop_vtable = base + 0x200;
            m_device3_vtable = base + 0x300;
            m_target_vtable = base + 0x500;
            m_visual_vtable = base + 0x580;
            m_translate_transform_vtable = base + 0x680;
            m_scale_transform_vtable = base + 0x700;
            m_rotate_transform_vtable = base + 0x780;
            m_skew_transform_vtable = base + 0x800;
            m_matrix_transform_vtable = base + 0x880;
            m_rectangle_clip_vtable = base + 0x900;
            m_surface_vtable = base + 0xA00;
            m_update_vtable = base + 0xB00;
            m_next_object = base + 0x1000;
            break;
        }
        minimum = base + STORAGE_SIZE;
    }
    if (m_region_base == 0) return false;
    const usize pointer_size = m_is_64bit ? 8 : 4;
    // Surfaces are optional at startup: a caller that has not published
    // their thunks yet still builds the device and visual tables. Empty
    // slots are zero, so a guest call fails closed instead of jumping
    // through an uninitialised pointer.
    const auto write_vtable = [&](GuestAddr address, const auto& thunks,
                                  bool required) {
        for (usize slot = 0; slot < thunks.size(); ++slot) {
            const GuestAddr thunk = thunks[slot];
            if (thunk == 0 && !required) {
                if (!write_pointer(address + slot * pointer_size, 0)) {
                    return false;
                }
                continue;
            }
            if (thunk == 0 ||
                !write_pointer(address + slot * pointer_size, thunk)) {
                return false;
            }
        }
        return true;
    };
    m_storage_ready =
        write_vtable(m_device_vtable, m_method_thunks.device, true) &&
        write_vtable(m_device2_vtable, m_method_thunks.device2, true) &&
        write_vtable(m_desktop_vtable, m_method_thunks.desktop_device,
                     true) &&
        write_vtable(m_device3_vtable, m_method_thunks.device3, true) &&
        write_vtable(m_target_vtable, m_method_thunks.target, true) &&
        write_vtable(m_visual_vtable, m_method_thunks.visual2, true) &&
        write_vtable(m_translate_transform_vtable,
                     m_method_thunks.translate_transform, true) &&
        write_vtable(m_scale_transform_vtable,
                     m_method_thunks.scale_transform, true) &&
        write_vtable(m_rotate_transform_vtable,
                     m_method_thunks.rotate_transform, true) &&
        write_vtable(m_skew_transform_vtable,
                     m_method_thunks.skew_transform, true) &&
        write_vtable(m_matrix_transform_vtable,
                     m_method_thunks.matrix_transform, true) &&
        write_vtable(m_rectangle_clip_vtable,
                     m_method_thunks.rectangle_clip, true) &&
        write_vtable(m_surface_vtable, m_method_thunks.surface, false) &&
        write_vtable(m_update_vtable, m_method_thunks.update_object, false);
    return m_storage_ready;
}

GuestAddr DcompManager::create_device(GuestAddr rendering_device,
                                      DeviceInterface requested) {
    if (!ensure_storage() || m_next_object + 0x100 >
                                 m_region_base + STORAGE_SIZE) {
        return 0;
    }
    const GuestAddr owner = m_next_object;
    m_next_object += 0x100;
    Device device{};
    device.legacy_interface = owner;
    device.device2_interface = owner + 0x20;
    device.desktop_interface = owner + 0x40;
    device.device3_interface = owner + 0x60;
    device.rendering_device = rendering_device;
    if (!write_pointer(device.legacy_interface, m_device_vtable) ||
        !write_pointer(device.device2_interface, m_device2_vtable) ||
        !write_pointer(device.desktop_interface, m_desktop_vtable) ||
        !write_pointer(device.device3_interface, m_device3_vtable)) {
        return 0;
    }
    m_devices.emplace(owner, device);
    m_device_interfaces.emplace(
        device.legacy_interface,
        DeviceInterfaceEntry{owner, DeviceInterface::Legacy});
    m_device_interfaces.emplace(
        device.device2_interface,
        DeviceInterfaceEntry{owner, DeviceInterface::Device2});
    m_device_interfaces.emplace(
        device.desktop_interface,
        DeviceInterfaceEntry{owner, DeviceInterface::Desktop});
    m_device_interfaces.emplace(
        device.device3_interface,
        DeviceInterfaceEntry{owner, DeviceInterface::Device3});
    switch (requested) {
    case DeviceInterface::Legacy: return device.legacy_interface;
    case DeviceInterface::Device2: return device.device2_interface;
    case DeviceInterface::Desktop: return device.desktop_interface;
    case DeviceInterface::Device3: return device.device3_interface;
    }
    return 0;
}

GuestAddr DcompManager::create_visual(GuestAddr device_owner) {
    if (!ensure_storage() || m_devices.find(device_owner) == m_devices.end() ||
        m_next_object + 0x40 > m_region_base + STORAGE_SIZE) {
        return 0;
    }
    const GuestAddr address = m_next_object;
    m_next_object += 0x40;
    if (!write_pointer(address, m_visual_vtable)) return 0;
    Visual visual{};
    visual.interface_address = address;
    visual.device_owner = device_owner;
    m_visuals.emplace(address, std::move(visual));
    ++m_devices.at(device_owner).ref_count;
    return address;
}

GuestAddr DcompManager::create_target(GuestAddr device_owner, GuestAddr hwnd,
                                      bool topmost) {
    if (!ensure_storage() || m_devices.find(device_owner) == m_devices.end() ||
        m_next_object + 0x40 > m_region_base + STORAGE_SIZE) {
        return 0;
    }
    const GuestAddr address = m_next_object;
    m_next_object += 0x40;
    if (!write_pointer(address, m_target_vtable)) return 0;
    Target target{};
    target.interface_address = address;
    target.device_owner = device_owner;
    target.hwnd = hwnd;
    target.topmost = topmost;
    m_targets.emplace(address, target);
    m_target_layers.emplace((static_cast<u64>(hwnd) << 1) |
                                static_cast<u64>(topmost),
                            address);
    ++m_devices.at(device_owner).ref_count;
    return address;
}

GuestAddr DcompManager::create_transform(GuestAddr device_owner,
                                         TransformKind kind) {
    if (!ensure_storage() || !m_devices.contains(device_owner) ||
        m_next_object + 0x40 > m_region_base + STORAGE_SIZE) {
        return 0;
    }
    GuestAddr vtable = 0;
    switch (kind) {
    case TransformKind::Translate: vtable = m_translate_transform_vtable; break;
    case TransformKind::Scale: vtable = m_scale_transform_vtable; break;
    case TransformKind::Rotate: vtable = m_rotate_transform_vtable; break;
    case TransformKind::Skew: vtable = m_skew_transform_vtable; break;
    case TransformKind::Matrix:
    case TransformKind::Group: vtable = m_matrix_transform_vtable; break;
    }
    const GuestAddr address = m_next_object;
    m_next_object += 0x40;
    if (!write_pointer(address, vtable)) return 0;
    Transform transform{};
    transform.interface_address = address;
    transform.device_owner = device_owner;
    transform.kind = kind;
    if (kind == TransformKind::Translate || kind == TransformKind::Rotate ||
        kind == TransformKind::Skew) {
        transform.values.fill(0.0f);
    } else if (kind == TransformKind::Scale) {
        transform.values = {1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    }
    m_transforms.emplace(address, std::move(transform));
    ++m_devices.at(device_owner).ref_count;
    return address;
}

GuestAddr DcompManager::create_transform_group(
    GuestAddr device_owner, const std::vector<GuestAddr>& transforms) {
    const GuestAddr address = create_transform(device_owner,
                                               TransformKind::Group);
    if (address == 0) return 0;
    auto& group = m_transforms.at(address).group;
    group = transforms;
    for (const GuestAddr transform : group) {
        ++m_transforms.at(transform).ref_count;
    }
    return address;
}

GuestAddr DcompManager::create_rectangle_clip(GuestAddr device_owner) {
    if (!ensure_storage() || !m_devices.contains(device_owner) ||
        m_next_object + 0x40 > m_region_base + STORAGE_SIZE) {
        return 0;
    }
    const GuestAddr address = m_next_object;
    m_next_object += 0x40;
    if (!write_pointer(address, m_rectangle_clip_vtable)) return 0;
    RectangleClip clip{};
    clip.interface_address = address;
    clip.device_owner = device_owner;
    clip.values[0] = -2097152.0f;
    clip.values[1] = -2097152.0f;
    clip.values[2] = 2097152.0f;
    clip.values[3] = 2097152.0f;
    m_rectangle_clips.emplace(address, clip);
    ++m_devices.at(device_owner).ref_count;
    return address;
}

bool DcompManager::retain_rendering_device(GuestAddr address) {
    if (m_d3d11.is_device_interface(address)) {
        return m_d3d11.retain_device_reference(address);
    }
    // Learn (DCompositionCreateDevice2): the rendering device may be a
    // Direct3D device or NULL. D3D12 publishes no IDXGIDevice; callers pass
    // the ID3D12Device or the ID3D12CommandQueue (the object
    // CreateSwapChainForComposition takes). DCompositionCreateDevice accepts
    // those pointers too.
    if (D3D12Manager* d3d12 = D3D12Manager::for_guest_memory(m_memory)) {
        if (!d3d12->is_device(address) && !d3d12->is_command_queue(address)) {
            return false;
        }
        return d3d12->retain_rendering_device(address);
    }
    return false;
}

void DcompManager::release_rendering_device(GuestAddr address) {
    if (address == 0) return;
    if (m_d3d11.is_device_interface(address)) {
        m_d3d11.release_device_interface_reference(address);
        return;
    }
    if (D3D12Manager* d3d12 = D3D12Manager::for_guest_memory(m_memory)) {
        if (d3d12->is_device(address) || d3d12->is_command_queue(address)) {
            d3d12->release_rendering_device(address);
        }
    }
}

void DcompManager::create_device_entry(Marshaler& m,
                                       DeviceInterface requested,
                                       const char* api) {
    const GuestAddr rendering_device = m.arg_ptr(0);
    const GuestAddr iid_address = m.arg_ptr(1);
    const GuestAddr output = m.arg_ptr(2);
    if (output == 0) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(output, 0);
    GuidBytes iid{};
    if (!read_guid(iid_address, iid)) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    DeviceInterface iid_interface{};
    if (guid_equal(iid, IID_IDCOMPOSITIONDEVICE)) {
        iid_interface = DeviceInterface::Legacy;
    } else if (guid_equal(iid, IID_IDCOMPOSITIONDEVICE2)) {
        iid_interface = DeviceInterface::Device2;
    } else if (guid_equal(iid, IID_IDCOMPOSITIONDESKTOPDEVICE)) {
        iid_interface = DeviceInterface::Desktop;
    } else if (guid_equal(iid, IID_IDCOMPOSITIONDEVICE3)) {
        iid_interface = DeviceInterface::Device3;
    } else {
        m.set_return(dcomp::E_NOINTERFACE);
        return;
    }
    if (requested == DeviceInterface::Legacy &&
        iid_interface != DeviceInterface::Legacy) {
        m.set_return(dcomp::E_NOINTERFACE);
        return;
    }
    if (rendering_device != 0 &&
        !retain_rendering_device(rendering_device)) {
        dcomp_diag("%s rendering_device=0x%llx -> E_INVALIDARG",
                   api, static_cast<unsigned long long>(rendering_device));
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    const GuestAddr device = create_device(rendering_device, iid_interface);
    if (device == 0 || m.write_ptr(output, device).is_err()) {
        if (device != 0) {
            (void)release_device(device);
        } else if (rendering_device != 0) {
            release_rendering_device(rendering_device);
        }
        m.set_return(dcomp::E_POINTER);
        return;
    }
    dcomp_diag("%s rendering_device=0x%llx -> S_OK device=0x%llx kind=%u",
               api, static_cast<unsigned long long>(rendering_device),
               static_cast<unsigned long long>(device),
               static_cast<unsigned>(iid_interface));
    m.set_return(dcomp::S_OK);
}

void DcompManager::impl_DCompositionCreateDevice(Marshaler& m) {
    create_device_entry(m, DeviceInterface::Legacy,
                        "DCompositionCreateDevice");
}

void DcompManager::impl_DCompositionCreateDevice2(Marshaler& m) {
    create_device_entry(m, DeviceInterface::Device2,
                        "DCompositionCreateDevice2");
}

void DcompManager::impl_DCompositionCreateDevice3(Marshaler& m) {
    create_device_entry(m, DeviceInterface::Device3,
                        "DCompositionCreateDevice3");
}

void DcompManager::impl_DCompositionCreateSurfaceHandle(Marshaler& m) {
    const u32 desired_access = static_cast<u32>(m.arg(0));
    (void)m.arg_ptr(1);
    const GuestAddr output = m.arg_ptr(2);
    if ((desired_access & ~3u) != 0 || output == 0) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(output, 0);
    dcomp_diag("DCompositionCreateSurfaceHandle access=0x%x -> E_NOTIMPL",
               desired_access);
    m.set_return(dcomp::E_NOTIMPL);
}

void DcompManager::impl_PublicUnsupported(Marshaler& m, const char* name,
                                          usize output_arg) {
    if (output_arg != NO_OUTPUT) {
        const GuestAddr output = m.arg_ptr(output_arg);
        if (output != 0) (void)m.write_ptr(output, 0);
    }
    dcomp_diag("%s -> E_NOTIMPL", name);
    m.set_return(dcomp::E_NOTIMPL);
}

u32 DcompManager::query_device_interface(Marshaler& m,
                                         GuestAddr interface_address) {
    const GuestAddr output = m.arg_ptr(2);
    if (output == 0) return dcomp::E_POINTER;
    (void)m.write_ptr(output, 0);
    const auto entry = m_device_interfaces.find(interface_address);
    GuidBytes iid{};
    if (entry == m_device_interfaces.end() ||
        !read_guid(m.arg_ptr(1), iid)) {
        return dcomp::E_INVALIDARG;
    }
    const auto device = m_devices.find(entry->second.owner);
    if (device == m_devices.end()) return dcomp::E_INVALIDARG;
    GuestAddr result = 0;
    if (guid_equal(iid, IID_IUNKNOWN) ||
        guid_equal(iid, IID_IDCOMPOSITIONDEVICE)) {
        result = device->second.legacy_interface;
    } else if (guid_equal(iid, IID_IDCOMPOSITIONDEVICE2)) {
        result = device->second.device2_interface;
    } else if (guid_equal(iid, IID_IDCOMPOSITIONDESKTOPDEVICE)) {
        result = device->second.desktop_interface;
    } else if (guid_equal(iid, IID_IDCOMPOSITIONDEVICE3)) {
        result = device->second.device3_interface;
    } else {
        return dcomp::E_NOINTERFACE;
    }
    ++device->second.ref_count;
    if (m.write_ptr(output, result).is_err()) {
        --device->second.ref_count;
        return dcomp::E_POINTER;
    }
    return dcomp::S_OK;
}

u32 DcompManager::add_ref_device(GuestAddr interface_address) {
    const auto entry = m_device_interfaces.find(interface_address);
    if (entry == m_device_interfaces.end()) return 0;
    const auto device = m_devices.find(entry->second.owner);
    return device == m_devices.end() ? 0 : ++device->second.ref_count;
}

u32 DcompManager::release_device(GuestAddr interface_address) {
    const auto entry = m_device_interfaces.find(interface_address);
    if (entry == m_device_interfaces.end()) return 0;
    const GuestAddr owner = entry->second.owner;
    const auto device = m_devices.find(owner);
    if (device == m_devices.end() || device->second.ref_count == 0) return 0;
    const u32 count = --device->second.ref_count;
    if (count != 0) return count;
    const GuestAddr rendering_device = device->second.rendering_device;
    m_device_interfaces.erase(device->second.legacy_interface);
    m_device_interfaces.erase(device->second.device2_interface);
    m_device_interfaces.erase(device->second.desktop_interface);
    m_device_interfaces.erase(device->second.device3_interface);
    m_devices.erase(device);
    if (rendering_device != 0) {
        release_rendering_device(rendering_device);
    }
    return 0;
}

void DcompManager::release_device_owner(GuestAddr owner) {
    const auto device = m_devices.find(owner);
    if (device != m_devices.end()) {
        (void)release_device(device->second.legacy_interface);
    }
}

void DcompManager::detach_visual(GuestAddr address) {
    auto visual = m_visuals.find(address);
    if (visual == m_visuals.end()) return;
    if (visual->second.parent != 0) {
        auto parent = m_visuals.find(visual->second.parent);
        if (parent != m_visuals.end()) {
            auto& children = parent->second.children;
            children.erase(std::remove(children.begin(), children.end(),
                                       address),
                           children.end());
        }
        visual->second.parent = 0;
    }
    if (visual->second.root_target != 0) {
        auto target = m_targets.find(visual->second.root_target);
        if (target != m_targets.end() && target->second.root == address) {
            target->second.root = 0;
        }
        visual->second.root_target = 0;
    }
}

u32 DcompManager::release_visual(GuestAddr address) {
    auto visual = m_visuals.find(address);
    if (visual == m_visuals.end() || visual->second.ref_count == 0) return 0;
    const u32 count = --visual->second.ref_count;
    if (count != 0) return count;
    detach_visual(address);
    Visual dead = std::move(visual->second);
    m_visuals.erase(visual);
    for (const GuestAddr child : dead.children) {
        auto child_visual = m_visuals.find(child);
        if (child_visual != m_visuals.end()) child_visual->second.parent = 0;
        (void)release_visual(child);
    }
    if (dead.content != 0) release_visual_content(dead.content);
    if (dead.transform != 0) (void)release_transform(dead.transform);
    if (dead.transform_parent != 0) (void)release_visual(dead.transform_parent);
    if (dead.clip != 0) (void)release_rectangle_clip(dead.clip);
    release_device_owner(dead.device_owner);
    return 0;
}

u32 DcompManager::release_target(GuestAddr address) {
    auto target = m_targets.find(address);
    if (target == m_targets.end() || target->second.ref_count == 0) return 0;
    const u32 count = --target->second.ref_count;
    if (count != 0) return count;
    Target dead = target->second;
    m_targets.erase(target);
    m_target_layers.erase((static_cast<u64>(dead.hwnd) << 1) |
                          static_cast<u64>(dead.topmost));
    if (dead.root != 0) {
        auto root = m_visuals.find(dead.root);
        if (root != m_visuals.end()) root->second.root_target = 0;
        (void)release_visual(dead.root);
    }
    release_device_owner(dead.device_owner);
    return 0;
}

u32 DcompManager::release_transform(GuestAddr address) {
    auto transform = m_transforms.find(address);
    if (transform == m_transforms.end() || transform->second.ref_count == 0)
        return 0;
    const u32 count = --transform->second.ref_count;
    if (count != 0) return count;
    Transform dead = std::move(transform->second);
    m_transforms.erase(transform);
    for (const GuestAddr member : dead.group) (void)release_transform(member);
    release_device_owner(dead.device_owner);
    return 0;
}

u32 DcompManager::release_rectangle_clip(GuestAddr address) {
    auto clip = m_rectangle_clips.find(address);
    if (clip == m_rectangle_clips.end() || clip->second.ref_count == 0)
        return 0;
    const u32 count = --clip->second.ref_count;
    if (count != 0) return count;
    const GuestAddr owner = clip->second.device_owner;
    m_rectangle_clips.erase(clip);
    release_device_owner(owner);
    return 0;
}

bool DcompManager::read_float_array(GuestAddr address, float* values,
                                    usize count) const {
    if (address == 0 || values == nullptr || count == 0) return false;
    return m_memory.read_bytes(address, reinterpret_cast<Byte*>(values),
                               count * sizeof(float)).is_ok();
}

float DcompManager::scalar_arg(Marshaler& m, usize index) const {
    if (m.cpu_is_64bit() && index < 4 && m.cpu() != nullptr) {
        return m.cpu()->sse.xmm[index].f32_[0];
    }
    const u32 bits = static_cast<u32>(m.arg(index));
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool DcompManager::transform_matrix(
    GuestAddr address, std::array<float, 6>& matrix,
    std::vector<GuestAddr>& visiting) const {
    const auto transform = m_transforms.find(address);
    if (transform == m_transforms.end() ||
        std::find(visiting.begin(), visiting.end(), address) != visiting.end())
        return false;
    visiting.push_back(address);
    const auto multiply = [](const std::array<float, 6>& a,
                             const std::array<float, 6>& b) {
        return std::array<float, 6>{
            a[0] * b[0] + a[1] * b[2],
            a[0] * b[1] + a[1] * b[3],
            a[2] * b[0] + a[3] * b[2],
            a[2] * b[1] + a[3] * b[3],
            a[4] * b[0] + a[5] * b[2] + b[4],
            a[4] * b[1] + a[5] * b[3] + b[5]};
    };
    const auto& value = transform->second.values;
    constexpr float degrees_to_radians =
        3.14159265358979323846f / 180.0f;
    switch (transform->second.kind) {
    case TransformKind::Translate:
        matrix = {1.0f, 0.0f, 0.0f, 1.0f, value[0], value[1]};
        break;
    case TransformKind::Scale:
        matrix = {value[0], 0.0f, 0.0f, value[1],
                  value[2] * (1.0f - value[0]),
                  value[3] * (1.0f - value[1])};
        break;
    case TransformKind::Rotate: {
        const float radians = value[0] * degrees_to_radians;
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        matrix = {cosine, sine, -sine, cosine,
                  value[1] * (1.0f - cosine) + value[2] * sine,
                  value[2] * (1.0f - cosine) - value[1] * sine};
        break;
    }
    case TransformKind::Skew: {
        const float tangent_x = std::tan(value[0] * degrees_to_radians);
        const float tangent_y = std::tan(value[1] * degrees_to_radians);
        matrix = {1.0f, tangent_y, tangent_x, 1.0f,
                  -value[3] * tangent_x, -value[2] * tangent_y};
        break;
    }
    case TransformKind::Matrix: matrix = value; break;
    case TransformKind::Group:
        matrix = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        for (const GuestAddr member : transform->second.group) {
            std::array<float, 6> member_matrix{};
            if (!transform_matrix(member, member_matrix, visiting)) {
                visiting.pop_back();
                return false;
            }
            matrix = multiply(matrix, member_matrix);
        }
        break;
    }
    visiting.pop_back();
    return std::all_of(matrix.begin(), matrix.end(),
                       [](float item) { return std::isfinite(item); });
}

bool DcompManager::content_is_swap_chain(GuestAddr content) const {
    if (content == 0) return false;
    if (m_d3d11.is_swap_chain_object(content)) return true;
    if (D3D12Manager* d3d12 = D3D12Manager::for_guest_memory(m_memory)) {
        return d3d12->is_swap_chain(content);
    }
    return false;
}

bool DcompManager::retain_visual_content(GuestAddr content) {
    if (const auto surface = m_surfaces.find(content);
        surface != m_surfaces.end()) {
        ++surface->second.ref_count;
        return true;
    }
    if (m_d3d11.is_swap_chain_object(content)) {
        return m_d3d11.retain_swap_chain_reference(content);
    }
    if (D3D12Manager* d3d12 = D3D12Manager::for_guest_memory(m_memory)) {
        return d3d12->retain_swap_chain_reference(content);
    }
    return false;
}

void DcompManager::release_visual_content(GuestAddr content) {
    if (content == 0) return;
    if (m_surfaces.contains(content)) {
        (void)release_surface(content);
        return;
    }
    if (m_d3d11.is_swap_chain_object(content)) {
        m_d3d11.release_swap_chain_reference(content);
        return;
    }
    if (D3D12Manager* d3d12 = D3D12Manager::for_guest_memory(m_memory)) {
        d3d12->release_swap_chain_reference(content);
    }
}

bool DcompManager::bind_visual_content(
    GuestAddr content, GuestAddr hwnd,
    const graphics::BackendCompositionLayerDesc& layer) {
    if (m_surfaces.contains(content)) {
        return bind_composition_surface(content, hwnd, layer);
    }
    if (m_d3d11.is_swap_chain_object(content)) {
        return m_d3d11.bind_composition_swap_chain(content, hwnd, layer);
    }
    if (D3D12Manager* d3d12 = D3D12Manager::for_guest_memory(m_memory)) {
        return d3d12->bind_composition_swap_chain(content, hwnd, layer);
    }
    return false;
}

bool DcompManager::bind_visual_tree(GuestAddr address, GuestAddr hwnd,
                                    bool above_window_children,
                                    std::vector<GuestAddr>& visiting) {
    const auto multiply = [](const std::array<float, 6>& a,
                             const std::array<float, 6>& b) {
        return std::array<float, 6>{
            a[0] * b[0] + a[1] * b[2],
            a[0] * b[1] + a[1] * b[3],
            a[2] * b[0] + a[3] * b[2],
            a[2] * b[1] + a[3] * b[3],
            a[4] * b[0] + a[5] * b[2] + b[4],
            a[4] * b[1] + a[5] * b[3] + b[5]};
    };
    int z_order = 0;
    std::function<bool(GuestAddr, const std::array<float, 6>&, const WorldClip&)>
        bind = [&](GuestAddr current, const std::array<float, 6>& parent_matrix,
                   const WorldClip& ancestor_clip) {
        auto visual = m_visuals.find(current);
        if (visual == m_visuals.end() ||
            std::find(visiting.begin(), visiting.end(), current) !=
                visiting.end()) {
            dcomp_diag("bind-abort visual=0x%llx reason=unknown-or-cycle",
                       static_cast<unsigned long long>(current));
            return false;
        }
        visiting.push_back(current);
        // Effects, non-source-over composition, and transform-parent links
        // need additional compositor semantics. Do not ignore them.
        if ((visual->second.composite_mode != 0 &&
             visual->second.composite_mode != 0xFFFFFFFFu) ||
            visual->second.transform_parent != 0) {
            dcomp_diag("bind-abort visual=0x%llx reason=composite-mode "
                       "mode=0x%x transform_parent=0x%llx",
                       static_cast<unsigned long long>(current),
                       visual->second.composite_mode,
                       static_cast<unsigned long long>(
                           visual->second.transform_parent));
            visiting.pop_back();
            return false;
        }
        std::array<float, 6> local{
            1.0f, 0.0f, 0.0f, 1.0f,
            visual->second.offset_x, visual->second.offset_y};
        std::array<float, 6> transform{
            1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        if (visual->second.has_matrix) {
            transform = visual->second.matrix;
        } else if (visual->second.transform != 0) {
            std::vector<GuestAddr> transform_visiting;
            if (!transform_matrix(visual->second.transform, transform,
                                  transform_visiting)) {
                dcomp_diag("bind-abort visual=0x%llx reason=transform-matrix",
                           static_cast<unsigned long long>(current));
                visiting.pop_back();
                return false;
            }
        }
        local = multiply(local, transform);
        const auto world = multiply(local, parent_matrix);
        graphics::BackendCompositionLayerDesc layer{};
        layer.transform = world;
        layer.above_window_children = above_window_children;
        layer.z_order = z_order++;
        layer.interpolation = visual->second.bitmap_interpolation_mode == 0
            ? graphics::BackendFilter::Nearest
            : graphics::BackendFilter::Linear;
        WorldClip world_clip = ancestor_clip;
        std::array<float, 4> local_clip{};
        bool has_local_clip = false;
        if (visual->second.has_clip_rect) {
            local_clip = visual->second.clip_rect;
            has_local_clip = true;
        } else if (visual->second.clip != 0) {
            const auto clip = m_rectangle_clips.find(visual->second.clip);
            if (clip == m_rectangle_clips.end() ||
                std::any_of(clip->second.values.begin() + 4,
                            clip->second.values.end(),
                            [](float radius) { return radius != 0.0f; })) {
                dcomp_diag("bind-abort visual=0x%llx reason=clip-object "
                           "clip=0x%llx found=%d",
                           static_cast<unsigned long long>(current),
                           static_cast<unsigned long long>(
                               visual->second.clip),
                           clip != m_rectangle_clips.end());
                visiting.pop_back();
                return false;
            }
            std::copy_n(clip->second.values.begin(), 4, local_clip.begin());
            has_local_clip = true;
        }
        if (has_local_clip) {
            const auto this_world = transform_rect_aabb(world, local_clip);
            world_clip = world_clip
                ? WorldClip{intersect_rect(*world_clip, this_world)}
                : WorldClip{this_world};
        }
        if (world_clip) {
            std::array<float, 6> inverse{};
            if (!invert_affine(world, inverse)) {
                dcomp_diag("bind-abort visual=0x%llx reason=invert-affine",
                           static_cast<unsigned long long>(current));
                visiting.pop_back();
                return false;
            }
            layer.clip_rect = transform_rect_aabb(inverse, *world_clip);
            layer.has_clip_rect = true;
        }
        if (visual->second.content != 0 &&
            !bind_visual_content(visual->second.content, hwnd, layer)) {
            dcomp_diag("bind-abort visual=0x%llx reason=content-bind "
                       "content=0x%llx hwnd=0x%llx",
                       static_cast<unsigned long long>(current),
                       static_cast<unsigned long long>(
                           visual->second.content),
                       static_cast<unsigned long long>(hwnd));
            visiting.pop_back();
            return false;
        }
        if (visual->second.content != 0) {
            dcomp_diag(
                "Bind visual=0x%llx content=0x%llx hwnd=0x%llx "
                "offset=(%.1f,%.1f) children=%zu",
                static_cast<unsigned long long>(current),
                static_cast<unsigned long long>(visual->second.content),
                static_cast<unsigned long long>(hwnd), layer.transform[4],
                layer.transform[5], visual->second.children.size());
        }
        for (const GuestAddr child : visual->second.children) {
            if (!bind(child, world, world_clip)) {
                visiting.pop_back();
                return false;
            }
        }
        visiting.pop_back();
        return true;
    };
    return bind(address, {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, std::nullopt);
}

graphics::GraphicsBackendDevice* DcompManager::graphics_backend() {
    if (D3D12Manager* d3d12 = D3D12Manager::for_guest_memory(m_memory)) {
        return &d3d12->frontend().m_backend;
    }
    return nullptr;
}

void* DcompManager::presentation_view_for(GuestAddr hwnd) {
    if (hwnd == 0) return nullptr;
    D3D12Manager* d3d12 = D3D12Manager::for_guest_memory(m_memory);
    if (d3d12 == nullptr ||
        !d3d12->frontend().m_presentation.presentation_view) {
        return nullptr;
    }
    return d3d12->frontend().m_presentation.presentation_view(hwnd);
}

bool DcompManager::rendering_device_answers(GuestAddr device,
                                           const GuidBytes& iid) {
    if (device == 0) return false;
    if (guid_equal(iid, IID_IUNKNOWN)) return true;
    D3D12Manager* d3d12 = D3D12Manager::for_guest_memory(m_memory);
    if (d3d12 == nullptr) return false;
    if (d3d12->is_device(device) &&
        guid_equal(iid, d3d12::IID_ID3D12DEVICE)) {
        return true;
    }
    return d3d12->is_command_queue(device) &&
           guid_equal(iid, d3d12::IID_ID3D12COMMANDQUEUE);
}

u32 DcompManager::release_surface(GuestAddr address) {
    auto found = m_surfaces.find(address);
    if (found == m_surfaces.end() || found->second.ref_count == 0) return 0;
    const u32 count = --found->second.ref_count;
    if (count != 0) return count;
    Surface dead = std::move(found->second);
    m_surfaces.erase(found);
    if (auto device = m_devices.find(dead.device_owner);
        device != m_devices.end() &&
        device->second.drawing_surface == address) {
        device->second.drawing_surface = 0;
    }
    for (auto& entry : m_update_objects) {
        if (entry.second.surface == address) {
            entry.second.surface = 0;
            entry.second.live = false;
        }
    }
    if (dead.pixels != 0) {
        (void)m_memory.unmap_region(dead.pixels, dead.pixels_mapped);
    }
    release_device_owner(dead.device_owner);
    return 0;
}

void DcompManager::create_composition_surface(Marshaler& m, GuestAddr owner,
                                             bool virtual_surface) {
    const u32 width = static_cast<u32>(m.arg(1));
    const u32 height = static_cast<u32>(m.arg(2));
    const u32 format = static_cast<u32>(m.arg(3));
    const u32 alpha = static_cast<u32>(m.arg(4));
    const GuestAddr output = m.arg_ptr(5);
    if (output == 0) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(output, 0);
    // Learn CreateSurface / CreateVirtualSurface: the out pointer must not
    // be null, and only B8G8R8A8_UNORM, R8G8B8A8_UNORM and
    // R16G16B16A16_FLOAT are surfaces. UNSPECIFIED alpha matches IGNORE.
    // STRAIGHT is not in that list. A virtual surface fails when either
    // edge exceeds 16,777,216. Zero is a valid empty virtual surface;
    // CreateSurface still rejects it.
    const bool format_ok =
        format == d3d12::DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == d3d12::DXGI_FORMAT_R8G8B8A8_UNORM ||
        format == d3d12::DXGI_FORMAT_R16G16B16A16_FLOAT;
    const bool alpha_ok = alpha == DXGI_ALPHA_MODE_UNSPECIFIED ||
                          alpha == DXGI_ALPHA_MODE_PREMULTIPLIED ||
                          alpha == DXGI_ALPHA_MODE_IGNORE;
    if (!format_ok || !alpha_ok ||
        (!virtual_surface && (width == 0 || height == 0))) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    if (virtual_surface && (width > kVirtualSurfaceMaxEdge ||
                            height > kVirtualSurfaceMaxEdge)) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    const graphics::PixelFormat pixel_format =
        graphics::pixel_format_from_dxgi(format);
    const graphics::FormatInfo info = graphics::format_table(pixel_format);
    if (pixel_format == graphics::PixelFormat::Unknown ||
        info.bytes_per_pixel == 0) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    const u32 bpp = info.bytes_per_pixel;
    // A virtual surface allocates nothing until BeginDraw. The logical
    // size is not a texture.
    u32 row_pitch = 0;
    u32 byte_count = 0;
    std::unique_ptr<graphics::BackendTexture2D> texture;
    GuestAddr pixels = 0;
    usize mapped = 0;
    if (!virtual_surface) {
        if (width > (0xFFFFFFFFu / bpp)) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        row_pitch = width * bpp;
        if (height > (0xFFFFFFFFu / row_pitch)) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        byte_count = height * row_pitch;
        graphics::GraphicsBackendDevice* backend = graphics_backend();
        if (backend == nullptr) {
            static bool logged = false;
            unimplemented_once(logged, "IDCompositionDevice::CreateSurface",
                               "no graphics backend can store the surface");
            m.set_return(dcomp::E_NOTIMPL);
            return;
        }
        graphics::BackendTexture2DDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.pixel_format = pixel_format;
        desc.shader_resource = true;
        texture = backend->create_texture_2d(desc, nullptr, 0);
        if (!texture || !texture->valid()) {
            static bool logged = false;
            unimplemented_once(
                logged, "IDCompositionDevice::CreateSurface",
                "the graphics backend cannot store this format");
            m.set_return(dcomp::E_NOTIMPL);
            return;
        }
        if (!ensure_storage() ||
            m_next_object + 0x40 > m_region_base + STORAGE_SIZE) {
            m.set_return(dcomp::E_OUTOFMEMORY);
            return;
        }
        mapped = (static_cast<usize>(byte_count) + 0xFFFu) & ~usize{0xFFFu};
        pixels = m_memory.map_first_fit(
            m_is_64bit ? 0x20000000ull : 0x12000000ull, mapped,
            MemoryProtection::ReadWrite);
        if (pixels == 0) {
            m.set_return(dcomp::E_OUTOFMEMORY);
            return;
        }
        std::array<u8, 4096> zeros{};
        for (u32 done = 0; done < byte_count;) {
            const u32 chunk = std::min<u32>(static_cast<u32>(zeros.size()),
                                            byte_count - done);
            if (m_memory.write_bytes(pixels + done, zeros.data(), chunk)
                    .is_err()) {
                (void)m_memory.unmap_region(pixels, mapped);
                m.set_return(dcomp::E_OUTOFMEMORY);
                return;
            }
            done += chunk;
        }
    } else if (!ensure_storage() ||
               m_next_object + 0x40 > m_region_base + STORAGE_SIZE) {
        m.set_return(dcomp::E_OUTOFMEMORY);
        return;
    }
    const GuestAddr address = m_next_object;
    m_next_object += 0x40;
    if (!write_pointer(address, m_surface_vtable)) {
        if (pixels != 0) (void)m_memory.unmap_region(pixels, mapped);
        m.set_return(dcomp::E_OUTOFMEMORY);
        return;
    }
    Surface surface{};
    surface.interface_address = address;
    surface.device_owner = owner;
    surface.width = width;
    surface.height = height;
    surface.format = format;
    surface.alpha = alpha;
    surface.bpp = bpp;
    surface.row_pitch = row_pitch;
    surface.pixels = pixels;
    surface.pixels_mapped = mapped;
    surface.virtual_surface = virtual_surface;
    if (byte_count != 0) surface.published.assign(byte_count, 0);
    surface.texture = std::move(texture);
    m_surfaces.emplace(address, std::move(surface));
    ++m_devices.at(owner).ref_count;
    if (m.write_ptr(output, address).is_err()) {
        (void)release_surface(address);
        m.set_return(dcomp::E_POINTER);
        return;
    }
    dcomp_diag("%s %ux%u format=%u alpha=%u -> 0x%llx",
               virtual_surface ? "CreateVirtualSurface" : "CreateSurface",
               width, height, format, alpha,
               static_cast<unsigned long long>(address));
    m.set_return(dcomp::S_OK);
}

bool DcompManager::bind_composition_surface(
    GuestAddr content, GuestAddr hwnd,
    const graphics::BackendCompositionLayerDesc& layer) {
    auto found = m_surfaces.find(content);
    if (found == m_surfaces.end() || hwnd == 0) return false;
    Surface& surface = found->second;
    // A target whose HWND has no presentation view cannot be committed.
    // An uninitialized surface (Learn: 100% transparent, no effect) still
    // needs that view; with one, Commit succeeds and shows nothing.
    void* view = presentation_view_for(hwnd);
    if (view == nullptr) return false;
    if (surface.virtual_surface) {
        return bind_virtual_surface(surface, hwnd, view, layer);
    }
    if (!surface.initialized || !surface.texture) return true;
    graphics::GraphicsBackendDevice* backend = graphics_backend();
    if (backend == nullptr) return false;
    if (!surface.presenter || surface.presenter_hwnd != hwnd ||
        surface.presenter_view != view) {
        graphics::BackendSwapChainDesc desc{};
        desc.native_view = view;
        desc.width = surface.width;
        desc.height = surface.height;
        desc.pixel_format = surface.texture->desc().pixel_format;
        desc.buffer_count = 2;
        desc.vsync = true;
        desc.premultiplied_alpha =
            surface.alpha == DXGI_ALPHA_MODE_PREMULTIPLIED;
        std::unique_ptr<graphics::BackendSwapChain> created =
            backend->create_swap_chain(desc);
        if (!created || !created->valid()) return false;
        surface.presenter = std::move(created);
        surface.presenter_hwnd = hwnd;
        surface.presenter_view = view;
    }
    if (!surface.presenter->configure_composition_layer(layer)) return false;
    return surface.presenter->present_texture(surface.texture.get(), 0, 0);
}

namespace {

[[nodiscard]] bool read_i32x4(const GuestMemory& memory, GuestAddr address,
                              i32* values) {
    std::array<u8, 16> bytes{};
    if (address == 0 ||
        memory.read_bytes(address, bytes.data(), bytes.size()).is_err()) {
        return false;
    }
    std::memcpy(values, bytes.data(), bytes.size());
    return true;
}

[[nodiscard]] bool write_i32x2(GuestMemory& memory, GuestAddr address,
                               i32 x, i32 y) {
    const i32 point[2] = {x, y};
    return address != 0 &&
           memory.write_bytes(address, reinterpret_cast<const u8*>(point),
                              sizeof(point))
               .is_ok();
}

}  // namespace

bool DcompManager::read_surface_rect(GuestAddr address, IntRect& rect) const {
    i32 values[4] = {};
    if (!read_i32x4(m_memory, address, values)) return false;
    rect = {values[0], values[1], values[2], values[3]};
    return true;
}

bool DcompManager::rect_inside_surface(const Surface& surface,
                                       const IntRect& rect) const {
    if (rect.left < 0 || rect.top < 0 || rect.right < rect.left ||
        rect.bottom < rect.top) {
        return false;
    }
    return static_cast<u64>(rect.right) <= surface.width &&
           static_cast<u64>(rect.bottom) <= surface.height;
}

bool DcompManager::ensure_update_pixels(Surface& surface, u32 bytes) {
    if (bytes == 0) return true;
    const usize need = (static_cast<usize>(bytes) + 0xFFFu) & ~usize{0xFFFu};
    if (surface.pixels != 0 && surface.pixels_mapped >= need) return true;
    if (surface.pixels != 0) {
        (void)m_memory.unmap_region(surface.pixels, surface.pixels_mapped);
        surface.pixels = 0;
        surface.pixels_mapped = 0;
    }
    const GuestAddr pixels = m_memory.map_first_fit(
        m_is_64bit ? 0x20000000ull : 0x12000000ull, need,
        MemoryProtection::ReadWrite);
    if (pixels == 0) return false;
    surface.pixels = pixels;
    surface.pixels_mapped = need;
    return true;
}

void DcompManager::blit_realized(Surface& surface, i32 x, i32 y, u32 width,
                                 u32 height, const u8* source,
                                 u32 source_pitch) {
    if (width == 0 || height == 0 || surface.bpp == 0 || source == nullptr) {
        return;
    }
    const u32 bpp = surface.bpp;
    std::vector<Surface::RealizedRect> next;
    next.reserve(surface.realized.size() + 1);
    const i32 hole_r = x + static_cast<i32>(width);
    const i32 hole_b = y + static_cast<i32>(height);
    for (auto& span : surface.realized) {
        const i32 sl = span.x;
        const i32 st = span.y;
        const i32 sr = span.x + static_cast<i32>(span.width);
        const i32 sb = span.y + static_cast<i32>(span.height);
        const i32 il = std::max(sl, x);
        const i32 it = std::max(st, y);
        const i32 ir = std::min(sr, hole_r);
        const i32 ib = std::min(sb, hole_b);
        if (il >= ir || it >= ib) {
            next.push_back(std::move(span));
            continue;
        }
        const auto emit = [&](i32 px, i32 py, i32 rw, i32 rh) {
            if (rw <= 0 || rh <= 0) return;
            Surface::RealizedRect piece;
            piece.x = px;
            piece.y = py;
            piece.width = static_cast<u32>(rw);
            piece.height = static_cast<u32>(rh);
            piece.pixels.resize(static_cast<usize>(rw) * static_cast<usize>(rh) *
                                bpp);
            for (i32 row = 0; row < rh; ++row) {
                const usize src_off =
                    (static_cast<usize>(py - st + row) * span.width +
                     static_cast<usize>(px - sl)) *
                    bpp;
                const usize dst_off = static_cast<usize>(row) *
                                      static_cast<usize>(rw) * bpp;
                std::memcpy(piece.pixels.data() + dst_off,
                            span.pixels.data() + src_off,
                            static_cast<usize>(rw) * bpp);
            }
            next.push_back(std::move(piece));
        };
        emit(sl, st, sr - sl, it - st);
        emit(sl, ib, sr - sl, sb - ib);
        emit(sl, it, il - sl, ib - it);
        emit(ir, it, sr - ir, ib - it);
    }
    Surface::RealizedRect piece;
    piece.x = x;
    piece.y = y;
    piece.width = width;
    piece.height = height;
    piece.pixels.resize(static_cast<usize>(width) * height * bpp);
    for (u32 row = 0; row < height; ++row) {
        std::memcpy(piece.pixels.data() + static_cast<usize>(row) * width * bpp,
                    source + static_cast<usize>(row) * source_pitch,
                    static_cast<usize>(width) * bpp);
    }
    next.push_back(std::move(piece));
    surface.realized = std::move(next);
}

void DcompManager::retain_realized(Surface& surface,
                                   const std::vector<IntRect>& keep) {
    std::vector<Surface::RealizedRect> snapshot = std::move(surface.realized);
    surface.realized.clear();
    const u32 bpp = surface.bpp;
    if (bpp == 0) return;
    for (const auto& span : snapshot) {
        const i32 sl = span.x;
        const i32 st = span.y;
        const i32 sr = span.x + static_cast<i32>(span.width);
        const i32 sb = span.y + static_cast<i32>(span.height);
        for (const IntRect& rect : keep) {
            const i32 left = std::max(sl, rect.left);
            const i32 top = std::max(st, rect.top);
            const i32 right = std::min(sr, rect.right);
            const i32 bottom = std::min(sb, rect.bottom);
            if (left >= right || top >= bottom) continue;
            const u32 w = static_cast<u32>(right - left);
            const u32 h = static_cast<u32>(bottom - top);
            std::vector<u8> piece(static_cast<usize>(w) * h * bpp);
            for (u32 row = 0; row < h; ++row) {
                const usize src =
                    (static_cast<usize>(top - st + static_cast<i32>(row)) *
                         span.width +
                     static_cast<usize>(left - sl)) *
                    bpp;
                std::memcpy(piece.data() + static_cast<usize>(row) * w * bpp,
                            span.pixels.data() + src,
                            static_cast<usize>(w) * bpp);
            }
            blit_realized(surface, left, top, w, h, piece.data(), w * bpp);
        }
    }
}

bool DcompManager::compose_scroll(const Surface& surface, const IntRect* scroll,
                                  const IntRect* clip, i32 dx, i32 dy,
                                  IntRect& source, IntRect& destination) const {
    if (surface.width > 0x7FFFFFFFu || surface.height > 0x7FFFFFFFu) {
        return false;
    }
    const IntRect whole{0, 0, static_cast<i32>(surface.width),
                        static_cast<i32>(surface.height)};
    const IntRect request = scroll != nullptr ? *scroll : whole;
    // Learn Scroll: scrollRect must lie inside the surface. NULL scrolls
    // the whole surface. clipRect is not required to.
    if (!rect_inside_surface(surface, request)) return false;
    source = request;
    if (clip != nullptr) {
        source.left = std::max(source.left, clip->left);
        source.top = std::max(source.top, clip->top);
        source.right = std::min(source.right, clip->right);
        source.bottom = std::min(source.bottom, clip->bottom);
    }
    destination = {};
    if (source.right <= source.left || source.bottom <= source.top) {
        return true;
    }
    // Source bits are scrollRect ∩ clipRect. The destination is that
    // rectangle shifted by the offset, then clipped again, and it cannot
    // extend outside the surface.
    i64 left = static_cast<i64>(source.left) + dx;
    i64 top = static_cast<i64>(source.top) + dy;
    i64 right = static_cast<i64>(source.right) + dx;
    i64 bottom = static_cast<i64>(source.bottom) + dy;
    if (clip != nullptr) {
        left = std::max(left, static_cast<i64>(clip->left));
        top = std::max(top, static_cast<i64>(clip->top));
        right = std::min(right, static_cast<i64>(clip->right));
        bottom = std::min(bottom, static_cast<i64>(clip->bottom));
    }
    left = std::max(left, static_cast<i64>(0));
    top = std::max(top, static_cast<i64>(0));
    right = std::min(right, static_cast<i64>(surface.width));
    bottom = std::min(bottom, static_cast<i64>(surface.height));
    if (right <= left || bottom <= top) return true;
    destination = {static_cast<i32>(left), static_cast<i32>(top),
                   static_cast<i32>(right), static_cast<i32>(bottom)};
    return true;
}

void DcompManager::scroll_published(Surface& surface, IntRect source,
                                    IntRect destination, i32 dx, i32 dy) {
    if (surface.published.empty() || surface.bpp == 0 || surface.width == 0 ||
        surface.height == 0) {
        return;
    }
    std::vector<u8> next = surface.published;
    std::vector<u8> written(static_cast<usize>(surface.width) * surface.height,
                            0);
    const u32 bpp = surface.bpp;
    const u32 pitch = surface.row_pitch;
    for (i32 y = destination.top; y < destination.bottom; ++y) {
        for (i32 x = destination.left; x < destination.right; ++x) {
            const i64 sx = static_cast<i64>(x) - dx;
            const i64 sy = static_cast<i64>(y) - dy;
            if (sx < source.left || sy < source.top || sx >= source.right ||
                sy >= source.bottom || sx < 0 || sy < 0) {
                continue;
            }
            const auto usx = static_cast<u32>(sx);
            const auto usy = static_cast<u32>(sy);
            if (usx >= surface.width || usy >= surface.height) continue;
            std::memcpy(next.data() + static_cast<usize>(y) * pitch +
                            static_cast<usize>(x) * bpp,
                        surface.published.data() + static_cast<usize>(usy) * pitch +
                            static_cast<usize>(usx) * bpp,
                        bpp);
            written[static_cast<usize>(y) * surface.width +
                    static_cast<u32>(x)] = 1;
        }
    }
    for (i32 y = source.top; y < source.bottom; ++y) {
        for (i32 x = source.left; x < source.right; ++x) {
            if (x < 0 || y < 0) continue;
            const auto ux = static_cast<u32>(x);
            const auto uy = static_cast<u32>(y);
            if (ux >= surface.width || uy >= surface.height) continue;
            const usize index = static_cast<usize>(uy) * surface.width + ux;
            if (written[index] != 0) continue;
            std::memset(next.data() + static_cast<usize>(uy) * pitch +
                            static_cast<usize>(ux) * bpp,
                        0, bpp);
        }
    }
    surface.published.swap(next);
}

bool DcompManager::scroll_realized(Surface& surface, IntRect source,
                                   IntRect destination, i32 dx, i32 dy) {
    const auto hits = [](const Surface::RealizedRect& span, IntRect rect) {
        if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
        const i32 sr = span.x + static_cast<i32>(span.width);
        const i32 sb = span.y + static_cast<i32>(span.height);
        return span.x < rect.right && sr > rect.left && span.y < rect.bottom &&
               sb > rect.top;
    };
    bool any = false;
    i32 min_x = 0;
    i32 min_y = 0;
    i32 max_x = 0;
    i32 max_y = 0;
    const auto include = [&](i32 left, i32 top, i32 right, i32 bottom) {
        if (right <= left || bottom <= top) return;
        if (!any) {
            min_x = left;
            min_y = top;
            max_x = right;
            max_y = bottom;
            any = true;
            return;
        }
        min_x = std::min(min_x, left);
        min_y = std::min(min_y, top);
        max_x = std::max(max_x, right);
        max_y = std::max(max_y, bottom);
    };
    bool touched = false;
    for (const auto& span : surface.realized) {
        if (!hits(span, source) && !hits(span, destination)) continue;
        touched = true;
        include(span.x, span.y, span.x + static_cast<i32>(span.width),
                span.y + static_cast<i32>(span.height));
    }
    include(destination.left, destination.top, destination.right,
            destination.bottom);
    if (!touched || !any) return true;
    const u64 box_w = static_cast<u64>(max_x - min_x);
    const u64 box_h = static_cast<u64>(max_y - min_y);
    if (surface.bpp == 0 || box_w == 0 || box_h == 0) return true;
    const u64 pixel_limit = kRealizedByteLimit / surface.bpp;
    if (box_h > pixel_limit || box_w > pixel_limit / box_h) return false;

    const u32 bw = static_cast<u32>(box_w);
    const u32 bh = static_cast<u32>(box_h);
    const u32 bpp = surface.bpp;
    const auto index_of = [&](i32 x, i32 y) {
        return static_cast<usize>(y - min_y) * bw +
               static_cast<usize>(x - min_x);
    };
    std::vector<u8> pixels(static_cast<usize>(bw) * bh * bpp, 0);
    std::vector<u8> mask(static_cast<usize>(bw) * bh, 0);
    std::vector<Surface::RealizedRect> keep;
    for (auto& span : surface.realized) {
        if (!hits(span, source) && !hits(span, destination)) {
            keep.push_back(std::move(span));
            continue;
        }
        for (u32 row = 0; row < span.height; ++row) {
            for (u32 col = 0; col < span.width; ++col) {
                const i32 x = span.x + static_cast<i32>(col);
                const i32 y = span.y + static_cast<i32>(row);
                const usize at = index_of(x, y);
                std::memcpy(pixels.data() + at * bpp,
                            span.pixels.data() +
                                (static_cast<usize>(row) * span.width + col) * bpp,
                            bpp);
                mask[at] = 1;
            }
        }
    }
    std::vector<u8> out_pixels = pixels;
    std::vector<u8> out_mask = mask;
    std::vector<u8> written(mask.size(), 0);
    for (i32 y = destination.top; y < destination.bottom; ++y) {
        for (i32 x = destination.left; x < destination.right; ++x) {
            const i64 sx = static_cast<i64>(x) - dx;
            const i64 sy = static_cast<i64>(y) - dy;
            if (sx < source.left || sy < source.top || sx >= source.right ||
                sy >= source.bottom) {
                continue;
            }
            if (sx < min_x || sy < min_y || sx >= max_x || sy >= max_y ||
                x < min_x || y < min_y || x >= max_x || y >= max_y) {
                continue;
            }
            const usize from = index_of(static_cast<i32>(sx),
                                        static_cast<i32>(sy));
            if (mask[from] == 0) continue;
            const usize to = index_of(x, y);
            std::memcpy(out_pixels.data() + to * bpp, pixels.data() + from * bpp,
                        bpp);
            out_mask[to] = 1;
            written[to] = 1;
        }
    }
    for (i32 y = source.top; y < source.bottom; ++y) {
        for (i32 x = source.left; x < source.right; ++x) {
            if (x < min_x || y < min_y || x >= max_x || y >= max_y) continue;
            const usize at = index_of(x, y);
            if (mask[at] != 0 && written[at] == 0) out_mask[at] = 0;
        }
    }
    for (u32 row = 0; row < bh; ++row) {
        u32 run = 0;
        bool open = false;
        for (u32 col = 0; col <= bw; ++col) {
            const bool on = col < bw && out_mask[static_cast<usize>(row) * bw + col] != 0;
            if (on && !open) {
                run = col;
                open = true;
            } else if (!on && open) {
                Surface::RealizedRect piece;
                piece.x = min_x + static_cast<i32>(run);
                piece.y = min_y + static_cast<i32>(row);
                piece.width = col - run;
                piece.height = 1;
                piece.pixels.resize(static_cast<usize>(piece.width) * bpp);
                std::memcpy(piece.pixels.data(),
                            out_pixels.data() +
                                (static_cast<usize>(row) * bw + run) * bpp,
                            piece.pixels.size());
                keep.push_back(std::move(piece));
                open = false;
            }
        }
    }
    surface.realized = std::move(keep);
    return true;
}

u32 DcompManager::publish_virtual_surface(Surface& surface) {
    if (surface.realized.empty()) {
        surface.texture.reset();
        surface.presenter.reset();
        surface.pieces.clear();
        surface.initialized = false;
        surface.present_blocked = false;
        surface.content_x = 0;
        surface.content_y = 0;
        surface.content_w = 0;
        surface.content_h = 0;
        return dcomp::S_OK;
    }
    i32 min_x = surface.realized.front().x;
    i32 min_y = surface.realized.front().y;
    i32 max_x = min_x;
    i32 max_y = min_y;
    u64 area = 0;
    for (const auto& span : surface.realized) {
        min_x = std::min(min_x, span.x);
        min_y = std::min(min_y, span.y);
        max_x = std::max(max_x, span.x + static_cast<i32>(span.width));
        max_y = std::max(max_y, span.y + static_cast<i32>(span.height));
        area += static_cast<u64>(span.width) * span.height;
    }
    const u64 box_w = static_cast<u64>(max_x - min_x);
    const u64 box_h = static_cast<u64>(max_y - min_y);
    const u64 box_area = box_w * box_h;
    // Learn Trim: pixels outside the kept rectangles are no longer used for
    // texturing. That hole is the absence of a rectangle, not a texel of a
    // bounding box (transparent or otherwise). One covering rectangle is one
    // texture; anything else is one texture per realized rectangle.
    const bool rectangular = box_w != 0 && box_h != 0 && area == box_area;
    const bool fits = rectangular && surface.bpp != 0 &&
                      box_h <= (kRealizedByteLimit / surface.bpp) &&
                      box_w <= (kRealizedByteLimit / surface.bpp) / box_h;
    if (rectangular && !fits) {
        return dcomp::E_OUTOFMEMORY;
    }
    if (!rectangular) {
        if (surface.bpp == 0) return dcomp::E_OUTOFMEMORY;
        graphics::GraphicsBackendDevice* backend = graphics_backend();
        if (backend == nullptr) {
            static bool logged = false;
            unimplemented_once(logged, "IDCompositionVirtualSurface",
                               "no graphics backend can store the realization");
            return dcomp::E_NOTIMPL;
        }
        const u64 pixel_limit = kRealizedByteLimit / surface.bpp;
        std::vector<Surface::PresentedPiece> built;
        built.reserve(surface.realized.size());
        for (const auto& span : surface.realized) {
            if (span.width == 0 || span.height == 0) continue;
            const u64 span_w = span.width;
            const u64 span_h = span.height;
            if (span_h > pixel_limit || span_w > pixel_limit / span_h) {
                return dcomp::E_OUTOFMEMORY;
            }
            const usize need = static_cast<usize>(span_w) *
                               static_cast<usize>(span_h) * surface.bpp;
            if (span.pixels.size() < need) return dcomp::E_OUTOFMEMORY;
            const u32 pitch = span.width * surface.bpp;
            graphics::BackendTexture2DDesc desc{};
            desc.width = span.width;
            desc.height = span.height;
            desc.pixel_format = graphics::pixel_format_from_dxgi(surface.format);
            desc.shader_resource = true;
            std::unique_ptr<graphics::BackendTexture2D> created =
                backend->create_texture_2d(desc, nullptr, 0);
            if (!created || !created->valid() ||
                !created->update(0, span.pixels.data(), pitch)) {
                static bool logged = false;
                unimplemented_once(
                    logged, "IDCompositionVirtualSurface",
                    "the graphics backend cannot store this realization");
                return dcomp::E_NOTIMPL;
            }
            Surface::PresentedPiece piece;
            piece.x = span.x;
            piece.y = span.y;
            piece.width = span.width;
            piece.height = span.height;
            piece.texture = std::move(created);
            built.push_back(std::move(piece));
        }
        surface.pieces = std::move(built);
        surface.texture.reset();
        surface.presenter.reset();
        surface.present_blocked = false;
        surface.initialized = !surface.pieces.empty();
        surface.content_x = min_x;
        surface.content_y = min_y;
        surface.content_w = 0;
        surface.content_h = 0;
        return dcomp::S_OK;
    }
    const u32 bw = static_cast<u32>(box_w);
    const u32 bh = static_cast<u32>(box_h);
    const u32 bpp = surface.bpp;
    const u32 pitch = bw * bpp;
    std::vector<u8> image(static_cast<usize>(pitch) * bh, 0);
    for (const auto& span : surface.realized) {
        for (u32 row = 0; row < span.height; ++row) {
            const usize src = static_cast<usize>(row) * span.width * bpp;
            const usize dst =
                (static_cast<usize>(span.y - min_y) * bw +
                 static_cast<usize>(span.x - min_x)) *
                bpp;
            std::memcpy(image.data() + static_cast<usize>(row) * bw * bpp + dst,
                        span.pixels.data() + src,
                        static_cast<usize>(span.width) * bpp);
        }
    }
    graphics::GraphicsBackendDevice* backend = graphics_backend();
    if (backend == nullptr) {
        static bool logged = false;
        unimplemented_once(logged, "IDCompositionVirtualSurface",
                           "no graphics backend can store the realization");
        return dcomp::E_NOTIMPL;
    }
    const bool same =
        surface.texture && surface.texture->valid() &&
        surface.texture->desc().width == bw &&
        surface.texture->desc().height == bh;
    if (same) {
        if (!surface.texture->update(0, image.data(), pitch)) {
            static bool logged = false;
            unimplemented_once(logged, "IDCompositionVirtualSurface",
                               "the graphics backend refused the pixel upload");
            return dcomp::E_NOTIMPL;
        }
    } else {
        graphics::BackendTexture2DDesc desc{};
        desc.width = bw;
        desc.height = bh;
        desc.pixel_format = graphics::pixel_format_from_dxgi(surface.format);
        desc.shader_resource = true;
        std::unique_ptr<graphics::BackendTexture2D> created =
            backend->create_texture_2d(desc, nullptr, 0);
        if (!created || !created->valid() ||
            !created->update(0, image.data(), pitch)) {
            static bool logged = false;
            unimplemented_once(logged, "IDCompositionVirtualSurface",
                               "the graphics backend cannot store this realization");
            return dcomp::E_NOTIMPL;
        }
        surface.texture = std::move(created);
        surface.presenter.reset();
    }
    surface.pieces.clear();
    surface.content_x = min_x;
    surface.content_y = min_y;
    surface.content_w = bw;
    surface.content_h = bh;
    surface.present_blocked = false;
    surface.initialized = true;
    surface.row_pitch = pitch;
    return dcomp::S_OK;
}

void DcompManager::end_virtual_draw(Marshaler& m, Surface& surface,
                                    Device& device) {
    const u32 update_bytes = surface.update_h * surface.update_pitch;
    std::vector<u8> drawn(update_bytes);
    if (surface.pixels == 0 ||
        m_memory.read_bytes(surface.pixels, drawn.data(), drawn.size()).is_err()) {
        m.set_return(dcomp::E_OUTOFMEMORY);
        return;
    }
    auto saved = surface.realized;
    blit_realized(surface, surface.origin_x, surface.origin_y, surface.update_w,
                  surface.update_h, drawn.data(), surface.update_pitch);
    const u32 hr = publish_virtual_surface(surface);
    if (hr != dcomp::S_OK) {
        surface.realized = std::move(saved);
        m.set_return(hr);
        return;
    }
    surface.draw = SurfaceDraw::Idle;
    surface.mapped = false;
    if (auto update = m_update_objects.find(surface.update_object);
        update != m_update_objects.end()) {
        update->second.live = false;
    }
    surface.update_object = 0;
    if (device.drawing_surface == surface.interface_address) {
        device.drawing_surface = 0;
    }
    m.set_return(dcomp::S_OK);
}

void DcompManager::scroll_surface(Marshaler& m, Surface& surface) {
    // Learn Scroll: only before BeginDraw or after EndDraw. A suspended
    // or resumed surface is still being updated.
    if (surface.draw != SurfaceDraw::Idle) {
        m.set_return(dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
        return;
    }
    IntRect scroll{};
    IntRect clip{};
    const IntRect* scroll_ptr = nullptr;
    const IntRect* clip_ptr = nullptr;
    if (m.arg_ptr(1) != 0) {
        if (!read_surface_rect(m.arg_ptr(1), scroll)) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        scroll_ptr = &scroll;
    }
    if (m.arg_ptr(2) != 0) {
        if (!read_surface_rect(m.arg_ptr(2), clip)) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        clip_ptr = &clip;
    }
    const i32 dx = static_cast<i32>(m.arg(3));
    const i32 dy = static_cast<i32>(m.arg(4));
    IntRect source{};
    IntRect destination{};
    if (!compose_scroll(surface, scroll_ptr, clip_ptr, dx, dy, source,
                        destination)) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    if (surface.virtual_surface) {
        auto saved = surface.realized;
        if (!scroll_realized(surface, source, destination, dx, dy)) {
            m.set_return(dcomp::E_OUTOFMEMORY);
            return;
        }
        const u32 hr = publish_virtual_surface(surface);
        if (hr != dcomp::S_OK) {
            surface.realized = std::move(saved);
            m.set_return(hr);
            return;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    auto saved = surface.published;
    scroll_published(surface, source, destination, dx, dy);
    if (surface.initialized && surface.texture) {
        if (!surface.texture->update(0, surface.published.data(),
                                     surface.row_pitch)) {
            surface.published = std::move(saved);
            static bool logged = false;
            unimplemented_once(logged, "IDCompositionSurface::Scroll",
                               "the graphics backend refused the pixel upload");
            m.set_return(dcomp::E_NOTIMPL);
            return;
        }
    }
    m.set_return(dcomp::S_OK);
}

void DcompManager::resize_virtual_surface(Marshaler& m, Surface& surface) {
    const u32 width = static_cast<u32>(m.arg(1));
    const u32 height = static_cast<u32>(m.arg(2));
    if (width > kVirtualSurfaceMaxEdge || height > kVirtualSurfaceMaxEdge) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    const u32 old_width = surface.width;
    const u32 old_height = surface.height;
    auto saved = surface.realized;
    surface.width = width;
    surface.height = height;
    const IntRect bounds{0, 0, static_cast<i32>(width),
                         static_cast<i32>(height)};
    retain_realized(surface, {bounds});
    const u32 hr = publish_virtual_surface(surface);
    if (hr != dcomp::S_OK) {
        surface.realized = std::move(saved);
        surface.width = old_width;
        surface.height = old_height;
        m.set_return(hr);
        return;
    }
    m.set_return(dcomp::S_OK);
}

void DcompManager::trim_virtual_surface(Marshaler& m, Surface& surface) {
    const GuestAddr rectangles = m.arg_ptr(1);
    const u32 count = static_cast<u32>(m.arg(2));
    // Learn Trim: a null rectangle pointer is legal only when count is 0,
    // and count 0 releases every realized pixel. Trim is immediate.
    if (count == 0) {
        auto saved = surface.realized;
        surface.realized.clear();
        const u32 hr = publish_virtual_surface(surface);
        if (hr != dcomp::S_OK) surface.realized = std::move(saved);
        m.set_return(hr);
        return;
    }
    if (rectangles == 0) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    if (static_cast<u64>(count) > kRealizedByteLimit / 16u) {
        m.set_return(dcomp::E_OUTOFMEMORY);
        return;
    }
    std::vector<IntRect> keep(count);
    for (u32 index = 0; index < count; ++index) {
        if (!read_surface_rect(rectangles + static_cast<GuestAddr>(index) * 16u,
                               keep[index])) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
    }
    auto saved = surface.realized;
    retain_realized(surface, keep);
    const u32 hr = publish_virtual_surface(surface);
    if (hr != dcomp::S_OK) surface.realized = std::move(saved);
    m.set_return(hr);
}

bool DcompManager::bind_virtual_surface(
    Surface& surface, GuestAddr hwnd, void* view,
    const graphics::BackendCompositionLayerDesc& layer) {
    if (!surface.initialized || surface.realized.empty()) return true;
    if (surface.present_blocked ||
        (surface.pieces.empty() && !surface.texture)) {
        static bool logged = false;
        unimplemented_once(logged, "IDCompositionVirtualSurface",
                           "non-rectangular realization");
        return false;
    }
    // The texture's origin is the realization, not the logical (0, 0).
    // A clip is in surface space. One texture keeps the clip the layer
    // already carries. Each piece's layer is only that rectangle, so the
    // clip is translated into the piece.
    const auto present_at = [&](graphics::BackendTexture2D* texture,
                                std::unique_ptr<graphics::BackendSwapChain>&
                                    presenter,
                                GuestAddr& bound_hwnd, void*& bound_view,
                                u32 width, u32 height, float origin_x,
                                float origin_y, bool translate_clip) -> bool {
        if (texture == nullptr || width == 0 || height == 0) return false;
        graphics::BackendCompositionLayerDesc placed = layer;
        placed.transform[4] = origin_x * layer.transform[0] +
                              origin_y * layer.transform[2] +
                              layer.transform[4];
        placed.transform[5] = origin_x * layer.transform[1] +
                              origin_y * layer.transform[3] +
                              layer.transform[5];
        if (translate_clip && placed.has_clip_rect) {
            placed.clip_rect[0] -= origin_x;
            placed.clip_rect[1] -= origin_y;
            placed.clip_rect[2] -= origin_x;
            placed.clip_rect[3] -= origin_y;
        }
        if (!presenter || bound_hwnd != hwnd || bound_view != view ||
            presenter->desc().width != width ||
            presenter->desc().height != height) {
            graphics::GraphicsBackendDevice* backend = graphics_backend();
            if (backend == nullptr) return false;
            graphics::BackendSwapChainDesc desc{};
            desc.native_view = view;
            desc.width = width;
            desc.height = height;
            desc.pixel_format = texture->desc().pixel_format;
            desc.buffer_count = 2;
            desc.vsync = true;
            desc.premultiplied_alpha =
                surface.alpha == DXGI_ALPHA_MODE_PREMULTIPLIED;
            // One realized rectangle of a virtual surface. Other rectangles
            // on this HWND own their own layers and must stay visible.
            desc.preserve_sibling_metal_layers = true;
            std::unique_ptr<graphics::BackendSwapChain> created =
                backend->create_swap_chain(desc);
            if (!created || !created->valid()) return false;
            presenter = std::move(created);
            bound_hwnd = hwnd;
            bound_view = view;
        }
        if (!presenter->configure_composition_layer(placed)) return false;
        return presenter->present_texture(texture, 0, 0);
    };
    if (!surface.pieces.empty()) {
        for (auto& piece : surface.pieces) {
            if (!present_at(piece.texture.get(), piece.presenter,
                            piece.presenter_hwnd, piece.presenter_view,
                            piece.width, piece.height,
                            static_cast<float>(piece.x),
                            static_cast<float>(piece.y), true)) {
                return false;
            }
        }
        return true;
    }
    return present_at(surface.texture.get(), surface.presenter,
                      surface.presenter_hwnd, surface.presenter_view,
                      surface.content_w, surface.content_h,
                      static_cast<float>(surface.content_x),
                      static_cast<float>(surface.content_y), false);
}

void DcompManager::impl_SurfaceMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto found = m_surfaces.find(self);
    if (found == m_surfaces.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : dcomp::E_INVALIDARG);
        return;
    }
    Surface& surface = found->second;
    auto device = m_devices.find(surface.device_owner);
    if (device == m_devices.end()) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr output = m.arg_ptr(2);
        if (output == 0) { m.set_return(dcomp::E_POINTER); return; }
        (void)m.write_ptr(output, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_IDCOMPOSITIONSURFACE) &&
             !(surface.virtual_surface &&
               guid_equal(iid, IID_IDCOMPOSITIONVIRTUALSURFACE)))) {
            m.set_return(dcomp::E_NOINTERFACE);
            return;
        }
        ++surface.ref_count;
        if (m.write_ptr(output, self).is_err()) {
            --surface.ref_count;
            m.set_return(dcomp::E_POINTER);
            return;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    case 1: m.set_return(++surface.ref_count); return;
    case 2: m.set_return(release_surface(self)); return;
    case 3: {
        // Learn BeginDraw. The returned DXGI surface is a rectangle of the
        // update's size (composition surface: DirectComposition allocates
        // that rectangle). Its offset is that rectangle's upper left, which
        // is (0, 0) for this allocation. The caller draws update-width by
        // update-height at that offset. ID2D1DeviceContext is E_INVALIDARG
        // unless the composition device was created with a Direct2D device;
        // a D3D11, D3D12 or null rendering device is not one. Any other IID
        // is not a surface we can hand back.
        const GuestAddr rect_address = m.arg_ptr(1);
        const GuestAddr iid_address = m.arg_ptr(2);
        const GuestAddr object_out = m.arg_ptr(3);
        const GuestAddr offset_out = m.arg_ptr(4);
        if (object_out == 0 || offset_out == 0) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        if (iid_address == 0) {
            (void)write_i32x2(m_memory, offset_out, 0, 0);
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        const auto fail = [&](u32 hr) {
            (void)write_i32x2(m_memory, offset_out, 0, 0);
            m.set_return(hr);
        };
        GuidBytes iid{};
        if (!read_guid(iid_address, iid)) {
            fail(dcomp::E_INVALIDARG);
            return;
        }
        if (guid_equal(iid, IID_ID2D1DEVICECONTEXT)) {
            fail(dcomp::E_INVALIDARG);
            return;
        }
        if (!guid_equal(iid, IID_IDXGISURFACE) &&
            !guid_equal(iid, IID_IDXGISURFACE1)) {
            fail(dcomp::E_NOINTERFACE);
            return;
        }
        i32 left = 0;
        i32 top = 0;
        i32 right = static_cast<i32>(surface.width);
        i32 bottom = static_cast<i32>(surface.height);
        if (rect_address != 0) {
            i32 rect[4] = {};
            if (!read_i32x4(m_memory, rect_address, rect)) {
                fail(dcomp::E_INVALIDARG);
                return;
            }
            left = rect[0];
            top = rect[1];
            right = rect[2];
            bottom = rect[3];
        }
        if (left < 0 || top < 0 || right <= left || bottom <= top ||
            static_cast<u32>(right) > surface.width ||
            static_cast<u32>(bottom) > surface.height) {
            fail(dcomp::E_INVALIDARG);
            return;
        }
        const bool full = left == 0 && top == 0 &&
                          static_cast<u32>(right) == surface.width &&
                          static_cast<u32>(bottom) == surface.height;
        // Learn BeginDraw: the first update of a non-virtual surface must
        // cover it. A virtual surface may start with any sub-rectangle.
        if (!surface.virtual_surface && !surface.has_drawn && !full) {
            fail(dcomp::E_INVALIDARG);
            return;
        }
        if (surface.draw != SurfaceDraw::Idle ||
            (device->second.drawing_surface != 0 &&
             device->second.drawing_surface != self)) {
            fail(dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
            return;
        }
        if (!ensure_storage() ||
            m_next_object + 0x40 > m_region_base + STORAGE_SIZE) {
            fail(dcomp::E_OUTOFMEMORY);
            return;
        }
        const u32 update_w = static_cast<u32>(right - left);
        const u32 update_h = static_cast<u32>(bottom - top);
        if (surface.virtual_surface &&
            (surface.bpp == 0 || update_w > (0xFFFFFFFFu / surface.bpp))) {
            fail(dcomp::E_OUTOFMEMORY);
            return;
        }
        const u32 update_pitch = update_w * surface.bpp;
        if (surface.virtual_surface && update_h != 0 &&
            update_pitch > (0xFFFFFFFFu / update_h)) {
            fail(dcomp::E_OUTOFMEMORY);
            return;
        }
        const u32 update_bytes = update_h * update_pitch;
        if (surface.virtual_surface &&
            (static_cast<u64>(update_bytes) > kRealizedByteLimit ||
             !ensure_update_pixels(surface, update_bytes))) {
            fail(dcomp::E_OUTOFMEMORY);
            return;
        }
        std::array<u8, 4096> zeros{};
        for (u32 done = 0; done < update_bytes;) {
            const u32 chunk = std::min<u32>(static_cast<u32>(zeros.size()),
                                            update_bytes - done);
            if (m_memory.write_bytes(surface.pixels + done, zeros.data(),
                                    chunk).is_err()) {
                fail(dcomp::E_OUTOFMEMORY);
                return;
            }
            done += chunk;
        }
        const GuestAddr update = m_next_object;
        m_next_object += 0x40;
        if (!write_pointer(update, m_update_vtable)) {
            fail(dcomp::E_OUTOFMEMORY);
            return;
        }
        UpdateObject object{};
        object.interface_address = update;
        object.surface = self;
        object.width = update_w;
        object.height = update_h;
        object.format = surface.format;
        m_update_objects.emplace(update, object);
        surface.draw = SurfaceDraw::Drawing;
        surface.mapped = false;
        surface.origin_x = left;
        surface.origin_y = top;
        surface.update_w = update_w;
        surface.update_h = update_h;
        surface.update_pitch = update_pitch;
        surface.update_object = update;
        device->second.drawing_surface = self;
        if (m.write_ptr(object_out, update).is_err() ||
            !write_i32x2(m_memory, offset_out, 0, 0)) {
            surface.draw = SurfaceDraw::Idle;
            surface.update_object = 0;
            device->second.drawing_surface = 0;
            m_update_objects.erase(update);
            fail(dcomp::E_POINTER);
            return;
        }
        surface.has_drawn = true;
        m.set_return(dcomp::S_OK);
        return;
    }
    case 4: {
        // EndDraw completes a Drawing or Suspended update (Learn: EndDraw
        // on a suspended surface is an implied resume/end) and publishes
        // that rectangle. Commit before this keeps the previous contents.
        if (surface.draw == SurfaceDraw::Idle) {
            m.set_return(dcomp::DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED);
            return;
        }
        if (surface.virtual_surface) {
            end_virtual_draw(m, surface, device->second);
            return;
        }
        const u32 update_bytes = surface.update_h * surface.update_pitch;
        std::vector<u8> drawn(update_bytes);
        if (m_memory.read_bytes(surface.pixels, drawn.data(), drawn.size())
                .is_err()) {
            m.set_return(dcomp::E_OUTOFMEMORY);
            return;
        }
        std::vector<u8> published = surface.published;
        for (u32 row = 0; row < surface.update_h; ++row) {
            const usize src = static_cast<usize>(row) * surface.update_pitch;
            const usize dst =
                (static_cast<usize>(surface.origin_y) + row) * surface.row_pitch +
                static_cast<usize>(surface.origin_x) * surface.bpp;
            std::memcpy(published.data() + dst, drawn.data() + src,
                        surface.update_pitch);
        }
        if (!surface.texture ||
            !surface.texture->update(0, published.data(),
                                    surface.row_pitch)) {
            static bool logged = false;
            unimplemented_once(logged, "IDCompositionSurface::EndDraw",
                               "the graphics backend refused the pixel upload");
            m.set_return(dcomp::E_NOTIMPL);
            return;
        }
        surface.published = std::move(published);
        surface.initialized = true;
        surface.draw = SurfaceDraw::Idle;
        surface.mapped = false;
        if (auto update = m_update_objects.find(surface.update_object);
            update != m_update_objects.end()) {
            update->second.live = false;
        }
        surface.update_object = 0;
        if (device->second.drawing_surface == self) {
            device->second.drawing_surface = 0;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    case 5: {
        if (surface.draw == SurfaceDraw::Idle) {
            m.set_return(dcomp::DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED);
            return;
        }
        if (surface.draw == SurfaceDraw::Suspended) {
            m.set_return(dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
            return;
        }
        surface.draw = SurfaceDraw::Suspended;
        if (device->second.drawing_surface == self) {
            device->second.drawing_surface = 0;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    case 6: {
        if (surface.draw == SurfaceDraw::Idle) {
            m.set_return(dcomp::DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED);
            return;
        }
        if (surface.draw == SurfaceDraw::Drawing ||
            (device->second.drawing_surface != 0 &&
             device->second.drawing_surface != self)) {
            m.set_return(dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
            return;
        }
        surface.draw = SurfaceDraw::Drawing;
        device->second.drawing_surface = self;
        m.set_return(dcomp::S_OK);
        return;
    }
    case 7:
        scroll_surface(m, surface);
        return;
    case 8:
    case 9:
        // Resize and Trim belong to IDCompositionVirtualSurface.
        if (!surface.virtual_surface) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        if (surface.draw != SurfaceDraw::Idle) {
            m.set_return(dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
            return;
        }
        if (slot == 8) resize_virtual_surface(m, surface);
        else trim_virtual_surface(m, surface);
        return;
    default: m.set_return(dcomp::E_NOTIMPL); return;
    }
}

void DcompManager::impl_UpdateObjectMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto found = m_update_objects.find(self);
    if (found == m_update_objects.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : dcomp::E_INVALIDARG);
        return;
    }
    UpdateObject& object = found->second;
    auto surface_entry = m_surfaces.find(object.surface);
    Surface* surface = surface_entry == m_surfaces.end()
                           ? nullptr
                           : &surface_entry->second;
    switch (slot) {
    case 0: {
        const GuestAddr output = m.arg_ptr(2);
        if (output == 0) { m.set_return(dcomp::E_POINTER); return; }
        (void)m.write_ptr(output, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_IDXGISURFACE) &&
             !guid_equal(iid, IID_IDXGISURFACE1))) {
            m.set_return(dcomp::E_NOINTERFACE);
            return;
        }
        ++object.ref_count;
        if (m.write_ptr(output, self).is_err()) {
            --object.ref_count;
            m.set_return(dcomp::E_POINTER);
            return;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    case 1: m.set_return(++object.ref_count); return;
    case 2: {
        if (object.ref_count == 0) { m.set_return(0); return; }
        const u32 count = --object.ref_count;
        if (count == 0) {
            if (surface != nullptr && surface->update_object == self) {
                surface->mapped = false;
                surface->update_object = 0;
            }
            m_update_objects.erase(found);
        }
        m.set_return(count);
        return;
    }
    case 3:
    case 4:
    case 5: {
        static bool logged = false;
        unimplemented_once(logged, "IDXGISurface::SetPrivateData",
                           "private data on a composition update surface");
        m.set_return(dcomp::E_NOTIMPL);
        return;
    }
    case 6:
    case 7: {
        const GuestAddr output = m.arg_ptr(2);
        if (output == 0) { m.set_return(dcomp::E_POINTER); return; }
        (void)m.write_ptr(output, 0);
        if (surface == nullptr) {
            m.set_return(dcomp::E_NOINTERFACE);
            return;
        }
        const GuestAddr device =
            m_devices.contains(surface->device_owner)
                ? m_devices.at(surface->device_owner).rendering_device
                : 0;
        GuidBytes iid{};
        if (device == 0 || !read_guid(m.arg_ptr(1), iid) ||
            !rendering_device_answers(device, iid)) {
            m.set_return(dcomp::E_NOINTERFACE);
            return;
        }
        if (!retain_rendering_device(device)) {
            m.set_return(dcomp::E_NOINTERFACE);
            return;
        }
        if (m.write_ptr(output, device).is_err()) {
            release_rendering_device(device);
            (void)m.write_ptr(output, 0);
            m.set_return(dcomp::E_POINTER);
            return;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    case 8: {
        const GuestAddr output = m.arg_ptr(1);
        if (output == 0) { m.set_return(dcomp::E_INVALIDARG); return; }
        const std::array<u32, 5> words{object.width, object.height,
                                       object.format, 1u, 0u};
        if (m_memory.write_bytes(output,
                                reinterpret_cast<const u8*>(words.data()),
                                sizeof(words)).is_err()) {
            m.set_return(dcomp::E_POINTER);
            return;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    case 9: {
        const GuestAddr rect = m.arg_ptr(1);
        const u32 flags = static_cast<u32>(m.arg(2));
        const u32 known = DXGI_MAP_READ | DXGI_MAP_WRITE | DXGI_MAP_DISCARD;
        const bool access = (flags & (DXGI_MAP_READ | DXGI_MAP_WRITE)) != 0;
        const bool discard_alone =
            (flags & DXGI_MAP_DISCARD) != 0 && (flags & DXGI_MAP_WRITE) == 0;
        if (!object.live || surface == nullptr ||
            surface->update_object != self) {
            m.set_return(dcomp::DXGI_ERROR_INVALID_CALL);
            return;
        }
        if (rect == 0 || (flags & ~known) != 0 || !access || discard_alone ||
            surface->mapped) {
            m.set_return(surface->mapped ? dcomp::DXGI_ERROR_INVALID_CALL
                                        : dcomp::E_INVALIDARG);
            return;
        }
        if (m.write_u32(rect, surface->update_pitch).is_err() ||
            !write_pointer(rect + (m_is_64bit ? 8u : 4u), surface->pixels)) {
            m.set_return(dcomp::E_POINTER);
            return;
        }
        surface->mapped = true;
        m.set_return(dcomp::S_OK);
        return;
    }
    case 10: {
        if (!object.live || surface == nullptr || !surface->mapped ||
            surface->update_object != self) {
            m.set_return(dcomp::DXGI_ERROR_INVALID_CALL);
            return;
        }
        surface->mapped = false;
        m.set_return(dcomp::S_OK);
        return;
    }
    case 11:
    case 12:
        // Not a GDI-compatible surface. Leave the HDC output untouched.
        m.set_return(dcomp::DXGI_ERROR_INVALID_CALL);
        return;
    default: m.set_return(dcomp::E_NOTIMPL); return;
    }
}

void DcompManager::unsupported_device_method(Marshaler& m,
                                              DeviceInterface interface,
                                              usize slot) {
    usize output_arg = NO_OUTPUT;
    if (interface == DeviceInterface::Legacy) {
        if (slot == 8 || slot == 9) output_arg = 5;
        else if (slot == 10 || slot == 11) output_arg = 2;
        else if (slot >= 12 && slot <= 16) output_arg = 1;
        else if (slot == 17 || slot == 22) output_arg = 3;
        else if (slot >= 18 && slot <= 25) output_arg = 1;
    } else {
        if (slot == 7) output_arg = 2;
        else if (slot == 8 || slot == 9) output_arg = 5;
        else if (slot >= 10 && slot <= 14) output_arg = 1;
        else if (slot == 15 || slot == 20) output_arg = 3;
        else if (slot >= 16 && slot <= 23) output_arg = 1;
        else if (interface == DeviceInterface::Desktop &&
                 (slot == 25 || slot == 26)) output_arg = 2;
        else if (interface == DeviceInterface::Device3 && slot >= 24)
            output_arg = 1;
    }
    if (output_arg != NO_OUTPUT) {
        const GuestAddr output = m.arg_ptr(output_arg);
        if (output != 0) (void)m.write_ptr(output, 0);
    }
    dcomp_diag("device method kind=%u slot=%zu -> E_NOTIMPL",
               static_cast<unsigned>(interface), slot);
    m.set_return(dcomp::E_NOTIMPL);
}

void DcompManager::impl_DeviceMethod(Marshaler& m,
                                     DeviceInterface interface,
                                     usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    const auto interface_entry = m_device_interfaces.find(self);
    if (interface_entry == m_device_interfaces.end() ||
        interface_entry->second.kind != interface) {
        m.set_return(slot == 1 || slot == 2 ? 0 : dcomp::E_INVALIDARG);
        return;
    }
    const GuestAddr owner = interface_entry->second.owner;
    auto device = m_devices.find(owner);
    if (device == m_devices.end()) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: m.set_return(query_device_interface(m, self)); return;
    case 1: m.set_return(++device->second.ref_count); return;
    case 2: m.set_return(release_device(self)); return;
    case 3: {
        for (const auto& [target_address, target] : m_targets) {
            (void)target_address;
            if (target.device_owner != owner || target.root == 0) continue;
            std::vector<GuestAddr> visiting;
            if (!bind_visual_tree(target.root, target.hwnd, target.topmost,
                                  visiting)) {
                dcomp_diag("Commit device=0x%llx target=0x%llx -> E_NOTIMPL",
                           static_cast<unsigned long long>(self),
                           static_cast<unsigned long long>(target.interface_address));
                m.set_return(dcomp::E_NOTIMPL);
                return;
            }
        }
        ++device->second.commit_id;
        dcomp_diag("Commit device=0x%llx id=%llu -> S_OK",
                   static_cast<unsigned long long>(self),
                   static_cast<unsigned long long>(device->second.commit_id));
        m.set_return(dcomp::S_OK);
        return;
    }
    case 4:
        // Vitreo applies supported transactions synchronously in Commit.
        m.set_return(dcomp::S_OK);
        return;
    case 5:
        unsupported_device_method(m, interface, slot);
        return;
    default: break;
    }

    const bool legacy = interface == DeviceInterface::Legacy;
    const usize visual_slot = legacy ? 7 : 6;
    if (slot == visual_slot) {
        const GuestAddr output = m.arg_ptr(1);
        if (output == 0) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        (void)m.write_ptr(output, 0);
        const GuestAddr visual = create_visual(owner);
        if (visual == 0 || m.write_ptr(output, visual).is_err()) {
            if (visual != 0) (void)release_visual(visual);
            m.set_return(dcomp::E_POINTER);
            return;
        }
        dcomp_diag("CreateVisual device=0x%llx -> 0x%llx",
                   static_cast<unsigned long long>(self),
                   static_cast<unsigned long long>(visual));
        m.set_return(dcomp::S_OK);
        return;
    }
    const bool target_slot =
        (legacy && slot == 6) ||
        (interface == DeviceInterface::Desktop && slot == 24);
    if (target_slot) {
        const GuestAddr hwnd = m.arg_ptr(1);
        const bool topmost = m.arg(2) != 0;
        const GuestAddr output = m.arg_ptr(3);
        if (output == 0 || hwnd == 0) {
            if (output != 0) (void)m.write_ptr(output, 0);
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        (void)m.write_ptr(output, 0);
        const WindowOwnership ownership = m_window_ownership_resolver
            ? m_window_ownership_resolver(hwnd)
            : WindowOwnership::Invalid;
        if (ownership == WindowOwnership::OtherProcess) {
            m.set_return(dcomp::DCOMPOSITION_ERROR_ACCESS_DENIED);
            return;
        }
        if (ownership != WindowOwnership::CurrentProcess) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        const u64 layer = (static_cast<u64>(hwnd) << 1) |
                          static_cast<u64>(topmost);
        if (m_target_layers.contains(layer)) {
            m.set_return(
                dcomp::DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED);
            return;
        }
        const GuestAddr target = create_target(owner, hwnd, topmost);
        if (target == 0 || m.write_ptr(output, target).is_err()) {
            if (target != 0) (void)release_target(target);
            m.set_return(dcomp::E_POINTER);
            return;
        }
        dcomp_diag("CreateTargetForHwnd hwnd=0x%llx topmost=%u -> 0x%llx",
                   static_cast<unsigned long long>(hwnd), topmost,
                   static_cast<unsigned long long>(target));
        m.set_return(dcomp::S_OK);
        return;
    }
    if (legacy && slot == 26) {
        const GuestAddr state = m.arg_ptr(1);
        if (state == 0 || m.write_u32(state, 1).is_err()) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        m.set_return(dcomp::S_OK);
        return;
    }

    const usize translate_slot = legacy ? 12 : 10;
    if (slot >= translate_slot && slot <= translate_slot + 4) {
        const GuestAddr output = m.arg_ptr(1);
        if (output == 0) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        (void)m.write_ptr(output, 0);
        const std::array<TransformKind, 5> kinds{
            TransformKind::Translate, TransformKind::Scale,
            TransformKind::Rotate, TransformKind::Skew,
            TransformKind::Matrix};
        const GuestAddr transform =
            create_transform(owner, kinds[slot - translate_slot]);
        if (transform == 0 || m.write_ptr(output, transform).is_err()) {
            if (transform != 0) (void)release_transform(transform);
            m.set_return(dcomp::E_POINTER);
            return;
        }
        dcomp_diag("CreateTransform kind=%zu device=0x%llx -> 0x%llx",
                   slot - translate_slot,
                   static_cast<unsigned long long>(self),
                   static_cast<unsigned long long>(transform));
        m.set_return(dcomp::S_OK);
        return;
    }
    const usize group_slot = legacy ? 17 : 15;
    if (slot == group_slot) {
        const GuestAddr array = m.arg_ptr(1);
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr output = m.arg_ptr(3);
        if (array == 0 || count == 0 || count > 4096 || output == 0) {
            if (output != 0) (void)m.write_ptr(output, 0);
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        (void)m.write_ptr(output, 0);
        std::vector<GuestAddr> members;
        members.reserve(count);
        const usize pointer_size = m_is_64bit ? 8 : 4;
        for (u32 index = 0; index < count; ++index) {
            GuestAddr member = 0;
            if (m_is_64bit) {
                const auto value = m_memory.read_u64(array + index * pointer_size);
                if (value.is_err()) { m.set_return(dcomp::E_INVALIDARG); return; }
                member = value.value();
            } else {
                const auto value = m_memory.read_u32(array + index * pointer_size);
                if (value.is_err()) { m.set_return(dcomp::E_INVALIDARG); return; }
                member = value.value();
            }
            const auto transform = m_transforms.find(member);
            if (transform == m_transforms.end() ||
                transform->second.device_owner != owner) {
                m.set_return(dcomp::E_INVALIDARG);
                return;
            }
            members.push_back(member);
        }
        const GuestAddr group = create_transform_group(owner, members);
        if (group == 0 || m.write_ptr(output, group).is_err()) {
            if (group != 0) (void)release_transform(group);
            m.set_return(dcomp::E_POINTER);
            return;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    const usize clip_slot = legacy ? 24 : 22;
    if (slot == clip_slot) {
        const GuestAddr output = m.arg_ptr(1);
        if (output == 0) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        (void)m.write_ptr(output, 0);
        const GuestAddr clip = create_rectangle_clip(owner);
        if (clip == 0 || m.write_ptr(output, clip).is_err()) {
            if (clip != 0) (void)release_rectangle_clip(clip);
            m.set_return(dcomp::E_POINTER);
            return;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    // dcomp.h: CreateSurface is slot 8 on IDCompositionDevice and on
    // IDCompositionDevice2 (which Device3 and the desktop device inherit).
    // CreateVirtualSurface is the following slot.
    if (slot == 8) {
        create_composition_surface(m, owner, false);
        return;
    }
    if (slot == 9) {
        create_composition_surface(m, owner, true);
        return;
    }
    unsupported_device_method(m, interface, slot);
}

void DcompManager::impl_TargetMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto target = m_targets.find(self);
    if (target == m_targets.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : dcomp::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr output = m.arg_ptr(2);
        if (output == 0) { m.set_return(dcomp::E_POINTER); return; }
        (void)m.write_ptr(output, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_IDCOMPOSITIONTARGET))) {
            m.set_return(dcomp::E_NOINTERFACE);
            return;
        }
        ++target->second.ref_count;
        if (m.write_ptr(output, self).is_err()) {
            --target->second.ref_count;
            m.set_return(dcomp::E_POINTER);
            return;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    case 1: m.set_return(++target->second.ref_count); return;
    case 2: m.set_return(release_target(self)); return;
    case 3: {
        const GuestAddr root = m.arg_ptr(1);
        if (root == target->second.root) {
            m.set_return(dcomp::S_OK);
            return;
        }
        if (root != 0) {
            auto visual = m_visuals.find(root);
            if (visual == m_visuals.end() ||
                visual->second.device_owner != target->second.device_owner ||
                visual->second.parent != 0 ||
                visual->second.root_target != 0) {
                m.set_return(dcomp::E_INVALIDARG);
                return;
            }
            ++visual->second.ref_count;
            visual->second.root_target = self;
        }
        const GuestAddr old_root = target->second.root;
        target->second.root = root;
        if (old_root != 0) {
            auto old = m_visuals.find(old_root);
            if (old != m_visuals.end()) old->second.root_target = 0;
            (void)release_visual(old_root);
        }
        dcomp_diag("SetRoot target=0x%llx root=0x%llx -> S_OK",
                   static_cast<unsigned long long>(self),
                   static_cast<unsigned long long>(root));
        m.set_return(dcomp::S_OK);
        return;
    }
    default: m.set_return(dcomp::E_NOTIMPL); return;
    }
}

void DcompManager::impl_VisualMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto visual = m_visuals.find(self);
    if (visual == m_visuals.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : dcomp::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr output = m.arg_ptr(2);
        if (output == 0) { m.set_return(dcomp::E_POINTER); return; }
        (void)m.write_ptr(output, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_IDCOMPOSITIONVISUAL) &&
             !guid_equal(iid, IID_IDCOMPOSITIONVISUAL2))) {
            m.set_return(dcomp::E_NOINTERFACE);
            return;
        }
        ++visual->second.ref_count;
        if (m.write_ptr(output, self).is_err()) {
            --visual->second.ref_count;
            m.set_return(dcomp::E_POINTER);
            return;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    case 1: m.set_return(++visual->second.ref_count); return;
    case 2: m.set_return(release_visual(self)); return;
    // dcomp.h declares the animation overload before the static scalar
    // overload for both offsets.
    case 4:
    case 6: {
        const float value = scalar_arg(m, 1);
        if (!std::isfinite(value)) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        if (slot == 4) visual->second.offset_x = value;
        else visual->second.offset_y = value;
        m.set_return(dcomp::S_OK);
        return;
    }
    case 3:
    case 5:
        m.set_return(m.arg_ptr(1) == 0 ? dcomp::E_INVALIDARG
                                      : dcomp::E_NOTIMPL);
        return;
    // The object overload precedes the static D2D_MATRIX_3X2_F overload.
    case 8: {
        std::array<float, 6> matrix{};
        if (!read_float_array(m.arg_ptr(1), matrix.data(), matrix.size()) ||
            !std::all_of(matrix.begin(), matrix.end(),
                         [](float value) { return std::isfinite(value); })) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        const GuestAddr old = visual->second.transform;
        visual->second.transform = 0;
        visual->second.matrix = matrix;
        visual->second.has_matrix = true;
        if (old != 0) (void)release_transform(old);
        m.set_return(dcomp::S_OK);
        return;
    }
    case 7: {
        const GuestAddr transform_address = m.arg_ptr(1);
        if (transform_address == visual->second.transform &&
            !visual->second.has_matrix) {
            m.set_return(dcomp::S_OK);
            return;
        }
        if (transform_address != 0) {
            auto transform = m_transforms.find(transform_address);
            if (transform == m_transforms.end() ||
                transform->second.device_owner != visual->second.device_owner) {
                m.set_return(dcomp::E_INVALIDARG);
                return;
            }
            ++transform->second.ref_count;
        }
        const GuestAddr old = visual->second.transform;
        visual->second.transform = transform_address;
        visual->second.has_matrix = false;
        if (old != 0) (void)release_transform(old);
        m.set_return(dcomp::S_OK);
        return;
    }
    case 9: {
        const GuestAddr parent_address = m.arg_ptr(1);
        if (parent_address == visual->second.transform_parent) {
            m.set_return(dcomp::S_OK);
            return;
        }
        if (parent_address != 0) {
            auto parent = m_visuals.find(parent_address);
            if (parent == m_visuals.end() || parent_address == self ||
                parent->second.device_owner != visual->second.device_owner) {
                m.set_return(dcomp::E_INVALIDARG);
                return;
            }
            for (GuestAddr current = parent_address; current != 0;) {
                if (current == self) {
                    m.set_return(dcomp::E_INVALIDARG);
                    return;
                }
                const auto item = m_visuals.find(current);
                current = item == m_visuals.end()
                    ? 0 : item->second.transform_parent;
            }
            ++parent->second.ref_count;
        }
        const GuestAddr old = visual->second.transform_parent;
        visual->second.transform_parent = parent_address;
        if (old != 0) (void)release_visual(old);
        m.set_return(dcomp::S_OK);
        return;
    }
    case 10:
        // No effect objects are published yet; NULL correctly clears Effect.
        m.set_return(m.arg_ptr(1) == 0 ? dcomp::S_OK : dcomp::E_INVALIDARG);
        return;
    // The IDCompositionClip overload precedes the static D2D_RECT_F overload.
    case 14: {
        std::array<float, 4> rect{};
        if (!read_float_array(m.arg_ptr(1), rect.data(), rect.size()) ||
            !std::all_of(rect.begin(), rect.end(),
                         [](float value) { return std::isfinite(value); })) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        for (float& value : rect)
            value = std::clamp(value, -2097152.0f, 2097152.0f);
        const GuestAddr old = visual->second.clip;
        visual->second.clip = 0;
        visual->second.clip_rect = rect;
        visual->second.has_clip_rect = true;
        if (old != 0) (void)release_rectangle_clip(old);
        m.set_return(dcomp::S_OK);
        return;
    }
    case 13: {
        const GuestAddr clip_address = m.arg_ptr(1);
        if (clip_address == visual->second.clip &&
            !visual->second.has_clip_rect) {
            m.set_return(dcomp::S_OK);
            return;
        }
        if (clip_address != 0) {
            auto clip = m_rectangle_clips.find(clip_address);
            if (clip == m_rectangle_clips.end() ||
                clip->second.device_owner != visual->second.device_owner) {
                m.set_return(dcomp::E_INVALIDARG);
                return;
            }
            ++clip->second.ref_count;
        }
        const GuestAddr old = visual->second.clip;
        visual->second.clip = clip_address;
        visual->second.has_clip_rect = false;
        if (old != 0) (void)release_rectangle_clip(old);
        m.set_return(dcomp::S_OK);
        return;
    }
    case 11:
    case 12:
    case 19:
    case 20:
    case 21: {
        const u32 value = static_cast<u32>(m.arg(1));
        const u32 maximum = slot == 19 ? 2u : 1u;
        if (value != 0xFFFFFFFFu && value > maximum) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        if (slot == 11) visual->second.bitmap_interpolation_mode = value;
        else if (slot == 12) visual->second.border_mode = value;
        else if (slot == 19) visual->second.composite_mode = value;
        else if (slot == 20) visual->second.opacity_mode = value;
        else visual->second.back_face_visibility = value;
        m.set_return(dcomp::S_OK);
        return;
    }
    case 15: {
        const GuestAddr content = m.arg_ptr(1);
        const auto surface = m_surfaces.find(content);
        const bool is_surface = content != 0 && surface != m_surfaces.end();
        if (content != 0 && !is_surface && !content_is_swap_chain(content)) {
            dcomp_diag("SetContent visual=0x%llx content=0x%llx -> E_INVALIDARG",
                       static_cast<unsigned long long>(self),
                       static_cast<unsigned long long>(content));
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        if (is_surface &&
            surface->second.device_owner != visual->second.device_owner) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        if (content == visual->second.content) {
            m.set_return(dcomp::S_OK);
            return;
        }
        if (content != 0 && !retain_visual_content(content)) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        const GuestAddr old = visual->second.content;
        visual->second.content = content;
        if (old != 0) release_visual_content(old);
        dcomp_diag("SetContent visual=0x%llx content=0x%llx -> S_OK",
                   static_cast<unsigned long long>(self),
                   static_cast<unsigned long long>(content));
        m.set_return(dcomp::S_OK);
        return;
    }
    case 16: {
        const GuestAddr child_address = m.arg_ptr(1);
        const bool insert_above = m.arg(2) != 0;
        const GuestAddr reference = m.arg_ptr(3);
        auto child = m_visuals.find(child_address);
        if (child == m_visuals.end() || child_address == self ||
            child->second.device_owner != visual->second.device_owner ||
            child->second.root_target != 0 ||
            (child->second.parent != 0 && child->second.parent != self)) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        if (reference != 0 &&
            std::find(visual->second.children.begin(),
                      visual->second.children.end(), reference) ==
                visual->second.children.end()) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        for (GuestAddr ancestor = self; ancestor != 0;) {
            if (ancestor == child_address) {
                m.set_return(dcomp::E_INVALIDARG);
                return;
            }
            const auto current = m_visuals.find(ancestor);
            ancestor = current == m_visuals.end() ? 0 : current->second.parent;
        }
        auto& children = visual->second.children;
        const auto existing = std::find(children.begin(), children.end(),
                                        child_address);
        const bool newly_attached = existing == children.end();
        if (!newly_attached) children.erase(existing);
        auto position = reference == 0
            ? (insert_above ? children.end() : children.begin())
            : std::find(children.begin(), children.end(), reference);
        if (reference != 0 && insert_above && position != children.end())
            ++position;
        children.insert(position, child_address);
        child->second.parent = self;
        if (newly_attached) ++child->second.ref_count;
        dcomp_diag(
            "AddVisual parent=0x%llx child=0x%llx above=%u reference=0x%llx "
            "children=%zu -> S_OK",
            static_cast<unsigned long long>(self),
            static_cast<unsigned long long>(child_address),
            insert_above ? 1u : 0u,
            static_cast<unsigned long long>(reference), children.size());
        m.set_return(dcomp::S_OK);
        return;
    }
    case 17: {
        const GuestAddr child_address = m.arg_ptr(1);
        auto position = std::find(visual->second.children.begin(),
                                  visual->second.children.end(), child_address);
        if (position == visual->second.children.end()) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        visual->second.children.erase(position);
        auto child = m_visuals.find(child_address);
        if (child != m_visuals.end()) child->second.parent = 0;
        (void)release_visual(child_address);
        dcomp_diag("RemoveVisual parent=0x%llx child=0x%llx -> S_OK",
                 static_cast<unsigned long long>(self),
                 static_cast<unsigned long long>(child_address));
        m.set_return(dcomp::S_OK);
        return;
    }
    case 18: {
        auto children = std::move(visual->second.children);
        visual->second.children.clear();
        for (const GuestAddr child_address : children) {
            auto child = m_visuals.find(child_address);
            if (child != m_visuals.end()) child->second.parent = 0;
            (void)release_visual(child_address);
        }
        dcomp_diag("RemoveAllVisuals parent=0x%llx count=%zu -> S_OK",
                   static_cast<unsigned long long>(self), children.size());
        m.set_return(dcomp::S_OK);
        return;
    }
    default: m.set_return(dcomp::E_NOTIMPL); return;
    }
}

void DcompManager::impl_TransformMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto transform = m_transforms.find(self);
    if (transform == m_transforms.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : dcomp::E_INVALIDARG);
        return;
    }
    if (slot == 0) {
        const GuestAddr output = m.arg_ptr(2);
        if (output == 0) { m.set_return(dcomp::E_POINTER); return; }
        (void)m.write_ptr(output, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid)) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        bool supported = guid_equal(iid, IID_IUNKNOWN) ||
            guid_equal(iid, IID_IDCOMPOSITIONEFFECT) ||
            guid_equal(iid, IID_IDCOMPOSITIONTRANSFORM3D) ||
            guid_equal(iid, IID_IDCOMPOSITIONTRANSFORM);
        switch (transform->second.kind) {
        case TransformKind::Translate:
            supported = supported ||
                guid_equal(iid, IID_IDCOMPOSITIONTRANSLATETRANSFORM);
            break;
        case TransformKind::Scale:
            supported = supported ||
                guid_equal(iid, IID_IDCOMPOSITIONSCALETRANSFORM);
            break;
        case TransformKind::Rotate:
            supported = supported ||
                guid_equal(iid, IID_IDCOMPOSITIONROTATETRANSFORM);
            break;
        case TransformKind::Skew:
            supported = supported ||
                guid_equal(iid, IID_IDCOMPOSITIONSKEWTRANSFORM);
            break;
        case TransformKind::Matrix:
            supported = supported ||
                guid_equal(iid, IID_IDCOMPOSITIONMATRIXTRANSFORM);
            break;
        case TransformKind::Group: break;
        }
        if (!supported) { m.set_return(dcomp::E_NOINTERFACE); return; }
        ++transform->second.ref_count;
        if (m.write_ptr(output, self).is_err()) {
            --transform->second.ref_count;
            m.set_return(dcomp::E_POINTER);
            return;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    if (slot == 1) {
        m.set_return(++transform->second.ref_count);
        return;
    }
    if (slot == 2) {
        m.set_return(release_transform(self));
        return;
    }
    if (transform->second.kind == TransformKind::Group) {
        m.set_return(dcomp::E_NOTIMPL);
        return;
    }
    if (transform->second.kind == TransformKind::Matrix) {
        if (slot == 3) {
            std::array<float, 6> matrix{};
            if (!read_float_array(m.arg_ptr(1), matrix.data(), matrix.size()) ||
                !std::all_of(matrix.begin(), matrix.end(),
                             [](float value) { return std::isfinite(value); })) {
                m.set_return(dcomp::E_INVALIDARG);
                return;
            }
            transform->second.values = matrix;
            m.set_return(dcomp::S_OK);
            return;
        }
        if (slot == 4) {
            const u32 row = static_cast<u32>(m.arg(1));
            const u32 column = static_cast<u32>(m.arg(2));
            const float value = scalar_arg(m, 3);
            if (row >= 3 || column >= 2 || !std::isfinite(value)) {
                m.set_return(dcomp::E_INVALIDARG);
                return;
            }
            transform->second.values[row * 2 + column] = value;
            m.set_return(dcomp::S_OK);
            return;
        }
        if (slot == 5) {
            m.set_return(m.arg_ptr(3) == 0 ? dcomp::E_INVALIDARG
                                          : dcomp::E_NOTIMPL);
            return;
        }
        m.set_return(dcomp::E_NOTIMPL);
        return;
    }
    const bool numeric_slot = slot >= 3 && (slot & 1u) != 0;
    if (!numeric_slot) {
        const GuestAddr animation = m.arg_ptr(1);
        m.set_return(animation == 0 ? dcomp::E_INVALIDARG
                                   : dcomp::E_NOTIMPL);
        return;
    }
    const usize property = (slot - 3) / 2;
    usize property_count = 0;
    switch (transform->second.kind) {
    case TransformKind::Translate: property_count = 2; break;
    case TransformKind::Scale: property_count = 4; break;
    case TransformKind::Rotate: property_count = 3; break;
    case TransformKind::Skew: property_count = 4; break;
    default: break;
    }
    const float value = scalar_arg(m, 1);
    if (property >= property_count || !std::isfinite(value)) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    transform->second.values[property] = value;
    m.set_return(dcomp::S_OK);
}

void DcompManager::impl_RectangleClipMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto clip = m_rectangle_clips.find(self);
    if (clip == m_rectangle_clips.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : dcomp::E_INVALIDARG);
        return;
    }
    if (slot == 0) {
        const GuestAddr output = m.arg_ptr(2);
        if (output == 0) { m.set_return(dcomp::E_POINTER); return; }
        (void)m.write_ptr(output, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_IDCOMPOSITIONCLIP) &&
             !guid_equal(iid, IID_IDCOMPOSITIONRECTANGLECLIP))) {
            m.set_return(dcomp::E_NOINTERFACE);
            return;
        }
        ++clip->second.ref_count;
        if (m.write_ptr(output, self).is_err()) {
            --clip->second.ref_count;
            m.set_return(dcomp::E_POINTER);
            return;
        }
        m.set_return(dcomp::S_OK);
        return;
    }
    if (slot == 1) { m.set_return(++clip->second.ref_count); return; }
    if (slot == 2) { m.set_return(release_rectangle_clip(self)); return; }
    if (slot < 3 || slot >= dcomp::RECTANGLE_CLIP_METHOD_COUNT) {
        m.set_return(dcomp::E_NOTIMPL);
        return;
    }
    const usize property = (slot - 3) / 2;
    if ((slot & 1u) == 0) {
        m.set_return(m.arg_ptr(1) == 0 ? dcomp::E_INVALIDARG
                                      : dcomp::E_NOTIMPL);
        return;
    }
    float value = scalar_arg(m, 1);
    if (!std::isfinite(value)) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    value = std::clamp(value, -2097152.0f, 2097152.0f);
    if (property >= 4) value = std::max(0.0f, value);
    clip->second.values[property] = value;
    m.set_return(dcomp::S_OK);
}

DllExports create_dcomp_thunks(DcompManager& manager) {
    DllExports exports;
    exports.entries.reserve(3 + dcomp::DEVICE_METHOD_COUNT +
                            dcomp::DEVICE2_METHOD_COUNT +
                            dcomp::DESKTOP_DEVICE_METHOD_COUNT +
                            dcomp::DEVICE3_METHOD_COUNT +
                            dcomp::TARGET_METHOD_COUNT +
                            dcomp::VISUAL2_METHOD_COUNT +
                            dcomp::TRANSLATE_TRANSFORM_METHOD_COUNT +
                            dcomp::SCALE_TRANSFORM_METHOD_COUNT +
                            dcomp::ROTATE_TRANSFORM_METHOD_COUNT +
                            dcomp::SKEW_TRANSFORM_METHOD_COUNT +
                            dcomp::MATRIX_TRANSFORM_METHOD_COUNT +
                            dcomp::RECTANGLE_CLIP_METHOD_COUNT +
                            dcomp::SURFACE_METHOD_COUNT +
                            dcomp::UPDATE_OBJECT_METHOD_COUNT + 8);
    exports.entries.push_back({"DCompositionCreateDevice", 0,
        [&manager](Marshaler& m) { manager.impl_DCompositionCreateDevice(m); },
        12});
    exports.entries.push_back({"DCompositionCreateDevice2", 0,
        [&manager](Marshaler& m) { manager.impl_DCompositionCreateDevice2(m); },
        12});
    exports.entries.push_back({"DCompositionCreateDevice3", 0,
        [&manager](Marshaler& m) { manager.impl_DCompositionCreateDevice3(m); },
        12});
    exports.entries.push_back({"DCompositionCreateSurfaceHandle", 0,
        [&manager](Marshaler& m) {
            manager.impl_DCompositionCreateSurfaceHandle(m);
        }, 12});

    const auto add_device_methods = [&](const char* family, usize count,
                                        DcompManager::DeviceInterface interface) {
        for (usize slot = 0; slot < count; ++slot) {
            exports.entries.push_back({
                method_export_name(family, slot), 0,
                [&manager, interface, slot](Marshaler& m) {
                    manager.impl_DeviceMethod(m, interface, slot);
                }, static_cast<u16>(device_stdcall_bytes(interface, slot))});
        }
    };
    add_device_methods("Device", dcomp::DEVICE_METHOD_COUNT,
                       DcompManager::DeviceInterface::Legacy);
    add_device_methods("Device2", dcomp::DEVICE2_METHOD_COUNT,
                       DcompManager::DeviceInterface::Device2);
    add_device_methods("DesktopDevice", dcomp::DESKTOP_DEVICE_METHOD_COUNT,
                       DcompManager::DeviceInterface::Desktop);
    add_device_methods("Device3", dcomp::DEVICE3_METHOD_COUNT,
                       DcompManager::DeviceInterface::Device3);
    for (usize slot = 0; slot < dcomp::TARGET_METHOD_COUNT; ++slot) {
        exports.entries.push_back({
            dcomp_target_method_export_name(slot), 0,
            [&manager, slot](Marshaler& m) {
                manager.impl_TargetMethod(m, slot);
            }, static_cast<u16>(TARGET_BYTES[slot])});
    }
    for (usize slot = 0; slot < dcomp::VISUAL2_METHOD_COUNT; ++slot) {
        exports.entries.push_back({
            dcomp_visual_method_export_name(slot), 0,
            [&manager, slot](Marshaler& m) {
                manager.impl_VisualMethod(m, slot);
            }, static_cast<u16>(VISUAL_BYTES[slot])});
    }
    const auto add_transform_methods = [&](const char* family, usize count) {
        for (usize slot = 0; slot < count; ++slot) {
            const u16 bytes = static_cast<u16>(
                slot == 0 ? 12 : (slot == 1 || slot == 2 ? 4 : 8));
            exports.entries.push_back({
                method_export_name(family, slot), 0,
                [&manager, slot](Marshaler& m) {
                    manager.impl_TransformMethod(m, slot);
                }, bytes});
        }
    };
    add_transform_methods("TranslateTransform",
                          dcomp::TRANSLATE_TRANSFORM_METHOD_COUNT);
    add_transform_methods("ScaleTransform",
                          dcomp::SCALE_TRANSFORM_METHOD_COUNT);
    add_transform_methods("RotateTransform",
                          dcomp::ROTATE_TRANSFORM_METHOD_COUNT);
    add_transform_methods("SkewTransform",
                          dcomp::SKEW_TRANSFORM_METHOD_COUNT);
    for (usize slot = 0; slot < dcomp::MATRIX_TRANSFORM_METHOD_COUNT; ++slot) {
        exports.entries.push_back({
            dcomp_matrix_transform_method_export_name(slot), 0,
            [&manager, slot](Marshaler& m) {
                manager.impl_TransformMethod(m, slot);
            }, static_cast<u16>(MATRIX_TRANSFORM_BYTES[slot])});
    }
    for (usize slot = 0; slot < dcomp::RECTANGLE_CLIP_METHOD_COUNT; ++slot) {
        const u16 bytes = static_cast<u16>(
            slot == 0 ? 12 : (slot == 1 || slot == 2 ? 4 : 8));
        exports.entries.push_back({
            dcomp_rectangle_clip_method_export_name(slot), 0,
            [&manager, slot](Marshaler& m) {
                manager.impl_RectangleClipMethod(m, slot);
            }, bytes});
    }
    for (usize slot = 0; slot < dcomp::SURFACE_METHOD_COUNT; ++slot) {
        exports.entries.push_back({
            dcomp_surface_method_export_name(slot), 0,
            [&manager, slot](Marshaler& m) {
                manager.impl_SurfaceMethod(m, slot);
            }, static_cast<u16>(SURFACE_BYTES[slot])});
    }
    for (usize slot = 0; slot < dcomp::UPDATE_OBJECT_METHOD_COUNT; ++slot) {
        exports.entries.push_back({
            dcomp_update_object_method_export_name(slot), 0,
            [&manager, slot](Marshaler& m) {
                manager.impl_UpdateObjectMethod(m, slot);
            }, static_cast<u16>(UPDATE_BYTES[slot])});
    }

    // Publish the remaining documented dcomp.dll surface with exact x86
    // footprints. They fail explicitly until compositor-clock/mouse-routing
    // state exists; callers never fall into a generic zero-cleanup thunk.
    exports.entries.push_back({"DCompositionAttachMouseDragToHwnd", 0,
        [&manager](Marshaler& m) {
            manager.impl_PublicUnsupported(
                m, "DCompositionAttachMouseDragToHwnd");
        }, 12});
    exports.entries.push_back({"DCompositionAttachMouseWheelToHwnd", 0,
        [&manager](Marshaler& m) {
            manager.impl_PublicUnsupported(
                m, "DCompositionAttachMouseWheelToHwnd");
        }, 12});
    exports.entries.push_back({"DCompositionBoostCompositorClock", 0,
        [&manager](Marshaler& m) {
            manager.impl_PublicUnsupported(
                m, "DCompositionBoostCompositorClock");
        }, 4});
    exports.entries.push_back({"DCompositionGetFrameId", 0,
        [&manager](Marshaler& m) {
            manager.impl_PublicUnsupported(m, "DCompositionGetFrameId", 1);
        }, 8});
    exports.entries.push_back({"DCompositionGetStatistics", 0,
        [&manager](Marshaler& m) {
            manager.impl_PublicUnsupported(
                m, "DCompositionGetStatistics");
        }, 28});
    exports.entries.push_back({"DCompositionGetTargetStatistics", 0,
        [&manager](Marshaler& m) {
            manager.impl_PublicUnsupported(
                m, "DCompositionGetTargetStatistics");
        }, 16});
    exports.entries.push_back({"DCompositionWaitForCompositorClock", 0,
        [&manager](Marshaler& m) {
            manager.impl_PublicUnsupported(
                m, "DCompositionWaitForCompositorClock");
        }, 12});
    return exports;
}

} // namespace vitreo::winapi
