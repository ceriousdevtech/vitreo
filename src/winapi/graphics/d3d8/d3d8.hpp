// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d8.hpp
/// @brief Direct3D 8 (d3d8.dll) guest COM frontend.
///
/// Direct3D 8 is the API Direct3D 9 grew out of: Microsoft Learn's
/// "Converting to Direct3D 9" lists every place the two differ, and
/// everything else (render states, transforms, lights, materials, viewports,
/// formats, resource locking, shader 1.x token streams) carried over
/// unchanged. So this frontend owns only what is Direct3D-8-visible:
///
/// * its own COM objects with the exact D3D8 vtables (d3d8.h order, x86
///   stdcall sizes), IIDs and QueryInterface answers;
/// * the D3D8 structure layouts (D3DCAPS8, D3DPRESENT_PARAMETERS with
///   FullScreen_PresentationInterval, D3DSURFACE_DESC with Size, ...);
/// * the D3D8-only contracts: handle-based vertex shaders whose declaration
///   tokens (D3DVSD_*) bind stream data to registers, handle-based pixel
///   shaders and state blocks, SetIndices' BaseVertexIndex, CopyRects,
///   texture-stage sampler states, ZBIAS and the other render states D3D9
///   removed, and adapter-mode enumeration across formats.
///
/// Each D3D8 object fronts one D3D9 object of the shared D3D9 frontend and
/// forwards to it through `impl_method` on a private guest call frame, so
/// the GPU work, resource storage and reference counting are D3D9's. The
/// wrapper's AddRef/Release are the D3D9 object's, so a D3D8 object lives
/// exactly as long as the object behind it.
///
/// d3d8.dll exists on Windows only as an x86 DLL (SysWOW64); there is no
/// 64-bit Direct3D 8.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/d3d9/d3d9.hpp"

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo {
class GuestMemory;
}
namespace vitreo::cpu {
struct CpuState;
}

namespace vitreo::winapi {

class Marshaler;

/// Every concrete D3D8 COM interface the frontend maps into guest memory.
/// (IDirect3DResource8 and IDirect3DBaseTexture8 are only bases.)
enum class D3D8InterfaceId : u32 {
    Direct3D8 = 0,
    Device,
    SwapChain,
    Surface,
    Volume,
    Texture,
    CubeTexture,
    VolumeTexture,
    VertexBuffer,
    IndexBuffer,
    Count,
};

constexpr usize D3D8_INTERFACE_COUNT = static_cast<usize>(D3D8InterfaceId::Count);

struct D3D8MethodInfo {
    const char* name;
    u16 stdcall_bytes;
};

/// Methods of @p iface in vtable order (generated from d3d8.h).
[[nodiscard]] const D3D8MethodInfo* d3d8_interface_methods(D3D8InterfaceId iface,
                                                           usize& count) noexcept;
[[nodiscard]] usize d3d8_interface_method_count(D3D8InterfaceId iface) noexcept;
[[nodiscard]] const char* d3d8_interface_name(D3D8InterfaceId iface) noexcept;
/// Synthetic export backing one vtable slot, e.g. "_D3D8_Device_Clear".
[[nodiscard]] std::string d3d8_method_export_name(D3D8InterfaceId iface, usize slot);

/// One `[vitreo] UNIMPLEMENTED d3d8.dll!<api>: <what>` line per pair.
void d3d8_log_unimplemented_once(const char* api, const char* what);

class D3D8Manager {
public:
    using InterfaceThunks = std::vector<GuestAddr>;

    D3D8Manager();
    ~D3D8Manager();
    D3D8Manager(const D3D8Manager&) = delete;
    D3D8Manager& operator=(const D3D8Manager&) = delete;

    void set_memory(GuestMemory* memory, bool is_64bit) {
        m_memory = memory;
        m_is_64bit = is_64bit;
    }
    /// The D3D9 frontend every D3D8 object is layered on. Installs the
    /// object-destroyed hook on it.
    void set_d3d9(D3D9Manager* d3d9);
    void set_interface_thunks(D3D8InterfaceId iface, InterfaceThunks thunks);

    /// Direct3DCreate8. Returns the IDirect3D8 guest pointer or 0.
    [[nodiscard]] u32 direct3d_create8(u32 sdk_version);

