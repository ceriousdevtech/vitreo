// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d8.cpp
/// @brief D3D8 object model, nested D3D9 calls, IUnknown/IDirect3DResource8,
///        the IDirect3D8 factory and the d3d8.dll exports.

#include "winapi/graphics/d3d8/d3d8_internal.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace vitreo::winapi {

using namespace d3d8c;

namespace {

// Guest storage: vtables, then 16-byte COM objects, then scratch data for
// translated structures, then the nested-call stack at the top.
constexpr usize STORAGE_SIZE = 0x400000;
constexpr GuestAddr STORAGE_MIN = 0x62000000;
constexpr GuestAddr STORAGE_MAX = 0x6E000000;
constexpr usize VTABLE_AREA = 0x1000;
constexpr usize OBJECT_END = 0x100000;
constexpr usize OBJECT_SIZE = 16;
constexpr usize SCRATCH_BEGIN = 0x100000;
constexpr usize SCRATCH_END = 0x3C0000;
constexpr usize STACK_TOP = 0x400000 - 0x40;

bool failed(u32 hr) { return (hr & 0x80000000u) != 0; }

/// The D3D9 device type a D3D8 device type is realised with. Measured
/// (tools/oracle_probe/d3d8_ref_device_probe): Windows 11's d3d8.dll
/// answers GetDeviceCaps, CheckDeviceType and CreateDevice for
/// D3DDEVTYPE_REF (the reference rasterizer), while D3DDEVTYPE_SW is
/// D3DERR_INVALIDCALL (d3d8_contract_probe). A REF device renders the same
/// Direct3D 8 contract as HAL, so it is the same D3D9 HAL device here; its
/// caps and creation parameters still name REF.
u32 device_type_to9(u32 type) {
    return type == d3d8c::D3DDEVTYPE_REF ? d3d8c::D3DDEVTYPE_HAL : type;
}

struct InterfaceInfo {
    const char* name;
    const D3D8MethodInfo* methods;
    usize count;
    const std::array<u8, 16>* iid;
};

template <usize N>
constexpr InterfaceInfo info(const char* name, const D3D8MethodInfo (&methods)[N],
                             const std::array<u8, 16>& iid) {
    return {name, methods, N, &iid};
}

const InterfaceInfo kInterfaces[D3D8_INTERFACE_COUNT] = {
    info("Direct3D", kIDirect3D8Methods, kIID_IDirect3D8),
    info("Device", kIDirect3DDevice8Methods, kIID_IDirect3DDevice8),
    info("SwapChain", kIDirect3DSwapChain8Methods, kIID_IDirect3DSwapChain8),
    info("Surface", kIDirect3DSurface8Methods, kIID_IDirect3DSurface8),
    info("Volume", kIDirect3DVolume8Methods, kIID_IDirect3DVolume8),
    info("Texture", kIDirect3DTexture8Methods, kIID_IDirect3DTexture8),
    info("CubeTexture", kIDirect3DCubeTexture8Methods, kIID_IDirect3DCubeTexture8),
    info("VolumeTexture", kIDirect3DVolumeTexture8Methods,
         kIID_IDirect3DVolumeTexture8),
    info("VertexBuffer", kIDirect3DVertexBuffer8Methods,
         kIID_IDirect3DVertexBuffer8),
    info("IndexBuffer", kIDirect3DIndexBuffer8Methods, kIID_IDirect3DIndexBuffer8),
};

bool is_resource(D3D8InterfaceId iface) {
    switch (iface) {
    case D3D8InterfaceId::Texture:
    case D3D8InterfaceId::CubeTexture:
    case D3D8InterfaceId::VolumeTexture:
    case D3D8InterfaceId::VertexBuffer:
    case D3D8InterfaceId::IndexBuffer:
        return true;
    default:
        return false;
    }
}

bool is_texture(D3D8InterfaceId iface) {
    return iface == D3D8InterfaceId::Texture ||
           iface == D3D8InterfaceId::CubeTexture ||
           iface == D3D8InterfaceId::VolumeTexture;
}

bool counterpart(D3D9InterfaceId iface9, D3D8InterfaceId& iface8) {
    switch (iface9) {
    case D3D9InterfaceId::Direct3D9: iface8 = D3D8InterfaceId::Direct3D8; return true;
    case D3D9InterfaceId::Device: iface8 = D3D8InterfaceId::Device; return true;
    case D3D9InterfaceId::SwapChain: iface8 = D3D8InterfaceId::SwapChain; return true;
    case D3D9InterfaceId::Surface: iface8 = D3D8InterfaceId::Surface; return true;
    case D3D9InterfaceId::Volume: iface8 = D3D8InterfaceId::Volume; return true;
    case D3D9InterfaceId::Texture: iface8 = D3D8InterfaceId::Texture; return true;
    case D3D9InterfaceId::CubeTexture: iface8 = D3D8InterfaceId::CubeTexture; return true;
    case D3D9InterfaceId::VolumeTexture:
        iface8 = D3D8InterfaceId::VolumeTexture;
        return true;
    case D3D9InterfaceId::VertexBuffer: iface8 = D3D8InterfaceId::VertexBuffer; return true;
    case D3D9InterfaceId::IndexBuffer: iface8 = D3D8InterfaceId::IndexBuffer; return true;
    default:
        return false;
    }
}

D3D9InterfaceId backing_interface(D3D8InterfaceId iface) {
    switch (iface) {
    case D3D8InterfaceId::Direct3D8: return D3D9InterfaceId::Direct3D9;
    case D3D8InterfaceId::Device: return D3D9InterfaceId::Device;
    case D3D8InterfaceId::SwapChain: return D3D9InterfaceId::SwapChain;
    case D3D8InterfaceId::Surface: return D3D9InterfaceId::Surface;
    case D3D8InterfaceId::Volume: return D3D9InterfaceId::Volume;
    case D3D8InterfaceId::Texture: return D3D9InterfaceId::Texture;
    case D3D8InterfaceId::CubeTexture: return D3D9InterfaceId::CubeTexture;
    case D3D8InterfaceId::VolumeTexture: return D3D9InterfaceId::VolumeTexture;
    case D3D8InterfaceId::VertexBuffer: return D3D9InterfaceId::VertexBuffer;
    case D3D8InterfaceId::IndexBuffer: return D3D9InterfaceId::IndexBuffer;
    default: return D3D9InterfaceId::Direct3D9;
    }
}

}  // namespace

// =============================================================================
// Tables
// =============================================================================

