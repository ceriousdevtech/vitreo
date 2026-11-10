// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <vitreo/common.hpp>

namespace vitreo::cpu::jit::compile_test {
// The callback runs under that compiler's mutex, immediately before the late
// memo probe. Tests install it before starting callers and remove it after
// joining them. It must not reenter the same compiler.
using BeforeBackendHook = void (*)(GuestAddr);
void set_before_backend_hook(BeforeBackendHook hook) noexcept;
}
