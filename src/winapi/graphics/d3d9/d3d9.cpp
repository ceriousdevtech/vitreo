// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9.cpp
/// @brief D3D9 guest COM object model and the IDirect3D9 factory interface.

#include "winapi/graphics/d3d9/d3d9_state.hpp"
#include "winapi/graphics/d3d9/d3d9_frame_dump.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/common/display_device_identity.hpp"

#include <algorithm>
#include <cstdio>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace vitreo::winapi {
namespace {

/// Object memory and vtables live in one region. Each object occupies a fixed
/// slot whose first pointer-sized word is its vtable, which is all a guest may
/// legitimately read from a COM object it did not allocate.
///
/// 1 MiB (~4080 slots) is not enough: one load creates thousands of
/// Texture and Surface objects (one Surface per mip level).
/// 16 MiB gives ~65k slots; released addresses are also recycled so long
/// sessions do not keep bumping forever.
constexpr usize STORAGE_SIZE = 0x1000000;
constexpr usize OBJECT_STRIDE = 0x100;
constexpr usize VTABLE_AREA = 0x1000;
constexpr GuestAddr STORAGE_MIN = 0x62000000;
constexpr GuestAddr STORAGE_MAX = 0x6E000000;

constexpr GuestAddr LOCK_REGION_MIN = 0x50000000;
/// Preferred window ends here (256 MiB). Search continues below the TEB
/// island when this range is full — CreateVertexBuffer must not fail
/// while the process still has user VA (Learn: E_OUTOFMEMORY / D3DERR_).
constexpr GuestAddr LOCK_REGION_PREFERRED_END = 0x60000000;
constexpr GuestAddr LOCK_REGION_SEARCH_END = 0x7F000000;

constexpr u32 D3DADAPTER_IDENTIFIER9_SIZE = 1100;
constexpr u32 D3DENUM_WHQL_LEVEL = 0x2;
constexpr u32 PRIMARY_MONITOR_HANDLE = 0x000D0001;

[[nodiscard]] bool d3d9_texture_lifetime_diag_enabled() noexcept {
    static const bool enabled =
        std::getenv("VITREO_D3D9_TEXTURE_LIFETIME_DIAG") != nullptr;
    return enabled;
}

using GuidBytes = std::array<u8, 16>;

constexpr GuidBytes IID_IUNKNOWN{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
constexpr GuidBytes IID_IDIRECT3D9{
    0xCA, 0xCB, 0xBD, 0x81, 0xD4, 0x64, 0x6D, 0x42,
    0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C};
constexpr GuidBytes IID_IDIRECT3DDEVICE9{
    0x96, 0x3B, 0x22, 0xD0, 0x7A, 0xBF, 0xFD, 0x43,
    0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB};
constexpr GuidBytes IID_IDIRECT3DSWAPCHAIN9{
    0xF2, 0x50, 0x49, 0x79, 0xFC, 0xAD, 0x8A, 0x45,
    0x90, 0x5E, 0x10, 0xA1, 0x0B, 0x0B, 0x50, 0x3B};
constexpr GuidBytes IID_IDIRECT3DRESOURCE9{
    0x5D, 0xC0, 0xEE, 0x05, 0x7D, 0x8F, 0x62, 0x43,
    0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04};
constexpr GuidBytes IID_IDIRECT3DSURFACE9{
    0x3A, 0xAF, 0xFB, 0x0C, 0xF6, 0x9F, 0x9A, 0x42,
    0x99, 0xB3, 0xA2, 0x79, 0x6A, 0xF8, 0xB8, 0x9B};
constexpr GuidBytes IID_IDIRECT3DBASETEXTURE9{
    0x7E, 0xA8, 0x0C, 0x58, 0x3C, 0x1D, 0x54, 0x4D,
    0x99, 0x1D, 0xB7, 0xD3, 0xE3, 0xC2, 0x98, 0xCE};
constexpr GuidBytes IID_IDIRECT3DTEXTURE9{
    0x27, 0x12, 0xC3, 0x85, 0xE5, 0x3D, 0x00, 0x4F,
    0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5};
constexpr GuidBytes IID_IDIRECT3DVERTEXBUFFER9{
    0xB5, 0xB1, 0x4B, 0xB6, 0x70, 0xFD, 0xF6, 0x4D,
    0xBF, 0x91, 0x19, 0xD0, 0xA1, 0x24, 0x55, 0xE3};
constexpr GuidBytes IID_IDIRECT3DINDEXBUFFER9{
    0x5E, 0xD6, 0x9D, 0x7C, 0xF7, 0xD3, 0x29, 0x45,
    0xAC, 0xEE, 0x78, 0x58, 0x30, 0xAC, 0xDE, 0x35};
constexpr GuidBytes IID_IDIRECT3DVERTEXSHADER9{
    0x7E, 0x55, 0xC5, 0xEF, 0x65, 0x62, 0x13, 0x46,
    0x8A, 0x94, 0x43, 0x85, 0x78, 0x89, 0xEB, 0x36};
constexpr GuidBytes IID_IDIRECT3DPIXELSHADER9{
    0xDC, 0xDB, 0x3B, 0x6D, 0x02, 0x5B, 0x15, 0x44,
    0xB8, 0x52, 0xCE, 0x5E, 0x8B, 0xCC, 0xB2, 0x89};
constexpr GuidBytes IID_IDIRECT3DSTATEBLOCK9{
    0xE5, 0x4F, 0x7C, 0xB0, 0x0D, 0x31, 0xA8, 0x4B,
    0xA2, 0x3C, 0x4F, 0x0F, 0x20, 0x6F, 0x21, 0x8B};
constexpr GuidBytes IID_IDIRECT3DVERTEXDECLARATION9{
    0x9C, 0xC5, 0x13, 0xDD, 0xFA, 0x36, 0x98, 0x40,
    0xA8, 0xFB, 0xC7, 0xED, 0x39, 0xDC, 0x85, 0x46};
constexpr GuidBytes IID_IDIRECT3DCUBETEXTURE9{
    0x81, 0x2F, 0xF3, 0xFF, 0x53, 0xD9, 0x3A, 0x47,
    0x92, 0x23, 0x93, 0xD6, 0x52, 0xAB, 0xA9, 0x3F};
constexpr GuidBytes IID_IDIRECT3DQUERY9{
    0x60, 0x14, 0x77, 0xD9, 0x95, 0xA6, 0x26, 0x4F,
    0xBB, 0xD3, 0x27, 0xB8, 0x40, 0xB5, 0x41, 0xCC};
// {2518526C-E789-4111-A7B9-47EF328D13E6}
constexpr GuidBytes IID_IDIRECT3DVOLUMETEXTURE9{
    0x6C, 0x52, 0x18, 0x25, 0x89, 0xE7, 0x11, 0x41,
    0xA7, 0xB9, 0x47, 0xEF, 0x32, 0x8D, 0x13, 0xE6};
// {24F416E6-1F67-4AA7-B88E-D33F6F3128A1}
constexpr GuidBytes IID_IDIRECT3DVOLUME9{
    0xE6, 0x16, 0xF4, 0x24, 0x67, 0x1F, 0xA7, 0x4A,
    0xB8, 0x8E, 0xD3, 0x3F, 0x6F, 0x31, 0x28, 0xA1};

/// IIDs a QueryInterface on @p iface must accept, terminated by nullptr.
const GuidBytes* const* accepted_iids(D3D9InterfaceId iface) {
    static const GuidBytes* d3d9[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3D9, nullptr};
    static const GuidBytes* device[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DDEVICE9, nullptr};
    static const GuidBytes* swap_chain[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DSWAPCHAIN9, nullptr};
    static const GuidBytes* surface[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DRESOURCE9, &IID_IDIRECT3DSURFACE9,
        nullptr};
    static const GuidBytes* texture[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DRESOURCE9, &IID_IDIRECT3DBASETEXTURE9,
        &IID_IDIRECT3DTEXTURE9, nullptr};
    static const GuidBytes* cube_texture[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DRESOURCE9, &IID_IDIRECT3DBASETEXTURE9,
        &IID_IDIRECT3DCUBETEXTURE9, nullptr};
    static const GuidBytes* volume_texture[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DRESOURCE9, &IID_IDIRECT3DBASETEXTURE9,
        &IID_IDIRECT3DVOLUMETEXTURE9, nullptr};
    static const GuidBytes* volume[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DVOLUME9, nullptr};
    static const GuidBytes* vertex_buffer[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DRESOURCE9, &IID_IDIRECT3DVERTEXBUFFER9,
        nullptr};
    static const GuidBytes* index_buffer[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DRESOURCE9, &IID_IDIRECT3DINDEXBUFFER9,
        nullptr};
    // A shader is not an IDirect3DResource9: it has no pool, priority, or
    // type, and Windows does not expose those methods on it.
    static const GuidBytes* vertex_shader[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DVERTEXSHADER9, nullptr};
    static const GuidBytes* pixel_shader[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DPIXELSHADER9, nullptr};
    static const GuidBytes* state_block[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DSTATEBLOCK9, nullptr};
    static const GuidBytes* vertex_declaration[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DVERTEXDECLARATION9, nullptr};
    static const GuidBytes* query[] = {
        &IID_IUNKNOWN, &IID_IDIRECT3DQUERY9, nullptr};

    switch (iface) {
    case D3D9InterfaceId::Direct3D9:    return d3d9;
    case D3D9InterfaceId::Device:       return device;
    case D3D9InterfaceId::SwapChain:    return swap_chain;
    case D3D9InterfaceId::Surface:      return surface;
    case D3D9InterfaceId::Texture:      return texture;
    case D3D9InterfaceId::CubeTexture:  return cube_texture;
    case D3D9InterfaceId::VertexBuffer: return vertex_buffer;
    case D3D9InterfaceId::IndexBuffer:  return index_buffer;
    case D3D9InterfaceId::VertexShader: return vertex_shader;
    case D3D9InterfaceId::PixelShader:  return pixel_shader;
    case D3D9InterfaceId::StateBlock:   return state_block;
    case D3D9InterfaceId::VertexDeclaration: return vertex_declaration;
    case D3D9InterfaceId::Query:        return query;
    case D3D9InterfaceId::VolumeTexture: return volume_texture;
    case D3D9InterfaceId::Volume:       return volume;
    case D3D9InterfaceId::Count:        break;
    }
    return d3d9;
}

[[nodiscard]] bool format_supported(u32 format) {
    return d3d9_format_supported(format);
}

void write_u32_at(std::array<u8, D3DADAPTER_IDENTIFIER9_SIZE>& destination,
                  usize offset, u32 value) {
    if (offset + sizeof(u32) > destination.size()) return;
    std::memcpy(destination.data() + offset, &value, sizeof(u32));
}

/// PCI vendor/device the adapter reports, and the matching Description.
///
/// Zero by default. A guest that ships a per-GPU database keys off these, and
/// an unmatched adapter takes the unknown-card fallback: a 32 MB texture
/// budget. Overriding lets a run present a known identity without baking a
/// per-title lie into the default path.
///
/// The per-capsule knob is the display-class registry key (see
/// display_device_identity.hpp). VITREO_D3D9_ADAPTER_ID="vendor:device" (hex)
/// and VITREO_D3D9_ADAPTER_DESC are the per-run overrides above it.
///
/// The default stays 0:0 -- D3D9 has never advertised Apple's DXGI vendor ID
/// here, and an unconfigured capsule must keep reporting exactly what it
/// reported before this knob existed.
[[nodiscard]] std::pair<u32, u32> adapter_pci_identity() {
    namespace identity = display_device_identity;
    const auto spec =
        identity::parse_vendor_device_spec(std::getenv("VITREO_D3D9_ADAPTER_ID"));
    if (spec) return *spec;
    const auto& configured = identity::adapter_identity();
    return {configured.vendor_id.value_or(0u),
            configured.device_id.value_or(0u)};
}

/// Subsystem ID and revision follow the same configured identity. They stay
/// zero unless a capsule asserts one, matching the pre-knob behaviour.
[[nodiscard]] std::pair<u32, u32> adapter_subsystem_revision() {
    const auto& configured = display_device_identity::adapter_identity();
    return {configured.subsystem_id.value_or(0u),
            configured.revision.value_or(0u)};
}

/// D3DADAPTER_IDENTIFIER9::liDriverVersion. Zero when nothing is configured,
/// so an unconfigured capsule is byte-identical to before; a capsule that
/// asserts an identity gets the same driver version DXGI reports, because a
/// guest that reads both must not see two different drivers.
[[nodiscard]] u64 adapter_driver_version() {
    namespace identity = display_device_identity;
    const auto& configured = identity::adapter_identity();
    if (configured.driver_version) return *configured.driver_version;
    if (configured.has_pci_identity() || configured.description) {
        return identity::DEFAULT_DRIVER_VERSION;
    }
    return 0;
}

/// Resolved on every call rather than cached: the per-capsule identity is
/// installed after the graphics managers are constructed, so a cached first
/// answer would be the pre-configuration one.
///
/// The default is a fixed Description, not the host GPU name. Guests ship
/// per-GPU databases keyed on this string. A host name that substring-matches
/// a recognized card (for example "AMD Radeon Pro 570") sizes loading pools
/// for that card: about 2 GB of committed blocks, more than a
/// non-large-address-aware 2 GB window, which retries until it hangs. A name
/// that matches nothing takes the unknown-card fallback (32 MB texture
/// budget). Leaking the host name makes guest behaviour vary by host
/// hardware. One consistent name puts every host on the same path.
[[nodiscard]] std::string adapter_description() {
    if (const char* env = std::getenv("VITREO_D3D9_ADAPTER_DESC")) {
        return std::string(env);
    }
    const auto& configured = display_device_identity::adapter_identity();
    if (configured.description) return *configured.description;
    return std::string("Vitreo Display Adapter");
}

void copy_c_string(std::array<u8, D3DADAPTER_IDENTIFIER9_SIZE>& destination,
                   usize offset, usize capacity, const std::string& source) {
    if (offset >= destination.size() || capacity == 0) return;
    const usize available = std::min(capacity, destination.size() - offset);
    const usize length = std::min(source.size(), available - 1);
    std::memcpy(destination.data() + offset, source.data(), length);
}

/// Classic desktop sizes real HAL drivers enumerate for D3DFMT_X8R8G8B8.
/// Keep in sync with User32ExtManager::enum_display_settings. Callers match
/// a short list of 4:3 candidates against EnumAdapterModes before accepting
/// a graphics-quality preset.
struct StandardAdapterMode {
    u32 width;
    u32 height;
};

constexpr StandardAdapterMode kStandardAdapterModes[] = {
    {640, 480},   {800, 600},   {1024, 768},  {1152, 864},
    {1280, 720},  {1280, 800},  {1280, 960},  {1280, 1024},
    {1440, 900},  {1600, 900},  {1600, 1200}, {1680, 1050},
    {1920, 1080}, {1920, 1200}, {2560, 1440}, {2560, 1600},
    {3840, 2160},
};

/// Index 0 is always the current desktop mode (host backing pixels);
/// subsequent entries are the standard sizes that fit on that framebuffer
/// and are not duplicates of it.
[[nodiscard]] bool resolve_adapter_mode(u32 screen_width, u32 screen_height,
                                        u32 mode_index, u32& width,
                                        u32& height) {
    if (screen_width == 0 || screen_height == 0) {
        return false;
    }
    if (mode_index == 0) {
        width = screen_width;
        height = screen_height;
        return true;
    }
    u32 emitted = 1;
    for (const auto& mode : kStandardAdapterModes) {
        if (mode.width > screen_width || mode.height > screen_height) {
            continue;
        }
        if (mode.width == screen_width && mode.height == screen_height) {
            continue;
        }
        if (emitted == mode_index) {
            width = mode.width;
            height = mode.height;
            return true;
        }
        ++emitted;
    }
    return false;
}

[[nodiscard]] u32 count_adapter_modes(u32 screen_width, u32 screen_height) {
    u32 count = 0;
    u32 width = 0;
    u32 height = 0;
    while (resolve_adapter_mode(screen_width, screen_height, count, width,
                                height)) {
        ++count;
    }
    return count;
}

} // namespace


// =============================================================================
// Guest storage
// =============================================================================

bool D3D9Manager::ensure_storage() {
    if (m_storage_ready) return true;
    if (m_memory == nullptr || m_region_base != 0) return false;

    GuestAddr minimum = STORAGE_MIN;
    while (minimum < STORAGE_MAX) {
        const GuestAddr base = m_memory->find_free_gap(minimum, STORAGE_SIZE);
        if (base == 0 || base >= STORAGE_MAX) return false;
        if (m_memory->map_region(base, STORAGE_SIZE,
                                 MemoryProtection::ReadWrite).is_ok()) {
            m_region_base = base;
            break;
        }
        minimum = base + STORAGE_SIZE;
    }
    if (m_region_base == 0) return false;

    m_next_obj_addr = static_cast<u32>(m_region_base + VTABLE_AREA);
    m_storage_ready = true;
    return true;
}

bool D3D9Manager::map_vtable(D3D9InterfaceId iface) {
    const auto index = static_cast<usize>(iface);
    if (index >= D3D9_INTERFACE_COUNT) return false;
    if (m_vtables[index] != 0) return true;
    if (!ensure_storage()) return false;

    usize count = 0;
    (void)d3d9_interface_methods(iface, count);
    const InterfaceThunks& thunks = m_thunks[index];
    if (count == 0 || thunks.size() != count) return false;
    if (std::any_of(thunks.begin(), thunks.end(),
                    [](GuestAddr thunk) { return thunk == 0; })) {
        return false;
    }

    // Vtables are packed from the base of the region, ahead of objects.
    const usize pointer_size = m_is_64bit ? sizeof(u64) : sizeof(u32);
    GuestAddr cursor = m_region_base;
    for (usize i = 0; i < D3D9_INTERFACE_COUNT; ++i) {
        if (i == index) break;
        cursor += d3d9_interface_method_count(
                      static_cast<D3D9InterfaceId>(i)) * pointer_size;
    }
    if (cursor + count * pointer_size > m_region_base + VTABLE_AREA) {
        return false;
    }

    for (usize slot = 0; slot < count; ++slot) {
        const auto result = m_is_64bit
            ? m_memory->write_u64(cursor + slot * pointer_size, thunks[slot])
            : m_memory->write_u32(cursor + slot * pointer_size,
                                  static_cast<u32>(thunks[slot]));
        if (result.is_err()) {
            return false;
        }
    }
    m_vtables[index] = cursor;
    VITREO_TRACE("[d3d9] mapped %s vtable=0x%08llx slots=%zu\n",
                 d3d9_interface_name(iface),
                 static_cast<unsigned long long>(cursor), count);
    return true;
}

u32 D3D9Manager::alloc_com_object(D3D9InterfaceId iface, u32 device) {
    if (!map_vtable(iface)) return 0;

    u32 address = 0;
    if (!m_free_obj_addrs.empty()) {
        address = m_free_obj_addrs.back();
        m_free_obj_addrs.pop_back();
    } else {
        if (static_cast<GuestAddr>(m_next_obj_addr) + OBJECT_STRIDE >
            m_region_base + STORAGE_SIZE) {
            VITREO_TRACE("[d3d9] object storage exhausted "
                         "(live=%zu free_list=%zu next=0x%08x end=0x%08llx)\n",
                         m_objects.size(), m_free_obj_addrs.size(),
                         m_next_obj_addr,
                         static_cast<unsigned long long>(
                             m_region_base + STORAGE_SIZE));
            return 0;
        }
        address = m_next_obj_addr;
        m_next_obj_addr += OBJECT_STRIDE;
    }

    const u32 vtable = static_cast<u32>(m_vtables[static_cast<usize>(iface)]);
    const auto write_vtable = m_is_64bit
        ? m_memory->write_u64(address, vtable)
        : m_memory->write_u32(address, vtable);
    if (write_vtable.is_err()) {
        m_free_obj_addrs.push_back(address);
        return 0;
    }

    ComObject object;
    object.iface = iface;
    object.ref_count = 1;
    object.vtable = vtable;
    object.device = device;
    m_objects.emplace(address, std::move(object));

    VITREO_TRACE("[d3d9] mapped %s object=0x%08x vtable=0x%08x\n",
                 d3d9_interface_name(iface), address, vtable);
    return address;
}

D3D9Manager::ComObject* D3D9Manager::find(u32 object, D3D9InterfaceId iface) {
    const auto found = m_objects.find(object);
    if (found == m_objects.end() || found->second.iface != iface) {
        return nullptr;
    }
    return &found->second;
}

D3D9Manager::ComObject* D3D9Manager::find_any(u32 object) {
    const auto found = m_objects.find(object);
    return found == m_objects.end() ? nullptr : &found->second;
}

u32 D3D9Manager::add_ref(u32 object) {
    ComObject* entry = find_any(object);
    if (entry == nullptr) return 0;
    return ++entry->ref_count;
}

u32 D3D9Manager::release(u32 object) {
    ComObject* entry = find_any(object);
    if (entry == nullptr) return 0;
    if (d3d9_texture_lifetime_diag_enabled() && entry->texture != nullptr &&
        static_cast<u64>(entry->texture->width) *
                static_cast<u64>(entry->texture->height) >=
            1000000u) {
        std::fprintf(stderr,
                     "[d3d9-texture-life] release texture=0x%08x %ux%u "
                     "refs=%u->%u\n",
                     object, entry->texture->width, entry->texture->height,
                     entry->ref_count,
                     entry->ref_count > 0 ? entry->ref_count - 1 : 0u);
    }
    if (entry->ref_count > 0) --entry->ref_count;
    const u32 count = entry->ref_count;
    if (count == 0) destroy_object(object);
    return count;
}

void D3D9Manager::destroy_object(u32 object) {
    const auto found = m_objects.find(object);
    if (found == m_objects.end()) return;

    ComObject& entry = found->second;

    // Learn: D3DSPD_IUNKNOWN private data is Released when the resource dies.
    clear_private_data(entry, object);

    // Release guest mappings this object owns before dropping its state.
    if (entry.surface && entry.surface->pixels != 0) {
        unmap_lock_region(entry.surface->pixels, entry.surface->pixel_bytes);
    }
    if (entry.volume && entry.volume->pixels != 0) {
        unmap_lock_region(entry.volume->pixels, entry.volume->pixel_bytes);
    }
    if (entry.buffer && entry.buffer->storage != 0) {
        unmap_lock_region(entry.buffer->storage, entry.buffer->length);
    }
    if (entry.device_state) {
        // Leaving exclusive mode before tearing down the swap chain restores
        // the host window chrome and user32 virtual display metrics.
        if (m_fullscreen_hwnd != 0) {
            u32 window = entry.device_state->present_field(PP_DEVICE_WINDOW);
            if (window == 0) window = entry.device_state->focus_window;
            if (window == m_fullscreen_hwnd) {
                clear_exclusive_fullscreen();
            }
        }
        // The implicit swap chain, back buffer, and depth stencil are owned by
        // the device: Windows destroys them with it.
        if (entry.device_state->vertex_declaration != 0) {
            const u32 decl = entry.device_state->vertex_declaration;
            entry.device_state->vertex_declaration = 0;
            (void)release(decl);
        }
        if (entry.device_state->cursor_surface != 0) {
            const u32 cursor = entry.device_state->cursor_surface;
            entry.device_state->cursor_surface = 0;
            (void)release(cursor);
        }
        if (entry.device_state->cursor_visible) {
            entry.device_state->cursor_visible = false;
            if (m_cursor_visible_cb) m_cursor_visible_cb(false);
        }
        for (u32& texture : entry.device_state->vertex_textures) {
            const u32 bound = texture;
            texture = 0;
            if (bound != 0) (void)release(bound);
        }
        auto additional = entry.device_state->additional_swap_chains;
        entry.device_state->additional_swap_chains.clear();
        for (usize i = 1;
             i < entry.device_state->render_target_surfaces.size(); ++i) {
            if (entry.device_state->render_target_surfaces[i] != 0) {
                (void)release(entry.device_state->render_target_surfaces[i]);
            }
        }
        const std::array<u32, 4> owned{
            entry.device_state->implicit_swap_chain,
            entry.device_state->back_buffer_surface,
            entry.device_state->depth_stencil_surface,
            entry.device_state->render_target_surface};
        d3d9_cs_drain(true);
        flush_render_pass(*entry.device_state);
        entry.device_state->pipelines.clear();
        entry.device_state->draw_reuse = {};
        entry.device_state->vs_const_dirty = true;
        entry.device_state->ps_const_dirty = true;
        entry.device_state->ff_uniform_dirty = true;
        entry.device_state->swap_chain.reset();
        for (u32 child : additional) {
            if (child != 0 && child != object &&
                m_objects.count(child) != 0) {
                destroy_object(child);
            }
        }
        for (u32 child : owned) {
            if (child != 0 && child != object &&
                m_objects.count(child) != 0) {
                destroy_object(child);
            }
        }
    }
    if (entry.swap_chain_state) {
        d3d9_cs_drain();
        // Additional swap chain: drop backend and owned surfaces with it.
        // Remove this handle from the parent device's enumeration list first
        // so GetSwapChain cannot revive a half-destroyed object.
        if (ComObject* device = find(entry.device, D3D9InterfaceId::Device);
            device != nullptr && device->device_state) {
            auto& chains = device->device_state->additional_swap_chains;
            chains.erase(std::remove(chains.begin(), chains.end(), object),
                         chains.end());
        }
        const u32 back = entry.swap_chain_state->back_buffer_surface;
        const u32 depth = entry.swap_chain_state->depth_stencil_surface;
        entry.swap_chain_state->back_buffer_surface = 0;
        entry.swap_chain_state->depth_stencil_surface = 0;
        entry.swap_chain_state->swap_chain.reset();
        for (u32 child : {back, depth}) {
            if (child != 0 && child != object &&
                m_objects.count(child) != 0) {
                destroy_object(child);
            }
        }
    }
    if (entry.buffer || entry.texture || entry.surface) {
        d3d9_cs_drain();
    }
    if (entry.texture) {
        auto levels = entry.texture->level_surfaces;
        entry.texture->backend.reset();
        for (u32 child : levels) {
            if (child != 0 && m_objects.count(child) != 0) {
                destroy_object(child);
            }
        }
    }
    if (entry.state_block) {
        StateBlockState& block = *entry.state_block;
        if (block.has_vertex_shader && block.vertex_shader != 0) {
            (void)release(block.vertex_shader);
        }
        if (block.has_pixel_shader && block.pixel_shader != 0) {
            (void)release(block.pixel_shader);
        }
        if (block.has_vertex_declaration &&
            block.vertex_declaration != 0) {
            (void)release(block.vertex_declaration);
        }
        if (block.has_textures) {
            for (u32 stage = 0; stage < D3D9_MAX_SAMPLERS; ++stage) {
                if (block.texture_included[stage] &&
                    block.textures[stage] != 0) {
                    (void)release(block.textures[stage]);
                }
            }
            for (u32 stage = 0; stage < D3D9_VERTEX_TEXTURE_SAMPLER_COUNT;
                 ++stage) {
                if (block.vertex_texture_included[stage] &&
                    block.vertex_textures[stage] != 0) {
                    (void)release(block.vertex_textures[stage]);
                }
            }
        }
    }

    // Leave the IUnknown vtable in guest memory. Release is `call [vtable+8]`;
    // storing 0 here turns a stale Release into a GP at address 8 (tutorial
    // quit). impl_method already returns 0 for AddRef/Release on a pointer
    // that is no longer in m_objects; the thunks must stay reachable.
    m_objects.erase(object);
    m_free_obj_addrs.push_back(object);
    VITREO_TRACE("[d3d9] destroyed object=0x%08x\n", object);
}

GuestAddr D3D9Manager::map_lock_region(usize bytes) {
    if (m_memory == nullptr || bytes == 0) return 0;
    const usize page = 0x1000;
    const usize rounded = (bytes + page - 1) & ~(page - 1);

    GuestAddr search_end = LOCK_REGION_SEARCH_END;
    if (m_memory->maximum_user_address() != 0) {
        const GuestAddr user_end = m_memory->maximum_user_address() + 1u;
        if (user_end < search_end) {
            search_end = user_end;
        }
    }

    GuestAddr minimum = LOCK_REGION_MIN;
    while (minimum < search_end) {
        const GuestAddr base = m_memory->find_free_gap(minimum, rounded);
        if (base == 0 || base >= search_end) {
            return 0;
        }
        if (m_memory->map_region(base, rounded,
                                 MemoryProtection::ReadWrite).is_ok()) {
            if (base >= LOCK_REGION_PREFERRED_END) {
                VITREO_TRACE("[d3d9] lock region fallback base=0x%llx "
                             "bytes=%zu (preferred window full)\n",
                             static_cast<unsigned long long>(base), rounded);
            }
            return base;
        }
        minimum = base + rounded;
    }
    return 0;
}

void D3D9Manager::unmap_lock_region(GuestAddr base, usize bytes) {
    if (m_memory == nullptr || base == 0 || bytes == 0) return;
    const usize page = 0x1000;
    const usize rounded = (bytes + page - 1) & ~(page - 1);
    (void)m_memory->unmap_region(base, rounded);
}

// =============================================================================
// Shared IUnknown dispatch
// =============================================================================

bool D3D9Manager::dispatch_unknown(Marshaler& m, usize slot, u32 self,
                                   D3D9InterfaceId iface) {
    switch (slot) {
    case 0: { // QueryInterface
        const GuestAddr iid_address = m.arg_ptr(1);
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0 || m.write_ptr(object_out, 0).is_err()) {
            m.set_return(E_POINTER_D3D);
            return true;
        }
        GuidBytes iid{};
        if (!read_guid(iid_address, iid)) {
            m.set_return(E_INVALIDARG_D3D);
            return true;
        }
        bool accepted = false;
        for (const GuidBytes* const* it = accepted_iids(iface);
             *it != nullptr; ++it) {
            if (**it == iid) {
                accepted = true;
                break;
            }
        }
        if (!accepted) {
            m.set_return(E_NOINTERFACE_D3D);
            return true;
        }
        (void)add_ref(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release(self);
            m.set_return(E_POINTER_D3D);
            return true;
        }
        m.set_return(D3D_OK);
        return true;
    }
    case 1:
        m.set_return(add_ref(self));
        return true;
    case 2:
        m.set_return(release(self));
        return true;
    default:
        return false;
    }
}

