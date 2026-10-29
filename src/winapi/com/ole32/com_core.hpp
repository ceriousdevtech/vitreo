#pragma once
/// @file com_core.hpp
/// @brief ole32 COM core stubs (WIN32-069)

#include <vitreo/common.hpp>
#include "ipc/com_class_registry.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "audio/contracts/audio_renderer.hpp"
#include "audio/contracts/audio_capture.hpp"
#include "winapi/com/ole32/audio_event_clock.hpp"

#include <array>
#include <deque>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <initializer_list>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo { class GuestMemory; }
namespace vitreo::capsule { class Registry; class Vfs; }
namespace vitreo::audio { class IMediaPlayer; }
namespace vitreo::windowing { class CocoaBridge; }

namespace vitreo::winapi {

class Marshaler;
class ComTaskMemoryManager;
class GlobalLocalManager;
class GuestElevationState;
class SyncManager;
class MessageSendManager;
class DirectInputManager;
namespace dshow { class DShowHost; }

// COM HRESULT constants
namespace com {
    constexpr u32 S_OK = 0;
    constexpr u32 S_FALSE = 1;
    constexpr u32 CLASS_E_CLASSNOTAVAILABLE = 0x80040111;
    constexpr u32 CLASS_E_NOAGGREGATION = 0x80040110;
    constexpr u32 RPC_E_CHANGED_MODE = 0x80010106;
    constexpr u32 E_INVALIDARG = 0x80070057;
    constexpr u32 E_NOINTERFACE = 0x80004002;
    constexpr u32 E_POINTER = 0x80004003;
    constexpr u32 E_NOTIMPL = 0x80004001;
    constexpr u32 CO_E_CLASSSTRING = 0x800401F3;
    constexpr u32 REGDB_E_CLASSNOTREG = 0x80040154;
    constexpr u32 CO_E_DLLNOTFOUND = 0x800401F8;
    constexpr u32 CO_E_ERRORINDLL = 0x800401F9;

    // COINIT flags
    constexpr u32 COINIT_MULTITHREADED = 0x0;
    constexpr u32 COINIT_APARTMENTTHREADED = 0x2;
    constexpr u32 COINIT_DISABLE_OLE1DDE = 0x4;
    constexpr u32 COINIT_SPEED_OVER_MEMORY = 0x8;
}

// =============================================================================
// GUID / CLSID
// =============================================================================

/// Windows GUID/CLSID/IID structure (16 bytes)
struct Guid {
    u32 Data1;
    u16 Data2;
    u16 Data3;
    u8  Data4[8];
};

static_assert(sizeof(Guid) == 16, "Guid must be 16 bytes (Windows ABI)");

class ModuleManager;
class ShellItemDialogs;

/// COM initialization manager
class WicContext;
[[nodiscard]] std::shared_ptr<WicContext> create_wic_context();

class ComCoreManager {
public:
    /// Guest-visible arena for built-in COM objects and vtables. GDI32 owns
    /// 0x5E100000..0x5ED00000 for DIB pixels; structured storage and CRT EH
    /// occupy the following ranges. This final 1 MiB ends exactly where
    /// shell32's PIDL allocator begins.
    static constexpr GuestAddr COM_SCRATCH_BASE = 0x5EF00000;
    static constexpr u32 COM_SCRATCH_SIZE = 0x100000;
    /// The shared-mode engine's (and render endpoint's) sample rate: the mix
    /// format's rate, and the unit of IAudioClock2 device positions.
    static constexpr u32 AUDIO_ENGINE_RATE = 48000u;
    /// IMMDevice::GetId of the render endpoint (the prefix of every audio
    /// session identifier).
    static constexpr char16_t AUDIO_RENDER_ENDPOINT_ID[] = u"Vitreo.CoreAudio.Render";

    struct ServerCallContext {
        bool active{false};
        bool caller_elevated{false};
        bool impersonating{false};
    };
    struct AutomationMethodLayout {
        u16 vtable_offset{0};
        u16 parameter_count{0};
        std::vector<u16> parameter_vts;
        std::vector<u16> parameter_flags;
        std::vector<Guid> parameter_iids;
        std::string name;
    };
    struct AutomationInterfaceLayout {
        std::vector<AutomationMethodLayout> methods;
    };

    ComCoreManager() = default;
    explicit ComCoreManager(ComTaskMemoryManager& task_memory) noexcept
        : m_guest_task_memory{&task_memory} {}

    ComCoreManager(const ComCoreManager&) = delete;
    ComCoreManager& operator=(const ComCoreManager&) = delete;
    ~ComCoreManager();

    /// CoInitializeEx. COM apartment state is per-thread on Windows, and the
    /// calling thread's apartment is published in its TEB (see
    /// publish_ole_tls); the Marshaler overloads are the guest-facing ones.
    /// The overloads without a Marshaler keep the host-only state for callers
    /// with no guest thread (tests, internal bring-up).
    [[nodiscard]] u32 co_initialize_ex(u32 coinit);
    [[nodiscard]] u32 co_initialize_ex(Marshaler& m, u32 coinit);

    /// RoInitialize — the Windows Runtime entry point onto the same per-thread
    /// COM apartment. RO_INIT_SINGLETHREADED enters an STA, RO_INIT_MULTITHREADED
    /// an MTA, and the initialize counts are shared with CoInitializeEx.
    [[nodiscard]] u32 ro_initialize(Marshaler& m, u32 init_type);

    /// CoInitialize (= CoInitializeEx with APARTMENTTHREADED)
    [[nodiscard]] u32 co_initialize();
    [[nodiscard]] u32 co_initialize(Marshaler& m);

    /// CoUninitialize
    void co_uninitialize();
    void co_uninitialize(Marshaler& m);

    /// CoIncrementMTAUsage / CoDecrementMTAUsage
    [[nodiscard]] u32 co_increment_mta_usage(GuestAddr* cookie);
    [[nodiscard]] u32 co_decrement_mta_usage(GuestAddr cookie);

    /// Out-of-process COM server lifecycle helpers.
    [[nodiscard]] u32 co_add_ref_server_process();
    [[nodiscard]] u32 co_release_server_process();
    [[nodiscard]] u32 co_resume_class_objects();
    [[nodiscard]] u32 co_suspend_class_objects();

    /// CoCreateInstance — returns CLASS_E_CLASSNOTAVAILABLE for unknown CLSIDs.
    [[nodiscard]] u32 co_create_instance() const;

    /// CoCreateInstance thunk path with guest marshaling support.
    [[nodiscard]] u32 co_create_instance(Marshaler& m);

    /// Bind the in-process DirectInput8 COM class provider. Kept out-of-line
    /// so COM owns activation while the media subsystem owns the interface.
    void set_direct_input_manager(DirectInputManager* manager) noexcept;

    /// IUnknown methods for a client-side proxy whose real object lives in a
    /// different Vitreo guest process.
    [[nodiscard]] u32 remote_proxy_query_interface(Marshaler& m);
    [[nodiscard]] u32 remote_proxy_add_ref(Marshaler& m);
    [[nodiscard]] u32 remote_proxy_release(Marshaler& m);
    [[nodiscard]] u32 remote_proxy_invoke(Marshaler& m);
    [[nodiscard]] u32 remote_dispatch_get_type_info_count(Marshaler& m);
    [[nodiscard]] u32 remote_dispatch_get_type_info(Marshaler& m);
    [[nodiscard]] u32 remote_dispatch_get_ids_of_names(Marshaler& m);
    [[nodiscard]] u32 remote_dispatch_invoke(Marshaler& m);
    /// Create a proxy in this process for an interface pointer owned by a
    /// peer process. Used by the process IPC endpoint when unmarshaling an
    /// Automation interface parameter.
    [[nodiscard]] GuestAddr create_marshaled_interface_proxy(
        Marshaler& m, u32 owner_pid, u32 owner_pointer_size,
        GuestAddr remote_object, const Guid& iid);

    /// CoLoadLibrary(lpszLibName, bAutoFree) — returns the HINSTANCE, or 0.
    [[nodiscard]] GuestAddr co_load_library(Marshaler& m);

    /// CoFreeLibrary(hInst) — releases a reference taken by CoLoadLibrary.
    void co_free_library(Marshaler& m);

    void set_thunk_registry(ThunkRegistry* tr) noexcept { m_thunks = tr; }
    [[nodiscard]] ThunkRegistry* thunk_registry() const noexcept { return m_thunks; }
    [[nodiscard]] SyncManager* sync_manager() const noexcept { return m_sync; }
    [[nodiscard]] audio::IAudioRenderer* audio_renderer() const noexcept { return m_audio_renderer; }
    /// Host-implemented DirectShow objects (dshow_host.hpp), created on first use.
    [[nodiscard]] dshow::DShowHost& dshow_host();
    void set_scratch_base(GuestAddr base) noexcept;
    void set_audio_renderer(audio::IAudioRenderer* renderer) noexcept {
        m_audio_renderer = renderer;
        m_audio_endpoint_available = renderer != nullptr;
    }
    /// The host microphone backend behind every capture endpoint (eCapture).
    void set_audio_capture(audio::IAudioCapture* capture) noexcept {
        m_audio_capture = capture;
    }
    void set_vfs(capsule::Vfs* vfs) noexcept { m_vfs = vfs; }
    [[nodiscard]] capsule::Vfs* vfs() const noexcept { return m_vfs; }

    /// Lay out a built-in in-proc COM object in the COM scratch region:
    /// `{vtable*}{vtable of ole32 thunk addresses}`, pointer-width correct.
    /// Returns 0 when a named thunk is missing or the region is exhausted.
    /// The object address is the identity every method thunk is keyed on.
    [[nodiscard]] GuestAddr allocate_builtin_object(
        Marshaler& m, std::span<const std::string_view> method_names);
    void set_cocoa_bridge(windowing::CocoaBridge* bridge) noexcept {
        m_cocoa_bridge = bridge;
    }
    [[nodiscard]] windowing::CocoaBridge* cocoa_bridge() const noexcept {
        return m_cocoa_bridge;
    }
    /// The Common Item Dialog and shell items (shell_item_dialogs.cpp).
    [[nodiscard]] ShellItemDialogs& item_dialogs();
    void set_sync_manager(SyncManager* sync) noexcept { m_sync = sync; }
    void set_message_send(MessageSendManager* messages) noexcept {
        m_message_send = messages;
    }
    void set_registry(capsule::Registry* registry) noexcept {
        m_registry = registry;
    }
    void set_capsule_root(std::string capsule_root) {
        m_capsule_root = std::move(capsule_root);
    }
    /// CoLoadLibrary/CoFreeLibrary are LoadLibrary/FreeLibrary with COM
    /// bookkeeping, so they load through the one real module manager rather
    /// than a second, divergent view of what is loaded.
    void set_module_manager(ModuleManager* modules) noexcept { m_modules = modules; }

    [[nodiscard]] const std::shared_ptr<WicContext>& wic_context() const { return m_wic_context; }
    // Guards retained WIC guest interfaces against process-runtime teardown.
    [[nodiscard]] std::weak_ptr<void> wic_lifetime() const { return m_wic_context; }

    /// Guest vtable calls for CoMarshalInterThreadInterfaceInStream /
    /// CoGetInterfaceAndReleaseStream (AddRef / QI / Release).
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }
    /// The injected guest caller; empty until the run loop installs it.
    [[nodiscard]] const GuestCall& guest_call() const noexcept {
        return m_guest_call;
    }
    /// Call vtable slot @p slot of the guest COM object @p punk (args[0] is
    /// the interface pointer) through the injected guest caller. E_FAIL when
    /// no caller is configured or the vtable cannot be read. For components
    /// outside ole32 that hold guest interfaces (Media Foundation attribute
    /// stores) and must AddRef/Release/QueryInterface them honestly.
    [[nodiscard]] u32 call_guest_slot(Marshaler& m, GuestAddr punk, u32 slot,
                                      const u64* args, usize count) {
        return guest_vtable_call(m, punk, slot, args, count);
    }
    using AudioActivationThread = std::function<bool(GuestAddr, GuestAddr)>;
    void set_audio_activation_thread(AudioActivationThread launch) {
        m_audio_activation_thread = std::move(launch);
    }
    [[nodiscard]] u32 activate_audio_interface_async(Marshaler& m);
    [[nodiscard]] u32 audio_activation_worker(Marshaler& m);
    [[nodiscard]] u32 audio_activation_query_interface(Marshaler& m);
    [[nodiscard]] u32 audio_activation_add_ref(Marshaler& m);
    [[nodiscard]] u32 audio_activation_release(Marshaler& m);
    [[nodiscard]] u32 audio_activation_get_result(Marshaler& m);


