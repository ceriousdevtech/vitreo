// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_sharing.cpp
/// @brief ID3D12Device::CreateSharedHandle / OpenSharedHandle /
///        OpenSharedHandleByName for heaps, committed resources and fences,
///        within the process.
///
/// Learn (CreateSharedHandle): the object must have been created shareable
/// (D3D12_HEAP_FLAG_SHARED, D3D12_FENCE_FLAG_SHARED); the handle is an NT
/// handle usable with CloseHandle and DuplicateHandle; a name is in the
/// kernel-object namespace. Measured (tools/oracle_probe/
/// d3d12_a04s_shared_handles_probe.c, WARP and an RTX 4080):
///  - every CreateSharedHandle makes another handle; pHandle NULL and an
///    object that is not shareable (a placed resource too) are E_INVALIDARG;
///  - OpenSharedHandle makes a NEW object each time, answering only its own
///    kind (E_NOINTERFACE otherwise); ppvObj NULL is E_UNEXPECTED; a closed,
///    foreign (an event) or NULL handle is E_HANDLE;
///  - the handle keeps the object alive after every interface is released;
///  - an opened fence IS the fence: signals and lowerings either way are seen
///    by both; an opened heap or buffer is the same memory under a new GPU VA
///    (the opened buffer's VA differs), reporting the original's heap flags
///    and properties (RTX 4080; WARP reports its CUSTOM equivalent);
///  - a SHARED heap or committed resource cannot be CPU-visible;
///  - a fence's NT handle is waitable and signalled (not implemented: a
///    SharedResourceObject handle is not waitable), a heap's is not;
///  - names: a live name (or an event's) is DXGI_ERROR_NAME_ALREADY_EXISTS,
///    lookup is case-insensitive with "Local\" the default namespace, an
///    unknown name is E_HANDLE, and the name dies with the object's handles.
///
/// The handles are kernel32's SharedResourceObject (the D3D11 NT-handle
/// object) with no backend storage id (kD3D12SharedObject) and this
/// process's nonce.
/// A handle duplicated into another process names nothing there; opening it
/// fails loudly (cross-process D3D12 sharing is not implemented).

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/kernel32/shared_resource_handle.hpp"

#include <unistd.h>

namespace vitreo::winapi::d3d12 {

namespace {

constexpr u32 DXGI_ERROR_NAME_ALREADY_EXISTS = 0x887A002Cu;
/// The SharedResourceObject storage id of a D3D12 object: no backend shared
/// storage (D3D11 storage ids count up from 1), and non-zero so the name
/// registry keeps the record.
constexpr u32 kD3D12SharedObject = 0xFFFFFFFFu;

} // namespace

struct Frontend::SharedPayload {
    Frontend* frontend{nullptr};
    Kind kind{Kind::Fence};
    // Fence.
    u32 fence_flags{0};
    std::shared_ptr<FenceTimeline> timeline;
    // Heap.
    u64 heap_size{0};
    u64 heap_alignment{0};
    u32 heap_flags{0};
    HeapProperties properties{};
    // Heap and buffer: the bytes.
    std::shared_ptr<Memory> memory;
    // Committed resource.
    ResourceDescription desc{};
    u32 resource_heap_flags{0};
    std::shared_ptr<graphics::BackendTexture2D> texture;
    std::shared_ptr<graphics::BackendTexture3D> texture3d;
    graphics::PixelFormat pixel_format{graphics::PixelFormat::Unknown};
    /// The name registration: it lives as long as any handle to the object
    /// (every handle holds this payload).
    std::shared_ptr<void> name;

