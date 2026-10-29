/// @file registry.cpp
/// @brief Capsule registry implementation (REG-001)

#include "capsule/registry/registry.hpp"

#include <algorithm>
#include <atomic>

#include <codecvt>
#include <cwctype>
#include <cstdio>
#include <fstream>
#include <locale>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

namespace vitreo::capsule {

namespace {

std::atomic<u64> g_next_value_order{1};

/// Store @p value under @p name keeping Windows' value order: a new name goes
/// to the end, an overwrite keeps its position.
void store_value(RegNode* node, const std::wstring& name, RegValue value) {
    const auto it = node->values.find(name);
    value.order = it != node->values.end() ? it->second.order
                                           : g_next_value_order.fetch_add(1);
    if (it != node->values.end()) {
        it->second = std::move(value);
    } else {
        node->values.emplace(name, std::move(value));
    }
}

/// A key's value names in enumeration (creation) order.
std::vector<std::wstring> ordered_value_names(const RegNode* node) {
    std::vector<std::pair<u64, const std::wstring*>> ordered;
    ordered.reserve(node->values.size());
    for (const auto& [name, value] : node->values) {
        ordered.emplace_back(value.order, &name);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::wstring> names;
    names.reserve(ordered.size());
    for (const auto& entry : ordered) names.push_back(*entry.second);
    return names;
}

} // namespace

// =============================================================================
// Construction
// =============================================================================

Registry::Registry()
    : m_hkcr(std::make_unique<RegNode>(L"HKEY_CLASSES_ROOT"))
    , m_hkcu(std::make_unique<RegNode>(L"HKEY_CURRENT_USER"))
    , m_hklm(std::make_unique<RegNode>(L"HKEY_LOCAL_MACHINE"))
    , m_hku(std::make_unique<RegNode>(L"HKEY_USERS"))
{
    // Map well-known root handles to their nodes.
    m_handles[HKEY_CLASSES_ROOT]  = m_hkcr.get();
    m_handles[HKEY_CURRENT_USER]  = m_hkcu.get();
    m_handles[HKEY_LOCAL_MACHINE] = m_hklm.get();
    m_handles[HKEY_USERS]         = m_hku.get();
    m_handle_info[HKEY_CURRENT_USER]  = HandleInfo{RegView::Default, {m_hkcu->name}};
    m_handle_info[HKEY_LOCAL_MACHINE] = HandleInfo{RegView::Default, {m_hklm->name}};
    m_handle_info[HKEY_USERS]         = HandleInfo{RegView::Default, {m_hku->name}};

    init_defaults();

    // HKCR is a merged view over the interactive user's class registrations
    // and the machine defaults. Keep the backing nodes owned by their real
    // hives; merged handles below only bind views onto those nodes.
    MergedKeyBinding hkcr;
    hkcr.user_path = {m_hkcu->name, L"Software", L"Classes"};
    hkcr.machine_path = {m_hklm->name, L"SOFTWARE", L"Classes"};
    hkcr.user = walk_physical(hkcr.user_path, true);
    hkcr.machine = walk_physical(hkcr.machine_path, true);
    m_merged_handles.emplace(HKEY_CLASSES_ROOT, std::move(hkcr));
}

// =============================================================================
// open_key
// =============================================================================

Result<RegHandle, Error> Registry::open_key(RegHandle parent,
                                             std::wstring_view subkey,
                                             bool create) {
    return open_key(parent, subkey, create, RegView::Default, nullptr);
}

Result<RegHandle, Error> Registry::open_key(RegHandle parent,
                                             std::wstring_view subkey,
                                             bool create, RegView view,
                                             bool* created_out) {
    std::lock_guard lock(m_mutex);
    sync_live_journal();
    if (created_out != nullptr) *created_out = false;
    const bool wow32 = view == RegView::Default ? handle_is_wow32(parent)
                                                : view == RegView::Wow32;
    const auto parts = split_reg_path(subkey);

    if (const auto merged = m_merged_handles.find(parent);
        merged != m_merged_handles.end()) {
        std::wstring relative = merged->second.relative_path;
        for (const auto& part : parts) {
            if (!relative.empty()) relative.push_back(L'\\');
            relative += part;
        }
        const auto rel_parts = split_reg_path(relative);
        Path user_path{m_hkcu->name, L"Software", L"Classes"};
        Path machine_path{m_hklm->name, L"SOFTWARE", L"Classes"};
        user_path.insert(user_path.end(), rel_parts.begin(), rel_parts.end());
        machine_path.insert(machine_path.end(), rel_parts.begin(), rel_parts.end());
        user_path = redirect_path(std::move(user_path), wow32);
        machine_path = redirect_path(std::move(machine_path), wow32);
        RegNode* user = walk_physical(user_path, false);
        RegNode* machine = walk_physical(machine_path, false);
        bool created = false;
        // RegCreateKeyEx through HKCR creates new keys in the machine class
        // store (create_hkcr_clsid_default). Existing per-user keys remain
        // the write target for values.
        if (create && user == nullptr && machine == nullptr) {
            machine = walk_physical(machine_path, true);
            created = machine != nullptr;
        }
        if (user == nullptr && machine == nullptr) {
            return Result<RegHandle, Error>::err(Error::NotFound);
        }

        const RegHandle handle = m_next_handle++;
        MergedKeyBinding binding;
        binding.user = user;
        binding.machine = machine;
        binding.relative_path = std::move(relative);
        binding.wow32 = wow32;
        binding.user_path = std::move(user_path);
        binding.machine_path = std::move(machine_path);
        if (created) record_live_key(journal_path(binding.machine_path));
        m_merged_handles.emplace(handle, std::move(binding));
        m_handles[handle] = user != nullptr ? user : machine;
        if (created_out != nullptr) *created_out = created;
        return Result<RegHandle, Error>::ok(handle);
    }

    RegNode* node = resolve(parent);
    const auto info = m_handle_info.find(parent);
    if (!node || info == m_handle_info.end()) {
        return Result<RegHandle, Error>::err(Error::InvalidHandle);
    }

    Path path = info->second.path;
    path.insert(path.end(), parts.begin(), parts.end());
    path = redirect_path(std::move(path), wow32);

    RegNode* target = walk_physical(path, false);
    bool created = false;
    if (target == nullptr && create) {
        target = walk_physical(path, true);
        created = target != nullptr;
    }
    if (!target) {
        return Result<RegHandle, Error>::err(Error::NotFound);
    }

    auto h = m_next_handle++;
    m_handles[h] = target;
    if (created) record_live_key(journal_path(path));
    m_handle_info[h] = HandleInfo{wow32 ? RegView::Wow32 : RegView::Native64,
                                  std::move(path)};
    if (created_out != nullptr) *created_out = created;
    return Result<RegHandle, Error>::ok(h);
}

// =============================================================================
// close_key
// =============================================================================

void Registry::close_key(RegHandle handle) {
    std::lock_guard lock(m_mutex);
    // Silently ignore root handles – they are always valid.
    if (handle == HKEY_CLASSES_ROOT  || handle == HKEY_CURRENT_USER ||
        handle == HKEY_LOCAL_MACHINE || handle == HKEY_USERS) {
        return;
    }
    for (const auto& [key, root] : m_view_roots) {
        (void)key;
        if (root == handle) return;
    }
    m_merged_handles.erase(handle);
    m_handle_info.erase(handle);
    m_handles.erase(handle);
}

// =============================================================================
// query_value
// =============================================================================

Result<RegValue, Error> Registry::query_value(RegHandle key,
                                               std::wstring_view name) {
    std::lock_guard lock(m_mutex);
    sync_live_journal();
    if (const auto merged = m_merged_handles.find(key);
        merged != m_merged_handles.end()) {
        const std::wstring name_str(name);
        for (RegNode* node : {merged->second.user, merged->second.machine}) {
            if (node == nullptr) continue;
            const auto value = node->values.find(name_str);
            if (value != node->values.end()) {
                return Result<RegValue, Error>::ok(value->second);
            }
        }
        return Result<RegValue, Error>::err(Error::NotFound);
    }

    RegNode* node = resolve(key);
    if (!node) {
        return Result<RegValue, Error>::err(Error::InvalidHandle);
    }

    std::wstring name_str(name);
    auto it = node->values.find(name_str);
    if (it == node->values.end()) {
        return Result<RegValue, Error>::err(Error::NotFound);
    }

    return Result<RegValue, Error>::ok(it->second);
}

// =============================================================================
// set_value
// =============================================================================

VoidResult Registry::set_value(RegHandle key,
                                std::wstring_view name,
                                const RegValue& value) {
    std::lock_guard lock(m_mutex);
    sync_live_journal();
    if (const auto merged = m_merged_handles.find(key);
        merged != m_merged_handles.end()) {
        const bool user = merged->second.user != nullptr;
        RegNode* target = user ? merged->second.user : merged->second.machine;
        if (target == nullptr) return VoidResult::err(Error::InvalidHandle);
        store_value(target, std::wstring(name), value);
        record_live_value(journal_path(user ? merged->second.user_path
                                           : merged->second.machine_path),
                          name, value);
        return VoidResult::ok();
    }

    RegNode* node = resolve(key);
    if (!node) {
        return VoidResult::err(Error::InvalidHandle);
    }

    std::wstring name_str(name);
    store_value(node, name_str, value);
    if (const auto info = m_handle_info.find(key); info != m_handle_info.end()) {
        record_live_value(journal_path(info->second.path), name, value);
    }
    return VoidResult::ok();
}

// =============================================================================
// delete_value
// =============================================================================

VoidResult Registry::delete_value(RegHandle key,
                                   std::wstring_view name) {
    std::lock_guard lock(m_mutex);
    sync_live_journal();
    if (const auto merged = m_merged_handles.find(key);
        merged != m_merged_handles.end()) {
        const std::wstring name_str(name);
        const std::pair<RegNode*, const Path*> sides[] = {
            {merged->second.user, &merged->second.user_path},
            {merged->second.machine, &merged->second.machine_path}};
        for (const auto& [node, node_path] : sides) {
            if (node == nullptr) continue;
            const auto value = node->values.find(name_str);
            if (value != node->values.end()) {
                node->values.erase(value);
                record_live_value_delete(journal_path(*node_path), name);
                return VoidResult::ok();
            }
        }
        return VoidResult::err(Error::NotFound);
    }

    RegNode* node = resolve(key);
    if (!node) {
        return VoidResult::err(Error::InvalidHandle);
    }

    std::wstring name_str(name);
    auto it = node->values.find(name_str);
    if (it == node->values.end()) {
        return VoidResult::err(Error::NotFound);
    }

    node->values.erase(it);
    if (const auto info = m_handle_info.find(key); info != m_handle_info.end()) {
        record_live_value_delete(journal_path(info->second.path), name);
    }
    return VoidResult::ok();
}


// =============================================================================
// enum_values
// =============================================================================

Result<std::vector<std::wstring>, Error> Registry::enum_values(RegHandle key) {
    std::lock_guard lock(m_mutex);
    sync_live_journal();
    if (const auto merged = m_merged_handles.find(key);
        merged != m_merged_handles.end()) {
        std::map<std::wstring, std::wstring, RegCaseInsensitive> unique;
        for (RegNode* node : {merged->second.user, merged->second.machine}) {
            if (node == nullptr) continue;
            for (const auto& [name, value] : node->values) {
                (void)value;
                unique.try_emplace(name, name);
            }
        }
        std::vector<std::wstring> names;
        names.reserve(unique.size());
        for (const auto& [key_name, display_name] : unique) {
            (void)key_name;
            names.push_back(display_name);
        }
        return Result<std::vector<std::wstring>, Error>::ok(std::move(names));
    }

    RegNode* node = resolve(key);
    if (!node) {
        return Result<std::vector<std::wstring>, Error>::err(Error::InvalidHandle);
    }

    return Result<std::vector<std::wstring>, Error>::ok(ordered_value_names(node));
}

std::optional<std::wstring>
Registry::canonical_key_path(RegHandle key) const {
    std::lock_guard lock(m_mutex);
    if (const auto merged = m_merged_handles.find(key);
        merged != m_merged_handles.end()) {
        // HKCR-relative identity; a 32-bit-view handle names the WOW6432Node
        // form so the two views of one class key stay distinct.
        Path relative{L"HKEY_CLASSES_ROOT", L"Software", L"Classes"};
        const auto parts = split_reg_path(merged->second.relative_path);
        relative.insert(relative.end(), parts.begin(), parts.end());
        if (key != HKEY_CLASSES_ROOT && merged->second.wow32) {
            relative[0] = L"HKEY_CURRENT_USER";
            relative = redirect_path(std::move(relative), true);
        }
        relative.erase(relative.begin(), relative.begin() + 3);
        relative.insert(relative.begin(), L"HKEY_CLASSES_ROOT");
        return path_string(relative);
    }
    if (resolve(key) == nullptr) return std::nullopt;
    const auto info = m_handle_info.find(key);
    if (info == m_handle_info.end()) return std::nullopt;
    return path_string(info->second.path);
}

// =============================================================================
// Private helpers
// =============================================================================

RegNode* Registry::resolve(RegHandle handle) const {
    auto it = m_handles.find(handle);
    if (it == m_handles.end()) {
        return nullptr;
    }
    return it->second;
}

RegNode* Registry::walk_path(RegNode* root,
                              const std::vector<std::wstring>& parts,
                              bool create) {
    if (root == nullptr) {
        return nullptr;
    }
    RegNode* current = root;
    for (const auto& part : parts) {
        auto it = current->children.find(part);
        if (it != current->children.end()) {
            current = it->second.get();
        } else if (create) {
            auto child = std::make_unique<RegNode>(part);
            auto* raw = child.get();
            current->children[part] = std::move(child);
            current = raw;
        } else {
            return nullptr;
        }
    }
    return current;
}

// =============================================================================
// Persistence helpers — simple line-based text format
// =============================================================================
//
// Format per hive file (e.g. HKLM.reg):
//
//   [KEY_PATH]
//   "ValueName"=type:data
//   @=type:data            (default value, empty name)
//
// Types: sz, expand_sz, dword, qword, binary, multi_sz, none
// Strings are stored as UTF-8 with escaping for \, \n, \r, \t, "
// Binary is stored as hex pairs (e.g. 0a1b2c)
// Multi-string values use \0 as separator in the encoded string

namespace {

// -- UTF-8 <-> wstring conversion helpers ------------------------------------

std::string wstring_to_utf8(const std::wstring& ws) {
    std::string out;
    out.reserve(ws.size() * 2);
    for (wchar_t wc : ws) {
        auto cp = static_cast<uint32_t>(wc);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::wstring utf8_to_wstring(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        auto c = static_cast<uint8_t>(s[i]);
        if (c < 0x80) {
            cp = c; i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            if (i + 1 < s.size()) cp = (cp << 6) | (static_cast<uint8_t>(s[i+1]) & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            if (i + 1 < s.size()) cp = (cp << 6) | (static_cast<uint8_t>(s[i+1]) & 0x3F);
            if (i + 2 < s.size()) cp = (cp << 6) | (static_cast<uint8_t>(s[i+2]) & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            if (i + 1 < s.size()) cp = (cp << 6) | (static_cast<uint8_t>(s[i+1]) & 0x3F);
            if (i + 2 < s.size()) cp = (cp << 6) | (static_cast<uint8_t>(s[i+2]) & 0x3F);
            if (i + 3 < s.size()) cp = (cp << 6) | (static_cast<uint8_t>(s[i+3]) & 0x3F);
            i += 4;
        } else {
            i += 1; // skip invalid byte
            continue;
        }
        out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
}

// -- String escaping for the .reg format ------------------------------------

std::string escape_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\0': out += "\\0";  break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::string unescape_string(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case '\\': out.push_back('\\'); ++i; break;
                case '"':  out.push_back('"');  ++i; break;
                case 'n':  out.push_back('\n'); ++i; break;
                case 'r':  out.push_back('\r'); ++i; break;
                case 't':  out.push_back('\t'); ++i; break;
                case '0':  out.push_back('\0'); ++i; break;
                default:   out.push_back(s[i]); break;
            }
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// -- Hex encoding for binary data -------------------------------------------

std::string bytes_to_hex(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve(data.size() * 2);
    static const char hex[] = "0123456789abcdef";
    for (uint8_t b : data) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

std::vector<uint8_t> hex_to_bytes(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        auto nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
        };
        out.push_back(static_cast<uint8_t>((nibble(s[i]) << 4) | nibble(s[i + 1])));
    }
    return out;
}

// -- Serialize a RegValue to "type:data" string ------------------------------

std::string serialize_value(const RegValue& val) {
    switch (val.type) {
        case RegType::Sz: {
            auto& ws = std::get<std::wstring>(val.data);
            return "sz:" + escape_string(wstring_to_utf8(ws));
        }
        case RegType::ExpandSz: {
            auto& ws = std::get<std::wstring>(val.data);
            return "expand_sz:" + escape_string(wstring_to_utf8(ws));
        }
        case RegType::Dword: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "dword:%u", std::get<u32>(val.data));
            return buf;
        }
        case RegType::Qword: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "qword:%llu",
                          static_cast<unsigned long long>(std::get<u64>(val.data)));
            return buf;
        }
        case RegType::Binary: {
            auto& bytes = std::get<std::vector<u8>>(val.data);
            return "binary:" + bytes_to_hex(bytes);
        }
        case RegType::MultiSz: {
            auto& strings = std::get<std::vector<std::wstring>>(val.data);
            std::string encoded;
            for (size_t i = 0; i < strings.size(); ++i) {
                if (i > 0) encoded += "\\0";
                encoded += escape_string(wstring_to_utf8(strings[i]));
            }
            return "multi_sz:" + encoded;
        }
        case RegType::Raw: {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "raw:%u:", val.raw_type);
            return buf + bytes_to_hex(std::get<std::vector<u8>>(val.data));
        }
        case RegType::None:
        default:
            return "none:";
    }
}