    /// Dispatch one vtable slot; `m.arg_ptr(0)` is the guest `this`.
    void impl_method(Marshaler& m, D3D8InterfaceId iface, usize slot);

    /// Tests: the D3D9 object behind a D3D8 object (0 when unknown).
    [[nodiscard]] u32 backing_object(u32 object8);

    // ---- Device-private state (defined in d3d8_internal.hpp) --------------
    struct DeviceState;
    struct VertexShader;
    struct PixelShader;
    struct StateBlock;

private:
    struct Object {
        D3D8InterfaceId iface{D3D8InterfaceId::Direct3D8};
        u32 d3d9{0};
        /// A texture level, cube face or volume level shares its container's
        /// reference count (measured: d3d8_contract_probe "surface
        /// refcount"); this is the D3D9 container whose count it is, or 0.
        u32 container9{0};
        D3D9InterfaceId container_iface{D3D9InterfaceId::Texture};
        std::unique_ptr<DeviceState> device;
    };

    class Nested;

    // ---- object model (d3d8.cpp) ------------------------------------------
    [[nodiscard]] bool ensure_storage();
    [[nodiscard]] bool map_vtable(D3D8InterfaceId iface);
    /// The D3D8 object fronting @p object9 (created on first sight).
    /// @p iface9 names the D3D9 interface @p object9 is. Returns 0 when the
    /// D3D9 interface has no D3D8 counterpart.
    [[nodiscard]] u32 wrap(u32 object9);
    [[nodiscard]] u32 wrap_as(u32 object9, D3D8InterfaceId iface);
    [[nodiscard]] Object* find(u32 object8);
    [[nodiscard]] Object* find(u32 object8, D3D8InterfaceId iface);
    /// The D3D9 object behind a D3D8 pointer of one of @p ifaces (0 = none).
    [[nodiscard]] u32 unwrap(u32 object8);
    void on_d3d9_destroyed(u32 object9);
    void finalize_dead();
    void free_object(u32 object8);

    // ---- nested D3D9 calls -------------------------------------------------
    /// Call D3D9 @p iface slot @p slot with x86 stdcall arguments; returns EAX.
    u32 call9(D3D9InterfaceId iface, usize slot, std::initializer_list<u32> args) {
        return call9v(iface, slot, args.begin(), args.size());
    }
    u32 call9v(D3D9InterfaceId iface, usize slot, const u32* args, usize count);
    /// Scratch guest memory valid until the outer D3D8 call returns.
    [[nodiscard]] GuestAddr scratch(usize bytes);
    [[nodiscard]] u32 rd(GuestAddr addr) const;
    void wr(GuestAddr addr, u32 value);
    void copy_guest(GuestAddr dst, GuestAddr src, usize bytes);
    void zero_guest(GuestAddr dst, usize bytes);
    /// Write a D3D9 object returned through @p out9 as its D3D8 wrapper to
    /// @p out8 (0 when the call failed). Transfers the D3D9 reference.
    void return_wrapped(GuestAddr out8, GuestAddr out9, u32 hr);

    // ---- per-interface dispatch -------------------------------------------
    void dispatch_direct3d8(Marshaler& m, usize slot, u32 self, Object& obj);
    void log_device_call(Marshaler& m, usize slot);
    [[nodiscard]] bool skip_draw_for_diagnosis(Marshaler& m, usize slot);
    void dispatch_device(Marshaler& m, usize slot, u32 self, Object& obj);
    void dispatch_swap_chain(Marshaler& m, usize slot, u32 self, Object& obj);
    void dispatch_surface(Marshaler& m, usize slot, u32 self, Object& obj);
    void dispatch_volume(Marshaler& m, usize slot, u32 self, Object& obj);
    void dispatch_texture(Marshaler& m, usize slot, u32 self, Object& obj);
    void dispatch_buffer(Marshaler& m, usize slot, u32 self, Object& obj);
    /// IUnknown + IDirect3DResource8 slots 0..10 shared by every resource,
    /// and IUnknown for the rest. Returns true when it handled @p slot.
    bool dispatch_common(Marshaler& m, usize slot, u32 self, Object& obj);
    u32 get_container(Object& obj, GuestAddr riid, GuestAddr out);

