// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dxgi.hpp
/// @brief DXGI 1.2 factory and adapter COM bridge.
///
/// The bridge exposes the real Metal adapter to Windows guests through the
/// enumeration and description contracts used before D3D device creation.
/// IDXGIFactory2::CreateSwapChainForHwnd is delegated to the D3D11 frontend.

#include <vitreo/common.hpp>
#include "graphics/common/autorelease_scope.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/graphics/common/device_lock_profile.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

class Marshaler;

namespace dxgi {

/// The stable process-local LUID of the one adapter.
///
/// Named because several places must agree on it: the adapter descriptor
/// writes it, IDXGIFactory4::EnumAdapterByLuid matches against it, and
/// ID3D12Device::GetAdapterLuid reports it for the device created on that
/// adapter. A second literal that drifted would make EnumAdapterByLuid report
/// NOT_FOUND for the adapter it had just handed the guest.
inline constexpr u32 ADAPTER_LUID_LOW = 0x5654524Du;
inline constexpr u32 ADAPTER_LUID_HIGH = 1u;


constexpr u32 S_OK = 0;
constexpr u32 E_NOINTERFACE = 0x80004002u;
constexpr u32 E_POINTER = 0x80004003u;
constexpr u32 E_INVALIDARG = 0x80070057u;
constexpr u32 E_NOTIMPL = 0x80004001u;
constexpr u32 ERROR_INVALID_CALL = 0x887A0001u;
constexpr u32 ERROR_NOT_FOUND = 0x887A0002u;
constexpr u32 ERROR_MORE_DATA = 0x887A0003u;
constexpr u32 ERROR_UNSUPPORTED = 0x887A0004u;
/// DXGI_ERROR_SDK_COMPONENT_MISSING: the debug layer (dxgidebug.dll) is not
/// installed, which is the state of every non-developer Windows machine.
constexpr u32 ERROR_SDK_COMPONENT_MISSING = 0x887A002Du;

constexpr u32 VENDOR_ID_APPLE = 0x106Bu;

struct AdapterInfo {
    std::string description;
    u32 vendor_id{VENDOR_ID_APPLE};
    u32 device_id{0};
    u32 subsystem_id{0};
    u32 revision{0};
    u64 dedicated_video_memory{0};
    u64 dedicated_system_memory{0};
    u64 shared_system_memory{0};
};

/// Windows-visible description of one real host display output.
struct OutputInfo {
    std::string device_name;
    i32 left{0};
    i32 top{0};
    i32 right{0};
    i32 bottom{0};
    u64 monitor{0};
    u32 bits_per_color{8};
    double refresh_rate_hz{0.0};
    /// The display's own (desktop) resolution, which the mode list and
    /// FindClosestMatchingMode enumerate from. 0 means the desktop
    /// rectangle above; it differs while an exclusive full-screen chain has
    /// switched the output to another mode (DesktopCoordinates then follow
    /// the mode).
    u32 native_width{0};
    u32 native_height{0};
};

/// The resolution an output's modes are enumerated from.
[[nodiscard]] inline std::pair<u32, u32> output_native_size(
    const OutputInfo& info) {
    if (info.native_width != 0 && info.native_height != 0) {
        return {info.native_width, info.native_height};
    }
    return {static_cast<u32>(std::max(0, info.right - info.left)),
            static_cast<u32>(std::max(0, info.bottom - info.top))};
}

} // namespace dxgi

