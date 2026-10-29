/// @file test_kernel32_cpuz_module_resources.cpp
/// @brief Unit tests for KERNEL32 module handle, resource, DLL directory and activation-context contracts

#include "kernel32_cpuz_fixture.hpp"
#include <winapi/kernel32/module/module.hpp>
#include <algorithm>

TEST_F(Kernel32CpuzTest, LoadResourceReturnsDirectlyReadableMappedAddress) {
    constexpr GuestAddr image_base = DATA_BASE;
    constexpr u32 resource_rva = 0x1000;
    constexpr GuestAddr resource_base = image_base + resource_rva;
    constexpr GuestAddr resource_data = image_base + 0x3000;
    constexpr u32 resource_value = 0x52455321u;

    auto write_u16 = [&](u32 offset, u16 value) {
        ASSERT_TRUE(memory.write_u16(resource_base + offset, value).is_ok());
    };
    auto write_u32 = [&](u32 offset, u32 value) {
        ASSERT_TRUE(memory.write_u32(resource_base + offset, value).is_ok());
    };
    // Root -> RT_STRING(6) -> ID 6 -> language 0x0409 -> data entry.
    write_u16(12, 0);
    write_u16(14, 1);
    write_u32(16, 6);
    write_u32(20, 0x80000018u);
    write_u16(0x18 + 12, 0);
    write_u16(0x18 + 14, 1);
    write_u32(0x18 + 16, 6);
    write_u32(0x18 + 20, 0x80000030u);
    write_u16(0x30 + 12, 0);
    write_u16(0x30 + 14, 1);
    write_u32(0x30 + 16, 0x0409);
    write_u32(0x30 + 20, 0x48);
    write_u32(0x48, 0x3000);
    write_u32(0x4C, sizeof(resource_value));
    ASSERT_TRUE(memory.write_u32(resource_data, resource_value).is_ok());
    mgr.set_resource_info(image_base, resource_rva, 0x1000);

    auto exports = create_kernel32_ext_thunks(mgr);
    auto find_export = [&](std::string_view name) -> ExportEntry* {
        auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& entry) { return entry.name == name; });
        return it == exports.entries.end() ? nullptr : &*it;
    };
    const GuestAddr stack = cpu.sp();

    ASSERT_TRUE(memory.write_u32(stack + 4, image_base).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 6).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 6).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0x0409).is_ok());
    auto* find_resource = find_export("FindResourceExA");
    ASSERT_NE(find_resource, nullptr);
    find_resource->handler(marshaler);
    const u32 resource_handle = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(resource_handle, 0u);

    ASSERT_TRUE(memory.write_u32(stack + 4, image_base).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, resource_handle).is_ok());
    auto* load_resource = find_export("LoadResource");
    ASSERT_NE(load_resource, nullptr);
    load_resource->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), resource_data);
    EXPECT_EQ(
        memory.read_u32(static_cast<GuestAddr>(cpu.gpr.rax)).value_or(0),
        resource_value);

    ASSERT_TRUE(memory.write_u32(stack + 4, resource_data).is_ok());
    auto* lock_resource = find_export("LockResource");
    ASSERT_NE(lock_resource, nullptr);
    lock_resource->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), resource_data);

    ASSERT_TRUE(memory.write_u32(stack + 4, image_base).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, resource_handle).is_ok());
    auto* sizeof_resource = find_export("SizeofResource");
    ASSERT_NE(sizeof_resource, nullptr);
    sizeof_resource->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), sizeof(resource_value));
}

TEST_F(Kernel32CpuzTest, GetModuleHandleExFromAddressAcquiresReference) {
    constexpr GuestAddr image_base = 0x180000000ull;
    constexpr GuestAddr module_out = DATA_BASE;
    ThunkRegistry registry{0x7FFB00000000ull};
    ModuleManager modules{registry};
    modules.set_main_module(image_base, "C:\\Modules\\sample.dll");
    modules.register_main_module_exports(
        image_base, "C:\\Modules\\sample.dll", image_base + 0x100000,
        64, std::span<const u8>{});
    mgr.set_module_manager(&modules);

    cpu.init_long_mode();
    cpu.gpr.rcx = 0x04;  // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
    cpu.gpr.rdx = image_base + 0x1234;
    cpu.gpr.r8 = module_out;

    mgr.impl_GetModuleHandleExW(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u64(module_out).value(), image_base);
    const ModuleInfo* acquired = modules.get_module_info_by_handle(image_base);
    ASSERT_NE(acquired, nullptr);
    EXPECT_EQ(acquired->ref_count, 2u);

    ASSERT_TRUE(modules.free_library(image_base));
    EXPECT_EQ(modules.get_module_info_by_handle(image_base)->ref_count, 1u);
}

