/// @file test_module_exports.cpp
/// @brief Tests for ModuleManager GetProcAddress, forwarded exports and make_module_exports

#include "module_test_fixture.hpp"
#include "core/pe/imports/api_set_availability.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"

namespace {

/// A 64-bit DLL exporting one ordinary function and one forwarder to
/// "kernel32.TestFunc". The forwarder's EAT entry points back inside the
/// export directory, which is how the PE format marks a forwarded export.
std::vector<u8> build_forwarder_test_dll() {
    auto data = build_notification_test_dll(/*is_64bit=*/true);
    constexpr u32 pe_offset = 0x80;
    constexpr u32 section_offset = 0x200;
    constexpr u32 export_rva = 0x1100;
    constexpr u32 export_size = 0x100;

    const usize optional_offset =
        pe_offset + sizeof(u32) + sizeof(pe::CoffHeader);
    auto* optional = reinterpret_cast<pe::OptionalHeader64*>(
        data.data() + optional_offset);
    optional->DataDirectory[pe::DIR_EXPORT] = {export_rva, export_size};

    // The base helper declares a 1-byte section; widen it so the export
    // directory's RVAs fall inside it.
    auto* coff = reinterpret_cast<pe::CoffHeader*>(
        data.data() + pe_offset + sizeof(u32));
    auto* section = reinterpret_cast<pe::SectionHeader*>(
        data.data() + optional_offset + coff->SizeOfOptionalHeader);
    section->VirtualSize = 0x200;

    const auto file_of = [&](u32 rva) {
        return static_cast<usize>(section_offset + (rva - 0x1000));
    };
    const auto put32 = [&](u32 rva, u32 value) {
        std::memcpy(data.data() + file_of(rva), &value, sizeof(value));
    };
    const auto put16 = [&](u32 rva, u16 value) {
        std::memcpy(data.data() + file_of(rva), &value, sizeof(value));
    };
    const auto put_str = [&](u32 rva, const char* text) {
        std::memcpy(data.data() + file_of(rva), text, std::strlen(text) + 1);
    };

    constexpr u32 functions_rva = 0x1130;
    constexpr u32 names_rva     = 0x1138;
    constexpr u32 ordinals_rva  = 0x1140;
    constexpr u32 direct_name_rva    = 0x1144;
    constexpr u32 forwarded_name_rva = 0x1150;
    constexpr u32 forwarder_str_rva  = 0x1160;
    constexpr u32 dll_name_rva       = 0x1180;

    pe::ExportDirectory dir{};
    dir.Name = dll_name_rva;
    dir.Base = 1;
    dir.NumberOfFunctions = 2;
    dir.NumberOfNames = 2;
    dir.AddressOfFunctions = functions_rva;
    dir.AddressOfNames = names_rva;
    dir.AddressOfNameOrdinals = ordinals_rva;
    std::memcpy(data.data() + file_of(export_rva), &dir, sizeof(dir));

    // Ordinal 1 is ordinary code; ordinal 2's "address" is the forwarder
    // string, which lives inside [export_rva, export_rva + export_size).
    put32(functions_rva + 0, 0x1000);
    put32(functions_rva + 4, forwarder_str_rva);
    put32(names_rva + 0, direct_name_rva);
    put32(names_rva + 4, forwarded_name_rva);
    put16(ordinals_rva + 0, 0);
    put16(ordinals_rva + 2, 1);
    put_str(direct_name_rva, "DirectFunc");
    put_str(forwarded_name_rva, "ForwardedFunc");
    put_str(forwarder_str_rva, "kernel32.TestFunc");
    put_str(dll_name_rva, "fwdtest.dll");
    return data;
}

}  // namespace

// =============================================================================
// Forwarded export tests
// =============================================================================

TEST_F(ModuleManagerTest, ForwardedExportResolvesToTheTargetModulesFunction) {
    constexpr GuestAddr FWD_BASE = 0x50000000;
    const auto image = build_forwarder_test_dll();
    mm_->register_main_module_exports(FWD_BASE, "C:\\fwdtest.dll",
                                      FWD_BASE + 0x2000, 64,
                                      std::span<const u8>(image.data(),
                                                          image.size()));

    // The ordinary export still resolves to code inside the image.
    EXPECT_EQ(mm_->get_proc_address(FWD_BASE, "DirectFunc"), FWD_BASE + 0x1000);

    // The forwarded one must resolve to kernel32!TestFunc, NOT to the address
    // of the "kernel32.TestFunc" string. Calling the string would fault with
    // STATUS_ILLEGAL_INSTRUCTION.
    const GuestAddr expected =
        mm_->get_proc_address(ModuleManager::KERNEL32_BASE, "TestFunc");
    ASSERT_NE(expected, 0u);
    EXPECT_EQ(mm_->get_proc_address(FWD_BASE, "ForwardedFunc"), expected);
    EXPECT_EQ(mm_->get_proc_address_by_ordinal(FWD_BASE, 2), expected);

    // Guard against the regression directly: the resolved address must not
    // land inside the forwarder image, where the string lives.
    const GuestAddr resolved = mm_->get_proc_address(FWD_BASE, "ForwardedFunc");
    EXPECT_TRUE(resolved < FWD_BASE || resolved >= FWD_BASE + 0x2000);
}