    /// Call IUnknown::AddRef / Release on an arbitrary guest interface pointer.
    /// Composition roots may wire these into frontends (e.g. D3D9 private data)
    /// without those frontends depending on ole32 internals.
    [[nodiscard]] u32 guest_iunknown_addref(Marshaler& m, GuestAddr punk);
    void guest_iunknown_release(Marshaler& m, GuestAddr punk);
    using LocalServiceActivator = std::function<bool(
        const std::wstring&, const std::wstring&, u32&)>;
    void set_local_service_activator(LocalServiceActivator activator) {
        m_local_service_activator = std::move(activator);
    }
    using LocalServerActivator = std::function<bool(
        const std::wstring&, const std::wstring&, u32&)>;
    void set_local_server_activator(LocalServerActivator activator) {
        m_local_server_activator = std::move(activator);
    }
    void set_local_server_registration_timeout(
        std::chrono::milliseconds timeout) noexcept {
        m_local_server_registration_timeout = timeout;
    }
    using AutomationLayoutResolver = std::function<
        std::optional<AutomationInterfaceLayout>(const Guid&)>;
    void set_automation_layout_resolver(AutomationLayoutResolver resolver) {
        m_automation_layout_resolver = std::move(resolver);
    }
    /// Services a CLSID owned by another component (currently MSXML's DOM).
    /// Returns the HRESULT when the CLSID is claimed, or nullopt to let
    /// CoCreateInstance continue its normal search.
    using ExternalClassFactory = std::function<
        std::optional<u32>(Marshaler&, const Guid& clsid, const Guid& iid,
                           GuestAddr ppv)>;
    void set_external_class_factory(ExternalClassFactory factory) {
        m_external_class_factory = std::move(factory);
    }

    using AutomationBstrAllocator = std::function<GuestAddr(
        Marshaler&, std::span<const Byte>)>;
    void set_automation_bstr_allocator(AutomationBstrAllocator allocator) {
        m_automation_bstr_allocator = std::move(allocator);
    }
    void set_elevation_context(const GuestElevationState* elevation) noexcept {
        m_elevation = elevation;
    }
    using CallerImpersonation = std::function<bool(bool)>;
    using CallerRevert = std::function<void()>;
    void set_caller_impersonation(CallerImpersonation impersonate,
                                  CallerRevert revert) {
        m_caller_impersonation = std::move(impersonate);
        m_caller_revert = std::move(revert);
    }
    [[nodiscard]] ServerCallContext push_server_call_context(
        bool caller_elevated) noexcept;
    void pop_server_call_context(ServerCallContext previous) noexcept;

    [[nodiscard]] u32 server_security_query_interface(Marshaler& m);
    [[nodiscard]] u32 server_security_add_ref(Marshaler& m);
    [[nodiscard]] u32 server_security_release(Marshaler& m);
    [[nodiscard]] u32 server_security_query_blanket(Marshaler& m);
    [[nodiscard]] u32 server_security_impersonate_client(Marshaler& m);
    [[nodiscard]] u32 server_security_revert_to_self(Marshaler& m);
    [[nodiscard]] u32 server_security_is_impersonating(Marshaler& m);
    [[nodiscard]] u32 co_get_call_context(Marshaler& m);

    [[nodiscard]] u32 shelllink_query_interface(Marshaler& m);
    [[nodiscard]] u32 shelllink_add_ref(Marshaler& m);
    [[nodiscard]] u32 shelllink_release(Marshaler& m);
    [[nodiscard]] u32 shelllink_set_path(Marshaler& m);
    [[nodiscard]] u32 shelllink_get_path(Marshaler& m);
    [[nodiscard]] u32 shelllink_set_arguments(Marshaler& m);
    [[nodiscard]] u32 shelllink_set_working_directory(Marshaler& m);
    [[nodiscard]] u32 shelllink_set_description(Marshaler& m);
    [[nodiscard]] u32 shelllink_set_icon_location(Marshaler& m);
    [[nodiscard]] u32 shelllink_set_show_cmd(Marshaler& m);
    [[nodiscard]] u32 shelllink_persist_save(Marshaler& m);
    [[nodiscard]] u32 shelllink_persist_load(Marshaler& m);
    [[nodiscard]] u32 shelllink_persist_is_dirty(Marshaler& m);
    [[nodiscard]] u32 shelllink_persist_get_class_id(Marshaler& m);
    [[nodiscard]] u32 shelllink_persist_save_completed(Marshaler& m);
    [[nodiscard]] u32 shelllink_persist_get_cur_file(Marshaler& m);
    [[nodiscard]] u32 shelllink_propstore_get_count(Marshaler& m);
    [[nodiscard]] u32 shelllink_propstore_get_at(Marshaler& m);
    [[nodiscard]] u32 shelllink_propstore_get_value(Marshaler& m);
    [[nodiscard]] u32 shelllink_propstore_set_value(Marshaler& m);
    [[nodiscard]] u32 shelllink_propstore_commit(Marshaler& m);

    // SHGetPropertyStoreForWindow (shellapi.h): an IPropertyStore whose
    // values live on the window (com_window_property_store.cpp).
    [[nodiscard]] u32 create_window_property_store(Marshaler& m);
    [[nodiscard]] u32 window_propstore_query_interface(Marshaler& m);
    [[nodiscard]] u32 window_propstore_add_ref(Marshaler& m);
    [[nodiscard]] u32 window_propstore_release(Marshaler& m);
    [[nodiscard]] u32 window_propstore_get_count(Marshaler& m);
    [[nodiscard]] u32 window_propstore_get_at(Marshaler& m);
    [[nodiscard]] u32 window_propstore_get_value(Marshaler& m);
    [[nodiscard]] u32 window_propstore_set_value(Marshaler& m);
    [[nodiscard]] u32 window_propstore_commit(Marshaler& m);
    /// IsWindow for the window store's writes, which fail once the window is
    /// gone. Without it every write fails.
    void set_window_exists(std::function<bool(u32)> exists) {
        m_window_exists = std::move(exists);
    }

    // IGlobalOptions (IUnknown + Set + Query).
    [[nodiscard]] u32 global_options_query_interface(Marshaler& m);
    [[nodiscard]] u32 global_options_add_ref(Marshaler& m);
    [[nodiscard]] u32 global_options_release(Marshaler& m);
    [[nodiscard]] u32 global_options_set(Marshaler& m);
    [[nodiscard]] u32 global_options_query(Marshaler& m);

