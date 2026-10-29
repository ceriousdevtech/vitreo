/// @file test_run.cpp
/// @brief Unit tests for CLI-002: vitreoctl run command

#include <gtest/gtest.h>
#include "app/cli/commands/run.hpp"
#include "app/cli/commands/import_setup.hpp"
#include "app/cli/commands/command.hpp"
#include "app/cli/parser/arg_parser.hpp"
#include "app/cli/commands/dll_registry.hpp"
#include "runtime/process/runtime_context.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::cli;

namespace fs = std::filesystem;

// =============================================================================
// Test Fixture
// =============================================================================

class RunCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        char temp_template[] = "/tmp/vitreo_run_test_XXXXXX";
        char* dir = mkdtemp(temp_template);
        ASSERT_NE(dir, nullptr);
        test_dir_ = dir;
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    /// Write raw bytes to a temp file and return its path
    std::string write_temp_file(const std::string& name,
                                const std::vector<u8>& data) {
        auto path = (test_dir_ / name).string();
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
        f.close();
        return path;
    }

    /// Build a minimal valid PE32+ binary with a single .text section whose
    /// entry point is a single RET (0xC3), which returns to the loader's
    /// sentinel return address and exits the process cleanly.
    ///
    /// This used to be HLT (0xF4). HLT is a RING-0 instruction: Intel SDM
    /// Vol. 2 requires #GP(0) at CPL > 0, and Windows surfaces that as
    /// STATUS_PRIVILEGED_INSTRUCTION, so a user-mode HLT terminates the
    /// process with an unhandled exception -- it is not a clean halt. Vitreo
    /// implements that contract (interpreter/ops/misc.cpp), which left every
    /// test here asserting the opposite. RET is the honest way for a minimal
    /// PE to say "ran and exited". See PrivilegedHltRaisesGeneralProtection
    /// for the HLT contract itself.
    static std::vector<u8> build_minimal_pe() {
        // Layout:
        //   0x000 - 0x07F : DOS header (128 bytes, e_lfanew = 0x80)
        //   0x080 - 0x083 : PE signature "PE\0\0"
        //   0x084 - 0x097 : COFF header (20 bytes)
        //   0x098 - 0x107 : Optional header PE32+ (112 bytes)
        //   0x108 - 0x12F : Section header .text (40 bytes)
        //   0x130 - 0x1FF : Padding to file alignment (0x200)
        //   0x200 - 0x3FF : .text raw data (0x200 bytes, first byte = 0xC3)

        constexpr u32 pe_offset       = 0x80;
        constexpr u32 coff_offset     = pe_offset + 4;           // 0x84
        constexpr u32 opt_offset      = coff_offset + 20;        // 0x98
        constexpr u32 opt_size        = 112;                     // PE32+ minimal (no data dirs fully used)
        constexpr u32 sect_offset     = opt_offset + opt_size;   // 0x108
        constexpr u32 file_alignment  = 0x200;
        constexpr u32 section_align   = 0x1000;
        constexpr u32 text_file_off   = file_alignment;          // 0x200
        constexpr u32 text_raw_size   = file_alignment;          // 0x200
        constexpr u32 total_size      = text_file_off + text_raw_size; // 0x400

        std::vector<u8> pe(total_size, 0);

        // ----- DOS Header -----
        auto write16 = [&](u32 off, u16 val) {
            pe[off]     = static_cast<u8>(val & 0xFF);
            pe[off + 1] = static_cast<u8>((val >> 8) & 0xFF);
        };
        auto write32 = [&](u32 off, u32 val) {
            pe[off]     = static_cast<u8>(val & 0xFF);
            pe[off + 1] = static_cast<u8>((val >> 8) & 0xFF);
            pe[off + 2] = static_cast<u8>((val >> 16) & 0xFF);
            pe[off + 3] = static_cast<u8>((val >> 24) & 0xFF);
        };
        auto write64 = [&](u32 off, u64 val) {
            for (int i = 0; i < 8; ++i) {
                pe[off + static_cast<u32>(i)] =
                    static_cast<u8>((val >> (i * 8)) & 0xFF);
            }
        };

        // DOS magic "MZ"
        write16(0x00, 0x5A4D);
        // e_lfanew
        write32(0x3C, pe_offset);

        // ----- PE Signature -----
        write32(pe_offset, 0x00004550); // "PE\0\0"

        // ----- COFF Header (20 bytes at coff_offset) -----
        write16(coff_offset + 0, 0x8664);  // Machine = AMD64
        write16(coff_offset + 2, 1);       // NumberOfSections = 1
        write32(coff_offset + 4, 0);       // TimeDateStamp
        write32(coff_offset + 8, 0);       // PointerToSymbolTable
        write32(coff_offset + 12, 0);      // NumberOfSymbols
        write16(coff_offset + 16, opt_size); // SizeOfOptionalHeader
        write16(coff_offset + 18, 0x0022); // Characteristics: EXECUTABLE | LARGE_ADDRESS_AWARE

        // ----- Optional Header PE32+ (112 bytes at opt_offset) -----
        // Standard fields (24 bytes)
        write16(opt_offset + 0, 0x020B);   // Magic: PE32+
        pe[opt_offset + 2] = 14;           // MajorLinkerVersion
        pe[opt_offset + 3] = 0;            // MinorLinkerVersion
        write32(opt_offset + 4, text_raw_size); // SizeOfCode
        write32(opt_offset + 8, 0);        // SizeOfInitializedData
        write32(opt_offset + 12, 0);       // SizeOfUninitializedData
        write32(opt_offset + 16, 0x1000);  // AddressOfEntryPoint (RVA)
        write32(opt_offset + 20, 0x1000);  // BaseOfCode

        // Windows-specific fields (88 bytes)
        write64(opt_offset + 24, 0x400000);      // ImageBase
        write32(opt_offset + 32, section_align);  // SectionAlignment
        write32(opt_offset + 36, file_alignment); // FileAlignment
        write16(opt_offset + 40, 6);              // MajorOSVersion
        write16(opt_offset + 42, 0);              // MinorOSVersion
        write16(opt_offset + 44, 0);              // MajorImageVersion
        write16(opt_offset + 46, 0);              // MinorImageVersion
        write16(opt_offset + 48, 6);              // MajorSubsystemVersion
        write16(opt_offset + 50, 0);              // MinorSubsystemVersion
        write32(opt_offset + 52, 0);              // Win32VersionValue
        write32(opt_offset + 56, 0x2000);         // SizeOfImage (2 pages)
        write32(opt_offset + 60, file_alignment); // SizeOfHeaders
        write32(opt_offset + 64, 0);              // CheckSum
        write16(opt_offset + 68, 3);              // Subsystem: WINDOWS_CUI
        write16(opt_offset + 70, 0);              // DllCharacteristics
        write64(opt_offset + 72, 0x100000);       // SizeOfStackReserve
        write64(opt_offset + 80, 0x1000);         // SizeOfStackCommit
        write64(opt_offset + 88, 0x100000);       // SizeOfHeapReserve
        write64(opt_offset + 96, 0x1000);         // SizeOfHeapCommit
        write32(opt_offset + 104, 0);             // LoaderFlags
        write32(opt_offset + 108, 0);             // NumberOfRvaAndSizes = 0

        // ----- Section Header: .text (40 bytes at sect_offset) -----
        std::memcpy(&pe[sect_offset], ".text\0\0\0", 8); // Name
        write32(sect_offset + 8,  text_raw_size);  // VirtualSize
        write32(sect_offset + 12, 0x1000);         // VirtualAddress (RVA)
        write32(sect_offset + 16, text_raw_size);  // SizeOfRawData
        write32(sect_offset + 20, text_file_off);  // PointerToRawData
        write32(sect_offset + 24, 0);              // PointerToRelocations
        write32(sect_offset + 28, 0);              // PointerToLinenumbers
        write16(sect_offset + 32, 0);              // NumberOfRelocations
        write16(sect_offset + 34, 0);              // NumberOfLinenumbers
        // Characteristics: CODE | EXECUTE | READ
        write32(sect_offset + 36, 0x60000020);

        // ----- .text section data -----
        pe[text_file_off] = 0xC3; // RET — return to the loader sentinel

        return pe;
    }

    /// Build a minimal PE32+ whose entry point verifies that the loader copied
    /// the complete main-image TLS template into TEB.ThreadLocalStoragePointer.
    /// The final template DWORD models MSVC's per-thread static-init epoch.
    static std::vector<u8> build_minimal_pe_with_tls_epoch() {
        auto pe = build_minimal_pe();
        pe.resize(0x1200, 0);

        auto write16 = [&](u32 off, u16 val) {
            pe[off] = static_cast<u8>(val);
            pe[off + 1] = static_cast<u8>(val >> 8);
        };
        auto write32 = [&](u32 off, u32 val) {
            for (u32 i = 0; i < 4; ++i) {
                pe[off + i] = static_cast<u8>(val >> (i * 8));
            }
        };
        auto write64 = [&](u32 off, u64 val) {
            for (u32 i = 0; i < 8; ++i) {
                pe[off + i] = static_cast<u8>(val >> (i * 8));
            }
        };

        constexpr u32 coff = 0x84;
        constexpr u32 opt = 0x98;
        constexpr u32 section = opt + 240;
        constexpr u32 raw = 0x200;
        constexpr u64 image_base = 0x0000000140000000ULL;

        // Expand the PE32+ optional header to include all 16 data directories.
        std::fill(pe.begin() + 0x108, pe.begin() + section + 40, 0);
        write16(coff + 16, 240);
        write64(opt + 24, image_base);
        write32(opt + 4, 0x1000);   // SizeOfCode
        write32(opt + 8, 0x1000);   // SizeOfInitializedData
        write32(opt + 56, 0x2000);  // SizeOfImage
        write32(opt + 108, 16);     // NumberOfRvaAndSizes

        // IMAGE_DIRECTORY_ENTRY_TLS is data-directory index 9.
        write32(opt + 112 + 9 * 8, 0x1100);
        write32(opt + 112 + 9 * 8 + 4, 40);

        std::memcpy(&pe[section], ".text\0\0\0", 8);
        write32(section + 8, 0x1000);   // VirtualSize
        write32(section + 12, 0x1000);  // VirtualAddress
        write32(section + 16, 0x1000);  // SizeOfRawData
        write32(section + 20, raw);     // PointerToRawData
        write32(section + 36, 0xE0000020); // code, execute, read, write

        // mov rax,gs:[58h]; mov rax,[rax]
        // cmp dword ptr [rax+480h],80000000h; jne ud2
        // cmp byte ptr [rax+1484h],0; jne ud2; xor eax,eax; ret
        //
        // The success terminator was HLT, which is ring-0: it now raises
        // STATUS_PRIVILEGED_INSTRUCTION (see build_minimal_pe), so the "pass"
        // path was killing the process. Returning exits through the loader's
        // sentinel return address instead — and EAX must be zeroed first,
        // because the entry point's return value IS the process exit code and
        // RAX still holds the TLS array pointer this test just loaded.
        // UD2 remains the failure path; both JNE displacements are recomputed
        // for the two extra bytes.
        //
        //   +22  75 0C   jne  -> ud2 at +36
        //   +31  75 03   jne  -> ud2 at +36
        //   +33  31 C0   xor eax, eax
        //   +35  C3      ret
        //   +36  0F 0B   ud2
        const u8 entry[] = {
            0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00,
            0x48, 0x8B, 0x00,
            0x81, 0xB8, 0x80, 0x04, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x80,
            0x75, 0x0C,
            0x80, 0xB8, 0x84, 0x14, 0x00, 0x00, 0x00,
            0x75, 0x03, 0x31, 0xC0, 0xC3, 0x0F, 0x0B,
        };
        std::memcpy(&pe[raw], entry, sizeof(entry));

        // IMAGE_TLS_DIRECTORY64 at RVA 0x1100. The 0x485-byte template ends
        // with MSVC's _Init_thread_epoch at offset 0x480, matching Steam.
        constexpr u32 tls_dir_file = raw + 0x100;
        write64(tls_dir_file + 0, image_base + 0x1180);
        write64(tls_dir_file + 8, image_base + 0x1605);
        write64(tls_dir_file + 16, image_base + 0x1700);
        write64(tls_dir_file + 24, 0);
        // Cross a page boundary so allocating only the raw template cannot
        // accidentally satisfy the zero-fill contract.
        write32(tls_dir_file + 32, 0x1000); // SizeOfZeroFill
        write32(raw + 0x180 + 0x480, 0x80000000u);

        return pe;
    }

    fs::path test_dir_;
};

