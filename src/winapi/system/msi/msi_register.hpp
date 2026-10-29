#pragma once
/// @file msi_register.hpp
/// @brief Windows Installer registration area: the installed-products
/// database (what Windows Installer records about every product it installs,
/// advertises or removes) and the registration actions that write it.
///
/// Layout (measured on Windows 11 by tools/oracle_probe/msi_registration_run.ps1
/// with the package msi_registration_build.ps1 authors; recorded in
/// msi_registration_*.expected and .docs/contracts/msi/registration.md).
/// <P> is the packed product code, <C> a packed component code, <U> the packed
/// upgrade code, <SID> "S-1-5-18" for a per-machine instance or the user's SID.
///
///   advertised root (per context):
///     machine         HKLM\SOFTWARE\Classes\Installer
///     user unmanaged  HKCU\Software\Microsoft\Installer
///     user managed    HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\Managed\<SID>\Installer
///   <root>\Products\<P>                 ProductName, PackageCode (packed), Language,
///                                       Version, Assignment, AdvertiseFlags, InstanceType,
///                                       AuthorizedLUAApp, DeploymentFlags, Clients
///   <root>\Products\<P>\SourceList      PackageName, LastUsedSource "n;1;<path>",
///                                       Net\1.., URL\1.., Media\<disk> "label;prompt"
///   <root>\Features\<P>                 <feature> = parent name, prefixed "\x06" when absent
///   <root>\Components\<packed category> <qualifier> = REG_MULTI_SZ { descriptor + AppData }
///   <root>\UpgradeCodes\<U>             <P> = ""
///   HKLM\...\Installer\UpgradeCodes\<U> <P> = ""   (both contexts)
///   HKLM\...\Installer\UserData\<SID>\Products\<P>\InstallProperties   (ARP values, LocalPackage, ...)
///                                     \Features   <feature> = compressed component ids [+ "\x02" parent]
///                                     \Patches    AllPatches = ""
///   HKLM\...\Installer\UserData\<SID>\Components\<C>   <P> = key path
///   HKLM\SOFTWARE[\WOW6432Node]\Microsoft\Windows\CurrentVersion\Uninstall\<ProductCode>
///                                     the ARP entry (WOW6432Node for a 32-bit package)
///
/// The registry is the capsule registry, addressed by its physical 64-bit
/// Windows layout (WOW6432Node spelled out), the layout com_activation reads.

#include <vitreo/common.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::capsule { class Registry; }
namespace vitreo::winapi { class MsiManager; }

namespace vitreo::winapi::msi {

class Session;

// ---- Formats -------------------------------------------------------------------------

/// MSIINSTALLCONTEXT (Msi.h).
enum : u32 {
    kContextNone = 0,
    kContextUserManaged = 1,
    kContextUserUnmanaged = 2,
    kContextMachine = 4,
    kContextAll = 7,
    kContextAllUserManaged = 8,
};

/// SID that names "every user" in the *Ex functions (Learn: szUserSid "s-1-1-0").
inline constexpr const char* kEveryoneSid = "S-1-1-0";
/// UserData key of per-machine instances.
inline constexpr const char* kMachineSid = "S-1-5-18";

/// The packed GUID Windows Installer uses as a registry key name: the 32 hex
/// digits, upper case, with the first three fields reversed nibble by nibble
/// and each remaining byte's two nibbles swapped (measured: product
/// {EA844198-B6C8-4E38-9ABA-6BA7E226E338} -> 891448AE8C6B83E4A9ABB67A2E623E83).
/// Empty when @p guid is not a registry-format GUID (either case accepted).
[[nodiscard]] std::string pack_guid(std::string_view guid);
/// Inverse of pack_guid ("{...}" upper case); empty when @p packed is not 32 hex digits.
[[nodiscard]] std::string unpack_guid(std::string_view packed);

/// One registered instance of a product: the product in one context (and,
/// for the user contexts, one user's SID; empty for the machine context).
struct ProductInstance {
    std::string product_code;  ///< "{...}" upper case
    u32 context{kContextNone};
    std::string sid;
    friend bool operator==(const ProductInstance&, const ProductInstance&) = default;
};


// ---- The installed-products database ------------------------------------------------------

class ProductDatabase {
public:
    /// @p user_sid is the calling user's SID (the user contexts are its own).
    ProductDatabase(capsule::Registry& registry, std::string user_sid);

    [[nodiscard]] capsule::Registry& registry() { return m_registry; }
    [[nodiscard]] const std::string& user_sid() const { return m_user_sid; }

    // ---- lookups ----

