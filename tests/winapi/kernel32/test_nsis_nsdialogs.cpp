/// @file test_nsis_nsdialogs.cpp
/// @brief Tests for native NSIS plugin DLL loading (WIN32-091)

#include <gtest/gtest.h>

#include <vitreo/common.hpp>

#include <capsule/vfs/vfs.hpp>
#include "capsule/vfs/host_filesystem.hpp"

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <winapi/kernel32/filesystem/file_ops.hpp>
#include <winapi/kernel32/module/module.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

static constexpr GuestAddr STACK_BASE = 0x80000;
static constexpr GuestAddr THUNK_BASE = 0x70000000;

GuestAddr write_wstr(GuestMemory& mem, GuestAddr addr, std::wstring_view s) {
    for (usize i = 0; i < s.size(); ++i) {
        (void)mem.write_u16(addr + static_cast<u32>(i) * 2, static_cast<u16>(s[i]));
    }
    (void)mem.write_u16(addr + static_cast<u32>(s.size()) * 2, 0);
    return addr;
}

std::vector<u8> build_minimal_pe32_dll_with_export(std::string_view dll_name,
                                                   std::string_view export_name,
                                                   u32 export_rva) {
    // Minimal PE32 DLL with 1 section (.text) containing an export directory and 1 function.
    // No imports, no relocations, no entrypoint.
    std::vector<u8> pe(0x400, 0);

    auto w16 = [&](usize off, u16 v) {
        pe[off + 0] = static_cast<u8>(v & 0xFF);
        pe[off + 1] = static_cast<u8>((v >> 8) & 0xFF);
    };
    auto w32 = [&](usize off, u32 v) {
        pe[off + 0] = static_cast<u8>(v & 0xFF);
        pe[off + 1] = static_cast<u8>((v >> 8) & 0xFF);
        pe[off + 2] = static_cast<u8>((v >> 16) & 0xFF);
        pe[off + 3] = static_cast<u8>((v >> 24) & 0xFF);
    };

    // DOS header
    w16(0x00, 0x5A4D); // MZ
    w32(0x3C, 0x80);   // e_lfanew

    // PE signature
    pe[0x80] = 'P';
    pe[0x81] = 'E';
    pe[0x82] = 0;
    pe[0x83] = 0;

    // COFF header @ 0x84
    w16(0x84, 0x014C); // IMAGE_FILE_MACHINE_I386
    w16(0x86, 1);      // NumberOfSections
    w16(0x94, 0x00E0); // SizeOfOptionalHeader (224)
    w16(0x96, 0x2102); // Characteristics: EXECUTABLE | 32BIT | DLL

    // Optional header (PE32) @ 0x98
    const usize opt = 0x98;
    w16(opt + 0x00, 0x010B); // Magic PE32
    w32(opt + 0x10, 0);      // AddressOfEntryPoint
    w32(opt + 0x14, 0x1000); // BaseOfCode
    w32(opt + 0x18, 0x2000); // BaseOfData
    w32(opt + 0x1C, 0x10000000); // ImageBase
    w32(opt + 0x20, 0x1000); // SectionAlignment
    w32(opt + 0x24, 0x200);  // FileAlignment
    w32(opt + 0x38, 0x2000); // SizeOfImage
    w32(opt + 0x3C, 0x200);  // SizeOfHeaders
    w16(opt + 0x44, 2);      // Subsystem (GUI)
    w32(opt + 0x5C, 16);     // NumberOfRvaAndSizes

    // DataDirectory[EXPORT] starts at opt + 0x60
    const usize dd = opt + 0x60;
    w32(dd + 0 * 8 + 0, 0x1100); // Export RVA
    // The directory ends before the function body at 0x1164. An EAT entry
    // that lands *inside* the export directory is a forwarder string, not
    // code, so the declared size must not swallow the exported function.
    w32(dd + 0 * 8 + 4, 0x64);   // Export size

    // Section header @ 0x178
    const usize sh = 0x178;
    pe[sh + 0] = '.';
    pe[sh + 1] = 't';
    pe[sh + 2] = 'e';
    pe[sh + 3] = 'x';
    pe[sh + 4] = 't';
    w32(sh + 8, 0x1000);       // VirtualSize
    w32(sh + 12, 0x1000);      // VirtualAddress
    w32(sh + 16, 0x200);       // SizeOfRawData
    w32(sh + 20, 0x200);       // PointerToRawData
    w32(sh + 36, 0x60000020);  // Characteristics (RX)

    // Export directory at RVA 0x1100 => file offset 0x300
    const usize exp_off = 0x300;
    const u32 dll_name_rva = 0x1150;
    const u32 funcs_rva = 0x1140;
    const u32 names_rva = 0x1144;
    const u32 ords_rva = 0x1148;
    const u32 func_name_rva = 0x1159;

    w32(exp_off + 12, dll_name_rva); // Name
    w32(exp_off + 16, 1);            // Base
    w32(exp_off + 20, 1);            // NumberOfFunctions
    w32(exp_off + 24, 1);            // NumberOfNames
    w32(exp_off + 28, funcs_rva);
    w32(exp_off + 32, names_rva);
    w32(exp_off + 36, ords_rva);

    // Tables live inside the same .text raw data
    w32(0x200 + (funcs_rva - 0x1000), export_rva);
    w32(0x200 + (names_rva - 0x1000), func_name_rva);
    w16(0x200 + (ords_rva - 0x1000), 0);

    // DLL name string at RVA 0x1150
    {
        usize off = 0x200 + (dll_name_rva - 0x1000);
        for (char c : dll_name) {
            pe[off++] = static_cast<u8>(c);
        }
        pe[off++] = 0;
    }
    // Export name string at RVA 0x1159
    {
        usize off = 0x200 + (func_name_rva - 0x1000);
        for (char c : export_name) {
            pe[off++] = static_cast<u8>(c);
        }
        pe[off++] = 0;
    }

    // Function body: single RET
    pe[0x200 + (export_rva - 0x1000)] = 0xC3;

    return pe;
}

