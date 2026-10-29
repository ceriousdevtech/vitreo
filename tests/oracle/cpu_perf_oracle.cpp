// SPDX-License-Identifier: LGPL-2.1-only
#include "oracle/cpu_perf_oracle.hpp"

#include "core/cpu/decode/decoder.hpp"

#include <algorithm>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unistd.h>

namespace vitreo::oracle::perf {
namespace {

using vitreo::cpu::DecodeError;
using vitreo::cpu::DecodeMode;
using vitreo::cpu::DecodedInstruction;
using vitreo::cpu::MemoryByteReader;
using vitreo::cpu::Opcode;
using vitreo::cpu::Operand;
using vitreo::cpu::is_memory_operand;

[[nodiscard]] bool is_jcc(Opcode op) {
    return (op >= Opcode::JO_REL8 && op <= Opcode::JNLE_REL32) ||
           (op >= Opcode::LOOP_REL8 && op <= Opcode::JRCXZ_REL8);
}

[[nodiscard]] bool is_uncond_xfer(Opcode op) {
    return (op >= Opcode::JMP_REL8 && op <= Opcode::JMP_M16_64) ||
           (op >= Opcode::CALL_REL16 && op <= Opcode::CALL_M16_64) ||
           (op >= Opcode::RET_NEAR && op <= Opcode::IRETQ);
}

[[nodiscard]] bool is_trailing_cf(Opcode op) {
    return is_jcc(op) || is_uncond_xfer(op);
}

[[nodiscard]] bool is_string_op(Opcode op) {
    return (op >= Opcode::MOVSB && op <= Opcode::SCASQ) ||
           op == Opcode::INSB || op == Opcode::INSW_D ||
           op == Opcode::OUTSB || op == Opcode::OUTSW_D;
}

[[nodiscard]] bool needs_align16(Opcode op) {
    switch (op) {
        case Opcode::MOVAPS_XMM_XMMM128:
        case Opcode::MOVAPS_XMMM128_XMM:
        case Opcode::MOVAPD_XMM_XMMM128:
        case Opcode::MOVAPD_XMMM128_XMM:
        case Opcode::MOVDQA_XMM_XMMM128:
        case Opcode::MOVDQA_XMMM128_XMM:
            return true;
        default:
            return false;
    }
}

sigjmp_buf g_bench_jmp;

void on_bench_alarm(int) {
    siglongjmp(g_bench_jmp, 1);
}

[[nodiscard]] bool decode_all(const std::vector<uint8_t>& bytes,
                              std::vector<DecodedInstruction>& out,
                              std::string& why) {
    out.clear();
    if (bytes.empty() || bytes.size() > 0x1000) {
        why = "empty or too large";
        return false;
    }
    MemoryByteReader reader(bytes.data(), bytes.size());
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
            DecodeError::Success) {
            why = "decode failed";
            return false;
        }
        out.push_back(inst);
    }
    return true;
}

[[nodiscard]] uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    return 0xFF;
}

[[nodiscard]] bool parse_hex_bytes(const std::string& hex,
                                   std::vector<uint8_t>& out) {
    if (hex.size() < 2 || (hex.size() % 2) != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const uint8_t hi = hex_nibble(hex[i]);
        const uint8_t lo = hex_nibble(hex[i + 1]);
        if (hi > 15 || lo > 15) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return !out.empty();
}

[[nodiscard]] bool json_string_field(const std::string& object,
                                     const char* key, std::string& value) {
    const std::string pat = std::string("\"") + key + "\":\"";
    const auto pos = object.find(pat);
    if (pos == std::string::npos) return false;
    const auto start = pos + pat.size();
    const auto end = object.find('"', start);
    if (end == std::string::npos) return false;
    value = object.substr(start, end - start);
    return true;
}

[[nodiscard]] bool json_u64_field(const std::string& object, const char* key,
                                  uint64_t& value) {
    const std::string pat = std::string("\"") + key + "\":";
    const auto pos = object.find(pat);
    if (pos == std::string::npos) return false;
    const char* p = object.c_str() + pos + pat.size();
    while (*p == ' ') ++p;
    if (p[0] == '"') {
        char* end = nullptr;
        value = std::strtoull(p + 1, &end, 0);
        return end != p + 1;
    }
    char* end = nullptr;
    value = std::strtoull(p, &end, 0);
    return end != p;
}

[[nodiscard]] bool json_bool_field(const std::string& object, const char* key,
                                   bool& value) {
    const std::string pat = std::string("\"") + key + "\":";
    const auto pos = object.find(pat);
    if (pos == std::string::npos) return false;
    const auto start = pos + pat.size();
    if (object.compare(start, 4, "true") == 0) {
        value = true;
        return true;
    }
    if (object.compare(start, 5, "false") == 0) {
        value = false;
        return true;
    }
    return false;
}

[[nodiscard]] std::vector<uint8_t> make_safe_looped_body(
    const std::vector<uint8_t>& body, uint32_t& replay_count) {
    replay_count = 1;
    std::vector<DecodedInstruction> insts;
    std::string why;
    if (!decode_all(body, insts, why)) return body;
    for (const auto& inst : insts) {
        if (is_jcc(inst.opcode) || is_uncond_xfer(inst.opcode) ||
            is_string_op(inst.opcode)) {
            return body;
        }
        for (u8 oi = 0; oi < inst.operand_count; ++oi) {
            const Operand& operand = inst.operands[oi];
            if (is_memory_operand(operand.type)) return body;
            if (operand.type >= cpu::OperandType::Reg8 &&
                operand.type <= cpu::OperandType::Reg64 &&
                operand.reg == 1) {
                return body;  // LOOP owns ECX without changing flags.
            }
        }
    }
    if (body.empty() || body.size() > 120) return body;
    replay_count = 256;
    std::vector<uint8_t> looped{0xB9, 0x00, 0x01, 0x00, 0x00};
    looped.insert(looped.end(), body.begin(), body.end());
    looped.push_back(0xE2);  // LOOP rel8: decrements ECX, preserves flags.
    looped.push_back(static_cast<uint8_t>(-static_cast<int>(body.size() + 2)));
    return looped;
}

}  // namespace

