// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_objects.cpp
/// @brief The D3D12 object model: guest interface storage, vtables,
/// QueryInterface / AddRef / Release, ID3D12Object private data and names,
/// ID3D12DeviceChild::GetDevice, and dispatch of every vtable slot.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace vitreo::winapi::d3d12 {
namespace {

/// Guest bytes per COM object: the vtable pointer, padded. Nothing reads past
/// it -- the interface is opaque to the guest -- and a D3D12 frame loop
/// creates and destroys objects continuously, so it is kept small and freed
/// addresses are recycled.
constexpr usize OBJECT_BYTES = 16;
/// Object storage grows in chunks; a title holds tens of thousands of
/// resources and descriptor heaps.
constexpr usize OBJECT_CHUNK_BYTES = 0x100000;
constexpr GuestAddr OBJECT_REGION_HINT = 0x64000000ull;

/// The interfaces each kind answers QueryInterface for, from the SDK
/// inheritance chains (d3d12.h: ID3D12DeviceChild : ID3D12Object,
/// ID3D12Pageable : ID3D12DeviceChild, ID3D12Heap / Resource / DescriptorHeap /
/// QueryHeap / CommandSignature / CommandQueue / PipelineState / Fence /
/// CommandAllocator : ID3D12Pageable, ID3D12RootSignature / CommandList :
/// ID3D12DeviceChild). Each newer interface version is listed only once every
/// slot it adds is implemented or fails the documented way.
constexpr GuidBytes DEVICE_IIDS[] = {
    IID_IUNKNOWN,       IID_ID3D12OBJECT,   IID_ID3D12DEVICE,
    IID_ID3D12DEVICE1,  IID_ID3D12DEVICE2,  IID_ID3D12DEVICE3,
    IID_ID3D12DEVICE4,  IID_ID3D12DEVICE5,  IID_ID3D12DEVICE6,
    IID_ID3D12DEVICE7,  IID_ID3D12DEVICE8,  IID_ID3D12DEVICE9,
    IID_ID3D12DEVICE10, IID_ID3D12DEVICE11, IID_ID3D12DEVICE12,
    IID_ID3D12DEVICE13, IID_ID3D12DEVICE14};
constexpr GuidBytes QUEUE_IIDS[] = {IID_IUNKNOWN, IID_ID3D12OBJECT,
                                    IID_ID3D12DEVICECHILD, IID_ID3D12PAGEABLE,
                                    IID_ID3D12COMMANDQUEUE};
constexpr GuidBytes ALLOCATOR_IIDS[] = {
    IID_IUNKNOWN, IID_ID3D12OBJECT, IID_ID3D12DEVICECHILD, IID_ID3D12PAGEABLE,
    IID_ID3D12COMMANDALLOCATOR};
constexpr GuidBytes FENCE_IIDS[] = {IID_IUNKNOWN, IID_ID3D12OBJECT,
                                    IID_ID3D12DEVICECHILD, IID_ID3D12PAGEABLE,
                                    IID_ID3D12FENCE, IID_ID3D12FENCE1};
constexpr GuidBytes LIST_IIDS[] = {
    IID_IUNKNOWN,
    IID_ID3D12OBJECT,
    IID_ID3D12DEVICECHILD,
    IID_ID3D12COMMANDLIST,
    IID_ID3D12GRAPHICSCOMMANDLIST,
    IID_ID3D12GRAPHICSCOMMANDLIST1,
    IID_ID3D12GRAPHICSCOMMANDLIST2,
    IID_ID3D12GRAPHICSCOMMANDLIST3,
    IID_ID3D12GRAPHICSCOMMANDLIST4,
    IID_ID3D12GRAPHICSCOMMANDLIST5,
    IID_ID3D12GRAPHICSCOMMANDLIST6,
    IID_ID3D12GRAPHICSCOMMANDLIST7,
    IID_ID3D12GRAPHICSCOMMANDLIST8,
    IID_ID3D12GRAPHICSCOMMANDLIST9,
    IID_ID3D12GRAPHICSCOMMANDLIST10};
constexpr GuidBytes HEAP_IIDS[] = {IID_IUNKNOWN, IID_ID3D12OBJECT,
                                   IID_ID3D12DEVICECHILD, IID_ID3D12PAGEABLE,
                                   IID_ID3D12HEAP, IID_ID3D12HEAP1};
constexpr GuidBytes RESOURCE_IIDS[] = {
    IID_IUNKNOWN,    IID_ID3D12OBJECT,    IID_ID3D12DEVICECHILD,
    IID_ID3D12PAGEABLE, IID_ID3D12RESOURCE, IID_ID3D12RESOURCE1,
    IID_ID3D12RESOURCE2};
constexpr GuidBytes DESCRIPTOR_HEAP_IIDS[] = {
    IID_IUNKNOWN, IID_ID3D12OBJECT, IID_ID3D12DEVICECHILD, IID_ID3D12PAGEABLE,
    IID_ID3D12DESCRIPTORHEAP};
constexpr GuidBytes QUERY_HEAP_IIDS[] = {
    IID_IUNKNOWN, IID_ID3D12OBJECT, IID_ID3D12DEVICECHILD, IID_ID3D12PAGEABLE,
    IID_ID3D12QUERYHEAP};
constexpr GuidBytes SIGNATURE_IIDS[] = {
    IID_IUNKNOWN, IID_ID3D12OBJECT, IID_ID3D12DEVICECHILD, IID_ID3D12PAGEABLE,
    IID_ID3D12COMMANDSIGNATURE};
constexpr GuidBytes ROOT_IIDS[] = {IID_IUNKNOWN, IID_ID3D12OBJECT,
                                   IID_ID3D12DEVICECHILD,
                                   IID_ID3D12ROOTSIGNATURE};
constexpr GuidBytes PSO_IIDS[] = {IID_IUNKNOWN, IID_ID3D12OBJECT,
                                  IID_ID3D12DEVICECHILD, IID_ID3D12PAGEABLE,
                                  IID_ID3D12PIPELINESTATE};

template <usize N>
[[nodiscard]] bool listed(const GuidBytes (&list)[N], const GuidBytes& iid) {
    return std::any_of(std::begin(list), std::end(list),
                       [&](const GuidBytes& g) { return guid_equal(g, iid); });
}

}  // namespace

