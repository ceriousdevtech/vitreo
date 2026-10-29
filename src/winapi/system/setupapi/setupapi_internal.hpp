// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file setupapi_internal.hpp
/// @brief Private surface shared by the setupapi_*.cpp parts and the cfgmgr32
///        parts.
///
/// setupapi.cpp was one translation unit whose helpers sat in an anonymous
/// namespace. The parts now live in separate files, so what more than one part
/// uses is named here, in vitreo::winapi::setupapi_internal. Each function is
/// defined in exactly one part (named in the group comment above it). The SDK
/// constants stay plain `constexpr`, so each part keeps its own internal-linkage
/// copy exactly as before.
///
/// Private to src/winapi/system/setupapi/: nothing else may include this header.

#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/common/elevation.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/system/setupapi/setupapi.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vitreo::winapi::setupapi_internal {

using Guid = std::array<u8, 16>;

/// INVALID_HANDLE_VALUE is (HANDLE)-1, which is pointer-sized. Returning the
/// 32-bit 0xFFFFFFFF to a 64-bit caller gives 0x00000000FFFFFFFF, and the
/// caller's `handle == INVALID_HANDLE_VALUE` test then *passes* the bogus
/// value through as if the call had succeeded — turning a clean failure into
/// an enumeration loop that never terminates.
[[nodiscard]] inline u64 invalid_handle_value(const Marshaler& m) {
    return m.cpu_is_64bit() ? ~0ull : 0xFFFFFFFFull;
}

// ---- SetupAPI.h / winerror.h (Windows SDK 10.0.26100) ----------------------
constexpr u32 ERROR_INVALID_USER_BUFFER = 1784;            // winerror.h
constexpr u32 ERROR_CLASS_MISMATCH = 0xE0000201u;
constexpr u32 ERROR_KEY_DOES_NOT_EXIST = 0xE0000204u;
constexpr u32 ERROR_INVALID_DEVINST_NAME = 0xE0000205u;
constexpr u32 ERROR_DEVINST_ALREADY_EXISTS = 0xE0000207u;
constexpr u32 ERROR_INVALID_REG_PROPERTY = 0xE0000209u;
constexpr u32 ERROR_NO_SUCH_DEVINST = 0xE000020Bu;
constexpr u32 ERROR_DI_DO_DEFAULT = 0xE000020Eu;
constexpr u32 ERROR_NO_SUCH_DEVICE_INTERFACE = 0xE0000225u;
constexpr u32 ERROR_NO_COMPAT_DRIVERS = 0xE0000228u;
constexpr u32 ERROR_IN_WOW64 = 0xE0000235u;

constexpr u32 DIGCF_ALLCLASSES = 0x00000004;
constexpr u32 DIGCF_DEVICEINTERFACE = 0x00000010;
constexpr u32 SPINT_ACTIVE = 0x00000001;
constexpr u32 SPDIT_CLASSDRIVER = 1;
constexpr u32 SPDIT_COMPATDRIVER = 2;
constexpr u32 DI_CLASSINSTALLPARAMS = 0x00100000;
constexpr u32 DI_FLAGSEX_DIDINFOLIST = 0x00000010;
constexpr u32 DI_FLAGSEX_DIDCOMPATINFO = 0x00000020;
constexpr u32 DI_ENUMSINGLEINF = 0x00010000;
constexpr u32 DICD_GENERATE_ID = 0x00000001;
constexpr u32 DICD_INHERIT_CLASSDRVS = 0x00000002;
constexpr u32 DIOD_INHERIT_CLASSDRVS = 0x00000002;
constexpr u32 DIOD_CANCEL_REMOVE = 0x00000004;
constexpr u32 DIODI_NO_ADD = 0x00000001;
constexpr u32 DICS_FLAG_GLOBAL = 0x00000001;
constexpr u32 DICS_FLAG_CONFIGSPECIFIC = 0x00000002;
constexpr u32 DICS_FLAG_CONFIGGENERAL = 0x00000004;
constexpr u32 DIREG_DEV = 0x00000001;
constexpr u32 DIREG_DRV = 0x00000002;
constexpr u32 SPOST_URL = 2;
/// Upper bound on SPDRP_* ordinals (SDK SetupAPI.h; s7_setupapi_probe
/// gdrp_root_prop: 0x24 SPDRP_BASE_CONTAINERID answers, 0x25 is refused).
constexpr u32 SPDRP_MAXIMUM_PROPERTY = 0x00000025;