TEST_F(Kernel32CpuzTest, LoadStringWMissingResourceReturnsZero) {
    // Learn winuser.h: missing string → 0. A fail-thunk 0xFFFFFFFF is a
    // length and becomes std::bad_alloc.
    auto exports = create_kernel32_ext_thunks(mgr);
    auto load_string = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "LoadStringW";
        });
    ASSERT_NE(load_string, exports.entries.end());

    cpu.init_long_mode();
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8 = DATA_BASE;
    cpu.gpr.r9 = 64;
    load_string->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(Kernel32CpuzTest, RtlPcToFileHeaderReturnsContainingImageNotMainExe) {
    // Microsoft Learn winnt.h: RtlPcToFileHeader returns the base of the
    // mapped image that contains PcValue. Returning the process EXE for
    // every PC makes x64 MSVC throws from a DLL publish the wrong
    // ExceptionInformation[3] image base.
    constexpr GuestAddr exe_base = 0x140000000ull;
    constexpr GuestAddr dll_base = 0x180000000ull;
    constexpr GuestAddr base_out = DATA_BASE;
    ThunkRegistry registry{0x7FFB00000000ull};
    ModuleManager modules{registry};
    modules.set_main_module(exe_base, "C:\\App\\app.exe");
    modules.register_main_module_exports(
        exe_base, "C:\\App\\app.exe", exe_base + 0x200000,
        64, std::span<const u8>{});
    modules.register_main_module_exports(
        dll_base, "C:\\App\\helper.dll", dll_base + 0x100000,
        64, std::span<const u8>{});
    mgr.set_module_manager(&modules);

    auto exports = create_kernel32_ext_thunks(mgr);
    auto pc_to_header = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "RtlPcToFileHeader";
        });
    ASSERT_NE(pc_to_header, exports.entries.end());

    cpu.init_long_mode();
    cpu.gpr.rcx = dll_base + 0x1234;
    cpu.gpr.rdx = base_out;
    pc_to_header->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dll_base);
    EXPECT_EQ(memory.read_u64(base_out).value(), dll_base);

    cpu.gpr.rcx = 0x2000;
    cpu.gpr.rdx = base_out;
    pc_to_header->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(base_out).value(), 0u);
}

TEST_F(Kernel32CpuzTest, GetModuleHandleExUnchangedRefcountDoesNotAcquire) {
    constexpr GuestAddr image_base = 0x180000000ull;
    constexpr GuestAddr module_out = DATA_BASE;
    ThunkRegistry registry{0x7FFB00000000ull};
    ModuleManager modules{registry};
    modules.set_main_module(image_base, "C:\\Modules\\sample.dll");
    modules.register_main_module_exports(
        image_base, "C:\\Modules\\sample.dll", image_base + 0x100000,
        64, std::span<const u8>{});
    mgr.set_module_manager(&modules);

    cpu.init_long_mode();
    cpu.gpr.rcx = 0x06;  // FROM_ADDRESS | UNCHANGED_REFCOUNT
    cpu.gpr.rdx = image_base + 0x1234;
    cpu.gpr.r8 = module_out;

    mgr.impl_GetModuleHandleExW(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u64(module_out).value(), image_base);
    const ModuleInfo* acquired = modules.get_module_info_by_handle(image_base);
    ASSERT_NE(acquired, nullptr);
    EXPECT_EQ(acquired->ref_count, 1u);
}

// =============================================================================
// DLL / IO
// =============================================================================

TEST_F(Kernel32CpuzTest, SetDllDirectoryA_ReturnsTRUE) {
    ThunkRegistry registry{0x70000000};
    ModuleManager modules{registry};
    mgr.set_module_manager(&modules);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0);  // lpPathName (NULL)
    mgr.impl_SetDllDirectoryA(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(Kernel32CpuzTest, CreateActCtxActivateDeactivateRelease) {
    constexpr GuestAddr actctx = DATA_BASE;
    constexpr GuestAddr cookie = DATA_BASE + 0x40;
    ASSERT_TRUE(memory.write_u32(actctx, 32).is_ok());
    ASSERT_TRUE(memory.write_u32(actctx + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(actctx + 8, 0).is_ok());

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), actctx);
    mgr.impl_CreateActCtxA(marshaler);
    const u32 handle = static_cast<u32>(cpu.gpr.rax);
    EXPECT_NE(handle, INVALID_HANDLE);
    EXPECT_NE(handle, 0u);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), handle);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), cookie);
    mgr.impl_ActivateActCtx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    const u32 cookie_value = memory.read_u32(cookie).value();
    EXPECT_NE(cookie_value, 0u);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), cookie_value);
    mgr.impl_DeactivateActCtx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), handle);
    mgr.impl_ReleaseActCtx(marshaler);
}
