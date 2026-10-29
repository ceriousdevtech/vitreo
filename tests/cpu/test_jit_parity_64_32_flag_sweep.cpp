// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): the shift and
// NEG flag sweep.

#include "jit_parity_64_fixture.hpp"

// Run blocks under the interpreter and the JIT and compare them.
//
// Every distinct block runs at its own guest address: recompiling one address
// with new bytes thousands of times trips the JIT's rewrite/recompile limit,
// which then routes that address to the interpreter for the whole process.
class FlagSweep {
public:
    FlagSweep(GuestMemory& memory, JitCompiler& jit) : m_memory(memory), m_jit(jit) {}

    static constexpr GuestAddr kCodeBase = 0x200000;
    static constexpr usize kCodeSize = 0x200000;
    static constexpr GuestAddr kData = 0x8000;
    static constexpr GuestAddr kStackSlot = 0x10000 - 8;

    bool map() {
        return static_cast<bool>(m_memory.map_region(
            kCodeBase, kCodeSize, MemoryProtection::ReadWrite));
    }

    /// Run `code` once under the interpreter and once under the JIT from the
    /// same state; compare every GPR, RIP, the defined flags (AF only when
    /// asked), the data qword at kData and the PUSHFQ stack slot.
    static constexpr u64 kAF = 0x10;
    static constexpr u64 kOF = 0x800;

    /// `ignore` names the RFLAGS bits the SDM leaves undefined for this case
    /// (kAF, kOF); they are compared neither in RFLAGS nor in the PUSHFQ slot.
    void run(const std::vector<u8>& code,
             const std::function<void(CpuState&)>& setup, u64 data,
             u64 ignore) {
        const u64 slot_mask = ~ignore;
        const bool check_af = (ignore & kAF) == 0;
        const bool check_of = (ignore & kOF) == 0;
        ASSERT_LT(m_next + 0x20, kCodeBase + kCodeSize) << "sweep arena full";
        const GuestAddr at = m_next;
        m_next += 0x20;
        ASSERT_TRUE(m_memory.write_bytes(at, code.data(), code.size()));
        BasicBlock block;
        block.start_addr = at;
        block.is_64bit = true;
        MemoryByteReader reader(code.data(), code.size());
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
                      DecodeError::Success);
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());

        const auto start = [&](CpuState& cpu) {
            cpu.init_long_mode();
            cpu.gpr.set64(4, 0x10000);
            setup(cpu);
            cpu.rip = at;
            ASSERT_TRUE(m_memory.write_u64(kData, data));
            ASSERT_TRUE(m_memory.write_u64(kStackSlot, 0));
        };
        const auto read64 = [&](GuestAddr a) {
            u64 v = 0;
            EXPECT_TRUE(m_memory.read_bytes(a, reinterpret_cast<Byte*>(&v),
                                            sizeof(v)));
            return v;
        };

        CpuState icpu;
        start(icpu);
        {
            Interpreter interp(icpu, m_memory);
            (void)interp.run(block.instruction_count);
        }
        icpu.materialize_lazy_flags();
        const u64 idata = read64(kData);
        const u64 islot = read64(kStackSlot);

        CpuState jcpu;
        start(jcpu);
        CompiledBlock* cb = m_jit.compile(block);
        ASSERT_NE(cb, nullptr) << "block did not compile";
        (void)test::execute_like_dispatcher(m_jit, *cb, jcpu);
        jcpu.materialize_lazy_flags();
        m_last_jit = jcpu;

        for (u8 r = 0; r < 16; ++r) {
            EXPECT_EQ(icpu.gpr.get64(r), jcpu.gpr.get64(r)) << "r" << +r;
        }
        EXPECT_EQ(icpu.rip, jcpu.rip);
        EXPECT_EQ(icpu.flags.cf(), jcpu.flags.cf()) << "CF diverged";
        EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf()) << "ZF diverged";
        EXPECT_EQ(icpu.flags.sf(), jcpu.flags.sf()) << "SF diverged";
        if (check_of) {
            EXPECT_EQ(icpu.flags.of(), jcpu.flags.of()) << "OF diverged";
        }
        EXPECT_EQ(icpu.flags.pf(), jcpu.flags.pf()) << "PF diverged";
        if (check_af) {
            EXPECT_EQ(icpu.flags.af(), jcpu.flags.af()) << "AF diverged";
        }
        EXPECT_EQ(idata, read64(kData)) << "data qword diverged";
        EXPECT_EQ(islot & slot_mask, read64(kStackSlot) & slot_mask)
            << "PUSHFQ slot diverged";
    }

    /// The JIT's state after the last run(), flags materialized.
    [[nodiscard]] const CpuState& last_jit() const noexcept { return m_last_jit; }

