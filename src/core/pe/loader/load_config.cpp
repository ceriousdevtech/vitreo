// SPDX-License-Identifier: LGPL-2.1-only

/// @file load_config.cpp
/// @brief Windows loader processing for PE load-configuration data.

#include "core/pe/loader/load_config.hpp"

#include <cstdlib>
#include <vector>

namespace vitreo::pe {
namespace {

constexpr usize LOAD_CONFIG32_SECURITY_COOKIE_OFFSET = 0x3C;
constexpr usize LOAD_CONFIG64_SECURITY_COOKIE_OFFSET = 0x58;
constexpr usize LOAD_CONFIG32_GUARD_CF_CHECK_OFFSET = 0x48;
constexpr usize LOAD_CONFIG32_GUARD_CF_DISPATCH_OFFSET = 0x4C;
constexpr usize LOAD_CONFIG64_GUARD_CF_CHECK_OFFSET = 0x70;
constexpr usize LOAD_CONFIG64_GUARD_CF_DISPATCH_OFFSET = 0x78;
constexpr u32 DEFAULT_SECURITY_COOKIE32 = 0xBB40E64Eu;
constexpr u64 DEFAULT_SECURITY_COOKIE64 = 0x00002B992DDFA232ull;
constexpr u64 SECURITY_COOKIE64_MASK = 0x0000FFFFFFFFFFFFull;

template <typename T>
[[nodiscard]] T make_security_cookie();

template <>
u32 make_security_cookie<u32>() {
    u32 cookie = 0;
    do {
        arc4random_buf(&cookie, sizeof(cookie));
    } while (cookie == 0 || cookie == DEFAULT_SECURITY_COOKIE32 ||
             (cookie & 0xFFFF0000u) == 0);
    return cookie;
}

// XP-era /GS images carry a 16-bit preset cookie (high word zero, e.g.
// 0x0000BB40) and their __security_check_cookie fast-fails ANY cookie whose
// high word is nonzero — the zero byte is the deliberate guard against
// string-based overwrites. The Windows loader keeps that shape: a
// replacement cookie for such an image is masked to 16 bits.
[[nodiscard]] u32 make_security_cookie16() {
    u32 cookie = 0;
    do {
        arc4random_buf(&cookie, sizeof(cookie));
        cookie &= 0xFFFFu;
    } while (cookie == 0);
    return cookie;
}

template <>
u64 make_security_cookie<u64>() {
    u64 cookie = 0;
    do {
        arc4random_buf(&cookie, sizeof(cookie));
        cookie &= SECURITY_COOKIE64_MASK;
    } while (cookie == 0 || cookie == DEFAULT_SECURITY_COOKIE64);
    return cookie;
}

template <typename T>
[[nodiscard]] bool write_load_config_value(GuestMemory& memory,
                                           GuestAddr address,
                                           T value) {
    GuestAddr region_base = 0;
    usize region_size = 0;
    MemoryProtection old_protection = MemoryProtection::None;
    if (!memory.query_region(address, region_base, region_size,
                             old_protection)) {
        return false;
    }

    const bool needs_write = !has_flag(old_protection, MemoryProtection::Write);
    if (needs_write) {
        auto protect = memory.protect_region(
            region_base, region_size, old_protection | MemoryProtection::Write);
        if (protect.is_err()) {
            return false;
        }
    }

    const auto write = [&]() {
        if constexpr (sizeof(T) == sizeof(u64)) {
            return memory.write_u64(address, value);
        } else {
            return memory.write_u32(address, value);
        }
    }();

    bool restored = true;
    if (needs_write) {
        restored = memory.protect_region(
            region_base, region_size, old_protection).is_ok();
    }
    return write.is_ok() && restored;
}

} // namespace

bool initialize_load_config_security_cookie(GuestMemory& memory,
                                            const ParsedPe& pe,
                                            GuestAddr image_base) {
    if (pe.num_data_directories <= DIR_LOAD_CONFIG) {
        return true;
    }

    const auto& directory = pe.data_directories[DIR_LOAD_CONFIG];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(u32)) {
        return true;
    }

    const GuestAddr load_config = image_base + directory.VirtualAddress;
    const auto structure_size = memory.read_u32(load_config);
    if (structure_size.is_err()) {
        return false;
    }

    const usize cookie_offset = pe.is_64bit
        ? LOAD_CONFIG64_SECURITY_COOKIE_OFFSET
        : LOAD_CONFIG32_SECURITY_COOKIE_OFFSET;
    const usize pointer_size = pe.is_64bit ? sizeof(u64) : sizeof(u32);
    if (directory.Size < cookie_offset + pointer_size ||
        structure_size.value() < cookie_offset + pointer_size) {
        return true;
    }

    if (pe.is_64bit) {
        const auto address = memory.read_u64(load_config + cookie_offset);
        if (address.is_err()) {
            return false;
        }
        if (address.value() == 0) {
            return true;
        }
        // Same replace-only-the-default contract as the 32-bit path below;
        // x64 has no 16-bit legacy shape, but a non-default preset likewise
        // belongs to the image's own CRT init.
        const auto preset = memory.read_u64(address.value());
        if (preset.is_err()) {
            return false;
        }
        if (preset.value() != DEFAULT_SECURITY_COOKIE64 &&
            preset.value() != 0) {
            return true;
        }
        return write_load_config_value(
            memory, address.value(), make_security_cookie<u64>());
    }