std::vector<u8> build_minimal_pe64_dll_with_export(std::string_view dll_name,
                                                   std::string_view export_name,
                                                   u32 export_rva) {
    std::vector<u8> pe(0x400, 0);

    auto w16 = [&](usize off, u16 value) {
        pe[off] = static_cast<u8>(value);
        pe[off + 1] = static_cast<u8>(value >> 8);
    };
    auto w32 = [&](usize off, u32 value) {
        for (usize i = 0; i < 4; ++i) pe[off + i] = static_cast<u8>(value >> (i * 8));
    };
    auto w64 = [&](usize off, u64 value) {
        for (usize i = 0; i < 8; ++i) pe[off + i] = static_cast<u8>(value >> (i * 8));
    };

    w16(0x00, 0x5A4D);
    w32(0x3C, 0x80);
    pe[0x80] = 'P';
    pe[0x81] = 'E';

    w16(0x84, 0x8664);
    w16(0x86, 1);
    w16(0x94, 0x00F0);
    w16(0x96, 0x2022);

    const usize opt = 0x98;
    w16(opt + 0x00, 0x020B);
    w32(opt + 0x10, 0);
    w32(opt + 0x14, 0x1000);
    w64(opt + 0x18, 0x00007FFA'00000000ULL);
    w32(opt + 0x20, 0x1000);
    w32(opt + 0x24, 0x200);
    w32(opt + 0x38, 0x2000);
    w32(opt + 0x3C, 0x200);
    w16(opt + 0x44, 2);
    w32(opt + 0x6C, 16);
    w32(opt + 0x70, 0x1100);
    // Ends before the function body at 0x1170; see the 32-bit builder.
    w32(opt + 0x74, 0x70);

    const usize sh = 0x188;
    std::memcpy(pe.data() + sh, ".text", 5);
    w32(sh + 8, 0x1000);
    w32(sh + 12, 0x1000);
    w32(sh + 16, 0x200);
    w32(sh + 20, 0x200);
    w32(sh + 36, 0x60000020);

    const u32 dll_name_rva = 0x1150;
    const u32 funcs_rva = 0x1140;
    const u32 names_rva = 0x1144;
    const u32 ords_rva = 0x1148;
    const u32 func_name_rva = 0x1160;
    const usize exp_off = 0x300;
    w32(exp_off + 12, dll_name_rva);
    w32(exp_off + 16, 1);
    w32(exp_off + 20, 1);
    w32(exp_off + 24, 1);
    w32(exp_off + 28, funcs_rva);
    w32(exp_off + 32, names_rva);
    w32(exp_off + 36, ords_rva);
    w32(0x200 + funcs_rva - 0x1000, export_rva);
    w32(0x200 + names_rva - 0x1000, func_name_rva);
    w16(0x200 + ords_rva - 0x1000, 0);

    std::memcpy(pe.data() + 0x200 + dll_name_rva - 0x1000,
                dll_name.data(), dll_name.size());
    std::memcpy(pe.data() + 0x200 + func_name_rva - 0x1000,
                export_name.data(), export_name.size());
    pe[0x200 + export_rva - 0x1000] = 0xC3;
    return pe;
}

} // namespace

