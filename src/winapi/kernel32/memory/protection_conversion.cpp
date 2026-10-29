// SPDX-License-Identifier: LGPL-2.1-only

/// @file protection_conversion.cpp
/// @brief Win32 PAGE_* ↔ host MemoryProtection, including modifiers.

#include "winapi/kernel32/memory/memory.hpp"

namespace vitreo::winapi {

namespace {

constexpr u32 kBaseMask = 0xFFu;
constexpr u32 kModifierMask =
    PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE | PAGE_TARGETS_INVALID;

[[nodiscard]] u32 base_protection(u32 win32_prot) noexcept {
    return win32_prot & kBaseMask;
}

}  // namespace

MemoryProtection win32_prot_to_native(u32 win32_prot) noexcept {
    switch (base_protection(win32_prot)) {
        case PAGE_READONLY:          return MemoryProtection::Read;
        case PAGE_READWRITE:         return MemoryProtection::ReadWrite;
        case PAGE_WRITECOPY:         return MemoryProtection::ReadWrite;
        case PAGE_EXECUTE:           return MemoryProtection::Execute;
        case PAGE_EXECUTE_READ:      return MemoryProtection::ReadExecute;
        case PAGE_EXECUTE_READWRITE: return MemoryProtection::ReadWriteExecute;
        case PAGE_EXECUTE_WRITECOPY: return MemoryProtection::ReadWriteExecute;
        case PAGE_NOACCESS:          return MemoryProtection::None;
        default:                     return MemoryProtection::None;
    }
}

namespace {

[[nodiscard]] bool valid_protection_mask(u32 win32_prot,
                                         bool allow_writecopy) noexcept {
    // Reject unknown bits (PAGE_ENCLAVE_*, PAGE_GRAPHICS_*, etc. measured as
    // ERROR_INVALID_PARAMETER on Windows for ordinary VirtualAlloc/Protect).
    if ((win32_prot & ~(kBaseMask | kModifierMask)) != 0) {
        return false;
    }

    const u32 base = base_protection(win32_prot);
    const u32 mods = win32_prot & kModifierMask;
    const bool guard = (mods & PAGE_GUARD) != 0;
    const bool nocache = (mods & PAGE_NOCACHE) != 0;
    const bool writecombine = (mods & PAGE_WRITECOMBINE) != 0;
    const bool targets = (mods & PAGE_TARGETS_INVALID) != 0;

    // Learn: PAGE_GUARD / NOCACHE / WRITECOMBINE cannot combine with each
    // other or with PAGE_NOACCESS. WRITECOPY forms are rejected by
    // VirtualAlloc everywhere and by VirtualProtect on private memory only
    // (virtual_memory_contract_probe valloc 0x8/0x80;
    // writecopy_protect_probe img/map wc vs priv wc).
    switch (base) {
        case PAGE_NOACCESS:
            return mods == 0;
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
            break;
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_WRITECOPY:
            if (!allow_writecopy) return false;
            break;
        default:
            return false;
    }

    if (guard && (nocache || writecombine)) return false;
    if (nocache && writecombine) return false;
    // PAGE_TARGETS_* requires an execute protection (probe:
    // targets_no_update|rw fails; |er succeeds).
    if (targets) {
        switch (base) {
            case PAGE_EXECUTE:
            case PAGE_EXECUTE_READ:
            case PAGE_EXECUTE_READWRITE:
                break;
            default:
                return false;
        }
    }
    return true;
}

}  // namespace

bool is_supported_win32_protection(u32 win32_prot) noexcept {
    return valid_protection_mask(win32_prot, false);
}

bool is_valid_protect_request(u32 win32_prot) noexcept {
    return valid_protection_mask(win32_prot, true);
}

bool is_writecopy_win32_protection(u32 win32_prot) noexcept {
    const u32 base = base_protection(win32_prot);
    return base == PAGE_WRITECOPY || base == PAGE_EXECUTE_WRITECOPY;
}

bool is_execute_win32_protection(u32 win32_prot) noexcept {
    switch (base_protection(win32_prot)) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

u32 stored_win32_protection(u32 win32_prot, bool from_virtual_protect) noexcept {
    u32 stored = win32_prot & ~PAGE_TARGETS_INVALID;
    // VirtualProtect accepts NOCACHE/WRITECOMBINE but does not keep them in
    // VirtualQuery Protect (virtual_memory_contract_probe: vprot nocache /
    // vprot wc). VirtualAlloc RESERVE|COMMIT does keep them.
    if (from_virtual_protect) {
        stored &= ~(PAGE_NOCACHE | PAGE_WRITECOMBINE);
    }
    return stored;
}

u32 stored_image_win32_protection(u32 win32_prot) noexcept {
    u32 stored = stored_win32_protection(win32_prot, true);
    switch (base_protection(stored)) {
        case PAGE_READWRITE:
            return (stored & ~kBaseMask) | PAGE_WRITECOPY;
        case PAGE_EXECUTE_READWRITE:
            return (stored & ~kBaseMask) | PAGE_EXECUTE_WRITECOPY;
        default:
            return stored;
    }
}

u32 native_prot_to_win32(MemoryProtection prot) noexcept {
    switch (prot) {
        case MemoryProtection::Read:
            return PAGE_READONLY;
        case MemoryProtection::ReadWrite:
            return PAGE_READWRITE;
        case MemoryProtection::Execute:
            return PAGE_EXECUTE;
        case MemoryProtection::ReadExecute:
            return PAGE_EXECUTE_READ;
        case MemoryProtection::ReadWriteExecute:
            return PAGE_EXECUTE_READWRITE;
        case MemoryProtection::None:
        default:
            return PAGE_NOACCESS;
    }
}

} // namespace vitreo::winapi