// =============================================================================
// Guest storage
// =============================================================================

bool Frontend::write_pointer(GuestAddr address, GuestAddr value) {
    return m_is_64bit
               ? m_memory.write_u64(address, value).is_ok()
               : m_memory.write_u32(address, static_cast<u32>(value)).is_ok();
}

bool Frontend::read_pointer(GuestAddr address, GuestAddr& value) const {
    if (m_is_64bit) {
        const auto read = m_memory.read_u64(address);
        if (!read.is_ok()) return false;
        value = read.value();
        return true;
    }
    const auto read = m_memory.read_u32(address);
    if (!read.is_ok()) return false;
    value = read.value();
    return true;
}

bool Frontend::read_guid(GuestAddr address, GuidBytes& out) const {
    if (address == 0) return false;
    return m_memory.read_bytes(address, out.data(), out.size()).is_ok();
}

u32 Frontend::read_u32(GuestAddr address) const {
    const auto value = m_memory.read_u32(address);
    return value.is_ok() ? value.value() : 0u;
}

u64 Frontend::read_u64(GuestAddr address) const {
    const auto value = m_memory.read_u64(address);
    return value.is_ok() ? value.value() : 0u;
}

float Frontend::read_f32(GuestAddr address) const {
    const u32 bits = read_u32(address);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool Frontend::ensure_vtables() {
    if (m_vtables_ready) return true;
    const usize pointer_size = m_is_64bit ? 8u : 4u;
    usize bytes = 0;
    for (usize table = 0; table < INTERFACE_TABLE_COUNT; ++table) {
        bytes += INTERFACE_TABLES[table].method_count * pointer_size;
    }
    bytes = (bytes + 0xFFFFu) & ~usize{0xFFFFu};
    GuestAddr base = m_vtables[0] != 0 ? m_vtables[0] : 0;
    if (base == 0) {
        base = m_memory.map_first_fit(OBJECT_REGION_HINT, bytes,
                                      MemoryProtection::ReadWrite);
        if (base == 0) return false;
        GuestAddr cursor = base;
        for (usize table = 0; table < INTERFACE_TABLE_COUNT; ++table) {
            m_vtables[table] = cursor;
            cursor += INTERFACE_TABLES[table].method_count * pointer_size;
        }
    }
    // A zero thunk would leave a null vtable entry, which is a guest call to
    // address zero. Refuse the whole table rather than serve a hole.
    for (usize table = 0; table < INTERFACE_TABLE_COUNT; ++table) {
        const usize count = INTERFACE_TABLES[table].method_count;
        if (m_thunks[table].size() != count) return false;
        for (usize slot = 0; slot < count; ++slot) {
            if (m_thunks[table][slot] == 0 ||
                !write_pointer(m_vtables[table] + slot * pointer_size,
                               m_thunks[table][slot])) {
                return false;
            }
        }
    }
    m_vtables_ready = true;
    return true;
}

GuestAddr Frontend::allocate_interface(usize table) {
    if (!ensure_vtables() || table >= INTERFACE_TABLE_COUNT) return 0;
    GuestAddr address = 0;
    if (!m_free_objects.empty()) {
        address = m_free_objects.back();
        m_free_objects.pop_back();
    } else {
        if (m_next_object == 0 ||
            m_next_object + OBJECT_BYTES > m_object_limit) {
            const GuestAddr chunk = m_memory.map_first_fit(
                OBJECT_REGION_HINT, OBJECT_CHUNK_BYTES,
                MemoryProtection::ReadWrite);
            if (chunk == 0) return 0;
            m_object_chunks.push_back(chunk);
            m_next_object = chunk;
            m_object_limit = chunk + OBJECT_CHUNK_BYTES;
        }
        address = m_next_object;
        m_next_object += OBJECT_BYTES;
    }
    if (!write_pointer(address, m_vtables[table])) return 0;
    return address;
}

void Frontend::free_interface(GuestAddr address) {
    if (address == 0) return;
    // Null the vtable pointer, so a guest that uses the interface after its
    // last Release calls address zero and faults at the mistake rather than
    // dispatching into whichever object was handed out next.
    (void)write_pointer(address, 0);
    m_free_objects.push_back(address);
}

GuestAddr Frontend::publish(std::unique_ptr<Object> object) {
    const GuestAddr address = allocate_interface(table_for(object->kind));
    if (address == 0) return 0;
    object->address = address;
    object->serial = m_next_serial++;
    m_objects.emplace(address, std::move(object));
    return address;
}

void Frontend::add_device_ref(GuestAddr device) {
    if (Device* owner = find<Device>(device, Kind::Device)) add_ref(*owner);
}

u32 Frontend::release(GuestAddr address) {
    const auto entry = m_objects.find(address);
    if (entry == m_objects.end()) return 0;
    Object& object = *entry->second;
    if (object.ref_count == 0) return 0;
    if (--object.ref_count > 0) return object.ref_count;
    std::unique_ptr<Object> owned = std::move(entry->second);
    m_objects.erase(entry);
    destroy(std::move(owned));
    return 0;
}

void Frontend::destroy(std::unique_ptr<Object> object) {
    const GuestAddr address = object->address;
    const GuestAddr device = object->device;
    switch (object->kind) {
    case Kind::Device:
        if (m_device == address) m_device = 0;
        break;
    case Kind::Fence: {
        auto& fence = static_cast<Fence&>(*object);
        GpuEventSignaler::instance().cancel(fence.backend.get());
        break;
    }
    case Kind::CommandList: {
        auto& list = static_cast<CommandList&>(*object);
        // A list destroyed while recording releases its claim on the
        // allocator, or the allocator could never be Reset again.
        if (!list.closed) {
            if (auto* allocator = find<CommandAllocator>(
                    list.allocator, Kind::CommandAllocator);
                allocator != nullptr && allocator->recording_lists > 0) {
                --allocator->recording_lists;
            }
        }
        break;
    }
    case Kind::DescriptorHeap: {
        auto& heap = static_cast<DescriptorHeap&>(*object);
        m_cpu_heaps.erase(heap.cpu_base);
        if (heap.gpu_base != 0) m_gpu_heaps.erase(heap.gpu_base);
        if (heap.cpu_base != 0) {
            (void)m_memory.unmap_region(heap.cpu_base, heap.cpu_reserved);
        }
        break;
    }
    case Kind::SwapChain: {
        // The chain's buffers, queue, factory and restrict-to output go with
        // it; the frame-latency semaphore's pending releases see it dead.
        auto& chain = static_cast<SwapChain&>(*object);
        release_swap_chain_buffers(chain);
        chain.backend.reset();
        if (chain.latency) {
            std::lock_guard<std::mutex> hold(chain.latency->mutex);
            chain.latency->alive = false;
            if (chain.latency->handle != 0 && m_presentation.close_handle) {
                m_presentation.close_handle(chain.latency->handle);
            }
            chain.latency->handle = 0;
        }
        if (m_presentation.dxgi != nullptr) {
            if (chain.factory != 0) {
                m_presentation.dxgi->release_dxgi_object(chain.factory);
            }
            if (chain.restrict_output != 0) {
                m_presentation.dxgi->release_dxgi_object(chain.restrict_output);
            }
        }
        if (chain.queue != 0 &&
            find<CommandQueue>(chain.queue, Kind::CommandQueue) != nullptr) {
            (void)release(chain.queue);
        }
        break;
    }
    default:
        break;
    }
    // Private interfaces are released last, after every container mutation:
    // a guest Release may re-enter this frontend (winapi/core/guest_com.hpp).
    std::vector<GuestAddr> held;
    for (const auto& [guid, pointer] : object->private_data.interfaces) {
        if (pointer != 0) held.push_back(pointer);
    }
    const std::vector<Object::DestructionCallback> callbacks =
        std::move(object->destruction_callbacks);
    if (object->notifier != 0) {
        m_tearoffs.erase(object->notifier);
        free_interface(object->notifier);
    }
    object.reset();
    free_interface(address);
    // A placed resource holds no COM reference on its heap (measured,
    // d12core_facts_probe heap_addref_with_placed): only the heap's memory,
    // which the shared Memory keeps alive.
    if (device != 0) (void)release(device);
    if (m_guest_call) {
        for (const GuestAddr pointer : held) {
            (void)guest_com_release(m_memory, m_is_64bit, m_guest_call,
                                    pointer);
        }
        // ID3DDestructionNotifier: PFN_DESTRUCTION_CALLBACK(void *pData),
        // once the object is gone.
        for (const auto& callback : callbacks) {
            const u64 argument = callback.data;
            (void)m_guest_call(callback.function, &argument, 1);
        }
    } else if (!callbacks.empty()) {
        unimplemented("ID3DDestructionNotifier",
                      "callbacks without a guest caller");
    }
}

// =============================================================================
// QueryInterface and the ID3D12Object / ID3D12DeviceChild slots
// =============================================================================

bool Frontend::answers(const Object& object, const GuidBytes& iid) {
    switch (object.kind) {
    case Kind::Device: return listed(DEVICE_IIDS, iid);
    case Kind::CommandQueue: return listed(QUEUE_IIDS, iid);
    case Kind::CommandAllocator: return listed(ALLOCATOR_IIDS, iid);
    case Kind::Fence: return listed(FENCE_IIDS, iid);
    case Kind::CommandList: return listed(LIST_IIDS, iid);
    case Kind::Heap: return listed(HEAP_IIDS, iid);
    case Kind::Resource: return listed(RESOURCE_IIDS, iid);
    case Kind::DescriptorHeap: return listed(DESCRIPTOR_HEAP_IIDS, iid);
    case Kind::QueryHeap: return listed(QUERY_HEAP_IIDS, iid);
    case Kind::CommandSignature: return listed(SIGNATURE_IIDS, iid);
    case Kind::RootSignature: return listed(ROOT_IIDS, iid);
    case Kind::PipelineState: return listed(PSO_IIDS, iid);
    case Kind::SwapChain:
        return guid_equal(iid, IID_IUNKNOWN) || guid_equal(iid, IID_IDXGIOBJECT) ||
               guid_equal(iid, IID_IDXGIDEVICESUBOBJECT) ||
               guid_equal(iid, IID_IDXGISWAPCHAIN) ||
               guid_equal(iid, IID_IDXGISWAPCHAIN1) ||
               guid_equal(iid, IID_IDXGISWAPCHAIN2) ||
               guid_equal(iid, IID_IDXGISWAPCHAIN3) ||
               guid_equal(iid, IID_IDXGISWAPCHAIN4);
    }
    return false;
}

u32 Frontend::return_new_object(std::unique_ptr<Object> object,
                                GuestAddr iid_address, GuestAddr out_address) {
    GuidBytes iid{};
    if (!read_guid(iid_address, iid)) return E_INVALIDARG;
    if (!answers(*object, iid)) return E_NOINTERFACE;
    const GuestAddr device = object->device;
    const GuestAddr address = publish(std::move(object));
    if (address == 0) return E_OUTOFMEMORY;
    // Every device child holds a reference on its device, released when the
    // child is destroyed (Frontend::destroy).
    if (device != 0) add_device_ref(device);
    if (!write_pointer(out_address, address)) {
        (void)release(address);
        return E_POINTER;
    }
    return S_OK;
}

bool Frontend::common_method(Marshaler& m, usize slot, Object& object) {
    switch (slot) {
    case 0: {  // QueryInterface(REFIID riid, void **ppvObject)
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(E_POINTER);
            return true;
        }
        GuidBytes iid{};
        const GuestAddr answer =
            read_guid(m.arg_ptr(1), iid) ? query_interface(object, iid) : 0;
        (void)write_pointer(out, answer);
        m.set_return(answer != 0 ? S_OK : E_NOINTERFACE);
        return true;
    }
    case 1:
        add_ref(object);
        m.set_return(object.ref_count);
        return true;
    case 2:
        m.set_return(release(object.address));
        return true;
    case 3:
        m.set_return(get_private_data(m, object));
        return true;
    case 4:
        m.set_return(set_private_data(m, object));
        return true;
    case 5:
        m.set_return(set_private_data_interface(m, object));
        return true;
    case 6:
        m.set_return(set_name(m, object));
        return true;
    case 7: {
        if (object.kind == Kind::Device) return false;
        // ID3D12DeviceChild::GetDevice(REFIID riid, void **ppvDevice).
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(E_POINTER);
            return true;
        }
        (void)write_pointer(out, 0);
        Device* device = find<Device>(object.device, Kind::Device);
        GuidBytes iid{};
        if (device == nullptr) {
            m.set_return(E_FAIL);
            return true;
        }
        const GuestAddr answer =
            read_guid(m.arg_ptr(1), iid) ? query_interface(*device, iid) : 0;
        (void)write_pointer(out, answer);
        m.set_return(answer != 0 ? S_OK : E_NOINTERFACE);
        return true;
    }
    default:
        return false;
    }
}