    // ---- device helpers (d3d8_device*.cpp) --------------------------------
    void device_method(Marshaler& m, usize slot, u32 self, Object& obj);
    /// The creation-time state D3D8 sets that D3D9 defaults differently
    /// (CreateDevice and Reset).
    void apply_creation_defaults(Object& dev);
    /// D3DRS_ZBIAS as D3D9 D3DRS_DEPTHBIAS for the bound depth buffer.
    u32 apply_zbias(Object& dev);
    /// True (and logged) when a draw would need N-patch tessellation.
    bool npatch_draw(Object& dev);
    u32 device_set_render_state(Object& dev, u32 state, u32 value);
    u32 device_get_render_state(Object& dev, u32 state, GuestAddr out);
    u32 device_texture_stage_state(Object& dev, bool set, u32 stage, u32 type,
                                   u32 value, GuestAddr out);
    u32 device_copy_rects(Object& dev, u32 src8, GuestAddr rects, u32 count,
                          u32 dst8, GuestAddr points);
    u32 device_state_block(Marshaler& m, usize slot, Object& dev);

    // ---- shaders (d3d8_shaders.cpp) ----------------------------------------
    u32 create_vertex_shader(Object& dev, GuestAddr declaration,
                             GuestAddr function, GuestAddr out_handle, u32 usage);
    u32 set_vertex_shader(Object& dev, u32 handle);
    u32 get_vertex_shader(Object& dev, GuestAddr out_handle);
    u32 delete_vertex_shader(Object& dev, u32 handle);
    u32 get_vertex_shader_blob(Object& dev, u32 handle, bool declaration,
                               GuestAddr data, GuestAddr size);
    u32 create_pixel_shader(Object& dev, GuestAddr function, GuestAddr out_handle);
    u32 set_pixel_shader(Object& dev, u32 handle);
    u32 get_pixel_shader(Object& dev, GuestAddr out_handle);
    u32 delete_pixel_shader(Object& dev, u32 handle);
    u32 get_pixel_shader_function(Object& dev, u32 handle, GuestAddr data,
                                  GuestAddr size);
    void release_device_handles(DeviceState& state);

    // ---- structure translation (d3d8_structs.cpp) -------------------------
    /// D3DPRESENT_PARAMETERS (D3D8) at @p pp8 -> a D3D9 copy in scratch.
    /// Returns 0 and sets @p hr for parameters D3D8 rejects.
    GuestAddr present_parameters_to9(GuestAddr pp8, u32& hr);
    void present_parameters_from9(GuestAddr pp9, GuestAddr pp8);
    void caps_from9(GuestAddr caps9, GuestAddr caps8);
    u32 surface_desc_from9(GuestAddr desc9, GuestAddr desc8);
    u32 volume_desc_from9(GuestAddr desc9, GuestAddr desc8);
    u32 get_adapter_mode_list(u32 d3d9, u32 adapter,
                              std::vector<std::array<u32, 4>>& modes);

    GuestMemory* m_memory{nullptr};
    bool m_is_64bit{false};
    D3D9Manager* m_d3d9{nullptr};

    std::recursive_mutex m_mutex;
    /// Guards m_by9 / m_dead: the D3D9 destroy hook may run on any thread.
    std::mutex m_hook_mutex;

    GuestAddr m_region_base{0};
    bool m_storage_ready{false};
    std::array<InterfaceThunks, D3D8_INTERFACE_COUNT> m_thunks{};
    std::array<GuestAddr, D3D8_INTERFACE_COUNT> m_vtables{};
    GuestAddr m_next_object{0};
    std::vector<GuestAddr> m_free_objects;
    std::unordered_map<u32, Object> m_objects;
    std::unordered_map<u32, u32> m_by9;
    std::vector<u32> m_dead;

    /// One call frame per nesting level of call9 (a D3D9 call can re-enter
    /// the D3D8 frontend through a guest IUnknown it AddRefs).
    std::vector<std::unique_ptr<Nested>> m_nested;
    usize m_call_level{0};
    GuestAddr m_frame_sp{0};
    GuestAddr m_scratch_cursor{0};
    u32 m_depth{0};
};

/// The d3d8.dll export table.
[[nodiscard]] DllExports create_d3d8_thunks(D3D8Manager& manager);

}  // namespace vitreo::winapi