const D3D8MethodInfo* d3d8_interface_methods(D3D8InterfaceId iface,
                                             usize& count) noexcept {
    const auto index = static_cast<usize>(iface);
    if (index >= D3D8_INTERFACE_COUNT) {
        count = 0;
        return nullptr;
    }
    count = kInterfaces[index].count;
    return kInterfaces[index].methods;
}

usize d3d8_interface_method_count(D3D8InterfaceId iface) noexcept {
    usize count = 0;
    (void)d3d8_interface_methods(iface, count);
    return count;
}

const char* d3d8_interface_name(D3D8InterfaceId iface) noexcept {
    const auto index = static_cast<usize>(iface);
    return index < D3D8_INTERFACE_COUNT ? kInterfaces[index].name : "?";
}

std::string d3d8_method_export_name(D3D8InterfaceId iface, usize slot) {
    usize count = 0;
    const D3D8MethodInfo* methods = d3d8_interface_methods(iface, count);
    std::string name = "_D3D8_";
    name += d3d8_interface_name(iface);
    name += '_';
    name += slot < count ? methods[slot].name : "?";
    return name;
}

void d3d8_log_unimplemented_once(const char* api, const char* what) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::string key = std::string(api) + ": " + what;
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen.insert(key).second) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED d3d8.dll!%s\n", key.c_str());
}

namespace d3d8 {
usize d3d9_slot(D3D9InterfaceId iface, const char* name) {
    usize count = 0;
    const D3D9MethodDesc* methods = d3d9_interface_methods(iface, count);
    for (usize i = 0; i < count; ++i) {
        if (std::strcmp(methods[i].name, name) == 0) return i;
    }
    std::fprintf(stderr, "[d3d8] no D3D9 %s::%s slot\n",
                 d3d9_interface_name(iface), name);
    std::abort();
}
}  // namespace d3d8

// =============================================================================
// Nested D3D9 calls
// =============================================================================

class D3D8Manager::Nested {
public:
    explicit Nested(GuestMemory& memory) : marshaler(memory) {
        cpu.init_protected_mode();
        marshaler.attach_cpu(cpu);
    }
    cpu::CpuState cpu{};
    Marshaler marshaler;
};

D3D8Manager::D3D8Manager() = default;
D3D8Manager::~D3D8Manager() = default;

void D3D8Manager::set_d3d9(D3D9Manager* d3d9) {
    m_d3d9 = d3d9;
    if (m_d3d9 != nullptr) {
        m_d3d9->set_object_destroyed_callback(
            [this](u32 object) { on_d3d9_destroyed(object); });
    }
}

void D3D8Manager::set_interface_thunks(D3D8InterfaceId iface,
                                       InterfaceThunks thunks) {
    const auto index = static_cast<usize>(iface);
    if (index < D3D8_INTERFACE_COUNT) m_thunks[index] = std::move(thunks);
}

u32 D3D8Manager::call9v(D3D9InterfaceId iface, usize slot, const u32* args,
                        usize count) {
    const usize level = m_call_level;
    while (m_nested.size() <= level) {
        m_nested.push_back(std::make_unique<Nested>(*m_memory));
    }
    Nested& frame = *m_nested[level];
    const GuestAddr saved_sp = m_frame_sp;
    // Return address slot, then the stdcall arguments, `this` first.
    const GuestAddr sp = (saved_sp - 4u * (count + 1u)) & ~GuestAddr{0xF};
    wr(sp, 0);
    for (usize index = 0; index < count; ++index) wr(sp + 4u + 4u * index, args[index]);
    m_frame_sp = sp - 0x40;
    ++m_call_level;
    frame.cpu.gpr.rsp = sp;
    frame.cpu.gpr.rax = 0;
    m_d3d9->impl_method(frame.marshaler, iface, slot);
    --m_call_level;
    m_frame_sp = saved_sp;
    return static_cast<u32>(frame.cpu.gpr.rax);
}

GuestAddr D3D8Manager::scratch(usize bytes) {
    const usize size = (bytes + 15u) & ~usize{15};
    if (m_scratch_cursor + size > m_region_base + SCRATCH_END) return 0;
    const GuestAddr addr = m_scratch_cursor;
    m_scratch_cursor += size;
    zero_guest(addr, size);
    return addr;
}

u32 D3D8Manager::rd(GuestAddr addr) const {
    const auto value = m_memory->read_u32(addr);
    return value.is_ok() ? value.value() : 0;
}

void D3D8Manager::wr(GuestAddr addr, u32 value) {
    (void)m_memory->write_u32(addr, value);
}

void D3D8Manager::copy_guest(GuestAddr dst, GuestAddr src, usize bytes) {
    if (bytes == 0) return;
    std::vector<Byte> buffer(bytes);
    if (m_memory->read_bytes(src, buffer.data(), bytes).is_err()) return;
    (void)m_memory->write_bytes(dst, buffer.data(), bytes);
}

void D3D8Manager::zero_guest(GuestAddr dst, usize bytes) {
    if (bytes == 0) return;
    std::vector<Byte> zeros(bytes, Byte{0});
    (void)m_memory->write_bytes(dst, zeros.data(), bytes);
}

// =============================================================================
// Object model
// =============================================================================

bool D3D8Manager::ensure_storage() {
    if (m_storage_ready) return true;
    if (m_memory == nullptr || m_d3d9 == nullptr) return false;
    GuestAddr minimum = STORAGE_MIN;
    while (minimum < STORAGE_MAX) {
        const GuestAddr base = m_memory->find_free_gap(minimum, STORAGE_SIZE);
        if (base == 0 || base >= STORAGE_MAX) return false;
        if (m_memory->map_region(base, STORAGE_SIZE, MemoryProtection::ReadWrite)
                .is_ok()) {
            m_region_base = base;
            break;
        }
        minimum = base + STORAGE_SIZE;
    }
    if (m_region_base == 0) return false;
    m_next_object = m_region_base + VTABLE_AREA;
    m_frame_sp = m_region_base + STACK_TOP;
    m_scratch_cursor = m_region_base + SCRATCH_BEGIN;
    m_storage_ready = true;
    return true;
}

