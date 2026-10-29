// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): SSE 128-bit
// memory operand alignment.

#include "jit_parity_64_fixture.hpp"

// ---------------------------------------------------------------------------
// Legacy SSE: a 128-bit MEMORY operand must be 16-byte aligned
// ---------------------------------------------------------------------------
//
// The rule is a property of the OPERAND, not the opcode: measured on silicon,
// every legacy 128-bit memory operand raises #GP when misaligned and every
// narrower one does not (tools/oracle_probe/sse_align_probe.c, recorded output
// beside it). The explicitly unaligned spellings -- MOVUPS, MOVUPD, MOVDQU,
// LDDQU -- are the exceptions, and VEX encodings carry no such rule at all.
//
// This sweeps the legacy m128 families through BOTH engines with the operand
// misaligned by one and requires them to agree. It is a gate rather than a
// one-off audit: a new SSE lowering that reaches memory by some path without
// the check shows up here instead of in a guest.
// A VEX-encoded 128-bit memory operand has NO alignment requirement (Intel SDM
// Vol. 2: VEX forms are unaligned unless the opcode says otherwise). Raising
// #GP on one is not a missing feature, it is a fabricated fault in the middle
// of code the guest expects to run.
//
// This is a REGRESSION test for a real, deterministic Steam startup kill. The
// long-mode XORPS/XORPD/PXOR and PSHUFD lowerings in ops64.cpp gained an
// alignment check guarded only on `instr.prefixes.vex`. That field is already
// cleared by compile_sse's VEX.128 bridge, which re-presents a legacy-shaped
// instruction to those same lowerings, so every bridged VEX form took the
// legacy check. `VPXOR xmm2, xmm1, [rdi]` at steam.exe+0x16ddc, inside Steam's
// AES-NI/PCLMULQDQ startup path with rdi 8-byte aligned, raised 0xC0000005 and
// Steam died before spawning its UI. The correct predicate is
// `!instr.prefixes.vex && !m_current_original_vex`, which is what every
// equivalent site in sse.cpp already used.
TEST_F(JitParity64Test, VexM128MemoryOperandsNeverRaiseOnMisalignment) {
    constexpr GuestAddr kBase = 0x14000C000ull;   // 16-byte aligned
    ASSERT_TRUE(m_memory->map_region(kBase, 0x1000,
                                     MemoryProtection::ReadWrite));
    struct Form { const char* name; std::vector<u8> bytes; };
    // Each encodes <op> xmm2, xmm1, [rax+8] -- deliberately 8-byte aligned and
    // therefore NOT 16-byte aligned, which is the case that separates a
    // correct VEX lowering from one taking the legacy path.
    const std::vector<Form> kForms = {
        {"vpxor",   {0xC5, 0xF1, 0xEF, 0x50, 0x08}},
        {"vxorps",  {0xC5, 0xF0, 0x57, 0x50, 0x08}},
        {"vxorpd",  {0xC5, 0xF1, 0x57, 0x50, 0x08}},
        {"vpand",   {0xC5, 0xF1, 0xDB, 0x50, 0x08}},
        {"vpor",    {0xC5, 0xF1, 0xEB, 0x50, 0x08}},
        {"vandps",  {0xC5, 0xF0, 0x54, 0x50, 0x08}},
        {"vorps",   {0xC5, 0xF0, 0x56, 0x50, 0x08}},
        // VPSHUFD is the second site that had the same weak guard.
        {"vpshufd", {0xC5, 0xF9, 0x70, 0x50, 0x08, 0x1B}},
    };
    u64 code_at = 0x2000;
    std::vector<std::string> raised, skipped;
    for (const Form& form : kForms) {
        SCOPED_TRACE(testing::Message() << form.name);
        const u64 entry = code_at;
        code_at += 64;
        ASSERT_TRUE(m_memory->write_bytes(entry, form.bytes.data(),
                                          form.bytes.size()));
        BasicBlock block = build_block(form.bytes.data(), form.bytes.size());
        block.start_addr = entry;
        if (block.instruction_count != 1) { skipped.push_back(form.name); continue; }
        CompiledBlock* compiled = m_jit->compile(block);
        if (compiled == nullptr) { skipped.push_back(form.name); continue; }

        CpuState cpu;
        init_cpu(cpu);
        cpu.rip = entry;
        cpu.gpr.set64(RAX, kBase);          // +8 displacement -> misaligned
        for (u8 x = 0; x < 16; ++x) {
            cpu.sse.xmm[x].u64_[0] = 0x0102030405060708ull * (x + 1);
            cpu.sse.xmm[x].u64_[1] = 0x1122334455667788ull ^ (0x11ull * x);
        }
        for (u64 off = 0; off < 0x40; off += 8)
            ASSERT_TRUE(m_memory->write_u64(kBase + off, 0xFEED0000 + off));
        const JitExecResult r = test::execute_like_dispatcher(*m_jit, *compiled, cpu);
        if (r == JitExecResult::Unsupported) { skipped.push_back(form.name); continue; }
        if (cpu.pending_raise.active) raised.push_back(form.name);
    }
    std::string names;
    for (const std::string& s : raised) names += "\n  " + s;
    EXPECT_TRUE(raised.empty())
        << raised.size() << " VEX form(s) raised on a misaligned 16-byte "
        << "operand that VEX permits to be unaligned:" << names;
    std::printf("[vex-align] checked %zu form(s), %zu raised, %zu skipped\n",
                kForms.size(), raised.size(), skipped.size());
}

