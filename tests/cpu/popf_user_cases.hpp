// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "core/cpu/state/flags.hpp"
#include <array>
namespace vitreo::cpu::popf_test {
struct PopfUserCase { u64 seed, value, wide_expected, narrow_expected; };
// Expected images measured by popf_user_flags_probe{32,64} on Windows;
// Intel SDM Vol.2B POPF, Table4-20, CPL3/IOPL0. Not derived from the emulator.
inline constexpr u64 kPopfBase=FLAG_IF|2;
inline constexpr std::array<PopfUserCase,5> kPopfUserCases{{
 {kPopfBase,2,0x202,0x202},
 {kPopfBase,FLAG_IF|FLAG_IOPL_MASK|FLAG_RF|FLAG_VM|FLAG_VIF|FLAG_VIP|2,0x202,0x202},
 {kPopfBase|FLAG_AC|FLAG_ID,2,0x202,0x240202},
 {kPopfBase,STATUS_FLAGS_MASK|FLAG_DF|FLAG_NT|FLAG_AC|FLAG_ID|2,0x244ed7,0x4ed7},
 {kPopfBase|FLAG_AC|FLAG_ID,FLAG_IF|2,0x202,0x240202},
}};
}