bool prepare_body(const std::vector<uint8_t>& bytes,
                  std::vector<uint8_t>& body, std::string& why,
                  bool nop_jcc) {
    std::vector<DecodedInstruction> insts;
    if (!decode_all(bytes, insts, why)) return false;
    if (insts.empty()) {
        why = "no instructions";
        return false;
    }
    usize keep = insts.size();
    if (is_trailing_cf(insts.back().opcode)) {
        --keep;
    }
    if (keep == 0) {
        why = "body empty after stripping trailing control flow";
        return false;
    }
    usize body_bytes = 0;
    for (usize i = 0; i < keep; ++i) {
        if (is_uncond_xfer(insts[i].opcode)) {
            why = "CALL/JMP/RET/INT in body";
            return false;
        }
        if ((insts[i].opcode >= Opcode::PUSH_R16 &&
             insts[i].opcode <= Opcode::PUSHA) ||
            (insts[i].opcode >= Opcode::POP_R16 &&
             insts[i].opcode <= Opcode::POPA) ||
            insts[i].opcode == Opcode::ENTER ||
            insts[i].opcode == Opcode::LEAVE) {
            // Live 32-bit ESP on the host x86 runner is a 64-bit RSP. PUSH/POP would
            // be 8-byte and smash the 512-byte scratch.
            why = "PUSH/POP/ENTER/LEAVE in body";
            return false;
        }
        if ((insts[i].prefixes.rep || insts[i].prefixes.repne) &&
            is_string_op(insts[i].opcode)) {
            // Dump scratch points ECX into the page (~512). A captured
            // REP STOS would run that many iterations per timed call.
            why = "REP string op";
            return false;
        }
        for (u8 oi = 0; oi < insts[i].operand_count; ++oi) {
            const Operand& op = insts[i].operands[oi];
            if (!is_memory_operand(op.type)) continue;
            if (needs_align16(insts[i].opcode) &&
                ((op.mem.disp & 15) != 0 || op.mem.has_index())) {
                // MOVAPS to a non-16-aligned EA SIGBUS/SIGSEGVs inside
                // the timed loop (dump scratch used to leave ECX=16).
                why = "unaligned aligned-SSE mem";
                return false;
            }
            if (op.mem.is_disp_only()) {
                const u32 addr = static_cast<u32>(op.mem.disp);
                if (addr < kMemVa || addr >= kMemVa + 0x1000u) {
                    why = "absolute mem outside scratch";
                    return false;
                }
            } else if (op.mem.disp < -64 || op.mem.disp > 512) {
                // [ebp-0x120] etc. leave the 4 KiB scratch page and the
                // linear reservation then faults inside the timed loop.
                why = "wide mem disp";
                return false;
            }
        }
        body_bytes += insts[i].length;
    }
    if (body_bytes == 0 || body_bytes > 512) {
        why = "body size out of range";
        return false;
    }
    body.assign(bytes.begin(),
                bytes.begin() + static_cast<std::ptrdiff_t>(body_bytes));
    // Dump scratch is not the guest's flags. A taken mid-block Jcc
    // leaves the snippet (JIT status=5). Default is to NOP them so
    // both hosts time the fall-through ALU. Pass nop_jcc=false when
    // the caller planted operands so every Jcc falls through.
    if (nop_jcc) {
        usize off = 0;
        for (usize i = 0; i < keep; ++i) {
            if (is_jcc(insts[i].opcode)) {
                std::fill(body.begin() + static_cast<std::ptrdiff_t>(off),
                          body.begin() +
                              static_cast<std::ptrdiff_t>(off + insts[i].length),
                          static_cast<uint8_t>(0x90));
            }
            off += insts[i].length;
        }
    } else {
        for (usize i = 0; i < keep; ++i) {
            if (is_jcc(insts[i].opcode) && insts[i].operand_count > 0 &&
                insts[i].operands[0].imm < 0) {
                why = "backward Jcc in body";
                return false;
            }
        }
    }
    why.clear();
    return true;
}

bool parse_hot_dump(const std::string& path, std::vector<DumpBlock>& out,
                    std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    out.clear();
    size_t cursor = 0;
    while (true) {
        const auto open = text.find('{', cursor);
        if (open == std::string::npos) break;
        const auto close = text.find('}', open);
        if (close == std::string::npos) break;
        const std::string object = text.substr(open, close - open + 1);
        cursor = close + 1;

        DumpBlock block;
        std::string rip_s;
        std::string bytes_s;
        if (!json_string_field(object, "rip", rip_s) ||
            !json_string_field(object, "bytes", bytes_s) ||
            !parse_hex_bytes(bytes_s, block.bytes)) {
            continue;
        }
        char* end = nullptr;
        block.rip = std::strtoull(rip_s.c_str(), &end, 0);
        uint64_t code_variant = UINT32_MAX;
        (void)json_u64_field(object, "code_variant", code_variant);
        if (code_variant <= UINT32_MAX) {
            block.code_variant = static_cast<uint32_t>(code_variant);
        }
        (void)json_u64_field(object, "exec_count", block.exec_count);
        (void)json_u64_field(object, "insn_count", block.insn_count);
        (void)json_bool_field(object, "delegated", block.delegated);
        uint64_t sample_rate = 0;
        (void)json_u64_field(object, "sample_rate", sample_rate);
        block.sample_rate = static_cast<uint32_t>(sample_rate);
        (void)json_bool_field(object, "linked_hops_included",
                              block.linked_hops_included);
        (void)json_string_field(object, "class", block.cls);
        (void)json_string_field(object, "accounting", block.accounting);
        (void)json_string_field(object, "capture_file", block.capture_file);
        if (block.cls.empty()) block.cls = "alu";
        out.push_back(std::move(block));
    }
    if (out.empty()) {
        error = "no blocks in " + path;
        return false;
    }
    return true;
}

State scratch_state() {
    State in{};
    in.mode = kMode32;
    in.mem.fill(0x5A);
    const uint32_t scratch = kMemVa + 64u;
    for (int i = 0; i < 8; ++i) {
        if (i == 4) continue;  // ESP stays 0 — runner keeps its host stack
        in.gpr[static_cast<size_t>(i)] = scratch;
    }
    in.gpr[1] = 16;  // ECX: short REP / loop counts
    // Finite 1.0f in every XMM dword so SSE/x87 loops are not denormal-bound.
    const uint32_t one = 0x3f800000u;
    for (auto& xmm : in.xmm) {
        for (int d = 0; d < 4; ++d) {
            std::memcpy(xmm.data() + static_cast<size_t>(d) * 4, &one, 4);
        }
    }
    return in;
}