TEST_F(JitParity64Test, LegacySseM128MemoryOperandsAgreeOnAlignmentFault) {
    constexpr GuestAddr kBase = 0x14000C000ull;   // 16-byte aligned
    ASSERT_TRUE(m_memory->map_region(kBase, 0x1000,
                                     MemoryProtection::ReadWrite));

    struct Form {
        const char* name;
        std::vector<u8> prefix;   // 66 / F2 / F3, or none
        std::vector<u8> op;       // opcode bytes after the prefix
        bool has_imm8;
        bool exempt;              // an explicitly UNALIGNED spelling
    };
    // Forms whose behaviour is RECORDED in
    // tools/oracle_probe/sse_align_probe.expected. Only these get the silicon
    // rule asserted; for the rest this test checks that the two engines agree
    // with each other and reports them as unverified. Asserting a rule on a
    // form nobody measured would be asserting a belief.
    const std::set<std::string> kMeasured = {
        "xorps", "pxor", "paddb", "pmullw", "pcmpeqb", "packsswb", "pandn",
        "movups", "movdqu",
    };
    const std::vector<Form> kForms = {
        // Packed float arithmetic
        {"addps",   {},      {0x0F, 0x58}, false, false},
        {"subps",   {},      {0x0F, 0x5C}, false, false},
        {"mulps",   {},      {0x0F, 0x59}, false, false},
        {"divps",   {},      {0x0F, 0x5E}, false, false},
        {"minps",   {},      {0x0F, 0x5D}, false, false},
        {"maxps",   {},      {0x0F, 0x5F}, false, false},
        {"sqrtps",  {},      {0x0F, 0x51}, false, false},
        {"addpd",   {0x66}, {0x0F, 0x58}, false, false},
        {"subpd",   {0x66}, {0x0F, 0x5C}, false, false},
        {"mulpd",   {0x66}, {0x0F, 0x59}, false, false},
        {"divpd",   {0x66}, {0x0F, 0x5E}, false, false},
        {"minpd",   {0x66}, {0x0F, 0x5D}, false, false},
        {"maxpd",   {0x66}, {0x0F, 0x5F}, false, false},
        {"sqrtpd",  {0x66}, {0x0F, 0x51}, false, false},
        // Compare and shuffle (imm8)
        {"cmpps",   {},      {0x0F, 0xC2}, true,  false},
        {"cmppd",   {0x66}, {0x0F, 0xC2}, true,  false},
        {"shufps",  {},      {0x0F, 0xC6}, true,  false},
        {"shufpd",  {0x66}, {0x0F, 0xC6}, true,  false},
        {"pshufd",  {0x66}, {0x0F, 0x70}, true,  false},
        // Unpack
        {"unpcklps",{},      {0x0F, 0x14}, false, false},
        {"unpckhps",{},      {0x0F, 0x15}, false, false},
        {"unpcklpd",{0x66}, {0x0F, 0x14}, false, false},
        {"punpcklbw",{0x66},{0x0F, 0x60}, false, false},
        {"punpcklqdq",{0x66},{0x0F,0x6C}, false, false},
        // Converts with a 128-bit source
        {"cvtdq2ps",{},      {0x0F, 0x5B}, false, false},
        {"cvtps2dq",{0x66}, {0x0F, 0x5B}, false, false},
        {"cvttps2dq",{0xF3},{0x0F, 0x5B}, false, false},
        {"cvtpd2ps",{0x66}, {0x0F, 0x5A}, false, false},
        // Integer arithmetic and compare
        {"paddb",   {0x66}, {0x0F, 0xFC}, false, false},
        {"psubw",   {0x66}, {0x0F, 0xF9}, false, false},
        {"pmullw",  {0x66}, {0x0F, 0xD5}, false, false},
        {"pcmpeqb", {0x66}, {0x0F, 0x74}, false, false},
        {"packsswb",{0x66}, {0x0F, 0x63}, false, false},
        {"pandn",   {0x66}, {0x0F, 0xDF}, false, false},
        // Bitwise (already covered elsewhere; kept as in-family controls)
        {"xorps",   {},      {0x0F, 0x57}, false, false},
        {"pxor",    {0x66}, {0x0F, 0xEF}, false, false},
        // Non-temporal STORES -- the memory operand is the destination
        {"movntps", {},      {0x0F, 0x2B}, false, false},
        {"movntdq", {0x66}, {0x0F, 0xE7}, false, false},
        // The explicitly unaligned spellings: these must NOT raise
        {"movups",  {},      {0x0F, 0x10}, false, true},
        {"movupd",  {0x66}, {0x0F, 0x10}, false, true},
        {"movdqu",  {0xF3}, {0x0F, 0x6F}, false, true},
        {"lddqu",   {0xF2}, {0x0F, 0xF0}, false, true},
    };

    // Each form gets its OWN guest code address. Reusing one address makes
    // every iteration overwrite a block the compiler has already cached, which
    // is self-modifying code as far as the JIT is concerned -- it declines
    // rather than recompiling, and the sweep then silently measures nothing.
    u64 code_at = 0x2000;
    unsigned raised = 0;
    unsigned completed = 0;
    unsigned skipped = 0;
    std::vector<std::string> disagreed;
    std::vector<std::string> undecoded;
    std::vector<std::string> declined;
    std::vector<std::string> missing_rule;
    std::vector<std::string> unsupported;
    std::vector<std::string> unverified;
    for (const Form& form : kForms) {
        std::vector<u8> code = form.prefix;
        code.insert(code.end(), form.op.begin(), form.op.end());
        code.push_back(0x98);                     // xmm3, [rax+disp32]
        for (unsigned shift = 0; shift < 32; shift += 8)
            code.push_back(static_cast<u8>(1u >> shift));   // disp32 = 1
        if (form.has_imm8) code.push_back(0x00);
        SCOPED_TRACE(testing::Message() << form.name);

        const u64 entry = code_at;
        code_at += 64;
        ASSERT_TRUE(m_memory->write_bytes(entry, code.data(), code.size()));
        BasicBlock block = build_block(code.data(), code.size());
        block.start_addr = entry;
        if (block.instruction_count != 1 ||
            block.byte_length != code.size()) {
            undecoded.push_back(form.name);
            ++skipped;
            continue;
        }
        CompiledBlock* compiled = m_jit->compile(block);
        if (compiled == nullptr) {
            declined.push_back(form.name);
            ++skipped;
            continue;
        }

        const auto seed_state = [&](CpuState& cpu) {
            init_cpu(cpu);
            cpu.rip = entry;
            cpu.gpr.set64(RAX, kBase);
            for (u8 x = 0; x < 16; ++x) {
                cpu.sse.xmm[x].u64_[0] = 0x0102030405060708ull * (x + 1);
                cpu.sse.xmm[x].u64_[1] = 0x1122334455667788ull ^ (0x11ull * x);
            }
        };
        const auto seed_memory = [&]() {
            for (u64 off = 0; off < 0x80; off += 8)
                ASSERT_TRUE(m_memory->write_u64(kBase + off, 0xFEED0000 + off));
        };

        CpuState icpu;
        seed_state(icpu);
        seed_memory();
        Interpreter interp(icpu, *m_memory);
        const ExecResult iref = interp.step();
        std::vector<u64> imem;
        for (u64 off = 0; off < 0x80; off += 8)
            imem.push_back(m_memory->read_u64(kBase + off).value_or(0));

        CpuState jcpu;
        seed_state(jcpu);
        seed_memory();
        // Was the alignment check even EMITTED? Distinguishes "this lowering
        // has no check" from "the check is there but never raises".
        const u32 align_probe = arm64::ubfx_x(8, REG_CALL_TMP, 0, 4);
        const auto* words = static_cast<const u32*>(compiled->code);
        const bool has_check =
            std::find(words, words + compiled->code_size / sizeof(u32),
                      align_probe) != words + compiled->code_size / sizeof(u32);
        const JitExecResult jres = test::execute_like_dispatcher(*m_jit, *compiled, jcpu);
        if (jres == JitExecResult::Unsupported) {
            // The block compiled, but this instruction's lowering bailed at
            // run time and handed the guest back to the interpreter. Nothing
            // was executed natively, so there is no second engine to compare.
            unsupported.push_back(form.name);
            ++skipped;
            continue;
        }

        const bool iraised = iref != ExecResult::Continue;
        const bool jraised = jcpu.pending_raise.active;
        // What the GUEST sees, which is the only thing that has to agree.
        //
        // The two engines reach it differently and that is worth pinning. The
        // JIT's `jit_require_align16` sets STATUS_ACCESS_VIOLATION directly.
        // The interpreter's readers set CpuException::GeneralProtection, and
        // then every caller overwrites it with SimdFloatingPoint on its way
        // out -- so the interpreter believes it raised #XM. It still reports
        // 0xC0000005 only because `cpu_exception_to_status` has no arm for
        // SimdFloatingPoint and falls through its `default:`.
        //
        // That is an accident, not a contract, and it is a live landmine:
        // adding the CORRECT mapping (#XM -> STATUS_FLOAT_MULTIPLE_TRAPS)
        // would silently change every misaligned legacy SSE operand into the
        // wrong exception for the guest. This assertion is what makes that
        // change fail loudly instead. It cannot be fixed by having the caller
        // preserve an already-set exception, because `m_ctx.exception` is not
        // cleared between instructions -- a stale value would then suppress a
        // real #XM.
        if (iraised && jraised) {
            EXPECT_EQ(cpu_exception_to_status(interp.context().exception),
                      jcpu.pending_raise.code)
                << form.name << ": engines disagree on the exception the guest "
                << "observes for a misaligned 16-byte operand";
        }
        if (iraised != jraised) {
            disagreed.push_back(std::string(form.name) +
                                (iraised ? " interp-only" : " jit-only") +
                                (has_check ? "(check-emitted)" : "(no-check)"));
            continue;
        }
        if (iraised) ++raised; else ++completed;
        if (jraised == !form.exempt) continue;   // matches the measured rule
        if (kMeasured.count(form.name) != 0) missing_rule.push_back(form.name);
        else unverified.push_back(form.name);
        for (u8 x = 0; x < 16; ++x) {
            EXPECT_EQ(jcpu.sse.xmm[x].u64_[0], icpu.sse.xmm[x].u64_[0])
                << form.name << " xmm" << unsigned(x) << " low";
            EXPECT_EQ(jcpu.sse.xmm[x].u64_[1], icpu.sse.xmm[x].u64_[1])
                << form.name << " xmm" << unsigned(x) << " high";
        }
        for (usize i = 0; i < imem.size(); ++i)
            EXPECT_EQ(m_memory->read_u64(kBase + i * 8).value_or(1), imem[i])
                << form.name << " memory+" << (i * 8);
    }
    std::string joined;
    for (const auto& d : disagreed) { joined += d; joined += " "; }
    EXPECT_TRUE(disagreed.empty())
        << "engines disagree on the alignment fault for: " << joined;
    // Guard against a vacuous pass in either direction.
    EXPECT_GT(raised, 20u) << "the #GP path must actually be taken";
    EXPECT_GT(completed, 0u) << "the exempt spellings must still complete";
    std::string miss, unv;
    for (const auto& n : missing_rule) { miss += n; miss += " "; }
    for (const auto& n : unverified)   { unv  += n; unv  += " "; }
    EXPECT_TRUE(missing_rule.empty())
        << "both engines agree, but disagree with the RECORDED silicon "
           "measurement (tools/oracle_probe/sse_align_probe.expected) for: "
        << miss;
    // Not a failure: these forms are unchecked in both engines and the rule
    // has not been measured for them. Every one of the twenty-four m128 forms
    // that HAS been measured faults, so they very likely should too -- extend
    // the probe's TRY list and run it on the oracle to settle them.
    if (!unverified.empty())
        std::fprintf(stderr,
                     "[sse-align] UNVERIFIED (unchecked in both engines, not "
                     "measured): %s\n", unv.c_str());
    std::string und, dec, uns;
    for (const auto& n : undecoded)   { und += n; und += " "; }
    for (const auto& n : declined)    { dec += n; dec += " "; }
    for (const auto& n : unsupported) { uns += n; uns += " "; }
    std::fprintf(stderr,
                 "[sse-align] raised=%u completed=%u skipped=%u\n"
                 "[sse-align]   undecoded: %s\n"
                 "[sse-align]   tier0-declined: %s\n"
                 "[sse-align]   tier0-unsupported-at-runtime: %s\n",
                 raised, completed, skipped, und.c_str(), dec.c_str(),
                 uns.c_str());
}