bool deserialize_value(const std::string& type_data, RegValue& out) {
    auto colon = type_data.find(':');
    if (colon == std::string::npos) return false;

    std::string type_str = type_data.substr(0, colon);
    std::string data_str = type_data.substr(colon + 1);

    if (type_str == "sz") {
        out.type = RegType::Sz;
        out.data = utf8_to_wstring(unescape_string(data_str));
    } else if (type_str == "expand_sz") {
        out.type = RegType::ExpandSz;
        out.data = utf8_to_wstring(unescape_string(data_str));
    } else if (type_str == "dword") {
        out.type = RegType::Dword;
        out.data = static_cast<u32>(std::stoul(data_str));
    } else if (type_str == "qword") {
        out.type = RegType::Qword;
        out.data = static_cast<u64>(std::stoull(data_str));
    } else if (type_str == "binary") {
        out.type = RegType::Binary;
        out.data = hex_to_bytes(data_str);
    } else if (type_str == "multi_sz") {
        out.type = RegType::MultiSz;
        std::vector<std::wstring> parts;
        // Split on literal \0 (escaped null)
        std::string raw = unescape_string(data_str);
        size_t start = 0;
        while (start <= raw.size()) {
            auto pos = raw.find('\0', start);
            if (pos == std::string::npos) {
                parts.push_back(utf8_to_wstring(raw.substr(start)));
                break;
            }
            parts.push_back(utf8_to_wstring(raw.substr(start, pos - start)));
            start = pos + 1;
        }
        out.data = std::move(parts);
    } else if (type_str == "raw") {
        // raw:<Win32 type>:<hex bytes>
        const auto sep = data_str.find(':');
        if (sep == std::string::npos) return false;
        out.type = RegType::Raw;
        out.raw_type = static_cast<u32>(std::stoul(data_str.substr(0, sep)));
        out.data = hex_to_bytes(data_str.substr(sep + 1));
    } else if (type_str == "none") {
        out.type = RegType::None;
        out.data = std::monostate{};
    } else {
        return false;
    }
    return true;
}