// DIF codes (SetupAPI.h).
constexpr u32 DIF_SELECTDEVICE = 0x01;
constexpr u32 DIF_INSTALLDEVICE = 0x02;
constexpr u32 DIF_REMOVE = 0x05;
constexpr u32 DIF_DETECT = 0x0F;
constexpr u32 DIF_INSTALLWIZARD = 0x10;
constexpr u32 DIF_DESTROYWIZARDDATA = 0x11;
constexpr u32 DIF_PROPERTYCHANGE = 0x12;
constexpr u32 DIF_ENABLECLASS = 0x13;
constexpr u32 DIF_UNREMOVE = 0x16;
constexpr u32 DIF_SELECTBESTCOMPATDRV = 0x17;
constexpr u32 DIF_REGISTERDEVICE = 0x19;
constexpr u32 DIF_NEWDEVICEWIZARD_PRESELECT = 0x1A;
constexpr u32 DIF_NEWDEVICEWIZARD_FINISHINSTALL = 0x1E;
constexpr u32 DIF_ADDPROPERTYPAGE_ADVANCED = 0x23;
constexpr u32 DIF_ADDPROPERTYPAGE_BASIC = 0x24;
constexpr u32 DIF_RESERVED1 = 0x25;
constexpr u32 DIF_TROUBLESHOOTER = 0x26;
constexpr u32 DIF_POWERMESSAGEWAKE = 0x27;
constexpr u32 DIF_ADDREMOTEPROPERTYPAGE_ADVANCED = 0x28;
constexpr u32 DIF_FINISHINSTALL_ACTION = 0x2A;
constexpr u32 DIF_RESERVED2 = 0x30;

constexpr u32 REG_SZ = 1;
constexpr u32 REG_DWORD = 4;
constexpr u32 REG_MULTI_SZ = 7;
// devpropdef.h
constexpr u32 DEVPROP_TYPE_UINT32 = 0x00000007;
constexpr u32 DEVPROP_TYPE_GUID = 0x0000000D;
constexpr u32 DEVPROP_TYPE_STRING = 0x00000012;
constexpr u32 DEVPROP_TYPE_STRING_LIST = 0x00002012;

// cfgmgr32.h DN_* status bits.
constexpr u32 DN_ROOT_ENUMERATED = 0x00000001;
constexpr u32 DN_DRIVER_LOADED = 0x00000002;
constexpr u32 DN_STARTED = 0x00000008;
constexpr u32 DN_NT_ENUMERATOR = 0x00800000;
constexpr u32 DN_NT_DRIVER = 0x01000000;
constexpr u32 DN_NO_SHOW_IN_DM = 0x40000000;

constexpr Guid GUID_DEVCLASS_DISPLAY{
    0x68, 0xE9, 0x36, 0x4D, 0x25, 0xE3, 0xCE, 0x11,
    0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18};
constexpr Guid GUID_DEVINTERFACE_DISPLAY_ADAPTER{
    0x1D, 0x20, 0x45, 0x5B, 0xF2, 0xF2, 0x3B, 0x4F,
    0x85, 0xBB, 0x30, 0xFF, 0x1F, 0x95, 0x35, 0x99};
constexpr Guid GUID_DISPLAY_DEVICE_ARRIVAL{
    0x80, 0x51, 0xA0, 0x1C, 0x99, 0xA6, 0x0A, 0x45,
    0x9A, 0x0C, 0xDE, 0x4F, 0xBE, 0x3D, 0xDD, 0x89};
constexpr Guid GUID_ZERO{};

