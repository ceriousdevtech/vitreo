#pragma once
/// @file msi_register_internal.hpp
/// @brief Helpers the registration-area action files share (not for other areas).

#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi_register.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi::msi {

class Session;
struct Table;
}  // namespace vitreo::winapi::msi
namespace vitreo::winapi { class MsiManager; }
namespace vitreo::winapi::msi {

namespace reg_detail {

[[nodiscard]] std::wstring widen(std::string_view utf8);
[[nodiscard]] std::string narrow(std::wstring_view wide);
[[nodiscard]] std::string upper(std::string_view s);

/// The product instance an install session registers: per-machine when
/// ALLUSERS is set (the engine resolves ALLUSERS=2 / MSIINSTALLPERUSER),
/// otherwise the installing user's unmanaged instance.
[[nodiscard]] ProductInstance session_instance(ActionContext& ctx);
/// The database actions write (the services' registry, the manager's SID).
[[nodiscard]] std::optional<ProductDatabase> action_database(ActionContext& ctx);

/// Table rows by column name ("" for a null cell / missing column).
class Rows {
public:
    Rows(Session& session, const char* table);
    [[nodiscard]] usize size() const;
    [[nodiscard]] std::string get(usize row, std::string_view column) const;
    [[nodiscard]] bool present() const { return m_table != nullptr; }

private:
    const Table* m_table{nullptr};
};

/// The state a component / feature ends the install in: its action state
/// when one is set, else its installed state (INSTALLSTATE_*).
[[nodiscard]] i32 component_final_state(Session& session, const std::string& component);
[[nodiscard]] i32 feature_final_state(Session& session, const std::string& feature);
/// True when the component is being installed (local or from source) or
/// removed by this session.
[[nodiscard]] bool component_installing(Session& session, const std::string& component);
[[nodiscard]] bool component_removing(Session& session, const std::string& component);
/// True when no feature of the product remains installed after this session
/// (REMOVE=ALL or every feature going absent): the product is being removed.
[[nodiscard]] bool removing_product(Session& session);
/// The package's Template platform is 64-bit (x64 / Intel64 / Arm64).
[[nodiscard]] bool package_is_64bit(Session& session);
/// Darwin descriptor for (feature, component) of the session's product. The
/// component is left out (the '<' form) when the feature has a single
/// component (measured with such a feature: "<product><feature><").
[[nodiscard]] std::string session_descriptor(Session& session, const std::string& feature,
                                             const std::string& component);
/// Full path of a component's key file ("" when its KeyPath is not a file).
[[nodiscard]] std::string component_key_file(Session& session, const std::string& component);

/// Schedule a registration operation (Learn "Execution Scripts"); fails the
/// action outside InstallInitialize..InstallFinalize as the engine contract
/// requires.
[[nodiscard]] bool require_script(ActionContext& ctx);
void schedule(ActionContext& ctx, const std::string& action, std::function<void()> run,
              std::function<void()> rollback = {});

}  // namespace reg_detail

/// Registration of COM classes, ProgIds, extensions, MIME types, type
/// libraries, fonts, self-registering modules, shortcuts and COM+
/// (msi_register_com.cpp); assemblies are msi_assembly.cpp.
void register_com_and_shell_actions(ActionRegistry& r);

}  // namespace vitreo::winapi::msi