// -- Recursive serialization of a RegNode tree --------------------------------

/// HKLM\HARDWARE is volatile on Windows: rebuilt at every boot from the
/// firmware and devices and never written to a hive file. Vitreo rebuilds it
/// at every process start, so a saved copy would only resurrect stale values.
bool volatile_hardware_key(const RegNode* parent, const RegNode* child) {
    if (parent == nullptr || parent->name != L"HKEY_LOCAL_MACHINE") return false;
    std::wstring name = child->name;
    for (wchar_t& c : name) c = static_cast<wchar_t>(std::towupper(c));
    return name == L"HARDWARE";
}

void write_node(std::ofstream& out, const RegNode* node, const std::string& path) {
    // Write key header
    out << "[" << escape_string(path) << "]\n";

    // Write values
    // In enumeration order, so a reload recreates the same order.
    for (const auto& name : ordered_value_names(node)) {
        const RegValue& val = node->values.find(name)->second;
        std::string name_utf8 = wstring_to_utf8(name);
        if (name_utf8.empty()) {
            out << "@=" << serialize_value(val) << "\n";
        } else {
            out << "\"" << escape_string(name_utf8) << "\"=" << serialize_value(val) << "\n";
        }
    }
    out << "\n";

    // Recurse into children
    for (const auto& [child_name, child] : node->children) {
        if (volatile_hardware_key(node, child.get())) continue;
        std::string child_path = path.empty()
            ? wstring_to_utf8(child->name)
            : path + "\\" + wstring_to_utf8(child->name);
        write_node(out, child.get(), child_path);
    }
}

