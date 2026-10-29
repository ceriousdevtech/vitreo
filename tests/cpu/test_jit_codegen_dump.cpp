// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_jit_codegen_dump.cpp
/// @brief Offline compile of guest code windows, for codegen-density work.
///
/// A diagnostic, not a gate: skipped unless VITREO_CODEGEN_DUMP_DIR names a
/// directory of `<va-hex>.bin` code windows (raw guest bytes that belong at
/// that virtual address) and VITREO_CODEGEN_DUMP_RIPS lists block starts.
/// Each block is decoded with the runtime's own block decoder, compiled at
/// tier 1 (the tier a hot block runs at) over a linear64 reservation, and
/// printed one emitted word per line:
///
///     [codegen] block 0x... guest_bytes=N insns=N hot_words=N cold_words=N
///     [codegen] +OFF WORD
///
/// so tools can classify the words and compare emitted bytes per guest byte
/// before and after an emitter change. Nothing here knows which image the
/// windows came from.

#if defined(__aarch64__)
#include <gtest/gtest.h>

#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/block_decoder.hpp"
#include "ir_tier1_first_compile.hpp"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

TEST(JitCodegenDump, CompileListedBlocks) {
    const char* dir = std::getenv("VITREO_CODEGEN_DUMP_DIR");
    const char* rips = std::getenv("VITREO_CODEGEN_DUMP_RIPS");
    if (dir == nullptr || rips == nullptr)
        GTEST_SKIP() << "set VITREO_CODEGEN_DUMP_DIR and VITREO_CODEGEN_DUMP_RIPS";

    // VITREO_CODEGEN_DUMP_TIER0=1 compiles at the runtime's default tiering
    // (a first compile is tier 0: stencils plus an entry counter), which is
    // what a block the IR declines keeps running live.
    std::optional<vitreo::cpu::jit::test::IrTier1FirstCompile> tier1;
    if (std::getenv("VITREO_CODEGEN_DUMP_TIER0") == nullptr) tier1.emplace();
    vitreo::cli::refresh_decode_env_flags();
    GuestMemory memory;
    constexpr usize reservation = usize{1} << 46;
    ASSERT_TRUE(install_linear_fault_handler());
    ASSERT_TRUE(memory.enable_linear_reservation(reservation));
    ASSERT_TRUE(memory.enable_linear_hardware_protection());
    ASSERT_TRUE(register_linear_reservation(memory.linear_base(), reservation));

    DIR* d = ::opendir(dir);
    ASSERT_NE(d, nullptr);
    while (dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name.size() < 5 || name.substr(name.size() - 4) != ".bin") continue;
        const GuestAddr va = std::strtoull(name.c_str(), nullptr, 16);
        std::ifstream in(std::string(dir) + "/" + name, std::ios::binary);
        std::vector<u8> bytes((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // Windows may overlap; map page by page, skipping pages already mapped.
        for (usize off = 0; off < bytes.size(); off += 0x1000) {
            if (memory.is_mapped(va + off)) continue;
            ASSERT_TRUE(memory.map_region(va + off, 0x1000,
                                          MemoryProtection::ReadWrite));
            ASSERT_TRUE(memory.write_bytes(va + off, bytes.data() + off, 0x1000));
            ASSERT_TRUE(memory.protect_region(va + off, 0x1000,
                                              MemoryProtection::ReadExecute)
                            .is_ok());
        }
    }
    ::closedir(d);

    // The runtime enables block linking once after JIT init; without it every
    // exit is a full dispatcher return, which is not what hot code runs.
    JitCompiler::enable_jit_link();
    BlockCache cache;
    JitCompiler jit(memory);
    jit.set_block_cache(&cache);

    std::istringstream list(rips);
    std::string tok;
    u64 total_guest = 0, total_hot = 0;
    while (list >> tok) {
        const GuestAddr rip = std::strtoull(tok.c_str(), nullptr, 16);
        BasicBlock block = vitreo::cli::decode_basic_block(memory, rip, true);
        CompiledBlock* cb = jit.compile(block);
        if (cb == nullptr || cb->code == nullptr) {
            std::printf("[codegen] block %#llx DECLINED\n",
                        static_cast<unsigned long long>(rip));
            continue;
        }
        // A split-placed block's cold part may live outside [code, code+size).
        const bool split_out =
            cb->cold_size != 0 && cb->cold_offset >= cb->code_size;
        const u32 hot = cb->cold_size == 0 || split_out ? cb->code_size
                                                        : cb->cold_offset;
        const u32 cold = cb->cold_size;
        std::printf("[codegen] block %#llx guest_bytes=%u insns=%u hot_words=%u "
                    "cold_words=%u\n",
                    static_cast<unsigned long long>(rip), block.byte_length,
                    block.instruction_count, hot / 4, cold / 4);
        static const char* const kNames[EmitAttribution::Count] = {
            "prologue", "body", "address", "memfast", "flags", "gpr", "rip",
            "helper", "guard", "exit_link", "exit_probe", "exit_dispatch",
            "exit_other", "raise", "cold_mem", "cold_arms", "other"};
        const std::vector<u8>& tags = jit.emit_attribution_tags_for_test();
        const auto tag_of = [&](u32 i) {
            return i < tags.size() && tags[i] < EmitAttribution::Count
                       ? kNames[tags[i]] : "-";
        };
        const auto* w = static_cast<const u32*>(cb->code);
        for (u32 i = 0; i < hot / 4; ++i)
            std::printf("[codegen] +%u %08x %s\n", i * 4, w[i], tag_of(i));
        const auto* c = reinterpret_cast<const u32*>(cb->cold_code());
        for (u32 i = 0; c != nullptr && i < cold / 4; ++i)
            std::printf("[codegen] c+%u %08x %s\n", i * 4, c[i], tag_of(hot / 4 + i));
        total_guest += block.byte_length;
        total_hot += hot;
    }
    std::printf("[codegen] total guest_bytes=%llu hot_bytes=%llu ratio=%.2f\n",
                static_cast<unsigned long long>(total_guest),
                static_cast<unsigned long long>(total_hot),
                total_guest ? double(total_hot) / double(total_guest) : 0.0);
    JitCompiler::disable_jit_link();
    unregister_linear_reservation(memory.linear_base());
}
#endif