State dump_scratch_state() {
    State in = scratch_state();
    // Catalog scratch leaves ECX=16 for short REP. Captured SSE does
    // `movaps [ecx+0x30]` — EA 0x46 is not 16-aligned and the JIT
    // SIGBUS handler never returns. Every GPR, including ECX, has to
    // be a 16-aligned pointer inside the scratch page. +0x200 leaves
    // room for `sub ebx, 0x80` twice without walking off the page.
    const uint32_t ptr = kMemVa + 0x200u;
    for (int i = 0; i < 8; ++i) {
        in.gpr[static_cast<size_t>(i)] = ptr;
    }
    // State.mem is only 512 bytes. ESP must sit low enough that
    // [esp+0x160] (hottest stack slot in the dump) is initialized;
    // +0x300 made those loads return 0 and the next [edx] SEGVd
    // on the host x86 runner (JIT's 128 MiB linear map swallowed [0]).
    in.gpr[4] = kMemVa + 0x80u;
    for (size_t i = 0; i + 4 <= in.mem.size(); i += 4) {
        const uint32_t p = kMemVa + static_cast<uint32_t>(i & ~0xFu);
        std::memcpy(in.mem.data() + i, &p, 4);
    }
    return in;
}

void bench_body(const std::vector<uint8_t>& body, uint32_t warmup,
                uint32_t iters, SnippetBench& row) {
    bench_body(body, warmup, iters, row, scratch_state());
}

void bench_body(const std::vector<uint8_t>& body, uint32_t warmup,
                uint32_t iters, SnippetBench& row, const State& in) {
    row.body = body;
    row.skip.clear();
    if (body.empty()) {
        row.skip = "empty body";
        return;
    }

    struct sigaction sa{};
    struct sigaction old{};
    sa.sa_handler = on_bench_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, &old);
    if (sigsetjmp(g_bench_jmp, 1) != 0) {
        alarm(0);
        sigaction(SIGALRM, &old, nullptr);
        row.skip = "timeout";
        return;
    }
    alarm(8);

    row.jit = contract::bench_on_jit(in, body, warmup, iters);
    if (row.jit.status != 0) {
        alarm(0);
        sigaction(SIGALRM, &old, nullptr);
        row.skip = "jit status=" + std::to_string(row.jit.status);
        return;
    }
    BenchSession bench;
    if (!bench.load(in, body)) {
        alarm(0);
        sigaction(SIGALRM, &old, nullptr);
        row.skip = "oracle load failed";
        return;
    }
    State once{};
    if (!bench.execute_once(&once) || once.status != 0) {
        alarm(0);
        sigaction(SIGALRM, &old, nullptr);
        row.skip = "oracle once status=" +
                   std::to_string(once.status);
        return;
    }
    if (!bench.warmup(warmup) || !bench.run(iters) || !bench.times(&row.oracle)) {
        alarm(0);
        sigaction(SIGALRM, &old, nullptr);
        row.skip = "oracle bench failed";
        return;
    }
    alarm(0);
    sigaction(SIGALRM, &old, nullptr);
    if (row.oracle.status != 0 || row.oracle.run_ns == 0 ||
        row.jit.run_ns == 0) {
        row.skip = "zero or failed times";
        return;
    }
}

Report bench_dump(const std::vector<DumpBlock>& blocks, uint32_t warmup,
                  uint32_t iters, bool with_windows) {
    Report report;
    for (const DumpBlock& block : blocks) {
        SnippetBench row;
        row.rip = block.rip;
        row.code_variant = block.code_variant;
        row.capture_file = block.capture_file;
        row.exec_count = block.exec_count;
        row.insn_count = block.insn_count;
        row.accounting = block.accounting;
        row.sample_rate = block.sample_rate;
        row.linked_hops_included = block.linked_hops_included;
        row.cls = block.cls.empty() ? "alu" : block.cls;
        char name[32];
        std::snprintf(name, sizeof(name), "0x%llx",
                      static_cast<unsigned long long>(block.rip));
        row.name = name;
        if (block.code_variant != UINT32_MAX) {
            row.name += "#" + std::to_string(block.code_variant);
        }
        if (block.delegated) {
            row.skip = "delegated";
            report.rows.push_back(std::move(row));
            ++report.skipped;
            continue;
        }
        std::vector<uint8_t> body;
        std::string why;
        if (!prepare_body(block.bytes, body, why)) {
            row.skip = why;
            std::fprintf(stderr, "[cpu-perf-dump] skip 0x%llx %s\n",
                         static_cast<unsigned long long>(block.rip),
                         why.c_str());
            report.rows.push_back(std::move(row));
            ++report.skipped;
            continue;
        }
        std::fprintf(stderr, "[cpu-perf-dump] bench 0x%llx class=%s body=%zu\n",
                     static_cast<unsigned long long>(block.rip),
                     row.cls.c_str(), body.size());
        std::fflush(stderr);
        const std::vector<uint8_t> replay_body =
            make_safe_looped_body(body, row.replay_count);
        bench_body(replay_body, warmup, iters, row, dump_scratch_state());
        if (with_windows) attach_windows(row, warmup, iters);
        if (row.skip.empty()) {
            ++report.comparable;
            const double r = ratio(row);
            std::fprintf(stderr,
                         "[cpu-perf-dump] done 0x%llx j/ros=%.2f "
                         "insn=%llu\n",
                         static_cast<unsigned long long>(block.rip), r,
                         static_cast<unsigned long long>(row.insn_count));
        } else {
            ++report.skipped;
            std::fprintf(stderr, "[cpu-perf-dump] fail 0x%llx %s\n",
                         static_cast<unsigned long long>(block.rip),
                         row.skip.c_str());
        }
        std::fflush(stderr);
        report.rows.push_back(std::move(row));
    }
    return report;
}

namespace {

[[nodiscard]] double ns_per(uint64_t run_ns, uint64_t iters) {
    if (iters == 0) return 0.0;
    return static_cast<double>(run_ns) / static_cast<double>(iters);
}

}  // namespace

double ratio(const SnippetBench& row) {
    if (!row.skip.empty()) return 0.0;
    const double oracle_per = ns_per(row.oracle.run_ns, row.oracle.iters);
    const double jit_per = ns_per(row.jit.run_ns, row.jit.iters);
    if (oracle_per <= 0.0 || jit_per <= 0.0) return 0.0;
    return jit_per / oracle_per;
}

double ratio_windows(const SnippetBench& row) {
    if (!row.skip.empty() || !row.windows_skip.empty()) return 0.0;
    const double win_per = ns_per(row.windows.run_ns, row.windows.iters);
    const double jit_per = ns_per(row.jit.run_ns, row.jit.iters);
    if (win_per <= 0.0 || jit_per <= 0.0) return 0.0;
    return jit_per / win_per;
}

