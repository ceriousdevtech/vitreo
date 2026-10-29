#pragma once

#include <array>
#include <string>
#include <string_view>

namespace vitreo::pe {

// IsApiSetImplemented promises a VALID implementation of every API in a set.
// Import redirection is weaker: the legacy resolver includes family fallbacks
// and individual exports with explicit unsupported cases. Do not infer this
// table from those routes. Each addition needs the complete SDK contract member
// inventory and source/oracle validation of all its members, in both bitnesses.
// No complete API-set contract has yet met that gate in the contract audit.
inline constexpr std::array<std::string_view, 0> certified_api_sets{};

[[nodiscard]] inline bool is_api_set_implemented(std::string_view query) {
    std::string contract(query);
    for (char& ch : contract) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    // The Windows query accepts both .dll and .exe suffixes (apiquery_probe).
    if (contract.ends_with(".dll") || contract.ends_with(".exe")) contract.resize(contract.size() - 4);
    for (const auto certified : certified_api_sets) if (contract == certified) return true;
    return false;
}

// Namespace presence is weaker than complete API-set implementation. Only
// exact, oracle-observed routes declare namespaces; family/catch-all resolver
// routes must never create namespace-query answers.
[[nodiscard]] inline std::string_view api_set_namespace_host(std::string_view query) {
    std::string contract(query);
    for (char& ch : contract)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    // apiset_presence_probe (Windows x86/x64): final extension is arbitrary,
    // and the final numeric schema revision does not affect namespace identity.
    if (const auto dot = contract.rfind('.'); dot != std::string::npos)
        contract.resize(dot);
    const auto identity = [](std::string_view name) -> std::string_view {
        const auto dash = name.rfind('-');
        if (dash == std::string_view::npos || dash + 1 == name.size()) return {};
        for (const char ch : name.substr(dash + 1))
            if (ch < '0' || ch > '9') return {};
        return name.substr(0, dash);
    };
    const auto wanted = identity(contract);
    if (wanted.empty()) return {};
    static constexpr struct { std::string_view contract, host; } namespaces[] = {
#include "api_set_observed_hosts.inc"
    };
    for (const auto& route : namespaces) {
        const auto name = route.contract.substr(0, route.contract.size() - 4);
        if (wanted == identity(name)) return route.host;
    }
    return {};
}

} // namespace vitreo::pe
