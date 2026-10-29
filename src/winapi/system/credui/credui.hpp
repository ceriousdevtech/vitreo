#pragma once

/// @file credui.hpp
/// @brief credui.dll CredUIPromptForWindowsCredentialsW/A.
/// Exports: CredUIPromptForWindowsCredentialsW, CredUIPromptForWindowsCredentialsA
/// stdcall_bytes: 36 (9 * 4) for both.
/// Register: (void)reg.register_dll("credui.dll", winapi::create_credui_thunks());

#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

DllExports create_credui_thunks();

}  // namespace vitreo::winapi