class DxgiManager {
public:
    /// Shared by DXGI and its D3D11 frontend. Both directions of interop
    /// acquire this same recursive lock, never two graphics API locks.
    [[nodiscard]] std::recursive_mutex& api_mutex() const noexcept {
        return m_api_mutex;
    }
    [[nodiscard]] int& api_lock_depth() const noexcept;
    class ApiLock {
    public:
        explicit ApiLock(const DxgiManager& manager)
            : m_lock(manager.api_mutex(), std::defer_lock),
              m_depth(manager.api_lock_depth()), m_outer(m_depth == 0) {
            device_lock_profile::lock(manager.api_mutex());
            m_lock = std::unique_lock<std::recursive_mutex>(
                manager.api_mutex(), std::adopt_lock);
            if (m_outer) device_lock_profile::begin_hold();
            ++m_depth;
        }
        ~ApiLock() {
            --m_depth;
            if (m_outer) device_lock_profile::end_hold("IDXGI", 0);
        }
        ApiLock(const ApiLock&) = delete;
        ApiLock& operator=(const ApiLock&) = delete;
    private:
        // Constructed before the lock and popped after it is released.
        graphics::AutoreleaseScope m_autorelease;
        std::unique_lock<std::recursive_mutex> m_lock;
        int& m_depth;
        bool m_outer;
    };
    /// Suspend every tracked acquisition around external calls and waits.
    /// No iterator into graphics state may survive across this scope.
    class ApiUnlock {
    public:
        explicit ApiUnlock(const DxgiManager& manager)
            : m_mutex(manager.api_mutex()), m_current_depth(manager.api_lock_depth()),
              m_depth(m_current_depth) {
            m_current_depth = 0;
            if (m_depth > 0) device_lock_profile::suspend();
            for (int i = 0; i < m_depth; ++i) m_mutex.unlock();
        }
        ApiUnlock(const ApiUnlock&) = delete;
        ApiUnlock& operator=(const ApiUnlock&) = delete;
        ~ApiUnlock() {
            if (m_depth > 0) device_lock_profile::lock(m_mutex);
            for (int i = 1; i < m_depth; ++i) m_mutex.lock();
            m_current_depth = m_depth;
            if (m_depth > 0) device_lock_profile::resume();
        }
    private:
        std::recursive_mutex& m_mutex;
        int& m_current_depth;
        int m_depth;
    };
    using SwapChainCreator = std::function<void(Marshaler&)>;
    /// Injected host->guest call, used to AddRef/Release an interface the guest
    /// owns. See winapi/core/guest_com.hpp, including its reentrancy rule.
    using GuestFunctionCaller = winapi::GuestFunctionCaller;
    using OutputResolver = std::function<std::vector<dxgi::OutputInfo>()>;

    // IDXGIOutput6 (dxgi1_6.h): GetDesc1 at 27, CheckHardwareCompositionSupport
    // at 28. QI answers IDXGIOutput6, so the vtable must reach slot 28; with 28
    // entries a guest call to it branched to address zero.
    static constexpr usize OUTPUT_METHOD_COUNT = 29;

    struct MethodThunks {
        GuestAddr query_interface{0};
        GuestAddr add_ref{0};
        GuestAddr release{0};
        GuestAddr set_private_data{0};
        GuestAddr set_private_data_interface{0};
        GuestAddr get_private_data{0};
        GuestAddr get_parent{0};
        GuestAddr enum_adapters{0};
        GuestAddr make_window_association{0};
        GuestAddr get_window_association{0};
        GuestAddr create_swap_chain{0};
        GuestAddr create_software_adapter{0};
        GuestAddr enum_adapters1{0};
        GuestAddr is_current{0};
        GuestAddr is_windowed_stereo_enabled{0};
        GuestAddr create_swap_chain_for_hwnd{0};
        GuestAddr create_swap_chain_for_core_window{0};
        GuestAddr create_swap_chain_for_composition{0};
        GuestAddr get_shared_resource_adapter_luid{0};
        GuestAddr register_stereo_status_window{0};
        GuestAddr register_stereo_status_event{0};
        GuestAddr unregister_stereo_status{0};
        GuestAddr register_occlusion_status_window{0};
        GuestAddr register_occlusion_status_event{0};
        GuestAddr unregister_occlusion_status{0};
        // IDXGIFactory3 through IDXGIFactory7.
        GuestAddr get_creation_flags{0};
        GuestAddr enum_adapter_by_luid{0};
        GuestAddr enum_warp_adapter{0};
        GuestAddr factory_check_feature_support{0};
        GuestAddr enum_adapter_by_gpu_preference{0};
        GuestAddr register_adapters_changed_event{0};
        GuestAddr unregister_adapters_changed_event{0};
        GuestAddr enum_outputs{0};
        GuestAddr get_desc{0};
        GuestAddr check_interface_support{0};
        GuestAddr get_desc1{0};
        GuestAddr get_desc2{0};
        GuestAddr register_hwcp_teardown_event{0};
        GuestAddr unregister_hwcp_teardown{0};
        GuestAddr query_video_memory_info{0};
        GuestAddr set_video_memory_reservation{0};
        GuestAddr register_budget_change_event{0};
        GuestAddr unregister_budget_change{0};
        GuestAddr get_desc3{0};
        std::array<GuestAddr, OUTPUT_METHOD_COUNT> output{};
    };

    DxgiManager(GuestMemory& memory, bool is_64bit,
                dxgi::AdapterInfo adapter_info = {});
    ~DxgiManager();

