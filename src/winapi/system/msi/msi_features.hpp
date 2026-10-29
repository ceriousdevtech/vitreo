#pragma once
/// @file msi_features.hpp
/// @brief Windows Installer features area: directory resolution and target
/// paths, the feature/component selection state machine, and costing.
///
/// What other areas consume (all keyed by the install Session):
///   * component action / request / installed state (INSTALLSTATE_* of Msi.h),
///     its resolved directory, KeyPath, 64-bit flag and attributes — the
///     files, search, registration and custom-action areas decide what to do
///     for a component from `ComponentInfo::action` alone;
///   * feature installed / request / action states — publication (registration
///     area), SelectionTree and AddLocal/Remove control events (ui area);
///   * resolved target and source directory paths (`target_path`,
///     `source_path`), and `set_target_path` (MsiSetTargetPath, the
///     SetTargetPath control event, custom action type 35);
///   * `recompute_states(session)` after any selection change, and the
///     per-volume costs the VolumeCostList / SelectionSize UI shows.
///
/// Lifecycle (Learn: CostInitialize / FileCost / CostFinalize actions):
///   CostInitialize loads the Feature, Component, FeatureComponents and
///   Directory tables into a FeatureModel (Session::state<FeatureModel>()),
///   FileCost computes per-component file costs, CostFinalize resolves every
///   directory (setting each Directory key as a property), evaluates the
///   Condition table and component conditions, and selects features from
///   INSTALLLEVEL and ADDLOCAL/REMOVE/ADDSOURCE/ADDDEFAULT/REINSTALL/
///   ADVERTISE/COMPADD*/FILEADD*. After CostFinalize, `recompute_states`
///   re-derives component action states from feature requests.
///
/// Engine integration (the engine area owns Session and the condition
/// grammar):
///   * The `$comp ?comp &feat !feat` condition operators read
///     `component_action_state`, `component_installed_state`,
///     `feature_action_state`, `feature_installed_state`; each returns
///     nullopt for an unknown name (the operators then compare against an
///     absent value).
///   * `[$comp]` / `[#file]` formatting reads `component_directory` /
///     `ComponentInfo::action` and `target_path`.
///   * Session::directory_path(key) should return `target_path(session, key)`
///     once CostFinalize ran (`FeatureModel::directories_resolved`): this area's
///     resolution is the one that honours SetTargetPath and the
///     TARGETDIR/ROOTDRIVE/SourceDir rules.
///
/// Installed state (maintenance installs, MigrateFeatureStates) comes from
/// the registration area through `InstalledStateProvider`; when none is
/// registered, a product is treated as not installed (every installed state
/// INSTALLSTATE_ABSENT for a product absent from the machine, as Windows
/// reports for a first-time install).
///
/// Contract sources: Msi.h / MsiQuery.h / MsiDefs.h (SDK 10.0.26100), Learn
/// (Feature, Component, FeatureComponents, Condition, Directory tables;
/// CostInitialize, FileCost, CostFinalize, IsolateComponents,
/// MigrateFeatureStates actions; ADDLOCAL..FILEADDDEFAULT, INSTALLLEVEL,
/// Preselected, TARGETDIR, ROOTDRIVE, SourceDir properties; the Msi*Feature*,
/// Msi*Component*, Msi*Path, MsiSetInstallLevel, MsiVerifyDiskSpace,
/// MsiEnumComponentCosts functions) and tools/oracle_probe/msi_features_*
/// (measured on Windows 11). Contract notes: .docs/contracts/msi/features.md.

#include <vitreo/common.hpp>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace vitreo::winapi::msi {

class Session;
class ActionContext;

// INSTALLSTATE (Msi.h). Mirrors the values in msi.hpp so this header stands
// alone for areas that do not include msi.hpp.
namespace install_state {
inline constexpr i32 kNotUsed = -7;
inline constexpr i32 kBadConfig = -6;
inline constexpr i32 kIncomplete = -5;
inline constexpr i32 kSourceAbsent = -4;
inline constexpr i32 kMoreData = -3;
inline constexpr i32 kInvalidArg = -2;
inline constexpr i32 kUnknown = -1;
inline constexpr i32 kBroken = 0;
inline constexpr i32 kAdvertised = 1;
inline constexpr i32 kRemoved = 1;  ///< INSTALLSTATE_REMOVED: component being removed (action state)
inline constexpr i32 kAbsent = 2;
inline constexpr i32 kLocal = 3;
inline constexpr i32 kSource = 4;
inline constexpr i32 kDefault = 5;
}  // namespace install_state

