#pragma once

/// @file d3d11_in_chain.hpp
/// @brief The in-chain contract class (winapi::InChainClass) of each D3D11
/// COM method, derived from the method's documented contract.
///
/// The class is a property of the METHOD, so it is keyed by the method's
/// name as the SDK header puts it in its vtable slot (CONTEXT_METHOD_NAMES,
/// generated from d3d11_3.h and checked by tools/audit_d3d11_vtables.py),
/// never by a hand-typed slot number and never by what a run observed. The
/// rule table in d3d11_in_chain.cpp names every one of the 149
/// ID3D11DeviceContext4 methods (D3D11InChainClassTest checks that no slot
/// falls through), each with the Microsoft Learn contract it rests on.

#include "winapi/core/thunk_registry.hpp"

#include <string_view>

namespace vitreo::winapi::d3d11 {

/// One method's class and the contract it rests on.
struct InChainRule {
    std::string_view method;
    InChainClass cls;
    /// The documented behaviour the class rests on, for the inventory.
    std::string_view reason;
};

/// The rule naming @p method (an ID3D11DeviceContext4 method name), or null
/// when no rule does -- a method no rule names is never in-chain.
[[nodiscard]] const InChainRule* context_in_chain_rule(
    std::string_view method) noexcept;

/// The class of ID3D11DeviceContext4 vtable slot @p slot: its method's
/// rule, None for a slot past the table or a method no rule names.
[[nodiscard]] InChainClass context_method_in_chain_class(usize slot) noexcept;

/// The class of slot @p slot of a D3D11 device or device-child interface
/// (ID3D11Device5, ID3D11Buffer, the views, shaders, states, queries, ...):
/// its IUnknown methods (slots 0-2, the same three on every COM interface)
/// are classified -- QueryInterface and AddRef A, Release AGuestComRef (a
/// final Release destroys the object and releases its private-data
/// interfaces) -- and every later, interface-specific slot is None.
[[nodiscard]] InChainClass device_child_in_chain_class(usize slot) noexcept;

}  // namespace vitreo::winapi::d3d11
