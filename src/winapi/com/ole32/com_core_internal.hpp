// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file com_core_internal.hpp
/// @brief File-local HRESULTs, interface IDs and helpers that more than one
/// of the com_*.cpp translation units implementing ComCoreManager use. Names
/// that only one of those files needs stay in that file. Everything sits in
/// an anonymous namespace: each including TU gets its own internal-linkage
/// copy. Include from the com_core*.cpp / com_*.cpp files only, LAST among
/// the project includes (these names must not collide with anything a later
/// header declares in vitreo::winapi).

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"

namespace vitreo::winapi {

namespace {

// HRESULTs used by more than one of the com_*.cpp files.
constexpr u32 E_NOINTERFACE = 0x80004002;
constexpr u32 E_OUTOFMEMORY = 0x8007000E;
constexpr u32 E_FAIL = 0x80004005;
constexpr u32 RPC_E_SERVERFAULT = 0x80010105;
constexpr u32 CO_E_NOTINITIALIZED = 0x800401F0u;
constexpr u32 kEOutOfMemory = 0x8007000Eu;

// CLSIDs and IIDs used by more than one of the com_*.cpp files.
constexpr Guid CLSID_SHELLLINK{0x00021401u, 0x0000u, 0x0000u, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
constexpr Guid IID_INETWORKLISTMANAGER{
    0xDCB00000u, 0x570Fu, 0x4A9Bu, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
constexpr Guid IID_IDISPATCH{
    0x00020400u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr Guid IID_ICONNECTIONPOINTCONTAINER{
    0xB196B284u, 0xBAB4u, 0x101Au, {0xB6, 0x9C, 0x00, 0xAA, 0x00, 0x34, 0x1D, 0x07}};
constexpr Guid IID_IGLOBALOPTIONS{0x0000015Bu, 0x0000u, 0x0000u, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
constexpr Guid IID_ISHELLLINKA{0x000214EEu, 0x0000u, 0x0000u, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
constexpr Guid IID_ISHELLLINKW{0x000214F9u, 0x0000u, 0x0000u, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
constexpr Guid IID_IPERSISTFILE{0x0000010Bu, 0x0000u, 0x0000u, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

// NLM_CONNECTIVITY — Microsoft Learn netlistmgr.h
constexpr u32 NLM_CONNECTIVITY_IPV4_SUBNET = 0x10;
constexpr u32 NLM_CONNECTIVITY_IPV4_LOCALNETWORK = 0x20;
constexpr u32 NLM_CONNECTIVITY_IPV4_INTERNET = 0x40;
constexpr u32 NLM_CONNECTIVITY_CONNECTED =
    NLM_CONNECTIVITY_IPV4_SUBNET | NLM_CONNECTIVITY_IPV4_LOCALNETWORK |
    NLM_CONNECTIVITY_IPV4_INTERNET;
constexpr u16 VARIANT_TRUE_VALUE = 0xFFFFu;

// Helpers used by more than one of the com_*.cpp files.
[[maybe_unused]] [[nodiscard]] u32 automation_value_slots(u16 vt, bool is_64bit) {
    if (is_64bit) return 1;
    switch (vt) {
        case 5:  // VT_R8
        case 6:  // VT_CY
        case 7:  // VT_DATE
        case 20: // VT_I8
        case 21: // VT_UI8
            return 2;
        default:
            return 1;
    }
}

[[maybe_unused]] [[nodiscard]] GuestAddr guest_teb(Marshaler& m) {
    if (!m.has_cpu()) return 0;
    const auto* c = m.cpu();
    return static_cast<GuestAddr>(m.cpu_is_64bit() ? c->seg.gs_desc.base
                                                   : c->seg.fs_desc.base);
}

} // namespace

} // namespace vitreo::winapi
