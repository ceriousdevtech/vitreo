/// @file dsound_device.cpp
/// @brief IDirectSound(8) device object, device enumeration and device identity.

#include "winapi/media/dsound/dsound_internal.hpp"

#include "winapi/kernel32/locale/string_conv.hpp"

#include <string_view>

namespace vitreo::winapi {
namespace {

constexpr u32 DSCAPS_BYTES = 96;

constexpr std::string_view kPrimaryCaptureDriver = "Primary Sound Capture Driver";

} // namespace

u32 DirectSoundManager::direct_sound_create(GuestAddr guid, GuestAddr output,
                                            GuestAddr outer, bool version8) {
    std::lock_guard lock(m_mutex);
    if (!m_memory || output == 0) return dsound::E_INVALIDARG;
    (void)write_ptr(output, 0);
    if (outer != 0) return dsound::CLASS_E_NOAGGREGATION;
    if (!m_renderer) return dsound::DSERR_NODRIVER;

    // A null GUID selects the default playback device; Vitreo exposes exactly
    // one device, so any other explicit device GUID has no driver.
    if (guid != 0) {
        Guid requested{};
        if (m_memory->read_bytes(guid, reinterpret_cast<Byte*>(&requested),
                                 sizeof(requested)).is_err()) {
            return dsound::E_INVALIDARG;
        }
        if (!guid_equal(requested, GUID_NULL) &&
            !guid_equal(requested, PLAYBACK_DEVICE) &&
            !guid_equal(requested, DSDEVID_DEFAULTPLAYBACK) &&
            !guid_equal(requested, DSDEVID_DEFAULTVOICEPLAYBACK)) return dsound::DSERR_NODRIVER;
    }
    const GuestAddr object = new_object(Vtable::DSound);
    if (object == 0 || !write_ptr(output, object)) return dsound::E_OUTOFMEMORY;
    Object state;
    state.version8 = version8;
    m_objects.emplace(object, state);
    VITREO_TRACE("[dsound] DirectSoundCreate -> object=0x%llx\n",
                 static_cast<unsigned long long>(object));
    return dsound::DS_OK;
}

u32 DirectSoundManager::query_interface(GuestAddr object, GuestAddr iid,
                                        GuestAddr output) {
    std::lock_guard lock(m_mutex);
    if (!m_memory || output == 0) return dsound::E_INVALIDARG;
    (void)write_ptr(output, 0);
    Object* state = find_object(object);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    Guid requested{};
    if (iid == 0 || m_memory->read_bytes(
            iid, reinterpret_cast<Byte*>(&requested), sizeof(requested)).is_err()) {
        return dsound::E_INVALIDARG;
    }
    if (!guid_equal(requested, IID_IUNKNOWN) &&
        !guid_equal(requested, IID_IDIRECTSOUND) &&
        !(state->version8 && guid_equal(requested, IID_IDIRECTSOUND8))) {
        return dsound::E_NOINTERFACE;
    }
    (void)write_ptr(output, object);
    ++state->ref_count;
    return dsound::DS_OK;
}

u32 DirectSoundManager::add_ref(GuestAddr object) {
    std::lock_guard lock(m_mutex);
    Object* state = find_object(object);
    return state ? ++state->ref_count : 0;
}

u32 DirectSoundManager::release(GuestAddr object) {
    std::lock_guard lock(m_mutex);
    Object* state = find_object(object);
    if (!state) return 0;
    const u32 result = --state->ref_count;
    if (result == 0) {
        m_objects.erase(object);
        free_object(object, pointer_size());
    }
    return result;
}

u32 DirectSoundManager::set_cooperative_level(GuestAddr object, u64 hwnd, u32 level) {
    std::lock_guard lock(m_mutex);
    Object* state = find_object(object);
    if (!state) return dsound::DSERR_UNINITIALIZED;
    if (hwnd == 0 || level < dsound::DSSCL_NORMAL || level > dsound::DSSCL_WRITEPRIMARY) {
        return dsound::E_INVALIDARG;
    }
    state->hwnd = hwnd;
    state->cooperative_level = level;
    VITREO_TRACE("[dsound] SetCooperativeLevel object=0x%llx hwnd=0x%llx level=%u\n",
                 static_cast<unsigned long long>(object),
                 static_cast<unsigned long long>(hwnd), level);
    return dsound::DS_OK;
}

u32 DirectSoundManager::get_caps(GuestAddr object, GuestAddr caps) {
    std::lock_guard lock(m_mutex);
    if (!find_object(object)) return dsound::DSERR_UNINITIALIZED;
    if (!m_memory || caps == 0) return dsound::E_INVALIDARG;
    if (m_memory->read_u32(caps).value_or(0) != DSCAPS_BYTES) {
        return dsound::E_INVALIDARG;
    }
    for (u32 offset = 4; offset < DSCAPS_BYTES; offset += 4) {
        (void)m_memory->write_u32(caps + offset, 0);
    }
    // Software mixing only: the format capability bits are real, and every
    // hardware mixing/3D count stays zero because none is provided.
    constexpr u32 DSCAPS_PRIMARYMONO = 0x1;
    constexpr u32 DSCAPS_PRIMARYSTEREO = 0x2;
    constexpr u32 DSCAPS_PRIMARY8BIT = 0x4;
    constexpr u32 DSCAPS_PRIMARY16BIT = 0x8;
    constexpr u32 DSCAPS_CONTINUOUSRATE = 0x10;
    constexpr u32 DSCAPS_SECONDARYMONO = 0x100;
    constexpr u32 DSCAPS_SECONDARYSTEREO = 0x200;
    constexpr u32 DSCAPS_SECONDARY8BIT = 0x400;
    constexpr u32 DSCAPS_SECONDARY16BIT = 0x800;
    (void)m_memory->write_u32(caps + 4,
        DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT |
        DSCAPS_PRIMARY16BIT | DSCAPS_CONTINUOUSRATE | DSCAPS_SECONDARYMONO |
        DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT);
    (void)m_memory->write_u32(caps + 8, 100);      // dwMinSecondarySampleRate
    (void)m_memory->write_u32(caps + 12, 200000);  // dwMaxSecondarySampleRate
    (void)m_memory->write_u32(caps + 16, 1);       // dwPrimaryBuffers
    return dsound::DS_OK;
}

u32 DirectSoundManager::cooperative_level(GuestAddr object) const {
    std::lock_guard lock(m_mutex);
    const Object* state = find_object(object);
    return state ? state->cooperative_level : 0;
}

// ---------------------------------------------------------------------------
// Device enumeration and identity
// ---------------------------------------------------------------------------

u32 DirectSoundManager::enumerate(GuestAddr callback, GuestAddr context,
                                    bool wide, bool capture, bool x64) {
    if (!callback) return dsound::E_INVALIDARG;
    if (!m_guest_call) return dsound::E_NOTIMPL;
    note_guest_bitness(x64);
    GuestAddr guid = 0, description = 0, module = 0;
    std::vector<audio::CaptureDevice> devices;
    if (capture) {
        if (!m_capture) return dsound::DS_OK;
        devices = audio::capture_devices_in_id_order(*m_capture);
        if (devices.empty()) return dsound::DS_OK;
    } else if (!m_renderer) {
        return dsound::DS_OK;
    }
    {
        std::lock_guard lock(m_mutex);
        if (!ensure_storage()) return dsound::E_OUTOFMEMORY;
        // Scratch for the callback strings: the last page of the region.
        guid = m_region_base + m_region_size - 0x1000;
        description = guid + 0x10;
        module = description + 0x400;
    }
    const auto put = [&](GuestAddr address, const std::u16string& text) {
        if (wide) {
            for (usize i = 0; i < text.size() && i < 0x1FF; ++i) {
                (void)m_memory->write_u16(address + i * 2, static_cast<u16>(text[i]));
            }
            (void)m_memory->write_u16(address + std::min<usize>(text.size(), 0x1FF) * 2, 0);
        } else {
            std::vector<u16> units(text.begin(), text.end());
            units.push_back(0);
            const std::string ansi = utf8_to_acp(utf16_to_utf8(units.data()));
            const usize n = std::min<usize>(ansi.size(), 0x3FF);
            (void)m_memory->write_bytes(address, reinterpret_cast<const Byte*>(ansi.data()), n);
            (void)m_memory->write_u8(address + n, 0);
        }
    };
    const auto to_u16 = [](std::string_view ascii) {
        return std::u16string(ascii.begin(), ascii.end());
    };
    if (!capture) {
        (void)m_memory->write_bytes(guid, reinterpret_cast<const Byte*>(&PLAYBACK_DEVICE), 16);
        put(description, u"Vitreo audio output");
        put(module, u"dsound.dll");
        // Windows enumerates the primary driver (null GUID), then the device.
        u64 args[] = {0, description, module, context};
        if (m_guest_call(callback, args, 4, x64)) {
            args[0] = guid;
            (void)m_guest_call(callback, args, 4, x64);
        }
        return dsound::DS_OK;
    }
    // Measured: "Primary Sound Capture Driver" with a NULL GUID and an empty
    // module first, then every device (default first) under its endpoint GUID,
    // with its endpoint friendly name and its endpoint id as the module.
    put(description, to_u16(kPrimaryCaptureDriver));
    put(module, u"");
    u64 args[] = {0, description, module, context};
    if (!m_guest_call(callback, args, 4, x64)) return dsound::DS_OK;
    for (const auto& device : devices) {
        const auto g = audio::capture_device_guid(device.uid);
        (void)m_memory->write_bytes(guid, reinterpret_cast<const Byte*>(g.data()), 16);
        put(description, device.name);
        put(module, to_u16(audio::capture_endpoint_id(device.uid)));
        args[0] = guid;
        if (!m_guest_call(callback, args, 4, x64)) break;
    }
    return dsound::DS_OK;
}

bool DirectSoundManager::resolve_capture_device(GuestAddr guid, std::string& uid) const {
    if (!m_capture) return false;
    std::array<u8, 16> requested{};
    if (guid != 0 && !read_guid(guid, requested)) return false;
    const bool wants_default = guid == 0 || guid_equal(requested, GUID_NULL) ||
                               guid_equal(requested, DSDEVID_DEFAULTCAPTURE) ||
                               guid_equal(requested, DSDEVID_DEFAULTVOICECAPTURE);
    const auto devices = audio::capture_devices_in_id_order(*m_capture);
    if (devices.empty()) return false;
    if (wants_default) {
        // Vitreo maps every capture role to the host default input; with no
        // host default the first device in id order holds the role.
        uid = devices.front().uid;
        return true;
    }
    for (const auto& device : devices) {
        if (audio::capture_device_guid(device.uid) == requested) {
            uid = device.uid;
            return true;
        }
    }
    return false;
}

u32 DirectSoundManager::get_device_id(GuestAddr source, GuestAddr destination) {
    if (!m_memory || !source || !destination) return dsound::E_INVALIDARG;
    std::array<u8, 16> requested{};
    if (!read_guid(source, requested)) return dsound::E_INVALIDARG;
    const auto write = [&](const void* bytes) {
        return m_memory->write_bytes(destination, static_cast<const Byte*>(bytes), 16).is_ok()
            ? dsound::DS_OK : dsound::E_INVALIDARG;
    };
    if (guid_equal(requested, DSDEVID_DEFAULTPLAYBACK) ||
        guid_equal(requested, DSDEVID_DEFAULTVOICEPLAYBACK) ||
        guid_equal(requested, PLAYBACK_DEVICE)) {
        return m_renderer ? write(&PLAYBACK_DEVICE) : dsound::DSERR_NODRIVER;
    }
    // Measured: GUID_NULL and unknown GUIDs are DSERR_NODRIVER with the
    // destination untouched; the capture defaults resolve to a device GUID.
    if (guid_equal(requested, GUID_NULL)) return dsound::DSERR_NODRIVER;
    std::string uid;
    if (guid_equal(requested, DSDEVID_DEFAULTCAPTURE) ||
        guid_equal(requested, DSDEVID_DEFAULTVOICECAPTURE) ||
        !std::all_of(requested.begin(), requested.end(), [](u8 b) { return b == 0; })) {
        if (resolve_capture_device(source, uid)) {
            const auto g = audio::capture_device_guid(uid);
            return write(g.data());
        }
    }
    return dsound::DSERR_NODRIVER;
}

} // namespace vitreo::winapi