// =============================================================================
// Entry point
// =============================================================================

bool D3D9Manager::read_guid(GuestAddr address, GuidBytes& guid) const {
    return m_memory != nullptr && address != 0 &&
        m_memory->read_bytes(address, guid.data(), guid.size()).is_ok();
}

bool D3D9Manager::valid_adapter(u32 adapter, u32 device_type) const {
    return adapter == 0 && device_type == D3DDEVTYPE_HAL &&
        m_backend != nullptr && m_backend->available();
}

bool D3D9Manager::current_display_mode(std::array<u32, 4>& mode) const {
    // Exclusive CreateDevice/Reset is the adapter's current mode. Returning
    // the host Retina framebuffer here made GetDisplayMode report 3600×2338
    // while the guest surface and mouse were 1920×1200.
    if (m_exclusive_mode_width > 0 && m_exclusive_mode_height > 0) {
        mode = {m_exclusive_mode_width, m_exclusive_mode_height, 0,
                D3DFMT_X8R8G8B8};
        return true;
    }
    if (!m_display_size_resolver) return false;
    u32 width = 0;
    u32 height = 0;
    if (!m_display_size_resolver(width, height) || width == 0 || height == 0) {
        return false;
    }
    mode = {width, height, 0, D3DFMT_X8R8G8B8};
    return true;
}