constexpr u64 HKEY_LOCAL_MACHINE_VALUE = 0x80000002u;
constexpr char CLASS_KEY[] = "SYSTEM\\CurrentControlSet\\Control\\Class";
constexpr char ENUM_KEY[] = "SYSTEM\\CurrentControlSet\\Enum";

inline void fail(Marshaler& m, u32 error, u64 value = 0) {
    set_last_error(error);
    m.set_return(value);
}

/// Every successful SetupDi call measured sets ERROR_SUCCESS
/// (s7_setupapi_probe: *_success_gle, cdil, odi_root, gdp_instid...).
inline void succeed(Marshaler& m, u64 value = 1) {
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(value);
}

// ---- shared string and guest-memory helpers (setupapi_util.cpp) -------------

void log_unimplemented_once(const char* api, const char* what);
[[nodiscard]] char ascii_upper_char(char c);
[[nodiscard]] std::string ascii_upper(std::string_view text);
[[nodiscard]] std::string ascii_lower(std::string_view text);
[[nodiscard]] bool iequal(std::string_view a, std::string_view b);
[[nodiscard]] std::string guid_string(const Guid& g, bool upper = false);
[[nodiscard]] std::optional<Guid> parse_guid_string(std::string_view text);
[[nodiscard]] bool valid_instance_id(std::string_view id);
[[nodiscard]] std::string first_component(std::string_view id);
[[nodiscard]] std::vector<u8> utf16_bytes(std::string_view text);
[[nodiscard]] std::vector<u8> utf16_multi_bytes(const std::vector<std::string>& list);
[[nodiscard]] std::optional<std::string> read_guest_string(
    Marshaler& m, GuestAddr ptr, bool wide);
[[nodiscard]] std::optional<Guid> read_guid(Marshaler& m, GuestAddr ptr);

constexpr char ROOT_INSTANCE_ID[] = "HTREE\\ROOT\\0";

struct NodeRecord {
    std::string instance_id;
    std::string parent;                  ///< Empty for the root.
    std::optional<Guid> class_guid;
    std::string class_name;
    std::string device_desc;             ///< Empty: property absent.
    std::vector<std::string> hardware_ids;
    std::string mfg;
    std::string friendly_name;
    std::string enumerator;
    std::string driver;                  ///< Driver key under Control\Class.
    std::string container_id;
    u32 status{0};
    u32 problem{0};
    std::vector<Guid> interfaces;
};

// ---- the modelled device tree (setupapi_device_tree.cpp) ---------------------

[[nodiscard]] std::string display_hardware_id();
[[nodiscard]] std::vector<NodeRecord> device_nodes(const std::string& display_name);
[[nodiscard]] const NodeRecord* find_node(const std::vector<NodeRecord>& nodes,
                                          std::string_view id);
[[nodiscard]] std::string interface_path(std::string_view instance_id,
                                         const Guid& interface_guid,
                                         bool lower_case);
/// The default display-name provider: the injected one, or the process-wide
/// Metal device's name (used by setupapi.cpp and cfgmgr32.cpp).
std::function<std::string()> default_display_name_provider(
    std::function<std::string()> provider);

/// Process-wide DEVINST handles. cfgmgr32 hands them out in first-reference
/// order (the root is 1 only when it is referenced first: s7_cfgmgr_probe
/// locate_null dn=1 but s7_setupapi_probe's root devinst is later), and
/// CM_Locate_DevNode(CM_LOCATE_DEVNODE_NOVALIDATION) allocates one for any
/// well-formed ID (locate_missing_noval).
class DevInstTable {
public:
    static DevInstTable& instance() {
        static DevInstTable table;
        return table;
    }
    [[nodiscard]] u32 handle_for(std::string_view id) {
        std::lock_guard lock(m_mutex);
        const std::string key = ascii_upper(id);
        if (const auto it = m_by_id.find(key); it != m_by_id.end()) return it->second;
        m_ids.emplace_back(id);
        const u32 handle = static_cast<u32>(m_ids.size());
        m_by_id.emplace(key, handle);
        return handle;
    }
    [[nodiscard]] std::optional<std::string> id_for(u32 handle) {
        std::lock_guard lock(m_mutex);
        if (handle == 0 || handle > m_ids.size()) return std::nullopt;
        return m_ids[handle - 1];
    }

private:
    std::mutex m_mutex;
    std::vector<std::string> m_ids;
    std::unordered_map<std::string, u32> m_by_id;
};

