#pragma once
/// @file d3d12.hpp
/// @brief d3d12.dll — the guest-visible Direct3D 12 COM surface.
///
/// D3D12 is a guest API frontend like D3D9 and D3D11: every GPU operation goes
/// through `graphics::GraphicsBackendDevice`, whose macOS implementation is
/// Metal. Nothing here talks to Metal directly.
///
/// **Device creation is opt-in and off by default.** See `d3d12_enabled()`: a
/// title that supports both D3D11 and D3D12 picks D3D12 when the device
/// creates, so a device is only served once the frontend has been validated on
/// a real title. Until then `D3D12CreateDevice` reports the adapter
/// unsupported, which is what makes a guest fall back.
///
/// The implementation lives in `d3d12::Frontend` (d3d12_internal.hpp); this
/// class is its thin, stable face for the registry and DXGI.

#include "graphics/contracts/backend_device.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#include "winapi/graphics/d3d12/d3d12_vtables.hpp"

#include <vitreo/common.hpp>

#include <array>
#include <functional>
#include <optional>
#include <utility>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

class Marshaler;
class DxgiManager;

namespace d3d12 {

class Frontend;

/// Whether this process serves D3D12 at all.
///
/// Off unless `VITREO_D3D12=1`. A successful `D3D12CreateDevice` moves a title
/// with both back ends off D3D11 for good, so the default flips only after the
/// frontend has been validated on one.
[[nodiscard]] bool d3d12_enabled();

}  // namespace d3d12

/// d3d12.dll.
class D3D12Manager {
public:
    /// One thunk address per vtable slot of every interface in
    /// d3d12::INTERFACE_TABLES (d3d12_vtables.hpp), in table order.
    struct MethodThunks {
        std::array<std::vector<GuestAddr>, d3d12::INTERFACE_TABLE_COUNT>
            tables{};
        // d3d12_root_signature.cpp: ID3DBlob and the two deserializers.
        std::array<GuestAddr, d3d12::BLOB_METHOD_COUNT> blob{};
        std::array<GuestAddr, d3d12::ROOT_SIGNATURE_DESERIALIZER_METHOD_COUNT>
            root_signature_deserializer{};
        std::array<GuestAddr,
                   d3d12::VERSIONED_ROOT_SIGNATURE_DESERIALIZER_METHOD_COUNT>
            versioned_root_signature_deserializer{};
    };

    D3D12Manager(GuestMemory& memory, bool is_64bit,
                 graphics::GraphicsBackendDevice& backend);
    ~D3D12Manager();
    D3D12Manager(const D3D12Manager&) = delete;
    D3D12Manager& operator=(const D3D12Manager&) = delete;

    void set_method_thunks(MethodThunks thunks);
    /// The guest caller used to AddRef/Release interfaces a guest hands to
    /// SetPrivateDataInterface (winapi/core/guest_com.hpp).
    void set_guest_function_caller(GuestFunctionCaller caller);

    /// What a DXGI swap chain on a D3D12 queue needs from the host: the
    /// window's presentation view and client area, and the kernel objects
    /// behind its frame-latency waitable object (the same services the
    /// D3D11 swap chains use), plus the DXGI manager for its factory,
    /// outputs and restrict-to output.
    struct PresentationServices {
        std::function<void*(u64 hwnd)> presentation_view;
        std::function<std::optional<std::pair<u32, u32>>(u64 hwnd)> client_size;
        std::function<bool(u64 hwnd, u32 width, u32 height)> resize_client;
        std::function<std::optional<std::array<i32, 4>>(u64 hwnd)> window_rect;
        std::function<u64(i32 initial, i32 maximum)> create_semaphore;
        std::function<u64(u64 handle)> duplicate_handle;
        std::function<void(u64 handle, i32 count, i32 cap)> release_semaphore;
        std::function<void(u64 handle)> close_handle;
        DxgiManager* dxgi{nullptr};
    };
    void set_presentation_services(PresentationServices services);

    // IDXGIFactory::CreateSwapChain / IDXGIFactory2::CreateSwapChainForHwnd /
    // CreateSwapChainForComposition when pDevice is a D3D12 command queue.
    void impl_CreateSwapChain(Marshaler& m);
    void impl_CreateSwapChainForHwnd(Marshaler& m);
    void impl_CreateSwapChainForComposition(Marshaler& m);

    /// True when @p address is a live ID3D12Device this frontend handed out.
    [[nodiscard]] bool is_device(GuestAddr address) const;
    /// True when @p address is a live ID3D12CommandQueue (DXGI swap chains
    /// are created on a queue, not on the device).
    [[nodiscard]] bool is_command_queue(GuestAddr address) const;

    // -- Exported entry points ------------------------------------------------
    void impl_D3D12CreateDevice(Marshaler& m);
    void impl_D3D12GetDebugInterface(Marshaler& m);
    void impl_D3D12SerializeRootSignature(Marshaler& m);
    void impl_D3D12SerializeVersionedRootSignature(Marshaler& m);
    void impl_D3D12CreateRootSignatureDeserializer(Marshaler& m);
    void impl_D3D12CreateVersionedRootSignatureDeserializer(Marshaler& m);
    void impl_D3D12EnableExperimentalFeatures(Marshaler& m);
    void impl_D3D12GetInterface(Marshaler& m);

