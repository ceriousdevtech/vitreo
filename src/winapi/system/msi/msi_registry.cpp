// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_registry.cpp
/// @brief The Registry-table standard actions (WriteRegistryValues,
/// RemoveRegistryValues) and the registry value encodings the installer
/// reads (RegLocator raw values) and writes (Registry.Value).
///
/// Contract: Learn "Registry Table", "RemoveRegistry Table",
/// "WriteRegistryValues Action", "RemoveRegistryValues Action",
/// "RegLocator Table"; measured where Learn is silent or differs by
/// tools/oracle_probe/msi_search_install_probe.c and msi_search_probe.c
/// (Windows 11, x86 and x64, .expected beside them):
///   * a value that is not storable ("#", "#12ab", "#0x10", "# 3", "#xZZ")
///     fails the install with error 1406 when the script runs; "#+7" is 7,
///     "#4294967296" wraps to 0, "#x123" pads to 01 23, "#x" is an empty
///     REG_BINARY, "#X" is accepted, "#%" stores the rest unexpanded;
///   * a [~] list appends when it starts with [~], prepends when it ends
///     with one, else replaces; an existing non-REG_MULTI_SZ value is
///     replaced; an empty middle item is kept;
///   * a null Value with a plain Name writes an empty REG_SZ; Name "+" with
///     a Value is a value literally named "+";
///   * removal deletes the value (a REG_MULTI_SZ loses only the strings the
///     row wrote, and goes when none remain), then every key left with no
///     values and no subkeys, outward ("Deep" case); "*" and "-" keys go
///     with their whole tree, "+" keys stay;
///   * rows run grouped by Root (-1, 0, 1, 2, 3), then by key in table order.

#include "winapi/system/msi/msi_search.hpp"
#include "winapi/system/msi/msi_search_internal.hpp"

#include "capsule/registry/registry.hpp"

#include <algorithm>
#include <memory>

namespace vitreo::winapi::msi {

using namespace search_detail;
using capsule::RegHandle;
using capsule::RegType;
using capsule::RegValue;

// ---- RegLocator raw values ------------------------------------------------------

std::optional<std::string> format_raw_registry_value(
    const RegValue& value, const std::function<std::optional<std::string>(const std::string&)>& expand) {
    switch (value.type) {
        case RegType::Sz: {
            const auto* text = std::get_if<std::wstring>(&value.data);
            if (text == nullptr || text->empty()) return std::nullopt;
            std::string out = narrow(*text);
            if (!out.empty() && out[0] == '#') out.insert(out.begin(), '#');
            return out;
        }
        case RegType::ExpandSz: {
            const auto* text = std::get_if<std::wstring>(&value.data);
            if (text == nullptr) return std::nullopt;
            // ExpandEnvironmentStrings: %NAME% for a defined NAME is its
            // value; an undefined one stays as written.
            const std::string raw = narrow(*text);
            std::string out;
            usize i = 0;
            while (i < raw.size()) {
                if (raw[i] == '%') {
                    const usize close = raw.find('%', i + 1);
                    if (close != std::string::npos && close > i + 1) {
                        const std::string name = raw.substr(i + 1, close - i - 1);
                        if (auto v = expand ? expand(name) : std::nullopt; v) {
                            out += *v;
                            i = close + 1;
                            continue;
                        }
                    }
                }
                out.push_back(raw[i++]);
            }
            if (out.empty()) return std::nullopt;
            return out;
        }
        case RegType::Dword: {
            const auto* d = std::get_if<u32>(&value.data);
            if (d == nullptr) return std::nullopt;
            return "#" + std::to_string(static_cast<i32>(*d));
        }
        case RegType::Binary: {
            const auto* bytes = std::get_if<std::vector<u8>>(&value.data);
            if (bytes == nullptr || bytes->empty()) return std::nullopt;
            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string out = "#x";
            for (const u8 b : *bytes) {
                out.push_back(kHex[b >> 4]);
                out.push_back(kHex[b & 15]);
            }
            return out;
        }
        case RegType::MultiSz: {
            const auto* items = std::get_if<std::vector<std::wstring>>(&value.data);
            if (items == nullptr) return std::nullopt;
            std::string out;
            for (const auto& item : *items) {
                out.push_back('\0');
                out += narrow(item);
            }
            out.push_back('\0');
            return out;
        }
        case RegType::Qword:
        case RegType::Raw:
        case RegType::None: {
            // Measured: REG_QWORD, REG_NONE and REG_DWORD_BIG_ENDIAN data
            // come back as the bytes read as UTF-16 code units.
            std::vector<u8> bytes;
            if (const auto* q = std::get_if<u64>(&value.data)) {
                for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<u8>(*q >> (8 * i)));
            } else if (const auto* b = std::get_if<std::vector<u8>>(&value.data)) {
                bytes = *b;
            }
            std::wstring units;
            for (usize i = 0; i + 1 < bytes.size(); i += 2) {
                const wchar_t unit = static_cast<wchar_t>(bytes[i] | (bytes[i + 1] << 8));
                if (unit == 0) break;
                units.push_back(unit);
            }
            if (units.empty()) return std::nullopt;
            return narrow(units);
        }
    }
    return std::nullopt;
}

