// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo oracle client — spawns the x86 oracle runner and exchanges CPU state
// over stdin/stdout. See tests/oracle/x86_oracle_runner.c for wire protocol.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vitreo::oracle {

// Bitness selector carried in State::mode. The runner builds a different
// prologue/epilogue per mode; the Vitreo-side contract adapters pick
// protected vs long mode from the same field.
inline constexpr uint32_t kMode32 = 32u;
inline constexpr uint32_t kMode64 = 64u;

#pragma pack(push, 1)
struct State {
    // GPRs in Intel order, full width:
    //   RAX,RCX,RDX,RBX,RSP,RBP,RSI,RDI,R8..R15
    // In kMode32 only indices 0..7 are meaningful and only their low 32 bits
    // are defined; the runner zeroes the upper halves and R8-R15 on return so
    // a 32-bit state is always a clean zero-extension of the 8-register view.
    // NB: RSP (index 4) is loaded only when the incoming value is non-zero;
    // otherwise the runner keeps the host stack (legacy snippets). Live-stack
    // tests point RSP into the scratch page at kMemVa.
    std::array<uint64_t, 16> gpr{};
    // Architectural flags. Bits 32..63 of RFLAGS are reserved and read as
    // zero, so in kMode32 this is exactly the old EFLAGS value.
    uint64_t rflags = 0x202;   // reserved bit 1 + IF; safe default
    // kMode64: requested (in) / actual (out) VA of the first snippet byte, so
    // RIP-relative operands resolve identically on both sides. 0 on input
    // means "use kCodeVa64". Unused in kMode32.
    uint64_t rip = 0;
    uint32_t mxcsr  = 0x1F80;  // Intel default
    uint32_t mode   = kMode32; // kMode32 or kMode64
    // XMM0..XMM15. In kMode32 only 0..7 are meaningful; 8..15 are zeroed on
    // return by the runner.
    std::array<std::array<uint8_t, 16>, 16> xmm{};
    // First 512 bytes of the per-call scratch memory page at kMemVa (enough
    // for a 16-byte-aligned FXSAVE image). Round-trips through the oracle:
    // snapshot pre-snippet -> observed bytes post-snippet.
    std::array<uint8_t, 512> mem{};
    uint32_t mem_va = 0;       // set by oracle to kMemVa on return
    uint32_t status = 0;       // 0=ok, 1=SEGV, 2=ILL, 3=FPE, 4=BUS, 5=other, 6=setup
    uint32_t bytes_run = 0;
    uint32_t reserved = 0;     // padding; keeps the wire struct 8-byte sized

    // ---- x87 / MMX register file -------------------------------------------
    // Without these the harness cannot validate ANY instruction that reads an
    // x87 or MMX register as input: the two sides simply start from different
    // (unshared) FPU state and every such comparison is meaningless. That is
    // what made PMOVMSKB/PEXTRW/MOVQ2DQ report bogus "divergences".
    //
    // Layout is exactly the FXSAVE legacy image's, so both sides can marshal
    // through one shared routine rather than re-deriving the mapping:
    //   - st[] is TOP-RELATIVE (ST(0)..ST(7)), like FXSAVE stores it, not the
    //     physical R0..R7 order CpuState keeps internally.
    //   - ftw is the ABRIDGED 8-bit tag, like FXSAVE, not the full 16-bit FTW.
    // MMx aliases the low 8 bytes of st[x], so this covers MMX too.
    uint16_t fcw = 0x037F;     // x87 control word (FNINIT default)
    uint16_t fsw = 0;          // x87 status word, incl. TOP in bits 11..13
    uint8_t  ftw = 0;          // abridged tag byte (bit i set = ST(i) valid)
    uint8_t  fx_pad[3]{};      // keep st[] 8-byte aligned in the wire struct
    std::array<std::array<uint8_t, 10>, 8> st{};  // ST(0)..ST(7) / MM0..MM7

    bool is64() const { return mode == kMode64; }
    // Low-32 projection of the flags word, i.e. the historical EFLAGS view.
    uint32_t eflags32() const { return static_cast<uint32_t>(rflags); }
};
#pragma pack(pop)

// Fixed guest-visible VA where the 64-byte `mem` blob lives. Both the oracle
// runner (via MAP_FIXED, requires reduced __PAGEZERO) and the interpreter-side
// contract harness map this address, so snippets addressing `[EDI]` with
// EDI = kMemVa observe the same bytes on both sides.
inline constexpr uint32_t kMemVa = 0x04000000u;

// Fixed VA at which the *snippet's first byte* is placed in kMode64. The
// runner MAP_FIXEDs a two-page window ending one page above this address and
// lays the prologue immediately below the snippet so execution falls through.
// The Vitreo-side adapters load the snippet at the same VA, which is what
// makes RIP-relative operands comparable.
inline constexpr uint64_t kCodeVa64 = 0x05000000ull;

static_assert(sizeof(State) == 1024, "State must match runner layout");

// Run a snippet on the oracle. Returns true if the runner exited cleanly and
// produced a full state packet. The returned State.status tells you whether
// the snippet itself executed without fault.
bool execute(const State& in, const std::vector<uint8_t>& code, State* out);

// Convenience: return default ok/fail counts for oracle availability.
bool available();

// Persistent warm-loop session. Spawns the runner with `--bench` once and
// reuses mapped code/scratch across iterations. Does not change `execute()`.
struct BenchTimes {
    uint64_t warmup_ns = 0;
    uint64_t run_ns = 0;
    uint64_t iters = 0;
    uint32_t status = 0;
    uint32_t reserved = 0;
};

class BenchSession {
public:
    BenchSession() = default;
    ~BenchSession() { close(); }
    BenchSession(const BenchSession&) = delete;
    BenchSession& operator=(const BenchSession&) = delete;

    bool open();
    bool load(const State& in, const std::vector<uint8_t>& code);
    bool execute_once(State* out);
    bool warmup(uint32_t n);
    bool run(uint32_t n);
    bool times(BenchTimes* out);
    void close();
    bool ok() const { return m_pid > 0; }

private:
    bool write_cmd(uint32_t cmd, uint32_t n);

    int m_pid = -1;
    int m_to = -1;
    int m_from = -1;
};

// Pretty-printer for diffing two states (interpreter vs oracle).
std::string diff(const State& got, const State& want);

}  // namespace vitreo::oracle
