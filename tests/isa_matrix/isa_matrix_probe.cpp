// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file isa_matrix_probe.cpp
/// @brief See isa_matrix_probe.hpp.

#include "isa_matrix_probe.hpp"

#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/jit_coverage.hpp"
#include "core/memory/guest_memory.hpp"

#include <memory>

namespace vitreo::isa_matrix {

namespace {

using vitreo::GuestMemory;
using vitreo::MemoryProtection;
using vitreo::cpu::jit::BasicBlock;
using vitreo::cpu::jit::CompiledBlock;
using vitreo::cpu::jit::JitCompiler;

// Each probe compiles at its own guest address. Compiling repeatedly at ONE
// address trips the self-modifying-code demotion path, which makes later
// probes report "declined" for reasons that have nothing to do with the
// instruction under test — the same trap that made an early draft of the
// emission-golden test mis-report 29 of 44 snippets.
constexpr vitreo::GuestAddr kCodeBase32 = 0x00100000;
constexpr vitreo::GuestAddr kCodeBase64 = 0x05000000;
constexpr vitreo::u64 kCodeRegionSize = 0x00200000;
constexpr vitreo::GuestAddr kDataBase = 0x04000000;
constexpr vitreo::u64 kDataSize = 0x1000;

}  // namespace

ProbeResult probe_form(const FormCase& fc) {
    ProbeResult out;

    static thread_local unsigned s_slot = 0;
    const bool is64 = (fc.mode == 64);
    const vitreo::GuestAddr base = is64 ? kCodeBase64 : kCodeBase32;
    // 64-byte stride: wider than the longest x86 instruction, so no two probes
    // share a cache line's worth of guest code.
    const vitreo::GuestAddr addr =
        base + ((s_slot++ * 64u) % (kCodeRegionSize - 0x1000));

    GuestMemory memory;
    if (!memory.map_region(base, kCodeRegionSize, MemoryProtection::ReadWrite)) {
        return out;
    }
    if (!memory.map_region(kDataBase, kDataSize, MemoryProtection::ReadWrite)) {
        return out;
    }
    if (!memory.write_bytes(addr, fc.bytes.data(), fc.bytes.size())) return out;

    BasicBlock block;
    block.start_addr = addr;
    block.is_64bit = is64;
    block.instructions.push_back(fc.decoded);
    block.byte_length = static_cast<vitreo::u32>(fc.bytes.size());
    block.instruction_count = 1;

    // Record into a clean table so the counts read back belong to this
    // compile only.
    const bool was = vitreo::cpu::jit::coverage::set_enabled(true);
    vitreo::cpu::jit::coverage::reset();

    JitCompiler jit(memory);
    CompiledBlock* cb = jit.compile(block);
    if (cb != nullptr) {
        out.compiled = true;
        out.arm_words = cb->code_size / 4u;
    }

    for (LowerKind k : {LowerKind::Native, LowerKind::Helper,
                        LowerKind::Delegate}) {
        if (vitreo::cpu::jit::coverage::compiled_count(fc.opcode, fc.form,
                                                       is64, k) != 0) {
            out.kind = k;
            break;
        }
    }

    vitreo::cpu::jit::coverage::set_enabled(was);
    return out;
}

}  // namespace vitreo::isa_matrix
