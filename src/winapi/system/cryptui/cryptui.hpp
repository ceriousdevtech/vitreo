#pragma once

/// @file cryptui.hpp
/// @brief cryptui.dll certificate-viewer dialog contracts.
///
/// Exports (stdcall_bytes = 8):
///   CryptUIDlgViewCertificateW
///   CryptUIDlgViewCertificateA
///
/// dll_registry:
///   (void)reg.register_dll("cryptui.dll", winapi::create_cryptui_thunks());

#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

DllExports create_cryptui_thunks();

}  // namespace vitreo::winapi