void read_hive(std::ifstream& in, RegNode* root) {
    std::string line;
    RegNode* current_node = root;

    while (std::getline(in, line)) {
        // Skip empty lines
        if (line.empty() || line[0] == '#') continue;

        // Key header: [path]
        if (line.front() == '[' && line.back() == ']') {
            std::string key_path = unescape_string(line.substr(1, line.size() - 2));

            // Parse backslash-delimited path and walk/create nodes
            current_node = root;
            if (!key_path.empty()) {
                std::wstring wpath = utf8_to_wstring(key_path);
                auto parts = split_reg_path(wpath);
                for (const auto& part : parts) {
                    auto it = current_node->children.find(part);
                    if (it != current_node->children.end()) {
                        current_node = it->second.get();
                    } else {
                        auto child = std::make_unique<RegNode>(part);
                        auto* raw = child.get();
                        current_node->children[part] = std::move(child);
                        current_node = raw;
                    }
                }
            }
            continue;
        }

        // Value: "name"=type:data  or  @=type:data
        std::string name_utf8;
        std::string type_data;

        if (line[0] == '@' && line.size() > 1 && line[1] == '=') {
            // Default value
            type_data = line.substr(2);
        } else if (line[0] == '"') {
            // Find closing quote (handle escaped quotes)
            size_t end_quote = 1;
            while (end_quote < line.size()) {
                if (line[end_quote] == '"' && line[end_quote - 1] != '\\') break;
                ++end_quote;
            }
            if (end_quote >= line.size()) continue; // malformed

            name_utf8 = unescape_string(line.substr(1, end_quote - 1));

            // Skip past "=
            if (end_quote + 1 < line.size() && line[end_quote + 1] == '=') {
                type_data = line.substr(end_quote + 2);
            } else {
                continue; // malformed
            }
        } else {
            continue; // unknown line
        }

        RegValue val;
        if (deserialize_value(type_data, val)) {
            std::wstring wname = utf8_to_wstring(name_utf8);
            store_value(current_node, wname, std::move(val));
        }
    }
}

} // anonymous namespace