// =============================================================================
// RunCommand Interface Tests
// =============================================================================

TEST(RunCommandBasicTest, NameReturnsRun) {
    RunCommand cmd;
    EXPECT_EQ(cmd.name(), "run");
}

TEST(RunCommandBasicTest, HelpIsNonEmpty) {
    RunCommand cmd;
    EXPECT_FALSE(cmd.help().empty());
}

TEST(RunCommandBasicTest, HelpContainsExecute) {
    RunCommand cmd;
    auto h = cmd.help();
    EXPECT_NE(h.find("xecut"), std::string_view::npos);
}

TEST(MainImportSearchTest, ExecutableDirectoryPrecedesCurrentDirectory) {
    const auto directories = main_import_search_directories(
        R"(C:\Program Files\Steam\bin\cef\cef.win64\steamwebhelper.exe)",
        R"(C:\Program Files\Steam)");

    // Microsoft Learn, "Dynamic-link library search order" (safe DLL search
    // mode): application folder, system folder, 16-bit system folder, Windows
    // folder, and only then the current folder.
    ASSERT_EQ(directories.size(), 5u);
    EXPECT_EQ(
        directories[0],
        R"(C:\Program Files\Steam\bin\cef\cef.win64)");
    EXPECT_EQ(directories[1], R"(C:\Windows\System32)");
    EXPECT_EQ(directories[2], R"(C:\Windows\System)");
    EXPECT_EQ(directories[3], R"(C:\Windows)");
    EXPECT_EQ(directories[4], R"(C:\Program Files\Steam)");
}