// ---- device properties ---------------------------------------------------------

enum class Prop {
    DeviceDesc, HardwareIds, CompatibleIds, Service, Class, ClassGuid, Driver,
    ConfigFlags, Mfg, FriendlyName, EnumeratorName, InstanceId, Parent,
    Children, DevNodeStatus, ProblemCode, BaseContainerId,
};

/// One property value, kept typed so each API encodes it its own way.
struct PropValue {
    enum class Kind { String, List, Dword, GuidValue } kind{Kind::String};
    std::string text;
    std::vector<std::string> list;
    u32 dword{0};
    Guid guid{};
};

// ---- device properties and registry helpers (setupapi_device_tree.cpp) ------

[[nodiscard]] std::optional<PropValue> string_prop(const std::string& text);
[[nodiscard]] std::optional<PropValue> node_property(
    const std::vector<NodeRecord>& nodes, const NodeRecord& node, Prop prop);
[[nodiscard]] std::optional<Prop> spdrp_property(u32 spdrp);
[[nodiscard]] std::optional<Prop> devpkey_property(const Guid& fmtid, u32 pid);
[[nodiscard]] std::pair<u32, std::vector<u8>> registry_encoding(
    const PropValue& v, bool wide);
[[nodiscard]] std::pair<u32, std::vector<u8>> devprop_encoding(const PropValue& v);
[[nodiscard]] std::wstring widen(std::string_view text);
[[nodiscard]] bool wants_write_access(u32 sam);
[[nodiscard]] bool registry_key_exists(RegistryThunks* registry, const std::string& path);
[[nodiscard]] u64 open_registry_key(RegistryThunks* registry, const std::string& path,
                                    u32 sam);
void set_registry_value(RegistryThunks* registry, u64 key, const wchar_t* name,
                        u32 type, const std::vector<u8>& data);
void ensure_device_hardware_key(RegistryThunks* registry, const NodeRecord& node);

/// An installed device setup class: the "Class" value of
/// HKLM\SYSTEM\CurrentControlSet\Control\Class\{guid}, in registry key order.
struct SetupClass {
    const char* name;
    std::array<u8, 16> guid;
};

constexpr u32 ERROR_INVALID_CLASS = 0xE0000206u;        // SetupAPI.h

// ---- setup classes (setupapi_classes.cpp) ------------------------------------

[[nodiscard]] const SetupClass* find_setup_class(const Guid& guid);
void ensure_class_key(RegistryThunks* registry, const SetupClass& setup_class);
void class_guids_from_name(Marshaler& m, bool wide, bool ex);
void class_name_from_guid(Marshaler& m, bool wide, bool ex);

struct QueuedCopy {
    std::string source_file;
    std::string source_path_root;
    std::string destination_name;
    u32 copy_style{0};
};

struct DriverEntry {
    u64 token{0};
    std::string description;
    std::string hardware_id;
};

struct SetElement {
    u64 token{0};
    std::string instance_id;
    Guid class_guid{};
    /// Created by SetupDiCreateDeviceInfo and never registered: it lives only
    /// in this set, with its own properties.
    bool created{false};
    std::map<u32, std::vector<u8>> created_props;   ///< SPDRP -> REG bytes (UTF-16)
    std::vector<u8> install_params;                 ///< SP_DEVINSTALL_PARAMS_W image
    std::vector<u8> class_install_params;
    std::optional<std::vector<DriverEntry>> class_drivers;
    std::optional<std::vector<DriverEntry>> compat_drivers;
};