// ---- Registry.Value encoding ----------------------------------------------------

namespace {

[[nodiscard]] int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

[[nodiscard]] std::vector<std::wstring> split_multi(std::string_view body) {
    std::vector<std::wstring> items;
    if (body.empty()) return items;
    usize start = 0;
    for (;;) {
        const usize nul = body.find('\0', start);
        items.push_back(widen(body.substr(start, nul == std::string_view::npos ? std::string_view::npos : nul - start)));
        if (nul == std::string_view::npos) break;
        start = nul + 1;
    }
    return items;
}

}  // namespace

std::optional<EncodedRegistryValue> encode_registry_value(const std::string& formatted) {
    EncodedRegistryValue out;
    std::string_view text = formatted;
    if (text.size() >= 2 && text[0] == '#' && text[1] == '#') {
        // Two or more leading '#': the first is dropped, the rest is text.
        text.remove_prefix(1);
    } else if (!text.empty() && text[0] == '#') {
        if (text.size() >= 2 && (text[1] == 'x' || text[1] == 'X')) {
            std::string_view hex = text.substr(2);
            std::vector<u8> bytes;
            usize i = 0;
            if (hex.size() % 2 == 1) {
                const int lo = hex_nibble(hex[0]);
                if (lo < 0) return std::nullopt;
                bytes.push_back(static_cast<u8>(lo));
                i = 1;
            }
            for (; i < hex.size(); i += 2) {
                const int hi = hex_nibble(hex[i]);
                const int lo = hex_nibble(hex[i + 1]);
                if (hi < 0 || lo < 0) return std::nullopt;
                bytes.push_back(static_cast<u8>((hi << 4) | lo));
            }
            out.value = RegValue{RegType::Binary, std::move(bytes)};
            return out;
        }
        if (text.size() >= 2 && text[1] == '%') {
            out.value = RegValue{RegType::ExpandSz, widen(text.substr(2))};
            return out;
        }
        std::string_view digits = text.substr(1);
        bool negative = false;
        if (!digits.empty() && (digits[0] == '+' || digits[0] == '-')) {
            negative = digits[0] == '-';
            digits.remove_prefix(1);
        }
        if (digits.empty()) return std::nullopt;
        u32 v = 0;
        for (const char c : digits) {
            if (c < '0' || c > '9') return std::nullopt;
            v = v * 10u + static_cast<u32>(c - '0');  // wraps mod 2^32 (measured)
        }
        if (negative) v = 0u - v;
        out.value = RegValue{RegType::Dword, v};
        return out;
    }
    if (text.find('\0') != std::string_view::npos) {
        const bool leading = text.front() == '\0';
        const bool trailing = text.back() == '\0';
        if (leading && !trailing) out.multi = EncodedRegistryValue::MultiMode::Append;
        else if (trailing && !leading) out.multi = EncodedRegistryValue::MultiMode::Prepend;
        if (leading) text.remove_prefix(1);
        if (trailing && !text.empty()) text.remove_suffix(1);
        out.value = RegValue{RegType::MultiSz, split_multi(text)};
        return out;
    }
    out.value = RegValue{RegType::Sz, widen(text)};
    return out;
}

std::vector<std::wstring> combine_multi_sz(const std::vector<std::wstring>& existing,
                                           const std::vector<std::wstring>& written,
                                           EncodedRegistryValue::MultiMode mode) {
    if (mode == EncodedRegistryValue::MultiMode::Replace) return written;
    std::vector<std::wstring> kept;
    for (const auto& s : existing) {
        if (std::find(written.begin(), written.end(), s) == written.end()) kept.push_back(s);
    }
    std::vector<std::wstring> out;
    if (mode == EncodedRegistryValue::MultiMode::Prepend) {
        out = written;
        out.insert(out.end(), kept.begin(), kept.end());
    } else {
        out = kept;
        out.insert(out.end(), written.begin(), written.end());
    }
    return out;
}