TEST(MainImportSearchTest, EquivalentWindowsPathsAreNotDuplicated) {
    const auto directories = main_import_search_directories(
        R"(C:\Program Files\Steam\steam.exe)",
        R"(c:/program files/steam/)");

    ASSERT_EQ(directories.size(), 4u);
    EXPECT_EQ(directories[0], R"(C:\Program Files\Steam)");
    EXPECT_EQ(directories[1], R"(C:\Windows\System32)");
    EXPECT_EQ(directories[2], R"(C:\Windows\System)");
    EXPECT_EQ(directories[3], R"(C:\Windows)");
}

TEST(MainImportSearchTest, ProbesManifestPrivateAssemblySubdirectories) {
    const std::vector<std::string> assemblies{
        "vendor.runtime.4.2",
        "second_runtime",
    };
    const auto directories = main_import_search_directories(
        R"(C:\Program Files\Browser\Application\browser.exe)",
        R"(C:\Program Files\Browser)",
        assemblies);

    ASSERT_EQ(directories.size(), 7u);
    EXPECT_EQ(
        directories[0],
        R"(C:\Program Files\Browser\Application)");
    EXPECT_EQ(
        directories[1],
        R"(C:\Program Files\Browser\Application\vendor.runtime.4.2)");
    EXPECT_EQ(
        directories[2],
        R"(C:\Program Files\Browser\Application\second_runtime)");
    EXPECT_EQ(directories[3], R"(C:\Windows\System32)");
    EXPECT_EQ(directories[4], R"(C:\Windows\System)");
    EXPECT_EQ(directories[5], R"(C:\Windows)");
    EXPECT_EQ(directories[6], R"(C:\Program Files\Browser)");
}

