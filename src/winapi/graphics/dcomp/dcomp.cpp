// SPDX-License-Identifier: LGPL-2.1-only
#include "dcomp.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
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

DcompManager::DcompManager(GuestMemory& memory, bool is_64bit,
                           D3D11Manager& d3d11_manager)
    : m_memory(memory), m_is_64bit(is_64bit), m_d3d11(d3d11_manager) {}

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
            m_next_object = base + 0x1000;
            break;
        }
        minimum = base + STORAGE_SIZE;
    }
    if (m_region_base == 0) return false;
    const usize pointer_size = m_is_64bit ? 8 : 4;
    const auto write_vtable = [&](GuestAddr address, const auto& thunks) {
        for (usize slot = 0; slot < thunks.size(); ++slot) {
            if (thunks[slot] == 0 ||
                !write_pointer(address + slot * pointer_size,
                               thunks[slot])) {
                return false;
            }
        }
        return true;
    };
    m_storage_ready =
        write_vtable(m_device_vtable, m_method_thunks.device) &&
        write_vtable(m_device2_vtable, m_method_thunks.device2) &&
        write_vtable(m_desktop_vtable, m_method_thunks.desktop_device) &&
        write_vtable(m_device3_vtable, m_method_thunks.device3) &&
        write_vtable(m_target_vtable, m_method_thunks.target) &&
        write_vtable(m_visual_vtable, m_method_thunks.visual2) &&
        write_vtable(m_translate_transform_vtable,
                     m_method_thunks.translate_transform) &&
        write_vtable(m_scale_transform_vtable,
                     m_method_thunks.scale_transform) &&
        write_vtable(m_rotate_transform_vtable,
                     m_method_thunks.rotate_transform) &&
        write_vtable(m_skew_transform_vtable,
                     m_method_thunks.skew_transform) &&
        write_vtable(m_matrix_transform_vtable,
                     m_method_thunks.matrix_transform) &&
        write_vtable(m_rectangle_clip_vtable,
                     m_method_thunks.rectangle_clip);
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
        !m_d3d11.is_device_interface(rendering_device)) {
        dcomp_diag("%s rendering_device=0x%llx -> E_INVALIDARG",
                   api, static_cast<unsigned long long>(rendering_device));
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    if (rendering_device != 0 &&
        !m_d3d11.retain_device_reference(rendering_device)) {
        m.set_return(dcomp::E_INVALIDARG);
        return;
    }
    const GuestAddr device = create_device(rendering_device, iid_interface);
    if (device == 0 || m.write_ptr(output, device).is_err()) {
        if (device != 0) {
            (void)release_device(device);
        } else if (rendering_device != 0) {
            m_d3d11.release_device_interface_reference(rendering_device);
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
        m_d3d11.release_device_interface_reference(rendering_device);
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
    if (dead.content != 0) m_d3d11.release_swap_chain_reference(dead.content);
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
            !m_d3d11.bind_composition_swap_chain(
                visual->second.content, hwnd, layer)) {
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
        if (content != 0 && !m_d3d11.is_swap_chain_object(content)) {
            dcomp_diag("SetContent visual=0x%llx content=0x%llx -> E_INVALIDARG",
                       static_cast<unsigned long long>(self),
                       static_cast<unsigned long long>(content));
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        if (content == visual->second.content) {
            m.set_return(dcomp::S_OK);
            return;
        }
        if (content != 0 && !m_d3d11.retain_swap_chain_reference(content)) {
            m.set_return(dcomp::E_INVALIDARG);
            return;
        }
        const GuestAddr old = visual->second.content;
        visual->second.content = content;
        if (old != 0) m_d3d11.release_swap_chain_reference(old);
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
                            dcomp::RECTANGLE_CLIP_METHOD_COUNT + 8);
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