void print_report(const Report& report) {
    if (!report.rows.empty()) {
        const auto& provenance = report.rows.front();
        std::fprintf(stdout,
                     "accounting=%s sample_rate=%u linked_hops_included=%s\n",
                     provenance.accounting.c_str(), provenance.sample_rate,
                     provenance.linked_hops_included ? "true" : "false");
    }
    std::fprintf(stdout,
                 "rip              class    body loop  win_ns  ros_ns  jit_ns  "
                 "j/ros  j/win  note\n");
    std::vector<double> ros_ratios;
    std::vector<double> win_ratios;
    for (const SnippetBench& row : report.rows) {
        if (!row.skip.empty()) {
            std::fprintf(stdout,
                         "%-16s %-8s %4zu %4u  %7s  %6s  %6s  %5s  %5s  %s\n",
                         row.name.c_str(), row.cls.c_str(), row.body.size(),
                         row.replay_count, "-", "-", "-", "-", "-",
                         row.skip.c_str());
            continue;
        }
        const double r = ratio(row);
        const double rw = ratio_windows(row);
        ros_ratios.push_back(r);
        if (rw > 0.0) win_ratios.push_back(rw);
        const double scale = static_cast<double>(row.replay_count);
        const double oracle_per =
            ns_per(row.oracle.run_ns, row.oracle.iters) / scale;
        const double jit_per = ns_per(row.jit.run_ns, row.jit.iters) / scale;
        const double win_per =
            ns_per(row.windows.run_ns, row.windows.iters) / scale;
        char win_buf[16];
        char jw_buf[16];
        if (win_per > 0.0) {
            std::snprintf(win_buf, sizeof(win_buf), "%7.1f", win_per);
            std::snprintf(jw_buf, sizeof(jw_buf), "%5.2f", rw);
        } else {
            std::snprintf(win_buf, sizeof(win_buf), "%7s", "-");
            std::snprintf(jw_buf, sizeof(jw_buf), "%5s", "-");
        }
        const char* hotspot = (r > 2.0 || rw > 2.0) ? "HOTSPOT >2x" : "";
        std::fprintf(stdout,
                     "%-16s %-8s %4zu %4u  %s  %6.1f  %6.1f  %5.2f  %s  "
                     "%s%s%s\n",
                     row.name.c_str(), row.cls.c_str(), row.body.size(),
                     row.replay_count, win_buf, oracle_per, jit_per, r, jw_buf,
                     hotspot, hotspot[0] && !row.windows_skip.empty() ? "; " : "",
                     row.windows_skip.c_str());
    }
    std::fprintf(stdout, "comparable=%u skipped=%u\n", report.comparable,
                 report.skipped);
    if (!ros_ratios.empty()) {
        std::sort(ros_ratios.begin(), ros_ratios.end());
        std::fprintf(stdout, "median JIT/oracle ratio=%.2f\n",
                     ros_ratios[ros_ratios.size() / 2]);
    }
    if (!win_ratios.empty()) {
        std::sort(win_ratios.begin(), win_ratios.end());
        std::fprintf(stdout, "median JIT/Windows ratio=%.2f\n",
                     win_ratios[win_ratios.size() / 2]);
    }

    uint64_t dump_insns = 0;
    uint64_t replay_insns = 0;
    uint64_t skip_insns = 0;
    uint64_t replay_execs = 0;
    double insn_weighted_ros = 0.0;
    double exec_weighted_ros = 0.0;
    double insn_weighted_win = 0.0;
    double exec_weighted_win = 0.0;
    uint64_t replay_win_insns = 0;
    uint64_t replay_win_execs = 0;
    for (const SnippetBench& row : report.rows) {
        dump_insns += row.insn_count;
        if (row.skip.empty()) {
            replay_insns += row.insn_count;
            replay_execs += row.exec_count;
            insn_weighted_ros +=
                ratio(row) * static_cast<double>(row.insn_count);
            exec_weighted_ros +=
                ratio(row) * static_cast<double>(row.exec_count);
            const double rw = ratio_windows(row);
            if (rw > 0.0) {
                replay_win_insns += row.insn_count;
                replay_win_execs += row.exec_count;
                insn_weighted_win += rw * static_cast<double>(row.insn_count);
                exec_weighted_win += rw * static_cast<double>(row.exec_count);
            }
        } else {
            skip_insns += row.insn_count;
        }
    }
    if (dump_insns > 0) {
        std::fprintf(stdout,
                     "dump insn_count total=%llu replayed=%llu (%.0f%%) "
                     "skipped=%llu\n",
                     static_cast<unsigned long long>(dump_insns),
                     static_cast<unsigned long long>(replay_insns),
                     100.0 * static_cast<double>(replay_insns) /
                         static_cast<double>(dump_insns),
                     static_cast<unsigned long long>(skip_insns));
    }
    if (replay_insns > 0) {
        std::fprintf(stdout,
                     "insn-weighted JIT/oracle=%.2f  "
                     "(ratio × dump insn_count / replayed)\n",
                     insn_weighted_ros / static_cast<double>(replay_insns));
    }
    if (replay_execs > 0) {
        std::fprintf(stdout, "exec-weighted JIT/oracle=%.2f\n",
                     exec_weighted_ros / static_cast<double>(replay_execs));
    }
    if (replay_win_insns > 0) {
        std::fprintf(stdout, "insn-weighted JIT/Windows=%.2f\n",
                     insn_weighted_win /
                         static_cast<double>(replay_win_insns));
    }
    if (replay_win_execs > 0) {
        std::fprintf(stdout, "exec-weighted JIT/Windows=%.2f\n",
                     exec_weighted_win /
                         static_cast<double>(replay_win_execs));
    }
}