    ~SharedPayload() {
        // The last NT handle may close on any thread: the memory's retirement
        // edits the frontend's VA map, under its lock. A frontend already
        // torn down cleared this payload (Frontend::~Frontend).
        if (frontend == nullptr) return;
        FrontendLock lock(*frontend);
        memory.reset();
        texture.reset();
        texture3d.reset();
        timeline.reset();
    }
};

void Frontend::detach_shared_payloads() {
    for (auto& [nonce, weak] : m_shared) {
        if (auto payload = weak.lock()) {
            payload->memory.reset();
            payload->texture.reset();
            payload->texture3d.reset();
            payload->timeline.reset();
            payload->frontend = nullptr;
        }
    }
    m_shared.clear();
}

void Frontend::create_shared_handle(Marshaler& m) {
    // CreateSharedHandle(ID3D12DeviceChild *pObject, const
    //     SECURITY_ATTRIBUTES *pAttributes, DWORD Access, LPCWSTR Name,
    //     HANDLE *pHandle).
    const GuestAddr object = m.arg_ptr(1);
    const GuestAddr attributes = m.arg_ptr(2);
    const u32 access = m.arg32(3);
    const GuestAddr name = m.arg_ptr(4);
    const GuestAddr out = m.arg_ptr(5);
    if (out != 0) (void)write_pointer(out, 0);
    auto payload = std::make_shared<SharedPayload>();
    if (const Heap* heap = find<Heap>(object, Kind::Heap);
        heap != nullptr && (heap->flags & D3D12_HEAP_FLAG_SHARED) != 0) {
        payload->kind = Kind::Heap;
        payload->heap_size = heap->size;
        payload->heap_alignment = heap->alignment;
        payload->heap_flags = heap->flags;
        payload->properties = heap->properties;
        payload->memory = heap->memory;
    } else if (const Resource* r = find<Resource>(object, Kind::Resource);
               r != nullptr && r->committed &&
               (r->heap_flags & D3D12_HEAP_FLAG_SHARED) != 0) {
        payload->kind = Kind::Resource;
        payload->desc = r->desc;
        payload->properties = r->heap_properties;
        payload->resource_heap_flags = r->heap_flags;
        payload->memory = r->memory;
        payload->texture = r->texture;
        payload->texture3d = r->texture3d;
        payload->pixel_format = r->pixel_format;
    } else if (const Fence* fence = find<Fence>(object, Kind::Fence);
               fence != nullptr && (fence->flags & D3D12_FENCE_FLAG_SHARED) != 0) {
        payload->kind = Kind::Fence;
        payload->fence_flags = fence->flags;
        payload->timeline = fence->timeline;
    } else {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (m_handle_table == nullptr) {
        unimplemented("ID3D12Device::CreateSharedHandle", "no process handle table");
        m.set_return(E_INVALIDARG);
        return;
    }
    bool inherit = false;
    if (attributes != 0) {
        // SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor;
        // BOOL bInheritHandle; }: bInheritHandle at 8 (x86) / 16 (x64).
        inherit = read_u32(attributes + (m_is_64bit ? 16 : 8)) != 0;
    }
    // The nonce names the payload; the process id keeps it apart from
    // another Vitreo process's in the capsule-wide name registry.
    const u64 nonce = (static_cast<u64>(::getpid()) << 32) | m_next_shared_nonce++;
    auto shared = std::make_shared<SharedResourceObject>();
    shared->surface_id = kD3D12SharedObject;
    shared->nonce = nonce;
    shared->always_signaled = payload->kind == Kind::Fence;
    payload->frontend = this;
    shared->keepalive = payload;
    if (name != 0) {
        const auto text = m.read_wide_string(name, 260);
        if (text.is_err() || text.value().empty()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        std::string key;
        switch (register_shared_resource_name(text.value(), kD3D12SharedObject, nonce,
                                              payload->name, key)) {
        case SharedResourceNameResult::Registered:
            shared->name_key = std::move(key);
            break;
        case SharedResourceNameResult::AlreadyExists:
            m.set_return(DXGI_ERROR_NAME_ALREADY_EXISTS);
            return;
        case SharedResourceNameResult::Failed:
            m.set_return(E_OUTOFMEMORY);
            return;
        }
    }
    for (auto it = m_shared.begin(); it != m_shared.end();) {
        it = it->second.expired() ? m_shared.erase(it) : std::next(it);
    }
    m_shared[nonce] = payload;
    const Handle handle =
        allocate_shared_resource_handle(*m_handle_table, std::move(shared), access, inherit);
    if (handle == NULL_HANDLE) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (!write_pointer(out, handle)) {
        (void)m_handle_table->close(handle);
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(S_OK);
}

void Frontend::open_shared_handle(Marshaler& m, Device& device) {
    // OpenSharedHandle(HANDLE NTHandle, REFIID riid, void **ppvObj).
    const u64 value = m.arg(1);
    const GuestAddr iid = m.arg_ptr(2);
    const GuestAddr out = m.arg_ptr(3);
    if (out == 0) {
        m.set_return(E_UNEXPECTED);
        return;
    }
    (void)write_pointer(out, 0);
    std::shared_ptr<SharedResourceObject> shared;
    if (m_handle_table != nullptr && value != 0 && value <= 0xFFFFFFFFull) {
        shared = shared_resource_object(*m_handle_table, static_cast<Handle>(value));
    }
    if (shared == nullptr) {
        m.set_return(E_HANDLE);
        return;
    }
    std::shared_ptr<SharedPayload> payload;
    if (shared->surface_id == kD3D12SharedObject) {
        if (auto it = m_shared.find(shared->nonce); it != m_shared.end()) {
            payload = it->second.lock();
        }
    }
    if (payload == nullptr || payload.get() != shared->keepalive.get()) {
        // A D3D11 / DXGI shared resource, or a D3D12 object of another
        // process (DuplicateHandle): no D3D12 object here holds it.
        unimplemented("ID3D12Device::OpenSharedHandle",
                      shared->surface_id != kD3D12SharedObject
                          ? "a D3D11 / DXGI shared resource handle"
                          : "a D3D12 object shared by another process");
        m.set_return(E_INVALIDARG);
        return;
    }
    std::unique_ptr<Object> object;
    switch (payload->kind) {
    case Kind::Fence: {
        auto fence = std::make_unique<Fence>();
        fence->flags = payload->fence_flags;
        fence->timeline = payload->timeline;
        object = std::move(fence);
        break;
    }
    case Kind::Heap: {
        auto heap = std::make_unique<Heap>();
        heap->size = payload->heap_size;
        heap->alignment = payload->heap_alignment;
        heap->flags = payload->heap_flags;
        // The creator's properties (measured on the RTX 4080; WARP reports
        // its own CUSTOM equivalent instead).
        heap->properties = payload->properties;
        heap->memory = alias_memory(payload->memory);
        if (!heap->memory) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        object = std::move(heap);
        break;
    }
    case Kind::Resource: {
        auto resource = std::make_unique<Resource>();
        resource->desc = payload->desc;
        resource->committed = true;
        resource->heap_properties = payload->properties;
        resource->heap_flags = payload->resource_heap_flags;
        resource->texture = payload->texture;
        resource->texture3d = payload->texture3d;
        resource->pixel_format = payload->pixel_format;
        if (payload->memory) {
            resource->memory = alias_memory(payload->memory);
            if (!resource->memory) {
                m.set_return(E_OUTOFMEMORY);
                return;
            }
        }
        object = std::move(resource);
        break;
    }
    default:
        m.set_return(E_HANDLE);
        return;
    }
    object->device = device.address;
    m.set_return(return_new_object(std::move(object), iid, out));
}

void Frontend::open_shared_handle_by_name(Marshaler& m) {
    // OpenSharedHandleByName(LPCWSTR Name, DWORD Access, HANDLE *pNTHandle).
    // (pNTHandle NULL faults on Windows; here it is E_INVALIDARG.)
    const GuestAddr name = m.arg_ptr(1);
    const u32 access = m.arg32(2);
    const GuestAddr out = m.arg_ptr(3);
    if (out == 0 || name == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    (void)write_pointer(out, 0);
    const auto text = m.read_wide_string(name, 260);
    if (text.is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto target = lookup_shared_resource_name(text.value());
    if (!target) {
        m.set_return(E_HANDLE);
        return;
    }
    std::shared_ptr<SharedPayload> payload;
    if (target->surface_id == kD3D12SharedObject) {
        if (auto it = m_shared.find(target->nonce); it != m_shared.end()) {
            payload = it->second.lock();
        }
    }
    if (payload == nullptr || m_handle_table == nullptr) {
        unimplemented("ID3D12Device::OpenSharedHandleByName",
                      "a name another process or API holds");
        m.set_return(E_HANDLE);
        return;
    }
    // A new handle to the same object. The name itself stays with the
    // creating handle's object: it lives as long as any handle to it.
    auto shared = std::make_shared<SharedResourceObject>();
    shared->surface_id = kD3D12SharedObject;
    shared->nonce = target->nonce;
    shared->always_signaled = payload->kind == Kind::Fence;
    shared->keepalive = payload;
    const Handle handle =
        allocate_shared_resource_handle(*m_handle_table, std::move(shared), access, false);
    if (handle == NULL_HANDLE || !write_pointer(out, handle)) {
        if (handle != NULL_HANDLE) (void)m_handle_table->close(handle);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(S_OK);
}

} // namespace vitreo::winapi::d3d12

namespace vitreo::winapi {

void D3D12Manager::set_virtual_memory(VirtualMemoryManager* vmm) {
    m_frontend->m_vmm = vmm;
}

void D3D12Manager::set_section_services(SectionServices services) {
    d3d12::FrontendLock lock(*m_frontend);
    m_frontend->m_sections = std::move(services);
}

void D3D12Manager::set_shader_cache_directories(ShaderCacheDirectories dirs) {
    d3d12::FrontendLock lock(*m_frontend);
    m_frontend->m_shader_cache_dirs = std::move(dirs);
}

void D3D12Manager::set_handle_table(HandleTable* table) {
    d3d12::FrontendLock lock(*m_frontend);
    m_frontend->m_handle_table = table;
}

} // namespace vitreo::winapi