// msidbFeatureAttributes (MsiDefs.h).
enum : u32 {
    kFeatureFavorLocal = 0x0000,
    kFeatureFavorSource = 0x0001,
    kFeatureFollowParent = 0x0002,
    kFeatureFavorAdvertise = 0x0004,
    kFeatureDisallowAdvertise = 0x0008,
    kFeatureUIDisallowAbsent = 0x0010,
    kFeatureNoUnsupportedAdvertise = 0x0020,
};

// msidbComponentAttributes (MsiDefs.h).
enum : u32 {
    kComponentLocalOnly = 0x0000,
    kComponentSourceOnly = 0x0001,
    kComponentOptional = 0x0002,
    kComponentRegistryKeyPath = 0x0004,
    kComponentSharedDllRefCount = 0x0008,
    kComponentPermanent = 0x0010,
    kComponentODBCDataSource = 0x0020,
    kComponentTransitive = 0x0040,
    kComponentNeverOverwrite = 0x0080,
    kComponent64bit = 0x0100,
    kComponentDisableRegistryReflection = 0x0200,
    kComponentUninstallOnSupersedence = 0x0400,
    kComponentShared = 0x0800,
};

struct FeatureInfo {
    std::string name;
    std::string parent;  ///< empty for a root
    std::string title;
    std::string description;
    std::string directory;  ///< Directory_ (configurable folder), may be empty
    i32 display{0};
    i32 authored_level{0};  ///< Feature.Level as authored
    i32 level{0};           ///< after the Condition table
    u32 attributes{0};      ///< Feature.Attributes, then MsiSetFeatureAttributes
    i32 installed{install_state::kUnknown};
    i32 request{install_state::kUnknown};  ///< selection (Request: in the log)
    i32 action{install_state::kUnknown};
    std::vector<std::string> children;    ///< in Feature-table order
    std::vector<std::string> components;  ///< FeatureComponents order
};

struct ComponentInfo {
    std::string name;
    std::string component_id;  ///< ComponentId GUID; empty = not registered
    std::string directory;     ///< Directory_ key
    std::string condition;
    std::string key_path;  ///< KeyPath (File / Registry / ODBCDataSource key), empty = the directory
    u32 attributes{0};
    bool enabled{true};  ///< Condition true (or null) at CostFinalize / MsiSetComponentState
    i32 installed{install_state::kUnknown};
    i32 request{install_state::kUnknown};
    i32 action{install_state::kUnknown};
    std::vector<std::string> features;  ///< features that link this component
    /// Directory of the key file when IsolateComponents redirected the
    /// component's files into an application component's folder; empty
    /// otherwise. The files area installs the component's files there too.
    std::vector<std::string> isolated_into;  ///< Component_Application names
    // Costs in bytes, per FileCost (cluster-rounded per the target volume).
    i64 local_cost{0};
    i64 source_cost{0};
    i64 remove_cost{0};
    [[nodiscard]] bool is_64bit() const { return (attributes & kComponent64bit) != 0; }
    [[nodiscard]] bool registry_key_path() const { return (attributes & kComponentRegistryKeyPath) != 0; }
    [[nodiscard]] bool odbc_key_path() const { return (attributes & kComponentODBCDataSource) != 0; }
};

struct DirectoryInfo {
    std::string key;
    std::string parent;      ///< empty for a root
    std::string default_dir;  ///< raw DefaultDir cell
    std::string target;      ///< resolved target path, trailing '\'
    std::string source;      ///< resolved source path, trailing '\'
    bool target_overridden{false};  ///< MsiSetTargetPath set this one explicitly
};

/// Per-session state of this area (Session::state<FeatureModel>()).
struct FeatureModel {
    bool loaded{false};               ///< CostInitialize ran
    bool file_costed{false};          ///< FileCost ran
    bool finalized{false};            ///< CostFinalize ran
    bool directories_resolved{false};
    i32 install_level{1};
    std::vector<std::string> feature_order;    ///< Feature-table order
    std::vector<std::string> component_order;  ///< Component-table order
    std::vector<std::string> directory_order;  ///< Directory-table order
    std::map<std::string, FeatureInfo> features;
    std::map<std::string, ComponentInfo> components;
    std::map<std::string, DirectoryInfo> directories;
    std::string target_root;  ///< key of the root directory (TARGETDIR)
};