void D3D9Manager::resolve_present_back_buffer(bool windowed, u32 window,
                                              u32& width, u32& height,
                                              u32& format) const {
    // Learn (D3DPRESENT_PARAMETERS): when Windowed is TRUE and either
    // BackBufferWidth or BackBufferHeight is zero, the corresponding client
    // dimension of hDeviceWindow (or the focus window) is taken.
    if (windowed && (width == 0 || height == 0) && window != 0 &&
        m_client_size_resolver) {
        u32 client_w = 0;
        u32 client_h = 0;
        if (m_client_size_resolver(window, client_w, client_h) &&
            client_w > 0 && client_h > 0) {
            if (width == 0) width = client_w;
            if (height == 0) height = client_h;
        }
    }

    // Display mode fills D3DFMT_UNKNOWN and any size still unresolved
    // (fullscreen zeros, or windowed when no client size is available).
    std::array<u32, 4> mode{};
    if ((width == 0 || height == 0 || format == D3DFMT_UNKNOWN) &&
        current_display_mode(mode)) {
        if (width == 0) width = mode[0];
        if (height == 0) height = mode[1];
        if (format == D3DFMT_UNKNOWN) format = mode[3];
    }
}

void D3D9Manager::get_raster_status(Marshaler& m, u32 swap_chain_index,
                                    GuestAddr out) {
    // Learn (D3DRASTER_STATUS): when InVBlank is TRUE, ScanLine is zero and
    // may be ignored. Vitreo's Metal present path is host-synchronous from
    // the guest's POV — once Present returns (or no draw is mid-flight), the
    // frame has already been submitted/presented, so report vertical blank
    // rather than fabricating progressive scanline numbers we cannot observe.
    if (swap_chain_index != 0 || out == 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    const u32 status[2] = {1u /* InVBlank = TRUE */, 0u /* ScanLine */};
    if (m.memory()
            .write_bytes(out, reinterpret_cast<const Byte*>(status),
                         D3DRASTER_STATUS_SIZE)
            .is_err()) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    m.set_return(D3D_OK);
}

u32 D3D9Manager::direct3d_create9(u32 sdk_version) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    VITREO_TRACE("[d3d9] Direct3DCreate9(SDKVersion=%u)\n", sdk_version);
    // Arm here rather than at CreateDevice: a run that never reaches a
    // device still reports whether the dump is available.
    d3d9_frame_dump_init();
    // D3D_SDK_VERSION is 32 in the final DirectX 9 SDK. Older headers pass
    // 31. Accept both.
    if (sdk_version != D3D_SDK_VERSION && sdk_version != 31u) {
        VITREO_TRACE("[d3d9] -> NULL (SDK version mismatch)\n");
        return 0;
    }
    if (m_backend == nullptr || !m_backend->available()) {
        VITREO_TRACE("[d3d9] -> NULL (graphics backend unavailable)\n");
        return 0;
    }
    const u32 object = alloc_com_object(D3D9InterfaceId::Direct3D9, 0);
    VITREO_TRACE("[d3d9] -> IDirect3D9*=0x%08x\n", object);
    return object;
}