    /// Slot @p slot of interface table @p table (d3d12::INTERFACE_TABLES).
    void impl_Method(Marshaler& m, usize table, usize slot);

    // d3d12_root_signature.cpp
    void impl_BlobMethod(Marshaler& m, usize slot);
    void impl_RootSignatureDeserializerMethod(Marshaler& m, usize slot);
    void impl_VersionedRootSignatureDeserializerMethod(Marshaler& m,
                                                       usize slot);

    /// A new ID3DBlob holding a copy of @p size bytes; 0 when out of memory.
    /// The bytes live in guest memory the blob owns (GetBufferPointer).
    [[nodiscard]] GuestAddr create_blob(const u8* bytes, usize size);

    [[nodiscard]] d3d12::Frontend& frontend() noexcept { return *m_frontend; }

private:
    using GuidBytes = std::array<u8, 16>;

    // -- d3d12_root_signature.cpp ---------------------------------------------
    //
    // ID3DBlob and the root-signature deserializers keep their own small
    // object pool and lock; every other D3D12 object is the Frontend's.
    struct Blob {
        u32 ref_count{1};
        GuestAddr data{0};
        usize size{0};
        usize mapped{0};
    };
    /// Both deserializers. The descs they return live in guest memory the
    /// object owns: `descs[v]` is the desc at version v (for the 1.0
    /// deserializer, `descs[1]` is a D3D12_ROOT_SIGNATURE_DESC; otherwise a
    /// D3D12_VERSIONED_ROOT_SIGNATURE_DESC), built on first request and kept
    /// until the final Release, as Learn requires of
    /// GetRootSignatureDescAtVersion.
    struct RootSignatureDeserializer {
        u32 ref_count{1};
        bool versioned{false};
        d3d12::RootSignatureDesc desc;
        std::array<GuestAddr, 4> descs{};
        std::array<usize, 4> mapped{};
    };
    void create_root_signature_deserializer(Marshaler& m, bool versioned);
    void serialize_root_signature_export(Marshaler& m, bool versioned);
    /// Lay out the desc at @p version for @p object; 0 when out of memory.
    [[nodiscard]] GuestAddr root_signature_desc_at(
        RootSignatureDeserializer& object, u32 version);
    [[nodiscard]] u32 release_blob(GuestAddr address);
    [[nodiscard]] u32 release_root_signature_deserializer(GuestAddr address);

    [[nodiscard]] bool ensure_storage();
    /// A COM object whose first word points at the vtable @p vtable names
    /// (a pointer to the member, read after ensure_storage assigns it).
    [[nodiscard]] GuestAddr allocate_object(GuestAddr D3D12Manager::* vtable);
    void free_object(GuestAddr address);
    [[nodiscard]] bool read_guid(GuestAddr address, GuidBytes& out) const;
    [[nodiscard]] bool write_pointer(GuestAddr address, GuestAddr value);

    std::unique_ptr<d3d12::Frontend> m_frontend;
    GuestMemory& m_memory;
    bool m_is_64bit;
    MethodThunks m_method_thunks{};
    GuestAddr m_storage{0};
    GuestAddr m_next_object{0};
    GuestAddr m_blob_vtable{0};
    GuestAddr m_root_signature_deserializer_vtable{0};
    GuestAddr m_versioned_root_signature_deserializer_vtable{0};
    bool m_storage_ready{false};
    std::vector<GuestAddr> m_free_objects;
    /// Guest threads call the root-signature exports concurrently (a
    /// PSO-compiling worker serializes root signatures while the render
    /// thread creates lists). The pool lock covers allocate_object and
    /// free_object; the root-signature lock covers m_blobs and
    /// m_root_signature_deserializers and is always taken first.
    std::mutex m_object_pool_mutex;
    std::recursive_mutex m_root_signature_mutex;
    std::unordered_map<GuestAddr, Blob> m_blobs;
    std::unordered_map<GuestAddr, RootSignatureDeserializer>
        m_root_signature_deserializers;
};

[[nodiscard]] DllExports create_d3d12_thunks(D3D12Manager& manager);
/// Append the ID3DBlob and root-signature-deserializer vtable thunks
/// (d3d12_root_signature.cpp); called by create_d3d12_thunks.
void append_d3d12_root_signature_thunks(DllExports& exports,
                                        D3D12Manager& manager);
/// Export name for one vtable slot. @p interface_name is an export name from
/// d3d12::INTERFACE_TABLES ("Device", "CommandQueue", ...), or "Blob",
/// "RootSignatureDeserializer" or "VersionedRootSignatureDeserializer".
[[nodiscard]] std::string d3d12_method_export_name(const char* interface_name,
                                                   usize slot);
[[nodiscard]] std::string d3d12_device_method_export_name(usize slot);

}  // namespace vitreo::winapi
