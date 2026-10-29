// SPDX-License-Identifier: LGPL-2.1-only
#include "oracle/cpu_perf_catalog.hpp"

#include <cstring>
#include <utility>

namespace vitreo::oracle::perf {

std::vector<uint8_t> loop256(const std::vector<uint8_t>& kernel) {
    if (kernel.empty() || kernel.size() + 5u > 128u) {
        return {};
    }
    std::vector<uint8_t> out{0xB9, 0x00, 0x01, 0x00, 0x00};  // mov ecx, 256
    out.insert(out.end(), kernel.begin(), kernel.end());
    out.insert(out.end(), {0x83, 0xE9, 0x01});  // sub ecx, 1
    const int rel = -static_cast<int>(kernel.size() + 5u);
    out.push_back(0x75);
    out.push_back(static_cast<uint8_t>(rel));
    return out;
}

std::vector<uint8_t> loop256_esi(const std::vector<uint8_t>& kernel) {
    if (kernel.empty() || kernel.size() + 5u > 128u) {
        return {};
    }
    std::vector<uint8_t> out{0xBE, 0x00, 0x01, 0x00, 0x00};  // mov esi, 256
    out.insert(out.end(), kernel.begin(), kernel.end());
    out.insert(out.end(), {0x83, 0xEE, 0x01});  // sub esi, 1
    const int rel = -static_cast<int>(kernel.size() + 5u);
    out.push_back(0x75);
    out.push_back(static_cast<uint8_t>(rel));
    return out;
}

std::vector<uint8_t> prefix_loop256(const std::vector<uint8_t>& prefix,
                                    const std::vector<uint8_t>& kernel) {
    auto out = prefix;
    const auto loop = loop256(kernel);
    out.insert(out.end(), loop.begin(), loop.end());
    return out;
}

std::vector<CatalogEntry> cpu_perf_catalog() {
    return {
        {"walk_loop256", "alu",
         loop256({0x03, 0x06, 0x83, 0xC6, 0x04})},  // add eax,[esi]; add esi,4
        {"alu_add_reg", "alu", loop256({0x01, 0xD0})},          // add eax, edx
        {"alu_xor_reg", "alu", loop256({0x31, 0xD8})},          // xor eax, ebx
        {"alu_lea", "alu", loop256({0x8D, 0x46, 0x04})},        // lea eax,[esi+4]
        {"alu_shl", "alu", loop256({0xD1, 0xE0})},              // shl eax, 1
        {"alu_imul", "alu", loop256({0x0F, 0xAF, 0xC2})},       // imul eax, edx

        {"mem_load32", "mem", loop256({0x8B, 0x06})},           // mov eax,[esi]
        {"mem_store32", "mem", loop256({0x89, 0x07})},          // mov [edi],eax
        {"mem_load_store", "mem",
         loop256({0x8B, 0x06, 0x89, 0x07})},                    // load; store

        {"sse_addps_reg", "sse", loop256({0x0F, 0x58, 0xC1})},  // addps xmm0,xmm1
        {"sse_mulps_reg", "sse", loop256({0x0F, 0x59, 0xC1})},  // mulps xmm0,xmm1
        {"sse_movaps_load", "sse", loop256({0x0F, 0x28, 0x06})},  // movaps xmm0,[esi]
        {"sse_addps_mem", "sse", loop256({0x0F, 0x58, 0x06})},  // addps xmm0,[esi]
        {"sse_movaps_store", "sse", loop256({0x0F, 0x29, 0x07})},  // movaps [edi],xmm0
        {"sse_paddd_reg", "sse",
         loop256({0x66, 0x0F, 0xFE, 0xC1})},                    // paddd xmm0,xmm1
        {"sse_pxor_self", "sse",
         loop256({0x66, 0x0F, 0xEF, 0xC0})},                    // pxor xmm0,xmm0
        {"sse_pxor_paddd_chain", "sse",
         loop256({0x66, 0x0F, 0xEF, 0xC1,                     // pxor xmm0,xmm1
                  0x66, 0x0F, 0xFE, 0xC2})},                  // paddd xmm0,xmm2
        {"sse_mixed_rr_chain", "sse",
         loop256({0x66, 0x0F, 0xEF, 0xC1,                     // pxor xmm0,xmm1
                  0x66, 0x0F, 0xED, 0xC2,                     // paddsw xmm0,xmm2
                  0x66, 0x0F, 0xFE, 0xC3})},                  // paddd xmm0,xmm3
        {"sse_movaps_bridge", "sse",
         loop256({0x66, 0x0F, 0xEF, 0xC1,                     // pxor xmm0,xmm1
                  0x0F, 0x28, 0xD0,                           // movaps xmm2,xmm0
                  0x66, 0x0F, 0xFE, 0xD3})},                  // paddd xmm2,xmm3

        {"x87_fadd_st", "x87",
         prefix_loop256({0xD9, 0xE8}, {0xD8, 0xC0})},           // fld1; fadd st,st
        {"x87_fmul_st", "x87",
         prefix_loop256({0xD9, 0xE8}, {0xD8, 0xC8})},           // fld1; fmul st,st
        {"x87_fadd_m32", "x87",
         loop256({0xD9, 0x06, 0xD8, 0x06, 0xD9, 0x1F})},  // fld/fadd/fstp m32

        {"jcc_not_taken", "jcc",
         loop256({0x39, 0xC0, 0x75, 0x03, 0x83, 0xC0, 0x01})},  // cmp eax,eax; jne; add
        // Forward taken Jcc exits the compiled block (status=5). CMOVcc
        // keeps the condition in-block.
        {"cmovz", "jcc",
         loop256({0x39, 0xC0, 0x0F, 0x44, 0xD3})},              // cmp eax,eax; cmovz edx,ebx
    };
}

Report bench_catalog(uint32_t warmup, uint32_t iters, bool with_windows) {
    Report report;
    for (const CatalogEntry& entry : cpu_perf_catalog()) {
        SnippetBench row;
        row.name = entry.name;
        row.cls = entry.cls;
        bench_body(entry.body, warmup, iters, row);
        if (with_windows && row.skip.empty()) {
            attach_windows(row, warmup, iters);
        }
        if (row.skip.empty()) {
            ++report.comparable;
        } else {
            ++report.skipped;
        }
        report.rows.push_back(std::move(row));
    }
    return report;
}


std::vector<uint8_t> loop256_ebx(const std::vector<uint8_t>& kernel) {
    if (kernel.empty() || kernel.size() + 5u > 128u) {
        return {};
    }
    std::vector<uint8_t> out{0xBB, 0x00, 0x01, 0x00, 0x00};  // mov ebx, 256
    out.insert(out.end(), kernel.begin(), kernel.end());
    out.insert(out.end(), {0x83, 0xEB, 0x01});  // sub ebx, 1
    const int rel = -static_cast<int>(kernel.size() + 5u);
    out.push_back(0x75);
    out.push_back(static_cast<uint8_t>(rel));
    return out;
}

std::vector<uint8_t> stack_loop256(const std::vector<uint8_t>& kernel) {
    // The Windows trampoline and the host x86 runner both return on the
    // host stack, and scratch_state leaves ESP at 0. Park ESP in the
    // scratch page for the loop and put it back before falling through.
    std::vector<uint8_t> out{0x89, 0xE5};              // mov ebp, esp
    out.insert(out.end(), {0xBC, 0x00, 0x01, 0x00, 0x04});  // mov esp,0x4000100
    const auto loop = loop256(kernel);
    out.insert(out.end(), loop.begin(), loop.end());
    out.insert(out.end(), {0x89, 0xEC});               // mov esp, ebp
    return out;
}

State fp_scratch_state() {
    State in = scratch_state();
    const uint32_t one = 0x3f800000u;  // 1.0f
    for (size_t i = 0; i + 4 <= in.mem.size(); i += 4) {
        std::memcpy(in.mem.data() + i, &one, 4);
    }
    return in;
}

State chase_scratch_state() {
    State in = scratch_state();
    for (size_t i = 0; i + 4 <= in.mem.size(); i += 4) {
        // 16-aligned ring inside the 512-byte window.
        const uint32_t p = kMemVa + static_cast<uint32_t>((i + 16u) & 0x1F0u);
        std::memcpy(in.mem.data() + i, &p, 4);
    }
    return in;
}

std::vector<ClassEntry> cpu_perf_class_catalog() {
    const State base = scratch_state();
    const State fp = fp_scratch_state();
    const State chase = chase_scratch_state();

    std::vector<ClassEntry> rows;
    const auto add = [&rows](const char* name, const char* cls,
                            std::vector<uint8_t> body, const State& in) {
        rows.push_back(ClassEntry{name, cls, std::move(body), in});
    };

    // ---- harness baselines -------------------------------------------
    add("base_nop1", "overhead", {0x90}, base);
    add("base_nop_loop256", "overhead", loop256({0x90}), base);

    // ---- integer ALU not covered by the base catalog -----------------
    add("alu_add_mem_rmw", "alu", loop256({0x01, 0x07}), base);   // add [edi],eax
    add("alu_div32", "alu",
        loop256({0x31, 0xD2,                          // xor edx,edx
                 0xB8, 0x00, 0x10, 0x00, 0x00,        // mov eax,4096
                 0xF7, 0xF3}), base);                 // div ebx
    add("alu_mul64", "alu",
        loop256({0xBA, 0x03, 0x00, 0x00, 0x00,        // mov edx,3
                 0xF7, 0xE2}), base);                 // mul edx
    add("alu_bswap", "alu", loop256({0x0F, 0xC8}), base);
    add("alu_rol_imm", "alu", loop256({0xC1, 0xC0, 0x05}), base);
    add("alu_shld", "alu", loop256({0x0F, 0xA4, 0xD0, 0x05}), base);
    add("alu_adc_chain", "alu", loop256({0x11, 0xD0, 0x11, 0xD8}), base);
    add("alu_setcc", "alu", loop256({0x39, 0xC0, 0x0F, 0x94, 0xC3}), base);
    add("alu_bt_reg", "alu", loop256({0x0F, 0xA3, 0xD0}), base);
    add("alu_bsf", "alu", loop256({0x0F, 0xBC, 0xC2}), base);
    add("alu_lahf_sahf", "alu", loop256({0x9F, 0x9E}), base);
    add("alu_cmp_jcc_pair", "alu",
        loop256({0x39, 0xD0, 0x0F, 0x94, 0xC3}), base);  // cmp eax,edx; sete bl

    // ---- memory ------------------------------------------------------
    add("mem_load8_zx", "mem", loop256({0x0F, 0xB6, 0x06}), base);
    add("mem_load16_zx", "mem", loop256({0x0F, 0xB7, 0x06}), base);
    add("mem_store8", "mem", loop256({0x88, 0x07}), base);
    add("mem_load32_unaligned", "mem", loop256({0x8B, 0x46, 0x01}), base);
    add("mem_load_chain_dep", "mem", loop256({0x8B, 0x36}), chase);  // mov esi,[esi]
    // ESP comes from the State, not `mov esp, imm`: that immediate form
    // ends the JIT block (status=5) and the row is then unmeasurable.
    // The Windows trampoline keeps its own host ESP either way.
    State stack = scratch_state();
    stack.gpr[4] = kMemVa + 0x100u;
    add("mem_push_pop", "mem", loop256({0x50, 0x58}), stack);
    add("mem_push_pop4", "mem",
        loop256({0x50, 0x52, 0x5A, 0x58}), stack);
    add("mem_rep_movsd16", "mem",
        loop256_ebx({0xBE, 0x40, 0x00, 0x00, 0x04,    // mov esi,0x04000040
                     0xBF, 0x00, 0x01, 0x00, 0x04,    // mov edi,0x04000100
                     0xB9, 0x10, 0x00, 0x00, 0x00,    // mov ecx,16
                     0xF3, 0xA5}), base);             // rep movsd
    add("mem_rep_stosd16", "mem",
        loop256_ebx({0xBF, 0x00, 0x01, 0x00, 0x04,    // mov edi,0x04000100
                     0xB9, 0x10, 0x00, 0x00, 0x00,    // mov ecx,16
                     0xF3, 0xAB}), base);             // rep stosd

    // ---- LOCK-prefixed atomics --------------------------------------
    add("lock_add_mem", "lock", loop256({0xF0, 0x01, 0x07}), base);
    add("lock_inc_mem", "lock", loop256({0xF0, 0xFF, 0x07}), base);
    add("lock_xadd", "lock", loop256({0xF0, 0x0F, 0xC1, 0x07}), base);
    add("lock_xchg", "lock", loop256({0x87, 0x07}), base);
    add("lock_cmpxchg", "lock",
        loop256({0x8B, 0x07, 0xF0, 0x0F, 0xB1, 0x17}), base);
    add("lock_mfence", "lock", loop256({0x0F, 0xAE, 0xF0}), base);

    // ---- SSE beyond the base catalog --------------------------------
    add("sse_mulss", "sse", loop256({0xF3, 0x0F, 0x59, 0xC1}), fp);
    add("sse_divss", "sse", loop256({0xF3, 0x0F, 0x5E, 0xC1}), fp);
    add("sse_sqrtss", "sse", loop256({0xF3, 0x0F, 0x51, 0xC1}), fp);
    add("sse_divps", "sse", loop256({0x0F, 0x5E, 0xC1}), fp);
    add("sse_cvtsi2sd", "sse", loop256({0xF2, 0x0F, 0x2A, 0xC2}), fp);
    add("sse_cvttss2si", "sse", loop256({0xF3, 0x0F, 0x2C, 0xC1}), fp);
    add("sse_movd_to_gpr", "sse", loop256({0x66, 0x0F, 0x7E, 0xC0}), fp);
    add("sse_movd_from_gpr", "sse", loop256({0x66, 0x0F, 0x6E, 0xC2}), fp);
    add("sse_comiss", "sse", loop256({0x0F, 0x2F, 0xC1}), fp);
    add("sse_shufps", "sse", loop256({0x0F, 0xC6, 0xC1, 0x1B}), fp);
    add("sse_movdqu_load_unaligned", "sse",
        loop256({0xF3, 0x0F, 0x6F, 0x46, 0x01}), fp);
    add("sse_movdqu_store_unaligned", "sse",
        loop256({0xF3, 0x0F, 0x7F, 0x47, 0x01}), fp);
    add("sse_pcmpeqb_pmovmskb", "sse",
        loop256({0x66, 0x0F, 0x74, 0xC1, 0x66, 0x0F, 0xD7, 0xC0}), fp);
    add("sse_psadbw", "sse", loop256({0x66, 0x0F, 0xF6, 0xC1}), fp);
    add("sse_movsd_load", "sse", loop256({0xF2, 0x0F, 0x10, 0x06}), fp);
    add("sse_ucomiss", "sse", loop256({0x0F, 0x2E, 0xC1}), fp);
    add("sse_andps", "sse", loop256({0x0F, 0x54, 0xC1}), fp);
    add("sse_xorps", "sse", loop256({0x0F, 0x57, 0xC1}), fp);
    add("sse_subps", "sse", loop256({0x0F, 0x5C, 0xC1}), fp);
    add("sse_minps", "sse", loop256({0x0F, 0x5D, 0xC1}), fp);
    add("sse_addss", "sse", loop256({0xF3, 0x0F, 0x58, 0xC1}), fp);
    add("sse_addpd", "sse", loop256({0x66, 0x0F, 0x58, 0xC1}), fp);
    add("sse_cmpps", "sse", loop256({0x0F, 0xC2, 0xC1, 0x00}), fp);

    // ---- x87 with finite in-memory operands --------------------------
    // Load from [esi] and store to [edi+0x40] -- NOT back over the source.
    // Storing over the input makes the value double every iteration, and
    // once FSTP m32 overflows to +Inf every store is a masked-overflow
    // microcode assist on the Intel oracle (~60-90 ns each). That inflates
    // the Windows column by ~50x and inverts the ratio, which reads like
    // "Vitreo x87 is faster than native" and is purely an operand bug.
    add("x87_fld_fstp_m32", "x87", loop256({0xD9, 0x06, 0xD9, 0x5F, 0x40}), fp);
    add("x87_fld_fstp_m64", "x87", loop256({0xDD, 0x06, 0xDD, 0x5F, 0x40}), fp);
    add("x87_fadd_m32", "x87",
        loop256({0xD9, 0x06, 0xD8, 0x06, 0xD9, 0x5F, 0x40}), fp);
    add("x87_fmul_m32", "x87",
        loop256({0xD9, 0x06, 0xD8, 0x0E, 0xD9, 0x5F, 0x40}), fp);
    add("x87_fdiv_m32", "x87",
        loop256({0xD9, 0x06, 0xD8, 0x36, 0xD9, 0x5F, 0x40}), fp);
    add("x87_fsqrt", "x87",
        loop256({0xD9, 0x06, 0xD9, 0xFA, 0xD9, 0x5F, 0x40}), fp);
    add("x87_fistp_m32", "x87",
        loop256({0xD9, 0xE8, 0xDB, 0x5F, 0x40}), fp);   // fld1; fistp m32
    add("x87_fcom_fnstsw", "x87",
        loop256({0xD9, 0x06, 0xD8, 0x16, 0xDF, 0xE0, 0xDD, 0xD8}), fp);
    add("x87_fadd_st_balanced", "x87",
        loop256({0xD9, 0xE8, 0xD8, 0xC0, 0xDD, 0xD8}), fp);  // fld1;fadd st,st;ffree-pop
    add("x87_fmul_st_balanced", "x87",
        loop256({0xD9, 0xE8, 0xD8, 0xC8, 0xDD, 0xD8}), fp);
    add("x87_fsin", "x87",
        loop256({0xD9, 0x06, 0xD9, 0xFE, 0xD9, 0x5F, 0x40}), fp);

    // ---- control transfer (documents what a one-block harness can and
    //      cannot time; CALL ends the JIT block, so those rows skip) ----
    add("call_direct_ret", "call",
        stack_loop256({0xE8, 0x02, 0x00, 0x00, 0x00,  // call -> the ret at +7
                       0xEB, 0x01,                    // jmp over
                       0xC3}), base);                 // ret
    add("call_indirect_ret", "call",
        stack_loop256({0xE8, 0x00, 0x00, 0x00, 0x00,  // call next (get PC)
                       0x58,                          // pop eax
                       0x83, 0xC0, 0x08,              // add eax,8 -> the ret
                       0xFF, 0xD0,                    // call eax
                       0xEB, 0x01,                    // jmp over
                       0xC3}), base);                 // ret

    return rows;
}

Report bench_class_catalog(uint32_t warmup, uint32_t iters, bool with_windows) {
    Report report;
    for (ClassEntry& entry : cpu_perf_class_catalog()) {
        SnippetBench row;
        row.name = entry.name;
        row.cls = entry.cls;
        if (entry.body.empty()) {
            row.skip = "body build failed";
            ++report.skipped;
            report.rows.push_back(std::move(row));
            continue;
        }
        bench_body(entry.body, warmup, iters, row, entry.in);
        if (with_windows && row.skip.empty()) {
            attach_windows(row, warmup, iters, entry.in);
        }
        if (row.skip.empty()) {
            ++report.comparable;
        } else {
            ++report.skipped;
        }
        report.rows.push_back(std::move(row));
    }
    return report;
}

}  // namespace vitreo::oracle::perf