namespace {

std::string live_hex(std::string_view text) {
    return bytes_to_hex(std::vector<u8>(text.begin(), text.end()));
}

std::string live_unhex(std::string_view text) {
    const auto bytes = hex_to_bytes(std::string(text));
    return std::string(bytes.begin(), bytes.end());
}

std::vector<std::string_view> split_live_record(std::string_view line) {
    std::vector<std::string_view> fields;
    usize begin = 0;
    for (;;) {
        const usize separator = line.find('\t', begin);
        fields.push_back(line.substr(
            begin, separator == std::string_view::npos
                ? line.size() - begin : separator - begin));
        if (separator == std::string_view::npos) break;
        begin = separator + 1;
    }
    return fields;
}

} // namespace

RegNode* Registry::live_write_node(std::wstring_view canonical_path,
                                   bool create) {
    // Records keep the key names' case (a peer creates the key as written);
    // only the root name is matched case-folded.
    const std::wstring path(canonical_path);
    const usize separator = path.find(L'\\');
    std::wstring root_name = path.substr(0, separator);
    std::transform(root_name.begin(), root_name.end(), root_name.begin(),
                   [](wchar_t ch) { return std::towlower(ch); });
    const std::wstring relative = separator == std::wstring::npos
        ? std::wstring{} : path.substr(separator + 1);
    const auto parts = split_reg_path(relative);

    if (root_name == L"hkey_classes_root") {
        RegNode* user_classes = walk_path(
            m_hkcu.get(), split_reg_path(L"Software\\Classes"), true);
        RegNode* machine_classes = walk_path(
            m_hklm.get(), split_reg_path(L"Software\\Classes"), true);
        RegNode* user = parts.empty()
            ? user_classes : walk_path(user_classes, parts, false);
        RegNode* machine = parts.empty()
            ? machine_classes : walk_path(machine_classes, parts, false);
        if (user != nullptr) return user;
        if (machine != nullptr || !create) return machine;
        return walk_path(machine_classes, parts, true);
    }

    RegNode* root = nullptr;
    if (root_name == L"hkey_current_user") root = m_hkcu.get();
    if (root_name == L"hkey_local_machine") root = m_hklm.get();
    if (root_name == L"hkey_users") root = m_hku.get();
    if (root_name == L"hkey_classes_root_storage") root = m_hkcr.get();
    if (root == nullptr) return nullptr;
    return parts.empty() ? root : walk_path(root, parts, create);
}