TEST(RunCommandBasicTest, ApplicationNameUsesWindowsExecutableBasename) {
    EXPECT_EQ(application_name_from_windows_path(
                  R"(C:\Program Files (x86)\Starcraft\StarCraft.exe)"),
              "StarCraft");
}

TEST(RunCommandBasicTest, ApplicationNameHandlesQuotedMixedCaseExtension) {
    EXPECT_EQ(application_name_from_windows_path(
                  R"("D:/Games/Editor/StarEdit.EXE")"),
              "StarEdit");
}

TEST(RunCommandBasicTest, ApplicationNamePreservesNonExeSuffix) {
    EXPECT_EQ(application_name_from_windows_path("C:\\Tools\\setup.com"),
              "setup.com");
}

TEST(RunCommandBasicTest, ApplicationNameRejectsBareExeSuffix) {
    EXPECT_TRUE(application_name_from_windows_path(".exe").empty());
}

TEST(RunCommandBasicTest, DisplayNamePrefersExplicitLaunchName) {
    ApplicationLaunchMetadata metadata;
    metadata.product_name = "PE Product";
    metadata.file_description = "PE Description";
    EXPECT_EQ(
        select_application_display_name(
            " Shortcut Name ", metadata, R"(C:\Games\file.exe)"),
        "Shortcut Name");
}