// ---- the actions ----------------------------------------------------------------

namespace {

/// One scheduled registry operation, resolved when the action runs.
struct RegOp {
    enum class Kind : u8 { SetValue, CreateKey, DeleteTree, RemoveValue } kind{Kind::SetValue};
    i64 root{0};
    RegHandle hive{0};
    /// The component's registry view: a 32-bit component's rows go to the
    /// 32-bit view (HKLM\SOFTWARE\WOW6432Node, Classes\WOW6432Node\CLSID)
    /// as on 64-bit Windows (Learn, "Windows Installer on 64-bit Operating
    /// Systems").
    capsule::RegView view{capsule::RegView::Native64};
    std::wstring key;      ///< below the hive
    std::wstring name;     ///< value name ("" = default value)
    std::string display_value;  ///< for ActionData / errors
    std::optional<EncodedRegistryValue> encoded;  ///< SetValue / RemoveValue of a written value
    bool removal_of_written{false};  ///< RemoveValue from the Registry table
};

[[nodiscard]] std::string show(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c == '\0') out += "[~]";
        else out.push_back(c);
    }
    return out;
}

[[nodiscard]] std::string key_text(const RegOp& op) { return narrow(hive_name(op.hive) + L"\\" + op.key); }

/// Run one op; the returned function undoes it.
u32 run_reg_op(OpEnv& env, const RegOp& op, std::function<void()>& undo) {
    capsule::Registry* reg = env.services.registry;
    if (reg == nullptr) {
        log_unimplemented("WriteRegistryValues", "no capsule registry wired into the engine");
        return kActionFailure;
    }
    const RegHandle hive = reg->view_root(op.hive, op.view);
    switch (op.kind) {
        case RegOp::Kind::CreateKey: {
            const auto missing = missing_keys(*reg, hive, op.key);
            auto opened = reg->open_key(hive, op.key, /*create=*/true);
            if (opened.is_err()) {
                error_message(env, 1401, {key_text(op), "5"});
                return kActionFailure;
            }
            reg->close_key(opened.value());
            undo = [reg, hive, missing] {
                for (auto it = missing.rbegin(); it != missing.rend(); ++it) prune_empty_keys(*reg, hive, *it);
            };
            return kActionSuccess;
        }
        case RegOp::Kind::DeleteTree: {
            auto snap = snapshot_key(*reg, hive, op.key);
            if (!snap) return kActionSuccess;  // "deleted, if present"
            delete_key_tree(*reg, hive, op.key);
            undo = [reg, hive, key = op.key, s = *snap] { restore_key(*reg, hive, key, s); };
            return kActionSuccess;
        }
        case RegOp::Kind::SetValue: {
            if (!op.encoded) {
                // Learn Error table 1406: Could not write value [2] to key [3].
                error_message(env, 1406, {narrow(op.name), key_text(op), ""});
                return kActionFailure;
            }
            const auto missing = missing_keys(*reg, hive, op.key);
            auto opened = reg->open_key(hive, op.key, /*create=*/true);
            if (opened.is_err()) {
                error_message(env, 1401, {key_text(op), "5"});
                return kActionFailure;
            }
            const RegHandle h = opened.value();
            std::optional<RegValue> prior;
            if (auto q = reg->query_value(h, op.name); q.is_ok()) prior = q.value();
            RegValue value = op.encoded->value;
            if (value.type == RegType::MultiSz && op.encoded->multi != EncodedRegistryValue::MultiMode::Replace &&
                prior && prior->type == RegType::MultiSz) {
                const auto* old_items = std::get_if<std::vector<std::wstring>>(&prior->data);
                const auto* new_items = std::get_if<std::vector<std::wstring>>(&value.data);
                if (old_items != nullptr && new_items != nullptr)
                    value.data = combine_multi_sz(*old_items, *new_items, op.encoded->multi);
            }
            const auto set = reg->set_value(h, op.name, value);
            reg->close_key(h);
            if (set.is_err()) {
                error_message(env, 1406, {narrow(op.name), key_text(op), "5"});
                return kActionFailure;
            }
            undo = [reg, hive, key = op.key, name = op.name, prior, missing] {
                auto k = reg->open_key(hive, key, prior.has_value());
                if (k.is_ok()) {
                    if (prior) (void)reg->set_value(k.value(), name, *prior);
                    else (void)reg->delete_value(k.value(), name);
                    reg->close_key(k.value());
                }
                for (auto it = missing.rbegin(); it != missing.rend(); ++it) prune_empty_keys(*reg, hive, *it);
            };
            return kActionSuccess;
        }
        case RegOp::Kind::RemoveValue: {
            auto opened = reg->open_key(hive, op.key, /*create=*/false);
            if (opened.is_err()) return kActionSuccess;  // nothing to remove
            const RegHandle h = opened.value();
            auto q = reg->query_value(h, op.name);
            if (q.is_err()) {
                reg->close_key(h);
                return kActionSuccess;
            }
            const RegValue prior = q.value();
            bool remove = true;
            if (op.removal_of_written && op.encoded && op.encoded->value.type == RegType::MultiSz &&
                prior.type == RegType::MultiSz) {
                const auto* old_items = std::get_if<std::vector<std::wstring>>(&prior.data);
                const auto* written = std::get_if<std::vector<std::wstring>>(&op.encoded->value.data);
                std::vector<std::wstring> kept;
                if (old_items != nullptr && written != nullptr) {
                    for (const auto& s : *old_items)
                        if (std::find(written->begin(), written->end(), s) == written->end()) kept.push_back(s);
                }
                if (!kept.empty()) {
                    remove = false;
                    (void)reg->set_value(h, op.name, RegValue{RegType::MultiSz, kept});
                }
            }
            if (remove && reg->delete_value(h, op.name).is_err()) {
                reg->close_key(h);
                error_message(env, 1403, {key_text(op), narrow(op.name), "5"});
                return kActionFailure;
            }
            reg->close_key(h);
            if (remove) prune_empty_keys(*reg, hive, op.key);
            undo = [reg, hive, key = op.key, name = op.name, prior] {
                auto k = reg->open_key(hive, key, true);
                if (k.is_ok()) {
                    (void)reg->set_value(k.value(), name, prior);
                    reg->close_key(k.value());
                }
            };
            return kActionSuccess;
        }
    }
    return kActionFailure;
}