// =============================================================================
// GetProcAddress Tests
// =============================================================================

TEST_F(ModuleManagerTest, GetProcAddressKnownFunctionReturnsNonZero) {
    GuestAddr addr = mm_->get_proc_address(ModuleManager::KERNEL32_BASE, "TestFunc");
    EXPECT_NE(addr, 0u);
}

TEST_F(ModuleManagerTest, GetProcAddressAnotherKnownFunction) {
    GuestAddr addr = mm_->get_proc_address(ModuleManager::KERNEL32_BASE, "AnotherFunc");
    EXPECT_NE(addr, 0u);
}

TEST_F(ModuleManagerTest, GetProcAddressUnknownFunctionReturnsZero) {
    GuestAddr addr = mm_->get_proc_address(ModuleManager::KERNEL32_BASE, "NoSuchFunc");
    EXPECT_EQ(addr, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_PROC_NOT_FOUND);
}

TEST_F(ModuleManagerTest, GetProcAddressInvalidModuleReturnsZero) {
    GuestAddr addr = mm_->get_proc_address(0xDEADBEEF, "TestFunc");
    EXPECT_EQ(addr, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MOD_NOT_FOUND);
}

TEST_F(ModuleManagerTest, GetProcAddressByOrdinalKnown) {
    GuestAddr addr = mm_->get_proc_address_by_ordinal(ModuleManager::KERNEL32_BASE, 1);
    EXPECT_NE(addr, 0u);
}

TEST_F(ModuleManagerTest, GetProcAddressByOrdinalUnknown) {
    GuestAddr addr = mm_->get_proc_address_by_ordinal(ModuleManager::KERNEL32_BASE, 999);
    EXPECT_EQ(addr, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_PROC_NOT_FOUND);
}

TEST_F(ModuleManagerTest, GetProcAddressDrvExtensionUsesThunkKey) {
    // normalize_name keeps ".drv"; GetProcAddress must not append ".dll"
    // (that produced "winspool.drv.dll" and missed the registered exports).
    DllExports spool_exports;
    spool_exports.entries.push_back(
        ExportEntry{"EnumPortsA", 0, [](Marshaler&) {}, 24});
    ASSERT_TRUE(registry_->register_dll("winspool.drv", std::move(spool_exports)).is_ok());

    constexpr GuestAddr spool_base = 0x6F220000;
    mm_->register_module("winspool.drv", spool_base,
                         "C:\\Windows\\System32\\winspool.drv");
    EXPECT_EQ(mm_->get_module_handle("WINSPOOL.DRV"), spool_base);

    set_last_error(0xDEADu);
    GuestAddr addr = mm_->get_proc_address(spool_base, "EnumPortsA");
    EXPECT_NE(addr, 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)
}

// =============================================================================
// make_module_exports
// =============================================================================

TEST_F(ModuleManagerTest, MakeModuleExportsHasCorrectEntryCount) {
    auto exports = make_module_exports(*mm_);
    EXPECT_EQ(exports.entries.size(), 11u);
}

TEST_F(ModuleManagerTest, MakeModuleExportsContainsExpectedNames) {
    auto exports = make_module_exports(*mm_);

    std::vector<std::string> names;
    names.reserve(exports.entries.size());
    for (const auto& entry : exports.entries) {
        names.push_back(entry.name);
    }

    EXPECT_NE(std::find(names.begin(), names.end(), "GetModuleHandleA"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "GetModuleHandleW"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "GetProcAddress"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "LoadLibraryA"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "LoadLibraryW"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "FreeLibrary"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "GetModuleFileNameA"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "GetModuleFileNameW"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "DisableThreadLibraryCalls"),
              names.end());
}

TEST_F(ModuleManagerTest, MakeModuleExportsHandlersAreCallable) {
    auto exports = make_module_exports(*mm_);
    for (const auto& entry : exports.entries) {
        EXPECT_TRUE(static_cast<bool>(entry.handler))
            << "Handler for " << entry.name << " is null";
    }
}