struct SetInterface {
    u64 token{0};
    Guid guid{};
    usize element{0};
    std::string path;       ///< SetupDi (lower-case) symbolic link
};

struct DevInfoSet {
    bool has_class{false};
    Guid class_guid{};
    std::vector<SetElement> elements;
    std::vector<SetInterface> interfaces;
    std::vector<u8> install_params;
    std::vector<u8> class_install_params;
    std::optional<std::vector<DriverEntry>> class_drivers;
};

/// SetupInitDefaultQueueCallback[Ex] state. Vitreo shows no setup progress
/// or prompt dialogs, so every context behaves as one created with
/// AlternateProgressWindow = INVALID_HANDLE_VALUE (measured: the no-UI rules
/// in setupapi_inf_probe*.expected).
struct DefaultQueueContext {
    u64 owner = 0;
    u64 progress_window = 0;
    bool wants_ui = false;
};

struct OpenInf;

struct SetupapiState {
    std::function<std::string()> display_name_provider;
    capsule::Vfs* vfs{nullptr};
    RegistryThunks* registry{nullptr};
    const GuestElevationState* elevation{nullptr};
    std::mutex mutex;                           ///< Guards sets and next_set.
    std::unordered_map<u64, DevInfoSet> sets;
    u32 next_set{0x5E700000u};
    std::unordered_map<u32, std::vector<QueuedCopy>> file_queues;
    u32 next_file_queue{0x5E7A1001u};
    const SetupapiGuestServices* services{nullptr};
    /// Open HINFs (setupapi_inf_install.cpp), keyed by the handle value.
    std::unordered_map<u64, std::shared_ptr<OpenInf>> infs;
    u32 next_inf{0x5E7B0001u};
    /// SetupInitDefaultQueueCallback[Ex] contexts.
    std::unordered_map<u64, DefaultQueueContext> default_contexts;
    u32 next_default_context{0x5E7C0001u};
    /// Guest ring for callback records; reserved on first use.
    GuestAddr scratch_base{0};
    u32 scratch_cursor{0};

    [[nodiscard]] std::string display_name() const {
        return display_name_provider ? display_name_provider() : std::string{};
    }
    [[nodiscard]] std::vector<NodeRecord> nodes() const {
        return device_nodes(display_name());
    }
    [[nodiscard]] bool elevated() const {
        return elevation != nullptr && elevation->is_elevated();
    }
    [[nodiscard]] DevInfoSet* find_set(u64 handle) {
        const auto it = sets.find(handle);
        return it == sets.end() ? nullptr : &it->second;
    }
    [[nodiscard]] u64 create_set() {
        next_set += 4;
        sets.emplace(next_set, DevInfoSet{});
        return next_set;
    }
};

using StatePtr = std::shared_ptr<SetupapiState>;

struct Layout {
    bool is64;
    u32 devinfo;      ///< sizeof(SP_DEVINFO_DATA)
    u32 iface;        ///< sizeof(SP_DEVICE_INTERFACE_DATA)
};

[[nodiscard]] inline Layout layout(const Marshaler& m) {
    const bool is64 = m.cpu_is_64bit();
    return {is64, is64 ? 32u : 28u, is64 ? 32u : 28u};
}

// ---- device information sets (setupapi_device_info_set.cpp) ------------------

[[nodiscard]] u64 next_token();
[[nodiscard]] std::optional<usize> resolve_element(Marshaler& m, const DevInfoSet& set,
                                                   GuestAddr did);
[[nodiscard]] std::optional<usize> resolve_interface(Marshaler& m, const DevInfoSet& set,
                                                     GuestAddr data);
[[nodiscard]] bool write_devinfo(Marshaler& m, GuestAddr did, const SetElement& e);
[[nodiscard]] bool write_interface_data(Marshaler& m, GuestAddr data, const SetInterface& i);
usize add_node_element(DevInfoSet& set, const NodeRecord& node);
usize add_interface(DevInfoSet& set, usize element, const Guid& guid,
                    const std::string& instance_id);