// Legacy packed forms whose memory operand is NARROWER than 16 bytes carry no
// alignment rule and read only their own width (Intel SDM Vol. 2: CVTDQ2PD,
// CVTPS2PD and MOVDDUP take xmm2/m64; PMOVSXBW m64; PMOVZXBD and INSERTPS
// m32). Measured on silicon: all `ok` misaligned in
// tools/oracle_probe/sse_align_probe.expected, while CVTPD2DQ/CVTTPD2DQ, which
// share opcode 0F E6 with CVTDQ2PD, fault.
//
// Regression: CVTDQ2PD was decoded as m128, so both engines raised #GP on
// `cvtdq2pd xmm0, [rdi+0x24]` and VS Code (Electron) died at startup. Each
// operand here ends exactly at the end of a mapped page with nothing mapped
// after it, so a 16-byte read would also fault.
TEST_F(JitParity64Test, LegacySseNarrowMemoryOperandsNeverRaiseOnMisalignment) {
    constexpr GuestAddr kBase = 0x14000C000ull;
    ASSERT_TRUE(m_memory->map_region(kBase, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->read_u64(kBase + 0x1000).is_err())
        << "the page after the operand must be unmapped";
    struct Form {
        const char* name;
        std::vector<u8> op;   // prefix + opcode bytes, before ModRM
        u32 width;            // memory operand size in bytes
        bool has_imm8;
    };
    const std::vector<Form> kForms = {
        {"cvtdq2pd", {0xF3, 0x0F, 0xE6},       8, false},
        {"cvtps2pd", {0x0F, 0x5A},             8, false},
        {"movddup",  {0xF2, 0x0F, 0x12},       8, false},
        {"pmovsxbw", {0x66, 0x0F, 0x38, 0x20}, 8, false},
        {"pmovzxbd", {0x66, 0x0F, 0x38, 0x31}, 4, false},
        {"insertps", {0x66, 0x0F, 0x3A, 0x21}, 4, true},
    };
    u64 code_at = 0x2000;
    for (const Form& form : kForms) {
        SCOPED_TRACE(testing::Message() << form.name);
        const u32 disp = 0x1000 - form.width;   // ends at the page end; not
        ASSERT_NE(disp & 15u, 0u);              // 16-byte aligned
        std::vector<u8> code = form.op;
        code.push_back(0x98);                   // xmm3, [rax+disp32]
        for (unsigned shift = 0; shift < 32; shift += 8)
            code.push_back(static_cast<u8>(disp >> shift));
        if (form.has_imm8) code.push_back(0x10);
        const u64 entry = code_at;
        code_at += 64;
        ASSERT_TRUE(m_memory->write_bytes(entry, code.data(), code.size()));

        const auto seed = [&](CpuState& cpu) {
            init_cpu(cpu);
            cpu.rip = entry;
            cpu.gpr.set64(RAX, kBase);
            for (u8 x = 0; x < 16; ++x) {
                cpu.sse.xmm[x].u64_[0] = 0x0102030405060708ull * (x + 1);
                cpu.sse.xmm[x].u64_[1] = 0x1122334455667788ull ^ (0x11ull * x);
            }
            // i32 {-7, 1000} for cvtdq2pd; any bytes for the rest.
            ASSERT_TRUE(m_memory->write_u64(kBase + 0xFF8, 0x000003E8FFFFFFF9ull));
        };

        CpuState icpu;
        seed(icpu);
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.step(), ExecResult::Continue)
            << "interpreter raised on a misaligned narrow operand";

        BasicBlock block = build_block(code.data(), code.size());
        block.start_addr = entry;
        ASSERT_EQ(block.instruction_count, 1u);
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        CpuState jcpu;
        seed(jcpu);
        const JitExecResult jres = test::execute_like_dispatcher(*m_jit, *compiled, jcpu);
        if (jres == JitExecResult::Unsupported) continue;   // interpreter ran it
        EXPECT_FALSE(jcpu.pending_raise.active)
            << "JIT raised on a misaligned narrow operand";
        for (u8 x = 0; x < 16; ++x) {
            EXPECT_EQ(jcpu.sse.xmm[x].u64_[0], icpu.sse.xmm[x].u64_[0]) << "xmm" << unsigned(x);
            EXPECT_EQ(jcpu.sse.xmm[x].u64_[1], icpu.sse.xmm[x].u64_[1]) << "xmm" << unsigned(x);
        }
        if (std::string(form.name) == "cvtdq2pd") {
            EXPECT_DOUBLE_EQ(icpu.sse.xmm[3].f64_[0], -7.0);
            EXPECT_DOUBLE_EQ(icpu.sse.xmm[3].f64_[1], 1000.0);
        }
    }
}

