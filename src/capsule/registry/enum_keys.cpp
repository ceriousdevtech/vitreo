// SPDX-License-Identifier: LGPL-2.1-only

/// @file registry.cpp
/// @brief Capsule registry implementation (REG-001)

#include "capsule/registry/registry.hpp"

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

/// @file enum_keys.cpp
/// @brief enum_keys
///
/// Closed cut from registry.cpp — verified with plan_extraction.py.

namespace vitreo::capsule {

// =============================================================================
// enum_keys
// =============================================================================

Result<void, Error> Registry::delete_physical(const Path& path) {
    if (path.size() < 2) return Result<void, Error>::err(Error::AccessDenied);
    RegNode* owner = walk_physical(Path(path.begin(), path.end() - 1), false);
    if (owner == nullptr) return Result<void, Error>::err(Error::NotFound);
    const auto found = owner->children.find(path.back());
    if (found == owner->children.end()) {
        return Result<void, Error>::err(Error::NotFound);
    }
    // Windows NT refuses to delete a key that still has subkeys.
    if (!found->second->children.empty()) {
        return Result<void, Error>::err(Error::AccessDenied);
    }
    // Drop every handle to the node so a stale handle cannot reach freed
    // memory afterwards.
    forget_handles_to(found->second.get());
    owner->children.erase(found);
    record_live_key_delete(journal_path(path));
    return Result<void, Error>::ok();
}

Result<void, Error> Registry::delete_key(RegHandle parent,
                                         std::wstring_view subkey,
                                         RegView view) {
    std::lock_guard lock(m_mutex);
    sync_live_journal();
    const bool wow32 = view == RegView::Default ? handle_is_wow32(parent)
                                                : view == RegView::Wow32;
    const auto parts = split_reg_path(subkey);

    if (const auto merged = m_merged_handles.find(parent);
        merged != m_merged_handles.end()) {
        if (parts.empty()) {
            return Result<void, Error>::err(Error::InvalidHandle);
        }
        auto rel = split_reg_path(merged->second.relative_path);
        rel.insert(rel.end(), parts.begin(), parts.end());
        Path user_path{m_hkcu->name, L"Software", L"Classes"};
        Path machine_path{m_hklm->name, L"SOFTWARE", L"Classes"};
        user_path.insert(user_path.end(), rel.begin(), rel.end());
        machine_path.insert(machine_path.end(), rel.begin(), rel.end());
        // The per-user registration hides the machine one, so it goes first.
        for (const Path& path : {redirect_path(std::move(user_path), wow32),
                                 redirect_path(std::move(machine_path), wow32)}) {
            auto result = delete_physical(path);
            if (result.is_ok() || result.error() != Error::NotFound) return result;
        }
        return Result<void, Error>::err(Error::NotFound);
    }

    if (resolve(parent) == nullptr) {
        return Result<void, Error>::err(Error::InvalidHandle);
    }
    const auto info = m_handle_info.find(parent);
    if (info == m_handle_info.end()) {
        return Result<void, Error>::err(Error::InvalidHandle);
    }
    // An empty subkey deletes the key `parent` itself (RegDeleteKeyEx on an
    // opened handle); a predefined root cannot be deleted.
    Path path = info->second.path;
    path.insert(path.end(), parts.begin(), parts.end());
    if (!parts.empty()) path = redirect_path(std::move(path), wow32);
    return delete_physical(path);
}

Result<std::vector<std::wstring>, Error> Registry::enum_keys(RegHandle key) {
    std::lock_guard lock(m_mutex);
    sync_live_journal();
    if (const auto merged = m_merged_handles.find(key);
        merged != m_merged_handles.end()) {
        std::map<std::wstring, std::wstring, RegCaseInsensitive> unique;
        for (RegNode* node : {merged->second.user, merged->second.machine}) {
            if (node == nullptr) continue;
            for (const auto& [name, child] : node->children) {
                unique.try_emplace(name, child->name);
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

    // Original-case names, plus the links a WOW6432Node key carries to the
    // shared keys (enum_software_32key, enum_wow_microsoft_64key).
    std::map<std::wstring, std::wstring, RegCaseInsensitive> unique;
    for (const auto& [child_name, child] : node->children) {
        unique.try_emplace(child_name, child->name);
    }
    if (const auto info = m_handle_info.find(key); info != m_handle_info.end()) {
        add_link_children(info->second.path, unique);
    }
    std::vector<std::wstring> names;
    names.reserve(unique.size());
    for (const auto& [key_name, display_name] : unique) {
        (void)key_name;
        names.push_back(display_name);
    }
    return Result<std::vector<std::wstring>, Error>::ok(std::move(names));
}

} // namespace vitreo::capsule