TEST(RunCommandBasicTest, DisplayNamePrefersDescriptionThenProduct) {
    ApplicationLaunchMetadata metadata;
    metadata.product_name = "Microsoft\xC2\xAE Windows\xC2\xAE Operating System";
    metadata.file_description = "Spider";
    EXPECT_EQ(
        select_application_display_name(
            {}, metadata, R"(C:\Games\file.exe)"),
        "Spider");

    metadata.file_description.clear();
    EXPECT_EQ(
        select_application_display_name(
            {}, metadata, R"(C:\Games\file.exe)"),
        "Microsoft\xC2\xAE Windows\xC2\xAE Operating System");
}

TEST(RunCommandBasicTest, DisplayNameFallsBackToExecutableBasename) {
    EXPECT_EQ(
        select_application_display_name(
            {}, {}, R"(C:\Games\actual-file-name.exe)"),
        "actual-file-name");
}

// =============================================================================
// RunCommand::execute Validation Tests
// =============================================================================

TEST(RunCommandBasicTest, ExecuteWithNoPositionalArgsFails) {
    RunCommand cmd;
    ParsedArgs args;
    args.command = "run";
    auto result = cmd.execute(args);
    EXPECT_NE(result.exit_code, 0);
    EXPECT_FALSE(result.message.empty());
}

TEST_F(RunCommandTest, ExecuteWithNonexistentFileFails) {
    RunCommand cmd;
    ParsedArgs args;
    args.command = "run";
    args.positional_args.push_back("/no/such/file.exe");
    auto result = cmd.execute(args);
    EXPECT_NE(result.exit_code, 0);
}

TEST_F(RunCommandTest, PrelaunchIconExtractionIsEmptyWithoutResources) {
    const auto path = write_temp_file("no-icon.exe", build_minimal_pe());
    EXPECT_TRUE(extract_application_icon_for_launch(path, "default").empty());
}

TEST_F(RunCommandTest, ExecuteDispatchesParsedArgs) {
    // Build a PE, write it, and verify execute returns normally
    auto pe_data = build_minimal_pe();
    auto path = write_temp_file("dispatch.exe", pe_data);

    RunCommand cmd;
    ParsedArgs args;
    args.command = "run";
    args.positional_args.push_back(path);
    args.verbose = false;
    auto result = cmd.execute(args);
    // Should succeed — the PE executes its entry RET and exits
    EXPECT_EQ(result.exit_code, 0);
}

// =============================================================================
// RunConfig Default Value Tests
// =============================================================================

TEST(RunConfigTest, DefaultStackSizeIsSensible) {
    RunConfig cfg;
    // At least 64 KiB
    EXPECT_GE(cfg.stack_size, 0x10000u);
}

TEST(RunConfigTest, DefaultStackBaseIsNonZero) {
    RunConfig cfg;
    EXPECT_NE(cfg.stack_base, 0u);
}

TEST(RunConfigTest, DefaultMaxInstructionsIsUnlimited) {
    RunConfig cfg;
    EXPECT_EQ(cfg.max_instructions, 0u);
}

TEST(RunConfigTest, DefaultVerboseIsFalse) {
    RunConfig cfg;
    EXPECT_FALSE(cfg.verbose);
}

TEST(RunConfigTest, VerboseFlagCanBeSet) {
    RunConfig cfg;
    cfg.verbose = true;
    EXPECT_TRUE(cfg.verbose);
}

// =============================================================================
// RunResult Default State Tests
// =============================================================================

TEST(RunResultTest, DefaultStateIsNotSuccess) {
    RunResult r;
    EXPECT_FALSE(r.success);
}

TEST(RunResultTest, DefaultExitCodeIsZero) {
    RunResult r;
    EXPECT_EQ(r.exit_code, 0);
}