    const auto address = memory.read_u32(load_config + cookie_offset);
    if (address.is_err()) {
        return false;
    }
    if (address.value() == 0) {
        return true;
    }
    // The Windows loader replaces the preset cookie only when it is the
    // compiler default or has a zero high word (the XP-era 16-bit shape); an
    // image with any other preset keeps it — its own CRT init owns it. A
    // 16-bit preset gets a 16-bit replacement so the image's high-word-zero
    // check keeps passing.
    const auto preset = memory.read_u32(address.value());
    if (preset.is_err()) {
        return false;
    }
    const bool preset_is_16bit = (preset.value() & 0xFFFF0000u) == 0;
    if (preset.value() != DEFAULT_SECURITY_COOKIE32 && !preset_is_16bit) {
        return true;
    }
    return write_load_config_value(
        memory, address.value(),
        preset_is_16bit ? make_security_cookie16()
                        : make_security_cookie<u32>());
}

GuardCfStubs install_guard_cf_stubs(GuestMemory& memory) {
    // Immediately above the 32-bit CRT accelerator arena (0x7D200000), so the
    // synthetic-code arenas stay one block at the top of the 2 GB user window.
    // find_free_gap keeps a 64-bit guest — whose layout has no fixed constants
    // here — off anything already mapped.
    constexpr GuestAddr STUB_MIN_BASE = 0x7D201000;
    constexpr usize STUB_PAGE_SIZE = 0x1000;
    constexpr usize DISPATCH_OFFSET = 16;

    static GuardCfStubs cached;
    static bool installed = false;
    if (installed) {
        return cached;
    }
    installed = true;

    const GuestAddr base = memory.find_free_gap(STUB_MIN_BASE, STUB_PAGE_SIZE);
    if (base == 0 ||
        memory.map_region(base, STUB_PAGE_SIZE, MemoryProtection::ReadWrite)
            .is_err()) {
        return cached;
    }

    // 0xCC fill so a stray branch into the page traps instead of running on.
    std::vector<Byte> page(STUB_PAGE_SIZE, 0xCC);
    // _guard_check_icall_nop: the target is in ECX/RCX and every register,
    // including the flags, is preserved. A bare RET does exactly that.
    page[0] = 0xC3;
    // _guard_dispatch_icall_nop: the target is in EAX/RAX and the CALL that
    // reached here already pushed the final target's return address, so this
    // must tail-jump rather than call. FF /4 is `jmp eax` in a 32-bit image
    // and `jmp rax` in a 64-bit one.
    page[DISPATCH_OFFSET] = 0xFF;
    page[DISPATCH_OFFSET + 1] = 0xE0;

    if (memory.write_bytes(base, page.data(), page.size()).is_err()) {
        return cached;
    }
    (void)memory.protect_region(base, STUB_PAGE_SIZE,
                                MemoryProtection::ReadExecute);

    cached.check = base;
    cached.dispatch = base + DISPATCH_OFFSET;
    return cached;
}

bool initialize_load_config_guard_cf(GuestMemory& memory,
                                     const ParsedPe& pe,
                                     GuestAddr image_base,
                                     GuestAddr check_target,
                                     GuestAddr dispatch_target) {
    if (pe.num_data_directories <= DIR_LOAD_CONFIG) {
        return true;
    }

    const auto& directory = pe.data_directories[DIR_LOAD_CONFIG];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(u32)) {
        return true;
    }

    const GuestAddr load_config = image_base + directory.VirtualAddress;
    const auto structure_size = memory.read_u32(load_config);
    if (structure_size.is_err()) {
        return false;
    }

    const usize check_offset = pe.is_64bit
        ? LOAD_CONFIG64_GUARD_CF_CHECK_OFFSET
        : LOAD_CONFIG32_GUARD_CF_CHECK_OFFSET;
    const usize dispatch_offset = pe.is_64bit
        ? LOAD_CONFIG64_GUARD_CF_DISPATCH_OFFSET
        : LOAD_CONFIG32_GUARD_CF_DISPATCH_OFFSET;
    const usize pointer_size = pe.is_64bit ? sizeof(u64) : sizeof(u32);
    if (directory.Size < dispatch_offset + pointer_size ||
        structure_size.value() < dispatch_offset + pointer_size) {
        return true;
    }

    if (pe.is_64bit) {
        const auto check_variable = memory.read_u64(load_config + check_offset);
        const auto dispatch_variable =
            memory.read_u64(load_config + dispatch_offset);
        if (check_variable.is_err() || dispatch_variable.is_err()) {
            return false;
        }
        if (check_variable.value() != 0 &&
            !write_load_config_value(memory, check_variable.value(),
                                     static_cast<u64>(check_target))) {
            return false;
        }
        return dispatch_variable.value() == 0 ||
               write_load_config_value(memory, dispatch_variable.value(),
                                       static_cast<u64>(dispatch_target));
    }

    const auto check_variable = memory.read_u32(load_config + check_offset);
    const auto dispatch_variable = memory.read_u32(load_config + dispatch_offset);
    if (check_variable.is_err() || dispatch_variable.is_err()) {
        return false;
    }
    if (check_variable.value() != 0 &&
        !write_load_config_value(memory, check_variable.value(),
                                 static_cast<u32>(check_target))) {
        return false;
    }
    return dispatch_variable.value() == 0 ||
           write_load_config_value(memory, dispatch_variable.value(),
                                   static_cast<u32>(dispatch_target));
}

} // namespace vitreo::pe