[[nodiscard]] const NodeRecord* element_node(const std::vector<NodeRecord>& nodes,
                                             const SetElement& e);
[[nodiscard]] std::optional<PropValue> element_property(
    const std::vector<NodeRecord>& nodes, const SetElement& e, Prop prop);

// ---- SetupDi / SetupCopy handlers registered by create_setupapi_thunks ------

// setupapi_device_enum.cpp
void get_class_devs(Marshaler& m, const StatePtr& state, bool wide, bool ex);
void enum_device_info(Marshaler& m, const StatePtr& state);
void enum_device_interfaces(Marshaler& m, const StatePtr& state);
void get_device_interface_detail(Marshaler& m, const StatePtr& state, bool wide);
void destroy_device_info_list(Marshaler& m, const StatePtr& state);
void create_device_info_list(Marshaler& m, const StatePtr& state, bool ex, bool wide);
void open_device_info(Marshaler& m, const StatePtr& state, bool wide);
void open_device_interface(Marshaler& m, const StatePtr& state, bool wide);

// setupapi_device_properties.cpp
void get_registry_property(Marshaler& m, const StatePtr& state, bool wide);
void get_device_property(Marshaler& m, const StatePtr& state);
void get_instance_id(Marshaler& m, const StatePtr& state, bool wide);
void open_dev_reg_key(Marshaler& m, const StatePtr& state);
void open_class_reg_key(Marshaler& m, const StatePtr& state);
void open_interface_reg_key(Marshaler& m, const StatePtr& state);

// setupapi_install_params.cpp
[[nodiscard]] u32 devinstall_size(const Marshaler& m);
[[nodiscard]] std::vector<u8>& install_params_of(DevInfoSet& set, SetElement* e,
                                                 u32 size);
void update_u32(std::vector<u8>& bytes, usize offset, u32 set_bits, u32 clear_bits);
void get_device_install_params(Marshaler& m, const StatePtr& state);
void set_device_install_params(Marshaler& m, const StatePtr& state);
void set_class_install_params(Marshaler& m, const StatePtr& state);
void get_class_install_params(Marshaler& m, const StatePtr& state);

// setupapi_driver_info.cpp
void build_driver_info_list(Marshaler& m, const StatePtr& state);
void enum_driver_info(Marshaler& m, const StatePtr& state, bool wide);
void destroy_driver_info_list(Marshaler& m, const StatePtr& state);
void get_driver_info_detail(Marshaler& m, const StatePtr& state);

// setupapi_device_install.cpp
void create_device_info(Marshaler& m, const StatePtr& state);
void set_registry_property(Marshaler& m, const StatePtr& state);
void set_registry_property_a(Marshaler& m, const StatePtr& state);
void call_class_installer(Marshaler& m, const StatePtr& state);
void copy_oem_inf(Marshaler& m, const StatePtr& state);
void copy_oem_inf_a(Marshaler& m, const StatePtr& state);
void copy_oem_inf_impl(Marshaler& m, const StatePtr& state, bool wide);
void inf_driver_store_location(Marshaler& m, const StatePtr& state);

// setupapi_file_copy.cpp: shared with the SetupQueueCopy lambda in setupapi.cpp.
[[nodiscard]] std::string join_win_path(const std::string& directory,
                                        const std::string& name);
[[nodiscard]] bool vfs_read_all(capsule::Vfs& vfs, const std::string& path,
                                std::vector<u8>& bytes);
[[nodiscard]] bool vfs_write_all(capsule::Vfs& vfs, const std::string& path,
                                 std::span<const u8> bytes);
[[nodiscard]] bool vfs_is_file(capsule::Vfs& vfs, const std::string& path);

// setupapi_inf_install.cpp: the INF engine (SetupOpenInfFile ...
// SetupInstallFromInfSection), the file-queue commit, the default queue
// callback and SetupIterateCabinet.
void add_inf_exports(DllExports& exports, const StatePtr& state);

} // namespace vitreo::winapi::setupapi_internal