    // INetworkListManager — network connectivity queries.
    [[nodiscard]] u32 network_list_manager_query_interface(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_add_ref(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_release(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_get_network_connections(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_get_networks(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_get_is_connected_to_internet(
        Marshaler& m);
    [[nodiscard]] u32 network_list_manager_get_is_connected(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_get_connectivity(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_cpc_query_interface(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_cpc_find_connection_point(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_cp_query_interface(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_cp_get_container(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_cp_advise(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_cp_unadvise(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_enum_query_interface(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_enum_add_ref(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_enum_release(Marshaler& m);
    [[nodiscard]] GuestAddr create_network_object(Marshaler& m, const char* const* names,
                                                usize slot_count);
    [[nodiscard]] u32 network_list_manager_enum_next(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_enum_skip(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_enum_reset(Marshaler& m);
    [[nodiscard]] u32 network_list_manager_enum_clone(Marshaler& m);
    [[nodiscard]] u32 network_connection_query_interface(Marshaler& m);
    [[nodiscard]] u32 network_connection_add_ref(Marshaler& m);
    [[nodiscard]] u32 network_connection_release(Marshaler& m);
    [[nodiscard]] u32 network_connection_get_network(Marshaler& m);
    [[nodiscard]] u32 network_connection_get_is_connected_to_internet(Marshaler& m);
    [[nodiscard]] u32 network_connection_get_is_connected(Marshaler& m);
    [[nodiscard]] u32 network_connection_get_connectivity(Marshaler& m);
    [[nodiscard]] u32 network_connection_get_connection_id(Marshaler& m);
    [[nodiscard]] u32 network_connection_get_adapter_id(Marshaler& m);
    [[nodiscard]] u32 network_connection_get_domain_type(Marshaler& m);
    [[nodiscard]] u32 network_query_interface(Marshaler& m);
    [[nodiscard]] u32 network_add_ref(Marshaler& m);
    [[nodiscard]] u32 network_release(Marshaler& m);
    [[nodiscard]] u32 network_get_connectivity(Marshaler& m);
    [[nodiscard]] u32 network_get_is_connected(Marshaler& m);
    [[nodiscard]] u32 network_get_is_connected_to_internet(Marshaler& m);
    [[nodiscard]] u32 network_get_category(Marshaler& m);
    [[nodiscard]] u32 network_get_domain_type(Marshaler& m);
    [[nodiscard]] u32 network_get_network_id(Marshaler& m);

    // MMDevice API. The enumerator itself is always available on Windows;
    // endpoint absence is represented by an empty collection / E_NOTFOUND,
    // not CLASS_E_CLASSNOTAVAILABLE from CoCreateInstance.
    [[nodiscard]] u32 mmdevice_enumerator_query_interface(Marshaler& m);
    [[nodiscard]] u32 mmdevice_enumerator_add_ref(Marshaler& m);
    [[nodiscard]] u32 mmdevice_enumerator_release(Marshaler& m);
    [[nodiscard]] u32 mmdevice_enumerator_enum_audio_endpoints(Marshaler& m);
    [[nodiscard]] u32 mmdevice_enumerator_get_default_audio_endpoint(
        Marshaler& m);
    [[nodiscard]] u32 mmdevice_enumerator_get_device(Marshaler& m);
    [[nodiscard]] u32 mmdevice_enumerator_register_notification(Marshaler& m);
    [[nodiscard]] u32 mmdevice_enumerator_unregister_notification(Marshaler& m);
    [[nodiscard]] u32 mmdevice_collection_query_interface(Marshaler& m);
    [[nodiscard]] u32 mmdevice_collection_add_ref(Marshaler& m);
    [[nodiscard]] u32 mmdevice_collection_release(Marshaler& m);
    [[nodiscard]] u32 mmdevice_collection_get_count(Marshaler& m);
    [[nodiscard]] u32 mmdevice_collection_item(Marshaler& m);
    [[nodiscard]] u32 mmdevice_query_interface(Marshaler& m);
    [[nodiscard]] u32 mmdevice_add_ref(Marshaler& m);
    [[nodiscard]] u32 mmdevice_release(Marshaler& m);
    [[nodiscard]] u32 mmdevice_activate(Marshaler& m);
    [[nodiscard]] u32 mmdevice_open_property_store(Marshaler& m);
    [[nodiscard]] u32 mmdevice_get_id(Marshaler& m);
    [[nodiscard]] u32 mmdevice_get_state(Marshaler& m);
    /// One audio endpoint: the host render device, or a host input device.
    struct AudioEndpointRef {
        bool capture{false};
        std::string uid;  ///< host device uid (capture only)
    };
    [[nodiscard]] GuestAddr create_mmdevice_endpoint(Marshaler& m,
                                                     const AudioEndpointRef& endpoint);
    /// The active endpoints of a data flow (0 render, 1 capture, 2 all), in
    /// IMMDeviceCollection order.
    [[nodiscard]] std::vector<AudioEndpointRef> audio_endpoints(u32 data_flow) const;
    /// The capture endpoint whose MMDevice id is `id` (case-insensitive).
    [[nodiscard]] std::optional<AudioEndpointRef> capture_endpoint_by_id(
        std::string_view id) const;
    [[nodiscard]] u32 mmendpoint_query_interface(Marshaler& m);
    [[nodiscard]] u32 mmendpoint_add_ref(Marshaler& m);
    [[nodiscard]] u32 mmendpoint_release(Marshaler& m);
    [[nodiscard]] u32 mmendpoint_get_data_flow(Marshaler& m);
    [[nodiscard]] GuestAddr create_mmendpoint(Marshaler& m, bool capture);
    [[nodiscard]] u32 property_store_query_interface(Marshaler& m);
    [[nodiscard]] u32 property_store_add_ref(Marshaler& m);
    [[nodiscard]] u32 property_store_release(Marshaler& m);
    [[nodiscard]] u32 property_store_get_count(Marshaler& m);
    [[nodiscard]] u32 property_store_get_at(Marshaler& m);
    [[nodiscard]] u32 property_store_get_value(Marshaler& m);
    [[nodiscard]] u32 property_store_set_value(Marshaler& m);
    [[nodiscard]] u32 property_store_commit(Marshaler& m);
    [[nodiscard]] GuestAddr create_audio_property_store(Marshaler& m,
                                                        const AudioEndpointRef& endpoint);
    /// A new IAudioClient on `endpoint`, or on the render endpoint.
    [[nodiscard]] GuestAddr create_audio_client(Marshaler& m,
                                              const AudioEndpointRef& endpoint);
    [[nodiscard]] GuestAddr create_audio_client(Marshaler& m);
    /// A zero-duration shared-mode capture stream's endpoint buffer at client
    /// rate `rate` on the 48 kHz capture engine (AUTOCONVERTPCM when the rate
    /// differs), the measured rule IAudioClient::Initialize applies.
    [[nodiscard]] static u32 capture_minimum_buffer(u32 rate);
    [[nodiscard]] u32 audio_client_query_interface(Marshaler& m);
    [[nodiscard]] u32 audio_client_add_ref(Marshaler& m);
    [[nodiscard]] u32 audio_client_release(Marshaler& m);
    [[nodiscard]] u32 audio_client_initialize(Marshaler& m);
    /// The host device of a capture IAudioClient (empty: not capture, or gone).
    [[nodiscard]] std::optional<audio::CaptureDevice> capture_device_of(GuestAddr client);
    [[nodiscard]] u32 audio_client_get_buffer_size(Marshaler& m);
    [[nodiscard]] u32 audio_client_get_stream_latency(Marshaler& m);
    [[nodiscard]] u32 audio_client_get_current_padding(Marshaler& m);
    [[nodiscard]] u32 audio_client_is_format_supported(Marshaler& m);
    [[nodiscard]] u32 audio_client_get_mix_format(Marshaler& m);
    [[nodiscard]] u32 audio_client_get_device_period(Marshaler& m);
    [[nodiscard]] u32 audio_client_start(Marshaler& m);
    [[nodiscard]] u32 audio_client_stop(Marshaler& m);
    [[nodiscard]] u32 audio_client_reset(Marshaler& m);
    [[nodiscard]] u32 audio_client_set_event_handle(Marshaler& m);
    [[nodiscard]] u32 audio_client_get_service(Marshaler& m);
    [[nodiscard]] GuestAddr create_audio_render_client(Marshaler& m,
                                                        GuestAddr client);
    [[nodiscard]] u32 audio_render_client_query_interface(Marshaler& m);
    [[nodiscard]] u32 audio_render_client_add_ref(Marshaler& m);
    [[nodiscard]] u32 audio_render_client_release(Marshaler& m);
    [[nodiscard]] u32 audio_render_client_get_buffer(Marshaler& m);
    [[nodiscard]] u32 audio_render_client_release_buffer(Marshaler& m);
    [[nodiscard]] GuestAddr create_audio_capture_client(Marshaler& m, GuestAddr client);
    [[nodiscard]] u32 audio_capture_client_query_interface(Marshaler& m);
    [[nodiscard]] u32 audio_capture_client_add_ref(Marshaler& m);
    [[nodiscard]] u32 audio_capture_client_release(Marshaler& m);
    [[nodiscard]] u32 audio_capture_client_get_buffer(Marshaler& m);
    [[nodiscard]] u32 audio_capture_client_release_buffer(Marshaler& m);
    [[nodiscard]] u32 audio_capture_client_get_next_packet_size(Marshaler& m);

    // IAudioClient::GetService objects (wasapi_services.cpp). Every facet of a
    // service object shares QueryInterface / AddRef / Release.
    [[nodiscard]] u32 audio_service_query_interface(Marshaler& m);
    [[nodiscard]] u32 audio_service_add_ref(Marshaler& m);
    [[nodiscard]] u32 audio_service_release(Marshaler& m);
    [[nodiscard]] u32 audio_clock_get_frequency(Marshaler& m);
    [[nodiscard]] u32 audio_clock_get_position(Marshaler& m);
    [[nodiscard]] u32 audio_clock_get_characteristics(Marshaler& m);
    [[nodiscard]] u32 audio_clock2_get_device_position(Marshaler& m);
    [[nodiscard]] u32 audio_clock_adjustment_set_sample_rate(Marshaler& m);
    [[nodiscard]] u32 audio_session_get_state(Marshaler& m);
    [[nodiscard]] u32 audio_session_get_display_name(Marshaler& m);
    [[nodiscard]] u32 audio_session_set_display_name(Marshaler& m);
    [[nodiscard]] u32 audio_session_get_icon_path(Marshaler& m);
    [[nodiscard]] u32 audio_session_set_icon_path(Marshaler& m);
    [[nodiscard]] u32 audio_session_get_grouping_param(Marshaler& m);
    [[nodiscard]] u32 audio_session_set_grouping_param(Marshaler& m);
    [[nodiscard]] u32 audio_session_register_notification(Marshaler& m);
    [[nodiscard]] u32 audio_session_unregister_notification(Marshaler& m);
    [[nodiscard]] u32 audio_session_get_session_identifier(Marshaler& m);
    [[nodiscard]] u32 audio_session_get_session_instance_identifier(Marshaler& m);
    [[nodiscard]] u32 audio_session_get_process_id(Marshaler& m);
    [[nodiscard]] u32 audio_session_is_system_sounds_session(Marshaler& m);
    [[nodiscard]] u32 audio_session_set_ducking_preference(Marshaler& m);
    [[nodiscard]] u32 simple_audio_volume_set_master_volume(Marshaler& m);
    [[nodiscard]] u32 simple_audio_volume_get_master_volume(Marshaler& m);
    [[nodiscard]] u32 simple_audio_volume_set_mute(Marshaler& m);
    [[nodiscard]] u32 simple_audio_volume_get_mute(Marshaler& m);
    [[nodiscard]] u32 channel_audio_volume_get_channel_count(Marshaler& m);
    [[nodiscard]] u32 channel_audio_volume_set_channel_volume(Marshaler& m);
    [[nodiscard]] u32 channel_audio_volume_get_channel_volume(Marshaler& m);
    [[nodiscard]] u32 channel_audio_volume_set_all_volumes(Marshaler& m);
    [[nodiscard]] u32 channel_audio_volume_get_all_volumes(Marshaler& m);
    [[nodiscard]] u32 stream_audio_volume_get_channel_count(Marshaler& m);
    [[nodiscard]] u32 stream_audio_volume_set_channel_volume(Marshaler& m);
    [[nodiscard]] u32 stream_audio_volume_get_channel_volume(Marshaler& m);
    [[nodiscard]] u32 stream_audio_volume_set_all_volumes(Marshaler& m);
    [[nodiscard]] u32 stream_audio_volume_get_all_volumes(Marshaler& m);
    /// Guest thread entry that delivers queued IAudioSessionEvents callbacks.
    [[nodiscard]] u32 audio_session_event_worker(Marshaler& m);

    /// CoGetObjectContext — the calling thread's COM context object.
    /// Windows answers IID_IComThreadingInfo, IID_IContextCallback and
    /// IID_IObjContext here; Vitreo implements the threading-info view, whose
    /// answers are exactly what its per-thread apartment state already knows.
    [[nodiscard]] u32 co_get_object_context(Marshaler& m);
    /// CoGetApartmentType — the calling thread's apartment type/qualifier.
    [[nodiscard]] u32 co_get_apartment_type(Marshaler& m);
    [[nodiscard]] u32 co_get_context_token(Marshaler& m);
    /// Build the per-thread COM context object. Caller holds m_com_mutex.
    [[nodiscard]] bool materialize_threading_info_locked(Marshaler& m,
                                                         GuestAddr teb);

    /// ole32 completions: free-threaded marshaler, agile references,
    /// PROPVARIANT copy and the HGLOBAL view of a memory stream.
    [[nodiscard]] u32 co_create_free_threaded_marshaler(Marshaler& m);
    [[nodiscard]] u32 ftm_query_interface(Marshaler& m);
    [[nodiscard]] u32 ftm_add_ref(Marshaler& m);
    [[nodiscard]] u32 ftm_release(Marshaler& m);
    /// The FTM's IMarshal (Learn CoCreateFreeThreadedMarshaler: the outer
    /// object delegates QueryInterface(IID_IMarshal) to the FTM). Its
    /// IUnknown delegates to the controlling unknown.
    [[nodiscard]] u32 ftm_marshal_method(Marshaler& m, u32 slot);
    [[nodiscard]] u32 ro_get_agile_reference(Marshaler& m);
    [[nodiscard]] u32 agile_query_interface(Marshaler& m);
    [[nodiscard]] u32 agile_add_ref(Marshaler& m);
    [[nodiscard]] u32 agile_release(Marshaler& m);
    [[nodiscard]] u32 agile_resolve(Marshaler& m);
    [[nodiscard]] u32 prop_variant_copy(Marshaler& m);
    /// HKCR\<ProgID>\CLSID lookup shared by CLSIDFromString/CLSIDFromProgID.
    [[nodiscard]] u32 clsid_from_progid_text(const std::u16string& progid, Guid& clsid);
    [[nodiscard]] u32 prop_variant_copy_at(Marshaler& m, GuestAddr dst, GuestAddr src);
    [[nodiscard]] u32 prop_variant_clear_at(Marshaler& m, GuestAddr pv);
    void set_global_local_manager(GlobalLocalManager* globals) noexcept {
        m_globals = globals;
    }
    /// Call an oleaut32 export (SysFreeString, SafeArrayDestroy, ...) the
    /// way ole32 does on Windows: through the guest-visible function.
    u64 call_oleaut32(Marshaler& m, const char* name, std::initializer_list<u64> args);
    /// Call any guest-visible export (user32/gdi32/kernel32/...) through the
    /// guest-call path; 0 when it is not registered.
    u64 call_guest_export(Marshaler& m, const char* dll, const char* name,
                          std::initializer_list<u64> args);
    /// OLE-level init (OleInitialize) on the calling thread; RegisterDragDrop
    /// needs it, not merely CoInitialize.
    [[nodiscard]] bool thread_ole_initialized(Marshaler& m);
    /// Call a guest function pointer (vtable slot) with native-width args.
    u64 call_guest_function(Marshaler& m, GuestAddr fn, std::initializer_list<u64> args);

    // WinRT apartment identity / shutdown notification (roapi.h), hosted in
    // combase on Windows but driven by this COM apartment state.
    [[nodiscard]] u32 ro_get_apartment_identifier(Marshaler& m, GuestAddr out);
    [[nodiscard]] u32 ro_register_for_apartment_shutdown(Marshaler& m, GuestAddr callback,
                                                         GuestAddr id_out,
                                                         GuestAddr cookie_out);
    [[nodiscard]] u32 ro_unregister_for_apartment_shutdown(Marshaler& m, GuestAddr cookie);
    [[nodiscard]] u32 get_hglobal_from_stream(Marshaler& m);
    [[nodiscard]] u32 com_threading_info_query_interface(Marshaler& m);
    [[nodiscard]] u32 com_threading_info_add_ref(Marshaler& m);
    [[nodiscard]] u32 com_threading_info_release(Marshaler& m);
    [[nodiscard]] u32 com_threading_info_get_apartment_type(Marshaler& m);
    [[nodiscard]] u32 com_threading_info_get_thread_type(Marshaler& m);
    [[nodiscard]] u32 com_threading_info_get_logical_thread_id(Marshaler& m);
    [[nodiscard]] u32 com_threading_info_set_logical_thread_id(Marshaler& m);

    // DirectShow Filter Graph (minimal stub — no real decode/render).
    [[nodiscard]] u32 filter_graph_query_interface(Marshaler& m);
    [[nodiscard]] u32 filter_graph_add_ref(Marshaler& m);
    [[nodiscard]] u32 filter_graph_release(Marshaler& m);
    [[nodiscard]] u32 filter_graph_add_filter(Marshaler& m);
    [[nodiscard]] u32 filter_graph_remove_filter(Marshaler& m);
    [[nodiscard]] u32 filter_graph_enum_filters(Marshaler& m);
    [[nodiscard]] u32 filter_graph_find_filter_by_name(Marshaler& m);
    /// IEnumFilters method `op` is the strmif.h vtable index (0 = QI).
    [[nodiscard]] u32 filter_graph_enum_filters_method(Marshaler& m, u32 op);
    /// Body of a graph's guest streaming thread (the source side of the
    /// graph: delivers decoded frames to a guest renderer's IMemInputPin).
    [[nodiscard]] u32 filter_graph_stream_thread(Marshaler& m);
    /// Body of the guest worker that completes a Run issued while the
    /// renderers were still cueing (measured: IMediaControl::Run returns
    /// S_FALSE and the filters' Run arrives later on another thread).
    [[nodiscard]] u32 filter_graph_run_worker(Marshaler& m);
    [[nodiscard]] u32 filter_graph_render_file(Marshaler& m);
    [[nodiscard]] u32 filter_graph_output_pin(Marshaler& m, u32 op);
    [[nodiscard]] u32 filter_graph_media_sample(Marshaler& m, u32 op);
    [[nodiscard]] u32 filter_graph_media_control_run(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_control_pause(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_control_stop(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_control_get_state(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_event_get_event(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_event_get_event_handle(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_event_wait_for_completion(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_event_set_notify_window(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_event_set_notify_flags(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_event_get_notify_flags(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_event_sink_notify(Marshaler& m);
    /// IMediaSeeking slot @p slot on a graph built from its filters
    /// (filter_graph_seeking.cpp); false when the graph is not one.
    bool intelligent_seeking(Marshaler& m, u32 slot, u32& result);
    /// A host filter's IMediaEventSink::Notify (no guest crossing).
    void filter_graph_post_event(GuestAddr graph_iface, u32 code, u64 p1, u64 p2);
    /// IUnknown slots 0-2 on a Filter Graph Manager interface from host
    /// code; false when @p pointer is not one.
    bool filter_graph_iunknown(Marshaler& m, GuestAddr pointer, u32 slot,
                               std::span<const u64> args, u32& result);
    [[nodiscard]] u32 filter_graph_connect_direct(Marshaler& m);
    [[nodiscard]] u32 filter_graph_disconnect(Marshaler& m);
    [[nodiscard]] u32 filter_graph_connect(Marshaler& m);
    [[nodiscard]] u32 filter_graph_render(Marshaler& m);
    [[nodiscard]] u32 filter_graph_add_source_filter(Marshaler& m);
    [[nodiscard]] u32 filter_graph_set_default_sync_source(Marshaler& m);
    /// CoCreateInstance for quartz's stock objects implemented on the host;
    /// @p claimed is false for any other CLSID.
    u32 dshow_co_create_instance(Marshaler& m, const Guid& clsid, const Guid& iid,
                                 GuestAddr outer, GuestAddr ppv, bool& claimed);
    /// IMediaEvent::Cancel/RestoreDefaultHandling(long lEvCode).
    [[nodiscard]] u32 filter_graph_media_event_default_handling(Marshaler& m, bool restore);
    [[nodiscard]] u32 filter_graph_basic_audio_get_volume(Marshaler& m);
    [[nodiscard]] u32 filter_graph_basic_audio_put_volume(Marshaler& m);
    [[nodiscard]] u32 filter_graph_basic_audio_get_balance(Marshaler& m);
    [[nodiscard]] u32 filter_graph_basic_audio_put_balance(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_seeking_set_positions(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_seeking_get_duration(Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_seeking_get_current_position(
        Marshaler& m);
    [[nodiscard]] u32 filter_graph_media_seeking_get_stop_position(
        Marshaler& m);
    /// IVideoWindow method `op` is the SDK vtable index after IDispatch.
    [[nodiscard]] u32 filter_graph_video_window(Marshaler& m, u32 op);
    /// IBasicVideo method `op` is the SDK vtable index after IDispatch.
    [[nodiscard]] u32 filter_graph_basic_video(Marshaler& m, u32 op);
    /// IMediaPosition method `op` is the SDK vtable index after IDispatch.
    [[nodiscard]] u32 filter_graph_media_position(Marshaler& m, u32 op);

    /// CoTaskMemAlloc — allocate in guest address space
    /// True when the calling thread is in an apartment, counting the
    /// implicit MTA (any thread holding the MTA puts uninitialized threads in
    /// it; Learn APTTYPEQUALIFIER_IMPLICIT_MTA).
    [[nodiscard]] bool thread_in_apartment(Marshaler& m);
    /// The process MTA exists: a thread holds it (CoInitializeEx MTA) or a
    /// CoIncrementMTAUsage cookie keeps it alive with no MTA thread running.
    /// Threads outside every apartment are then implicitly in it. Caller holds
    /// m_com_mutex.
    [[nodiscard]] bool process_mta_alive_locked() const;
    /// The audio engine's MTA: on Windows the engine runs on Media Foundation /
    /// real-time work queue threads, which are always MTA threads (Learn,
    /// "Media Foundation and COM"), so a process with a running audio client
    /// keeps the MTA alive for its lifetime. Vitreo's engine runs on host
    /// threads, so the first successful IAudioClient::Initialize takes one
    /// process-lifetime CoIncrementMTAUsage in their place.
    void hold_audio_engine_mta_usage();
    /// CoGetClassObject's in-process step: HKCR\CLSID\{clsid}\InprocServer32
    /// -> LoadLibrary -> DllGetClassObject(IClassFactory) -> CreateInstance.
    /// std::nullopt when no InprocServer32 is registered for the class.
    [[nodiscard]] std::optional<u32> activate_inproc_server(
        Marshaler& m, const Guid& clsid, GuestAddr rclsid_ptr, GuestAddr outer,
        GuestAddr riid_ptr, GuestAddr ppv_ptr);
    [[nodiscard]] GuestAddr co_task_mem_alloc_guest(u64 size);

    /// CoTaskMemFree — release guest task memory.
    void co_task_mem_free_guest(GuestAddr address);

    /// CoTaskMemRealloc — resize guest task memory.
    [[nodiscard]] GuestAddr co_task_mem_realloc_guest(
        GuestAddr address, u64 size);

    /// CoGetMalloc — return the process IMalloc (task allocator).
    void impl_CoGetMalloc(Marshaler& m);

    /// CoRegisterInitializeSpy / CoRevokeInitializeSpy — apartment init spies.
    void impl_CoRegisterInitializeSpy(Marshaler& m);
    void impl_CoRevokeInitializeSpy(Marshaler& m);
    void imalloc_query_interface(Marshaler& m);
    void imalloc_add_ref(Marshaler& m);
    void imalloc_release(Marshaler& m);
    void imalloc_alloc(Marshaler& m);
    void imalloc_realloc(Marshaler& m);
    void imalloc_free(Marshaler& m);
    void imalloc_get_size(Marshaler& m);
    void imalloc_did_alloc(Marshaler& m);
    void imalloc_heap_minimize(Marshaler& m);

    /// CoTaskMemAlloc (legacy host-side — kept for compatibility)
    [[nodiscard]] void* co_task_mem_alloc(usize size);

    /// CoTaskMemFree
    void co_task_mem_free(void* ptr);

    /// CoTaskMemRealloc
    [[nodiscard]] void* co_task_mem_realloc(void* ptr, usize size);

    /// OleInitialize — delegates to co_initialize_ex(COINIT_APARTMENTTHREADED)
    [[nodiscard]] u32 ole_initialize();
    [[nodiscard]] u32 ole_initialize(Marshaler& m);

    /// OleUninitialize — delegates to co_uninitialize()
    void ole_uninitialize();
    void ole_uninitialize(Marshaler& m);

    // --- Additional OLE32 stubs ---
    void impl_IsAccelerator(Marshaler& m);
    void impl_OleTranslateAccelerator(Marshaler& m);
    void impl_OleDestroyMenuDescriptor(Marshaler& m);
    void impl_OleCreateMenuDescriptor(Marshaler& m);
    void impl_OleLockRunning(Marshaler& m);
    /// com_wait_run.cpp: HRESULT CoWaitForMultipleHandles(DWORD, DWORD,
    /// ULONG, LPHANDLE, LPDWORD) and HRESULT OleRun(LPUNKNOWN).
    void impl_CoWaitForMultipleHandles(Marshaler& m);
    void impl_OleRun(Marshaler& m);
    /// Per-thread guest scratch for out-parameters of ole32's own guest calls.
    [[nodiscard]] GuestAddr wait_run_scratch(Marshaler& m);
    /// Call one of Vitreo's exports (@p dll / @p name) as the guest would;
    /// @p ok is false when the export or the guest caller is missing.
    u32 call_export(Marshaler& m, const char* dll, const char* name,
                    std::initializer_list<u64> args, bool& ok);
    void impl_OleGetClipboard(Marshaler& m);
    void impl_OleSetClipboard(Marshaler& m);
    void impl_OleIsCurrentClipboard(Marshaler& m);
    void impl_OleFlushClipboard(Marshaler& m);
    void impl_RevokeDragDrop(Marshaler& m);
    void impl_CoLockObjectExternal(Marshaler& m);
    void impl_RegisterDragDrop(Marshaler& m);
    void impl_DoDragDrop(Marshaler& m);
    void impl_CreateStreamOnHGlobal(Marshaler& m);
    void impl_WriteClassStm(Marshaler& m);
    void impl_OleSaveToStream(Marshaler& m);
    void impl_OleLoadFromStream(Marshaler& m);
    void impl_GetRunningObjectTable(Marshaler& m);
    void running_object_table_query_interface(Marshaler& m);
    void running_object_table_add_ref(Marshaler& m);
    void running_object_table_release(Marshaler& m);
    void running_object_table_register(Marshaler& m);
    void running_object_table_revoke(Marshaler& m);
    void running_object_table_is_running(Marshaler& m);
    void running_object_table_get_object(Marshaler& m);
    void running_object_table_note_change_time(Marshaler& m);
    void running_object_table_get_time_of_last_change(Marshaler& m);
    void running_object_table_enum_running(Marshaler& m);
    void impl_CreateItemMoniker(Marshaler& m);
    void item_moniker_method(Marshaler& m, u32 slot);
    void impl_ProgIDFromCLSID(Marshaler& m);
    void impl_CLSIDFromProgID(Marshaler& m);
    void impl_CoRegisterClassObject(Marshaler& m);
    void impl_CoRevokeClassObject(Marshaler& m);
    void impl_CoRegisterPSClsid(Marshaler& m);
    void impl_CoGetPSClsid(Marshaler& m);
    void impl_CoMarshalInterThreadInterfaceInStream(Marshaler& m);
    void impl_CoGetInterfaceAndReleaseStream(Marshaler& m);
    void impl_CoReleaseMarshalData(Marshaler& m);
    void impl_OleDuplicateData(Marshaler& m);
    void impl_ReleaseStgMedium(Marshaler& m);
    void impl_CoInitializeSecurity(Marshaler& m);
    void impl_CoSetProxyBlanket(Marshaler& m);

    /// Create the guest-visible IStream used by SHCreateMemStream.
    /// Returns NULL when the initial buffer is invalid or allocation fails.
    [[nodiscard]] GuestAddr create_memory_stream(
        Marshaler& m, GuestAddr initial_data, u32 initial_size);

    // Guest IStream vtable entry points.
    void memory_stream_query_interface(Marshaler& m);
    void memory_stream_add_ref(Marshaler& m);
    void memory_stream_release(Marshaler& m);
    void memory_stream_read(Marshaler& m);
    void memory_stream_write(Marshaler& m);
    void memory_stream_seek(Marshaler& m);
    void memory_stream_set_size(Marshaler& m);
    void memory_stream_copy_to(Marshaler& m);
    void memory_stream_commit(Marshaler& m);
    void memory_stream_revert(Marshaler& m);
    void memory_stream_lock_region(Marshaler& m);
    void memory_stream_unlock_region(Marshaler& m);
    void memory_stream_stat(Marshaler& m);
    void memory_stream_clone(Marshaler& m);

    // Accessors for testing — report the host-only apartment (no guest thread).
    [[nodiscard]] u32 init_count() const noexcept {
        std::lock_guard<std::mutex> lock(m_com_mutex);
        auto it = m_apartments.find(0);
        return it == m_apartments.end() ? 0u : it->second.init_count;
    }
    [[nodiscard]] bool is_initialized() const noexcept { return init_count() > 0; }
    [[nodiscard]] usize mta_usage_count() const noexcept {
        std::lock_guard<std::mutex> lock(m_com_mutex);
        return m_mta_usage_cookies.size();
    }

private:
    [[nodiscard]] GuestAddr create_server_security(Marshaler& m);
    [[nodiscard]] GuestAddr ensure_task_malloc_object(Marshaler& m);
    /// Per-thread COM apartment. Windows keys this off the calling thread, so
    /// one thread entering an MTA must not make another thread's STA
    /// CoInitializeEx fail with RPC_E_CHANGED_MODE. The key is the guest TEB
    /// address, or 0 for callers with no guest thread.
    struct ApartmentState {
        u32 init_count{0};
        bool sta{false};             ///< COINIT_APARTMENTTHREADED (bit 0x2)
        bool disable_ole1dde{false}; ///< COINIT_DISABLE_OLE1DDE (bit 0x4)
        bool winrt{false};           ///< entered through RoInitialize
        bool apartment_set{false};
        bool mta_usage_joined{false};
        u32 ole_init_count{0};       ///< OleInitialize count on this thread
        u64 sta_id{0};               ///< RoGetApartmentIdentifier of an STA
        GuestAddr ole_tls{0};        ///< guest OLE TLS block, once published
    };

    /// Guest address of the OLE TLS block for this thread, allocating and
    /// publishing it into TEB.ReservedForOle on first use. 0 when there is no
    /// guest thread to publish to.
    /// Drop cached apartment state that belonged to an exited thread whose
    /// TEB address has since been recycled.
    void discard_stale_apartment(Marshaler& m, ApartmentState& state);
    /// Shared body of CoInitializeEx / RoInitialize for a guest thread.
    [[nodiscard]] u32 initialize_apartment(Marshaler& m, u32 coinit, bool winrt);
    [[nodiscard]] GuestAddr ensure_ole_tls(Marshaler& m, ApartmentState& state);
    /// Write @p state's apartment bits into the thread's OLE TLS block.
    void publish_ole_tls(Marshaler& m, ApartmentState& state);

    mutable std::mutex m_com_mutex;
    std::unordered_map<GuestAddr, ApartmentState> m_apartments;
    GuestAddr m_ole_tls_next{0};
    GuestAddr m_ole_tls_end{0};
    GuestAddr m_next_mta_usage_cookie{0x10000};
    std::set<GuestAddr> m_mta_usage_cookies;
    std::once_flag m_audio_engine_mta_once;
    std::set<void*> m_task_allocs;  ///< Track CoTaskMemAlloc'd pointers
    static thread_local ServerCallContext s_server_call_context;
    const GuestElevationState* m_elevation{nullptr};
    CallerImpersonation m_caller_impersonation;
    CallerRevert m_caller_revert;
    std::mutex m_server_security_mutex;
    std::unordered_map<GuestAddr, u32> m_server_security_refs;

    enum class ShellLinkInterfaceKind : u8 {
        Ansi,
        Unicode,
        PersistFile,
        PropertyStore,
    };

    struct ShellLinkInterfaceBinding {
        u32 shell_link_id{0};
        ShellLinkInterfaceKind kind{ShellLinkInterfaceKind::Unicode};
    };

    struct ShellLinkState {
        GuestAddr ansi_iface{0};
        GuestAddr unicode_iface{0};
        GuestAddr persist_iface{0};
        GuestAddr propstore_iface{0};
        u32 ref_count{1};
        std::string target_path;
        std::string arguments;
        std::string working_dir;
        std::string description;
        std::string icon_path;
        i32 show_cmd{1};
        /// IPropertyStore values: each is a guest PROPVARIANT owned by the
        /// link (PropVariantCopy in, PropVariantCopy out), in set order.
        struct Property {
            Guid fmtid{};
            u32 pid{0};
            GuestAddr value{0};
        };
        std::vector<Property> properties;
    };
    /// [MS-SHLLINK] 2.5.7 PropertyStoreDataBlock for @p props (empty when
    /// nothing serializable is set).
    [[nodiscard]] std::vector<u8> shelllink_property_block(
        Marshaler& m, const std::vector<ShellLinkState::Property>& props);

    /// Bytes behind one or more IStream objects (clones share it). Two kinds:
    /// SHCreateMemStream keeps a host vector; CreateStreamOnHGlobal keeps its
    /// data IN the HGLOBAL (GetHGlobalFromStream hands that handle back, and
    /// GlobalLock sees exactly the stream's bytes). fDeleteOnRelease frees the
    /// HGLOBAL when the last stream sharing it is released.
    struct MemoryStreamBacking {
        std::mutex mutex;
        std::vector<u8> bytes;
        u64 hglobal{0};
        u64 size{0};                       ///< logical size (HGLOBAL kind)
        bool delete_on_release{false};
        GlobalLocalManager* globals{nullptr};
        [[nodiscard]] bool is_hglobal() const noexcept { return hglobal != 0; }
        [[nodiscard]] u64 stream_size() const noexcept {
            return is_hglobal() ? size : bytes.size();
        }
        /// Copy stream bytes [pos, pos+n) to/from a host buffer; caller holds
        /// mutex and has checked the range against stream_size().
        bool read_at(GuestMemory& memory, u64 pos, u8* out, u64 n);
        bool write_at(GuestMemory& memory, u64 pos, const u8* in, u64 n);
        /// Set the logical size, growing the HGLOBAL when needed.
        bool resize(u64 new_size);
        ~MemoryStreamBacking();
    };

    struct MemoryStreamState {
        std::mutex mutex;
        u32 ref_count{1};
        u64 position{0};
        std::shared_ptr<MemoryStreamBacking> backing;
    };

    /// In-process stand-in for an OBJREF written by
    /// CoMarshalInterThreadInterfaceInStream. Keyed by the IStream guest
    /// pointer handed back to the caller.
    struct InterThreadMarshal {
        Guid iid{};
        GuestAddr punk{0}; ///< AddRef'd when the stream was created.
    };

    struct RunningObjectEntry {
        u32 cookie{0};
        GuestAddr object{0};
        GuestAddr moniker{0};
        std::array<u8, 8> last_change{};
        std::shared_ptr<ipc::RotObjectRegistration> shared_registration;
    };

    struct ItemMonikerState {
        u32 ref_count{1};
        std::string delimiter;
        std::string item;
    };

    struct ClassObjectRegistration {
        Guid clsid{};
        GuestAddr class_object{0};
        u32 context{0};
        u32 flags{0};
        bool suspended{false};
        std::unique_ptr<ipc::ComClassRegistration> shared_registration;
        GuestAddr apartment{0}; ///< TEB of the registering thread
    };
    /// CoLoadLibrary(bAutoFree=TRUE) modules per apartment (TEB): the final
    /// CoUninitialize of that thread unloads them (Learn CoUninitialize).
    std::unordered_map<GuestAddr, std::vector<GuestAddr>> m_autofree_libraries;
    /// Release what the final CoUninitialize of apartment `key` owns.
    void release_apartment_resources(Marshaler& m, GuestAddr key);

    struct RemoteInterfaceProxy {
        u32 owner_pid{0};
        u32 pointer_size{0};
        GuestAddr remote_object{0};
        Guid iid{};
        u32 local_refs{1};
        GuestAddr vtable{0};
        // Canonical local IUnknown identity shared by every interface proxy
        // obtained from this remote object.
        GuestAddr identity_object{0};
    };

    struct RemoteMethodBinding {
        GuestAddr proxy_object{0};
        u32 vtable_slot{0};
        AutomationMethodLayout layout;
    };

    enum class FilterGraphInterfaceKind : u8 {
        GraphBuilder,
        MediaControl,
        BasicAudio,
        MediaEvent,
        MediaSeeking,
        VideoWindow,
        BasicVideo,
        MediaPosition,
        MediaEventSink,
        OutputPin,
        MediaSample,
        EnumFilters,
    };

    struct FilterGraphInterfaceBinding {
        u32 filter_graph_id{0};
        FilterGraphInterfaceKind kind{FilterGraphInterfaceKind::GraphBuilder};
    };

    struct FilterGraphStream;
    struct FilterGraphState {
        GuestAddr graph_builder_iface{0};
        GuestAddr media_control_iface{0};
        GuestAddr basic_audio_iface{0};
        GuestAddr media_event_iface{0};
        GuestAddr media_seeking_iface{0};
        GuestAddr video_window_iface{0};
        GuestAddr basic_video_iface{0};
        GuestAddr media_position_iface{0};
        GuestAddr media_event_sink_iface{0};
        u32 ref_count{1};
        i32 volume{0};
        i32 balance{0};
        /// Filter Graph Manager exposes IVideoWindow before RenderFile;
        /// methods return E_NOINTERFACE until a video renderer is connected.
        bool video_renderer_connected{false};
        /// OAHWND (LONG_PTR): 8 bytes for an x64 guest.
        u64 video_owner{0};
        u64 video_message_drain{0};
        i32 video_left{0};
        i32 video_top{0};
        i32 video_width{0};
        i32 video_height{0};
        bool video_position_set{false};
        i32 video_visible{0};
        i32 video_auto_show{-1};
        i32 video_window_style{0};
        i32 video_window_style_ex{0};
        i32 video_window_state{1};
        i32 video_background_palette{0};
        i32 video_border_color{0};
        i32 video_fullscreen{0};
        i32 video_hide_cursor{0};
        u32 video_overlay_hwnd{0};
        u64 last_video_frame{~u64{0}};
        bool video_overlay_logged{false};
        i32 native_video_width{0};
        i32 native_video_height{0};
        i32 source_left{0};
        i32 source_top{0};
        i32 source_width{0};
        i32 source_height{0};
        i32 dest_left{0};
        i32 dest_top{0};
        i32 dest_width{0};
        i32 dest_height{0};
        bool default_source{true};
        bool default_dest{true};
        double media_rate{1.0};
        /// IMediaControl filter state: 0=Stopped, 1=Paused, 2=Running.
        u32 filter_state{0};
        /// One-shot DirectShow event for IMediaEvent::GetEvent.
        bool has_pending_event{false};
        u32 pending_event_code{0};
        /// IMediaEvent::GetEvent lParam1/lParam2 are LONG_PTR.
        u64 pending_param1{0};
        u64 pending_param2{0};
        /// The params of the event GetEvent just dequeued.
        u64 taken_param1{0};
        u64 taken_param2{0};
        /// Learn IMediaEvent::GetEventHandle: manual-reset event, signaled
        /// while the notification queue is non-empty. Do not close this handle.
        u32 event_handle{0};
        /// IMediaEventEx::SetNotifyWindow — hwnd 0 disables window notify.
        u64 notify_hwnd{0};
        u32 notify_msg{0};
        u64 notify_instance{0};
        /// 0 = notify enabled; AM_MEDIAEVENT_NONOTIFY (1) disables the queue.
        u32 notify_flags{0};
        std::shared_ptr<audio::IMediaPlayer> player;
        /// Guest IBaseFilter pointers added with IFilterGraph::AddFilter.
        /// RenderFile connects a video stream to an input pin on these
        /// filters instead of inserting the default overlay renderer.
        struct GuestFilter {
            GuestAddr filter{0};
            GuestAddr input_pin{0};
            GuestAddr mem_input{0};
            GuestAddr allocator{0};
            /// Unique graph name (AddFilter de-duplicates it) and the guest
            /// copy passed to JoinFilterGraph, owned by the graph.
            std::u16string name;
            GuestAddr name_ptr{0};
        };
        /// In AddFilter order; quartz enumerates the most recent first.
        std::vector<GuestFilter> guest_filters;
        /// Bumped on every AddFilter/RemoveFilter: IEnumFilters returns
        /// VFW_E_ENUM_OUT_OF_SYNC once it differs (measured).
        u32 filter_list_version{0};
        /// Counter for generated names ("%04u", "<name> %04u"), measured.
        u32 filter_name_index{1};
        GuestAddr output_pin{0};
        GuestAddr media_type{0};
        GuestAddr format_block{0};
        GuestAddr sample_iface{0};
        GuestAddr sample_buffer{0};
        u32 sample_buffer_bytes{0};
        u32 sample_data_bytes{0};
        u32 output_pin_refs{1};
        u32 sample_refs{1};
        /// The graph's own IMediaSample (used when the renderer's input pin
        /// has no allocator), with quartz CMediaSample semantics (measured,
        /// dshow_sample_probe): times and flags reset on each reuse.
        i64 sample_start{0};
        i64 sample_end{0};
        bool sample_start_valid{false};
        bool sample_end_valid{false};
        i64 sample_media_start{0};
        i64 sample_media_end{0};
        bool sample_media_time_valid{false};
        bool sample_sync_point{false};
        bool sample_preroll{false};
        bool sample_discontinuity{false};
        bool guest_video_connected{false};
        /// FOURCC of the connected guest input type (e.g. 'YV12'), or 0
        /// for uncompressed RGB32.
        u32 guest_video_fourcc{0};
        /// Streaming state shared with the graph's guest streaming thread
        /// while a guest renderer is connected (null otherwise).
        std::shared_ptr<FilterGraphStream> stream;
        /// EC_COMPLETE default handling: renderer reports counted since the
        /// last Run (one EC_COMPLETE reaches the application once every
        /// connected renderer has sent one).
        u32 renderer_completions{0};
        /// Guest filters exposing IAMFilterMiscFlags IS_RENDERER, counted
        /// at Run (measured: the FGM asks each filter when it runs), plus one
        /// for the graph's own audio rendering when the file has audio: on
        /// Windows RenderFile gives that stream a DirectSound Renderer, which
        /// reports IS_RENDERER and its own EC_COMPLETE.
        u32 renderer_count{0};
        /// The host audio rendering was counted at Run and has not yet
        /// reported its EC_COMPLETE.
        bool host_audio_renderer_pending{false};
        /// IMediaEvent::CancelDefaultHandling(EC_COMPLETE): renderer
        /// EC_COMPLETEs pass straight through.
        bool complete_default_cancelled{false};
        /// Built by Intelligent Connect from the filters in the graph (no
        /// host player): IMediaControl drives the filters only.
        bool intelligent{false};
        /// The graph's reference clock (AddRef'd), chosen at the first
        /// Pause / Run / SetDefaultSyncSource.
        GuestAddr clock{0};
        /// IMediaSeeking on graphs built from their filters: the stream
        /// position the current run started from, the clock time it started
        /// (tStart), and whether the application has had EC_COMPLETE since.
        i64 seek_position{0};
        i64 seek_run_start{0};
        bool seek_completed{false};
        /// The renderers' stop position as of the last Run / SetPositions
        /// (GetCurrentPosition is polled per frame; -1 = not asked yet).
        i64 seek_stop_cache{-1};
        /// The final Release is tearing the graph down.
        bool releasing{false};
    };

    /// Everything the streaming thread uses. Set up on the application
    /// thread while the graph is stopped; the control fields are guarded by
    /// `mutex`, and no host mutex is ever held across a guest call.
    struct FilterGraphStream {
        std::mutex mutex;
        std::condition_variable cv;
        u32 filter_graph_id{0};
        std::shared_ptr<audio::IMediaPlayer> player;
        GuestAddr mem_input{0};
        GuestAddr input_pin{0};
        GuestAddr allocator{0};
        GuestAddr own_sample{0};
        GuestAddr own_buffer{0};
        u32 own_buffer_bytes{0};
        u32 fourcc{0};
        u32 width{0};
        u32 height{0};
        /// 32 bytes of guest memory for the streaming thread's
        /// out-parameters and REFERENCE_TIME arguments.
        GuestAddr scratch{0};
        /// Stream time zero: the media position the current segment
        /// started at (sample times are relative to it).
        i64 segment_start{0};
        /// Guest filters (IBaseFilter*) as of the last state command, for
        /// the deferred-Run worker.
        std::vector<GuestAddr> filters;
        /// The graph clock the deferred Run reads its tStart from (0: none).
        GuestAddr clock{0};
        // --- guarded by mutex ---
        u32 target{0};            ///< 0 Stopped, 1 Paused, 2 Running
        u32 generation{0};        ///< bumped by every state command
        u32 flushes{0};           ///< bumped by Stop and by a seek's flush
        bool exit{false};
        bool thread_alive{false};
        bool run_worker_alive{false};
        bool run_pending{false};
        bool prerolled{false};
        bool halted{false};
        bool eos_sent{false};
        bool discontinuity{true};
        u64 last_frame{~u64{0}};
        u32 delivered{0};
    };

    friend u32 filter_graph_co_create_instance(
        ComCoreManager& mgr, Marshaler& m, const Guid& iid);

    [[nodiscard]] GuestAddr resolve_filter_graph_iface(
        const FilterGraphState& st, const Guid& iid) const;
    void erase_filter_graph(u32 sid, FilterGraphState& st);
    /// Graph id behind an interface pointer, or 0. Diagnostics only: the
    /// state pointer alone cannot distinguish two graphs in a log line.
    [[nodiscard]] u32 find_filter_graph_id_by_iface(GuestAddr iface) const;
    [[nodiscard]] FilterGraphState* find_filter_graph_by_iface(GuestAddr iface);
    /// EC_COMPLETE default handling for one renderer's report (guest Notify
    /// or the graph's own audio rendering). Caller holds
    /// m_filter_graph_clock_mutex.
    void report_filter_graph_event_locked(FilterGraphState& st, u32 code, u64 p1, u64 p2);
    void report_host_audio_complete(u32 sid);
    [[nodiscard]] const FilterGraphState* find_filter_graph_by_iface(
        GuestAddr iface) const;
    void refresh_filter_graph_event(FilterGraphState& st);
    void present_filter_graph_video(FilterGraphState& st, Marshaler* m = nullptr);
    void hide_filter_graph_video(FilterGraphState& st);
    void connect_guest_video_renderer(Marshaler& m, FilterGraphState& st);
    void release_guest_filters(Marshaler& m, FilterGraphState& st);
    [[nodiscard]] GuestAddr ensure_graph_output_pin(Marshaler& m,
                                                    FilterGraphState& st);
    [[nodiscard]] GuestAddr ensure_graph_media_sample(Marshaler& m,
                                                      FilterGraphState& st,
                                                      u32 bytes);
    [[nodiscard]] u32 guest_vtable_call(Marshaler& m, GuestAddr punk, u32 slot,
                                        const u64* args, usize count);
    /// Drop one graph reference (IUnknown::Release semantics); destroys the
    /// graph at zero. Returns the new count.
    u32 release_filter_graph_ref(Marshaler& m, u32 sid);
    /// Binding of an interface pointer under the map mutex.
    [[nodiscard]] std::optional<FilterGraphInterfaceBinding>
    filter_graph_binding(GuestAddr iface) const;
    /// IMediaFilter calls on every guest filter (slot 4 Stop, 5 Pause,
    /// 6 Run); returns the first failure, else S_FALSE if any filter
    /// returned S_FALSE, else S_OK.
    u32 call_guest_filters_state(Marshaler& m, const std::vector<GuestAddr>& filters,
                                 u32 slot, i64 start = 0);
    /// IMediaFilter::GetState(ms) on every guest filter: the strongest of
    /// VFW_S_STATE_INTERMEDIATE / VFW_S_CANT_CUE / failure.
    u32 query_guest_filters_state(Marshaler& m, const std::vector<GuestAddr>& filters,
                                  u32 timeout_ms);
    [[nodiscard]] std::vector<GuestAddr> guest_filter_list(const FilterGraphState& st) const;
    /// Start (or retarget) the streaming thread for state 1/2.
    bool start_guest_stream(Marshaler& m, FilterGraphState& st, u32 target);
    /// Stop the streaming thread and the run worker and wait for both to
    /// leave guest code. Call after the renderers' Stop.
    void stop_guest_stream(FilterGraphState& st);
    /// stop_guest_stream, then free the stream state (graph disconnect).
    void drop_guest_stream(FilterGraphState& st);
    u32 set_stream_allocator_committed(Marshaler& m, const FilterGraphStream* s, bool commit);
    /// Intelligent Connect (filter_graph_connect.cpp).
    u32 add_filter_internal(Marshaler& m, FilterGraphState& st, GuestAddr filter,
                            const std::u16string* name);
    void remove_filter_internal(Marshaler& m, FilterGraphState& st, GuestAddr filter);
    u32 intelligent_render_file(Marshaler& m, FilterGraphState& st, const std::u16string& path);
    bool render_output_pin(Marshaler& m, FilterGraphState& st, GuestAddr pin, u32 depth,
                           std::vector<u32>& failures, std::vector<GuestAddr>& added);
    u32 ensure_graph_clock(Marshaler& m, FilterGraphState& st);
    std::vector<GuestAddr> filters_downstream_first(Marshaler& m, const FilterGraphState& st);
    void disconnect_all_pins(Marshaler& m, FilterGraphState& st);
    u32 intelligent_pause(Marshaler& m, FilterGraphState& st, bool from_running);
    u32 intelligent_run(Marshaler& m, FilterGraphState& st);
    u32 intelligent_stop(Marshaler& m, FilterGraphState& st);
    /// IMediaPosition method @p op on such a graph.
    u32 intelligent_position(Marshaler& m, FilterGraphState& st, u32 op);
    /// IMediaSeeking of the graph's sink filters (AddRef'd).
    std::vector<GuestAddr> graph_seekers(Marshaler& m, FilterGraphState& st);
    i64 graph_clock_now(Marshaler& m, GuestAddr clock);
    i64 graph_position(Marshaler& m, FilterGraphState& st);
    i64 graph_stop_position(Marshaler& m, FilterGraphState& st, u32* hr_out = nullptr);
    u32 graph_set_positions(Marshaler& m, FilterGraphState& st, GuestAddr current, u32 current_flags,
                            GuestAddr stop, u32 stop_flags);
    /// "[quartz]" milestone: the graph's filters and connections.
    void log_graph_shape(Marshaler& m, FilterGraphState& st, const char* reason);
    /// IBasicAudio put_Volume (7) / put_Balance (9) to the graph's audio renderers.
    void forward_basic_audio(Marshaler& m, FilterGraphState& st, u32 slot, u32 value);
    /// The graph clock's time plus the measured ~10 ms Run lead, or 0.
    i64 graph_run_start(Marshaler& m, GuestAddr clock);
    /// IMediaControl::Pause with guest filters (also Run's cue step).
    u32 guest_graph_pause(Marshaler& m, FilterGraphState& st, bool from_running);
    /// Seek; with a running streaming thread, flush the renderer around it.
    bool seek_graph(Marshaler& m, FilterGraphState& st, i64 position);
    /// Guest filters whose IAMFilterMiscFlags::GetMiscFlags has
    /// AM_FILTER_MISC_FLAGS_IS_RENDERER.
    u32 count_guest_renderers(Marshaler& m, const std::vector<GuestAddr>& filters);
    /// Deliver one decoded frame on the streaming thread. Returns the
    /// renderer's Receive result (or a failure before it).
    u32 deliver_stream_sample(Marshaler& m, FilterGraphStream& s,
                              const std::vector<u8>& rgba, u32 width, u32 height,
                              i64 start_100ns);
    /// New IEnumFilters over graph @p sid (holds one graph reference).
    [[nodiscard]] GuestAddr create_filter_enum(Marshaler& m, u32 sid,
                                               u32 position, u32 version);
    void ensure_filter_graph_event(FilterGraphState& st);
    void signal_filter_graph_event(FilterGraphState& st);
    void reset_filter_graph_event(FilterGraphState& st);
    void notify_filter_graph_window(const FilterGraphState& st);
    void start_filter_graph_clock();
    void stop_filter_graph_clock();
    void filter_graph_clock_loop();
    [[nodiscard]] u32 wait_filter_graph_notification(
        FilterGraphState& st, i32 timeout_ms, u32& ev_code,
        Marshaler* m = nullptr);

    [[nodiscard]] std::shared_ptr<MemoryStreamState>
    find_memory_stream(GuestAddr object);
    [[nodiscard]] GuestAddr create_memory_stream_object(
        Marshaler& m,
        std::shared_ptr<MemoryStreamBacking> backing,
        u64 position);

    /// Call IUnknown::QueryInterface on a guest object.
    [[nodiscard]] u32 guest_iunknown_qi(Marshaler& m, GuestAddr punk,
                                        GuestAddr iid_ptr, GuestAddr out_ptr);
    [[nodiscard]] bool monikers_equal(
        Marshaler& m, GuestAddr left, GuestAddr right);
    [[nodiscard]] GuestAddr create_remote_iunknown_proxy(
        Marshaler& m, const ipc::ComClassSnapshot& owner,
        GuestAddr remote_object, const Guid& iid,
        const AutomationInterfaceLayout* layout = nullptr,
        GuestAddr identity_object = 0);
    [[nodiscard]] std::optional<u32> call_remote_iunknown(
        const RemoteInterfaceProxy& proxy, u32 slot,
        std::span<const u64> arguments) const;

    [[nodiscard]] bool read_shelllink_string(
        Marshaler& m,
        const ShellLinkInterfaceBinding& binding,
        GuestAddr string_ptr,
        std::string& value) const;

    ThunkRegistry* m_thunks{nullptr};
    std::shared_ptr<dshow::DShowHost> m_dshow;  // shared_ptr: complete type not needed here
    capsule::Vfs* m_vfs{nullptr};
    windowing::CocoaBridge* m_cocoa_bridge{nullptr};
    std::shared_ptr<ShellItemDialogs> m_item_dialogs;   ///< created on first use
    std::once_flag m_item_dialogs_once;
    SyncManager* m_sync{nullptr};
    MessageSendManager* m_message_send{nullptr};
    capsule::Registry* m_registry{nullptr};
    ModuleManager* m_modules{nullptr};
    ComTaskMemoryManager* m_guest_task_memory{nullptr};
    GuestCall m_guest_call;
    GlobalLocalManager* m_globals{nullptr};
    /// RegisterDragDrop registrations: HWND -> AddRef'd IDropTarget.
    std::mutex m_drop_targets_mutex;
    std::unordered_map<u64, GuestAddr> m_drop_targets;


    // Family locks may nest this allocator lock; it never acquires a family
    // lock and is never held across guest calls, table operations or I/O.
    std::mutex m_scratch_mutex;
    GuestAddr m_next_com_addr{COM_SCRATCH_BASE};
    GuestAddr m_com_scratch_end{COM_SCRATCH_BASE + COM_SCRATCH_SIZE};
    [[nodiscard]] GuestAddr reserve_com_scratch(GuestAddr bytes) noexcept;
    // Shell links are separate apartment-owned objects sharing process tables.
    // This state lock is not held across file I/O or guest callbacks.
    std::mutex m_shell_links_mutex;
    // Window property stores: one CoTaskMem block per store (IUnknown, then
    // IPropertyStore), sharing one vtable per bitness. Keyed by block.
    struct WindowPropertyStoreState {
        u32 hwnd{0};
        u32 refs{0};
        GuestAddr unknown_iface{0};
        GuestAddr store_iface{0};
    };
    [[nodiscard]] WindowPropertyStoreState* window_store_locked(GuestAddr iface);
    [[nodiscard]] u32 window_propstore_hwnd(Marshaler& m, u32& hwnd);
    std::mutex m_window_stores_mutex;
    std::unordered_map<GuestAddr, WindowPropertyStoreState> m_window_stores;
    std::unordered_map<GuestAddr, GuestAddr> m_window_store_ifaces;
    GuestAddr m_window_store_vtable[2]{0, 0};
    std::function<bool(u32)> m_window_exists;
    u32 m_next_shelllink_id{1};
    std::unordered_map<u32, ShellLinkState> m_shell_links;
    std::unordered_map<GuestAddr, ShellLinkInterfaceBinding> m_iface_to_shelllink;
    std::unordered_map<GuestAddr, u32> m_global_options_refs;
    std::unordered_map<u32, u64> m_global_options_values;
    GuestAddr m_task_malloc_object{0};
    u32 m_task_malloc_refs{0};
    // Network List Manager object graph: NLM + CPC + CP share one refcount.
    // Connection enumerators and the one host connection are independently
    // refcounted. The adapter id matches iphlpapi's AdapterName GUID.
    struct NetworkListManagerState {
        u32 refs{1};
        GuestAddr nlm{0};
        GuestAddr cpc{0};
        GuestAddr cp{0};
        u32 next_advise_cookie{1};
    };
    u32 m_next_nlm_id{1};
    /// RoGetApartmentIdentifier values: one MTA id, one per STA.
    u64 m_mta_apartment_id{0};
    u64 m_next_apartment_id{0x10};
    struct ApartmentShutdownRegistration {
        GuestAddr callback{0};   ///< AddRef'd IApartmentShutdown
        u64 apartment_id{0};
        GuestAddr sta_teb{0};    ///< 0 for the MTA
    };
    std::unordered_map<u64, ApartmentShutdownRegistration> m_apartment_shutdowns;
    u64 m_next_shutdown_cookie{0x5A00};
    /// Resolve the calling thread's apartment id (MTA includes implicit MTA);
    /// caller holds m_com_mutex.
    [[nodiscard]] std::optional<std::pair<u64, GuestAddr>> apartment_identity_locked(
        Marshaler& m);
    void fire_apartment_shutdown(Marshaler& m, u64 apartment_id);
    std::unordered_map<u32, NetworkListManagerState> m_nlm_states;
    std::unordered_map<GuestAddr, u32> m_nlm_iface_to_id;
    struct NlmEnumState {
        u32 refs{1};
        u32 index{0};
        u32 count{1};
        bool yields_networks{false};
    };
    std::unordered_map<GuestAddr, NlmEnumState> m_nlm_enums;
    std::unordered_map<GuestAddr, u32> m_nlm_connection_refs;
    std::unordered_map<GuestAddr, u32> m_nlm_network_refs;
    struct MmDeviceEnumeratorState {
        u32 refs{1};
        std::set<GuestAddr> notification_clients;
    };
    std::unordered_map<GuestAddr, MmDeviceEnumeratorState>
        m_mmdevice_enumerators;
    struct MmDeviceCollectionState {
        u32 refs{1};
        u32 count{0};
        std::vector<AudioEndpointRef> endpoints;
    };
    std::unordered_map<GuestAddr, MmDeviceCollectionState>
        m_mmdevice_collections;
    struct MmDeviceState {
        u32 refs{1};
        AudioEndpointRef endpoint;
    };
    std::unordered_map<GuestAddr, MmDeviceState> m_mmdevice_refs;
    struct MmEndpointState {
        u32 refs{1};
        bool capture{false};
    };
    std::unordered_map<GuestAddr, MmEndpointState> m_mmendpoint_refs;
    std::unordered_map<GuestAddr, MmDeviceState> m_audio_property_store_refs;
    /// Shared between a capture stream and the backend's data callback, which
    /// runs on a host audio thread and must not take m_com_mutex: it signals
    /// the stream's event once a whole packet is waiting.
    struct CaptureSignal {
        std::mutex mutex;
        u64 event{0};
        bool started{false};
        audio::CapturePeriods periods{};
        audio::CaptureRing* ring{nullptr};
    };
    /// The next IAudioCaptureClient packet: its frames (0: none ready) and
    /// its engine period (device position periods.position(period)).
    struct CapturePacket {
        u32 frames{0};
        u64 period{0};
    };
    struct AudioClientState {
        u32 refs{1};
        audio::PcmFormat format{};
        u32 buffer_frames{0};
        /// IAudioRenderClient::GetBuffer packet still owed a ReleaseBuffer.
        bool buffer_pending{false};
        u32 pending_frames{0};
        GuestAddr staging{0};
        bool initialized{false};
        bool started{false};
        /// Initialize saw AUDCLNT_STREAMFLAGS_EVENTCALLBACK.
        bool event_driven{false};
        /// Data or a Start since the last Reset (Reset answers S_FALSE when
        /// the stream is already reset).
        bool needs_reset{false};
        /// Our duplicate of the SetEventHandle handle, so the guest may close
        /// its own copy while the stream lives (Windows' audio service holds
        /// its own reference the same way). 0 when none is registered.
        u64 event_handle{0};
        std::unique_ptr<audio::IAudioPushVoice> voice;
        /// Initialize's StreamFlags (AUDCLNT_STREAMFLAGS_* | SESSIONFLAGS_*).
        u32 stream_flags{0};
        /// Frames handed to the voice since the last Reset. Minus the voice's
        /// padding, it is the number of frames played: the IAudioClock
        /// position (frozen while stopped because nothing plays).
        u64 submitted_frames{0};
        /// Per-second `[wasapi-client]` render diagnostics (observation only).
        struct RenderDiag {
            double window_start_ms{0.0};
            double padding_lock_max_us{0.0};
            double get_buffer_lock_max_us{0.0};
            double release_lock_max_us{0.0};
            u32 padding_calls{0};
            u32 padding_min{UINT32_MAX};
            u32 releases{0};
            u32 release_zero{0};
            u32 release_silent{0};
            u64 frames{0};
            u32 windows{0};
        } diag;
        /// Start has been called at least once (IAudioClock2 reports
        /// AUDCLNT_S_POSITION_STALLED until then).
        bool ever_started{false};
        /// QPC time (100 ns units) of the last position reading: 0 until the
        /// first Start, frozen at Stop, untouched by Reset (measured).
        u64 position_qpc{0};
        /// IAudioClockAdjustment rate, as a ratio to the format's rate, and
        /// the device-position checkpoint taken at the last rate change.
        double rate_ratio{1.0};
        double device_frames_at_checkpoint{0.0};
        u64 played_frames_at_checkpoint{0};
        /// The audio session this stream joined at Initialize (0: none).
        u32 session{0};
        /// IAudioStreamVolume, one linear level per stream channel.
        std::vector<float> stream_volumes;
        /// Live GetService objects, one per AudioServiceKind (0: none): a
        /// second GetService answers the same object (measured).
        std::array<GuestAddr, 4> services{};
        /// The live IAudioRenderClient (0: none), shared the same way.
        GuestAddr render_client{0};
        /// A capture endpoint's stream (eCapture): the host device it was
        /// activated on, the live stream once initialized, its engine-period
        /// packetization and the IAudioCaptureClient packet held by GetBuffer.
        bool capture{false};
        std::string capture_uid;
        std::shared_ptr<audio::IAudioCaptureStream> capture_stream;
        std::shared_ptr<CaptureSignal> capture_signal;
        audio::CapturePeriods capture_periods{};
        GuestAddr capture_client{0};
        bool capture_held{false};
        u32 capture_held_frames{0};
    };
    std::unordered_map<GuestAddr, AudioClientState> m_audio_clients;
    /// IAudioClient::Initialize for a capture endpoint, after the common
    /// checks. Caller holds m_com_mutex.
    [[nodiscard]] u32 initialize_capture_locked(GuestAddr object, AudioClientState& state,
                                                u32 flags, u64 duration,
                                                const audio::PcmFormat& format,
                                                const Guid& session_guid, u32& buffer_frames);

    /// One RegisterAudioSessionNotification registration. Deliveries pin it
    /// so an Unregister that races a callback defers its Release until the
    /// callback has returned.
    struct AudioSinkRegistration {
        GuestAddr sink{0};
        u32 pins{0};
        bool release_pending{false};
    };
    /// One audio session (Learn: Audio Sessions). Streams join it at
    /// Initialize by session GUID (NULL means GUID_NULL) and by whether it is
    /// cross-process. Name, icon, grouping parameter, running state and
    /// notification sinks are per session; volume, mute and channel volume
    /// are shared by the process's sessions on the endpoint (measured: every
    /// session of the process shares them, see AudioEndpointVolume).
    struct AudioSessionState {
        Guid guid{};
        bool cross_process{false};
        /// Sessions belong to an endpoint: render, or one capture device.
        bool capture{false};
        std::string capture_uid;
        /// IChannelAudioVolume's channel count: the first stream's channels.
        u32 channels{0};
        std::u16string display_name;
        std::u16string icon_path;
        /// Initially a fresh GUID per session (measured).
        Guid grouping{};
        bool ducking_opt_out{false};
        /// One entry per registration (registering twice is two, measured).
        std::vector<std::shared_ptr<AudioSinkRegistration>> sinks;
        u32 streams{0};
        u32 running_streams{0};
        u32 state{0};
    };
    std::unordered_map<u32, AudioSessionState> m_audio_sessions;
    u32 m_next_audio_session{1};
    /// The session volume of the process on one endpoint: ISimpleAudioVolume
    /// and IChannelAudioVolume read and write it through any of the process's
    /// sessions there (measured, wasapi_services_probe). Sessions belong to an
    /// endpoint (Learn: Audio Sessions), so the render endpoint and each
    /// capture device have their own.
    struct AudioEndpointVolume {
        float master{1.0f};
        /// The BOOL exactly as SetMute stored it (GetMute returns it as is).
        u32 mute{0};
        /// Per channel index; entries a session has never set read 1.0.
        std::vector<float> channels;
    };
    std::unordered_map<std::string, AudioEndpointVolume> m_audio_endpoint_volumes;
    [[nodiscard]] static std::string audio_endpoint_key(bool capture, const std::string& uid) {
        return capture ? "capture:" + uid : std::string("render");
    }
    /// Caller holds m_com_mutex.
    AudioEndpointVolume& audio_endpoint_volume_locked(bool capture, const std::string& uid) {
        return m_audio_endpoint_volumes[audio_endpoint_key(capture, uid)];
    }
    /// A stream's effective per-channel gain: stream volume x session channel
    /// volume x session master volume, 0 when muted (Learn: Session Volume
    /// Controls). Caller holds m_com_mutex.
    [[nodiscard]] std::vector<float> audio_stream_gains_locked(const AudioClientState& state);

    enum class AudioServiceKind : u8 { Clock, ClockAdjustment, Session, StreamVolume };
    enum class AudioFacet : u8 {
        Identity, Clock, Clock2, ClockAdjustment, SessionControl, SimpleVolume,
        ChannelVolume, StreamVolume
    };
    /// A GetService object: one guest block of interface pointers, one per
    /// facet the object answers QueryInterface with (each pointing at that
    /// facet's shared vtable). One reference count for all facets; the
    /// object holds one reference on its IAudioClient while it lives.
    struct AudioServiceState {
        AudioServiceKind kind{AudioServiceKind::Clock};
        u32 refs{0};
        GuestAddr client{0};
        std::vector<std::pair<AudioFacet, GuestAddr>> facets;
    };
    std::unordered_map<GuestAddr, AudioServiceState> m_audio_services;
    /// Facet interface pointer -> owning service block and the facet it is.
    std::unordered_map<GuestAddr, std::pair<GuestAddr, AudioFacet>> m_audio_service_facets;
    /// Shared facet vtables, keyed by facet and pointer size.
    std::unordered_map<u32, GuestAddr> m_audio_service_vtables;

    /// One IAudioSessionEvents callback (vtable `slot`) for the sinks
    /// registered on `session` when the delivery thread reaches it.
    struct AudioSessionEvent {
        u32 slot{0};
        u32 session{0};
        std::u16string text;
        Guid context{};
        Guid grouping{};
        float volume{0.0f};
        u32 mute{0};
        std::vector<float> channel_volumes;
        u32 changed_channel{0};
        u32 state{0};
    };
    std::deque<AudioSessionEvent> m_audio_session_events;
    bool m_audio_session_worker_running{false};
    /// Sink references dropped under m_com_mutex, released by
    /// finish_audio_change once the lock is gone.
    std::vector<GuestAddr> m_audio_sink_releases;

    /// The object of `kind` for `client` (created on first use) and its
    /// `facet` interface pointer, AddRef'd. 0 when it cannot be created.
    /// Caller holds no lock.
    [[nodiscard]] GuestAddr get_audio_service(Marshaler& m, GuestAddr client,
                                              AudioServiceKind kind, AudioFacet facet);
    /// Resolve `this` of a service method to its block. Caller holds m_com_mutex.
    [[nodiscard]] AudioServiceState* find_audio_service_locked(GuestAddr facet,
                                                               AudioFacet expected);
    /// The stream behind a service method's `this`. Caller holds m_com_mutex.
    [[nodiscard]] AudioClientState* audio_service_client_locked(GuestAddr facet,
                                                                AudioFacet expected);
    /// Join `state` to its session at Initialize. Caller holds m_com_mutex.
    void join_audio_session_locked(AudioClientState& state, const Guid& session_guid);
    /// A stream left its session (client destroyed). Caller holds m_com_mutex.
    void leave_audio_session_locked(AudioClientState& state);
    /// Push the effective per-channel gain (stream volume x session channel
    /// volume x master volume x mute) to every stream's voice, or to one
    /// client's when `only` is non-zero. Caller holds m_com_mutex.
    void apply_audio_gains_locked(GuestAddr only = 0);
    /// Session running-state bookkeeping for Start/Stop: Active while any of
    /// its streams runs, Inactive when the last one stops, with an
    /// OnStateChanged event on each transition. Caller holds m_com_mutex.
    void update_audio_session_state_locked(u32 session, int running_delta);
    /// Queue an event for the session's sinks (dropped when it has none).
    /// Caller holds m_com_mutex and calls finish_audio_change() after
    /// releasing it.
    void queue_audio_session_event_locked(AudioSessionEvent event);
    /// Release sink references dropped under the lock and start the
    /// delivery thread when events are queued. Caller holds no lock.
    void finish_audio_change(Marshaler& m);
    /// Destroy a service object whose last reference is gone. Caller holds
    /// m_com_mutex.
    void destroy_audio_service_locked(
        std::unordered_map<GuestAddr, AudioServiceState>::iterator service);
    /// Frames the stream has played (submitted minus padding).
    [[nodiscard]] static u64 audio_played_frames(const AudioClientState& state);
    struct AudioRenderClientState { u32 refs{1}; GuestAddr client{0}; };
    std::unordered_map<GuestAddr, AudioRenderClientState> m_audio_render_clients;
    std::unordered_map<GuestAddr, AudioRenderClientState> m_audio_capture_clients;
    audio::IAudioCapture* m_audio_capture{nullptr};
    /// The capture packet the next GetBuffer/GetNextPacketSize reports.
    /// Caller holds m_com_mutex.
    [[nodiscard]] static CapturePacket capture_packet(const AudioClientState& state);
    audio::IAudioRenderer* m_audio_renderer{nullptr};
    bool m_audio_endpoint_available{false};
    /// Engine-period signaller for running event-driven WASAPI streams;
    /// created on the first Start of such a stream.
    std::unique_ptr<AudioEventClock> m_audio_event_clock;
    /// Tear down one IAudioClient whose last reference is gone. Caller holds
    /// m_com_mutex.
    void destroy_audio_client_locked(
        std::unordered_map<GuestAddr, AudioClientState>::iterator client);
    // CoGetObjectContext: one IComThreadingInfo per guest thread (keyed by
    // TEB), refcounted like any COM object. The logical thread id is per
    // thread and settable, as on Windows.
    struct ThreadingInfoState {
        u32 refs{0};
        GuestAddr object{0};
        GuestAddr teb{0};
        Guid logical_thread_id{};
    };
    std::unordered_map<GuestAddr, ThreadingInfoState> m_threading_infos;
    std::unordered_map<GuestAddr, GuestAddr> m_threading_info_iface_to_teb;
    /// First thread to enter an STA — APTTYPE_MAINSTA belongs to it alone.
    GuestAddr m_main_sta_teb{0};
    std::unordered_map<GuestAddr, u32> m_ftm_refs;
    std::unordered_map<GuestAddr, GuestAddr> m_ftm_outer;
    std::unordered_map<GuestAddr, GuestAddr> m_ftm_marshal;        ///< inner -> IMarshal
    std::unordered_map<GuestAddr, GuestAddr> m_ftm_marshal_owner;  ///< IMarshal -> inner
    /// Guest QueryInterface / AddRef on an arbitrary interface pointer.
    [[nodiscard]] u32 guest_query_interface(Marshaler& m, GuestAddr punk,
                                            GuestAddr iid, GuestAddr out);
    void guest_iunknown_add_ref(Marshaler& m, GuestAddr punk);
    std::unordered_map<GuestAddr, u32> m_agile_refs;
    std::unordered_map<GuestAddr, GuestAddr> m_agile_targets;
    /// Guest HGLOBAL backing an in-memory IStream, or 0 when @p stream is not
    /// one of ours.
    [[nodiscard]] GuestAddr memory_stream_hglobal(GuestAddr stream);
    u32 m_next_filter_graph_id{1};
    std::unordered_map<u32, FilterGraphState> m_filter_graphs;
    std::unordered_map<GuestAddr, FilterGraphInterfaceBinding> m_iface_to_filter_graph;
    /// IEnumFilters objects handed out by IFilterGraph::EnumFilters.
    struct FilterEnumState {
        u32 filter_graph_id{0};
        u32 refs{1};
        u32 position{0};
        u32 version{0};
    };
    std::unordered_map<GuestAddr, FilterEnumState> m_filter_enums;
    /// Shared IEnumFilters vtable per guest bitness (index 0 = x86).
    GuestAddr m_filter_enum_vtables[2]{0, 0};
    std::mutex m_filter_graph_clock_mutex;
    /// Structure of m_iface_to_filter_graph / m_filter_graphs /
    /// m_filter_streams: lookups from the streaming threads race the
    /// application thread's inserts and erases. Held only around map
    /// operations (after m_filter_graph_clock_mutex when both are held).
    mutable std::mutex m_filter_graph_map_mutex;
    std::unordered_map<u32, std::shared_ptr<FilterGraphStream>> m_filter_streams;
    std::condition_variable m_filter_graph_clock_cv;
    std::thread m_filter_graph_clock_thread;
    std::atomic<bool> m_filter_graph_clock_stop{false};
    std::atomic<u32> m_filter_graph_running_count{0};
    std::mutex m_memory_streams_mutex;
    std::unordered_map<GuestAddr, std::shared_ptr<MemoryStreamState>>
        m_memory_streams;
    std::mutex m_interthread_mutex;
    std::unordered_map<GuestAddr, InterThreadMarshal> m_interthread_marshals;
    std::mutex m_running_object_table_mutex;
    GuestAddr m_running_object_table{0};
    u32 m_running_object_table_refs{1};
    u32 m_next_running_object_cookie{1};
    std::vector<RunningObjectEntry> m_running_objects;
    std::mutex m_item_monikers_mutex;
    std::unordered_map<GuestAddr, ItemMonikerState> m_item_monikers;
    std::mutex m_class_objects_mutex;
    u32 m_next_class_object_cookie{1};
    u32 m_server_process_refs{0};
    std::unordered_map<u32, ClassObjectRegistration> m_class_objects;
    std::mutex m_remote_proxies_mutex;
    std::unordered_map<GuestAddr, RemoteInterfaceProxy> m_remote_proxies;
    std::unordered_map<GuestAddr, RemoteMethodBinding> m_remote_methods;
    GuestAddr m_next_remote_method_alias{0x7C000000};  // with the module arena
    std::string m_capsule_root;
    LocalServiceActivator m_local_service_activator;
    LocalServerActivator m_local_server_activator;
    std::chrono::milliseconds m_local_server_registration_timeout{60000};
    AutomationLayoutResolver m_automation_layout_resolver;
    AutomationBstrAllocator m_automation_bstr_allocator;
    ExternalClassFactory    m_external_class_factory;
    std::mutex m_proxy_stub_mutex;
    std::unordered_map<std::string, Guid> m_proxy_stub_clsids;
    std::mutex m_initialize_spy_mutex;
    u64 m_next_initialize_spy_cookie{1};
    std::unordered_map<u64, GuestAddr> m_initialize_spies;
    AudioActivationThread m_audio_activation_thread;
    struct AudioActivationState {
        u32 refs{2}; // caller and completion worker each own a reference
        GuestAddr handler{0};
        GuestAddr callback{0};
        GuestAddr activated{0};
        u32 result{0};
        bool complete{false};
    };
    std::unordered_map<GuestAddr, AudioActivationState> m_audio_activations;
    [[nodiscard]] u32 release_audio_activation(Marshaler& m, GuestAddr operation);
    std::shared_ptr<WicContext> m_wic_context{create_wic_context()};
};

DllExports create_com_core_thunks(ComCoreManager& mgr);
/// The IAudioClient::GetService object methods (wasapi_services.cpp).
void append_wasapi_service_exports(DllExports& exports, ComCoreManager& mgr);
/// IAudioClient2 / IAudioClient3 are not implemented: log the request once
/// (UNIMPLEMENTED, naming `api`) when `iid` is one of them. The caller answers
/// E_NOINTERFACE, the documented "interface not available" result.
void note_unimplemented_audio_client_version(const Guid& iid, const char* api);
/// The guest's QueryPerformanceCounter now, in 100-nanosecond units (the unit
/// IAudioClock::GetPosition reports its QPC position in).
[[nodiscard]] u64 audio_qpc_now_hns() noexcept;

} // namespace vitreo::winapi