TEST_F(ModuleManagerTest, ApiSetAvailabilityDoesNotPromiseLegacyFallbacks) {
    auto exports=make_module_exports(*mm_);
    const auto entry=std::find_if(exports.entries.begin(),exports.entries.end(),
        [](const ExportEntry& e){return e.name=="IsApiSetImplemented";});
    ASSERT_NE(entry,exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes,4u);
    for(bool x64:{false,true}){
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000,0x10000,MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if(x64)cpu.init_long_mode();else cpu.init_protected_mode();
        cpu.gpr.rsp=0x18000;
        Marshaler m(memory);m.attach_cpu(cpu);
        for(const char* name:{"api-ms-win-madeup-l1-1-0","api-ms-win-core-synch-l1-1-0", "api-ms-win-crt-math-l1-1-0", "kernel32.dll", ""}){
            ASSERT_TRUE(m.write_ansi_string(0x10000,name).is_ok());
            if(x64)cpu.gpr.rcx=0x10000;else(void)memory.write_u32(0x18004,0x10000);
            set_last_error(0x1234);entry->handler(m);
            EXPECT_EQ(cpu.gpr.rax,0u)<<name;
            EXPECT_EQ(get_last_error(),0x1234u);
        }
    }
}

// Expected ABI/status/validation observations come from the real Windows
// apiset_presence_probe{32,64}.expected, rather than from the implementation.
TEST_F(ModuleManagerTest, ApiSetPresenceUsesObservedNamespaceAndRegisteredHost) {
    const auto exports = make_module_exports(*mm_);
    const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "ApiSetQueryApiSetPresence"; });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 8u);
    for (bool x64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x18000;
        Marshaler m(memory); m.attach_cpu(cpu);
        constexpr GuestAddr descriptor = 0x10000, buffer = 0x11000, output = 0x14000;
        const auto prepare = [&](const char* name, int length_delta = 0, u16 maximum = 0) {
            EXPECT_TRUE(m.write_wide_string(buffer, name).is_ok());
            const u16 bytes = static_cast<u16>(std::strlen(name) * sizeof(u16) + length_delta);
            EXPECT_TRUE(memory.write_u16(descriptor, bytes).is_ok());
            EXPECT_TRUE(memory.write_u16(descriptor + sizeof(u16), maximum).is_ok());
            EXPECT_TRUE(m.write_ptr(descriptor + (x64 ? sizeof(u64) : sizeof(u32)), buffer).is_ok());
        };
        const auto call = [&](GuestAddr ns = descriptor, GuestAddr result = output) {
            cpu.pending_raise.active = false;
            if (x64) { cpu.gpr.rcx = ns; cpu.gpr.rdx = result; }
            else { (void)memory.write_u32(cpu.gpr.rsp + 4, static_cast<u32>(ns));
                   (void)memory.write_u32(cpu.gpr.rsp + 8, static_cast<u32>(result)); }
            EXPECT_TRUE(memory.write_u32(output, 0x5a5a5aa5).is_ok());
            set_last_error(0x12345678);
            entry->handler(m);
            EXPECT_EQ(get_last_error(), 0x12345678u);
        };
        // The fixture initially registers kernel32 but no kernelbase host.
        const bool host_registered = registry_->has_dll("kernelbase.dll");
        prepare("api-ms-win-core-processthreads-l1-1-0");
        call(); EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(memory.read_u32(output).value(), host_registered ? 0x5a5a5a01u : 0x5a5a5a00u);
        if (!registry_->has_dll("kernelbase.dll"))
            ASSERT_TRUE(registry_->register_dll("kernelbase.dll", DllExports{}).is_ok());
        for (const char* name : {"api-ms-win-core-processthreads-l1-1-0",
                                 "API-MS-WIN-CORE-PROCESSTHREADS-L1-1-0",
                                 "api-ms-win-core-processthreads-l1-1-0.dll",
                                 "api-ms-win-core-processthreads-l1-1-0.exe",
                                 "api-ms-win-core-processthreads-l1-1-0.abc",
                                 "api-ms-win-core-processthreads-l1-1-99"}) {
            prepare(name); call();
            EXPECT_FALSE(cpu.pending_raise.active) << name;
            EXPECT_EQ(cpu.gpr.rax, 0u) << name;
            EXPECT_EQ(memory.read_u32(output).value(), 0x5a5a5a01u) << name;
        }
        prepare("api-ms-win-core-processthreads-l1-1-0-garbage");
        ASSERT_TRUE(memory.write_u16(descriptor, static_cast<u16>(
            std::strlen("api-ms-win-core-processthreads-l1-1-0") * sizeof(u16))).is_ok());
        call(); EXPECT_EQ(memory.read_u8(output).value(), 1u);
        prepare("api-ms-win-core-processthreads-l1-1-0.x");
        ASSERT_TRUE(memory.write_u16(buffer +
            std::strlen("api-ms-win-core-processthreads-l1-1-0.") * sizeof(u16), 0x4e2d).is_ok());
        call(); EXPECT_EQ(memory.read_u8(output).value(), 0u);
        // MaximumLength is ignored, odd byte length is truncated to WCHARs,
        // and a counted terminator is not part of a valid namespace name.
        prepare("api-ms-win-core-processthreads-l1-1-0", 1, 2); call();
        EXPECT_EQ(memory.read_u8(output).value(), 1u);
        prepare("api-ms-win-core-processthreads-l1-1-0", 2); call();
        EXPECT_EQ(memory.read_u8(output).value(), 0u);
        for (const char* name : {"api-ms-win-madeup-l1-1-0", "kernel32", "",
                                 "C:\\api-ms-win-core-processthreads-l1-1-0.dll"}) {
            prepare(name); call(); EXPECT_EQ(cpu.gpr.rax, 0u);
            EXPECT_EQ(memory.read_u32(output).value(), 0x5a5a5a00u) << name;
        }
        // Presence does not certify the semantics of every API in a set.
        EXPECT_FALSE(pe::is_api_set_implemented("api-ms-win-core-processthreads-l1-1-0"));
        // A legacy family fallback alone declares no namespace.
        EXPECT_TRUE(pe::api_set_namespace_host("api-ms-win-core-synch-l1-1-0").empty());
    }
}