namespace {
/// VITREO_D3D9_COVERAGE=1: record every (interface, vtable slot) actually
/// dispatched, and dump the set at exit. Hand-written oracle scenes can only
/// cover paths someone thought of; diffing this between a real game run and
/// the oracle suite names the paths a game uses that no test touches, which
/// is where graphics bugs survive.
constexpr u32 kCovIfaces = 24;
constexpr u32 kCovSlots = 128;
std::array<std::atomic<u64>, kCovIfaces * kCovSlots> g_d3d9_cov{};

[[nodiscard]] bool d3d9_coverage_enabled() noexcept {
    static const bool on = std::getenv("VITREO_D3D9_COVERAGE") != nullptr;
    return on;
}

struct D3d9CoverageReporter {
    ~D3d9CoverageReporter() {
        if (!d3d9_coverage_enabled()) return;
        std::fprintf(stderr, "[d3d9-cov] pid=%d dispatched (iface,slot):\n",
                     static_cast<int>(::getpid()));
        for (u32 i = 0; i < kCovIfaces; ++i) {
            for (u32 sl = 0; sl < kCovSlots; ++sl) {
                const u64 n =
                    g_d3d9_cov[i * kCovSlots + sl].load(
                        std::memory_order_relaxed);
                if (n != 0) {
                    std::fprintf(stderr, "[d3d9-cov] %u %u %llu\n", i, sl,
                                 static_cast<unsigned long long>(n));
                }
            }
        }
    }
};
D3d9CoverageReporter g_d3d9_cov_reporter;

/// Distinct (kind, index, value) triples observed, plus a call count each.
/// A map keyed on the packed triple: guests set a small number of distinct
/// values per state, so this stays tiny even across millions of calls.
std::mutex g_d3d9_state_cov_mutex;
std::map<u64, u64> g_d3d9_state_cov;
/// Distinct vertex-declaration signatures, with a creation count each.
std::map<std::string, u64> g_d3d9_decl_cov;

[[nodiscard]] bool d3d9_state_coverage_enabled() noexcept {
    static const bool on =
        std::getenv("VITREO_D3D9_STATE_COVERAGE") != nullptr;
    return on;
}

/// Caller must hold `g_d3d9_state_cov_mutex`.
void dump_d3d9_state_coverage_locked() {
    std::fprintf(stderr, "[d3d9-statecov] pid=%d (kind index value calls):\n",
                 static_cast<int>(::getpid()));
    for (const auto& [key, calls] : g_d3d9_state_cov) {
        std::fprintf(stderr, "[d3d9-statecov] %u %u %u %llu\n",
                     static_cast<u32>(key >> 40) & 0xFFu,
                     static_cast<u32>(key >> 32) & 0xFFu,
                     static_cast<u32>(key),
                     static_cast<unsigned long long>(calls));
    }
    for (const auto& [signature, count] : g_d3d9_decl_cov) {
        std::fprintf(stderr, "[d3d9-decl] x%llu %s\n",
                     static_cast<unsigned long long>(count),
                     signature.c_str());
    }
    std::fflush(stderr);
}

struct D3d9StateCoverageReporter {
    ~D3d9StateCoverageReporter() {
        if (!d3d9_state_coverage_enabled()) return;
        const std::lock_guard<std::mutex> guard(g_d3d9_state_cov_mutex);
        dump_d3d9_state_coverage_locked();
    }
};
D3d9StateCoverageReporter g_d3d9_state_cov_reporter;
} // namespace