TEST(NsisPluginDllLoadTest, LoadLibraryExWLoadsPeBackedDllAndResolvesExport) {
    namespace fs = std::filesystem;

    // Create a host directory that acts as our C: drive in the capsule VFS.
    fs::path root = fs::temp_directory_path() / "vitreo_test_c";
    fs::path plugin_dir = root / "Temp" / "nsx1002.tmp";
    fs::create_directories(plugin_dir);

    // Write a minimal PE32 DLL to an NSIS-like temp path.
    const u32 export_rva = 0x1164;
    const auto dll_bytes = build_minimal_pe32_dll_with_export(
        "nsDialogs.dll", "TestFunc", export_rva);

    {
        std::ofstream out(plugin_dir / "nsDialogs.dll", std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out.write(reinterpret_cast<const char*>(dll_bytes.data()),
                  static_cast<std::streamsize>(dll_bytes.size()));
        ASSERT_TRUE(out.good());
    }

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x200000, MemoryProtection::ReadWrite).is_ok());

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());

    FileOpsManager file_ops(memory, vfs);

    ThunkRegistry registry(THUNK_BASE);
    // Ensure the registry contains a thunk DLL entry for nsDialogs.dll.
    // Explicit-path loads must still prefer the PE-backed module.
    ASSERT_TRUE(registry.register_dll("nsDialogs.dll", DllExports{}).is_ok());
    ModuleManager mm(registry);
    mm.attach_dynamic_loader(memory, vfs);
    mm.set_main_module(0x00400000, "C:\\Program Files\\test.exe");
    file_ops.set_module_manager(&mm);

    // Call LoadLibraryExW("C:\\Temp\\nsx1002.tmp\\nsDialogs.dll", nullptr, 0)
    GuestAddr path_ptr = write_wstr(memory, 0x2000, L"C:\\Temp\\nsx1002.tmp\\nsDialogs.dll");

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(STACK_BASE + 0x800);
    GuestAddr esp = cpu.sp();
    (void)memory.write_u32(esp + 4, static_cast<u32>(path_ptr));
    (void)memory.write_u32(esp + 8, 0);
    (void)memory.write_u32(esp + 12, 0);

    Marshaler m(memory);
    m.attach_cpu(cpu);
    file_ops.impl_LoadLibraryExW(m);

    GuestAddr base = static_cast<GuestAddr>(cpu.gpr.rax);
    ASSERT_NE(base, 0u);

    // Verify export resolution comes from the PE export directory.
    GuestAddr proc = mm.get_proc_address(base, "TestFunc");
    ASSERT_NE(proc, 0u);
    EXPECT_EQ(proc, base + export_rva);
}

