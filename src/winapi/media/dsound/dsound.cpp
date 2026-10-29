/// @file dsound.cpp
/// @brief DirectSoundManager storage and object identity: the guest vtable
/// region, object allocation and the destructor. The per-object method
/// bodies live in the sibling dsound_*.cpp files.

#include "winapi/media/dsound/dsound_internal.hpp"

namespace vitreo::winapi {
namespace {

/// Guest region for every DirectSound vtable and object block.
constexpr usize STORAGE_SIZE = 0x100000;
constexpr usize VTABLE_BYTES = 0x4000;

struct MethodSlot {
    const char* name;
    u16 stdcall_bytes;
};

constexpr MethodSlot DSOUND_SLOTS[] = {
    {"Vitreo_DSound_QueryInterface", 12},
    {"Vitreo_DSound_AddRef", 4},
    {"Vitreo_DSound_Release", 4},
    {"Vitreo_DSound_CreateSoundBuffer", 16},
    {"Vitreo_DSound_GetCaps", 8},
    {"Vitreo_DSound_DuplicateSoundBuffer", 12},
    {"Vitreo_DSound_SetCooperativeLevel", 12},
    {"Vitreo_DSound_Compact", 4},
    {"Vitreo_DSound_GetSpeakerConfig", 8},
    {"Vitreo_DSound_SetSpeakerConfig", 8},
    {"Vitreo_DSound_Initialize", 8},
    {"Vitreo_DSound_VerifyCertification", 8},
};

constexpr MethodSlot BUFFER_SLOTS[] = {
    {"Vitreo_DSBuffer_QueryInterface", 12},
    {"Vitreo_DSBuffer_AddRef", 4},
    {"Vitreo_DSBuffer_Release", 4},
    {"Vitreo_DSBuffer_GetCaps", 8},
    {"Vitreo_DSBuffer_GetCurrentPosition", 12},
    {"Vitreo_DSBuffer_GetFormat", 16},
    {"Vitreo_DSBuffer_GetVolume", 8},
    {"Vitreo_DSBuffer_GetPan", 8},
    {"Vitreo_DSBuffer_GetFrequency", 8},
    {"Vitreo_DSBuffer_GetStatus", 8},
    {"Vitreo_DSBuffer_Initialize", 12},
    {"Vitreo_DSBuffer_Lock", 32},
    {"Vitreo_DSBuffer_Play", 16},
    {"Vitreo_DSBuffer_SetCurrentPosition", 8},
    {"Vitreo_DSBuffer_SetFormat", 8},
    {"Vitreo_DSBuffer_SetVolume", 8},
    {"Vitreo_DSBuffer_SetPan", 8},
    {"Vitreo_DSBuffer_SetFrequency", 8},
    {"Vitreo_DSBuffer_Stop", 4},
    {"Vitreo_DSBuffer_Unlock", 20},
    {"Vitreo_DSBuffer_Restore", 4},
    {"Vitreo_DSBuffer_SetFX", 16},
    {"Vitreo_DSBuffer_AcquireResources", 16},
    {"Vitreo_DSBuffer_GetObjectInPath", 20},
};

constexpr MethodSlot LISTENER_SLOTS[] = {
    {"Vitreo_DS3DListener_QueryInterface", 12},
    {"Vitreo_DS3DListener_AddRef", 4},
    {"Vitreo_DS3DListener_Release", 4},
    {"Vitreo_DS3DListener_GetAllParameters", 8},
    {"Vitreo_DS3DListener_GetDistanceFactor", 8},
    {"Vitreo_DS3DListener_GetDopplerFactor", 8},
    {"Vitreo_DS3DListener_GetOrientation", 12},
    {"Vitreo_DS3DListener_GetPosition", 8},
    {"Vitreo_DS3DListener_GetRolloffFactor", 8},
    {"Vitreo_DS3DListener_GetVelocity", 8},
    {"Vitreo_DS3DListener_SetAllParameters", 12},
    {"Vitreo_DS3DListener_SetDistanceFactor", 12},
    {"Vitreo_DS3DListener_SetDopplerFactor", 12},
    {"Vitreo_DS3DListener_SetOrientation", 32},
    {"Vitreo_DS3DListener_SetPosition", 20},
    {"Vitreo_DS3DListener_SetRolloffFactor", 12},
    {"Vitreo_DS3DListener_SetVelocity", 20},
    {"Vitreo_DS3DListener_CommitDeferredSettings", 4},
};

// dsound.h IDirectSoundCapture vtable order.
constexpr MethodSlot CAPTURE_SLOTS[] = {
    {"Vitreo_DSCapture_QueryInterface", 12},
    {"Vitreo_DSCapture_AddRef", 4},
    {"Vitreo_DSCapture_Release", 4},
    {"Vitreo_DSCapture_CreateCaptureBuffer", 16},
    {"Vitreo_DSCapture_GetCaps", 8},
    {"Vitreo_DSCapture_Initialize", 8},
};

// dsound.h IDirectSoundCaptureBuffer8 vtable order.
constexpr MethodSlot CBUFFER_SLOTS[] = {
    {"Vitreo_DSCBuffer_QueryInterface", 12},
    {"Vitreo_DSCBuffer_AddRef", 4},
    {"Vitreo_DSCBuffer_Release", 4},
    {"Vitreo_DSCBuffer_GetCaps", 8},
    {"Vitreo_DSCBuffer_GetCurrentPosition", 12},
    {"Vitreo_DSCBuffer_GetFormat", 16},
    {"Vitreo_DSCBuffer_GetStatus", 8},
    {"Vitreo_DSCBuffer_Initialize", 12},
    {"Vitreo_DSCBuffer_Lock", 32},
    {"Vitreo_DSCBuffer_Start", 8},
    {"Vitreo_DSCBuffer_Stop", 4},
    {"Vitreo_DSCBuffer_Unlock", 20},
    {"Vitreo_DSCBuffer_GetObjectInPath", 20},
    {"Vitreo_DSCBuffer_GetFXStatus", 12},
};

constexpr MethodSlot NOTIFY_SLOTS[] = {
    {"Vitreo_DSNotify_QueryInterface", 12},
    {"Vitreo_DSNotify_AddRef", 4},
    {"Vitreo_DSNotify_Release", 4},
    {"Vitreo_DSNotify_SetNotificationPositions", 12},
};

// IUnknown facets (QueryInterface for IUnknown answers a pointer of its own,
// audio_capture_contract_probe "QI(IUnknown) same=0").
constexpr MethodSlot CAPTURE_UNKNOWN_SLOTS[] = {
    {"Vitreo_DSCapture_QueryInterface", 12},
    {"Vitreo_DSCapture_AddRef", 4},
    {"Vitreo_DSCapture_Release", 4},
};
constexpr MethodSlot CBUFFER_UNKNOWN_SLOTS[] = {
    {"Vitreo_DSCBuffer_QueryInterface", 12},
    {"Vitreo_DSCBuffer_AddRef", 4},
    {"Vitreo_DSCBuffer_Release", 4},
};

constexpr MethodSlot FULL_DUPLEX_SLOTS[] = {
    {"Vitreo_DSFullDuplex_QueryInterface", 12},
    {"Vitreo_DSFullDuplex_AddRef", 4},
    {"Vitreo_DSFullDuplex_Release", 4},
    {"Vitreo_DSFullDuplex_Initialize", 36},
};

constexpr MethodSlot FACTORY_SLOTS[] = {
    {"Vitreo_DSClassFactory_QueryInterface", 12},
    {"Vitreo_DSClassFactory_AddRef", 4},
    {"Vitreo_DSClassFactory_Release", 4},
    {"Vitreo_DSClassFactory_CreateInstance", 16},
    {"Vitreo_DSClassFactory_LockServer", 8},
};

} // namespace

// ---------------------------------------------------------------------------
// Storage and object identity
// ---------------------------------------------------------------------------

DirectSoundManager::~DirectSoundManager() {
    std::thread poller;
    std::vector<std::shared_ptr<audio::IAudioCaptureStream>> streams;
    {
        std::lock_guard lock(m_mutex);
        m_notify_stop = true;
        poller = std::move(m_notify_thread);
        for (auto& [address, buffer] : m_cbuffers) {
            (void)address;
            if (buffer.stream) streams.push_back(std::move(buffer.stream));
        }
    }
    m_notify_wake.notify_all();
    if (poller.joinable()) poller.join();
    for (auto& stream : streams) stream->stop();
    streams.clear();
}

void DirectSoundManager::note_guest_bitness(bool x64) noexcept {
    std::lock_guard lock(m_mutex);
    if (m_region_base == 0) m_x64 = x64;
}

bool DirectSoundManager::read_guid(GuestAddr address, std::array<u8, 16>& out) const {
    return m_memory && address != 0 &&
           m_memory->read_bytes(address, reinterpret_cast<Byte*>(out.data()), 16).is_ok();
}

bool DirectSoundManager::ensure_storage() {
    if (m_region_base != 0) return true;
    if (!m_memory || !m_thunks) return false;

    const GuestAddr base = m_memory->find_free_gap(0x63000000, STORAGE_SIZE);
    if (base == 0 ||
        m_memory->map_region(base, STORAGE_SIZE, MemoryProtection::ReadWrite).is_err()) {
        return false;
    }
    m_region_base = base;
    m_region_size = STORAGE_SIZE;
    m_next_object = base + VTABLE_BYTES;

    const u32 ptr = pointer_size();
    GuestAddr cursor = base;
    auto build = [&](Vtable which, const MethodSlot* slots, usize count) {
        m_vtables[static_cast<usize>(which)] = cursor;
        for (usize slot = 0; slot < count; ++slot) {
            auto thunk = m_thunks->get_thunk_address("dsound.dll", slots[slot].name);
            if (thunk.is_err() || !write_ptr(cursor + slot * ptr, thunk.value())) return false;
        }
        cursor += static_cast<GuestAddr>(count * ptr);
        return true;
    };
    const bool ok =
        build(Vtable::DSound, DSOUND_SLOTS, std::size(DSOUND_SLOTS)) &&
        build(Vtable::Buffer, BUFFER_SLOTS, std::size(BUFFER_SLOTS)) &&
        build(Vtable::Listener, LISTENER_SLOTS, std::size(LISTENER_SLOTS)) &&
        build(Vtable::Capture, CAPTURE_SLOTS, std::size(CAPTURE_SLOTS)) &&
        build(Vtable::CaptureBuffer, CBUFFER_SLOTS, std::size(CBUFFER_SLOTS)) &&
        build(Vtable::Notify, NOTIFY_SLOTS, std::size(NOTIFY_SLOTS)) &&
        build(Vtable::CaptureUnknown, CAPTURE_UNKNOWN_SLOTS, std::size(CAPTURE_UNKNOWN_SLOTS)) &&
        build(Vtable::CaptureBufferUnknown, CBUFFER_UNKNOWN_SLOTS,
              std::size(CBUFFER_UNKNOWN_SLOTS)) &&
        build(Vtable::FullDuplex, FULL_DUPLEX_SLOTS, std::size(FULL_DUPLEX_SLOTS)) &&
        build(Vtable::Factory, FACTORY_SLOTS, std::size(FACTORY_SLOTS));
    if (!ok) {
        (void)m_memory->unmap_region(base, STORAGE_SIZE);
        m_region_base = 0;
        return false;
    }
    return true;
}

GuestAddr DirectSoundManager::allocate_object(u32 bytes) {
    bytes = (bytes + 15u) & ~15u;
    auto& free_list = m_free_objects[bytes];
    if (!free_list.empty()) {
        const GuestAddr reused = free_list.back();
        free_list.pop_back();
        return reused;
    }
    // The last page is callback-string and Full Duplex scratch.
    if (m_next_object + bytes > m_region_base + m_region_size - 0x1000) return 0;
    const GuestAddr object = m_next_object;
    m_next_object += bytes;
    return object;
}

void DirectSoundManager::free_object(GuestAddr object, u32 bytes) {
    if (object == 0) return;
    bytes = (bytes + 15u) & ~15u;
    m_free_objects[bytes].push_back(object);
}

GuestAddr DirectSoundManager::new_object(Vtable vtable, u32 interfaces) {
    if (!ensure_storage()) return 0;
    const GuestAddr object = allocate_object(interfaces * pointer_size());
    if (object == 0) return 0;
    if (!write_ptr(object, m_vtables[static_cast<usize>(vtable)])) return 0;
    return object;
}

} // namespace vitreo::winapi