private:
    GuestMemory& m_memory;
    JitCompiler& m_jit;
    GuestAddr m_next{kCodeBase};
    CpuState m_last_jit{};
};

// Intel SDM Vol. 2 SAL/SAR/SHL/SHR: a zero masked count leaves every flag
// alone; otherwise CF is the last bit shifted out, ZF/SF/PF follow the
// result, OF is defined only for a count of 1 (SHL: MSB(result) XOR CF; SHR:
// MSB(original); SAR: cleared), and AF is undefined. OF above a count of 1 is
// undefined too, so it is not compared there (the hardware differential masks
// shift OF/AF the same way). The lazy encodings differ by width --
// Shl8/16/32 pack the count beside the result, Shl64/Shr64/Sar64 cannot, so
// their publishers write CF themselves and count-1 shifts publish the _1
// records -- so every width, every count class (0, 1, interior, width - 1,
// past the width for the masked narrow forms), the imm8, by-1 and CL forms,
// register and memory destinations, and both seeded flag states run against
// the interpreter. Each shift is followed by nothing (flags materialize at the
// exit), by PUSHFQ (an in-block reader of every flag), and by an ADC that
// consumes CF through the lazy-carry path.
TEST_F(JitParity64Test, ShiftFlagsMatchInterpreterAtEveryWidthAndCount) {
    struct Width { u8 bits; std::vector<u8> prefix; u8 op_imm; u8 op_one; u8 op_cl; };
    const Width widths[] = {
        {8, {}, 0xC0, 0xD0, 0xD2},
        {16, {0x66}, 0xC1, 0xD1, 0xD3},
        {32, {}, 0xC1, 0xD1, 0xD3},
        {64, {0x48}, 0xC1, 0xD1, 0xD3},
    };
    const u8 exts[] = {4, 5, 7};  // SHL, SHR, SAR
    const u8 counts[] = {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63};
    const u64 values[] = {0x8000000180000001ull, 0xFFFFFFFFFFFFFFFFull,
                          0x0123456789ABCDEFull, 0x4000000000000002ull};
    const u64 seeds[] = {0x2u | 0x8D5u, 0x2u};
    const std::vector<u8> adc_cf = {0x48, 0x11, 0xC2};  // adc rdx, rax
    FlagSweep sweep(*m_memory, *m_jit);
    ASSERT_TRUE(sweep.map());
    for (const Width& w : widths) {
        for (const u8 ext : exts) {
            for (const int form : {0, 1, 2}) {  // imm8, by-1, CL
                for (const bool mem : {false, true}) {
                    for (const u8 count : counts) {
                        if (form == 1 && count != 1) continue;
                        const u8 masked = count & (w.bits == 64 ? 63 : 31);
                        // AF is undefined after any nonzero-count shift, OF
                        // after any count above 1; a zero count leaves all.
                        const u64 ignore = masked == 0 ? 0
                            : masked == 1 ? FlagSweep::kAF
                                          : FlagSweep::kAF | FlagSweep::kOF;
                        // pushfq; pop rdx; and edx, <defined flags>
                        const u32 defined = 0x8D5u & ~static_cast<u32>(ignore);
                        const std::vector<u8> read_flags = {
                            0x9C, 0x5A, 0x81, 0xE2,
                            static_cast<u8>(defined), static_cast<u8>(defined >> 8),
                            0x00, 0x00};
                        std::vector<u8> shift = w.prefix;
                        shift.push_back(form == 0 ? w.op_imm
                                        : form == 1 ? w.op_one : w.op_cl);
                        // [rsi] or rbx.
                        shift.push_back(static_cast<u8>(
                            (mem ? 0x00 : 0xC0) | (ext << 3) | (mem ? 6 : 3)));
                        if (form == 0) shift.push_back(count);
                        for (const int follower : {0, 1, 2}) {
                            std::vector<u8> code = shift;
                            if (follower == 1) code.insert(code.end(), read_flags.begin(), read_flags.end());
                            if (follower == 2) code.insert(code.end(), adc_cf.begin(), adc_cf.end());
                            for (const u64 value : values) {
                                for (const u64 seed : seeds) {
                                    SCOPED_TRACE(::testing::Message()
                                        << "width=" << +w.bits << " ext=/" << +ext
                                        << " form=" << form << " mem=" << mem
                                        << " count=" << +count << " follower="
                                        << follower << " value=0x" << std::hex
                                        << value << " seed=0x" << seed);
                                    sweep.run(code,
                                        [&](CpuState& cpu) {
                                            cpu.gpr.rbx = value;
                                            cpu.gpr.rcx = count;
                                            cpu.gpr.rsi = FlagSweep::kData;
                                            cpu.gpr.rax = 0x1111;
                                            cpu.gpr.rdx = 0x2222;
                                            cpu.flags.rflags = seed;
                                        }, value, ignore);
                                    if (HasFatalFailure()) return;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// Intel SDM Vol. 2 SAL/SAR/SHL/SHR: "For the SAR instruction, the OF flag is
// cleared for all 1-bit shifts." The r/m64, CL form published FlagOp::Sar64
// for every nonzero count, and that record carries no count, so it cannot
// define OF: `sar r64, cl` with CL & 63 == 1 left an incoming OF set. The
// SHL/SHR CL forms already switch to their _1 record at a count of 1.
TEST_F(JitParity64Test, SarR64ByClOfOneClearsOverflow) {
    FlagSweep sweep(*m_memory, *m_jit);
    ASSERT_TRUE(sweep.map());
    for (const bool mem : {false, true}) {
        const std::vector<u8> code = mem
            ? std::vector<u8>{0x48, 0xD3, 0x3E}   // sar qword [rsi], cl
            : std::vector<u8>{0x48, 0xD3, 0xFB};  // sar rbx, cl
        for (const u64 cl : {u64{1}, u64{65}}) {  // 65 masks to 1
            for (const u64 value : {0x8000000000000001ull, 0x2ull,
                                    0x7FFFFFFFFFFFFFFFull}) {
                SCOPED_TRACE(::testing::Message() << "mem=" << mem << " cl="
                             << cl << " value=0x" << std::hex << value);
                sweep.run(code, [&](CpuState& cpu) {
                    cpu.gpr.rbx = value;
                    cpu.gpr.rcx = cl;
                    cpu.gpr.rsi = FlagSweep::kData;
                    cpu.flags.rflags = 0x2u | 0x8D5u;  // OF set on entry
                }, value, FlagSweep::kAF);
                if (HasFatalFailure()) return;
                EXPECT_FALSE(sweep.last_jit().flags.of())
                    << "a 1-bit SAR must clear OF";
            }
        }
    }
}

// Intel SDM Vol. 2 NEG: CF = 0 when the source is 0, else 1; OF, SF, ZF, AF
// and PF are set according to the result (0 - source). Every width, register
// and memory, against the interpreter with AF compared, both at the exit and
// through an in-block PUSHFQ, and through an ADC that consumes CF.
TEST_F(JitParity64Test, NegFlagsMatchInterpreterAtEveryWidth) {
    struct Width { u8 bits; std::vector<u8> prefix; u8 op; };
    const Width widths[] = {
        {8, {}, 0xF6}, {16, {0x66}, 0xF7}, {32, {}, 0xF7}, {64, {0x48}, 0xF7},
    };
    const u64 values[] = {0, 1, 0x80, 0x8000, 0x80000000ull,
                          0x8000000000000000ull, 0x10, 0xFFFFFFFFFFFFFFFFull,
                          0x0123456789ABCDEFull};
    const u64 seeds[] = {0x2u | 0x8D5u, 0x2u};
    // pushfq; pop rdx; and edx, 0x8D5 (CF PF AF ZF SF OF)
    const std::vector<u8> read_flags = {0x9C, 0x5A, 0x81, 0xE2, 0xD5, 0x08, 0x00, 0x00};
    const std::vector<u8> adc_cf = {0x48, 0x11, 0xC2};  // adc rdx, rax
    FlagSweep sweep(*m_memory, *m_jit);
    ASSERT_TRUE(sweep.map());
    for (const Width& w : widths) {
        for (const bool mem : {false, true}) {
            std::vector<u8> neg = w.prefix;
            neg.push_back(w.op);
            neg.push_back(mem ? 0x1E : 0xDB);  // /3 [rsi] or rbx
            for (const int follower : {0, 1, 2}) {
                std::vector<u8> code = neg;
                if (follower == 1) code.insert(code.end(), read_flags.begin(), read_flags.end());
                if (follower == 2) code.insert(code.end(), adc_cf.begin(), adc_cf.end());
                for (const u64 value : values) {
                    for (const u64 seed : seeds) {
                        SCOPED_TRACE(::testing::Message()
                            << "width=" << +w.bits << " mem=" << mem
                            << " follower=" << follower << " value=0x"
                            << std::hex << value << " seed=0x" << seed);
                        sweep.run(code,
                            [&](CpuState& cpu) {
                                cpu.gpr.rbx = value;
                                cpu.gpr.rsi = FlagSweep::kData;
                                cpu.gpr.rax = 0x1111;
                                cpu.gpr.rdx = 0x2222;
                                cpu.flags.rflags = seed;
                            }, value, /*ignore=*/0);
                        if (HasFatalFailure()) return;
                    }
                }
            }
        }
    }
}