    /// Re-detect the adapter description and PCI identity.
    ///
    /// The manager is constructed before the capsule registry is loaded, so
    /// the per-capsule advertised identity (and the Metal device itself) may
    /// not exist yet at construction time. Runtime start-up calls this once
    /// the capsule is in place. It is a no-op for a manager that was handed
    /// an explicit AdapterInfo.
    void refresh_adapter_info();

    void set_method_thunks(MethodThunks thunks);
    void set_swap_chain_creator(SwapChainCreator creator);
    void set_swap_chain_for_hwnd_creator(SwapChainCreator creator);
    void set_swap_chain_for_composition_creator(SwapChainCreator creator);
    void set_output_resolver(OutputResolver resolver);
    /// Supply the host->guest call used to AddRef/Release guest-owned
    /// interfaces. Without it SetPrivateDataInterface refuses, because storing
    /// the pointer without the reference is how a frontend reads freed memory.
    void set_guest_function_caller(GuestFunctionCaller caller);

    /// Host-side interop used by graphics frontends that receive an
    /// IDXGIAdapter pointer from the guest.
    [[nodiscard]] bool is_adapter_object(GuestAddr address) const;
    [[nodiscard]] GuestAddr create_default_adapter();
    [[nodiscard]] bool retain_adapter(GuestAddr address);
    void release_adapter(GuestAddr address);
    /// AddRef / Release any live DXGI object (factory, adapter, output)
    /// held on the guest's behalf by another frontend, e.g. a swap chain's
    /// parent factory and restrict-to output.
    [[nodiscard]] bool retain_object(GuestAddr address);
    void release_dxgi_object(GuestAddr address);
    [[nodiscard]] bool is_output_object(GuestAddr address) const;
    /// The current description of output object @p address (its desktop
    /// coordinates follow display-mode changes); nullopt when @p address is
    /// not a live IDXGIOutput.
    [[nodiscard]] std::optional<dxgi::OutputInfo> output_info_of(
        GuestAddr address) const;
    /// A new IDXGIOutput of @p adapter for the output named @p device_name
    /// (as EnumOutputs reports it now); 0 when there is none.
    [[nodiscard]] GuestAddr create_output_named(GuestAddr adapter,
                                                const std::string& device_name);
    /// The factory @p adapter was enumerated from (0 when unknown).
    [[nodiscard]] GuestAddr parent_factory_of(GuestAddr adapter) const;
    /// The host outputs, as EnumOutputs reports them.
    [[nodiscard]] std::vector<dxgi::OutputInfo> output_infos() const;
    /// Learn IDXGISwapChain::GetContainingOutput: a new IDXGIOutput of
    /// @p adapter for the output whose desktop rectangle overlaps the
    /// rectangle (left, top, right, bottom) most; 0 when none does.
    [[nodiscard]] GuestAddr create_containing_output(GuestAddr adapter,
                                                     i32 left, i32 top,
                                                     i32 right, i32 bottom);
    [[nodiscard]] u32 query_object_interface(GuestAddr object,
                                             GuestAddr iid_address,
                                             GuestAddr object_out);

    void impl_CreateDXGIFactory(Marshaler& m);
    void impl_CreateDXGIFactory1(Marshaler& m);
    void impl_CreateDXGIFactory2(Marshaler& m);
    void impl_DXGIGetDebugInterface1(Marshaler& m);
    void impl_DXGIDeclareAdapterRemovalSupport(Marshaler& m);

    void impl_QueryInterface(Marshaler& m);
    void impl_AddRef(Marshaler& m);
    void impl_Release(Marshaler& m);
    void impl_SetPrivateData(Marshaler& m);
    void impl_SetPrivateDataInterface(Marshaler& m);
    void impl_GetPrivateData(Marshaler& m);
    void impl_GetParent(Marshaler& m);