GuestAddr Frontend::query_interface(Object& object, const GuidBytes& iid) {
    if (guid_equal(iid, IID_ID3DDESTRUCTIONNOTIFIER)) {
        // Every D3D12 object answers it (measured, a01 qi_device / qi_child),
        // through a tear-off: a second pointer whose IUnknown is the
        // object's.
        if (object.notifier == 0) {
            object.notifier =
                allocate_interface(table_index("DestructionNotifier"));
            if (object.notifier == 0) return 0;
            m_tearoffs.emplace(object.notifier, object.address);
        }
        add_ref(object);
        return object.notifier;
    }
    if (!answers(object, iid)) return 0;
    add_ref(object);
    return object.address;
}

void Frontend::notifier_method(Marshaler& m, Object& owner, usize slot) {
    switch (slot) {
    case DestructionNotifierSlot::QueryInterface: {
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(E_POINTER);
            return;
        }
        GuidBytes iid{};
        const GuestAddr answer =
            read_guid(m.arg_ptr(1), iid) ? query_interface(owner, iid) : 0;
        (void)write_pointer(out, answer);
        m.set_return(answer != 0 ? S_OK : E_NOINTERFACE);
        return;
    }
    case DestructionNotifierSlot::AddRef:
        add_ref(owner);
        m.set_return(owner.ref_count);
        return;
    case DestructionNotifierSlot::Release:
        m.set_return(release(owner.address));
        return;
    case DestructionNotifierSlot::RegisterDestructionCallback: {
        // RegisterDestructionCallback(PFN_DESTRUCTION_CALLBACK callbackFn,
        //     void *pData, UINT *pCallbackID).
        const GuestAddr function = m.arg_ptr(1);
        const GuestAddr id_out = m.arg_ptr(3);
        if (function == 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        Object::DestructionCallback callback{};
        callback.id = owner.next_callback_id++;
        callback.function = function;
        callback.data = m.arg_ptr(2);
        owner.destruction_callbacks.push_back(callback);
        if (id_out != 0) (void)m_memory.write_u32(id_out, callback.id);
        m.set_return(S_OK);
        return;
    }
    case DestructionNotifierSlot::UnregisterDestructionCallback: {
        const u32 id = m.arg32(1);
        auto& list = owner.destruction_callbacks;
        const auto it = std::find_if(list.begin(), list.end(),
                                     [id](const auto& c) { return c.id == id; });
        if (it == list.end()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        list.erase(it);
        m.set_return(S_OK);
        return;
    }
    default:
        m.set_return(E_NOTIMPL);
        return;
    }
}

u32 Frontend::get_private_data(Marshaler& m, Object& object) {
    // GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData).
    GuidBytes name{};
    const GuestAddr size_address = m.arg_ptr(2);
    const GuestAddr data_address = m.arg_ptr(3);
    if (!read_guid(m.arg_ptr(1), name) || size_address == 0) {
        return E_INVALIDARG;
    }
    const auto held = object.private_data.interfaces.find(name);
    if (held != object.private_data.interfaces.end()) {
        // An interface answers with a pointer and a reference the caller owns.
        const u32 pointer_size = m_is_64bit ? 8u : 4u;
        const u32 capacity = read_u32(size_address);
        (void)m_memory.write_u32(size_address, pointer_size);
        if (data_address == 0) return S_OK;
        if (capacity < pointer_size) return DXGI_ERROR_MORE_DATA;
        const GuestAddr pointer = held->second;
        if (!write_pointer(data_address, pointer)) return E_INVALIDARG;
        if (pointer != 0 && m_guest_call) {
            (void)guest_com_add_ref(m_memory, m_is_64bit, m_guest_call,
                                    pointer);
        }
        return S_OK;
    }
    if (guid_equal(name, WKPDID_D3D12UNIQUEOBJECTID) &&
        object.private_data.bytes.find(name) ==
            object.private_data.bytes.end()) {
        // The object's serial: 64 bits, unique for the process's lifetime
        // and never reused, which is what the id promises.
        const u32 capacity = read_u32(size_address);
        (void)m_memory.write_u32(size_address, 8);
        if (data_address == 0) return S_OK;
        if (capacity < 8) return DXGI_ERROR_MORE_DATA;
        return m_memory.write_u64(data_address, object.serial).is_ok()
                   ? S_OK
                   : E_INVALIDARG;
    }
    const auto value = object.private_data.bytes.find(name);
    if (value == object.private_data.bytes.end()) {
        (void)m_memory.write_u32(size_address, 0);
        return DXGI_ERROR_NOT_FOUND;
    }
    const u32 capacity = read_u32(size_address);
    const u32 required = static_cast<u32>(value->second.size());
    (void)m_memory.write_u32(size_address, required);
    if (data_address == 0) return S_OK;
    if (capacity < required) return DXGI_ERROR_MORE_DATA;
    if (required != 0 &&
        m_memory.write_bytes(data_address, value->second.data(), required)
            .is_err()) {
        return E_INVALIDARG;
    }
    return S_OK;
}

u32 Frontend::set_private_data(Marshaler& m, Object& object) {
    // SetPrivateData(REFGUID guid, UINT DataSize, const void *pData).
    GuidBytes name{};
    const u32 size = m.arg32(2);
    const GuestAddr data = m.arg_ptr(3);
    if (!read_guid(m.arg_ptr(1), name)) return E_INVALIDARG;
    GuestAddr dropped = 0;
    if (const auto held = object.private_data.interfaces.find(name);
        held != object.private_data.interfaces.end()) {
        dropped = held->second;
        object.private_data.interfaces.erase(held);
    }
    u32 result = S_OK;
    if (data == 0) {
        // Learn: "If pData is NULL, DataSize must also be 0, and any data
        // previously associated with the specified guid will be destroyed."
        const bool had = object.private_data.bytes.erase(name) != 0;
        result = had || dropped != 0 ? S_OK : S_FALSE;
    } else {
        std::vector<u8> bytes(size);
        if (size != 0 &&
            m_memory.read_bytes(data, bytes.data(), bytes.size()).is_err()) {
            return E_INVALIDARG;
        }
        object.private_data.bytes.insert_or_assign(name, std::move(bytes));
    }
    if (dropped != 0 && m_guest_call) {
        (void)guest_com_release(m_memory, m_is_64bit, m_guest_call, dropped);
    }
    return result;
}

u32 Frontend::set_private_data_interface(Marshaler& m, Object& object) {
    // SetPrivateDataInterface(REFGUID guid, const IUnknown *pData).
    GuidBytes name{};
    if (!read_guid(m.arg_ptr(1), name)) return E_INVALIDARG;
    const GuestAddr pointer = m.arg_ptr(2);
    if (!m_guest_call) {
        // Keeping the pointer without the AddRef the contract requires would
        // leave this object holding a reference it never took.
        unimplemented("ID3D12Object::SetPrivateDataInterface",
                      "no guest caller to AddRef the interface");
        return E_NOTIMPL;
    }
    // AddRef first: the new and old pointers may be the same.
    if (pointer != 0) {
        (void)guest_com_add_ref(m_memory, m_is_64bit, m_guest_call, pointer);
    }
    GuestAddr previous = 0;
    if (const auto held = object.private_data.interfaces.find(name);
        held != object.private_data.interfaces.end()) {
        previous = held->second;
    }
    object.private_data.interfaces.insert_or_assign(name, pointer);
    object.private_data.bytes.erase(name);
    if (previous != 0) {
        (void)guest_com_release(m_memory, m_is_64bit, m_guest_call, previous);
    }
    return S_OK;
}

u32 Frontend::set_name(Marshaler& m, Object& object) {
    // SetName(LPCWSTR Name). Learn: equivalent to SetPrivateData with
    // WKPDID_D3DDebugObjectNameW and the string, including its terminator.
    const GuestAddr name = m.arg_ptr(1);
    std::vector<u8> bytes;
    if (name != 0) {
        for (u32 index = 0; index < 4096; ++index) {
            const auto unit = m_memory.read_u16(name + index * 2);
            if (!unit.is_ok()) return E_INVALIDARG;
            bytes.push_back(static_cast<u8>(unit.value() & 0xFF));
            bytes.push_back(static_cast<u8>(unit.value() >> 8));
            if (unit.value() == 0) break;
        }
        object.private_data.bytes.insert_or_assign(WKPDID_D3DDEBUGOBJECTNAMEW,
                                                   std::move(bytes));
    } else {
        object.private_data.bytes.erase(WKPDID_D3DDEBUGOBJECTNAMEW);
    }
    return S_OK;
}

// =============================================================================
// Dispatch
// =============================================================================

void Frontend::method(Marshaler& m, usize table, usize slot) {
    std::lock_guard lock(m_mutex);
    const GuestAddr self = m.arg_ptr(0);
    if (const auto tearoff = m_tearoffs.find(self);
        tearoff != m_tearoffs.end() &&
        table == table_index("DestructionNotifier")) {
        if (const auto owner = m_objects.find(tearoff->second);
            owner != m_objects.end()) {
            notifier_method(m, *owner->second, slot);
            return;
        }
    }
    const auto entry = m_objects.find(self);
    if (entry == m_objects.end() ||
        table_for(entry->second->kind) != table ||
        table >= INTERFACE_TABLE_COUNT) {
        // A call through a dead or foreign interface. COM leaves it
        // undefined; answer the counting slots with 0 and the rest with the
        // generic failure, never touching an output.
        d3d12_diag("%s slot %zu on 0x%llx: not a live object of that "
                   "interface",
                   table < INTERFACE_TABLE_COUNT
                       ? INTERFACE_TABLES[table].export_name
                       : "?",
                   slot, static_cast<unsigned long long>(self));
        m.set_return(slot == 1 || slot == 2 ? 0u : E_INVALIDARG);
        return;
    }
    Object& object = *entry->second;
    // IDXGISwapChain is an IDXGIObject, not an ID3D12Object: its slots 3-7
    // are DXGI's (SetPrivateData .. GetDevice).
    if (object.kind == Kind::SwapChain) {
        swap_chain_method(m, static_cast<SwapChain&>(object), slot);
        return;
    }
    if (common_method(m, slot, object)) return;
    switch (object.kind) {
    case Kind::SwapChain:
        return;
    case Kind::Device:
        device_method(m, static_cast<Device&>(object), slot);
        return;
    case Kind::CommandQueue:
        command_queue_method(m, static_cast<CommandQueue&>(object), slot);
        return;
    case Kind::CommandAllocator:
        command_allocator_method(m, static_cast<CommandAllocator&>(object),
                                 slot);
        return;
    case Kind::Fence:
        fence_method(m, static_cast<Fence&>(object), slot);
        return;
    case Kind::CommandList:
        command_list_method(m, static_cast<CommandList&>(object), slot);
        return;
    case Kind::Heap:
        heap_method(m, static_cast<Heap&>(object), slot);
        return;
    case Kind::Resource:
        resource_method(m, static_cast<Resource&>(object), slot);
        return;
    case Kind::DescriptorHeap:
        descriptor_heap_method(m, static_cast<DescriptorHeap&>(object), slot);
        return;
    case Kind::RootSignature:
        root_signature_method(m, static_cast<RootSignature&>(object), slot);
        return;
    case Kind::PipelineState:
        pipeline_state_method(m, static_cast<PipelineState&>(object), slot);
        return;
    case Kind::QueryHeap:
    case Kind::CommandSignature:
        simple_child_method(m, object, slot);
        return;
    }
}

void Frontend::simple_child_method(Marshaler& m, Object& object, usize slot) {
    // ID3D12QueryHeap and ID3D12CommandSignature add nothing to
    // ID3D12Pageable: every slot is handled by common_method.
    d3d12_diag("slot %zu on kind %u has no handler", slot,
               static_cast<unsigned>(object.kind));
    m.set_return(E_NOTIMPL);
}

}  // namespace vitreo::winapi::d3d12
