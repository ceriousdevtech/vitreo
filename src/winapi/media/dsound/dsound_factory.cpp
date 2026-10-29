/// @file dsound_factory.cpp
/// @brief DllGetClassObject and the in-proc IClassFactory.

#include "winapi/media/dsound/dsound_internal.hpp"

namespace vitreo::winapi {
// ---------------------------------------------------------------------------
// DllGetClassObject / IClassFactory
// ---------------------------------------------------------------------------

u32 DirectSoundManager::dll_get_class_object(GuestAddr clsid, GuestAddr iid, GuestAddr output) {
    std::lock_guard lock(m_mutex);
    if (!m_memory || output == 0) return dsound::E_POINTER;
    (void)write_ptr(output, 0);
    std::array<u8, 16> cls{}, requested{};
    if (!read_guid(clsid, cls) || !read_guid(iid, requested)) return dsound::E_INVALIDARG;
    FactoryClass which{};
    if (guid_equal(cls, CLSID_DIRECTSOUNDCAPTURE)) which = FactoryClass::Capture;
    else if (guid_equal(cls, CLSID_DIRECTSOUNDCAPTURE8)) which = FactoryClass::Capture8;
    else if (guid_equal(cls, CLSID_DIRECTSOUNDFULLDUPLEX)) which = FactoryClass::FullDuplex;
    else {
        if (guid_equal(cls, CLSID_DIRECTSOUND) || guid_equal(cls, CLSID_DIRECTSOUND8)) {
            static std::once_flag once;
            unimplemented_once(once, "dsound!DllGetClassObject: CLSID_DirectSound(8) "
                                     "(uninitialized playback objects)");
        }
        return dsound::CLASS_E_CLASSNOTAVAILABLE;
    }
    if (!guid_equal(requested, IID_IUNKNOWN) && !guid_equal(requested, IID_ICLASSFACTORY)) {
        return dsound::E_NOINTERFACE;
    }
    const GuestAddr factory = new_object(Vtable::Factory);
    if (factory == 0) return dsound::E_OUTOFMEMORY;
    m_factories.emplace(factory, FactoryObject{1, which});
    (void)write_ptr(output, factory);
    return dsound::DS_OK;
}

u32 DirectSoundManager::factory_query_interface(GuestAddr factory, GuestAddr iid,
                                                GuestAddr output) {
    std::lock_guard lock(m_mutex);
    if (!m_memory || output == 0) return dsound::E_POINTER;
    (void)write_ptr(output, 0);
    auto it = m_factories.find(factory);
    std::array<u8, 16> requested{};
    if (it == m_factories.end() || !read_guid(iid, requested)) return dsound::E_NOINTERFACE;
    if (!guid_equal(requested, IID_IUNKNOWN) && !guid_equal(requested, IID_ICLASSFACTORY)) {
        return dsound::E_NOINTERFACE;
    }
    ++it->second.refs;
    (void)write_ptr(output, factory);
    return dsound::DS_OK;
}

u32 DirectSoundManager::factory_add_ref(GuestAddr factory) {
    std::lock_guard lock(m_mutex);
    auto it = m_factories.find(factory);
    return it == m_factories.end() ? 0u : ++it->second.refs;
}

u32 DirectSoundManager::factory_release(GuestAddr factory) {
    std::lock_guard lock(m_mutex);
    auto it = m_factories.find(factory);
    if (it == m_factories.end()) return 0;
    const u32 refs = --it->second.refs;
    if (refs == 0) {
        m_factories.erase(it);
        free_object(factory, pointer_size());
    }
    return refs;
}

u32 DirectSoundManager::factory_create_instance(GuestAddr factory, GuestAddr outer,
                                                GuestAddr iid, GuestAddr output) {
    FactoryClass which{};
    GuestAddr created = 0;
    bool full_duplex = false;
    {
        std::lock_guard lock(m_mutex);
        if (!m_memory || output == 0) return dsound::E_POINTER;
        (void)write_ptr(output, 0);
        auto it = m_factories.find(factory);
        if (it == m_factories.end()) return dsound::E_INVALIDARG;
        if (outer != 0) return dsound::CLASS_E_NOAGGREGATION;
        which = it->second.cls;
        // Measured: a CoCreateInstance object is uninitialized until
        // Initialize binds it to a device.
        if (which == FactoryClass::FullDuplex) {
            created = new_object(Vtable::FullDuplex);
            if (created == 0) return dsound::E_OUTOFMEMORY;
            m_full_duplex.emplace(created, FullDuplexObject{1, false, 0, 0});
            full_duplex = true;
        } else {
            const u32 hr = create_capture_object(false, which == FactoryClass::Capture8, {},
                                                 created);
            if (hr != dsound::DS_OK) return hr;
        }
    }
    const u32 hr = full_duplex ? full_duplex_query_interface(created, iid, output)
                               : capture_query_interface(created, iid, output);
    // The creation reference is dropped: the QI reference keeps it alive.
    if (full_duplex) (void)full_duplex_release(created);
    else (void)capture_release(created);
    return hr;
}

u32 DirectSoundManager::factory_lock_server(GuestAddr factory, u32 lock_server) {
    std::lock_guard lock(m_mutex);
    (void)lock_server;
    return m_factories.count(factory) != 0 ? dsound::DS_OK : dsound::E_INVALIDARG;
}

} // namespace vitreo::winapi