struct RegRow {
    i64 root{0};
    std::string key, name, component;
    std::optional<std::string> value;  ///< formatted; nullopt = null
};

/// Rows of Registry (with_value) or RemoveRegistry, formatted, in the order
/// Windows runs them (Root, then key in first-seen order).
std::vector<RegRow> read_rows(Session& session, const char* table, bool with_value) {
    TableView t(session, table);
    std::vector<RegRow> rows;
    for (usize i = 0; i < t.size(); ++i) {
        RegRow r;
        r.root = t.integer(i, "Root").value_or(-1);
        r.key = session.format(t.cell(i, "Key"));
        r.name = session.format(t.cell(i, "Name"));
        if (with_value && !t.null(i, "Value")) r.value = session.format(t.cell(i, "Value"));
        r.component = t.cell(i, "Component_");
        rows.push_back(std::move(r));
    }
    std::vector<std::string> key_order;
    for (const auto& r : rows) {
        const std::string k = lower(r.key);
        if (std::find(key_order.begin(), key_order.end(), k) == key_order.end()) key_order.push_back(k);
    }
    auto key_rank = [&](const RegRow& r) {
        return std::find(key_order.begin(), key_order.end(), lower(r.key)) - key_order.begin();
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const RegRow& a, const RegRow& b) {
        if (a.root != b.root) return a.root < b.root;
        return key_rank(a) < key_rank(b);
    });
    return rows;
}

void schedule_reg_op(ActionContext& ctx, const char* action, RegOp op) {
    auto undo = std::make_shared<std::function<void()>>();
    ScriptOp script;
    script.action = action;
    script.run = [env = OpEnv(ctx), op, undo]() mutable -> u32 {
        std::vector<std::string> data{key_text(op), narrow(op.name)};
        if (op.kind == RegOp::Kind::SetValue) data.push_back(op.display_value);
        action_data(env, data);
        return run_reg_op(env, op, *undo);
    };
    script.rollback = [undo] {
        if (*undo) (*undo)();
    };
    schedule(ctx, std::move(script));
}

[[nodiscard]] capsule::RegView component_view(Session& session, const std::string& component) {
    return component_is_64bit(session, component) ? capsule::RegView::Native64 : capsule::RegView::Wow32;
}