    /// Every instance in @p contexts (MSIINSTALLCONTEXT bits) whose SID matches
    /// @p sid ("" = the calling user, kEveryoneSid = all users; the machine
    /// context ignores it), optionally only of @p product. Order: managed,
    /// unmanaged, machine; within a context by packed code (registry order).
    [[nodiscard]] std::vector<ProductInstance> instances(u32 contexts, std::string_view sid = {},
                                                         std::string_view product = {}) const;
    /// True when a Products key in @p contexts is not a packed GUID: the
    /// enumerations end with ERROR_BAD_CONFIGURATION (probe enum-malformed).
    [[nodiscard]] bool malformed_products(u32 contexts, std::string_view sid = {}) const;
    /// The instance of @p product the calling user sees (managed, then
    /// unmanaged, then machine), or nullopt.
    [[nodiscard]] std::optional<ProductInstance> visible(std::string_view product) const;
    /// Same, but also an instance installed for this user / machine that is
    /// not published (state ABSENT), for maintenance-mode lookups.
    [[nodiscard]] std::optional<ProductInstance> installed(std::string_view product) const;
    /// Installed state of one instance (msi_product_state_probe.c): DEFAULT
    /// when published and installed (InstallProperties carries
    /// WindowsInstaller), ADVERTISED when published only, ABSENT when
    /// installed but not published, UNKNOWN otherwise.
    [[nodiscard]] i32 instance_state(const ProductInstance& instance) const;
    /// MsiQueryProductState for the calling user.
    [[nodiscard]] i32 product_state(std::string_view product) const;

    /// A value of InstallProperties as text (REG_DWORD in decimal).
    [[nodiscard]] std::optional<std::string> install_property(const ProductInstance& instance,
                                                              std::string_view name) const;
    /// A value of the advertised Products\<P> key (REG_DWORD in decimal,
    /// REG_MULTI_SZ as its items, each followed by NUL).
    [[nodiscard]] std::optional<std::string> product_key_value(const ProductInstance& instance,
                                                               std::string_view name) const;
    /// A value of Products\<P>\SourceList[\sub].
    [[nodiscard]] std::optional<std::string> source_list_value(const ProductInstance& instance,
                                                               std::string_view sub, std::string_view name) const;
    /// The cached package (InstallProperties\LocalPackage), or empty.
    [[nodiscard]] std::string local_package(const ProductInstance& instance) const;

    /// MsiGetProductInfo(Ex) for one property name (case-sensitive; Msi.h
    /// INSTALLPROPERTY_*). ERROR_SUCCESS with @p out, or
    /// ERROR_UNKNOWN_PROPERTY. Never fails for an unset known property: it is "".
    u32 product_info(const ProductInstance& instance, std::string_view property, std::string& out,
                     bool extended) const;

    /// Published features in registry order: (feature, parent).
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> features(const ProductInstance& instance) const;
    /// MsiQueryFeatureState for @p feature of @p instance; INSTALLSTATE_UNKNOWN
    /// when the product does not publish it.
    [[nodiscard]] i32 feature_state(const ProductInstance& instance, std::string_view feature) const;
    /// The component codes the installed Features value lists for @p feature.
    [[nodiscard]] std::vector<std::string> feature_component_codes(const ProductInstance& instance,
                                                                   std::string_view feature) const;
    /// Every feature's installed state (MigrateFeatureStates).
    [[nodiscard]] std::map<std::string, i32> feature_states(const ProductInstance& instance) const;

    /// The key path a product registered for a component (UserData\<SID>\Components\<C>\<P>).
    [[nodiscard]] std::optional<std::string> component_key_path(const ProductInstance& instance,
                                                                std::string_view component) const;
    /// INSTALLSTATE of a registered key path: LOCAL for a path / "NN:" registry
    /// key path, SOURCE for a "NN" source form, ABSENT for an empty one,
    /// UNKNOWN when not registered.
    [[nodiscard]] i32 component_state(const ProductInstance& instance, std::string_view component) const;
    /// Clients of a component: (instance, key path), in registry order, over
    /// @p contexts / @p sid as instances().
    [[nodiscard]] std::vector<std::pair<ProductInstance, std::string>> component_clients(
        std::string_view component, u32 contexts, std::string_view sid = {}) const;
    /// Every registered component code in @p contexts / @p sid, with the
    /// context and SID it is registered under, registry order.
    struct RegisteredComponent {
        std::string component;
        u32 context{kContextNone};
        std::string sid;
    };
    [[nodiscard]] std::vector<RegisteredComponent> components(u32 contexts, std::string_view sid = {}) const;

    /// Installed products carrying @p upgrade_code (Installer\UpgradeCodes), registry order.
    [[nodiscard]] std::vector<std::string> related_products(std::string_view upgrade_code) const;
    /// The product whose PackageCode is @p package_code, or empty.
    [[nodiscard]] std::string product_from_package_code(std::string_view package_code) const;

