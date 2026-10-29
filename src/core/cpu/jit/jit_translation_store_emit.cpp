// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_translation_store_emit.cpp
/// @brief Capture and restore of the emitter state finalize_code consumes,
///        for the persistent translation store (translation_store.hpp).

#include "jit_compiler.hpp"
#include "jit_memory_emit_internal.hpp"

#include <algorithm>

namespace vitreo::cpu::jit {

void JitCompiler::capture_emission(const BasicBlock& block,
                                   StoredEmission& out) const {
    out.clear();
    out.words = m_code_buf;
    out.hot_end_pos = m_hot_end_pos;
    out.link_entry_pos = m_link_entry_pos;
    out.call_entry_pos = m_call_entry_pos;
    out.insn_pos_count = m_insn_pos_count;
    const u32 n = std::min<u32>(m_insn_pos_count, kMaxInsnArmPos);
    out.insn_arm_pos.assign(m_insn_arm_pos.begin(), m_insn_arm_pos.begin() + n);
    out.insn_arm_end.assign(m_insn_arm_end.begin(), m_insn_arm_end.begin() + n);
    out.insn_rips.assign(m_insn_rips.begin(), m_insn_rips.begin() + n);
    out.insn_flag_recovery.assign(m_insn_flag_recovery.begin(),
                                  m_insn_flag_recovery.begin() + n);
    out.insn_rsp_undo.assign(m_insn_rsp_undo.begin(), m_insn_rsp_undo.begin() + n);
    out.stencil_flag_recovery = m_stencil_flag_recovery;
    out.lean_ranges.reserve(m_lean_variant_ranges.size());
    for (const LeanVariantRange& r : m_lean_variant_ranges) {
        out.lean_ranges.push_back({r.begin, r.end, r.owner});
    }
    out.cold_lazy.reserve(m_cold_lazy_sites.size());
    for (const ColdLazySite& s : m_cold_lazy_sites) {
        out.cold_lazy.push_back({s.slot_pos, s.return_pos, s.mixed_pos, s.guard_pos,
                                 s.raise_kind, s.packed, s.config});
    }
    out.raise_entry_plain_pos = m_raise_entry_plain_pos;
    for (usize i = 0; i < m_raise_entry_restore_pos.size(); ++i) {
        out.raise_entry_restore_pos[i] = m_raise_entry_restore_pos[i];
    }
    out.links = m_emit_links;
    for (usize i = 0; i < 16; ++i) out.pin_arm[i] = m_pin_arm[i];
    out.pin_layout = m_pin_layout;
    out.native_insns = m_block_native_insns;
    out.terminal_opcode = m_block_terminal_opcode;
    out.stack_entry_load_proof = m_stack_entry_load_proof;
    out.instruction_count = block.instruction_count;
    out.ends_with_branch = block.ends_with_branch;
    out.relocs = m_ts_relocs;
    out.process_stubs = m_ts_stubs;
    out.mem_helpers = m_ts_mem_helpers;
}

void JitCompiler::ts_note_mem_helper(u32 helper_off, int width, bool is_store) {
    const StoredMemHelper h{helper_off, static_cast<u8>(width),
                            static_cast<u8>(is_store ? 1 : 0)};
    if (std::find(m_ts_mem_helpers.begin(), m_ts_mem_helpers.end(), h) ==
        m_ts_mem_helpers.end()) {
        m_ts_mem_helpers.push_back(h);
    }
}

bool JitCompiler::ts_publish_stubs(const StoredEmission& in) {
    static_assert(stored_stub::kLinkProbeKinds == kLinkProbeKinds);
    static_assert(stored_stub::kDispatchTailKinds == kDispatchTails);
    const u32 bits = in.process_stubs;
    if ((bits & stored_stub::kRaiseExit) != 0 &&
        materialize_process_raise_exit() == nullptr) {
        return false;
    }
    if ((bits & stored_stub::kCaptureCf) != 0 && materialize_capture_cf_stub() == nullptr) {
        return false;
    }
    if ((bits & stored_stub::kRsbPush) != 0 && materialize_rsb_push() == nullptr) {
        return false;
    }
    for (u32 k = 0; k < stored_stub::kLinkProbeKinds; ++k) {
        if ((bits & (1u << (stored_stub::kLinkProbeShift + k))) != 0 &&
            materialize_link_probe(static_cast<u8>(k)) == nullptr) {
            return false;
        }
    }
    for (u32 k = 0; k < stored_stub::kDispatchTailKinds; ++k) {
        if ((bits & (1u << (stored_stub::kDispatchTailShift + k))) != 0 &&
            materialize_dispatch_tail(static_cast<u8>(k)) == nullptr) {
            return false;
        }
    }
    for (u32 slot = 0; slot < stored_stub::kHotWideSlots; ++slot) {
        if ((bits & (1u << (stored_stub::kHotWideShift + slot))) == 0) continue;
        const bool is_store = (slot & 1u) != 0;
        const int width = (slot & 2u) != 0 ? 32 : 16;
        if (linear64_hot_wide_slot(is_store, width) != slot ||
            materialize_linear64_hot_helper(is_store, width) == nullptr) {
            return false;
        }
    }
    for (const StoredMemHelper& h : in.mem_helpers) {
        if (adopt_scalar_mem_helper(h.helper_off, h.width, h.is_store != 0) == nullptr) {
            return false;
        }
    }
    return true;
}

bool JitCompiler::restore_emission(const BasicBlock& block,
                                   const StoredEmission& in) {
    if (in.insn_pos_count > kMaxInsnArmPos ||
        in.insn_arm_pos.size() != in.insn_pos_count ||
        in.insn_arm_end.size() != in.insn_pos_count ||
        in.insn_rips.size() != in.insn_pos_count ||
        in.insn_flag_recovery.size() != in.insn_pos_count ||
        in.insn_rsp_undo.size() != in.insn_pos_count ||
        in.instruction_count != block.instruction_count ||
        in.ends_with_branch != block.ends_with_branch) {
        return false;
    }
    m_code_buf = in.words;
    m_hot_end_pos = in.hot_end_pos;
    m_link_entry_pos = in.link_entry_pos;
    m_call_entry_pos = in.call_entry_pos;
    m_insn_pos_count = in.insn_pos_count;
    for (u32 i = 0; i < in.insn_pos_count; ++i) {
        m_insn_arm_pos[i] = in.insn_arm_pos[i];
        m_insn_arm_end[i] = in.insn_arm_end[i];
        m_insn_rips[i] = in.insn_rips[i];
        m_insn_flag_recovery[i] = in.insn_flag_recovery[i];
        m_insn_rsp_undo[i] = in.insn_rsp_undo[i];
    }
    m_stencil_flag_recovery = in.stencil_flag_recovery;
    m_lean_variant_ranges.clear();
    for (const StoredEmission::LeanRange& r : in.lean_ranges) {
        m_lean_variant_ranges.push_back({static_cast<usize>(r.begin),
                                         static_cast<usize>(r.end), r.owner});
    }
    m_cold_lazy_sites.clear();
    for (const StoredEmission::ColdLazy& s : in.cold_lazy) {
        m_cold_lazy_sites.push_back({static_cast<usize>(s.slot_pos),
                                     static_cast<usize>(s.return_pos),
                                     static_cast<usize>(s.mixed_pos),
                                     static_cast<usize>(s.guard_pos),
                                     s.raise_kind, s.packed, s.config});
    }
    m_raise_entry_plain_pos = static_cast<usize>(in.raise_entry_plain_pos);
    for (usize i = 0; i < m_raise_entry_restore_pos.size(); ++i) {
        m_raise_entry_restore_pos[i] = static_cast<usize>(in.raise_entry_restore_pos[i]);
    }
    m_emit_links = in.links;
    for (usize i = 0; i < 16; ++i) m_pin_arm[i] = in.pin_arm[i];
    m_pin_layout = in.pin_layout;
    m_pins_live = true;
    m_block_native_insns = in.native_insns;
    m_block_terminal_opcode = in.terminal_opcode;
    m_stack_entry_load_proof = in.stack_entry_load_proof;
    m_has_predecoded_delegate = false;
    m_branch_reach_exceeded = false;
    m_block_start_addr = block.start_addr;
    m_block_is_64bit = block.is_64bit;
    m_ts_relocs = in.relocs;
    m_ts_stubs = in.process_stubs;
    m_ts_mem_helpers = in.mem_helpers;
    return true;
}

}  // namespace vitreo::cpu::jit