void Registry::live_delete_node(std::wstring_view canonical_path) {
    // Records keep the key names' case (a peer creates the key as written);
    // only the root name is matched case-folded.
    const std::wstring path(canonical_path);
    const usize separator = path.find(L'\\');
    std::wstring root_name = path.substr(0, separator);
    std::transform(root_name.begin(), root_name.end(), root_name.begin(),
                   [](wchar_t ch) { return std::towlower(ch); });
    const std::wstring relative = separator == std::wstring::npos
        ? std::wstring{} : path.substr(separator + 1);
    auto parts = split_reg_path(relative);
    if (parts.empty()) return;
    const std::wstring leaf = parts.back();
    parts.pop_back();

    const auto retire_from = [&](RegNode* base) -> bool {
        if (base == nullptr) return false;
        RegNode* owner = parts.empty()
            ? base : walk_path(base, parts, false);
        if (owner == nullptr) return false;
        auto found = owner->children.find(leaf);
        if (found == owner->children.end()) return false;
        m_live_retired_nodes.push_back(std::move(found->second));
        owner->children.erase(found);
        return true;
    };

    if (root_name == L"hkey_classes_root") {
        RegNode* user_classes = walk_path(
            m_hkcu.get(), split_reg_path(L"Software\\Classes"), true);
        if (retire_from(user_classes)) return;
        RegNode* machine_classes = walk_path(
            m_hklm.get(), split_reg_path(L"Software\\Classes"), true);
        (void)retire_from(machine_classes);
        return;
    }
    RegNode* root = nullptr;
    if (root_name == L"hkey_current_user") root = m_hkcu.get();
    if (root_name == L"hkey_local_machine") root = m_hklm.get();
    if (root_name == L"hkey_users") root = m_hku.get();
    if (root_name == L"hkey_classes_root_storage") root = m_hkcr.get();
    (void)retire_from(root);
}