TEST(SseNarrowOperandDecode, Cvtdq2pdMemoryOperandIs64Bits) {
    // F3 0F E6 00 -- cvtdq2pd xmm0, [rax]; Intel SDM Vol. 2: xmm2/m64.
    const u8 bytes[] = {0xF3, 0x0F, 0xE6, 0x00};
    MemoryByteReader reader(bytes, sizeof(bytes));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst), DecodeError::Success);
    EXPECT_EQ(inst.opcode, Opcode::CVTDQ2PD_XMM_XMMM64);
    EXPECT_EQ(inst.operands[1].type, OperandType::Mem64);
    // The 0F E6 siblings keep their full 128-bit operand.
    const u8 cvtpd2dq[] = {0xF2, 0x0F, 0xE6, 0x00};
    MemoryByteReader reader2(cvtpd2dq, sizeof(cvtpd2dq));
    ASSERT_EQ(decode_instruction(reader2, DecodeMode::Mode64, inst), DecodeError::Success);
    EXPECT_EQ(inst.operands[1].type, OperandType::Mem128);
}

// VEX.256 forms whose imm8 addresses elements across BOTH 128-bit lanes. Both
// engines execute a VEX.256 legacy-form op as two 128-bit halves; the upper
// half must take the upper selector bits, not repeat the lower ones (Intel
// SDM Vol. 2 BLENDPS/BLENDPD/SHUFPD/MPSADBW, measured on silicon by
// tools/oracle_probe/avx_vlmax_upper_probe and avx2_lane_imm_probe). Both
// engines repeated imm8[3:0] for VBLENDPS's upper lane, so parity alone could
// never see it: this test also checks each engine against the SDM result.
// dst = ymm0, src1 (vvvv) = ymm2, src2 (r/m) = ymm1, every imm8.
TEST_F(JitParity64Test, Vex256CrossLaneImmediatesSelectUpperLaneFromUpperBits) {
    struct Form {
        const char* name;
        u8 map_byte;  // C4 byte 1: R/X/B set, mmmmm
        u8 opcode;
    };
    const Form forms[] = {
        {"vblendps", 0xE3, 0x0C},
        {"vblendpd", 0xE3, 0x0D},
        {"vshufpd", 0xE1, 0xC6},
        {"vmpsadbw", 0xE3, 0x42},
    };
    auto seed = [](CpuState& cpu) {
        for (u8 i = 0; i < 16; ++i) {
            cpu.sse.xmm[1].u8_[i] = static_cast<u8>(0x11 * (i + 1));
            cpu.avx.ymm_high[1].u8_[i] = static_cast<u8>(0xF0 - 7 * i);
            cpu.sse.xmm[2].u8_[i] = static_cast<u8>(3 + 29 * i);
            cpu.avx.ymm_high[2].u8_[i] = static_cast<u8>(0x80 ^ (13 * i));
            cpu.sse.xmm[0].u8_[i] = 0xCC;
            cpu.avx.ymm_high[0].u8_[i] = 0xCC;
        }
    };
    CpuState ref_in;
    seed(ref_in);
    auto byte_of = [](const CpuState& c, u8 reg, u32 i) -> u8 {
        return i < 16 ? c.sse.xmm[reg].u8_[i] : c.avx.ymm_high[reg].u8_[i - 16];
    };
    auto expected = [&](u8 op, u8 imm) {
        std::array<u8, 32> out{};
        auto put32 = [&](u32 e, u8 reg, u32 src_e) {
            for (u32 b = 0; b < 4; ++b) out[e * 4 + b] = byte_of(ref_in, reg, src_e * 4 + b);
        };
        auto put64 = [&](u32 e, u8 reg, u32 src_e) {
            for (u32 b = 0; b < 8; ++b) out[e * 8 + b] = byte_of(ref_in, reg, src_e * 8 + b);
        };
        switch (op) {
        case 0x0C:
            for (u32 e = 0; e < 8; ++e) put32(e, (imm >> e) & 1 ? 1 : 2, e);
            break;
        case 0x0D:
            for (u32 e = 0; e < 4; ++e) put64(e, (imm >> e) & 1 ? 1 : 2, e);
            break;
        case 0xC6:
            // Even elements from SRC1, odd from SRC2, each within its lane.
            for (u32 e = 0; e < 4; ++e) {
                const u32 lane_base = (e / 2) * 2;
                put64(e, (e & 1) ? 1 : 2, lane_base + ((imm >> e) & 1));
            }
            break;
        case 0x42:
            for (u32 lane = 0; lane < 2; ++lane) {
                const u8 ctl = lane == 0 ? (imm & 7) : ((imm >> 3) & 7);
                const u32 s1 = lane * 16 + ((ctl >> 2) & 1) * 4;
                const u32 s2 = lane * 16 + (ctl & 3) * 4;
                for (u32 j = 0; j < 8; ++j) {
                    u32 sum = 0;
                    for (u32 k = 0; k < 4; ++k) {
                        const int a = byte_of(ref_in, 2, s1 + j + k);
                        const int b = byte_of(ref_in, 1, s2 + k);
                        sum += static_cast<u32>(a > b ? a - b : b - a);
                    }
                    out[lane * 16 + j * 2] = static_cast<u8>(sum);
                    out[lane * 16 + j * 2 + 1] = static_cast<u8>(sum >> 8);
                }
            }
            break;
        }
        return out;
    };
    for (const Form& f : forms) {
        for (u32 imm = 0; imm < 256; ++imm) {
            SCOPED_TRACE(::testing::Message() << f.name << " imm=0x" << std::hex << imm);
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            // These forms may run natively (lane split) or through the JIT's
            // delegate; either way the JIT path is what is measured here.
            check_parity({0xC4, f.map_byte, 0x6D, f.opcode, 0xC1,
                          static_cast<u8>(imm)},
                         seed, /*check_flags=*/false, /*check_xmm=*/true,
                         /*require_jit=*/true);
            if (!m_last_parity_ok) continue;
            const auto want = expected(f.opcode, static_cast<u8>(imm));
            for (u32 i = 0; i < 32; ++i) {
                ASSERT_EQ(byte_of(m_last_interp_cpu, 0, i), want[i]) << "interp byte " << i;
                ASSERT_EQ(byte_of(m_last_jit_cpu, 0, i), want[i]) << "jit byte " << i;
            }
        }
    }
}