bool D3D8Manager::map_vtable(D3D8InterfaceId iface) {
    const auto index = static_cast<usize>(iface);
    if (index >= D3D8_INTERFACE_COUNT) return false;
    if (m_vtables[index] != 0) return true;
    if (!ensure_storage()) return false;
    const usize count = kInterfaces[index].count;
    const InterfaceThunks& thunks = m_thunks[index];
    if (thunks.size() != count ||
        std::any_of(thunks.begin(), thunks.end(),
                    [](GuestAddr thunk) { return thunk == 0; })) {
        return false;
    }
    GuestAddr cursor = m_region_base;
    for (usize i = 0; i < index; ++i) cursor += kInterfaces[i].count * 4u;
    for (usize slot = 0; slot < count; ++slot) {
        wr(cursor + slot * 4u, static_cast<u32>(thunks[slot]));
    }
    m_vtables[index] = cursor;
    return true;
}

u32 D3D8Manager::wrap_as(u32 object9, D3D8InterfaceId iface) {
    if (object9 == 0) return 0;
    {
        std::lock_guard<std::mutex> lock(m_hook_mutex);
        const auto found = m_by9.find(object9);
        if (found != m_by9.end()) return found->second;
    }
    if (!map_vtable(iface)) return 0;
    GuestAddr address = 0;
    if (!m_free_objects.empty()) {
        address = m_free_objects.back();
        m_free_objects.pop_back();
    } else if (m_next_object + OBJECT_SIZE <= m_region_base + OBJECT_END) {
        address = m_next_object;
        m_next_object += OBJECT_SIZE;
    } else {
        return 0;
    }
    zero_guest(address, OBJECT_SIZE);
    wr(address, static_cast<u32>(m_vtables[static_cast<usize>(iface)]));
    Object object;
    object.iface = iface;
    object.d3d9 = object9;
    if (iface == D3D8InterfaceId::Surface || iface == D3D8InterfaceId::Volume) {
        // A level of a texture: find the container whose count it shares.
        const bool surface = iface == D3D8InterfaceId::Surface;
        const GuestAddr riid = scratch(16);
        const GuestAddr out = scratch(4);
        const std::array<u8, 16>& iid = surface ? d3d9h::kIID_IDirect3DBaseTexture9
                                                : d3d9h::kIID_IDirect3DVolumeTexture9;
        if (riid != 0 && out != 0) {
            (void)m_memory->write_bytes(riid, reinterpret_cast<const Byte*>(iid.data()), 16);
            const D3D9InterfaceId self9 =
                surface ? D3D9InterfaceId::Surface : D3D9InterfaceId::Volume;
            const u32 hr = call9(self9, surface ? D3D8_SLOT9(Surface, GetContainer)
                                                : D3D8_SLOT9(Volume, GetContainer),
                                 {object9, static_cast<u32>(riid), static_cast<u32>(out)});
            const u32 container = rd(out);
            D3D9InterfaceId container_iface{};
            if (!failed(hr) && container != 0 &&
                m_d3d9->object_interface(container, container_iface)) {
                if (container_iface == D3D9InterfaceId::Texture ||
                    container_iface == D3D9InterfaceId::CubeTexture ||
                    container_iface == D3D9InterfaceId::VolumeTexture) {
                    object.container9 = container;
                    object.container_iface = container_iface;
                }
                (void)call9(container_iface, 2, {container});
            }
        }
    }
    m_objects[static_cast<u32>(address)] = std::move(object);
    std::lock_guard<std::mutex> lock(m_hook_mutex);
    m_by9[object9] = static_cast<u32>(address);
    return static_cast<u32>(address);
}

u32 D3D8Manager::wrap(u32 object9) {
    if (object9 == 0) return 0;
    D3D9InterfaceId iface9{};
    D3D8InterfaceId iface8{};
    if (!m_d3d9->object_interface(object9, iface9) || !counterpart(iface9, iface8)) {
        return 0;
    }
    return wrap_as(object9, iface8);
}

D3D8Manager::Object* D3D8Manager::find(u32 object8) {
    const auto found = m_objects.find(object8);
    if (found == m_objects.end() || found->second.d3d9 == 0) return nullptr;
    return &found->second;
}

D3D8Manager::Object* D3D8Manager::find(u32 object8, D3D8InterfaceId iface) {
    Object* object = find(object8);
    return object != nullptr && object->iface == iface ? object : nullptr;
}

u32 D3D8Manager::unwrap(u32 object8) {
    const Object* object = find(object8);
    return object != nullptr ? object->d3d9 : 0;
}

u32 D3D8Manager::backing_object(u32 object8) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return unwrap(object8);
}

void D3D8Manager::on_d3d9_destroyed(u32 object9) {
    std::lock_guard<std::mutex> lock(m_hook_mutex);
    const auto found = m_by9.find(object9);
    if (found == m_by9.end()) return;
    m_dead.push_back(found->second);
    m_by9.erase(found);
}

void D3D8Manager::finalize_dead() {
    for (;;) {
        std::vector<u32> dead;
        {
            std::lock_guard<std::mutex> lock(m_hook_mutex);
            dead.swap(m_dead);
        }
        if (dead.empty()) return;
        for (const u32 object8 : dead) free_object(object8);
    }
}

void D3D8Manager::free_object(u32 object8) {
    const auto found = m_objects.find(object8);
    if (found == m_objects.end()) return;
    std::unique_ptr<DeviceState> device = std::move(found->second.device);
    m_objects.erase(found);
    // The vtable stays: a stale pointer the guest still holds then names no
    // object, and its AddRef/Release return 0 instead of faulting.
    m_free_objects.push_back(object8);
    if (device) release_device_handles(*device);
}

void D3D8Manager::return_wrapped(GuestAddr out8, GuestAddr out9, u32 hr) {
    if (out8 == 0) return;
    u32 result = 0;
    if (!failed(hr) && out9 != 0) {
        const u32 object9 = rd(out9);
        result = wrap(object9);
        if (object9 != 0 && result == 0) {
            (void)call9(D3D9InterfaceId::Direct3D9, 2, {object9});
        }
        const Object* object = result != 0 ? find(result) : nullptr;
        if (object != nullptr && object->container9 != 0) {
            // The reference the guest receives is one on the container.
            const u32 container = object->container9;
            const D3D9InterfaceId container_iface = object->container_iface;
            D3D9InterfaceId iface9{};
            (void)call9(container_iface, 1, {container});
            if (m_d3d9->object_interface(object9, iface9)) {
                (void)call9(iface9, 2, {object9});
            }
        }
    }
    wr(out8, result);
}

// =============================================================================
// Dispatch
// =============================================================================