void Registry::apply_live_record(std::string_view record) {
    const auto fields = split_live_record(record);
    if (fields.size() < 2) return;
    const std::wstring path = utf8_to_wstring(live_unhex(fields[1]));
    if (fields[0] == "C") {
        (void)live_write_node(path, true);
        return;
    }
    if (fields[0] == "S" && fields.size() == 4) {
        RegNode* node = live_write_node(path, true);
        if (node == nullptr) return;
        RegValue value;
        if (!deserialize_value(live_unhex(fields[3]), value)) return;
        store_value(node, utf8_to_wstring(live_unhex(fields[2])), std::move(value));
        return;
    }
    if (fields[0] == "D" && fields.size() == 3) {
        RegNode* node = live_write_node(path, false);
        if (node != nullptr) {
            node->values.erase(utf8_to_wstring(live_unhex(fields[2])));
        }
        return;
    }
    if (fields[0] == "K") live_delete_node(path);
}

void Registry::sync_live_journal() {
    if (m_registry_dir.empty()) return;
    const auto path = m_registry_dir / "live.reglog";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CREAT, 0600);
    if (fd < 0) return;
    if (::flock(fd, LOCK_SH) != 0) {
        ::close(fd);
        return;
    }
    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        (void)::flock(fd, LOCK_UN);
        ::close(fd);
        return;
    }
    const u64 file_size = static_cast<u64>(status.st_size);
    if (m_live_journal_offset > file_size) m_live_journal_offset = 0;
    const usize pending = static_cast<usize>(
        file_size - m_live_journal_offset);
    std::string content(pending, '\0');
    usize read_total = 0;
    while (read_total < pending) {
        const ssize_t count = ::pread(
            fd, content.data() + read_total, pending - read_total,
            static_cast<off_t>(m_live_journal_offset + read_total));
        if (count <= 0) break;
        read_total += static_cast<usize>(count);
    }
    (void)::flock(fd, LOCK_UN);
    ::close(fd);
    content.resize(read_total);

    usize consumed = 0;
    while (consumed < content.size()) {
        const usize newline = content.find('\n', consumed);
        if (newline == std::string::npos) break;
        apply_live_record(std::string_view(content).substr(
            consumed, newline - consumed));
        consumed = newline + 1;
    }
    m_live_journal_offset += consumed;
}

