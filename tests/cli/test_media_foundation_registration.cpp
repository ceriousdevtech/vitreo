// SPDX-License-Identifier: LGPL-2.1-only

#include <gtest/gtest.h>

#include "app/cli/commands/dll_registry.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/process/runtime_context.hpp"
#include "winapi/core/thunk_registry.hpp"

using namespace vitreo;

// Until the Media Session exists (WS8 M6/M7), mf.dll is not a built-in
// module: a guest that needs shlwapi/mf/mfplat/mfplay together (Unreal's
// WindowsMoviePlayer) must find mf.dll missing and leave Media Foundation
// alone, instead of reaching MFStartup and asserting on its refusal.
// mfplat.dll and mfplay.dll stay: mfplay is implemented on the host media
// backend and does not depend on mf.dll.
TEST(MediaFoundationRegistration, MfDllIsAbsentUntilTheMediaSessionExists) {
    for (const bool is64 : {false, true}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        vitreo::GuestMemory memory;
        cli::RuntimeContext ctx{memory, "mf_registration.exe", "default", is64,
                                false, false, nullptr, false, std::string{},
                                {}, nullptr};
        cli::register_all_dlls(ctx, memory, is64);
        const winapi::ThunkRegistry& reg = ctx.thunk_registry;

        EXPECT_FALSE(reg.has_dll("mf.dll"));
        EXPECT_TRUE(reg.has_dll("mfplat.dll"));
        EXPECT_TRUE(reg.has_dll("mfplay.dll"));
    }
}
