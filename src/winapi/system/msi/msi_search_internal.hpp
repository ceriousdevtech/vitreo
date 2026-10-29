#pragma once
/// @file msi_search_internal.hpp
/// @brief Helpers shared by msi_search.cpp and msi_registry.cpp (the search
/// area's two files): text conversion, table cells, component gating,
/// registry roots and key trees, and the execution-script plumbing.

#include <vitreo/common.hpp>
#include "capsule/registry/registry.hpp"
#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_package.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi::msi::search_detail {

// ---- text ----------------------------------------------------------------------

/// UTF-8 (the engine's property encoding) to the UTF-16 code units the
/// capsule registry stores in wchar_t, and back.
[[nodiscard]] std::wstring widen(std::string_view utf8);
[[nodiscard]] std::string narrow(std::wstring_view wide);
[[nodiscard]] std::u16string to_u16(std::string_view utf8);
[[nodiscard]] bool iequals(std::string_view a, std::string_view b);
[[nodiscard]] std::string lower(std::string_view s);
[[nodiscard]] std::optional<i64> parse_int(std::string_view s);

// ---- tables ----------------------------------------------------------------------

/// Column access by name for one table (nullptr table = no rows).
class TableView {
public:
    TableView(Session& session, const char* name);
    [[nodiscard]] bool present() const { return m_table != nullptr; }
    [[nodiscard]] usize size() const { return m_table ? m_table->rows.size() : 0; }
    [[nodiscard]] std::string cell(usize row, std::string_view column) const;
    [[nodiscard]] bool null(usize row, std::string_view column) const { return cell(row, column).empty(); }
    [[nodiscard]] std::optional<i64> integer(usize row, std::string_view column) const {
        return parse_int(cell(row, column));
    }

private:
    const Table* m_table{nullptr};
};

// ---- component gating ------------------------------------------------------------

enum class ComponentAction : u8 { None, Install, Remove };
/// What the component's action state asks of its resources: Install for
/// INSTALLSTATE_LOCAL / SOURCE, Remove for INSTALLSTATE_ABSENT, None for no
/// action or an unknown component.
[[nodiscard]] ComponentAction component_action(Session& session, const std::string& component);
[[nodiscard]] bool component_is_64bit(Session& session, const std::string& component);

// ---- registry ------------------------------------------------------------------------

/// Per-machine install (ALLUSERS resolved to a machine install).
[[nodiscard]] bool per_machine(Session& session);
/// A Registry/RemoveRegistry Root as a hive and key prefix: -1 by ALLUSERS,
/// 0 (HKCR) as HKLM\Software\Classes or HKCU\Software\Classes by the
/// installation context (Learn "Registry Table"). False for an unknown root.
[[nodiscard]] bool write_root(Session& session, i64 root, capsule::RegHandle& hive, std::wstring& prefix);
[[nodiscard]] std::wstring hive_name(capsule::RegHandle hive);

/// Whole-tree snapshot of a key, for rollback of a tree delete.
struct KeySnapshot {
    std::wstring name;
    std::vector<std::pair<std::wstring, capsule::RegValue>> values;
    std::vector<KeySnapshot> children;
};
[[nodiscard]] std::optional<KeySnapshot> snapshot_key(capsule::Registry& reg, capsule::RegHandle hive,
                                                      const std::wstring& path);
void restore_key(capsule::Registry& reg, capsule::RegHandle hive, const std::wstring& path,
                 const KeySnapshot& snap);
/// Delete @p path with every subkey and value. True when it existed.
bool delete_key_tree(capsule::Registry& reg, capsule::RegHandle hive, const std::wstring& path);
[[nodiscard]] bool key_exists(capsule::Registry& reg, capsule::RegHandle hive, const std::wstring& path);
/// Delete @p path when it has no values and no subkeys, then its parents
/// the same way, never a key directly below the hive (Windows removes a key
/// after its last value: msi_search_install_probe "Deep").
void prune_empty_keys(capsule::Registry& reg, capsule::RegHandle hive, std::wstring path);
/// The keys along @p path that do not exist yet, outermost first.
[[nodiscard]] std::vector<std::wstring> missing_keys(capsule::Registry& reg, capsule::RegHandle hive,
                                                     const std::wstring& path);

// ---- script ----------------------------------------------------------------------

/// What a ScriptOp needs when the script runs (after the scheduling
/// action's ActionContext is gone): the session and the host services.
struct OpEnv {
    explicit OpEnv(ActionContext& ctx) : session(&ctx.session()), services(ctx.services()) {}
    Session* session;
    Services services;
    /// MsiProcessMessage through the engine (Error table, log, UI), else
    /// straight to the UI host.
    i32 message(u32 type, const Record& record);
};
/// ActionData for the UI/log (Learn: each action's "ActionData Messages").
void action_data(OpEnv& env, const std::vector<std::string>& fields);
/// An Error-table message ([1] = error number, then the fields); returns
/// the UI's answer (IDIGNORE 5 lets the caller continue).
i32 error_message(OpEnv& env, u32 error, const std::vector<std::string>& fields);
/// ScriptOp scheduling refuses outside InstallInitialize..InstallFinalize
/// (engine contract: such actions return 1603 there).
[[nodiscard]] bool require_script(ActionContext& ctx, std::string_view action);
/// ctx.schedule, or the collect_script_ops() sink when one is active.
void schedule(ActionContext& ctx, ScriptOp op);

}  // namespace vitreo::winapi::msi::search_detail