    void impl_EnumAdapters(Marshaler& m);
    void impl_MakeWindowAssociation(Marshaler& m);
    void impl_GetWindowAssociation(Marshaler& m);
    void impl_CreateSwapChain(Marshaler& m);
    void impl_CreateSoftwareAdapter(Marshaler& m);
    void impl_EnumAdapters1(Marshaler& m);
    void impl_IsCurrent(Marshaler& m);
    void impl_IsWindowedStereoEnabled(Marshaler& m);
    void impl_CreateSwapChainForHwnd(Marshaler& m);
    void impl_CreateSwapChainForComposition(Marshaler& m);
    /// Passed as @p out_bytes to mean "one guest pointer", whose width depends
    /// on the guest's bitness.
    static constexpr u32 POINTER_OUT = 0xFFFFFFFFu;
    void impl_GetCreationFlags(Marshaler& m);
    void impl_EnumAdapterByLuid(Marshaler& m);
    void impl_EnumWarpAdapter(Marshaler& m);
    void impl_FactoryCheckFeatureSupport(Marshaler& m);
    void impl_EnumAdapterByGpuPreference(Marshaler& m);
    void impl_RegisterAdaptersChangedEvent(Marshaler& m);
    void impl_UnregisterAdaptersChangedEvent(Marshaler& m);
    /// IDXGIFactory2 Register{Stereo,Occlusion}Status{Window,Event} and
    /// the matching Unregister (void) methods.
    void impl_RegisterStatus(Marshaler& m, bool stereo, bool window);
    void impl_UnregisterStatus(Marshaler& m, bool stereo);
    void impl_GetSharedResourceAdapterLuid(Marshaler& m);
    /// Answers "is this a live shared-resource handle" for
    /// GetSharedResourceAdapterLuid; wired by the D3D11 frontend.
    void set_shared_resource_query(std::function<bool(u64)> query) {
        ApiLock lock(*this);
        m_shared_resource_query = std::move(query);
    }
    void impl_Factory2Unsupported(Marshaler& m, const char* method,
                                  usize out_index = 0, u32 out_bytes = 0);

    void impl_EnumOutputs(Marshaler& m);
    void impl_GetDesc(Marshaler& m);
    void impl_CheckInterfaceSupport(Marshaler& m);
    void impl_GetDesc1(Marshaler& m);
    void impl_GetDesc2(Marshaler& m);
    void impl_RegisterHardwareContentProtectionTeardownStatusEvent(Marshaler& m);
    void impl_UnregisterHardwareContentProtectionTeardownStatus(Marshaler& m);
    void impl_QueryVideoMemoryInfo(Marshaler& m);
    void impl_SetVideoMemoryReservation(Marshaler& m);
    void impl_RegisterVideoMemoryBudgetChangeNotificationEvent(Marshaler& m);
    void impl_UnregisterVideoMemoryBudgetChangeNotification(Marshaler& m);
    void impl_GetDesc3(Marshaler& m);
    void impl_OutputMethod(Marshaler& m, usize slot);

private:
    enum class ObjectType {
        Factory,
        Adapter,
        Output,
    };

    using GuidBytes = std::array<u8, 16>;

    struct GuidHash {
        usize operator()(const GuidBytes& guid) const noexcept;
    };

    using PrivateInterface = std::shared_ptr<GuestAddr>;

    struct Object {
        ObjectType type{ObjectType::Factory};
        u32 ref_count{1};
        GuestAddr parent_object{0};
        GuestAddr window_association{0};
        u32 window_association_flags{0};
        /// CreateDXGIFactory2 Flags (IDXGIFactory3::GetCreationFlags).
        u32 creation_flags{0};
        dxgi::OutputInfo output_info;
        std::unordered_map<GuidBytes, std::vector<u8>, GuidHash> private_data;
        /// SetPrivateDataInterface entries. Learn: private data set by GUID and
        /// private *interfaces* set by GUID share one namespace, so a GUID is in
        /// exactly one of these two maps -- setting either drops the other, and
        /// dropping an interface releases it.
        // A host lease keeps the stored guest reference alive while GetPrivateData
        // calls guest AddRef with the graphics lock released.
        std::unordered_map<GuidBytes, PrivateInterface, GuidHash>
            private_data_interfaces;
    };