void D3D8Manager::impl_method(Marshaler& m, D3D8InterfaceId iface, usize slot) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const u32 self = static_cast<u32>(m.arg_ptr(0));
    Object* object = m_storage_ready ? find(self, iface) : nullptr;
    if (object == nullptr || slot >= d3d8_interface_method_count(iface)) {
        // AddRef/Release must not fault on an unknown pointer.
        m.set_return(slot == 1 || slot == 2 ? 0 : D3DERR_INVALIDCALL);
        return;
    }
    if (m_depth++ == 0) m_scratch_cursor = m_region_base + SCRATCH_BEGIN;
    switch (iface) {
    case D3D8InterfaceId::Direct3D8: dispatch_direct3d8(m, slot, self, *object); break;
    case D3D8InterfaceId::Device: dispatch_device(m, slot, self, *object); break;
    case D3D8InterfaceId::SwapChain: dispatch_swap_chain(m, slot, self, *object); break;
    case D3D8InterfaceId::Surface: dispatch_surface(m, slot, self, *object); break;
    case D3D8InterfaceId::Volume: dispatch_volume(m, slot, self, *object); break;
    case D3D8InterfaceId::Texture:
    case D3D8InterfaceId::CubeTexture:
    case D3D8InterfaceId::VolumeTexture:
        dispatch_texture(m, slot, self, *object);
        break;
    case D3D8InterfaceId::VertexBuffer:
    case D3D8InterfaceId::IndexBuffer:
        dispatch_buffer(m, slot, self, *object);
        break;
    default:
        m.set_return(D3DERR_INVALIDCALL);
        break;
    }
    VITREO_TRACE("[d3d8] %s this=0x%08x ret=0x%08llx -> 0x%08llx\n",
                 d3d8_method_export_name(iface, slot).c_str(), self,
                 static_cast<unsigned long long>(m.guest_return_address()),
                 static_cast<unsigned long long>(m.return_value()));
    if (--m_depth == 0) finalize_dead();
}

bool D3D8Manager::dispatch_common(Marshaler& m, usize slot, u32 self, Object& obj) {
    const D3D9InterfaceId iface9 = backing_interface(obj.iface);
    switch (slot) {
    case 0: {  // QueryInterface(REFIID, void**)
        const GuestAddr riid = m.arg_ptr(1);
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(d3d8::E_POINTER);
            return true;
        }
        std::array<u8, 16> iid{};
        if (riid == 0 ||
            m_memory->read_bytes(riid, reinterpret_cast<Byte*>(iid.data()), 16)
                .is_err()) {
            wr(out, 0);
            m.set_return(D3DERR_INVALIDCALL);
            return true;
        }
        static const std::array<u8, 16> kUnknown = {
            0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
        const bool match =
            iid == kUnknown ||
            iid == *kInterfaces[static_cast<usize>(obj.iface)].iid ||
            (is_resource(obj.iface) && iid == kIID_IDirect3DResource8) ||
            (is_texture(obj.iface) && iid == kIID_IDirect3DBaseTexture8);
        if (!match) {
            wr(out, 0);
            m.set_return(d3d8::E_NOINTERFACE);
            return true;
        }
        if (obj.container9 != 0) {
            (void)call9(obj.container_iface, 1, {obj.container9});
        } else {
            (void)call9(iface9, 1, {obj.d3d9});
        }
        wr(out, self);
        m.set_return(D3D_OK);
        return true;
    }
    case 1:  // AddRef
        m.set_return(obj.container9 != 0
                         ? call9(obj.container_iface, 1, {obj.container9})
                         : call9(iface9, 1, {obj.d3d9}));
        return true;
    case 2:  // Release
        m.set_return(obj.container9 != 0
                         ? call9(obj.container_iface, 2, {obj.container9})
                         : call9(iface9, 2, {obj.d3d9}));
        return true;
    default:
        break;
    }
    // IDirect3DResource8 (resources) and the GetDevice/PrivateData group of
    // IDirect3DSurface8 / IDirect3DVolume8, which keep the same order.
    const bool resource_slots = is_resource(obj.iface) ||
                                obj.iface == D3D8InterfaceId::Surface ||
                                obj.iface == D3D8InterfaceId::Volume;
    if (!resource_slots) return false;
    const usize last = is_resource(obj.iface) ? 10 : 6;
    if (slot < 3 || slot > last) return false;
    switch (slot) {
    case 3: {  // GetDevice(IDirect3DDevice8**)
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return true;
        }
        const GuestAddr out9 = scratch(4);
        const u32 hr = call9(iface9, 3, {obj.d3d9, static_cast<u32>(out9)});
        return_wrapped(out, out9, hr);
        m.set_return(hr);
        return true;
    }
    case 4: {  // SetPrivateData(REFGUID, const void*, DWORD, DWORD)
        m.set_return(call9(iface9, 4,
                           {obj.d3d9, m.arg32(1), m.arg32(2), m.arg32(3), m.arg32(4)}));
        return true;
    }
    case 5:  // GetPrivateData(REFGUID, void*, DWORD*)
        m.set_return(call9(iface9, 5, {obj.d3d9, m.arg32(1), m.arg32(2), m.arg32(3)}));
        return true;
    case 6:  // FreePrivateData(REFGUID)
        m.set_return(call9(iface9, 6, {obj.d3d9, m.arg32(1)}));
        return true;
    case 7:  // SetPriority(DWORD)
        m.set_return(call9(iface9, 7, {obj.d3d9, m.arg32(1)}));
        return true;
    case 8:  // GetPriority
        m.set_return(call9(iface9, 8, {obj.d3d9}));
        return true;
    case 9:  // PreLoad
        (void)call9(iface9, 9, {obj.d3d9});
        return true;
    case 10:  // GetType
        m.set_return(call9(iface9, 10, {obj.d3d9}));
        return true;
    default:
        return false;
    }
}

// =============================================================================
// IDirect3D8
// =============================================================================

u32 D3D8Manager::direct3d_create8(u32 sdk_version) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Measured (tools/oracle_probe/d3d8_contract_probe.c, Windows 11
    // SysWOW64 d3d8.dll): 120 (DirectX 8.0) and 220 (D3D_SDK_VERSION, 8.1)
    // succeed; 0, 32 and 120 with the debug bit set return NULL.
    if (sdk_version != 120u && sdk_version != d3d8c::D3D_SDK_VERSION) return 0;
    if (m_is_64bit) {
        // There is no 64-bit d3d8.dll on Windows.
        d3d8_log_unimplemented_once("Direct3DCreate8", "x64 guest");
        return 0;
    }
    if (m_d3d9 == nullptr || !ensure_storage()) return 0;
    const u32 d3d9 = m_d3d9->direct3d_create9(d3d8::D3D9_SDK_VERSION);
    if (d3d9 == 0) return 0;
    const u32 object = wrap_as(d3d9, D3D8InterfaceId::Direct3D8);
    if (object == 0) {
        if (m_depth == 0) m_scratch_cursor = m_region_base + SCRATCH_BEGIN;
        (void)call9(D3D9InterfaceId::Direct3D9, 2, {d3d9});
        finalize_dead();
    }
    return object;
}