[[nodiscard]] bool resolve_op_root(Session& session, i64 root, RegOp& op) {
    std::wstring prefix;
    if (!write_root(session, root, op.hive, prefix)) return false;
    op.root = root;
    op.key = prefix.empty() ? op.key : prefix + L"\\" + op.key;
    return true;
}

}  // namespace

u32 action_write_registry_values(ActionContext& ctx) {
    if (!require_script(ctx, "WriteRegistryValues")) return kActionFailure;
    Session& session = ctx.session();
    for (const RegRow& r : read_rows(session, "Registry", true)) {
        if (component_action(session, r.component) != ComponentAction::Install) continue;
        if (r.key.empty()) continue;
        RegOp op;
        op.key = widen(r.key);
        op.view = component_view(session, r.component);
        if (!resolve_op_root(session, r.root, op)) continue;
        if (!r.value && (r.name == "+" || r.name == "*")) {
            op.kind = RegOp::Kind::CreateKey;
        } else if (!r.value && r.name == "-") {
            continue;  // removed on uninstall only
        } else {
            op.kind = RegOp::Kind::SetValue;
            op.name = widen(r.name);
            const std::string value = r.value.value_or("");
            op.display_value = show(value);
            op.encoded = encode_registry_value(value);
        }
        schedule_reg_op(ctx, "WriteRegistryValues", std::move(op));
    }
    return kActionSuccess;
}

u32 action_remove_registry_values(ActionContext& ctx) {
    if (!require_script(ctx, "RemoveRegistryValues")) return kActionFailure;
    Session& session = ctx.session();
    // Registry rows of components being removed.
    for (const RegRow& r : read_rows(session, "Registry", true)) {
        if (component_action(session, r.component) != ComponentAction::Remove) continue;
        if (r.key.empty()) continue;
        RegOp op;
        op.key = widen(r.key);
        op.view = component_view(session, r.component);
        if (!resolve_op_root(session, r.root, op)) continue;
        if (!r.value && r.name == "+") continue;
        if (!r.value && (r.name == "*" || r.name == "-")) {
            op.kind = RegOp::Kind::DeleteTree;
        } else {
            op.kind = RegOp::Kind::RemoveValue;
            op.name = widen(r.name);
            op.removal_of_written = true;
            op.encoded = encode_registry_value(r.value.value_or(""));
        }
        schedule_reg_op(ctx, "RemoveRegistryValues", std::move(op));
    }
    // RemoveRegistry rows of components being installed.
    for (const RegRow& r : read_rows(session, "RemoveRegistry", false)) {
        if (component_action(session, r.component) != ComponentAction::Install) continue;
        if (r.key.empty()) continue;
        RegOp op;
        op.key = widen(r.key);
        op.view = component_view(session, r.component);
        if (!resolve_op_root(session, r.root, op)) continue;
        if (r.name == "-") {
            op.kind = RegOp::Kind::DeleteTree;
        } else {
            op.kind = RegOp::Kind::RemoveValue;
            op.name = widen(r.name);
        }
        schedule_reg_op(ctx, "RemoveRegistryValues", std::move(op));
    }
    return kActionSuccess;
}

// The pre-sequencer msiexec path (msi.cpp) writes every Registry row at
// once, ungated by component state; it stays until the engine's sequencer
// replaces that path, and shares the value encoding above.
bool Session::write_registry_values(capsule::Registry& registry) {
    TableView t(*this, "Registry");
    if (!t.present()) return true;
    for (usize i = 0; i < t.size(); ++i) {
        const std::string key = format(t.cell(i, "Key"));
        const std::string name = format(t.cell(i, "Name"));
        if (key.empty()) continue;
        RegHandle hive = 0;
        std::wstring prefix;
        if (!write_root(*this, t.integer(i, "Root").value_or(-1), hive, prefix)) continue;
        const std::wstring path = prefix.empty() ? widen(key) : prefix + L"\\" + widen(key);
        const bool null_value = t.null(i, "Value");
        if (null_value && name == "-") continue;
        const capsule::RegView view = component_is_64bit(*this, t.cell(i, "Component_"))
                                          ? capsule::RegView::Native64
                                          : capsule::RegView::Wow32;
        auto opened = registry.open_key(registry.view_root(hive, view), path, true);
        if (opened.is_err()) continue;
        if (!(null_value && (name == "+" || name == "*"))) {
            if (auto enc = encode_registry_value(format(t.cell(i, "Value")))) {
                (void)registry.set_value(opened.value(), widen(name), enc->value);
            }
        }
        registry.close_key(opened.value());
    }
    return true;
}

}  // namespace vitreo::winapi::msi
