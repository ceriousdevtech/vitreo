// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_stats.cpp
/// @brief Emitted-ARM64 code-quality telemetry. See jit_stats.hpp.

#include "jit_stats.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace vitreo::cpu::jit::stats {

namespace {

// Same sizing rationale as jit_coverage.cpp: Opcode is a u16 enum with ~999
// values today; anything past the table lands in an overflow counter rather
// than corrupting a neighbour.
constexpr usize kOpcodeSlots = 2048;
constexpr usize kBitnessSlots = 2;  // 0 = 32-bit guest block, 1 = 64-bit
constexpr usize kMaxDisasmTargets = 8;

struct OpTables {
    std::array<std::array<std::atomic<u64>, kOpcodeSlots>, kBitnessSlots>
        compiled{};   // times this opcode was compiled
    std::array<std::array<std::atomic<u64>, kOpcodeSlots>, kBitnessSlots>
        emitted{};    // ARM instructions emitted for it, summed
    std::array<std::array<std::atomic<u64>, kOpcodeSlots>, kBitnessSlots>
        delegated{};  // of those compiles, how many took the delegate
    std::array<std::atomic<u64>, kBitnessSlots> overflow{};
};

OpTables& op_tables() noexcept {
    // Function-local static: no global-constructor ordering hazard against the
    // JIT's own statics, and never constructed when the feature is off because
    // every caller checks enabled() first.
    static OpTables t;
    return t;
}

struct BlockRecord {
    u64 guest_addr{0};
    u32 guest_size{0};
    bool is_64bit{false};
    u32 total_arm{0};   // whole code buffer, in ARM instructions
    u32 body_arm{0};    // sum over instructions
    u32 recompiles{0};
    std::vector<InsnSite> insns;
    std::atomic<u64> exec{0};
};

// Records are heap-allocated and never erased, so a pointer handed to the
// thread-local exec cache stays valid for the life of the process.
using BlockMap = std::unordered_map<u64, std::unique_ptr<BlockRecord>>;

BlockMap& blocks() noexcept {
    static BlockMap m;
    return m;
}

std::shared_mutex& blocks_mutex() noexcept {
    static std::shared_mutex mu;
    return mu;
}

// ---- env state ----------------------------------------------------------

std::array<u64, kMaxDisasmTargets> g_disasm_targets{};

/// Dumps from a static destructor so the report survives every exit path: the
/// CLI re-execs itself for elevation and for guest child processes, so a dump
/// placed only on the normal return path never runs. Same reasoning as
/// coverage::Reporter.
///
/// The constructor touches the storage so the counters are constructed
/// *before* this reporter and therefore destroyed *after* it.
using detail::g_disasm_count;

struct Reporter {
    Reporter() {
        (void)op_tables();
        (void)blocks();
        (void)blocks_mutex();
    }
    ~Reporter() { dump(); }
};

std::array<std::atomic<u64>, 8> g_link_slots{};
std::atomic<u64> g_link_patches{0};
std::atomic<u64> g_branch_reach_declines{0};

void parse_disasm_targets(const char* v) noexcept {
    if (v == nullptr) return;
    u32 n = 0;
    const char* p = v;
    while (*p != '\0' && n < kMaxDisasmTargets) {
        char* end = nullptr;
        // base 0: accepts 0x-prefixed hex (the documented form) and decimal.
        const unsigned long long a = std::strtoull(p, &end, 0);
        if (end == p) break;
        g_disasm_targets[n++] = static_cast<u64>(a);
        p = (*end == ',') ? end + 1 : end;
    }
    g_disasm_count.store(n, std::memory_order_relaxed);
}

const char* bitness_label(usize b) noexcept { return b == 0 ? "x86" : "x64"; }

double ratio(u64 num, u64 den) noexcept {
    return den == 0 ? 0.0
                    : static_cast<double>(num) / static_cast<double>(den);
}

}  // namespace