TEST(NsisPluginDllLoadTest, Pe64DllLoadsInHighDynamicArena) {
    namespace fs = std::filesystem;

    fs::path root = fs::temp_directory_path() / "vitreo_test_pe64_c";
    fs::path dll_dir = root / "Program Files" / "Test";
    fs::create_directories(dll_dir);

    constexpr u32 export_rva = 0x1170;
    const auto dll_bytes = build_minimal_pe64_dll_with_export(
        "Test64.dll", "TestFunc64", export_rva);
    {
        std::ofstream out(dll_dir / "Test64.dll", std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out.write(reinterpret_cast<const char*>(dll_bytes.data()),
                  static_cast<std::streamsize>(dll_bytes.size()));
    }

    GuestMemory memory;
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());

    ThunkRegistry registry(0x00007FFB'00000000ULL);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, true);

    GuestAddr base = modules.load_library("C:\\Program Files\\Test\\Test64.dll");

    ASSERT_EQ(base, 0x00007FFA'00000000ULL);
    EXPECT_EQ(modules.get_proc_address(base, "TestFunc64"), base + export_rva);
}

// Windows unmaps a DLL when FreeLibrary drops its last reference, so the next
// LoadLibrary maps a fresh image whose writable data holds its on-disk initial
// values again. Vitreo keeps PE images mapped, so without an explicit re-map
// the second DLL_PROCESS_ATTACH runs over whatever the first instance's
// DLL_PROCESS_DETACH left behind. steamclient64.dll's CRT zeroes its lowio
// __pioinfo table during detach and then dereferences it on re-attach.
TEST(ModuleReloadTest, ReloadAfterFreeRestoresPristineImageData) {
    namespace fs = std::filesystem;

    fs::path root = fs::temp_directory_path() / "vitreo_test_remap_c";
    fs::path dll_dir = root / "Program Files" / "Test";
    fs::create_directories(dll_dir);

    constexpr u32 export_rva = 0x1164;
    const auto dll_bytes = build_minimal_pe32_dll_with_export(
        "Remap.dll", "TestFunc", export_rva);
    {
        std::ofstream out(dll_dir / "Remap.dll",
                          std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out.write(reinterpret_cast<const char*>(dll_bytes.data()),
                  static_cast<std::streamsize>(dll_bytes.size()));
        ASSERT_TRUE(out.good());
    }

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x200000,
                                  MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());

    ThunkRegistry registry(THUNK_BASE);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs);
    modules.set_main_module(0x00400000, "C:\\Program Files\\test.exe");

    // The builder emits no entry point, so no DllMain is notified; the image
    // refresh is the entire observable effect of the reload here.
    modules.set_guest_call([](GuestAddr, const u64*, u32, bool) -> u64 {
        return 1;
    });

    const std::string path = "C:\\Program Files\\Test\\Remap.dll";
    const GuestAddr base = modules.load_library(path);
    ASSERT_NE(base, 0u);

    // Capture a byte the on-disk image defines, then corrupt it the way a
    // running DLL mutates its own data.
    const GuestAddr probe = base + export_rva;
    auto original = memory.read_u8(probe);
    ASSERT_TRUE(original.is_ok());
    // The builder emits one section at RVA 0x1000; make that region writable
    // so the test can dirty it the way running code would.
    ASSERT_TRUE(memory.protect_region(base + 0x1000, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
    const u8 corrupted = static_cast<u8>(original.value() ^ 0xFFu);
    ASSERT_TRUE(memory.write_u8(probe, corrupted).is_ok());
    ASSERT_EQ(memory.read_u8(probe).value(), corrupted);

    // Last reference released: DLL_PROCESS_DETACH runs and the image is stale.
    ASSERT_TRUE(modules.free_library(base));

    // Loading again must behave like a fresh Windows load.
    const GuestAddr reloaded = modules.load_library(path);
    ASSERT_NE(reloaded, 0u);
    EXPECT_EQ(reloaded, base) << "re-map must reuse the base the guest cached";

    auto after = memory.read_u8(probe);
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(after.value(), original.value())
        << "image data must be restored from disk, not carried over from the "
           "detached instance";

    // Exports still resolve against the re-mapped image.
    EXPECT_EQ(modules.get_proc_address(reloaded, "TestFunc"),
              reloaded + export_rva);
}