namespace {

void write_json_string(std::ostream& out, const std::string& value) {
    out << '"';
    for (const unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    char escaped[7];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out << escaped;
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    out << '"';
}

void write_csv_field(std::ostream& out, const std::string& value) {
    out << '"';
    for (char c : value) {
        if (c == '"') out << '"';
        out << c;
    }
    out << '"';
}

[[nodiscard]] double row_ns(uint64_t run_ns, uint64_t iters,
                            uint32_t replay_count) {
    const double per = ns_per(run_ns, iters);
    return replay_count == 0 ? per : per / replay_count;
}

struct WeightedRatios {
    double insn_oracle = 0.0;
    double exec_oracle = 0.0;
    double insn_windows = 0.0;
    double exec_windows = 0.0;
};

[[nodiscard]] WeightedRatios weighted_ratios(const Report& report) {
    WeightedRatios result;
    uint64_t oracle_insns = 0;
    uint64_t oracle_execs = 0;
    uint64_t win_insns = 0;
    uint64_t win_execs = 0;
    for (const auto& row : report.rows) {
        const double rr = ratio(row);
        if (rr > 0.0) {
            oracle_insns += row.insn_count;
            oracle_execs += row.exec_count;
            result.insn_oracle += rr * static_cast<double>(row.insn_count);
            result.exec_oracle += rr * static_cast<double>(row.exec_count);
        }
        const double rw = ratio_windows(row);
        if (rw > 0.0) {
            win_insns += row.insn_count;
            win_execs += row.exec_count;
            result.insn_windows += rw * static_cast<double>(row.insn_count);
            result.exec_windows += rw * static_cast<double>(row.exec_count);
        }
    }
    if (oracle_insns) result.insn_oracle /= static_cast<double>(oracle_insns);
    if (oracle_execs) result.exec_oracle /= static_cast<double>(oracle_execs);
    if (win_insns) result.insn_windows /= static_cast<double>(win_insns);
    if (win_execs) result.exec_windows /= static_cast<double>(win_execs);
    return result;
}

}  // namespace

bool write_report_json(const Report& report, const std::string& path,
                       std::string& error) {
    std::ofstream out(path);
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    const WeightedRatios weighted = weighted_ratios(report);
    out << "{\"schema\":\"vitreo.cpu_perf.v1\",\"comparable\":"
        << report.comparable << ",\"skipped\":" << report.skipped
        << ",\"weighted\":{\"insn_jit_oracle\":"
        << weighted.insn_oracle << ",\"exec_jit_oracle\":"
        << weighted.exec_oracle << ",\"insn_jit_windows\":"
        << weighted.insn_windows << ",\"exec_jit_windows\":"
        << weighted.exec_windows << "},\"rows\":[\n";
    bool first = true;
    for (const auto& row : report.rows) {
        if (!first) out << ",\n";
        first = false;
        out << "{\"name\":";
        write_json_string(out, row.name);
        out << ",\"class\":";
        write_json_string(out, row.cls);
        out << ",\"capture_file\":";
        write_json_string(out, row.capture_file);
        out << ",\"rip\":" << row.rip << ",\"code_variant\":";
        if (row.code_variant == UINT32_MAX) out << "null";
        else out << row.code_variant;
        out << ",\"exec_count\":" << row.exec_count
            << ",\"insn_count\":" << row.insn_count
            << ",\"accounting\":";
        write_json_string(out, row.accounting);
        out << ",\"sample_rate\":" << row.sample_rate
            << ",\"linked_hops_included\":"
            << (row.linked_hops_included ? "true" : "false")
            << ",\"replay_count\":" << row.replay_count << ",\"jit_ns\":"
            << row_ns(row.jit.run_ns, row.jit.iters, row.replay_count)
            << ",\"oracle_ns\":"
            << row_ns(row.oracle.run_ns, row.oracle.iters, row.replay_count)
            << ",\"windows_ns\":"
            << row_ns(row.windows.run_ns, row.windows.iters, row.replay_count)
            << ",\"jit_oracle_ratio\":" << ratio(row)
            << ",\"jit_windows_ratio\":" << ratio_windows(row)
            << ",\"hotspot_gt_2x\":"
            << ((ratio(row) > 2.0 || ratio_windows(row) > 2.0) ? "true"
                                                               : "false")
            << ",\"skip\":";
        write_json_string(out, row.skip);
        out << ",\"windows_skip\":";
        write_json_string(out, row.windows_skip);
        out << '}';
    }
    out << "\n]}\n";
    if (!out) {
        error = "write failed for " + path;
        return false;
    }
    error.clear();
    return true;
}

bool write_report_csv(const Report& report, const std::string& path,
                      std::string& error) {
    std::ofstream out(path);
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    out << "name,class,capture_file,rip,exec_count,insn_count,accounting,sample_rate,"
           "linked_hops_included,replay_count,jit_ns,"
           "oracle_ns,windows_ns,jit_oracle_ratio,jit_windows_ratio,"
           "hotspot_gt_2x,skip,windows_skip,code_variant\n";
    for (const auto& row : report.rows) {
        write_csv_field(out, row.name);
        out << ',';
        write_csv_field(out, row.cls);
        out << ',';
        write_csv_field(out, row.capture_file);
        out << ',' << row.rip << ',' << row.exec_count << ',' << row.insn_count
            << ',';
        write_csv_field(out, row.accounting);
        out << ',' << row.sample_rate << ','
            << (row.linked_hops_included ? "true" : "false") << ','
            << row.replay_count << ','
            << row_ns(row.jit.run_ns, row.jit.iters, row.replay_count) << ','
            << row_ns(row.oracle.run_ns, row.oracle.iters, row.replay_count)
            << ','
            << row_ns(row.windows.run_ns, row.windows.iters, row.replay_count)
            << ',' << ratio(row) << ',' << ratio_windows(row) << ','
            << ((ratio(row) > 2.0 || ratio_windows(row) > 2.0) ? "true"
                                                               : "false")
            << ',';
        write_csv_field(out, row.skip);
        out << ',';
        write_csv_field(out, row.windows_skip);
        out << ',';
        if (row.code_variant != UINT32_MAX) out << row.code_variant;
        out << '\n';
    }
    if (!out) {
        error = "write failed for " + path;
        return false;
    }
    error.clear();
    return true;
}

namespace {

[[nodiscard]] std::vector<uint8_t> x64_repeat_bounded(
    const std::vector<uint8_t>& kernel, uint32_t& repetitions) {
    // The current long-mode contract executes one compiled block. A backward
    // loop exits that block before reaching code_end, so repeat the kernel in
    // place. Fill most of the Windows runner's 256-byte code window so the
    // native work is large enough to survive wrapper subtraction.
    constexpr size_t kBodyBudget = 240u;
    if (kernel.empty() || kernel.size() > kBodyBudget) return {};
    repetitions = static_cast<uint32_t>(kBodyBudget / kernel.size());
    std::vector<uint8_t> body;
    body.reserve(kernel.size() * repetitions);
    for (uint32_t i = 0; i < repetitions; ++i) {
        body.insert(body.end(), kernel.begin(), kernel.end());
    }
    return body;
}

[[nodiscard]] State x64_catalog_state() {
    State in{};
    in.mode = kMode64;
    in.rflags = 0x202;
    in.gpr[4] = 0;  // Harness-owned controlled stack.
    in.gpr[8] = 1;
    in.gpr[9] = 3;
    in.gpr[11] = kMemVa;
    in.gpr[12] = 5;
    in.mxcsr = 0x1F80;
    for (size_t reg = 0; reg < in.xmm.size(); ++reg) {
        for (size_t lane = 0; lane < 4; ++lane) {
            const float value = static_cast<float>(reg * 4 + lane + 1);
            std::memcpy(in.xmm[reg].data() + lane * sizeof(value), &value,
                        sizeof(value));
        }
    }
    for (size_t offset = 0; offset + sizeof(uint64_t) <= in.mem.size();
         offset += sizeof(uint64_t)) {
        const uint64_t value = 0x0102030405060708ull + offset;
        std::memcpy(in.mem.data() + offset, &value, sizeof(value));
    }
    return in;
}

[[nodiscard]] BenchTimes as_bench_times(
    const contract::JitBenchResult& result) {
    BenchTimes times{};
    times.warmup_ns = result.warmup_ns;
    times.run_ns = result.run_ns;
    times.iters = result.iters;
    times.status = result.status;
    return times;
}

}  // namespace

std::vector<X64CatalogEntry> cpu_perf_x64_catalog() {
    const State in = x64_catalog_state();
    State x87_in = in;
    // Windows x64's CRT execution environment uses double precision for x87.
    // Carry it through the existing oracle state protocol instead of issuing
    // FNINIT inside every timed body (which would force PC=64 and measure only
    // Vitreo's deliberate correctness fallback).
    x87_in.fcw = 0x027F;
    const double x87_lhs = 1.5;
    const double x87_rhs = 2.25;
    const double x87_sqrt = 4.0;
    std::memcpy(x87_in.mem.data(), &x87_lhs, sizeof(x87_lhs));
    std::memcpy(x87_in.mem.data() + 8, &x87_rhs, sizeof(x87_rhs));
    std::memcpy(x87_in.mem.data() + 16, &x87_sqrt, sizeof(x87_sqrt));
    const auto entry = [&](const char* name, const char* cls,
                           std::vector<uint8_t> kernel,
                           bool multiblock = false,
                           uint32_t operations_per_kernel = 1) {
        uint32_t repetitions = 0;
        std::vector<uint8_t> body =
            x64_repeat_bounded(kernel, repetitions);
        return X64CatalogEntry{name, cls, std::move(body), in,
                               repetitions * operations_per_kernel,
                               multiblock};
    };
    const auto x87_entry = [&](const char* name, std::vector<uint8_t> kernel,
                               uint32_t x87_ops_per_kernel) {
        uint32_t repetitions = 0;
        std::vector<uint8_t> body = x64_repeat_bounded(kernel, repetitions);
        // The State wire image establishes FCW/FSW/FTW identically on Vitreo
        // and Windows. Each repeated kernel is stack-balanced, so the timed
        // stream stays deterministic without an in-body FNINIT tax.
        return X64CatalogEntry{name, "x87", std::move(body), x87_in,
                               repetitions * x87_ops_per_kernel};
    };
    return {
        entry("x64_add_dep", "integer", {0x4C, 0x01, 0xC0}),
        entry("x64_high_gpr_mix", "integer",
              {0x4D, 0x01, 0xC8,               // add r8,r9
               0x4D, 0x31, 0xC3,               // xor r11,r8
               0x4F, 0x8D, 0x64, 0x4B, 0x08}), // lea r12,[r11+r9*2+8]
        entry("x64_load64", "memory", {0x49, 0x8B, 0x03}),
        entry("x64_store64", "memory", {0x4D, 0x89, 0x43, 0x08}),
        entry("x64_rmw64", "memory", {0x4D, 0x01, 0x43, 0x10}),
        entry("x64_cmp_jcc_not_taken", "branch",
              {0x4D, 0x39, 0xC0,               // cmp r8,r8
               0x75, 0x04,                     // jne after add
               0x48, 0x83, 0xC0, 0x01}),       // add rax,1
        entry("x64_call_ret_direct", "call-ret",
              {0xE8, 0x02, 0x00, 0x00, 0x00,  // call local ret
               0xEB, 0x01,                    // skip callee after return
               0xC3},                         // ret
              true, 2),
        entry("x64_call_ret_indirect", "call-ret",
              {0x4C, 0x8D, 0x15, 0x08, 0x00, 0x00, 0x00,  // lea r10,[ret]
               0x41, 0xFF, 0xD2,                          // call r10
               0x4D, 0x31, 0xD2,                          // xor r10,r10
               0xEB, 0x01,                                // skip callee
               0xC3},                                     // ret
              true, 2),
        entry("x64_addps_dep", "simd-fp",
              {0x0F, 0x58, 0xC1}),              // addps xmm0,xmm1
        entry("x64_paddq_high", "simd-int",
              {0x66, 0x45, 0x0F, 0xD4, 0xC1}),  // paddq xmm8,xmm9
        entry("x64_pblendw", "simd-int",
              {0x66, 0x0F, 0x3A, 0x0E, 0xC1,
               0x55}),                           // pblendw xmm0,xmm1,0x55
        entry("x64_movdqu_load_unaligned", "simd-memory",
              {0xF3, 0x41, 0x0F, 0x6F, 0x43,
               0x01}),                           // movdqu xmm0,[r11+1]
        x87_entry("x64_x87_fld1_fadd_fstp", {
                      0xD9, 0xE8,                 // fld1
                      0xD8, 0xC0,                 // fadd st(0),st(0)
                      0x41, 0xDD, 0x9B, 0x00, 0x01, 0x00, 0x00,
                  }, 3),                          // fstp qword [r11+0x100]
        x87_entry("x64_x87_fld_fadd_m64", {
                      0x41, 0xDD, 0x03,           // fld qword [r11]
                      0x41, 0xDC, 0x43, 0x08,     // fadd qword [r11+8]
                      0x41, 0xDD, 0x9B, 0x08, 0x01, 0x00, 0x00,
                  }, 3),                          // fstp qword [r11+0x108]
        x87_entry("x64_x87_fld_fmul_m64", {
                      0x41, 0xDD, 0x03,           // fld qword [r11]
                      0x41, 0xDC, 0x4B, 0x08,     // fmul qword [r11+8]
                      0x41, 0xDD, 0x9B, 0x10, 0x01, 0x00, 0x00,
                  }, 3),                          // fstp qword [r11+0x110]
        x87_entry("x64_x87_fld_fsqrt_fstp", {
                      0x41, 0xDD, 0x43, 0x10,     // fld qword [r11+16]
                      0xD9, 0xFA,                 // fsqrt
                      0x41, 0xDD, 0x9B, 0x18, 0x01, 0x00, 0x00,
                  }, 3),                          // fstp qword [r11+0x118]
        x87_entry("x64_x87_fcompp_fnstsw", {
                      0xD9, 0xE8,                 // fld1
                      0xD9, 0xEE,                 // fldz
                      0xDE, 0xD9,                 // fcompp
                      0xDF, 0xE0,                 // fnstsw ax
                      0x66, 0x41, 0x89, 0x83, 0x20, 0x01, 0x00, 0x00,
                  }, 4),                          // mov [r11+0x120],ax
    };
}

bool bench_x64_jit_normalized(const std::vector<uint8_t>& body,
                              const State& in, uint32_t warmup,
                              uint32_t iters, X64JitNormalizedResult& result,
                              std::string& error, bool multiblock) {
    result = {};
    if (body.empty() || in.mode != kMode64) {
        error = "x64 JIT normalized benchmark requires a body and long mode";
        return false;
    }

    constexpr uint32_t kMaxIters = 10000000u;
    uint32_t count = std::min(std::max(iters, 1000u), kMaxIters);
    const auto measure_pair = [&]() {
        if (multiblock) {
            const auto raw = contract::bench_on_jit_64_multiblock(
                in, body, warmup, count);
            const auto wrapper = contract::bench_on_jit_64_multiblock(
                in, {}, warmup, count);
            result.raw = raw.bench;
            result.wrapper = wrapper.bench;
            result.raw_stack_delta = raw.stack_delta;
            result.wrapper_stack_delta = wrapper.stack_delta;
            result.raw_blocks_compiled = raw.blocks_compiled;
            result.raw_final_dispatches = raw.final_dispatches;
            result.wrapper_blocks_compiled = wrapper.blocks_compiled;
            result.wrapper_final_dispatches = wrapper.final_dispatches;
        } else {
            result.raw = contract::bench_on_jit_64(in, body, warmup, count);
            result.wrapper = contract::bench_on_jit_64(in, {}, warmup, count);
            result.raw_stack_delta = 0;
            result.wrapper_stack_delta = 0;
        }
    };
    measure_pair();

    BenchTimes raw = as_bench_times(result.raw);
    BenchTimes wrapper = as_bench_times(result.wrapper);
    if (raw.status != 0 || wrapper.status != 0 || raw.run_ns == 0 ||
        wrapper.run_ns == 0 || raw.iters != wrapper.iters) {
        error = "x64 JIT normalized benchmark status failure raw=" +
                std::to_string(raw.status) + " wrapper=" +
                std::to_string(wrapper.status);
        return false;
    }

    constexpr uint64_t kMinRunNs = 1000000ull;
    const uint64_t shorter = std::min(raw.run_ns, wrapper.run_ns);
    if (shorter < kMinRunNs) {
        const uint64_t scaled = std::min<uint64_t>(
            kMaxIters,
            (kMinRunNs * static_cast<uint64_t>(count) * 2ull) / shorter);
        if (scaled > count) {
            count = static_cast<uint32_t>(scaled);
            measure_pair();
            raw = as_bench_times(result.raw);
            wrapper = as_bench_times(result.wrapper);
        }
    }
    if (raw.status != 0 || wrapper.status != 0 || raw.run_ns == 0 ||
        wrapper.run_ns == 0 || raw.iters != wrapper.iters ||
        !normalize_bench_times(raw, wrapper, result.ns) ||
        (multiblock && (result.raw_stack_delta != 0 ||
                        result.wrapper_stack_delta != 0))) {
        error = "x64 JIT normalized benchmark invalid counters";
        return false;
    }
    error.clear();
    return true;
}

namespace {

[[nodiscard]] bool valid_jit_trial(const X64CatalogTrial& trial) {
    return trial.skip.empty() && trial.jit.raw.status == 0 &&
           trial.jit.wrapper.status == 0 && trial.jit.raw.iters != 0 &&
           trial.jit.wrapper.iters != 0;
}

[[nodiscard]] bool valid_windows_trial(const X64CatalogTrial& trial) {
    return trial.windows_skip.empty() && trial.windows.raw.times.status == 0 &&
           trial.windows.wrapper.status == 0 &&
           trial.windows.raw.times.iters != 0 &&
           trial.windows.wrapper.iters != 0 &&
           (trial.windows.raw.initial_rsp == 0 ||
            trial.windows.raw.gpr[4] == trial.windows.raw.initial_rsp);
}

[[nodiscard]] double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    if ((values.size() & 1u) != 0) return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

void write_normalized_json(std::ostream& out, const NormalizedNs& ns) {
    out << "{\"raw_ns_per_iter\":" << ns.raw_ns_per_iter
        << ",\"wrapper_ns_per_iter\":" << ns.wrapper_ns_per_iter
        << ",\"net_ns_per_iter\":" << ns.net_ns_per_iter << '}';
}

void write_exact_times_json(std::ostream& out, const BenchTimes& times) {
    out << "{\"warmup_ns\":" << times.warmup_ns << ",\"run_ns\":"
        << times.run_ns << ",\"iters\":" << times.iters
        << ",\"status\":" << times.status << '}';
}

}  // namespace

void summarize_x64_trials(X64CatalogRow& row) {
    row.median = {};
    std::vector<double> jit_raw;
    std::vector<double> jit_wrapper;
    std::vector<double> jit_net;
    std::vector<double> windows_raw;
    std::vector<double> windows_wrapper;
    std::vector<double> windows_net;
    std::vector<double> raw_ratios;
    std::vector<double> net_ratios;
    for (const X64CatalogTrial& trial : row.trials) {
        const bool jit_valid = valid_jit_trial(trial);
        const bool windows_valid = valid_windows_trial(trial);
        if (jit_valid) {
            jit_raw.push_back(trial.jit.ns.raw_ns_per_iter);
            jit_wrapper.push_back(trial.jit.ns.wrapper_ns_per_iter);
            jit_net.push_back(trial.jit.ns.net_ns_per_iter);
        }
        if (windows_valid) {
            windows_raw.push_back(trial.windows.ns.raw_ns_per_iter);
            windows_wrapper.push_back(trial.windows.ns.wrapper_ns_per_iter);
            windows_net.push_back(trial.windows.ns.net_ns_per_iter);
        }
        if (!jit_valid || !windows_valid) continue;
        if (trial.jit.ns.raw_ns_per_iter > 0.0 &&
            trial.windows.ns.raw_ns_per_iter > 0.0) {
            raw_ratios.push_back(trial.jit.ns.raw_ns_per_iter /
                                 trial.windows.ns.raw_ns_per_iter);
        }
        if (trial.jit.ns.net_ns_per_iter > 0.0 &&
            trial.windows.ns.net_ns_per_iter > 0.0) {
            net_ratios.push_back(trial.jit.ns.net_ns_per_iter /
                                 trial.windows.ns.net_ns_per_iter);
        }
    }
    row.median.jit_valid = !jit_raw.empty();
    row.median.windows_valid = !windows_raw.empty();
    row.median.jit = {median(jit_raw), median(jit_wrapper), median(jit_net)};
    row.median.windows = {median(windows_raw), median(windows_wrapper),
                          median(windows_net)};
    row.median.raw_ratio_valid = !raw_ratios.empty();
    row.median.raw_ratio = median(raw_ratios);
    row.median.net_ratio_valid = !net_ratios.empty();
    row.median.net_ratio = median(net_ratios);
}

X64CatalogReport bench_x64_catalog_trials(uint32_t warmup, uint32_t iters,
                                          uint32_t trial_count,
                                          bool with_windows) {
    X64CatalogReport report;
    trial_count = std::max(1u, trial_count);
    for (const X64CatalogEntry& entry : cpu_perf_x64_catalog()) {
        X64CatalogRow row;
        row.name = entry.name;
        row.cls = entry.cls;
        row.body = entry.body;
        row.operations_per_iter = entry.operations_per_iter;
        row.multiblock = entry.multiblock;
        row.trials.reserve(trial_count);
        for (uint32_t trial_index = 0; trial_index < trial_count;
             ++trial_index) {
            X64CatalogTrial trial;
            (void)bench_x64_jit_normalized(entry.body, entry.in, warmup,
                                           iters, trial.jit, trial.skip,
                                           entry.multiblock);
            if (with_windows) {
                (void)bench_on_windows64_normalized(
                    entry.body, entry.in, warmup, iters, trial.windows,
                    trial.windows_skip);
                if (entry.multiblock && trial.windows_skip.empty() &&
                    trial.windows.raw.gpr[4] !=
                        trial.windows.raw.initial_rsp) {
                    trial.windows_skip =
                        "x64 Windows CALL/RET benchmark left RSP unbalanced";
                }
            }
            row.trials.push_back(std::move(trial));
        }
        row.jit = row.trials.front().jit;
        row.windows = row.trials.front().windows;
        row.skip = row.trials.front().skip;
        row.windows_skip = row.trials.front().windows_skip;
        summarize_x64_trials(row);
        if (row.median.jit_valid) ++report.comparable;
        else ++report.skipped;
        if (row.median.windows_valid) ++report.windows_comparable;
        report.rows.push_back(std::move(row));
    }
    return report;
}

X64CatalogReport bench_x64_catalog(uint32_t warmup, uint32_t iters,
                                   bool with_windows) {
    return bench_x64_catalog_trials(warmup, iters, 1, with_windows);
}

bool write_x64_report_json(const X64CatalogReport& report,
                           const std::string& path, std::string& error) {
    std::ofstream out(path);
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << "{\"schema\":\"vitreo.cpu_perf.x64.v1\",\"comparable\":"
        << report.comparable << ",\"windows_comparable\":"
        << report.windows_comparable << ",\"skipped\":" << report.skipped
        << ",\"rows\":[\n";
    bool first_row = true;
    for (const X64CatalogRow& row : report.rows) {
        if (!first_row) out << ",\n";
        first_row = false;
        out << "{\"name\":";
        write_json_string(out, row.name);
        out << ",\"class\":";
        write_json_string(out, row.cls);
        out << ",\"operations_per_iter\":" << row.operations_per_iter
            << ",\"multiblock\":" << (row.multiblock ? "true" : "false")
            << ",\"median\":{\"jit_valid\":"
            << (row.median.jit_valid ? "true" : "false")
            << ",\"jit\":";
        write_normalized_json(out, row.median.jit);
        out << ",\"windows_valid\":"
            << (row.median.windows_valid ? "true" : "false")
            << ",\"windows\":";
        write_normalized_json(out, row.median.windows);
        out << ",\"raw_ratio\":";
        if (row.median.raw_ratio_valid) out << row.median.raw_ratio;
        else out << "null";
        out << ",\"net_ratio\":";
        if (row.median.net_ratio_valid) out << row.median.net_ratio;
        else out << "null";
        out << "},\"trials\":[";
        bool first_trial = true;
        for (const X64CatalogTrial& trial : row.trials) {
            if (!first_trial) out << ',';
            first_trial = false;
            out << "{\"jit\":{\"raw\":";
            write_exact_times_json(out, as_bench_times(trial.jit.raw));
            out << ",\"stack_delta\":" << trial.jit.raw_stack_delta;
            out << ",\"blocks_compiled\":"
                << trial.jit.raw_blocks_compiled
                << ",\"final_dispatches\":"
                << trial.jit.raw_final_dispatches;
            out << ",\"wrapper\":";
            write_exact_times_json(out, as_bench_times(trial.jit.wrapper));
            out << ",\"wrapper_stack_delta\":"
                << trial.jit.wrapper_stack_delta;
            out << ",\"wrapper_blocks_compiled\":"
                << trial.jit.wrapper_blocks_compiled
                << ",\"wrapper_final_dispatches\":"
                << trial.jit.wrapper_final_dispatches;
            out << ",\"normalized\":";
            write_normalized_json(out, trial.jit.ns);
            out << "},\"windows\":{\"raw\":";
            write_exact_times_json(out, trial.windows.raw.times);
            out << ",\"stack_delta\":"
                << (static_cast<int64_t>(trial.windows.raw.gpr[4]) -
                    static_cast<int64_t>(trial.windows.raw.initial_rsp));
            out << ",\"wrapper\":";
            write_exact_times_json(out, trial.windows.wrapper);
            out << ",\"normalized\":";
            write_normalized_json(out, trial.windows.ns);
            out << "},\"skip\":";
            write_json_string(out, trial.skip);
            out << ",\"windows_skip\":";
            write_json_string(out, trial.windows_skip);
            out << '}';
        }
        out << "]}";
    }
    out << "\n]}\n";
    if (!out) {
        error = "write failed for " + path;
        return false;
    }
    error.clear();
    return true;
}

}  // namespace vitreo::oracle::perf
