// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_core.cpp
/// @brief ComCoreManager apartment ownership: CoInitialize(Ex), CoUninitialize, the
/// OLE TLS block published through TEB.ReservedForOle, MTA usage,
/// server-process refs, OleInitialize, RoInitialize and apartment-shutdown
/// callbacks, and the CoInitialize spies. The rest of ComCoreManager is in
/// the sibling com_*.cpp files and the thunk registration in
/// com_core_thunks*.cpp.

#include "com_core.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "com_core_internal.hpp"

#include <optional>
#include <cstdio>
#include <vector>

#include <unistd.h>

namespace vitreo::winapi {

// =============================================================================
// Thread COM apartment state
// =============================================================================
//
// Windows records the calling thread's apartment in an OLE TLS block whose
// address lives in TEB.ReservedForOle. Guest code reads it directly rather
// than calling CoGetApartmentType. Callers dereference
// NtCurrentTeb()->ReservedForOle and require a live block, so the
// block must exist, not just the host-side bookkeeping.
//
// Offsets and flag values measured on a real Windows x64/x86 host by
// snapshotting the TEB across CoInitializeEx / CoUninitialize:
//
//   TEB.ReservedForOle          x86 TEB+0x0F80   x64 TEB+0x1758
//   OleTlsData.apartment_flags  x86  blk+0x0C    x64  blk+0x14
//
//   CoInitializeEx(MULTITHREADED)                 -> flags 0x141
//   CoInitializeEx(APARTMENTTHREADED)             -> flags 0x081
//   CoInitializeEx(APARTMENTTHREADED|DISABLE_DDE) -> flags 0x0C1
//   after the last CoUninitialize                 -> flags 0x001
//
// The block itself is *not* freed by CoUninitialize on Windows: only the
// apartment bits are cleared, leaving OLETLS_LOCALTID, and a later
// CoInitializeEx reuses the same block. Vitreo matches that.
//
// The same measurement showed that only COINIT_APARTMENTTHREADED (bit 0x2)
// selects the apartment: adding COINIT_DISABLE_OLE1DDE or
// COINIT_SPEED_OVER_MEMORY to an already-entered mode returns S_FALSE, not
// RPC_E_CHANGED_MODE.
namespace {

constexpr u32 TEB_RESERVED_FOR_OLE_32 = 0x0F80;
constexpr u32 TEB_RESERVED_FOR_OLE_64 = 0x1758;
constexpr u32 OLE_TLS_APARTMENT_FLAGS_32 = 0x0C;
constexpr u32 OLE_TLS_APARTMENT_FLAGS_64 = 0x14;

constexpr u32 OLETLS_LOCALTID = 0x001;
constexpr u32 OLETLS_DISABLE_OLE1DDE = 0x040;
constexpr u32 OLETLS_APARTMENTTHREADED = 0x080;
constexpr u32 OLETLS_MULTITHREADED = 0x100;
// Measured: RoInitialize additionally sets this bit (MTA 0x00200141,
// STA 0x002000C1); CoInitializeEx alone never does.
constexpr u32 OLETLS_WINRT_INITIALIZED = 0x200000;

// RO_INIT_TYPE (roapi.h)
constexpr u32 RO_INIT_SINGLETHREADED = 0;
constexpr u32 RO_INIT_MULTITHREADED = 1;

// Windows' SOleTlsData is far larger than the fields a guest can observe
// through this path; reserve a whole block per thread so an over-reading
// guest still lands on zeroed, mapped memory.
constexpr u32 kOleTlsBlockBytes = 0x100;
constexpr usize kOleTlsArenaBytes = 0x10000;
constexpr GuestAddr kOleTlsArenaMin = 0x5E0A0000;

[[nodiscard]] GuestAddr teb_ole_field(Marshaler& m, GuestAddr teb) {
    return teb + (m.cpu_is_64bit() ? TEB_RESERVED_FOR_OLE_64
                                   : TEB_RESERVED_FOR_OLE_32);
}

[[nodiscard]] u32 apartment_flags_offset(Marshaler& m) {
    return m.cpu_is_64bit() ? OLE_TLS_APARTMENT_FLAGS_64
                            : OLE_TLS_APARTMENT_FLAGS_32;
}

}  // namespace

namespace {

// VITREO_COM_DIAG, or the /tmp/vitreo-com-diag marker file: a store-launched
// guest does not reliably inherit the environment.
bool com_diag_enabled() {
    static const bool enabled = std::getenv("VITREO_COM_DIAG") != nullptr ||
                                ::access("/tmp/vitreo-com-diag", F_OK) == 0;
    return enabled;
}

} // namespace

void ComCoreManager::discard_stale_apartment(Marshaler& m,
                                             ApartmentState& state) {
    // TEB addresses are recycled when a thread exits and another starts, so a
    // cached entry can belong to a thread that no longer exists. The new
    // thread's TEB.ReservedForOle is zero, which is the tell: drop the stale
    // apartment rather than letting the new thread inherit it.
    if (state.ole_tls == 0) return;
    const GuestAddr teb = guest_teb(m);
    if (teb == 0) return;
    const GuestAddr field = teb_ole_field(m, teb);
    const GuestAddr published = static_cast<GuestAddr>(
        m.cpu_is_64bit() ? m.read_u64(field).value_or(0)
                         : static_cast<u64>(m.read_u32(field).value_or(0)));
    if (published != state.ole_tls) state = ApartmentState{};
}

GuestAddr ComCoreManager::ensure_ole_tls(Marshaler& m, ApartmentState& state) {
    if (state.ole_tls != 0) return state.ole_tls;
    const GuestAddr teb = guest_teb(m);
    if (teb == 0) return 0;

    // A thread may already carry a block from an earlier initialize.
    const GuestAddr field = teb_ole_field(m, teb);
    GuestAddr existing = 0;
    if (m.cpu_is_64bit()) {
        existing = static_cast<GuestAddr>(m.read_u64(field).value_or(0));
    } else {
        existing = static_cast<GuestAddr>(m.read_u32(field).value_or(0));
    }
    if (existing != 0) {
        state.ole_tls = existing;
        return existing;
    }

    if (m_ole_tls_next + kOleTlsBlockBytes > m_ole_tls_end) {
        GuestMemory& mem = m.memory();
        const GuestAddr base = mem.find_free_gap(kOleTlsArenaMin, kOleTlsArenaBytes);
        if (base == 0 ||
            mem.map_region(base, kOleTlsArenaBytes,
                           MemoryProtection::ReadWrite).is_err()) {
            VITREO_TRACE("[ole32] failed to allocate OLE TLS arena\n");
            return 0;
        }
        for (usize i = 0; i < kOleTlsArenaBytes; ++i) (void)mem.write_u8(base + i, 0);
        m_ole_tls_next = base;
        m_ole_tls_end = base + kOleTlsArenaBytes;
    }

    const GuestAddr block = m_ole_tls_next;
    m_ole_tls_next += kOleTlsBlockBytes;
    if (m.cpu_is_64bit()) {
        (void)m.write_u64(field, block);
    } else {
        (void)m.write_u32(field, static_cast<u32>(block));
    }
    state.ole_tls = block;
    VITREO_TRACE("[ole32] OLE TLS block 0x%llx published at TEB+0x%x\n",
                 static_cast<unsigned long long>(block),
                 static_cast<unsigned>(field - teb));
    return block;
}

void ComCoreManager::publish_ole_tls(Marshaler& m, ApartmentState& state) {
    const GuestAddr block = ensure_ole_tls(m, state);
    if (block == 0) return;
    u32 flags = OLETLS_LOCALTID;
    if (state.apartment_set) {
        if (state.sta) {
            flags |= OLETLS_APARTMENTTHREADED;
            if (state.disable_ole1dde) flags |= OLETLS_DISABLE_OLE1DDE;
        } else {
            flags |= OLETLS_MULTITHREADED | OLETLS_DISABLE_OLE1DDE;
        }
        if (state.winrt) flags |= OLETLS_WINRT_INITIALIZED;
    }
    (void)m.write_u32(block + apartment_flags_offset(m), flags);
}


u32 ComCoreManager::co_initialize_ex(u32 coinit) {
    std::lock_guard<std::mutex> lock(m_com_mutex);
    ApartmentState& state = m_apartments[0];
    const bool sta = (coinit & com::COINIT_APARTMENTTHREADED) != 0;
    if (!state.apartment_set) {
        state.sta = sta;
        state.disable_ole1dde = (coinit & com::COINIT_DISABLE_OLE1DDE) != 0;
        state.apartment_set = true;
        state.init_count = 1;
        return com::S_OK;
    }
    // Already initialized. Only the threading model selects the apartment.
    if (state.sta != sta) {
        return com::RPC_E_CHANGED_MODE;
    }
    state.init_count++;
    return com::S_FALSE;
}

u32 ComCoreManager::initialize_apartment(Marshaler& m, u32 coinit, bool winrt) {
    const GuestAddr key = guest_teb(m);
    std::lock_guard<std::mutex> lock(m_com_mutex);
    ApartmentState& state = m_apartments[key];
    discard_stale_apartment(m, state);
    const bool sta = (coinit & com::COINIT_APARTMENTTHREADED) != 0;
    u32 hr;
    if (!state.apartment_set) {
        state.sta = sta;
        state.disable_ole1dde = (coinit & com::COINIT_DISABLE_OLE1DDE) != 0;
        state.winrt = winrt;
        state.apartment_set = true;
        state.init_count = 1;
        hr = com::S_OK;
    } else if (state.sta != sta) {
        // Measured: the published flags are left alone on a mode conflict.
        return com::RPC_E_CHANGED_MODE;
    } else {
        // Measured: RoInitialize and CoInitializeEx share one count, and the
        // WinRT marker sticks once either has set it.
        state.winrt = state.winrt || winrt;
        state.init_count++;
        hr = com::S_FALSE;
    }
    publish_ole_tls(m, state);
    if (com_diag_enabled()) {
        std::fprintf(stderr,
                     "[com_diag pid=%d] %s teb=0x%llx coinit=0x%x -> "
                     "0x%08x block=0x%llx count=%u\n",
                     static_cast<int>(::getpid()),
                     winrt ? "RoInitialize" : "CoInitializeEx",
                     static_cast<unsigned long long>(key), coinit, hr,
                     static_cast<unsigned long long>(state.ole_tls),
                     state.init_count);
    }
    return hr;
}

u32 ComCoreManager::co_initialize_ex(Marshaler& m, u32 coinit) {
    return initialize_apartment(m, coinit, /*winrt=*/false);
}

u32 ComCoreManager::ro_initialize(Marshaler& m, u32 init_type) {
    // Measured: RO_INIT_SINGLETHREADED lands on an STA that also carries the
    // DISABLE_OLE1DDE bit; RO_INIT_MULTITHREADED lands on the same MTA
    // CoInitializeEx(COINIT_MULTITHREADED) produces.
    const u32 coinit = (init_type == RO_INIT_MULTITHREADED)
                           ? com::COINIT_MULTITHREADED
                           : (com::COINIT_APARTMENTTHREADED |
                              com::COINIT_DISABLE_OLE1DDE);
    if (init_type != RO_INIT_SINGLETHREADED &&
        init_type != RO_INIT_MULTITHREADED) {
        return com::E_INVALIDARG;
    }
    return initialize_apartment(m, coinit, /*winrt=*/true);
}

u32 ComCoreManager::co_initialize() {
    return co_initialize_ex(com::COINIT_APARTMENTTHREADED);
}

u32 ComCoreManager::co_initialize(Marshaler& m) {
    return co_initialize_ex(m, com::COINIT_APARTMENTTHREADED);
}

void ComCoreManager::co_uninitialize() {
    std::lock_guard<std::mutex> lock(m_com_mutex);
    ApartmentState& state = m_apartments[0];
    if (state.init_count > 0) {
        state.init_count--;
        if (state.init_count == 0 && !state.mta_usage_joined) {
            state.apartment_set = false;
        }
    }
}

void ComCoreManager::co_uninitialize(Marshaler& m) {
    const GuestAddr key = guest_teb(m);
    bool final_call = false;
    {
        std::lock_guard<std::mutex> lock(m_com_mutex);
        ApartmentState& state = m_apartments[key];
        discard_stale_apartment(m, state);
        if (state.init_count > 0) {
            state.init_count--;
            if (state.init_count == 0) {
                final_call = true;
                if (!state.mta_usage_joined) state.apartment_set = false;
            }
        }
        // The block survives; only the apartment bits are cleared.
        if (state.ole_tls != 0 || state.apartment_set) publish_ole_tls(m, state);
        if (com_diag_enabled()) {
            u32 mta_holders = 0;
            for (const auto& [teb, other] : m_apartments)
                if (!other.sta && other.init_count > 0) ++mta_holders;
            std::fprintf(stderr,
                         "[com_diag pid=%d] CoUninitialize teb=0x%llx sta=%d count=%u "
                         "mta_holders=%u\n",
                         static_cast<int>(::getpid()), static_cast<unsigned long long>(key),
                         state.sta ? 1 : 0, state.init_count, mta_holders);
        }
    }
    // Learn: the final call "unloads all DLLs loaded by the thread, frees any
    // other resources that the thread maintains, and forces all RPC
    // connections on the thread to close".
    if (final_call) release_apartment_resources(m, key);
}

void ComCoreManager::release_apartment_resources(Marshaler& m, GuestAddr key) {
    // RoRegisterForApartmentShutdown: an STA ends with its thread's final
    // uninitialize; the MTA ends when no thread holds it any more.
    u64 ended_sta = 0, ended_mta = 0;
    {
        std::lock_guard<std::mutex> lock(m_com_mutex);
        auto it = m_apartments.find(key);
        if (it != m_apartments.end() && it->second.sta_id != 0) {
            ended_sta = it->second.sta_id;
            it->second.sta_id = 0;
        } else if (m_mta_apartment_id != 0) {
            if (!process_mta_alive_locked()) {
                ended_mta = m_mta_apartment_id;
                m_mta_apartment_id = 0;
            }
        }
    }
    if (ended_sta != 0) fire_apartment_shutdown(m, ended_sta);
    if (ended_mta != 0) fire_apartment_shutdown(m, ended_mta);

    std::vector<GuestAddr> class_objects;
    {
        std::lock_guard lock(m_class_objects_mutex);
        for (auto it = m_class_objects.begin(); it != m_class_objects.end();) {
            if (it->second.apartment == key) {
                class_objects.push_back(it->second.class_object);
                it = m_class_objects.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const GuestAddr object : class_objects) guest_iunknown_release(m, object);
    std::vector<GuestAddr> libraries;
    {
        std::lock_guard<std::mutex> lock(m_com_mutex);
        if (auto it = m_autofree_libraries.find(key); it != m_autofree_libraries.end()) {
            libraries = std::move(it->second);
            m_autofree_libraries.erase(it);
        }
    }
    if (m_modules != nullptr) {
        for (const GuestAddr module : libraries) (void)m_modules->free_library(module);
    }
}

u32 ComCoreManager::co_increment_mta_usage(GuestAddr* cookie) {
    if (cookie == nullptr) return com::E_INVALIDARG;

    std::lock_guard<std::mutex> lock(m_com_mutex);
    ApartmentState& state = m_apartments[0];
    if (!state.apartment_set) {
        state.sta = false;
        state.apartment_set = true;
        state.mta_usage_joined = true;
    }

    const GuestAddr value = m_next_mta_usage_cookie++;
    m_mta_usage_cookies.insert(value);
    *cookie = value;
    static const bool com_diag = std::getenv("VITREO_COM_DIAG") != nullptr;
    if (com_diag) {
        std::fprintf(stderr, "[com_diag pid=%d] CoIncrementMTAUsage\n",
                     static_cast<int>(::getpid()));
    }
    VITREO_TRACE(
        "[ole32] CoIncrementMTAUsage -> S_OK cookie=0x%llx count=%zu\n",
        static_cast<unsigned long long>(value), m_mta_usage_cookies.size());
    return com::S_OK;
}

u32 ComCoreManager::co_decrement_mta_usage(GuestAddr cookie) {
    std::lock_guard<std::mutex> lock(m_com_mutex);
    const auto erased = m_mta_usage_cookies.erase(cookie);
    if (erased == 0) {
        VITREO_TRACE(
            "[ole32] CoDecrementMTAUsage cookie=0x%llx -> E_INVALIDARG\n",
            static_cast<unsigned long long>(cookie));
        return com::E_INVALIDARG;
    }

    ApartmentState& state = m_apartments[0];
    if (m_mta_usage_cookies.empty() && state.mta_usage_joined) {
        state.mta_usage_joined = false;
        if (state.init_count == 0) state.apartment_set = false;
    }
    VITREO_TRACE(
        "[ole32] CoDecrementMTAUsage cookie=0x%llx -> S_OK count=%zu\n",
        static_cast<unsigned long long>(cookie), m_mta_usage_cookies.size());
    return com::S_OK;
}

u32 ComCoreManager::co_add_ref_server_process() {
    std::lock_guard lock(m_class_objects_mutex);
    return ++m_server_process_refs;
}

u32 ComCoreManager::co_release_server_process() {
    std::lock_guard lock(m_class_objects_mutex);
    if (m_server_process_refs == 0) return 0;
    --m_server_process_refs;
    if (m_server_process_refs == 0) {
        // Windows automatically performs CoSuspendClassObjects when the
        // global per-process server count reaches zero.
        for (auto& [cookie, registration] : m_class_objects) {
            (void)cookie;
            registration.suspended = true;
            if (registration.shared_registration) {
                (void)registration.shared_registration->set_suspended(true);
            }
        }
    }
    return m_server_process_refs;
}

u32 ComCoreManager::co_resume_class_objects() {
    std::lock_guard lock(m_class_objects_mutex);
    for (auto& [cookie, registration] : m_class_objects) {
        (void)cookie;
        registration.suspended = false;
        if (registration.shared_registration) {
            (void)registration.shared_registration->set_suspended(false);
        }
    }
    return com::S_OK;
}

u32 ComCoreManager::co_suspend_class_objects() {
    std::lock_guard lock(m_class_objects_mutex);
    for (auto& [cookie, registration] : m_class_objects) {
        (void)cookie;
        registration.suspended = true;
        if (registration.shared_registration) {
            (void)registration.shared_registration->set_suspended(true);
        }
    }
    return com::S_OK;
}

u32 ComCoreManager::co_create_instance() const {
    return com::CLASS_E_CLASSNOTAVAILABLE;
}

GuestAddr ComCoreManager::co_load_library(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    const u32       auto_free = static_cast<u32>(m.arg(1));
    if (name_ptr == 0 || m_modules == nullptr) {
        VITREO_TRACE("[ole32] CoLoadLibrary: %s -> NULL\n",
                     name_ptr == 0 ? "NULL name" : "no module manager");
        return 0;
    }

    auto name = m.read_wide_string(name_ptr);
    if (name.is_err() || name.value().empty()) {
        VITREO_TRACE("[ole32] CoLoadLibrary: unreadable name -> NULL\n");
        return 0;
    }

    // CoLoadLibrary is defined to behave like LoadLibrary, including the
    // reference count, so it goes through the same loader the guest's own
    // LoadLibrary uses. Failing here has to yield NULL, not a sentinel: the
    // caller passes the result straight to GetProcAddress.
    const GuestAddr module = m_modules->load_library(name.value());
    if (module != 0 && auto_free != 0) {
        std::lock_guard<std::mutex> lock(m_com_mutex);
        m_autofree_libraries[guest_teb(m)].push_back(module);
    }
    VITREO_TRACE("[ole32] CoLoadLibrary(\"%s\", autoFree=%u) -> 0x%08x\n",
                 name.value().c_str(), auto_free,
                 static_cast<unsigned>(module));
    return module;
}

void ComCoreManager::co_free_library(Marshaler& m) {
    const GuestAddr module = m.arg_ptr(0);
    if (module == 0 || m_modules == nullptr) return;
    const bool freed = m_modules->free_library(module);
    VITREO_TRACE("[ole32] CoFreeLibrary(0x%08x) -> %s\n",
                 static_cast<unsigned>(module), freed ? "released" : "no-op");
}

bool ComCoreManager::thread_in_apartment(Marshaler& m) {
    const GuestAddr teb = guest_teb(m);
    std::lock_guard<std::mutex> lock(m_com_mutex);
    auto own = m_apartments.find(teb);
    if (own != m_apartments.end()) {
        discard_stale_apartment(m, own->second);
        if (own->second.init_count > 0) return true;
    }
    return process_mta_alive_locked();
}

bool ComCoreManager::process_mta_alive_locked() const {
    if (!m_mta_usage_cookies.empty()) return true;
    for (const auto& [key, state] : m_apartments) {
        if (!state.sta && state.init_count > 0) return true;
    }
    return false;
}

void ComCoreManager::hold_audio_engine_mta_usage() {
    std::call_once(m_audio_engine_mta_once, [this] {
        GuestAddr cookie = 0;
        (void)co_increment_mta_usage(&cookie);  // never released: process lifetime
    });
}

void ComCoreManager::impl_CoRegisterInitializeSpy(Marshaler& m) {
    // HRESULT CoRegisterInitializeSpy(IInitializeSpy* pSpy,
    //                                 ULARGE_INTEGER* puliCookie)
    // Learn (objbase.h): register the spy and store an opaque cookie for
    // revoke. CoInitializeEx/CoUninitialize notifications are not delivered.
    // Registration itself must succeed before apartment work.
    const GuestAddr spy = m.arg_ptr(0);
    const GuestAddr cookie_ptr = m.arg_ptr(1);
    if (spy == 0 || cookie_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    std::lock_guard lock(m_initialize_spy_mutex);
    const u64 cookie = m_next_initialize_spy_cookie++;
    if (cookie == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    // ULARGE_INTEGER is 8 bytes (QuadPart) on both ABIs.
    if (m.write_u64(cookie_ptr, cookie).is_err()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    m_initialize_spies.emplace(cookie, spy);
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_CoRevokeInitializeSpy(Marshaler& m) {
    // HRESULT CoRevokeInitializeSpy(ULARGE_INTEGER uliCookie)
    // Cookie is passed by value as a 64-bit integer (two stack slots on x86).
    const u64 cookie = m.cpu_is_64bit()
        ? m.arg(0)
        : (static_cast<u64>(static_cast<u32>(m.arg(0))) |
           (static_cast<u64>(static_cast<u32>(m.arg(1))) << 32));
    std::lock_guard lock(m_initialize_spy_mutex);
    const auto it = m_initialize_spies.find(cookie);
    if (it == m_initialize_spies.end()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    m_initialize_spies.erase(it);
    m.set_return(com::S_OK);
}

u32 ComCoreManager::ole_initialize() {
    return co_initialize_ex(com::COINIT_APARTMENTTHREADED);
}

u32 ComCoreManager::ole_initialize(Marshaler& m) {
    // OleInitialize enters an STA and keeps its own per-thread OLE count.
    // Measured (s7_ole_probe.expected): the first OleInitialize on a thread
    // already in an STA returns S_OK, not CoInitializeEx's S_FALSE.
    const u32 hr = co_initialize_ex(m, com::COINIT_APARTMENTTHREADED);
    if ((hr & 0x80000000u) != 0) return hr;
    std::lock_guard<std::mutex> lock(m_com_mutex);
    ApartmentState& state = m_apartments[guest_teb(m)];
    return state.ole_init_count++ == 0 ? com::S_OK : com::S_FALSE;
}

std::optional<std::pair<u64, GuestAddr>> ComCoreManager::apartment_identity_locked(
    Marshaler& m) {
    const GuestAddr teb = guest_teb(m);
    auto own = m_apartments.find(teb);
    if (own != m_apartments.end() && own->second.init_count > 0) {
        if (own->second.sta) {
            if (own->second.sta_id == 0) own->second.sta_id = m_next_apartment_id++;
            return std::make_pair(own->second.sta_id, teb);
        }
    } else {
        if (!process_mta_alive_locked()) return std::nullopt;
    }
    if (m_mta_apartment_id == 0) m_mta_apartment_id = m_next_apartment_id++;
    return std::make_pair(m_mta_apartment_id, GuestAddr{0});
}

// HRESULT RoGetApartmentIdentifier(UINT64* apartmentIdentifier)
// Measured (tools/oracle_probe/s7_winrt_probe.expected): CO_E_NOTINITIALIZED
// with 0 written outside any apartment; one id for the MTA shared by
// implicit-MTA threads; a distinct id per STA.
u32 ComCoreManager::ro_get_apartment_identifier(Marshaler& m, GuestAddr out) {
    std::lock_guard<std::mutex> lock(m_com_mutex);
    auto identity = apartment_identity_locked(m);
    (void)m.write_u64(out, identity ? identity->first : 0);
    return identity ? com::S_OK : CO_E_NOTINITIALIZED;
}

// HRESULT RoRegisterForApartmentShutdown(IApartmentShutdown*, UINT64*,
//                                        APARTMENT_SHUTDOWN_REGISTRATION_COOKIE*)
// Measured: AddRefs the callback, writes the apartment id and a pointer-sized
// cookie; on the apartment's final uninitialize IApartmentShutdown::
// OnUninitialize(id) runs and the callback is Released.
u32 ComCoreManager::ro_register_for_apartment_shutdown(Marshaler& m, GuestAddr callback,
                                                       GuestAddr id_out,
                                                       GuestAddr cookie_out) {
    u64 id = 0, cookie = 0;
    {
        std::lock_guard<std::mutex> lock(m_com_mutex);
        auto identity = apartment_identity_locked(m);
        if (!identity) return CO_E_NOTINITIALIZED;
        id = identity->first;
        cookie = m_next_shutdown_cookie++;
        m_apartment_shutdowns.emplace(
            cookie, ApartmentShutdownRegistration{callback, id, identity->second});
    }
    (void)guest_iunknown_addref(m, callback);
    (void)m.write_u64(id_out, id);
    (void)m.write_ptr(cookie_out, cookie);
    return com::S_OK;
}

// HRESULT RoUnregisterForApartmentShutdown(COOKIE)
// Measured: an unknown cookie is E_INVALIDARG; from a thread outside any
// apartment CO_E_NOTINITIALIZED.
u32 ComCoreManager::ro_unregister_for_apartment_shutdown(Marshaler& m, GuestAddr cookie) {
    GuestAddr callback = 0;
    {
        std::lock_guard<std::mutex> lock(m_com_mutex);
        if (!apartment_identity_locked(m)) return CO_E_NOTINITIALIZED;
        auto it = m_apartment_shutdowns.find(cookie);
        if (cookie == 0 || it == m_apartment_shutdowns.end()) return com::E_INVALIDARG;
        callback = it->second.callback;
        m_apartment_shutdowns.erase(it);
    }
    guest_iunknown_release(m, callback);
    return com::S_OK;
}

void ComCoreManager::fire_apartment_shutdown(Marshaler& m, u64 apartment_id) {
    std::vector<GuestAddr> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_com_mutex);
        for (auto it = m_apartment_shutdowns.begin(); it != m_apartment_shutdowns.end();) {
            if (it->second.apartment_id == apartment_id) {
                callbacks.push_back(it->second.callback);
                it = m_apartment_shutdowns.erase(it);
            } else {
                ++it;
            }
        }
    }
    const bool is64 = m.cpu_is_64bit();
    for (const GuestAddr callback : callbacks) {
        const GuestAddr vtable = static_cast<GuestAddr>(m.read_ptr(callback).value_or(0));
        // IApartmentShutdown::OnUninitialize is slot 3; its UINT64 takes two
        // stack slots on x86.
        const GuestAddr method = vtable == 0 ? 0
            : static_cast<GuestAddr>(m.read_ptr(vtable + 3u * (is64 ? 8u : 4u)).value_or(0));
        if (method != 0) {
            if (is64) (void)call_guest_function(m, method, {callback, apartment_id});
            else (void)call_guest_function(m, method, {callback, apartment_id & 0xFFFFFFFFu,
                                                       apartment_id >> 32});
        }
        guest_iunknown_release(m, callback);
    }
}

bool ComCoreManager::thread_ole_initialized(Marshaler& m) {
    std::lock_guard<std::mutex> lock(m_com_mutex);
    auto it = m_apartments.find(guest_teb(m));
    return it != m_apartments.end() && it->second.ole_init_count > 0 &&
           it->second.init_count > 0;
}

void ComCoreManager::ole_uninitialize() {
    co_uninitialize();
}

void ComCoreManager::ole_uninitialize(Marshaler& m) {
    {
        std::lock_guard<std::mutex> lock(m_com_mutex);
        ApartmentState& state = m_apartments[guest_teb(m)];
        if (state.ole_init_count > 0) --state.ole_init_count;
    }
    co_uninitialize(m);
}

} // namespace vitreo::winapi
