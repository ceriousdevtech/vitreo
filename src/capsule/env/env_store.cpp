/// @file env_store.cpp
/// @brief Capsule-level environment variable store implementation (ENV-001)

#include "capsule/env/env_store.hpp"

#include <cwctype>

namespace vitreo::capsule {

// =============================================================================
// Helper: wide string to uppercase
// =============================================================================

static std::wstring to_upper(std::wstring_view s) {
    std::wstring result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towupper(static_cast<std::wint_t>(c))); });
    return result;
}

// =============================================================================
// WideStringCaseInsensitive
// =============================================================================

bool WideStringCaseInsensitive::operator()(const std::wstring& a,
                                           const std::wstring& b) const {
    return to_upper(a) < to_upper(b);
}

// =============================================================================
// EnvStore
// =============================================================================

EnvStore::EnvStore() = default;

std::optional<std::wstring> EnvStore::get(std::wstring_view name) const {
    std::wstring key(name);
    auto it = m_vars.find(key);
    if (it == m_vars.end()) {
        return std::nullopt;
    }
    return it->second;
}

void EnvStore::set(std::wstring_view name, std::wstring_view value) {
    if (value.empty()) {
        remove(name);
        return;
    }
    std::wstring key(name);
    m_vars.insert_or_assign(std::move(key), std::wstring(value));
}

bool EnvStore::remove(std::wstring_view name) {
    std::wstring key(name);
    auto it = m_vars.find(key);
    if (it == m_vars.end()) {
        return false;
    }
    m_vars.erase(it);
    return true;
}

bool EnvStore::contains(std::wstring_view name) const {
    std::wstring key(name);
    return m_vars.find(key) != m_vars.end();
}

usize EnvStore::count() const {
    return m_vars.size();
}

std::vector<wchar_t> EnvStore::to_block() const {
    std::vector<wchar_t> block;

    for (const auto& [key, value] : m_vars) {
        // Append KEY=VALUE
        block.insert(block.end(), key.begin(), key.end());
        block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }

    // Trailing null (double-null terminator)
    block.push_back(L'\0');

    return block;
}

void EnvStore::init_defaults() {
    set(L"SystemRoot",    L"C:\\Windows");
    set(L"SystemDrive",   L"C:");
    set(L"TEMP",          L"C:\\Users\\Default\\AppData\\Local\\Temp");
    set(L"TMP",           L"C:\\Users\\Default\\AppData\\Local\\Temp");
    set(L"USERPROFILE",   L"C:\\Users\\Default");
    set(L"APPDATA",       L"C:\\Users\\Default\\AppData\\Roaming");
    set(L"LOCALAPPDATA",  L"C:\\Users\\Default\\AppData\\Local");
    set(L"PROGRAMFILES",  L"C:\\Program Files (x86)");
    set(L"ProgramFiles(x86)", L"C:\\Program Files (x86)");
    set(L"ProgramW6432",  L"C:\\Program Files");
    set(L"WINDIR",        L"C:\\Windows");
    set(L"PATH",          L"C:\\Windows\\System32;C:\\Windows");
    set(L"COMPUTERNAME",  L"VITREO");
    set(L"USERNAME",      L"Default");
}

} // namespace vitreo::capsule