/// Installed state of the product a session is installing, as recorded on
/// the machine (registration area). `product_code` is the session's
/// ProductCode property.
struct InstalledStateProvider {
    /// Installed state of a feature of the product (MsiQueryFeatureState).
    std::function<i32(Session&, const std::string& product_code, const std::string& feature)> feature;
    /// Installed state of a component of the product by ComponentId
    /// (MsiGetComponentPath for that product: local / source / absent).
    std::function<i32(Session&, const std::string& product_code, const std::string& component_id)> component;
    /// Feature states of another installed product (MigrateFeatureStates):
    /// feature name -> installed state. Empty when unknown.
    std::function<std::map<std::string, i32>(Session&, const std::string& product_code)> product_features;
    /// Installed products with @p upgrade_code (FindRelatedProducts / Upgrade
    /// table), as product codes.
    std::function<std::vector<std::string>(Session&, const std::string& upgrade_code)> related_products;
};
/// Registered once by the registration area (msi_register.cpp).
void set_installed_state_provider(InstalledStateProvider provider);

// ---- queries ------------------------------------------------------------------

[[nodiscard]] FeatureModel& feature_model(Session& session);
[[nodiscard]] const FeatureInfo* find_feature(Session& session, const std::string& name);
[[nodiscard]] const ComponentInfo* find_component(Session& session, const std::string& name);

/// Condition-operator views ($ ? & !): nullopt for an unknown name.
[[nodiscard]] std::optional<i32> feature_installed_state(Session& session, const std::string& name);
[[nodiscard]] std::optional<i32> feature_action_state(Session& session, const std::string& name);
[[nodiscard]] std::optional<i32> component_installed_state(Session& session, const std::string& name);
[[nodiscard]] std::optional<i32> component_action_state(Session& session, const std::string& name);

/// Resolved target path of a Directory key (trailing '\'); empty when the key
/// is unknown or directories are not resolved yet.
[[nodiscard]] std::string target_path(Session& session, const std::string& directory);
/// Resolved source path of a Directory key (trailing '\').
[[nodiscard]] std::string source_path(Session& session, const std::string& directory);
/// Target directory of a component's files ([$comp] for a local component).
[[nodiscard]] std::string component_directory(Session& session, const std::string& component);

// ---- changes (each returns a Windows error code) -------------------------------

/// MsiSetTargetPath: set @p directory and every directory beneath it, and
/// their properties; on failure nothing changes.
u32 set_target_path(Session& session, const std::string& directory, const std::string& path);
/// MsiSetFeatureState: request @p state for a feature (and the tree rules).
u32 set_feature_state(Session& session, const std::string& feature, i32 state);
/// MsiSetComponentState.
u32 set_component_state(Session& session, const std::string& component, i32 state);
/// MsiSetInstallLevel: <=0 keeps the level but re-selects.
u32 set_install_level(Session& session, i32 level);
/// MsiSetFeatureAttributes (INSTALLFEATUREATTRIBUTE_* bits).
u32 set_feature_attributes(Session& session, const std::string& feature, u32 install_feature_attributes);
/// Re-derive feature action states and component request/action states from
/// the current feature requests (after the UI or a custom action changed a
/// request), and refresh the disk-space properties.
void recompute_states(Session& session);
/// MsiGetFeatureValidStates bit mask (1 << INSTALLSTATE_*).
[[nodiscard]] u32 feature_valid_states(Session& session, const std::string& feature);

// ---- costing -------------------------------------------------------------------

struct VolumeCost {
    std::string volume;   ///< "C:" (drive letter and colon, as MsiEnumComponentCosts returns)
    i64 cost{0};          ///< final bytes
    i64 temp_cost{0};     ///< temporary bytes during the install
};
/// MsiGetFeatureCost in bytes (callers convert to 512-byte units).
/// tree: 0 = MSICOSTTREE_SELFONLY, 1 = CHILDREN, 2 = PARENTS.
[[nodiscard]] i64 feature_cost(Session& session, const std::string& feature, i32 tree, i32 state);
/// Per-volume costs of one component in @p state, or of the whole install
/// when @p component is empty.
[[nodiscard]] std::vector<VolumeCost> component_costs(Session& session, const std::string& component, i32 state);

// ---- actions (registered by register_feature_actions) ----------------------------
u32 action_cost_initialize(ActionContext& ctx);
u32 action_file_cost(ActionContext& ctx);
u32 action_cost_finalize(ActionContext& ctx);
u32 action_isolate_components(ActionContext& ctx);
u32 action_migrate_feature_states(ActionContext& ctx);

}  // namespace vitreo::winapi::msi