void Registry::append_live_record(std::string record) {
    if (m_registry_dir.empty()) return;
    record.push_back('\n');
    const auto path = m_registry_dir / "live.reglog";

    // Do not advance past a peer record written between our last read and the
    // exclusive append lock. Retry after replaying it first.
    for (;;) {
        sync_live_journal();
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0600);
        if (fd < 0) return;
        if (::flock(fd, LOCK_EX) != 0) {
            ::close(fd);
            return;
        }
        struct stat status{};
        if (::fstat(fd, &status) != 0) {
            (void)::flock(fd, LOCK_UN);
            ::close(fd);
            return;
        }
        const u64 end = static_cast<u64>(status.st_size);
        if (end != m_live_journal_offset) {
            (void)::flock(fd, LOCK_UN);
            ::close(fd);
            continue;
        }
        usize written = 0;
        while (written < record.size()) {
            const ssize_t count = ::write(
                fd, record.data() + written, record.size() - written);
            if (count <= 0) break;
            written += static_cast<usize>(count);
        }
        if (written == record.size()) {
            m_live_journal_offset = end + written;
        }
        (void)::flock(fd, LOCK_UN);
        ::close(fd);
        if (written == record.size()) {
            // A peer replayed above may have overwritten the already-applied
            // local value. Reapply our now-last journal operation locally.
            apply_live_record(std::string_view(record).substr(
                0, record.size() - 1));
        }
        return;
    }
}

void Registry::record_live_key(std::wstring_view canonical_path) {
    append_live_record("C\t" + live_hex(
        wstring_to_utf8(std::wstring(canonical_path))));
}

void Registry::record_live_value(std::wstring_view canonical_path,
                                 std::wstring_view name,
                                 const RegValue& value) {
    append_live_record(
        "S\t" + live_hex(wstring_to_utf8(std::wstring(canonical_path))) +
        "\t" + live_hex(wstring_to_utf8(std::wstring(name))) +
        "\t" + live_hex(serialize_value(value)));
}

void Registry::record_live_value_delete(std::wstring_view canonical_path,
                                        std::wstring_view name) {
    append_live_record(
        "D\t" + live_hex(wstring_to_utf8(std::wstring(canonical_path))) +
        "\t" + live_hex(wstring_to_utf8(std::wstring(name))));
}

void Registry::record_live_key_delete(std::wstring_view canonical_path) {
    append_live_record("K\t" + live_hex(
        wstring_to_utf8(std::wstring(canonical_path))));
}

// =============================================================================
// Registry::save
// =============================================================================

void Registry::save(const std::filesystem::path& registry_dir) {
    std::lock_guard lock(m_mutex);
    if (m_registry_dir.empty()) m_registry_dir = registry_dir;
    sync_live_journal();
    std::error_code ec;
    std::filesystem::create_directories(registry_dir, ec);

    struct HiveInfo {
        const char* filename;
        const RegNode* root;
    };

    HiveInfo hives[] = {
        {"HKCR.reg", m_hkcr.get()},
        {"HKCU.reg", m_hkcu.get()},
        {"HKLM.reg", m_hklm.get()},
        {"HKU.reg",  m_hku.get()},
    };

    for (const auto& hive : hives) {
        auto path = registry_dir / hive.filename;
        auto temporary = path;
        temporary += ".tmp." + std::to_string(::getpid()) + "." +
            std::to_string(reinterpret_cast<uintptr_t>(this));
        std::ofstream out(temporary, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            VITREO_TRACE( "[registry] Warning: cannot write %s\n",
                         path.string().c_str());
            continue;
        }
        out << "# Vitreo Registry Hive: " << hive.filename << "\n\n";
        write_node(out, hive.root, "");
        out.close();
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            VITREO_TRACE("[registry] Warning: cannot replace %s: %s\n",
                         path.string().c_str(), ec.message().c_str());
            std::filesystem::remove(temporary, ec);
            ec.clear();
            continue;
        }
        VITREO_TRACE( "[registry] Saved %s\n", path.string().c_str());
    }
}

// =============================================================================
// Registry::load
// =============================================================================

void Registry::load(const std::filesystem::path& registry_dir) {
    std::lock_guard lock(m_mutex);
    m_registry_dir = registry_dir;
    m_live_journal_offset = 0;
    std::error_code directory_error;
    std::filesystem::create_directories(registry_dir, directory_error);
    struct HiveInfo {
        const char* filename;
        RegNode* root;
    };

    HiveInfo hives[] = {
        {"HKCR.reg", m_hkcr.get()},
        {"HKCU.reg", m_hkcu.get()},
        {"HKLM.reg", m_hklm.get()},
        {"HKU.reg",  m_hku.get()},
    };

    for (const auto& hive : hives) {
        auto path = registry_dir / hive.filename;
        std::ifstream in(path);
        if (!in.is_open()) {
            // Missing file — silently skip (first run or empty hive)
            continue;
        }
        read_hive(in, hive.root);
        VITREO_TRACE( "[registry] Loaded %s\n", path.string().c_str());
    }
    sync_live_journal();
}

} // namespace vitreo::capsule