    // ---- writes (the registration actions' execution-script operations) ----

    /// InstallProperties + the ARP Uninstall entry (RegisterProduct).
    void register_product(const ProductInstance& instance, const std::map<std::string, std::string>& strings,
                          const std::map<std::string, u32>& dwords, const std::map<std::string, std::string>& expand_strings,
                          bool package_64bit);
    void unregister_product(const ProductInstance& instance, std::string_view upgrade_code, bool package_64bit);
    /// RegisterUser: RegOwner / RegCompany / ProductID in InstallProperties.
    void register_user(const ProductInstance& instance, std::string_view owner, std::string_view company,
                       std::string_view product_id);

    struct Publication {
        std::string product_name;
        std::string package_code;  ///< "{...}"
        u32 language{0};
        u32 version{0};
        u32 assignment{0};  ///< 1 machine, 0 user
        std::string upgrade_code;
        std::string package_name;
        std::string last_used_source;  ///< directory with trailing '\'
        std::vector<std::string> net_sources;
        std::vector<std::string> url_sources;
        std::vector<std::pair<u32, std::string>> media;  ///< disk id -> "label;prompt"
        std::string media_package_path;  ///< "" when not set
        std::string disk_prompt;         ///< "" when not set
        std::string product_icon;        ///< "" when the product has no ARPPRODUCTICON
    };
    /// PublishProduct: Products\<P> (+ SourceList) and the upgrade-code mappings.
    void publish_product(const ProductInstance& instance, const Publication& publication);
    void unpublish_product(const ProductInstance& instance, std::string_view upgrade_code);

    /// PublishFeatures: one feature. @p components are component codes.
    void publish_feature(const ProductInstance& instance, std::string_view feature, std::string_view parent,
                         bool absent, const std::vector<std::string>& components);
    void unpublish_feature(const ProductInstance& instance, std::string_view feature);

    /// ProcessComponents: register / unregister a product's key path for a component.
    void register_component(const ProductInstance& instance, std::string_view component, std::string_view key_path);
    void unregister_component(const ProductInstance& instance, std::string_view component);

    /// PublishComponents: a qualified component (category, qualifier) -> descriptor + AppData.
    void publish_qualified_component(const ProductInstance& instance, std::string_view category,
                                     std::string_view qualifier, std::string_view descriptor,
                                     std::string_view app_data);
    void unpublish_qualified_component(const ProductInstance& instance, std::string_view category,
                                       std::string_view qualifier, std::string_view descriptor);
    /// Qualifiers of a category visible to the calling user: (qualifier, descriptor, AppData).
    struct Qualifier {
        std::string qualifier;
        std::string descriptor;
        std::string app_data;
    };
    [[nodiscard]] std::vector<Qualifier> qualifiers(std::string_view category) const;

    /// MsiUseFeature(Ex) bookkeeping: Usage\<feature> = (count | day << 16).
    void record_feature_use(const ProductInstance& instance, std::string_view feature);
    /// (use count, MSI date word) from Usage\<feature>; (0, 0) when never used.
    [[nodiscard]] std::pair<u32, u16> feature_usage(const ProductInstance& instance, std::string_view feature) const;

    /// Source list edits (MsiSourceListAddSource(Ex) / ClearSource / ClearAll / SetInfo / media disks).
    void set_source_list_value(const ProductInstance& instance, std::string_view sub, std::string_view name,
                               std::string_view value, bool expand);
    void delete_source_list_value(const ProductInstance& instance, std::string_view sub, std::string_view name);
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> source_list_values(const ProductInstance& instance,
                                                                                      std::string_view sub) const;

    /// Registry location of an instance's data (for diagnostics and tests).
    struct Keys {
        u64 advertised_hive{0};
        std::wstring advertised_root;  ///< "...\Installer" (Products/Features/Components/UpgradeCodes beneath)
        std::wstring user_data;        ///< "SOFTWARE\...\Installer\UserData\<SID>"
    };
    [[nodiscard]] Keys keys(u32 context, std::string_view sid) const;

private:
    std::reference_wrapper<capsule::Registry> m_registry;
    std::string m_user_sid;
};

/// The product database the msi.dll exports and the actions use: the
/// manager's capsule registry and user SID. nullopt when no registry is wired.
[[nodiscard]] std::optional<ProductDatabase> product_database(MsiManager& manager);

/// Installed-state answers for the features area (MigrateFeatureStates,
/// maintenance installs, FindRelatedProducts), registered by
/// register_registration_actions through set_installed_state_provider.
void install_registration_state_provider();

}  // namespace vitreo::winapi::msi