TEST(RunResultTest, DefaultInstructionCountIsZero) {
    RunResult r;
    EXPECT_EQ(r.instructions_executed, 0u);
}

TEST(RunResultTest, DefaultErrorMessageIsEmpty) {
    RunResult r;
    EXPECT_TRUE(r.error_message.empty());
}

// =============================================================================
// execute_pe Error Path Tests
// =============================================================================

TEST(ExecutePeTest, EmptyPathFails) {
    RunConfig cfg;
    cfg.pe_path = "";
    auto result = execute_pe(cfg);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.exit_code, 0);
    EXPECT_FALSE(result.error_message.empty());
}

TEST(ExecutePeTest, NonexistentFileFails) {
    RunConfig cfg;
    cfg.pe_path = "/no/such/path/does_not_exist.exe";
    auto result = execute_pe(cfg);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.exit_code, 0);
}

TEST_F(RunCommandTest, InvalidPeDataFailsParse) {
    // Write random garbage that is not a valid PE
    std::vector<u8> garbage(512, 0xCC);
    auto path = write_temp_file("garbage.exe", garbage);

    RunConfig cfg;
    cfg.pe_path = path;
    auto result = execute_pe(cfg);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.exit_code, 0);
    // Should mention parse error
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(RunCommandTest, TruncatedPeFailsParse) {
    // Just a DOS header with valid MZ but truncated before PE offset
    std::vector<u8> truncated(32, 0);
    truncated[0] = 0x4D; // 'M'
    truncated[1] = 0x5A; // 'Z'
    auto path = write_temp_file("truncated.exe", truncated);

    RunConfig cfg;
    cfg.pe_path = path;
    auto result = execute_pe(cfg);
    EXPECT_FALSE(result.success);
}

// =============================================================================
// execute_pe With Minimal Valid PE Tests
// =============================================================================

TEST_F(RunCommandTest, MinimalPeExecutesAndExits) {
    auto pe_data = build_minimal_pe();
    auto path = write_temp_file("ret.exe", pe_data);

    RunConfig cfg;
    cfg.pe_path = path;
    auto result = execute_pe(cfg);

    // Returning from the entry point exits the process cleanly
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_GE(result.instructions_executed, 1u);
}

TEST_F(RunCommandTest, MinimalPeWithVerboseFlag) {
    auto pe_data = build_minimal_pe();
    auto path = write_temp_file("ret_verbose.exe", pe_data);

    RunConfig cfg;
    cfg.pe_path = path;
    cfg.verbose = true;
    // Should not crash or fail just because verbose is set
    auto result = execute_pe(cfg);
    EXPECT_TRUE(result.success);
}

TEST_F(RunCommandTest, MinimalPeReportsInstructionCount) {
    auto pe_data = build_minimal_pe();
    auto path = write_temp_file("ret_count.exe", pe_data);

    RunConfig cfg;
    cfg.pe_path = path;
    auto result = execute_pe(cfg);
    EXPECT_TRUE(result.success);
    // At least the entry RET should be counted
    EXPECT_GE(result.instructions_executed, 1u);
}

TEST_F(RunCommandTest, X64MainImageTlsPreservesMsvcThreadEpoch) {
    const auto path = write_temp_file(
        "tls_epoch.exe", build_minimal_pe_with_tls_epoch());

    RunConfig cfg;
    cfg.pe_path = path;
    cfg.max_instructions = 100;
    const auto result = execute_pe(cfg);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.exit_code, 0);
}

TEST_F(RunCommandTest, MinimalPeWithMaxInstructions) {
    auto pe_data = build_minimal_pe();
    auto path = write_temp_file("ret_max.exe", pe_data);

    RunConfig cfg;
    cfg.pe_path = path;
    cfg.max_instructions = 1000;
    auto result = execute_pe(cfg);
    EXPECT_TRUE(result.success);
}