    [[nodiscard]] bool ensure_storage();
    [[nodiscard]] GuestAddr create_object(ObjectType type,
                                          GuestAddr parent_factory = 0);
    [[nodiscard]] GuestAddr create_factory_for_iid(GuestAddr iid_address);
    [[nodiscard]] GuestAddr create_adapter(GuestAddr parent_factory);
    [[nodiscard]] GuestAddr create_output(
        GuestAddr parent_adapter, const dxgi::OutputInfo& info);
    [[nodiscard]] bool write_pointer(GuestAddr address, GuestAddr value);
    [[nodiscard]] bool read_guid(GuestAddr address, GuidBytes& guid) const;
    [[nodiscard]] bool iid_is_supported(GuestAddr iid_address,
                                        ObjectType type) const;
    [[nodiscard]] bool iid_is_factory1(GuestAddr iid_address) const;
    [[nodiscard]] bool iid_is_factory2(GuestAddr iid_address) const;
    [[nodiscard]] u32 release_object(GuestAddr address);
    /// Hand the one host adapter to @p adapter_out, returning the HRESULT.
    /// Shared by EnumAdapters, EnumAdapters1, EnumAdapterByLuid and
    /// EnumAdapterByGpuPreference so the four cannot disagree about which
    /// adapter exists.
    [[nodiscard]] u32 write_single_adapter(Marshaler& m, GuestAddr factory,
                                          GuestAddr adapter_out,
                                          const char* caller,
                                          GuestAddr* created = nullptr);
    /// Remove an interface lease from the map. The caller drops the returned
    /// lease after finishing mutations: the final drop calls guest Release,
    /// which may re-enter this manager.
    [[nodiscard]] PrivateInterface take_private_interface(GuestAddr object_address,
                                                   const GuidBytes& name);
    [[nodiscard]] bool write_adapter_desc(Marshaler& m, GuestAddr address,
                                          u32 version) const;
    /// /tmp/vitreo-d3d11-caps: log the adapter description a GetDesc* call
    /// just returned (@p version as write_adapter_desc). Log only.
    void log_adapter_desc(const char* api, u32 version) const;
    [[nodiscard]] bool write_output_desc(Marshaler& m, GuestAddr address,
                                         const dxgi::OutputInfo& info) const;
    [[nodiscard]] bool write_output_desc1(Marshaler& m, GuestAddr address,
                                          const dxgi::OutputInfo& info) const;
    [[nodiscard]] bool write_mode_desc(Marshaler& m, GuestAddr address,
                                       const dxgi::OutputInfo& info,
                                       u32 format) const;
    void create_factory(Marshaler& m);
    void create_factory_at(Marshaler& m, GuestAddr iid, GuestAddr factory_out);
    void enum_adapter(Marshaler& m, bool version1);
    [[nodiscard]] bool adapter_object(GuestAddr self) const;
    [[nodiscard]] u32 register_adapter3_event(Marshaler& m,
                                              std::unordered_set<u32>& cookies,
                                              u32& next_cookie);

    [[nodiscard]] u32 guest_reference(GuestAddr interface_ptr, usize slot);
    [[nodiscard]] PrivateInterface own_private_interface(GuestAddr address);

    // Declared before the tables/callbacks: it outlives their destruction.
    mutable std::recursive_mutex m_api_mutex;
    bool m_shutting_down{false};
    GuestMemory& m_memory;
    bool m_is_64bit;
    dxgi::AdapterInfo m_adapter_info;
    bool m_adapter_info_is_explicit{false};
    MethodThunks m_method_thunks{};
    SwapChainCreator m_swap_chain_creator;
    SwapChainCreator m_swap_chain_for_hwnd_creator;
    SwapChainCreator m_swap_chain_for_composition_creator;
    OutputResolver m_output_resolver;
    GuestFunctionCaller m_guest_call;
    /// Live IDXGIFactory7 adapters-changed registrations. The adapter set never
    /// changes here, so the event legitimately never fires; the cookies exist so
    /// Unregister can reject one that was never issued.
    std::unordered_set<u32> m_adapters_changed_cookies;
    u32 m_next_adapters_changed_cookie{1};
    /// Stereo / occlusion status registrations (cookie -> stereo?). Vitreo
    /// has no stereo display and never reports occlusion, so none fires.
    std::unordered_map<u32, bool> m_status_cookies;
    u32 m_next_status_cookie{1};
    std::function<bool(u64)> m_shared_resource_query;
    /// IDXGIAdapter3 notification cookies. Budget never changes and HWCP
    /// teardown never fires on this host; the cookies exist so Unregister can
    /// drop a registration that was actually issued.
    std::unordered_set<u32> m_budget_change_cookies;
    std::unordered_set<u32> m_hwcp_teardown_cookies;
    u32 m_next_budget_change_cookie{1};
    u32 m_next_hwcp_teardown_cookie{1};
    GuestAddr m_region_base{0};
    GuestAddr m_factory_vtable{0};
    GuestAddr m_adapter_vtable{0};
    GuestAddr m_output_vtable{0};
    GuestAddr m_next_object{0};
    std::unordered_map<GuestAddr, Object> m_objects;
    std::vector<GuestAddr> m_free_objects;
};

[[nodiscard]] DllExports create_dxgi_thunks(DxgiManager& manager);
[[nodiscard]] std::string dxgi_output_method_export_name(usize slot);

} // namespace vitreo::winapi
