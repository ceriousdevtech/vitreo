/// @file test_opengl32_registration.cpp
/// @brief opengl32.dll as register_all_dlls publishes it: the export table
/// is Windows' (opengl32_oracle_exports.inc), and the extension entry points
/// wglGetProcAddress hands out live inside opengl32, not in a module of
/// their own.

#include <gtest/gtest.h>

#include "app/cli/commands/dll_registry.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/process/runtime_context.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/error.hpp"

#include <set>
#include <string>

using namespace vitreo;

namespace {

// The 7 names GDI Generic's wglGetProcAddress returns non-NULL for
// (gl11_state_probe64.expected ext-gpa).
constexpr const char* kExtensionNames[] = {
    "glAddSwapHintRectWIN",          "glColorTableEXT",
    "glColorSubTableEXT",            "glGetColorTableEXT",
    "glGetColorTableParameterivEXT", "glGetColorTableParameterfvEXT",
    "glDrawRangeElementsWIN",
};

std::set<std::string> windows_opengl32_exports() {
    std::set<std::string> names;
#define OPENGL32_ORACLE_EXPORT(name, ord32, ord64, stack) names.insert(#name);
#include "../winapi/graphics/opengl32_oracle_exports.inc"
#undef OPENGL32_ORACLE_EXPORT
    return names;
}

}  // namespace

// Windows ships no module beyond opengl32.dll for these entry points: the
// registry names no other GL DLL, System32 and SysWOW64 list no such file,
// and LoadLibrary of the name Vitreo once used is ERROR_MOD_NOT_FOUND. The
// entry points are still reachable as internal opengl32 entries, and
// opengl32's published export table stays exactly Windows'.
TEST(Opengl32Registration, ExtensionEntryPointsLiveInsideOpengl32) {
    for (const bool is64 : {false, true}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        vitreo::GuestMemory memory;
        cli::RuntimeContext ctx{memory, "gl_registration.exe", "default", is64,
                                false, false, nullptr, false, std::string{},
                                {}, nullptr};
        cli::register_all_dlls(ctx, memory, is64);
        const winapi::ThunkRegistry& reg = ctx.thunk_registry;

        EXPECT_FALSE(reg.has_dll("vitreo-opengl32-ext.dll"));
        for (const std::string& dll : reg.dll_names()) {
            EXPECT_EQ(dll.find("opengl32-ext"), std::string::npos) << dll;
        }
        for (const char* dir : {"C:\\Windows\\System32", "C:\\Windows\\SysWOW64"}) {
            SCOPED_TRACE(dir);
            bool has_opengl32 = false;
            for (const auto& [name, size] : ctx.module_mgr.builtin_dll_files_in(dir)) {
                EXPECT_EQ(name.find("opengl32-ext"), std::string::npos) << name;
                if (name == "opengl32.dll") has_opengl32 = true;
            }
            EXPECT_TRUE(has_opengl32);
            EXPECT_EQ(ctx.module_mgr.builtin_dll_file(
                          std::string(dir) + "\\vitreo-opengl32-ext.dll"),
                      nullptr);
        }

        winapi::set_last_error(0);
        EXPECT_EQ(ctx.module_mgr.load_library("vitreo-opengl32-ext.dll"), 0u);
        EXPECT_EQ(winapi::get_last_error(), winapi::error_codes::ERROR_MOD_NOT_FOUND);

        // Internal entries: found only with include_internal, never through
        // the module's GetProcAddress or the export enumeration.
        const GuestAddr opengl32 = ctx.module_mgr.load_library("opengl32.dll");
        ASSERT_NE(opengl32, 0u);
        for (const char* name : kExtensionNames) {
            SCOPED_TRACE(name);
            EXPECT_TRUE(reg.get_thunk_address("opengl32.dll", name, true).is_ok());
            EXPECT_TRUE(reg.get_thunk_address("opengl32.dll", name).is_err());
            EXPECT_EQ(ctx.module_mgr.get_proc_address(opengl32, name), 0u);
        }

        std::set<std::string> published;
        for (const auto& e : reg.enumerate_exports()) {
            if (e.dll_name == "opengl32.dll") published.insert(e.function_name);
        }
        EXPECT_EQ(published, windows_opengl32_exports());
    }
}