// Nothing guarded this contract: every test that touched HLT asserted the
// OPPOSITE (that it halts cleanly), so when HLT was correctly changed to fault
// they all failed and the new behaviour itself went unguarded.
//
// Intel SDM Vol. 2, HLT: "If the LOCK prefix is used ... #UD. If CPL > 0,
// #GP(0)." Vitreo runs Windows user-mode code only, so the guest is always at
// CPL > 0. Windows surfaces that #GP as STATUS_PRIVILEGED_INSTRUCTION and, with
// no handler, terminates the process — it must NOT look like a clean exit.
TEST_F(RunCommandTest, UserModeHltFaultsInsteadOfHaltingCleanly) {
    auto pe_data = build_minimal_pe();
    // Entry point becomes HLT instead of RET.
    pe_data[0x200] = 0xF4;
    auto path = write_temp_file("hlt.exe", pe_data);

    RunConfig cfg;
    cfg.pe_path = path;
    auto result = execute_pe(cfg);

    EXPECT_FALSE(result.success)
        << "a user-mode HLT reported a clean run; it is a privileged "
           "instruction and must fault";
    EXPECT_NE(result.exit_code, 0);
}

TEST_F(RunCommandTest, EmptyFileFailsParseOrRead) {
    auto path = write_temp_file("empty.exe", {});

    RunConfig cfg;
    cfg.pe_path = path;
    auto result = execute_pe(cfg);
    EXPECT_FALSE(result.success);
}

// kernel32!X and kernelbase!X must be one implementation. KernelBase took the
// FIRST registration of a duplicated kernel32 export while ThunkRegistry binds
// the LAST for kernel32, so kernelbase!TzSpecificLocalTimeToSystemTime was a
// stub that copied local time to UTC unchanged (s4_kernelbase_surface_probe:
// Windows 11 gives 17:34 UTC through both DLLs for 12:34 at Bias=300).
TEST(KernelBaseSurfaceTest, SharedExportsUseTheImplementationKernel32Binds) {
    using namespace vitreo;
    vitreo::GuestMemory memory;
    constexpr GuestAddr kStack = 0x00080000;
    constexpr GuestAddr kData = 0x00100000;
    ASSERT_TRUE(memory.map_region(kStack, 0x10000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cli::RuntimeContext ctx{memory, "kernelbase_surface.exe", "default", false, false,
                            false, nullptr, false, std::string{}, {}, nullptr};
    cli::register_all_dlls(ctx, memory, /*is_64bit=*/false);

    constexpr GuestAddr tz = kData;          // TIME_ZONE_INFORMATION (172 bytes)
    constexpr GuestAddr local = kData + 0x100;
    constexpr GuestAddr utc = kData + 0x120;
    for (const char* dll : {"kernel32.dll", "kernelbase.dll"}) {
        for (u32 i = 0; i < 0x140; i += 4) ASSERT_TRUE(memory.write_u32(kData + i, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(tz, 300).is_ok());           // Bias: UTC-5
        ASSERT_TRUE(memory.write_u16(local + 0, 2026).is_ok());    // wYear
        ASSERT_TRUE(memory.write_u16(local + 2, 1).is_ok());       // wMonth
        ASSERT_TRUE(memory.write_u16(local + 6, 15).is_ok());      // wDay
        ASSERT_TRUE(memory.write_u16(local + 8, 12).is_ok());      // wHour
        ASSERT_TRUE(memory.write_u16(local + 10, 34).is_ok());     // wMinute
        auto thunk = ctx.thunk_registry.get_thunk_address(
            std::string(dll), std::string("TzSpecificLocalTimeToSystemTime"));
        ASSERT_TRUE(thunk.is_ok()) << dll;
        auto handler = ctx.thunk_registry.resolve_handler(thunk.value());
        ASSERT_TRUE(handler.is_ok()) << dll;
        cpu::CpuState cpu{};
        cpu.init_protected_mode();
        const GuestAddr esp = kStack + 0x8000;
        cpu.set_sp(esp);
        ASSERT_TRUE(memory.write_u32(esp + 4, static_cast<u32>(tz)).is_ok());
        ASSERT_TRUE(memory.write_u32(esp + 8, static_cast<u32>(local)).is_ok());
        ASSERT_TRUE(memory.write_u32(esp + 12, static_cast<u32>(utc)).is_ok());
        winapi::Marshaler m(memory);
        m.attach_cpu(cpu);
        (*handler.value())(m);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u) << dll;
        EXPECT_EQ(memory.read_u16(utc + 8).value_or(0), 17u) << dll;   // wHour
        EXPECT_EQ(memory.read_u16(utc + 10).value_or(0), 34u) << dll;  // wMinute
    }
}