TEST_F(ModuleManagerTest, ApiSetPresenceRaisesGuestFaultsAndAvoidsShortBufferReads) {
    const auto exports = make_module_exports(*mm_);
    const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "ApiSetQueryApiSetPresence"; });
    ASSERT_NE(entry, exports.entries.end());
    for (bool x64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x18000;
        Marshaler m(memory); m.attach_cpu(cpu);
        constexpr GuestAddr descriptor = 0x10000, buffer = 0x11000, output = 0x14000;
        ASSERT_TRUE(m.write_wide_string(buffer, "api-ms-win-madeup-l1-1-0").is_ok());
        ASSERT_TRUE(memory.write_u16(descriptor, 48).is_ok());
        ASSERT_TRUE(m.write_ptr(descriptor + (x64 ? sizeof(u64) : sizeof(u32)), buffer).is_ok());
        const auto call = [&](GuestAddr ns, GuestAddr result) {
            cpu.pending_raise.active = false;
            if (x64) { cpu.gpr.rcx = ns; cpu.gpr.rdx = result; }
            else { (void)memory.write_u32(cpu.gpr.rsp + 4, static_cast<u32>(ns));
                   (void)memory.write_u32(cpu.gpr.rsp + 8, static_cast<u32>(result)); }
            EXPECT_TRUE(memory.write_u32(output, 0x5a5a5aa5).is_ok());
            set_last_error(0x12345678); entry->handler(m);
            EXPECT_EQ(get_last_error(), 0x12345678u);
        };
        for (GuestAddr ns : {GuestAddr{0}, GuestAddr{1}}) {
            call(ns, output);
            EXPECT_TRUE(cpu.pending_raise.active);
            EXPECT_EQ(cpu.pending_raise.code, 0xc0000005u);
            EXPECT_EQ(cpu.pending_raise.args[0], 0u);
            EXPECT_EQ(cpu.pending_raise.args[1], ns);
            EXPECT_EQ(memory.read_u32(output).value(), 0x5a5a5aa5u);
        }
        for (GuestAddr result : {GuestAddr{0}, GuestAddr{1}}) {
            call(descriptor, result);
            EXPECT_TRUE(cpu.pending_raise.active);
            EXPECT_EQ(cpu.pending_raise.args[0], 1u);
            EXPECT_EQ(cpu.pending_raise.args[1], result);
        }
        ASSERT_TRUE(m.write_ptr(descriptor + (x64 ? sizeof(u64) : sizeof(u32)), 1).is_ok());
        ASSERT_TRUE(memory.write_u16(descriptor, 8).is_ok());
        call(descriptor, output);
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(memory.read_u32(output).value(), 0x5a5a5a00u);
        ASSERT_TRUE(memory.write_u16(descriptor, 10).is_ok());
        call(descriptor, output);
        EXPECT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.pending_raise.args[0], 0u);
        EXPECT_EQ(cpu.pending_raise.args[1], 1u);
        EXPECT_EQ(memory.read_u32(output).value(), 0x5a5a5aa5u);
    }
}