void D3D8Manager::dispatch_direct3d8(Marshaler& m, usize slot, u32 self, Object& obj) {
    if (dispatch_common(m, slot, self, obj)) return;
    const u32 d9 = obj.d3d9;
    using S = Direct3DSlot8;
    switch (static_cast<S>(slot)) {
    case S::RegisterSoftwareDevice:
        // Measured: a NULL initialization function is D3DERR_INVALIDCALL.
        if (m.arg32(1) == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(call9(D3D9InterfaceId::Direct3D9,
                           D3D8_SLOT9(Direct3D9, RegisterSoftwareDevice),
                           {d9, m.arg32(1)}));
        return;
    case S::GetAdapterCount:
        m.set_return(call9(D3D9InterfaceId::Direct3D9,
                           D3D8_SLOT9(Direct3D9, GetAdapterCount), {d9}));
        return;
    case S::GetAdapterIdentifier: {
        const u32 adapter = m.arg32(1);
        const u32 flags = m.arg32(2);
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // D3D8 computes the WHQL level unless D3DENUM_NO_WHQL_LEVEL is set;
        // D3D9 computes it only when D3DENUM_WHQL_LEVEL (the same bit) is.
        // Other bits go through for D3D9 to validate.
        const u32 flags9 = (flags & ~D3DENUM_NO_WHQL_LEVEL) |
                           ((flags & D3DENUM_NO_WHQL_LEVEL) != 0
                                ? 0u
                                : d3d9h::D3DENUM_WHQL_LEVEL);
        const GuestAddr id9 = scratch(layout_ADAPTER_IDENTIFIER8::SIZE9);
        const u32 hr = call9(D3D9InterfaceId::Direct3D9,
                             D3D8_SLOT9(Direct3D9, GetAdapterIdentifier),
                             {d9, adapter, flags9, static_cast<u32>(id9)});
        if (!failed(hr)) {
            namespace L = layout_ADAPTER_IDENTIFIER8;
            copy_guest(at(out, L::Driver.d3d8), at(id9, L::Driver.d3d9), 512);
            copy_guest(at(out, L::Description.d3d8), at(id9, L::Description.d3d9), 512);
            copy_guest(at(out, L::DriverVersion.d3d8), at(id9, L::DriverVersion.d3d9), 8);
            for (const FieldPair field : {L::VendorId, L::DeviceId, L::SubSysId,
                                          L::Revision, L::WHQLLevel}) {
                wr(at(out, field.d3d8), rd(at(id9, field.d3d9)));
            }
            copy_guest(at(out, L::DeviceIdentifier.d3d8),
                       at(id9, L::DeviceIdentifier.d3d9), 16);
        }
        m.set_return(hr);
        return;
    }
    case S::GetAdapterModeCount: {
        std::vector<std::array<u32, 4>> modes;
        (void)get_adapter_mode_list(d9, m.arg32(1), modes);
        m.set_return(static_cast<u32>(modes.size()));
        return;
    }
    case S::EnumAdapterModes: {
        const u32 mode = m.arg32(2);
        const GuestAddr out = m.arg_ptr(3);
        std::vector<std::array<u32, 4>> modes;
        const u32 hr = get_adapter_mode_list(d9, m.arg32(1), modes);
        if (failed(hr) || out == 0 || mode >= modes.size()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        for (usize i = 0; i < 4; ++i) wr(out + 4u * i, modes[mode][i]);
        m.set_return(D3D_OK);
        return;
    }
    case S::GetAdapterDisplayMode:
        m.set_return(call9(D3D9InterfaceId::Direct3D9,
                           D3D8_SLOT9(Direct3D9, GetAdapterDisplayMode),
                           {d9, m.arg32(1), m.arg32(2)}));
        return;
    case S::CheckDeviceType: {
        // Measured (d3d8_contract_probe): D3D8 has no windowed
        // D3DFMT_UNKNOWN display format (INVALIDCALL), and a display format
        // with alpha is not a display mode (NOTAVAILABLE). The display
        // formats are Learn's list without the alpha variants D3D9 folds in.
        const u32 display = m.arg32(3);
        if (display == fmt8::D3DFMT_UNKNOWN) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (display != fmt8::D3DFMT_X8R8G8B8 && display != fmt8::D3DFMT_R5G6B5 &&
            display != fmt8::D3DFMT_X1R5G5B5) {
            m.set_return(d3d8::D3DERR_NOTAVAILABLE);
            return;
        }
        m.set_return(call9(D3D9InterfaceId::Direct3D9,
                           D3D8_SLOT9(Direct3D9, CheckDeviceType),
                           {d9, m.arg32(1), device_type_to9(m.arg32(2)), m.arg32(3),
                            m.arg32(4), m.arg32(5)}));
        return;
    }
    case S::CheckDeviceFormat:
        m.set_return(call9(D3D9InterfaceId::Direct3D9,
                           D3D8_SLOT9(Direct3D9, CheckDeviceFormat),
                           {d9, m.arg32(1), device_type_to9(m.arg32(2)), m.arg32(3),
                            m.arg32(4), m.arg32(5), m.arg32(6)}));
        return;
    case S::CheckDeviceMultiSampleType:
        m.set_return(call9(D3D9InterfaceId::Direct3D9,
                           D3D8_SLOT9(Direct3D9, CheckDeviceMultiSampleType),
                           {d9, m.arg32(1), device_type_to9(m.arg32(2)), m.arg32(3),
                            m.arg32(4), m.arg32(5), 0u}));
        return;
    case S::CheckDepthStencilMatch:
        m.set_return(call9(D3D9InterfaceId::Direct3D9,
                           D3D8_SLOT9(Direct3D9, CheckDepthStencilMatch),
                           {d9, m.arg32(1), device_type_to9(m.arg32(2)), m.arg32(3),
                            m.arg32(4), m.arg32(5)}));
        return;
    case S::GetDeviceCaps: {
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const GuestAddr caps9 = scratch(layout_CAPS8::SIZE9);
        const u32 hr = call9(D3D9InterfaceId::Direct3D9,
                             D3D8_SLOT9(Direct3D9, GetDeviceCaps),
                             {d9, m.arg32(1), device_type_to9(m.arg32(2)),
                              static_cast<u32>(caps9)});
        if (!failed(hr)) {
            caps_from9(caps9, out);
            wr(at(out, layout_CAPS8::DeviceType.d3d8), m.arg32(2));
        }
        m.set_return(hr);
        return;
    }
    case S::GetAdapterMonitor:
        m.set_return(call9(D3D9InterfaceId::Direct3D9,
                           D3D8_SLOT9(Direct3D9, GetAdapterMonitor),
                           {d9, m.arg32(1)}));
        return;
    case S::CreateDevice: {
        const u32 adapter = m.arg32(1);
        const u32 type = m.arg32(2);
        const u32 focus = m.arg32(3);
        const u32 flags = m.arg32(4);
        const GuestAddr pp8 = m.arg_ptr(5);
        const GuestAddr out = m.arg_ptr(6);
        if (out == 0 || pp8 == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        wr(out, 0);
        u32 hr = D3D_OK;
        const GuestAddr pp9 = present_parameters_to9(pp8, hr);
        if (pp9 == 0) {
            m.set_return(hr);
            return;
        }
        const GuestAddr out9 = scratch(4);
        hr = call9(D3D9InterfaceId::Direct3D9, D3D8_SLOT9(Direct3D9, CreateDevice),
                   {d9, adapter, device_type_to9(type), focus, flags,
                    static_cast<u32>(pp9), static_cast<u32>(out9)});
        if (failed(hr)) {
            m.set_return(hr);
            return;
        }
        present_parameters_from9(pp9, pp8);
        const u32 device9 = rd(out9);
        const u32 device8 = wrap_as(device9, D3D8InterfaceId::Device);
        Object* device = find(device8, D3D8InterfaceId::Device);
        if (device == nullptr) {
            (void)call9(D3D9InterfaceId::Device, 2, {device9});
            m.set_return(d3d8::E_OUTOFMEMORY);
            return;
        }
        device->device = std::make_unique<DeviceState>();
        DeviceState& state = *device->device;
        state.direct3d8 = self;
        state.behavior_flags = flags;
        state.device_type = type;
        // D3D8-only render states at their creation values.
        state.render_states8[10] = 0;    // LINEPATTERN
        state.render_states8[30] = 0;    // ZVISIBLE
        state.render_states8[40] = 0;    // EDGEANTIALIAS
        state.render_states8[47] = 0;    // ZBIAS
        state.render_states8[153] =      // SOFTWAREVERTEXPROCESSING
            (flags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0 ? 1u : 0u;
        state.render_states8[164] = 0x3F800000u;  // PATCHSEGMENTS = 1.0f
        apply_creation_defaults(*device);
        wr(out, device8);
        m.set_return(hr);
        return;
    }
    default:
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
}

// =============================================================================
// IDirect3DSwapChain8
// =============================================================================

void D3D8Manager::dispatch_swap_chain(Marshaler& m, usize slot, u32 self,
                                      Object& obj) {
    if (dispatch_common(m, slot, self, obj)) return;
    using S = SwapChainSlot8;
    switch (static_cast<S>(slot)) {
    case S::Present:
        m.set_return(call9(D3D9InterfaceId::SwapChain,
                           D3D8_SLOT9(SwapChain, Present),
                           {obj.d3d9, m.arg32(1), m.arg32(2), m.arg32(3),
                            m.arg32(4), 0u}));
        return;
    case S::GetBackBuffer: {
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const GuestAddr out9 = scratch(4);
        const u32 hr = call9(D3D9InterfaceId::SwapChain,
                             D3D8_SLOT9(SwapChain, GetBackBuffer),
                             {obj.d3d9, m.arg32(1), m.arg32(2),
                              static_cast<u32>(out9)});
        return_wrapped(out, out9, hr);
        m.set_return(hr);
        return;
    }
    default:
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
}

// =============================================================================
// IDirect3DSurface8 / IDirect3DVolume8
// =============================================================================

namespace {
/// GetContainer: the D3D9 IID for a D3D8 IID read from the guest.
bool read_iid(GuestMemory& memory, GuestAddr riid, std::array<u8, 16>& iid) {
    return riid != 0 &&
           memory.read_bytes(riid, reinterpret_cast<Byte*>(iid.data()), 16).is_ok();
}
}  // namespace

u32 D3D8Manager::get_container(Object& obj, GuestAddr riid, GuestAddr out) {
    // GetContainer(riid, ppContainer): the texture that owns a level, or the
    // device for a surface no texture owns, if it answers riid (measured:
    // a level answers IDirect3DTexture8 and refuses IDirect3DDevice8 with
    // E_NOINTERFACE; the back buffer and an image surface answer
    // IDirect3DDevice8).
    std::array<u8, 16> iid{};
    if (out == 0 || !read_iid(*m_memory, riid, iid)) return D3DERR_INVALIDCALL;
    wr(out, 0);
    u32 container8 = 0;
    D3D9InterfaceId container_iface = D3D9InterfaceId::Device;
    u32 container9 = 0;
    if (obj.container9 != 0) {
        container9 = obj.container9;
        container_iface = obj.container_iface;
        container8 = wrap(container9);
    } else {
        const GuestAddr out9 = scratch(4);
        const D3D9InterfaceId self9 = obj.iface == D3D8InterfaceId::Surface
                                          ? D3D9InterfaceId::Surface
                                          : D3D9InterfaceId::Volume;
        if (failed(call9(self9, 3 /* GetDevice */, {obj.d3d9, static_cast<u32>(out9)}))) {
            return D3DERR_INVALIDCALL;
        }
        container9 = rd(out9);
        (void)call9(D3D9InterfaceId::Device, 2, {container9});
        container8 = wrap(container9);
    }
    Object* container = container8 != 0 ? find(container8) : nullptr;
    if (container == nullptr) return D3DERR_INVALIDCALL;
    static const std::array<u8, 16> kUnknown = {0, 0, 0, 0, 0, 0, 0, 0,
                                                0xC0, 0, 0, 0, 0, 0, 0, 0x46};
    const bool match =
        iid == kUnknown || iid == *kInterfaces[static_cast<usize>(container->iface)].iid ||
        (is_resource(container->iface) && iid == kIID_IDirect3DResource8) ||
        (is_texture(container->iface) && iid == kIID_IDirect3DBaseTexture8);
    if (!match) return d3d8::E_NOINTERFACE;
    (void)call9(container_iface, 1, {container9});
    wr(out, container8);
    return D3D_OK;
}

void D3D8Manager::dispatch_surface(Marshaler& m, usize slot, u32 self, Object& obj) {
    if (dispatch_common(m, slot, self, obj)) return;
    using S = SurfaceSlot8;
    switch (static_cast<S>(slot)) {
    case S::GetContainer:
        m.set_return(get_container(obj, m.arg_ptr(1), m.arg_ptr(2)));
        return;
    case S::GetDesc: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const GuestAddr desc9 = scratch(layout_SURFACE_DESC::SIZE9);
        u32 hr = call9(D3D9InterfaceId::Surface, D3D8_SLOT9(Surface, GetDesc),
                       {obj.d3d9, static_cast<u32>(desc9)});
        if (!failed(hr)) hr = surface_desc_from9(desc9, out);
        m.set_return(hr);
        return;
    }
    case S::LockRect:
        m.set_return(call9(D3D9InterfaceId::Surface, D3D8_SLOT9(Surface, LockRect),
                           {obj.d3d9, m.arg32(1), m.arg32(2), m.arg32(3)}));
        return;
    case S::UnlockRect:
        m.set_return(call9(D3D9InterfaceId::Surface, D3D8_SLOT9(Surface, UnlockRect),
                           {obj.d3d9}));
        return;
    default:
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
}

void D3D8Manager::dispatch_volume(Marshaler& m, usize slot, u32 self, Object& obj) {
    if (dispatch_common(m, slot, self, obj)) return;
    using S = VolumeSlot8;
    switch (static_cast<S>(slot)) {
    case S::GetContainer:
        m.set_return(get_container(obj, m.arg_ptr(1), m.arg_ptr(2)));
        return;
    case S::GetDesc: {
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const GuestAddr desc9 = scratch(layout_VOLUME_DESC::SIZE9);
        u32 hr = call9(D3D9InterfaceId::Volume, D3D8_SLOT9(Volume, GetDesc),
                       {obj.d3d9, static_cast<u32>(desc9)});
        if (!failed(hr)) hr = volume_desc_from9(desc9, out);
        m.set_return(hr);
        return;
    }
    case S::LockBox:
        m.set_return(call9(D3D9InterfaceId::Volume, D3D8_SLOT9(Volume, LockBox),
                           {obj.d3d9, m.arg32(1), m.arg32(2), m.arg32(3)}));
        return;
    case S::UnlockBox:
        m.set_return(call9(D3D9InterfaceId::Volume, D3D8_SLOT9(Volume, UnlockBox),
                           {obj.d3d9}));
        return;
    default:
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
}

// =============================================================================
// Textures
// =============================================================================

void D3D8Manager::dispatch_texture(Marshaler& m, usize slot, u32 self, Object& obj) {
    if (dispatch_common(m, slot, self, obj)) return;
    const D3D9InterfaceId i9 = backing_interface(obj.iface);
    const u32 t9 = obj.d3d9;
    // IDirect3DBaseTexture8: SetLOD, GetLOD, GetLevelCount (slots 11..13).
    switch (slot) {
    case 11:
        m.set_return(call9(i9, D3D8_SLOT9(Texture, SetLOD), {t9, m.arg32(1)}));
        return;
    case 12:
        m.set_return(call9(i9, D3D8_SLOT9(Texture, GetLOD), {t9}));
        return;
    case 13:
        m.set_return(call9(i9, D3D8_SLOT9(Texture, GetLevelCount), {t9}));
        return;
    default:
        break;
    }
    if (obj.iface == D3D8InterfaceId::Texture) {
        using S = TextureSlot8;
        switch (static_cast<S>(slot)) {
        case S::GetLevelDesc: {
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            const GuestAddr desc9 = scratch(layout_SURFACE_DESC::SIZE9);
            u32 hr = call9(i9, D3D8_SLOT9(Texture, GetLevelDesc),
                           {t9, m.arg32(1), static_cast<u32>(desc9)});
            if (!failed(hr)) hr = surface_desc_from9(desc9, out);
            m.set_return(hr);
            return;
        }
        case S::GetSurfaceLevel: {
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            const GuestAddr out9 = scratch(4);
            const u32 hr = call9(i9, D3D8_SLOT9(Texture, GetSurfaceLevel),
                                 {t9, m.arg32(1), static_cast<u32>(out9)});
            return_wrapped(out, out9, hr);
            m.set_return(hr);
            return;
        }
        case S::LockRect:
            m.set_return(call9(i9, D3D8_SLOT9(Texture, LockRect),
                               {t9, m.arg32(1), m.arg32(2), m.arg32(3), m.arg32(4)}));
            return;
        case S::UnlockRect:
            m.set_return(call9(i9, D3D8_SLOT9(Texture, UnlockRect), {t9, m.arg32(1)}));
            return;
        case S::AddDirtyRect:
            m.set_return(call9(i9, D3D8_SLOT9(Texture, AddDirtyRect), {t9, m.arg32(1)}));
            return;
        default:
            break;
        }
    } else if (obj.iface == D3D8InterfaceId::CubeTexture) {
        using S = CubeTextureSlot8;
        switch (static_cast<S>(slot)) {
        case S::GetLevelDesc: {
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            const GuestAddr desc9 = scratch(layout_SURFACE_DESC::SIZE9);
            u32 hr = call9(i9, D3D8_SLOT9(CubeTexture, GetLevelDesc),
                           {t9, m.arg32(1), static_cast<u32>(desc9)});
            if (!failed(hr)) hr = surface_desc_from9(desc9, out);
            m.set_return(hr);
            return;
        }
        case S::GetCubeMapSurface: {
            const GuestAddr out = m.arg_ptr(3);
            if (out == 0) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            const GuestAddr out9 = scratch(4);
            const u32 hr = call9(i9, D3D8_SLOT9(CubeTexture, GetCubeMapSurface),
                                 {t9, m.arg32(1), m.arg32(2), static_cast<u32>(out9)});
            return_wrapped(out, out9, hr);
            m.set_return(hr);
            return;
        }
        case S::LockRect:
            m.set_return(call9(i9, D3D8_SLOT9(CubeTexture, LockRect),
                               {t9, m.arg32(1), m.arg32(2), m.arg32(3), m.arg32(4),
                                m.arg32(5)}));
            return;
        case S::UnlockRect:
            m.set_return(call9(i9, D3D8_SLOT9(CubeTexture, UnlockRect),
                               {t9, m.arg32(1), m.arg32(2)}));
            return;
        case S::AddDirtyRect:
            m.set_return(call9(i9, D3D8_SLOT9(CubeTexture, AddDirtyRect),
                               {t9, m.arg32(1), m.arg32(2)}));
            return;
        default:
            break;
        }
    } else {
        using S = VolumeTextureSlot8;
        switch (static_cast<S>(slot)) {
        case S::GetLevelDesc: {
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            const GuestAddr desc9 = scratch(layout_VOLUME_DESC::SIZE9);
            u32 hr = call9(i9, D3D8_SLOT9(VolumeTexture, GetLevelDesc),
                           {t9, m.arg32(1), static_cast<u32>(desc9)});
            if (!failed(hr)) hr = volume_desc_from9(desc9, out);
            m.set_return(hr);
            return;
        }
        case S::GetVolumeLevel: {
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            const GuestAddr out9 = scratch(4);
            const u32 hr = call9(i9, D3D8_SLOT9(VolumeTexture, GetVolumeLevel),
                                 {t9, m.arg32(1), static_cast<u32>(out9)});
            return_wrapped(out, out9, hr);
            m.set_return(hr);
            return;
        }
        case S::LockBox:
            m.set_return(call9(i9, D3D8_SLOT9(VolumeTexture, LockBox),
                               {t9, m.arg32(1), m.arg32(2), m.arg32(3), m.arg32(4)}));
            return;
        case S::UnlockBox:
            m.set_return(call9(i9, D3D8_SLOT9(VolumeTexture, UnlockBox),
                               {t9, m.arg32(1)}));
            return;
        case S::AddDirtyBox:
            m.set_return(call9(i9, D3D8_SLOT9(VolumeTexture, AddDirtyBox),
                               {t9, m.arg32(1)}));
            return;
        default:
            break;
        }
    }
    m.set_return(D3DERR_INVALIDCALL);
}

// =============================================================================
// Vertex / index buffers
// =============================================================================

void D3D8Manager::dispatch_buffer(Marshaler& m, usize slot, u32 self, Object& obj) {
    if (dispatch_common(m, slot, self, obj)) return;
    const D3D9InterfaceId i9 = backing_interface(obj.iface);
    // Lock(UINT OffsetToLock, UINT SizeToLock, BYTE**, DWORD), Unlock,
    // GetDesc: same order, arguments and descriptor layout as D3D9
    // (D3DVERTEXBUFFER_DESC / D3DINDEXBUFFER_DESC, d3d8_contract.inc).
    static_assert(layout_VERTEXBUFFER_DESC::SIZE8 == layout_VERTEXBUFFER_DESC::SIZE9);
    static_assert(layout_INDEXBUFFER_DESC::SIZE8 == layout_INDEXBUFFER_DESC::SIZE9);
    switch (slot) {
    case 11:
        m.set_return(call9(i9, D3D8_SLOT9(VertexBuffer, Lock),
                           {obj.d3d9, m.arg32(1), m.arg32(2), m.arg32(3), m.arg32(4)}));
        return;
    case 12:
        m.set_return(call9(i9, D3D8_SLOT9(VertexBuffer, Unlock), {obj.d3d9}));
        return;
    case 13:
        m.set_return(call9(i9, D3D8_SLOT9(VertexBuffer, GetDesc),
                           {obj.d3d9, m.arg32(1)}));
        return;
    default:
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
}

// =============================================================================
// Exports
// =============================================================================

DllExports create_d3d8_thunks(D3D8Manager& manager) {
    DllExports exports;
    // Ordinals and names from the Windows 11 SysWOW64 d3d8.dll export table:
    // 1 Direct3D8EnableMaximizedWindowedModeShim, 2 ValidatePixelShader,
    // 3 ValidateVertexShader, 4 DebugSetMute, 5 Direct3DCreate8.
    exports.entries.push_back(ExportEntry{
        "Direct3D8EnableMaximizedWindowedModeShim", 1,
        [](Marshaler& m) {
            (void)m.arg32(0);
            d3d8_log_unimplemented_once("Direct3D8EnableMaximizedWindowedModeShim",
                                        "undocumented application shim");
            m.set_return(d3d8::E_NOTIMPL);
        },
        4,
    });
    // HRESULT ValidatePixelShader(const DWORD*, const D3DCAPS8*, BOOL, char**)
    // and ValidateVertexShader(const DWORD*, const DWORD*, const D3DCAPS8*,
    // BOOL, char**): undocumented validators the D3DX8 assembler calls.
    exports.entries.push_back(ExportEntry{
        "ValidatePixelShader", 2,
        [](Marshaler& m) {
            const GuestAddr errors = m.arg_ptr(3);
            if (errors != 0) (void)m.write_ptr(errors, 0);
            d3d8_log_unimplemented_once("ValidatePixelShader", "undocumented validator");
            m.set_return(d3d8::E_FAIL);
        },
        16,
    });
    exports.entries.push_back(ExportEntry{
        "ValidateVertexShader", 3,
        [](Marshaler& m) {
            const GuestAddr errors = m.arg_ptr(4);
            if (errors != 0) (void)m.write_ptr(errors, 0);
            d3d8_log_unimplemented_once("ValidateVertexShader", "undocumented validator");
            m.set_return(d3d8::E_FAIL);
        },
        20,
    });
    // void DebugSetMute(void): silences the debug runtime's output. The
    // retail runtime emits none, so there is nothing to change.
    exports.entries.push_back(ExportEntry{
        "DebugSetMute", 4, [](Marshaler&) {}, 0,
    });
    exports.entries.push_back(ExportEntry{
        "Direct3DCreate8", 5,
        [&manager](Marshaler& m) {
            m.set_return(manager.direct3d_create8(m.arg32(0)));
        },
        4,
    });
    for (usize index = 0; index < D3D8_INTERFACE_COUNT; ++index) {
        const auto iface = static_cast<D3D8InterfaceId>(index);
        usize count = 0;
        const D3D8MethodInfo* methods = d3d8_interface_methods(iface, count);
        for (usize slot = 0; slot < count; ++slot) {
            exports.entries.push_back(ExportEntry{
                d3d8_method_export_name(iface, slot), 0,
                [&manager, iface, slot](Marshaler& m) {
                    manager.impl_method(m, iface, slot);
                },
                methods[slot].stdcall_bytes,
            });
        }
    }
    return exports;
}

}  // namespace vitreo::winapi