bool d3d9_state_coverage_enabled_for_declarations() noexcept {
    return d3d9_state_coverage_enabled();
}

void record_d3d9_vertex_declaration(const std::string& signature) noexcept {
    if (!d3d9_state_coverage_enabled()) return;
    const std::lock_guard<std::mutex> guard(g_d3d9_state_cov_mutex);
    ++g_d3d9_decl_cov[signature];
}

void record_d3d9_state_use(D3D9StateKind kind, u32 index,
                           u32 value) noexcept {
    if (!d3d9_state_coverage_enabled()) return;
    const u64 key = (static_cast<u64>(kind) << 40) |
                    (static_cast<u64>(index & 0xFFu) << 32) |
                    static_cast<u64>(value);
    const std::lock_guard<std::mutex> guard(g_d3d9_state_cov_mutex);
    ++g_d3d9_state_cov[key];
    // Also dump periodically: a guest that is force-quit never runs the
    // exit-time reporter, and a graphics investigation usually ends that way.
    const auto now = std::chrono::steady_clock::now();
    static auto last_dump = now;
    if (now - last_dump >= std::chrono::seconds(30)) {
        last_dump = now;
        dump_d3d9_state_coverage_locked();
    }
}

thread_local std::unique_lock<std::recursive_mutex>*
    D3D9Manager::t_active_lock = nullptr;