namespace detail {

// Definitions for the header's inline gates.
std::atomic<int> g_enabled{-1};        // -1 unknown, 0 off, 1 on
std::atomic<int> g_report{0};          // VITREO_JIT_STATS specifically
std::atomic<u32> g_disasm_count{0};

[[gnu::noinline]] bool stats_init_slow() noexcept {
    const char* stats = std::getenv("VITREO_JIT_STATS");
    const char* disasm = std::getenv("VITREO_JIT_DISASM");
    parse_disasm_targets(disasm);
    const bool want_report = (stats != nullptr);
    const bool on =
        want_report || (g_disasm_count.load(std::memory_order_relaxed) != 0);
    g_report.store(want_report ? 1 : 0, std::memory_order_relaxed);
    if (want_report) {
        // Register the exit dumper only when the report is actually wanted.
        static const Reporter reporter{};
        (void)reporter;
    }
    g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
    return on;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Instruction-class attribution
// ---------------------------------------------------------------------------
//
// Which x86 instruction *shape* the emitted bytes go to. Opcode names are the
// classification key (the enum is grouped by mnemonic, and the name encodes
// the register file: XMM, MM), and the InsnSite carries whether the decoded
// instruction had a memory operand, and whether that operand is the
// destination -- which the opcode alone cannot tell (MOV_R32_RM32 is a load or
// a reg-reg move).

namespace {

enum class InsnClass : u8 {
    Load,         // MOV-family r <- [mem] (incl. MOVZX/MOVSX)
    Store,        // MOV-family [mem] <- r/imm
    MovReg,       // MOV-family register-only (MOV r,r / MOV r,imm / LEA / XCHG)
    PushPop,      // PUSH / POP / PUSHA / POPA / PUSHF / POPF
    AluFlagsReg,  // ADD/SUB/ADC/SBB/INC/DEC/NEG/AND/OR/XOR/NOT, register-only
    AluFlagsLoad, // same with a memory SOURCE
    AluFlagsRmw,  // same with a memory DESTINATION (load+op+store)
    CmpTestReg,   // CMP / TEST, register-only
    CmpTestMem,   // CMP / TEST with a memory operand
    ShiftRot,     // SHL/SHR/SAR/ROL/ROR/RCL/RCR/SHLD/SHRD
    MulDiv,       // MUL/IMUL/DIV/IDIV
    Bit,          // BT*/BSF/BSR/POPCNT/LZCNT/TZCNT/BMI
    Jcc,          // conditional jumps, LOOP, JCXZ
    Jmp,          // unconditional JMP
    CallRet,      // CALL / RET / LEAVE / ENTER
    CmovSetcc,    // CMOVcc / SETcc
    String,       // MOVS/STOS/LODS/SCAS/CMPS
    Sse,          // XMM/YMM, MXCSR
    Mmx,          // MM register file
    X87,          // F*
    Misc,         // everything else that was lowered natively
    Delegate,     // took the interpreter delegate (helper call)
    Count
};

const char* class_label(InsnClass c) noexcept {
    switch (c) {
        case InsnClass::Load:         return "load (mov r,[m])";
        case InsnClass::Store:        return "store (mov [m],r)";
        case InsnClass::MovReg:       return "mov reg/imm/lea";
        case InsnClass::PushPop:      return "push/pop";
        case InsnClass::AluFlagsReg:  return "alu+flags reg";
        case InsnClass::AluFlagsLoad: return "alu+flags r,[m]";
        case InsnClass::AluFlagsRmw:  return "alu+flags [m] rmw";
        case InsnClass::CmpTestReg:   return "cmp/test reg";
        case InsnClass::CmpTestMem:   return "cmp/test mem";
        case InsnClass::ShiftRot:     return "shift/rotate";
        case InsnClass::MulDiv:       return "mul/div";
        case InsnClass::Bit:          return "bit (bt/bsf/..)";
        case InsnClass::Jcc:          return "jcc/loop";
        case InsnClass::Jmp:          return "jmp";
        case InsnClass::CallRet:      return "call/ret/leave";
        case InsnClass::CmovSetcc:    return "cmov/setcc";
        case InsnClass::String:       return "string ops";
        case InsnClass::Sse:          return "sse/avx";
        case InsnClass::Mmx:          return "mmx";
        case InsnClass::X87:          return "x87";
        case InsnClass::Misc:         return "misc native";
        case InsnClass::Delegate:     return "delegate (helper)";
        case InsnClass::Count:        break;
    }
    return "?";
}

// Coarse family, keyed on the enum's section layout (opcodes.hpp groups the
// enumerators by mnemonic family, in declaration order). `opcode_name` is not
// usable for this: fewer than half the enumerators have a display name.
enum class Family : u8 { Mov, Lea, PushPop, Alu, Cmp, MulDiv, Shift, Jmp, Jcc,
                         CallRet, String, Bit, CmovSetcc, X87, Sse, Mmx, Misc };

Family family_of(u32 opcode) noexcept {
    struct Range { Opcode first; Family family; };
    // Each entry starts a family that runs until the next entry's `first`.
    // Sorted by enumerator value; the table is small enough that a linear
    // scan from the back is fine for a report printed once.
    static constexpr Range kRanges[] = {
        {Opcode::INVALID,               Family::Mov},      // MOV/MOVZX/MOVSX/XCHG
        {Opcode::LEA_R16_M,             Family::Lea},
        {Opcode::PUSH_R16,              Family::PushPop},
        {Opcode::ADD_AL_IMM8,           Family::Alu},      // ADD/SUB/ADC/SBB
        {Opcode::CMP_AL_IMM8,           Family::Cmp},
        {Opcode::INC_R16,               Family::Alu},      // INC/DEC/NEG
        {Opcode::MUL_RM8,               Family::MulDiv},
        {Opcode::AND_AL_IMM8,           Family::Alu},      // AND/OR/XOR/NOT
        {Opcode::TEST_AL_IMM8,          Family::Cmp},
        {Opcode::SHL_RM8_1,             Family::Shift},    // incl. SHLD/SHRD/rotates
        {Opcode::JMP_REL8,              Family::Jmp},
        {Opcode::JO_REL8,               Family::Jcc},      // incl. LOOP/JCXZ
        {Opcode::CALL_REL16,            Family::CallRet},  // incl. LEAVE/ENTER
        {Opcode::INT_3,                 Family::Misc},     // INT, flag ops, NOP, CPUID, CBW..
        {Opcode::MOVSB,                 Family::String},
        {Opcode::BT_RM16_R16,           Family::Bit},
        {Opcode::CMOVO_R16_RM16,        Family::CmovSetcc},
        {Opcode::FNCLEX,                Family::X87},
        {Opcode::ADDSS_XMM_XMMM32,      Family::Sse},
        {Opcode::MOVQ_MM_MMM64,         Family::Mmx},
        {Opcode::CVTSI2SS_XMM_RM32,     Family::Sse},      // conv/cmp/int/shuffle/SSSE3+/SHA/AES
        {Opcode::DAA,                   Family::Misc},     // decimal, BOUND, far pointers
        {Opcode::FFREE_STI,             Family::X87},
        {Opcode::ENDBR32,               Family::Misc},     // ENDBR, XSAVE, VZERO*
        {Opcode::ADCX_R32_RM32,         Family::Alu},
        {Opcode::MOVBE_R64_M64,         Family::Mov},
        {Opcode::ANDN_R32_R32_RM32,     Family::Bit},      // BMI1/2
        {Opcode::VCVTPH2PS_XMM_XMMM64,  Family::Sse},      // F16C, AVX, FMA
    };
    Family f = Family::Misc;
    for (const Range& r : kRanges) {
        if (opcode >= static_cast<u32>(r.first)) f = r.family;
        else break;
    }
    return f;
}

InsnClass classify(u32 opcode, u8 mem, bool native) noexcept {
    if (!native) return InsnClass::Delegate;
    const bool has_mem = (mem & 1u) != 0;
    const bool dst_mem = (mem & 2u) != 0;
    switch (family_of(opcode)) {
        case Family::Mov:
            if (!has_mem) return InsnClass::MovReg;
            return dst_mem ? InsnClass::Store : InsnClass::Load;
        case Family::Lea:       return InsnClass::MovReg;
        case Family::PushPop:   return InsnClass::PushPop;
        case Family::Alu:
            if (!has_mem) return InsnClass::AluFlagsReg;
            return dst_mem ? InsnClass::AluFlagsRmw : InsnClass::AluFlagsLoad;
        case Family::Cmp:
            return has_mem ? InsnClass::CmpTestMem : InsnClass::CmpTestReg;
        case Family::MulDiv:    return InsnClass::MulDiv;
        case Family::Shift:     return InsnClass::ShiftRot;
        case Family::Jmp:       return InsnClass::Jmp;
        case Family::Jcc:       return InsnClass::Jcc;
        case Family::CallRet:   return InsnClass::CallRet;
        case Family::String:    return InsnClass::String;
        case Family::Bit:       return InsnClass::Bit;
        case Family::CmovSetcc: return InsnClass::CmovSetcc;
        case Family::X87:       return InsnClass::X87;
        case Family::Sse:       return InsnClass::Sse;
        case Family::Mmx:       return InsnClass::Mmx;
        case Family::Misc:      break;
    }
    return InsnClass::Misc;
}

constexpr usize kClassCount = static_cast<usize>(InsnClass::Count);

struct ClassTables {
    std::array<std::array<std::atomic<u64>, kClassCount>, kBitnessSlots> insns{};
    std::array<std::array<std::atomic<u64>, kClassCount>, kBitnessSlots> words{};
    std::array<std::atomic<u64>, kBitnessSlots> blocks{};
    std::array<std::atomic<u64>, kBitnessSlots> overhead_words{};  // total - body
    std::array<std::atomic<u64>, kBitnessSlots> total_words{};
};

ClassTables& class_tables() noexcept {
    static ClassTables t;
    return t;
}

void record_block_classes(bool is_64bit, u32 total_arm_words, u32 body,
                          const InsnSite* insns, u32 insn_count) noexcept {
    auto& t = class_tables();
    const usize b = is_64bit ? 1u : 0u;
    for (u32 i = 0; i < insn_count; ++i) {
        const InsnSite& s = insns[i];
        const auto c = static_cast<usize>(
            classify(s.opcode, s.mem, s.native != 0));
        t.insns[b][c].fetch_add(1, std::memory_order_relaxed);
        t.words[b][c].fetch_add(s.arm_words, std::memory_order_relaxed);
    }
    t.blocks[b].fetch_add(1, std::memory_order_relaxed);
    t.total_words[b].fetch_add(total_arm_words, std::memory_order_relaxed);
    t.overhead_words[b].fetch_add(
        total_arm_words > body ? total_arm_words - body : 0,
        std::memory_order_relaxed);
}

}  // namespace

void dump_class_table() noexcept {
    auto& t = class_tables();
    for (usize b = 0; b < kBitnessSlots; ++b) {
        const u64 blocks = t.blocks[b].load(std::memory_order_relaxed);
        if (blocks == 0) continue;
        const u64 total = t.total_words[b].load(std::memory_order_relaxed);
        const u64 overhead = t.overhead_words[b].load(std::memory_order_relaxed);
        u64 total_insns = 0;
        for (usize c = 0; c < kClassCount; ++c) {
            total_insns += t.insns[b][c].load(std::memory_order_relaxed);
        }
        std::fprintf(stderr,
            "[jit-stats] per-class emitted bytes (%s): %llu blocks,"
            " %llu insns, %llu ARM bytes total => %.1f bytes/insn,"
            " %.1f bytes/block\n"
            "[jit-stats]   %-20s %10s %12s %8s %8s\n",
            bitness_label(b), static_cast<unsigned long long>(blocks),
            static_cast<unsigned long long>(total_insns),
            static_cast<unsigned long long>(total * 4),
            total_insns ? 4.0 * static_cast<double>(total) /
                              static_cast<double>(total_insns) : 0.0,
            4.0 * static_cast<double>(total) / static_cast<double>(blocks),
            "class", "insns", "bytes", "b/insn", "share");
        std::array<usize, kClassCount> order{};
        for (usize c = 0; c < kClassCount; ++c) order[c] = c;
        std::sort(order.begin(), order.end(), [&](usize x, usize y) {
            return t.words[b][x].load(std::memory_order_relaxed) >
                   t.words[b][y].load(std::memory_order_relaxed);
        });
        for (usize k = 0; k < kClassCount; ++k) {
            const usize c = order[k];
            const u64 n = t.insns[b][c].load(std::memory_order_relaxed);
            const u64 w = t.words[b][c].load(std::memory_order_relaxed);
            if (n == 0) continue;
            std::fprintf(stderr,
                "[jit-stats]   %-20s %10llu %12llu %8.1f %7.1f%%\n",
                class_label(static_cast<InsnClass>(c)),
                static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(w * 4),
                4.0 * static_cast<double>(w) / static_cast<double>(n),
                total ? 100.0 * static_cast<double>(w) /
                            static_cast<double>(total) : 0.0);
        }
        std::fprintf(stderr,
            "[jit-stats]   %-20s %10llu %12llu %8.1f %7.1f%%\n",
            "block overhead", static_cast<unsigned long long>(blocks),
            static_cast<unsigned long long>(overhead * 4),
            4.0 * static_cast<double>(overhead) / static_cast<double>(blocks),
            total ? 100.0 * static_cast<double>(overhead) /
                        static_cast<double>(total) : 0.0);
    }
}

void record_insn(Opcode op, bool is_64bit, bool native, u32 arm_words) noexcept {
    if (!enabled()) return;
    auto& t = op_tables();
    const usize b = is_64bit ? 1u : 0u;
    const auto idx = static_cast<usize>(op);
    if (idx >= kOpcodeSlots) {
        t.overflow[b].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    t.compiled[b][idx].fetch_add(1, std::memory_order_relaxed);
    t.emitted[b][idx].fetch_add(arm_words, std::memory_order_relaxed);
    if (!native) t.delegated[b][idx].fetch_add(1, std::memory_order_relaxed);
}

void record_block(u64 guest_addr, u32 guest_size, bool is_64bit,
                  u32 total_arm_words, const InsnSite* insns,
                  u32 insn_count) noexcept {
    if (!enabled()) return;

    u32 body = 0;
    for (u32 i = 0; i < insn_count; ++i) body += insns[i].arm_words;
    record_block_classes(is_64bit, total_arm_words, body, insns, insn_count);

    std::unique_lock<std::shared_mutex> lock(blocks_mutex());
    auto& map = blocks();
    const auto it = map.find(guest_addr);  // never at(): -fno-exceptions
    BlockRecord* rec = nullptr;
    if (it == map.end()) {
        auto owned = std::make_unique<BlockRecord>();
        rec = owned.get();
        map.emplace(guest_addr, std::move(owned));
    } else {
        rec = it->second.get();
        ++rec->recompiles;
    }
    rec->guest_addr = guest_addr;
    rec->guest_size = guest_size;
    rec->is_64bit = is_64bit;
    rec->total_arm = total_arm_words;
    rec->body_arm = body;
    rec->insns.assign(insns, insns + insn_count);
}

void record_block_exec(u64 guest_addr) noexcept {
    if (!enabled()) return;

    // A dispatcher loop re-enters the same block over and over; the cache
    // turns the steady state into one compare plus one relaxed add. Records
    // are never erased, so a cached pointer never dangles.
    static thread_local u64 last_addr = ~0ull;
    static thread_local BlockRecord* last_rec = nullptr;
    if (last_rec != nullptr && last_addr == guest_addr) {
        last_rec->exec.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    BlockRecord* rec = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(blocks_mutex());
        auto& map = blocks();
        const auto it = map.find(guest_addr);
        if (it == map.end()) {
            // Compiled before the stats were armed (or reused from the
            // process-wide compile memo by a thread that never compiled it).
            return;
        }
        rec = it->second.get();
    }
    last_addr = guest_addr;
    last_rec = rec;
    rec->exec.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Disassembly dump
// ---------------------------------------------------------------------------

namespace {

std::unordered_map<u64, bool>& disasm_done() noexcept {
    static std::unordered_map<u64, bool> done;
    return done;
}

std::mutex& disasm_mutex() noexcept {
    static std::mutex mu;
    return mu;
}

void print_words(const u32* code, u32 from, u32 to) noexcept {
    for (u32 i = from; i < to; ++i) {
        std::fprintf(stderr, "[jit-disasm]     +%04u  0x%08x\n", i * 4u,
                     static_cast<unsigned>(code[i]));
    }
}

}  // namespace

void maybe_dump_block(u64 guest_addr, u32 guest_size, bool is_64bit,
                      const u32* code, u32 code_words, const InsnSite* insns,
                      u32 insn_count, const u8* guest_bytes) noexcept {
    if (!disasm_enabled() || code == nullptr || code_words == 0) return;

    const u32 n_targets = g_disasm_count.load(std::memory_order_relaxed);
    bool hit = false;
    for (u32 i = 0; i < n_targets; ++i) {
        const u64 t = g_disasm_targets[i];
        if (t >= guest_addr && t < guest_addr + guest_size) {
            hit = true;
            break;
        }
    }
    if (!hit) return;

    std::lock_guard<std::mutex> lock(disasm_mutex());
    auto& done = disasm_done();
    if (done.find(guest_addr) != done.end()) return;  // once per address
    done.emplace(guest_addr, true);

    u32 body = 0;
    for (u32 i = 0; i < insn_count; ++i) body += insns[i].arm_words;

    std::fprintf(stderr,
        "[jit-disasm] ==== block 0x%016llx  (%s, %u x86 insns, %u x86 bytes)"
        " ====\n"
        "[jit-disasm]   emitted %u ARM instructions (%u bytes): %u in"
        " instruction bodies, %u in prologue/epilogue/cold-stubs\n"
        "[jit-disasm]   mean %.2f ARM per x86 instruction (bodies only)\n",
        static_cast<unsigned long long>(guest_addr), bitness_label(is_64bit),
        insn_count, guest_size, code_words, code_words * 4u, body,
        code_words - body, ratio(body, insn_count));

    // Anything before the first instruction body is the prologue.
    const u32 first = insn_count != 0 ? insns[0].arm_pos : code_words;
    if (first != 0) {
        std::fprintf(stderr, "[jit-disasm]   --- prologue (%u ARM) ---\n",
                     first);
        print_words(code, 0, first);
    }

    for (u32 i = 0; i < insn_count; ++i) {
        const InsnSite& s = insns[i];
        std::fprintf(stderr,
                     "[jit-disasm]   --- 0x%016llx  op=%u %-24s  (%u ARM)"
                     " ---\n",
                     static_cast<unsigned long long>(s.rip),
                     static_cast<unsigned>(s.opcode),
                     opcode_name(static_cast<Opcode>(s.opcode))
                         ? opcode_name(static_cast<Opcode>(s.opcode))
                         : "(unnamed)",
                     s.arm_words);
        if (guest_bytes != nullptr && s.rip >= guest_addr &&
            s.rip + s.x86_len <= guest_addr + guest_size) {
            const u8* p = guest_bytes + (s.rip - guest_addr);
            std::fprintf(stderr, "[jit-disasm]     x86:");
            for (u32 k = 0; k < s.x86_len; ++k) {
                std::fprintf(stderr, " %02x", static_cast<unsigned>(p[k]));
            }
            std::fputc('\n', stderr);
        }
        print_words(code, s.arm_pos, s.arm_pos + s.arm_words);
    }

    // Everything after the last body is the epilogue plus the cold Jcc stubs
    // that emit_flush_cold_jcc_exits() appended.
    const u32 tail = insn_count != 0
                         ? insns[insn_count - 1].arm_pos +
                               insns[insn_count - 1].arm_words
                         : 0;
    if (tail < code_words) {
        std::fprintf(stderr,
                     "[jit-disasm]   --- epilogue + cold Jcc stubs (%u ARM)"
                     " ---\n",
                     code_words - tail);
        print_words(code, tail, code_words);
    }

    // One paste-ready little-endian byte blob for an external disassembler:
    //   llvm-mc --disassemble -arch=arm64 <<< '0x.. 0x.. ...'
    // No third-party dependency is linked in; the words above are already
    // enough to count, and this line is only a convenience.
    std::fprintf(stderr, "[jit-disasm]   paste (llvm-mc --disassemble"
                         " -arch=arm64):\n[jit-disasm]   ");
    for (u32 i = 0; i < code_words; ++i) {
        const u32 w = code[i];
        std::fprintf(stderr, "0x%02x 0x%02x 0x%02x 0x%02x ",
                     static_cast<unsigned>(w & 0xffu),
                     static_cast<unsigned>((w >> 8) & 0xffu),
                     static_cast<unsigned>((w >> 16) & 0xffu),
                     static_cast<unsigned>((w >> 24) & 0xffu));
    }
    std::fputc('\n', stderr);
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

namespace {

struct OpRow {
    usize op = 0;
    usize bitness = 0;
    u64 compiled = 0;
    u64 emitted = 0;    // static ARM instructions summed over compiles
    u64 delegated = 0;
    u64 exec = 0;       // dispatcher-observed executions of this instruction
    u64 exec_arm = 0;   // sum over executions of the ARM it runs
};

void dump_opcode_table() noexcept {
    auto& t = op_tables();

    // Static half: what we emitted, straight out of the atomic tables.
    std::array<std::array<OpRow, kOpcodeSlots>, kBitnessSlots> rows{};
    for (usize b = 0; b < kBitnessSlots; ++b) {
        for (usize i = 0; i < kOpcodeSlots; ++i) {
            OpRow& r = rows[b][i];
            r.op = i;
            r.bitness = b;
            r.compiled = t.compiled[b][i].load(std::memory_order_relaxed);
            r.emitted = t.emitted[b][i].load(std::memory_order_relaxed);
            r.delegated = t.delegated[b][i].load(std::memory_order_relaxed);
        }
    }

    // Dynamic half: fold each block's execution count into its composition.
    // This is where "times executed" per opcode comes from — exactly the
    // block's exec count times the number of times that opcode appears in it.
    {
        std::shared_lock<std::shared_mutex> lock(blocks_mutex());
        for (const auto& [addr, rec] : blocks()) {
            (void)addr;
            const u64 execs = rec->exec.load(std::memory_order_relaxed);
            if (execs == 0) continue;
            const usize b = rec->is_64bit ? 1u : 0u;
            for (const InsnSite& s : rec->insns) {
                if (s.opcode >= kOpcodeSlots) continue;
                OpRow& r = rows[b][s.opcode];
                r.exec += execs;
                r.exec_arm += execs * s.arm_words;
            }
        }
    }

    std::vector<OpRow> ranked;
    ranked.reserve(256);
    for (usize b = 0; b < kBitnessSlots; ++b) {
        for (usize i = 0; i < kOpcodeSlots; ++i) {
            if (rows[b][i].compiled != 0) ranked.push_back(rows[b][i]);
        }
    }
    if (ranked.empty()) return;

    // Rank by executions x emitted — the actual ARM instruction stream this
    // opcode is responsible for on this workload. An expensive lowering that
    // never runs is not the problem; a 12-ARM lowering in the hot loop is.
    std::sort(ranked.begin(), ranked.end(), [](const OpRow& a, const OpRow& b) {
        if (a.exec_arm != b.exec_arm) return a.exec_arm > b.exec_arm;
        return a.emitted > b.emitted;
    });

    std::fprintf(stderr,
        "[jit-stats] per-opcode, ranked by executions x ARM emitted\n"
        "[jit-stats]   %-4s %-24s %-4s %10s %10s %8s %14s %10s\n",
        "op", "name", "bits", "compiled", "armEmit", "arm/x86", "execs",
        "exec*arm");
    const usize shown = std::min<usize>(40, ranked.size());
    for (usize i = 0; i < shown; ++i) {
        const OpRow& r = ranked[i];
        const char* name = opcode_name(static_cast<Opcode>(r.op));
        std::fprintf(stderr,
            "[jit-stats]   %-4zu %-24s %-4s %10llu %10llu %8.2f %14llu"
            " %10llu%s\n",
            r.op, name ? name : "(unnamed)", bitness_label(r.bitness),
            static_cast<unsigned long long>(r.compiled),
            static_cast<unsigned long long>(r.emitted),
            ratio(r.emitted, r.compiled),
            static_cast<unsigned long long>(r.exec),
            static_cast<unsigned long long>(r.exec_arm),
            r.delegated != 0 ? "  [delegated]" : "");
    }

    for (usize b = 0; b < kBitnessSlots; ++b) {
        const u64 ovf = t.overflow[b].load(std::memory_order_relaxed);
        if (ovf != 0) {
            std::fprintf(stderr,
                "[jit-stats]   WARNING: %llu %s records exceeded the opcode"
                " table (raise kOpcodeSlots)\n",
                static_cast<unsigned long long>(ovf), bitness_label(b));
        }
    }
}

void dump_block_table() noexcept {
    std::vector<u32> per_block_arm;
    std::vector<const BlockRecord*> hot;
    u64 total_blocks = 0;
    u64 total_arm = 0;
    u64 total_body_arm = 0;
    u64 total_x86 = 0;
    u64 exec_arm = 0;   // ARM instructions actually run (dispatcher-observed)
    u64 exec_x86 = 0;   // x86 instructions actually run

    {
        std::shared_lock<std::shared_mutex> lock(blocks_mutex());
        auto& map = blocks();
        per_block_arm.reserve(map.size());
        hot.reserve(map.size());
        for (const auto& [addr, rec] : map) {
            (void)addr;
            ++total_blocks;
            total_arm += rec->total_arm;
            total_body_arm += rec->body_arm;
            total_x86 += rec->insns.size();
            per_block_arm.push_back(rec->total_arm);
            const u64 e = rec->exec.load(std::memory_order_relaxed);
            if (e != 0) {
                exec_arm += e * rec->total_arm;
                exec_x86 += e * rec->insns.size();
                hot.push_back(rec.get());
            }
        }

        std::sort(hot.begin(), hot.end(),
                  [](const BlockRecord* a, const BlockRecord* b) {
                      return a->exec.load(std::memory_order_relaxed) *
                                 a->total_arm >
                             b->exec.load(std::memory_order_relaxed) *
                                 b->total_arm;
                  });

        std::fprintf(stderr,
            "[jit-stats] whole program: %llu blocks, %llu x86 instructions,"
            " %llu ARM instructions emitted (%.2f ARM per x86)\n"
            "[jit-stats]   bodies %llu ARM (%.2f per x86), block overhead"
            " (prologue/epilogue/cold stubs) %llu ARM (%.1f%%)\n"
            "[jit-stats]   dispatcher-observed execution: %llu x86 -> %llu ARM"
            " (%.2f ARM per x86 executed)\n",
            static_cast<unsigned long long>(total_blocks),
            static_cast<unsigned long long>(total_x86),
            static_cast<unsigned long long>(total_arm),
            ratio(total_arm, total_x86),
            static_cast<unsigned long long>(total_body_arm),
            ratio(total_body_arm, total_x86),
            static_cast<unsigned long long>(total_arm - total_body_arm),
            total_arm == 0
                ? 0.0
                : 100.0 * ratio(total_arm - total_body_arm, total_arm),
            static_cast<unsigned long long>(exec_x86),
            static_cast<unsigned long long>(exec_arm),
            ratio(exec_arm, exec_x86));

        if (!per_block_arm.empty()) {
            std::sort(per_block_arm.begin(), per_block_arm.end());
            const usize n = per_block_arm.size();
            const u32 med = per_block_arm[n / 2];
            const u32 p95 = per_block_arm[(n * 95) / 100 < n ? (n * 95) / 100
                                                             : n - 1];
            std::fprintf(stderr,
                "[jit-stats]   ARM per block: mean %.1f  median %u  p95 %u"
                "  max %u\n",
                ratio(total_arm, total_blocks), med, p95,
                per_block_arm.back());
        }

        std::fprintf(stderr,
            "[jit-stats]   hottest blocks (dispatcher-observed; feed an"
            " address to VITREO_JIT_DISASM):\n"
            "[jit-stats]   %-18s %-4s %6s %8s %8s %14s\n",
            "guest", "bits", "x86", "armTot", "arm/x86", "execs");
        const usize shown = std::min<usize>(12, hot.size());
        for (usize i = 0; i < shown; ++i) {
            const BlockRecord* r = hot[i];
            std::fprintf(stderr,
                "[jit-stats]   0x%016llx %-4s %6zu %8u %8.2f %14llu\n",
                static_cast<unsigned long long>(r->guest_addr),
                bitness_label(r->is_64bit), r->insns.size(), r->total_arm,
                ratio(r->total_arm, r->insns.size()),
                static_cast<unsigned long long>(
                    r->exec.load(std::memory_order_relaxed)));
        }
    }
}

}  // namespace

void record_link_slot(u32 edge_bit) noexcept {
    u32 idx = 0;
    while (idx < 7 && (edge_bit >> idx) != 1u) ++idx;
    g_link_slots[idx].fetch_add(1, std::memory_order_relaxed);
}

u64 record_branch_reach_decline() noexcept {
    return g_branch_reach_declines.fetch_add(1, std::memory_order_relaxed) + 1;
}

u64 branch_reach_declines() noexcept {
    return g_branch_reach_declines.load(std::memory_order_relaxed);
}

void record_link_patch() noexcept {
    g_link_patches.fetch_add(1, std::memory_order_relaxed);
}

u64 link_patches_performed() noexcept {
    return g_link_patches.load(std::memory_order_relaxed);
}

void dump_link_table() noexcept {
    static constexpr const char* kNames[] = {
        "MidJcc", "TermJcc", "TermFall", "JmpRel", "CallRel",
        "bit5", "bit6", "other",
    };
    u64 total = 0;
    for (const auto& c : g_link_slots) {
        total += c.load(std::memory_order_relaxed);
    }
    const u64 patched = g_link_patches.load(std::memory_order_relaxed);
    if (total == 0 && patched == 0) return;
    std::fprintf(stderr,
                 "[jit-stats] direct-link slots emitted (compile time) and"
                 " sites patched (runtime)\n");
    for (usize i = 0; i < g_link_slots.size(); ++i) {
        const u64 n = g_link_slots[i].load(std::memory_order_relaxed);
        if (n == 0) continue;
        std::fprintf(stderr, "[jit-stats]   %-9s %10llu slots\n", kNames[i],
                     static_cast<unsigned long long>(n));
    }
    std::fprintf(stderr,
                 "[jit-stats]   %-9s %10llu slots / %llu patched (%.1f%%)\n",
                 "TOTAL", static_cast<unsigned long long>(total),
                 static_cast<unsigned long long>(patched),
                 total ? 100.0 * static_cast<double>(patched) /
                             static_cast<double>(total) : 0.0);
}

void dump() noexcept {
    if (!report_enabled()) return;
    std::fprintf(stderr,
                 "[jit-stats] pid=%d emitted-ARM64 code quality\n"
                 "[jit-stats] NOTE: execution counts are dispatcher-observed;"
                 " inline block-link hops are NOT counted, so they are a lower"
                 " bound. The arm/x86 ratios are compile-time exact.\n",
                 static_cast<int>(::getpid()));
    dump_class_table();
    dump_opcode_table();
    dump_block_table();
    dump_link_table();
}

}  // namespace vitreo::cpu::jit::stats