D3D9Manager::UnlockDuringBlockingCall::UnlockDuringBlockingCall()
    : m_lock(t_active_lock) {
    // owns_lock() is false when an outer impl_method frame holds the mutex and
    // this inner unique_lock was never the owner; releasing then would unlock
    // something we do not hold.
    if (m_lock != nullptr && m_lock->owns_lock()) {
        m_lock->unlock();
    } else {
        m_lock = nullptr;
    }
}

D3D9Manager::UnlockDuringBlockingCall::~UnlockDuringBlockingCall() {
    if (m_lock != nullptr) m_lock->lock();
}

void D3D9Manager::impl_method(Marshaler& m, D3D9InterfaceId iface,
                              usize slot) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    // Publish this frame's lock so the Present path can drop it around the
    // vsync wait, and restore whatever an outer frame published on the way out.
    std::unique_lock<std::recursive_mutex>* const previous_lock = t_active_lock;
    t_active_lock = &lock;
    struct RestoreActiveLock {
        std::unique_lock<std::recursive_mutex>* previous;
        ~RestoreActiveLock() { t_active_lock = previous; }
    } restore_active_lock{previous_lock};
    const u32 self = static_cast<u32>(m.arg_ptr(0));
    if (d3d9_coverage_enabled()) {
        const u32 i = static_cast<u32>(iface);
        if (i < kCovIfaces && slot < kCovSlots) {
            g_d3d9_cov[i * kCovSlots + slot].fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    const ComObject* entry = find(self, iface);
    if (entry == nullptr || slot >= d3d9_interface_method_count(iface)) {
        // AddRef/Release must never fault on an unknown pointer; every other
        // method reports an invalid call.
        m.set_return(slot == 1 || slot == 2 ? 0 : D3DERR_INVALIDCALL);
        return;
    }

    switch (iface) {
    case D3D9InterfaceId::Direct3D9:    dispatch_direct3d9(m, slot, self); return;
    case D3D9InterfaceId::Device:       dispatch_device(m, slot, self); return;
    case D3D9InterfaceId::SwapChain:    dispatch_swap_chain(m, slot, self); return;
    case D3D9InterfaceId::Surface:      dispatch_surface(m, slot, self); return;
    case D3D9InterfaceId::Texture:      dispatch_texture(m, slot, self); return;
    case D3D9InterfaceId::CubeTexture:  dispatch_cube_texture(m, slot, self); return;
    case D3D9InterfaceId::VertexBuffer: dispatch_vertex_buffer(m, slot, self); return;
    case D3D9InterfaceId::IndexBuffer:  dispatch_index_buffer(m, slot, self); return;
    case D3D9InterfaceId::VertexShader: dispatch_shader(m, slot, self, false); return;
    case D3D9InterfaceId::PixelShader:  dispatch_shader(m, slot, self, true); return;
    case D3D9InterfaceId::StateBlock:   dispatch_state_block(m, slot, self); return;
    case D3D9InterfaceId::VertexDeclaration:
        dispatch_vertex_declaration(m, slot, self);
        return;
    case D3D9InterfaceId::Query:
        dispatch_query(m, slot, self);
        return;
    case D3D9InterfaceId::VolumeTexture:
        dispatch_volume_texture(m, slot, self);
        return;
    case D3D9InterfaceId::Volume:
        dispatch_volume(m, slot, self);
        return;
    case D3D9InterfaceId::Count:        break;
    }
    m.set_return(D3DERR_INVALIDCALL);
}

// =============================================================================
// IDirect3D9
// =============================================================================

void D3D9Manager::dispatch_direct3d9(Marshaler& m, usize slot, u32 self) {
    usize method_count = 0;
    const D3D9MethodDesc* methods =
        d3d9_interface_methods(D3D9InterfaceId::Direct3D9, method_count);
    VITREO_TRACE("[d3d9] IDirect3D9::%s(this=0x%08x)\n",
                 methods[slot].name, self);

    if (dispatch_unknown(m, slot, self, D3D9InterfaceId::Direct3D9)) return;

    switch (slot) {
    case 3:
        // Vitreo does not expose a pluggable software rasterizer.
        m.set_return(E_NOTIMPL_D3D);
        return;
    case 4:
        m.set_return(m_backend != nullptr && m_backend->available() ? 1 : 0);
        return;
    case 5: { // GetAdapterIdentifier
        const u32 adapter = static_cast<u32>(m.arg(1));
        const u32 flags = static_cast<u32>(m.arg(2));
        const GuestAddr identifier = m.arg_ptr(3);
        if (!valid_adapter(adapter, D3DDEVTYPE_HAL) ||
            (flags & ~D3DENUM_WHQL_LEVEL) != 0 || identifier == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<u8, D3DADAPTER_IDENTIFIER9_SIZE> bytes{};
        copy_c_string(bytes, 0, 512, "vitreo-metal");
        // A fixed Description, not the host GPU name. Guests ship per-GPU
        // databases keyed on this string. A host name that substring-matches
        // a recognized card sizes loading pools for that card (about 2 GB of
        // committed blocks, more than a non-large-address-aware 2 GB window).
        // A name that matches nothing takes the unknown-card fallback (32 MB
        // texture budget). The identity here is already synthetic — driver
        // "vitreo-metal", zero PCI ids — so leaking the host name only makes
        // guest behaviour vary by host hardware. One consistent name puts
        // every host on the same path.
        copy_c_string(bytes, 512, 512, adapter_description());
        copy_c_string(bytes, 1024, 32, "\\\\.\\DISPLAY1");
        // The host Metal adapter does not expose a Windows PCI identity,
        // driver version, Windows device GUID, or WHQL level. Those fields
        // are zero unless an identity is configured -- see
        // adapter_pci_identity() for why a guest may need a real one.
        const auto [vendor_id, device_id] = adapter_pci_identity();
        const auto [subsystem_id, revision] = adapter_subsystem_revision();
        const u64 driver_version = adapter_driver_version();
        // D3DADAPTER_IDENTIFIER9: szDriver[512], szDescription[512],
        // szDeviceName[32], LARGE_INTEGER liDriverVersion at 1056.
        write_u32_at(bytes, 1056, static_cast<u32>(driver_version & 0xFFFFFFFFull));
        write_u32_at(bytes, 1060, static_cast<u32>(driver_version >> 32));
        write_u32_at(bytes, 1064, vendor_id);
        write_u32_at(bytes, 1068, device_id);
        write_u32_at(bytes, 1072, subsystem_id);
        write_u32_at(bytes, 1076, revision);
        if (m.memory().write_bytes(identifier, bytes.data(),
                                   bytes.size()).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 6: { // GetAdapterModeCount
        std::array<u32, 4> desktop{};
        const u32 adapter = static_cast<u32>(m.arg(1));
        const u32 format = static_cast<u32>(m.arg(2));
        if (adapter != 0 || format != D3DFMT_X8R8G8B8 ||
            !current_display_mode(desktop)) {
            m.set_return(0);
            return;
        }
        m.set_return(count_adapter_modes(desktop[0], desktop[1]));
        return;
    }
    case 7: { // EnumAdapterModes
        const u32 adapter = static_cast<u32>(m.arg(1));
        const u32 format = static_cast<u32>(m.arg(2));
        const u32 mode_index = static_cast<u32>(m.arg(3));
        const GuestAddr mode_out = m.arg_ptr(4);
        std::array<u32, 4> desktop{};
        u32 width = 0;
        u32 height = 0;
        if (adapter != 0 || format != D3DFMT_X8R8G8B8 || mode_out == 0 ||
            !current_display_mode(desktop) ||
            !resolve_adapter_mode(desktop[0], desktop[1], mode_index, width,
                                  height)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const std::array<u32, 4> mode{width, height, 0, D3DFMT_X8R8G8B8};
        if (m.memory()
                .write_bytes(mode_out,
                             reinterpret_cast<const Byte*>(mode.data()),
                             sizeof(mode))
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        VITREO_TRACE("[d3d9] EnumAdapterModes(%u) -> %ux%u fmt=%u\n",
                     mode_index, width, height, D3DFMT_X8R8G8B8);
        m.set_return(D3D_OK);
        return;
    }
    case 8: { // GetAdapterDisplayMode
        const u32 adapter = static_cast<u32>(m.arg(1));
        const GuestAddr mode_out = m.arg_ptr(2);
        std::array<u32, 4> mode{};
        if (adapter != 0 || mode_out == 0 || !current_display_mode(mode) ||
            m.memory().write_bytes(
                mode_out, reinterpret_cast<const Byte*>(mode.data()),
                sizeof(mode)).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 9: { // CheckDeviceType
        const u32 adapter = static_cast<u32>(m.arg(1));
        const u32 device_type = static_cast<u32>(m.arg(2));
        const u32 adapter_format = static_cast<u32>(m.arg(3));
        const u32 back_buffer_format = static_cast<u32>(m.arg(4));
        m.set_return(valid_adapter(adapter, device_type) &&
                             format_supported(adapter_format) &&
                             format_supported(back_buffer_format)
                         ? D3D_OK
                         : D3DERR_NOTAVAILABLE);
        return;
    }
    case 10: { // CheckDeviceFormat
        const u32 adapter = static_cast<u32>(m.arg(1));
        const u32 device_type = static_cast<u32>(m.arg(2));
        const u32 adapter_format = static_cast<u32>(m.arg(3));
        const u32 usage = static_cast<u32>(m.arg(4));
        const u32 resource_type = static_cast<u32>(m.arg(5));
        const u32 check_format = static_cast<u32>(m.arg(6));
        constexpr u32 D3DUSAGE_QUERY_VERTEXTEXTURE = 0x00100000u;
        constexpr u32 D3DUSAGE_QUERY_LEGACYBUMPMAP = 0x00008000u;
        const bool vertex_texture_query =
            (usage & D3DUSAGE_QUERY_VERTEXTEXTURE) != 0;
        const bool legacy_bump_query =
            (usage & D3DUSAGE_QUERY_LEGACYBUMPMAP) != 0;
        const graphics::FormatInfo checked_format = graphics::format_table(
            d3d9_backend_format(check_format));
        const bool vertex_texture_compatible =
            !vertex_texture_query ||
            ((resource_type == 3 || resource_type == 4 ||
              resource_type == 5) &&
             !checked_format.is_depth);
        // The fixed-function bump path consumes signed DU/DV from formats that
        // map exactly onto the shared signed-normalized texture contract.
        const bool legacy_bump_compatible =
            !legacy_bump_query ||
            (resource_type == 3 /* D3DRTYPE_TEXTURE */ &&
             (check_format == 60 /* D3DFMT_V8U8 */ ||
              check_format == 62 /* D3DFMT_X8L8V8U8 */ ||
              check_format == 63 /* D3DFMT_Q8W8V8U8 */ ||
              check_format == 64 /* D3DFMT_V16U16 */ ||
              check_format == 117 /* D3DFMT_CxV8U8 */));
        const bool ok = valid_adapter(adapter, device_type) &&
                        format_supported(adapter_format) &&
                        resource_type >= 1 && resource_type <= 7 &&
                        format_supported(check_format) &&
                        vertex_texture_compatible && legacy_bump_compatible;
        VITREO_TRACE("[d3d9] CheckDeviceFormat adapter=%u type=%u "
                     "afmt=0x%x usage=0x%x rtype=%u cfmt=0x%x -> %s\n",
                     adapter, device_type, adapter_format, usage,
                     resource_type, check_format,
                     ok ? "OK" : "NOTAVAILABLE");
        m.set_return(ok ? D3D_OK : D3DERR_NOTAVAILABLE);
        return;
    }
    case 11: { // CheckDeviceMultiSampleType
        const u32 adapter = static_cast<u32>(m.arg(1));
        const u32 device_type = static_cast<u32>(m.arg(2));
        const u32 surface_format = static_cast<u32>(m.arg(3));
        const u32 sample_type = static_cast<u32>(m.arg(5));
        const GuestAddr quality_out = m.arg_ptr(6);
        const bool supported = valid_adapter(adapter, device_type) &&
                               format_supported(surface_format) &&
                               d3d9_multisample_supported(m_backend,
                                                          sample_type);
        const u32 levels =
            supported ? d3d9_multisample_quality_levels(m_backend, sample_type)
                      : 0u;
        if (quality_out != 0 && m.write_u32(quality_out, levels).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(supported ? D3D_OK : D3DERR_NOTAVAILABLE);
        return;
    }
    case 12: { // CheckDepthStencilMatch
        const u32 adapter = static_cast<u32>(m.arg(1));
        const u32 device_type = static_cast<u32>(m.arg(2));
        const u32 adapter_format = static_cast<u32>(m.arg(3));
        const u32 render_target_format = static_cast<u32>(m.arg(4));
        const u32 depth_format = static_cast<u32>(m.arg(5));
        m.set_return(valid_adapter(adapter, device_type) &&
                             format_supported(adapter_format) &&
                             format_supported(render_target_format) &&
                             format_supported(depth_format)
                         ? D3D_OK
                         : D3DERR_NOTAVAILABLE);
        return;
    }
    case 13: { // CheckDeviceFormatConversion
        const u32 adapter = static_cast<u32>(m.arg(1));
        const u32 device_type = static_cast<u32>(m.arg(2));
        const u32 source = static_cast<u32>(m.arg(3));
        const u32 target = static_cast<u32>(m.arg(4));
        // No D3D9 blit/conversion path is exposed yet. Identity conversion is
        // the only conversion contract that can be claimed.
        m.set_return(valid_adapter(adapter, device_type) &&
                             source == target && format_supported(source)
                         ? D3D_OK
                         : D3DERR_NOTAVAILABLE);
        return;
    }
    case 14: { // GetDeviceCaps
        const u32 adapter = static_cast<u32>(m.arg(1));
        const u32 device_type = static_cast<u32>(m.arg(2));
        const GuestAddr caps_out = m.arg_ptr(3);
        if (!valid_adapter(adapter, device_type) || caps_out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<u8, D3DCAPS9_SIZE> caps{};
        fill_d3d9_device_caps(caps, D3DDEVTYPE_HAL, adapter);
        if (m.memory().write_bytes(caps_out, caps.data(),
                                   caps.size()).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 15: {
        const u32 adapter = static_cast<u32>(m.arg(1));
        m.set_return(adapter == 0 ? PRIMARY_MONITOR_HANDLE : 0);
        return;
    }
    case 16: // CreateDevice
        create_device(m, self);
        return;
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

// =============================================================================
// Thunk table
// =============================================================================

static void impl_Direct3DCreate9(Marshaler& m, D3D9Manager& manager) {
    m.set_return(manager.direct3d_create9(static_cast<u32>(m.arg(0))));
}

static void impl_Direct3DCreate9Ex(Marshaler& m) {
    const GuestAddr object_out = m.arg_ptr(1);
    if (object_out != 0) (void)m.write_ptr(object_out, 0);
    // IDirect3D9Ex adds the reset/lost-free presentation model, which needs
    // its own device interface; claiming it here would be a fabricated object.
    VITREO_TRACE("[d3d9] Direct3DCreate9Ex -> E_NOTIMPL "
                 "(IDirect3D9Ex not implemented)\n");
    m.set_return(E_NOTIMPL_D3D);
}

/// Export signature: `void WINAPI DebugSetMute(void)` (DebugSetMute@0).
/// A mistaken 1-arg stdcall popped the return address and faulted at the
/// first call (unmapped branch target 0x1 after GetProcAddress succeeded).
static void impl_DebugSetMute(Marshaler& m) {
    m.set_return(0);
}

/// `IDirect3DShaderValidator9* WINAPI Direct3DShaderValidatorCreate9(void)`.
/// Retail d3d9.dll exports this but returns NULL — the shader validator only
/// exists in the debug runtime (d3d9d.dll). Callers null-check the
/// result and skip validation. Returning a resolvable export that yields NULL
/// is the correct retail contract; a GetProcAddress miss was wrong, and
/// fabricating a validator COM object would violate the contract.
static void impl_Direct3DShaderValidatorCreate9(Marshaler& m) {
    m.set_return(0);
}

DllExports create_d3d9_thunks(D3D9Manager& manager) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "Direct3DCreate9", 1,
        [&manager](Marshaler& m) { impl_Direct3DCreate9(m, manager); },
        4,
    });
    exports.entries.push_back(ExportEntry{
        "Direct3DCreate9Ex", 2,
        [](Marshaler& m) { impl_Direct3DCreate9Ex(m); },
        8,
    });
    exports.entries.push_back(ExportEntry{
        "DebugSetMute", 3,
        [](Marshaler& m) { impl_DebugSetMute(m); },
        0,
    });
    exports.entries.push_back(ExportEntry{
        "Direct3DShaderValidatorCreate9", 4,
        [](Marshaler& m) { impl_Direct3DShaderValidatorCreate9(m); },
        0,
    });
    // PIX markers (SDK shared\d3d9.h). Ordinals from the SysWOW64 and
    // System32 d3d9.dll export tables: 27 D3DPERF_BeginEvent, 28 EndEvent,
    // 29 GetStatus, 30 QueryRepeatFrame, 31 SetMarker, 32 SetOptions,
    // 33 SetRegion. All are WINAPI, so each must remove exactly its argument
    // bytes: a missing export is bound to a fail stub that cannot know its
    // arity, which left Goat Simulator's SetOptions(1) argument on the stack
    // and failed the caller's /GS check.
    // No profiler is attached, so GetStatus is 0 and the event calls do
    // not record. BeginEvent/EndEvent return -1 in that case (the documented
    // failure value of BeginEvent), QueryRepeatFrame returns FALSE, and
    // SetOptions changes nothing observable. Last-error is left untouched.
    // Measured: tools/oracle_probe/d3d9_perf_probe{32,64}.expected.
    exports.entries.push_back(ExportEntry{
        "D3DPERF_BeginEvent", 27,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg_ptr(1);
            m.set_return(static_cast<u64>(static_cast<i64>(-1)));
        },
        8,
    });
    exports.entries.push_back(ExportEntry{
        "D3DPERF_EndEvent", 28,
        [](Marshaler& m) {
            m.set_return(static_cast<u64>(static_cast<i64>(-1)));
        },
        0,
    });
    exports.entries.push_back(ExportEntry{
        "D3DPERF_GetStatus", 29,
        [](Marshaler& m) { m.set_return(0); },
        0,
    });
    exports.entries.push_back(ExportEntry{
        "D3DPERF_QueryRepeatFrame", 30,
        [](Marshaler& m) { m.set_return(0); },  // FALSE
        0,
    });
    exports.entries.push_back(ExportEntry{
        "D3DPERF_SetMarker", 31,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg_ptr(1);
        },
        8,
    });
    exports.entries.push_back(ExportEntry{
        "D3DPERF_SetOptions", 32,
        [](Marshaler& m) { (void)m.arg(0); },  // DWORD dwOptions
        4,
    });
    exports.entries.push_back(ExportEntry{
        "D3DPERF_SetRegion", 33,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg_ptr(1);
        },
        8,
    });

    for (usize index = 0; index < D3D9_INTERFACE_COUNT; ++index) {
        const auto iface = static_cast<D3D9InterfaceId>(index);
        usize count = 0;
        const D3D9MethodDesc* methods = d3d9_interface_methods(iface, count);
        for (usize slot = 0; slot < count; ++slot) {
            exports.entries.push_back(ExportEntry{
                d3d9_method_export_name(iface, slot), 0,
                [&manager, iface, slot](Marshaler& m) {
                    manager.impl_method(m, iface, slot);
                },
                methods[slot].stdcall_bytes,
            });
        }
    }
    return exports;
}

} // namespace vitreo::winapi
