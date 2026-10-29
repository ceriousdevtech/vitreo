// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

// Differential test: Vitreo's interpreter against a real x86-64 core.
//
// The interpreter is the CPU semantic reference, and the JIT must match it.
// Every existing divergence test therefore compares Vitreo to
// Vitreo. That catches a JIT that drifts from the interpreter; it cannot catch
// an interpreter that was wrong to begin with, and a wrong reference makes the
// JIT wrong by construction.
//
// This test supplies the missing external check. `hardware_oracle/x86_reference.c`
// is built for x86_64 and runs under Rosetta 2, executing the same instruction
// bytes on an actual x86 core and reporting the architectural state back.
// Registers, EFLAGS and a shared scratch buffer are compared.
//
// Snippets address memory only through r15, and both sides map the scratch
// buffer at the same address, so pointer-producing instructions compare
// directly. The flags an instruction leaves architecturally undefined are
// excluded per generator rather than globally, so the comparison stays sharp.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "core/cpu/interpreter/interpreter.hpp"
#include "hardware_oracle/x86_ref_proto.h"

using namespace vitreo;
using namespace vitreo::cpu;

namespace {

constexpr u64 kCodeBase = 0x10000;
constexpr u64 kCodeSize = 0x1000;

// ---------------------------------------------------------------------------
// Oracle process
// ---------------------------------------------------------------------------

class Oracle {
public:
    bool start() {
        const char* path = std::getenv("VITREO_X86_ORACLE");
#ifdef VITREO_X86_ORACLE_PATH
        if (path == nullptr) path = VITREO_X86_ORACLE_PATH;
#endif
        if (path == nullptr) return false;
        const std::string cmd = std::string("'") + path + "'";
        m_pipe = popen(cmd.c_str(), "r+");
        if (m_pipe == nullptr) return false;
        u64 base = 0;
        if (std::fread(&base, sizeof(base), 1, m_pipe) != 1) {
            pclose(m_pipe);
            m_pipe = nullptr;
            return false;
        }
        return true;
    }
    ~Oracle() { if (m_pipe != nullptr) pclose(m_pipe); }

    bool run(const VjCase& c, VjResult& r) {
        if (m_pipe == nullptr) return false;
        if (std::fwrite(&c, sizeof(c), 1, m_pipe) != 1) return false;
        std::fflush(m_pipe);
        return std::fread(&r, sizeof(r), 1, m_pipe) == 1;
    }

private:
    FILE* m_pipe{nullptr};
};

// ---------------------------------------------------------------------------
// Snippet encoding
// ---------------------------------------------------------------------------

struct Enc {
    std::vector<u8>& b;
    void u8v(u8 v) { b.push_back(v); }
    void u16v(u16 v) { u8v(u8(v)); u8v(u8(v >> 8)); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) u8v(u8(v >> (8 * i))); }
    void rex(bool w, unsigned r, unsigned x, unsigned rm, bool force = false) {
        const u8 v = u8(0x40 | (unsigned(w) << 3) | (((r >> 3) & 1) << 2) |
                        (((x >> 3) & 1) << 1) | ((rm >> 3) & 1));
        if (w || r >= 8 || rm >= 8 || x >= 8 || force) u8v(v);
    }
    void modrm_rr(unsigned reg, unsigned rm) {
        u8v(u8(0xC0 | ((reg & 7) << 3) | (rm & 7)));
    }
    void modrm_mem32(unsigned reg, unsigned base, i32 disp) {
        u8v(u8(0x80 | ((reg & 7) << 3) | (base & 7)));
        if ((base & 7) == 4) u8v(0x24);
        u32v(u32(disp));
    }
    void opsize16() { u8v(0x66); }
};

using Rng = std::mt19937_64;

// x86 flag bits, named locally so the test does not depend on which header
// happens to export them.
constexpr u32 kCF = 1u << 0;
constexpr u32 kPF = 1u << 2;
constexpr u32 kAF = 1u << 4;
constexpr u32 kZF = 1u << 6;
constexpr u32 kSF = 1u << 7;
constexpr u32 kOF = 1u << 11;
constexpr u32 kAll = kCF | kPF | kAF | kZF | kSF | kOF;
constexpr u32 kLogic = kCF | kPF | kZF | kSF | kOF;   // AF undefined
constexpr u32 kShift = kCF | kPF | kZF | kSF;         // OF/AF vary
constexpr u32 kMul = kCF | kOF;
constexpr u32 kNone = 0;

// How the random XMM state is seeded. Packed-integer snippets want arbitrary
// bits; FP snippets want values that are actually numbers, because x86 and
// AArch64 disagree about the sign of the NaN an invalid operation produces and
// that is a separate question from whether the arithmetic is right.
enum class XmmInit : u8 { Random, Floats, Doubles, Extremes };

// x87 cases need well-behaved doubles in the scratch buffer and a control word
// that puts the hardware in double precision, so its 80-bit registers round the
// way a double-based model does.
enum class ScratchInit : u8 { Random, X87 };

struct Case {
    std::vector<u8> code;
    u32 flag_mask{kNone};
    std::string label;
    bool compare_xmm{false};
    XmmInit xmm_init{XmmInit::Random};
    ScratchInit scratch_init{ScratchInit::Random};
};

// r15 carries the scratch base and rsp lives inside it, so neither is a free
// destination.
unsigned pick_reg(Rng& rng) {
    for (;;) {
        const unsigned r = unsigned(rng() % 16);
        if (r == 15) continue;
        if (r == 4) continue;   // rsp
        return r;
    }
}

unsigned pick_width(Rng& rng) {
    static const unsigned kW[] = {8, 16, 32, 64};
    return kW[rng() % 4];
}

void alu_rr(Enc& e, unsigned op, unsigned w, unsigned dst, unsigned src) {
    if (w == 16) e.opsize16();
    const bool byte = (w == 8);
    // A byte form needs a REX to name SPL/BPL/SIL/DIL rather than AH..BH.
    e.rex(w == 64, src, 0, dst, byte);
    e.u8v(u8(op * 8 + (byte ? 0 : 1)));
    e.modrm_rr(src, dst);
}

using Gen = std::function<Case(Rng&)>;

std::vector<std::pair<const char*, Gen>> generators() {
    std::vector<std::pair<const char*, Gen>> g;

    g.emplace_back("alu-rr", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned op = unsigned(rng() % 8);
        const unsigned w = pick_width(rng);
        alu_rr(e, op, w, pick_reg(rng), pick_reg(rng));
        const bool logic = (op == 1 || op == 4 || op == 6);
        c.flag_mask = logic ? kLogic : kAll;
        c.label = "alu r/m,r";
        return c;
    });

    g.emplace_back("alu-ri", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned op = unsigned(rng() % 8);
        const unsigned w = pick_width(rng);
        const unsigned dst = pick_reg(rng);
        if (w == 16) e.opsize16();
        const bool byte = (w == 8);
        e.rex(w == 64, 0, 0, dst, byte);
        e.u8v(u8(byte ? 0x80 : 0x81));
        e.modrm_rr(op, dst);
        if (byte) e.u8v(u8(rng()));
        else if (w == 16) e.u16v(u16(rng()));
        else e.u32v(u32(rng()));
        const bool logic = (op == 1 || op == 4 || op == 6);
        c.flag_mask = logic ? kLogic : kAll;
        c.label = "alu r/m,imm";
        return c;
    });

    g.emplace_back("inc-dec-neg-not", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned w = pick_width(rng);
        const unsigned r = pick_reg(rng);
        const unsigned which = unsigned(rng() % 4);
        if (w == 16) e.opsize16();
        const bool byte = (w == 8);
        e.rex(w == 64, 0, 0, r, byte);
        if (which < 2) {
            e.u8v(u8(byte ? 0xFE : 0xFF));
            e.modrm_rr(which, r);
            c.flag_mask = kAll & ~kCF;   // INC/DEC preserve CF
        } else {
            e.u8v(u8(byte ? 0xF6 : 0xF7));
            e.modrm_rr(which, r);
            c.flag_mask = (which == 2) ? kNone : kAll;   // NOT writes no flags
        }
        c.label = "unary";
        return c;
    });

    g.emplace_back("shift-rotate-imm", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned w = pick_width(rng);
        const unsigned r = pick_reg(rng);
        unsigned sub = unsigned(rng() % 8);
        if (sub == 6) sub = 4;                     // /6 aliases SHL
        const unsigned cnt = unsigned(rng() % (w == 64 ? 64 : 32));
        if (w == 16) e.opsize16();
        const bool byte = (w == 8);
        e.rex(w == 64, 0, 0, r, byte);
        e.u8v(u8(byte ? 0xC0 : 0xC1));
        e.modrm_rr(sub, r);
        e.u8v(u8(cnt));
        const bool rotate = (sub < 4);
        // OF is architecturally defined only for a single-bit rotate.
        c.flag_mask = rotate ? (cnt == 1 ? (kCF | kOF) : kCF) : kShift;
        if (cnt == 0) c.flag_mask = kNone;
        c.label = "shift/rotate imm";
        return c;
    });

    g.emplace_back("shift-cl", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned w = pick_width(rng);
        unsigned r = pick_reg(rng);
        while (r == 1) r = pick_reg(rng);          // not rcx
        unsigned sub = unsigned(rng() % 8);
        if (sub == 6) sub = 4;
        if (w == 16) e.opsize16();
        const bool byte = (w == 8);
        e.rex(w == 64, 0, 0, r, byte);
        e.u8v(u8(byte ? 0xD2 : 0xD3));
        e.modrm_rr(sub, r);
        c.flag_mask = (sub < 4) ? kCF : kShift;
        c.label = "shift/rotate cl";
        return c;
    });

    g.emplace_back("setcc-cmovcc", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        alu_rr(e, 7, 64, pick_reg(rng), pick_reg(rng));      // CMP
        const unsigned cc = unsigned(rng() % 16);
        if (rng() & 1) {
            const unsigned r = pick_reg(rng);
            e.rex(false, 0, 0, r, true);
            e.u8v(0x0F);
            e.u8v(u8(0x90 + cc));
            e.modrm_rr(0, r);
            c.label = "setcc";
        } else {
            const unsigned dst = pick_reg(rng), src = pick_reg(rng);
            e.rex(rng() & 1, dst, 0, src);
            e.u8v(0x0F);
            e.u8v(u8(0x40 + cc));
            e.modrm_rr(dst, src);
            c.label = "cmovcc";
        }
        c.flag_mask = kAll;
        return c;
    });

    g.emplace_back("mul-imul", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned w = (rng() & 1) ? 64u : 32u;
        if (rng() & 1) {
            const unsigned dst = pick_reg(rng), src = pick_reg(rng);
            e.rex(w == 64, dst, 0, src);
            e.u8v(0x0F);
            e.u8v(0xAF);
            e.modrm_rr(dst, src);
            c.label = "imul r,r/m";
        } else {
            unsigned r = pick_reg(rng);
            while (r == 0 || r == 2) r = pick_reg(rng);      // not rax/rdx
            e.rex(w == 64, 0, 0, r);
            e.u8v(0xF7);
            e.modrm_rr(4 + (rng() & 1), r);                  // MUL / IMUL
            c.label = "mul/imul r/m";
        }
        c.flag_mask = kMul;
        return c;
    });

    g.emplace_back("bit-ops", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned w = (rng() & 1) ? 64u : 32u;
        const unsigned dst = pick_reg(rng);
        e.rex(w == 64, 0, 0, dst);
        e.u8v(0x0F);
        e.u8v(0xBA);
        e.modrm_rr(4 + unsigned(rng() % 4), dst);            // BT/BTS/BTR/BTC
        e.u8v(u8(rng() % (w == 64 ? 64 : 32)));
        c.flag_mask = kCF;
        c.label = "bt family";
        return c;
    });

    g.emplace_back("bsf-bsr", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned w = (rng() & 1) ? 64u : 32u;
        const unsigned dst = pick_reg(rng), src = pick_reg(rng);
        e.rex(w == 64, dst, 0, src);
        e.u8v(0x0F);
        e.u8v(u8((rng() & 1) ? 0xBD : 0xBC));
        e.modrm_rr(dst, src);
        c.flag_mask = kZF;
        c.label = "bsf/bsr";
        return c;
    });

    g.emplace_back("shld-shrd", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned w = (rng() & 1) ? 64u : 32u;
        const unsigned dst = pick_reg(rng), src = pick_reg(rng);
        const unsigned cnt = 1 + unsigned(rng() % (w - 1));
        e.rex(w == 64, src, 0, dst);
        e.u8v(0x0F);
        e.u8v(u8((rng() & 1) ? 0xA4 : 0xAC));
        e.modrm_rr(src, dst);
        e.u8v(u8(cnt));
        c.flag_mask = kShift;
        c.label = "shld/shrd";
        return c;
    });

    g.emplace_back("mem-rmw", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned op = unsigned(rng() % 8);
        const unsigned w = pick_width(rng);
        const unsigned src = pick_reg(rng);
        if (w == 16) e.opsize16();
        const bool byte = (w == 8);
        e.rex(w == 64, src, 0, 15, byte);
        e.u8v(u8(op * 8 + (byte ? 0 : 1)));
        e.modrm_mem32(src, 15, i32(rng() % 64) & ~7);
        const bool logic = (op == 1 || op == 4 || op == 6);
        c.flag_mask = logic ? kLogic : kAll;
        c.label = "mem rmw";
        return c;
    });

    g.emplace_back("mem-load-store", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned w = pick_width(rng);
        const unsigned r = pick_reg(rng);
        const bool store = (rng() & 1) != 0;
        if (w == 16) e.opsize16();
        const bool byte = (w == 8);
        e.rex(w == 64, r, 0, 15, byte);
        e.u8v(u8((store ? 0x88 : 0x8A) + (byte ? 0 : 1)));
        e.modrm_mem32(r, 15, i32(rng() % 64) & ~7);
        c.flag_mask = kNone;
        c.label = store ? "store" : "load";
        return c;
    });

    g.emplace_back("movzx-movsx", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned dst = pick_reg(rng), src = pick_reg(rng);
        e.rex(rng() & 1, dst, 0, src, true);
        e.u8v(0x0F);
        e.u8v(u8(((rng() & 1) ? 0xBE : 0xB6) + ((rng() & 1) ? 1 : 0)));
        e.modrm_rr(dst, src);
        c.flag_mask = kNone;
        c.label = "movzx/movsx";
        return c;
    });

    g.emplace_back("xchg-bswap-cbw", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        switch (rng() % 3) {
        case 0: {
            const unsigned a = pick_reg(rng), b = pick_reg(rng);
            e.rex(true, b, 0, a);
            e.u8v(0x87);
            e.modrm_rr(b, a);
            c.label = "xchg";
            break;
        }
        case 1: {
            const unsigned a = pick_reg(rng);
            e.rex(rng() & 1, 0, 0, a);
            e.u8v(0x0F);
            e.u8v(u8(0xC8 + (a & 7)));
            c.label = "bswap";
            break;
        }
        default: {
            const unsigned w = pick_width(rng);
            if (w == 16) e.opsize16();
            else if (w == 64) e.u8v(0x48);
            e.u8v(u8((rng() & 1) ? 0x98 : 0x99));
            c.label = "cbw/cwd";
            break;
        }
        }
        c.flag_mask = kNone;
        return c;
    });

    // Partial flag producers stacked on a full one. INC/DEC preserve CF and BT
    // writes only CF, so the flag read at the end may be owned by an
    // instruction several back. The mask is deliberately the full set.
    g.emplace_back("partial-flag-owner", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned w = (rng() & 1) ? 64u : 32u;
        alu_rr(e, unsigned(rng() % 8), w, pick_reg(rng), pick_reg(rng));
        const unsigned n = 1 + unsigned(rng() % 3);
        for (unsigned i = 0; i < n; ++i) {
            const unsigned r = pick_reg(rng);
            if (rng() % 3 == 2) {
                e.rex(w == 64, 0, 0, r);
                e.u8v(0x0F);
                e.u8v(0xBA);
                e.modrm_rr(4, r);                  // BT
                e.u8v(u8(rng() % (w == 64 ? 64 : 32)));
            } else {
                e.rex(w == 64, 0, 0, r);
                e.u8v(0xFF);
                e.modrm_rr(unsigned(rng() % 2), r);   // INC / DEC
            }
        }
        c.flag_mask = kAll;
        c.label = "alu + partial producers";
        return c;
    });

    g.emplace_back("carry-flag-ops", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        alu_rr(e, unsigned(rng() % 8), (rng() & 1) ? 64u : 32u, pick_reg(rng),
               pick_reg(rng));
        static const u8 kOps[] = {0xF8, 0xF9, 0xF5};   // CLC, STC, CMC
        const unsigned n = 1 + unsigned(rng() % 3);
        for (unsigned i = 0; i < n; ++i) {
            e.u8v(kOps[rng() % 3]);
            if (rng() % 3 == 0) {
                const unsigned r = pick_reg(rng);
                e.rex(true, 0, 0, r);
                e.u8v(0xFF);
                e.modrm_rr(0, r);                      // INC keeps CF
            }
        }
        c.flag_mask = kAll;
        c.label = "clc/stc/cmc";
        return c;
    });

    g.emplace_back("flag-image", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        alu_rr(e, unsigned(rng() % 8), (rng() & 1) ? 64u : 32u, pick_reg(rng),
               pick_reg(rng));
        switch (rng() % 3) {
        case 0: e.u8v(0x9F); c.label = "lahf"; break;
        case 1: e.u8v(0x9E); c.label = "sahf"; break;
        default: e.u8v(0x9C); e.u8v(0x9D); c.label = "pushf+popf"; break;
        }
        c.flag_mask = kAll;
        return c;
    });

    g.emplace_back("atomic-rmw", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned w = pick_width(rng);
        unsigned r = pick_reg(rng);
        while (r == 0) r = pick_reg(rng);              // not rax
        const bool cmpxchg = (rng() & 1) != 0;
        if (rng() & 1) e.u8v(0xF0);                    // LOCK
        if (w == 16) e.opsize16();
        const bool byte = (w == 8);
        e.rex(w == 64, r, 0, 15, byte);
        e.u8v(0x0F);
        e.u8v(u8(cmpxchg ? (byte ? 0xB0 : 0xB1) : (byte ? 0xC0 : 0xC1)));
        e.modrm_mem32(r, 15, i32(rng() % 40) & ~7);
        // CMPXCHG compares only ZF here, because THE ORACLE IS WRONG for the
        // other five.
        //
        // This oracle is Rosetta 2, which is a translator, not an x86 core --
        // and it computes CMPXCHG's flags as `DEST - accumulator`. Real
        // silicon computes `accumulator - DEST`, as Intel SDM Vol. 2 CMPXCHG
        // says ("compares the value in the AL/AX/EAX/RAX register WITH the
        // first operand"). Measured both ways with the same hand-encoded
        // `lock cmpxchg byte ptr [ebx], cl` and the same operands:
        // tools/oracle_probe/cmpxchg_flag_probe.c on an i9-12900KF says
        // `accumulator - DEST` in every unequal case, and
        // tools/oracle_probe/cmpxchg_flag_probe_rosetta.c says the opposite
        // on this host. Recorded outputs sit beside both.
        //
        // Vitreo followed Rosetta here -- in the interpreter, in three stencil
        // arms and in every lock helper -- because this test made it a gate.
        // ZF is identical under both readings, which is why every spinlock
        // kept working and nothing ever surfaced it.
        //
        // XADD is unaffected and stays fully compared.
        c.flag_mask = cmpxchg ? kZF : kAll;
        c.label = cmpxchg ? "cmpxchg mem" : "xadd mem";
        return c;
    });

    g.emplace_back("string-ops", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned src_off = 64 + unsigned(rng() % 256);
        const unsigned dst_off = 1024 + unsigned(rng() % 256);
        e.u8v(0x49); e.u8v(0x8D); e.u8v(0xB7); e.u32v(src_off);   // lea rsi,[r15+..]
        e.u8v(0x49); e.u8v(0x8D); e.u8v(0xBF); e.u32v(dst_off);   // lea rdi,[r15+..]
        e.u8v(0xB9); e.u32v(1 + unsigned(rng() % 24));            // mov ecx,n
        e.u8v(0xFC);                                              // cld
        static const char* kNames[] = {"movs", "stos", "lods", "cmps", "scas"};
        static const u8 kByteOp[] = {0xA4, 0xAA, 0xAC, 0xA6, 0xAE};
        const unsigned kind = unsigned(rng() % 5);
        const unsigned repkind = unsigned(rng() % 3);
        const unsigned w = pick_width(rng);
        if (repkind == 1) e.u8v(0xF3);
        else if (repkind == 2) e.u8v(0xF2);
        if (w == 16) e.opsize16();
        if (w == 64) e.u8v(0x48);
        e.u8v(u8(kByteOp[kind] + (w == 8 ? 0 : 1)));
        c.flag_mask = (kind >= 3) ? kAll : kNone;
        c.label = kNames[kind];
        return c;
    });

    // ---------------------------------------------------------------------
    // Areas that produced the most semantic surprises when this harness was
    // first written against a different x86 implementation: the conversions,
    // the wide multiplies and divides, the atomics, and x87.
    // ---------------------------------------------------------------------

    // `[66|F2|F3] [REX] 0F <opcode> /r` for an xmm,xmm form.
    auto sse_rr = [](Enc& e, u8 pfx, u8 opcode, unsigned dst, unsigned src,
                     u8 map38 = 0) {
        if (pfx != 0) e.u8v(pfx);
        if (dst >= 8 || src >= 8)
            e.u8v(u8(0x40 | (((dst >> 3) & 1) << 2) | ((src >> 3) & 1)));
        e.u8v(0x0F);
        if (map38 != 0) e.u8v(map38);
        e.u8v(opcode);
        e.modrm_rr(dst, src);
    };
    auto pick_xmm = [](Rng& rng) { return unsigned(rng() % 16); };

    g.emplace_back("sse2-packed-int", [sse_rr, pick_xmm](Rng& rng) {
        static const u8 kOps[] = {
            0xFC, 0xFD, 0xFE, 0xD4, 0xF8, 0xF9, 0xFA, 0xFB,   // padd/psub
            0xDB, 0xDF, 0xEB, 0xEF,                            // pand/pandn/por/pxor
            0x74, 0x75, 0x76, 0x64, 0x65, 0x66,                // pcmpeq/pcmpgt
            0xD5, 0xE5, 0xE4, 0xF4, 0xF5,                      // pmul*/pmaddwd
            0xDA, 0xDE, 0xEA, 0xEE, 0xE0, 0xE3, 0xF6,          // pmin/pmax/pavg/psadbw
            0x63, 0x6B, 0x67,                                  // packs/packus
            0x60, 0x61, 0x62, 0x6C, 0x68, 0x69, 0x6A, 0x6D,    // punpck
        };
        Case c;
        Enc e{c.code};
        sse_rr(e, 0x66, kOps[rng() % (sizeof(kOps) / sizeof(kOps[0]))],
               pick_xmm(rng), pick_xmm(rng));
        c.flag_mask = kNone;
        c.compare_xmm = true;
        c.label = "packed int";
        return c;
    });

    g.emplace_back("sse2-shift-shuffle", [sse_rr, pick_xmm](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned dst = pick_xmm(rng);
        switch (rng() % 4) {
        case 0: {   // 66 0F 71/72/73 /sub ib
            static const u8 kOp[] = {0x71, 0x72, 0x73};
            const u8 op = kOp[rng() % 3];
            static const unsigned kSub71[] = {2, 4, 6};
            static const unsigned kSub73[] = {2, 3, 6, 7};
            const unsigned sub = (op == 0x73) ? kSub73[rng() % 4] : kSub71[rng() % 3];
            e.u8v(0x66);
            if (dst >= 8) e.u8v(0x41);
            e.u8v(0x0F); e.u8v(op); e.modrm_rr(sub, dst);
            e.u8v(u8(rng() % 20));       // includes out-of-range counts
            c.label = "psxx imm";
            break;
        }
        case 1:
            sse_rr(e, 0x66, 0x70, dst, pick_xmm(rng)); e.u8v(u8(rng()));
            c.label = "pshufd";
            break;
        case 2:
            sse_rr(e, 0x00, 0xC6, dst, pick_xmm(rng)); e.u8v(u8(rng()));
            c.label = "shufps";
            break;
        default:
            sse_rr(e, 0x66, 0x00, dst, pick_xmm(rng), 0x38);
            c.label = "pshufb";
            break;
        }
        c.flag_mask = kNone;
        c.compare_xmm = true;
        return c;
    });

    g.emplace_back("sse-fp", [sse_rr, pick_xmm](Rng& rng) {
        static const u8 kOps[] = {0x58, 0x5C, 0x59, 0x5E, 0x5D, 0x5F, 0x51};
        static const u8 kPfx[] = {0x00, 0x66, 0xF3, 0xF2};
        Case c;
        Enc e{c.code};
        const u8 pfx = kPfx[rng() % 4];
        sse_rr(e, pfx, kOps[rng() % (sizeof(kOps) / sizeof(kOps[0]))],
               pick_xmm(rng), pick_xmm(rng));
        c.flag_mask = kNone;
        c.compare_xmm = true;
        c.xmm_init = (pfx == 0x66 || pfx == 0xF2) ? XmmInit::Doubles
                                                  : XmmInit::Floats;
        c.label = "fp arith";
        return c;
    });

    g.emplace_back("sse-comis", [sse_rr, pick_xmm](Rng& rng) {
        Case c;
        Enc e{c.code};
        const bool pd = (rng() & 1) != 0;
        sse_rr(e, pd ? u8(0x66) : u8(0x00), (rng() & 1) ? u8(0x2E) : u8(0x2F),
               pick_xmm(rng), pick_xmm(rng));
        c.flag_mask = kAll;
        c.compare_xmm = true;
        c.xmm_init = pd ? XmmInit::Doubles : XmmInit::Floats;
        c.label = "comis/ucomis";
        return c;
    });

    // The float to integer directions are where x86 and AArch64 part company:
    // x86 answers with the integer indefinite whenever the truncated result
    // does not fit, including for NaN and infinity, where AArch64 saturates.
    g.emplace_back("sse-convert-extremes", [sse_rr, pick_xmm](Rng& rng) {
        Case c;
        Enc e{c.code};
        c.xmm_init = XmmInit::Extremes;
        c.compare_xmm = true;
        c.flag_mask = kNone;
        const unsigned x = pick_xmm(rng);
        switch (rng() % 4) {
        case 0: sse_rr(e, 0x66, 0x5B, x, pick_xmm(rng)); c.label = "cvtps2dq"; break;
        case 1: sse_rr(e, 0xF3, 0x5B, x, pick_xmm(rng)); c.label = "cvttps2dq"; break;
        default: {
            const unsigned r = pick_reg(rng);
            const bool w64 = (rng() & 1) != 0;
            const bool dbl = (rng() & 1) != 0;
            e.u8v(dbl ? 0xF2 : 0xF3);
            e.rex(w64, r, 0, x, r >= 8 || x >= 8);
            e.u8v(0x0F); e.u8v((rng() & 1) ? 0x2C : 0x2D); e.modrm_rr(r, x);
            c.label = "cvt(t)s?2si";
            break;
        }
        }
        return c;
    });

    g.emplace_back("popcnt-bitcount", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned dst = pick_reg(rng), src = pick_reg(rng);
        const unsigned w = (rng() & 1) ? 64u : 32u;
        e.u8v(0xF3);
        e.rex(w == 64, dst, 0, src);
        e.u8v(0x0F);
        static const u8 kOps[] = {0xB8, 0xBC, 0xBD};   // POPCNT, TZCNT, LZCNT
        e.u8v(kOps[rng() % 3]);
        e.modrm_rr(dst, src);
        c.flag_mask = kCF | kZF;
        c.label = "popcnt/tzcnt/lzcnt";
        return c;
    });

    // DIV and IDIV, with the divisor and quotient shrunk so most cases produce
    // a usable quotient; the ones that raise #DE are skipped by the harness.
    g.emplace_back("div-idiv", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const unsigned w = pick_width(rng);
        unsigned r = pick_reg(rng);
        while (r == 0 || r == 2) r = pick_reg(rng);      // not rax/rdx
        const bool is_signed = (rng() & 1) != 0;
        if (w == 8) { e.u8v(0x66); e.u8v(0x25); e.u16v(0x00FF); }   // and ax,0xff
        else { e.rex(w == 64, 0, 0, 2); e.u8v(0x83); e.modrm_rr(4, 2); e.u8v(0x00); }
        e.rex(w == 64, 0, 0, r, w == 8);
        e.u8v(0x83); e.modrm_rr(4, r); e.u8v(0x7F);      // and r,0x7f
        e.rex(w == 64, 0, 0, r, w == 8);
        e.u8v(0x83); e.modrm_rr(1, r); e.u8v(0x01);      // or  r,1
        if (w == 16) e.opsize16();
        e.rex(w == 64, 0, 0, r, w == 8);
        e.u8v(u8(w == 8 ? 0xF6 : 0xF7));
        e.modrm_rr(is_signed ? 7u : 6u, r);
        // DIV leaves every flag undefined.
        c.flag_mask = kNone;
        c.label = is_signed ? "idiv" : "div";
        return c;
    });

    // CMPXCHG8B: half the cases store EDX:EAX first so the comparison
    // succeeds, which is the path that decides whether the accumulator pair is
    // written back.
    g.emplace_back("cmpxchg8b", [](Rng& rng) {
        Case c;
        Enc e{c.code};
        const i32 disp = i32(rng() % 40) & ~7;
        if (rng() & 1) {
            e.u8v(0x41); e.u8v(0x89); e.modrm_mem32(0, 15, disp);
            e.u8v(0x41); e.u8v(0x89); e.modrm_mem32(2, 15, disp + 4);
        }
        if (rng() & 1) e.u8v(0xF0);                      // LOCK
        e.u8v(0x41); e.u8v(0x0F); e.u8v(0xC7);
        e.modrm_mem32(1, 15, disp);
        c.flag_mask = kZF;
        c.label = "cmpxchg8b";
        return c;
    });

    // ---- x87 -------------------------------------------------------------
    // Each case first loads a control word selecting 53-bit precision, so the
    // hardware's 80-bit registers round the way a double-based model does.
    // Results are written back to the scratch buffer, which is compared.
    auto x87_cw = [](Enc& e) { e.u8v(0x41); e.u8v(0xD9); e.modrm_mem32(5, 15, 0); };
    auto fld_m64 = [](Enc& e, unsigned i) {
        e.u8v(0x41); e.u8v(0xDD); e.modrm_mem32(0, 15, 64 + 8 * i32(i));
    };
    auto fstp_m64 = [](Enc& e, unsigned i) {
        e.u8v(0x41); e.u8v(0xDD); e.modrm_mem32(3, 15, 512 + 8 * i32(i));
    };

    g.emplace_back("x87-arith", [x87_cw, fld_m64, fstp_m64](Rng& rng) {
        Case c;
        Enc e{c.code};
        c.scratch_init = ScratchInit::X87;
        x87_cw(e);
        fld_m64(e, unsigned(rng() % 16));
        static const unsigned kSub[] = {0, 1, 4, 5, 6, 7};
        const unsigned sub = kSub[rng() % 6];
        if (rng() & 1) {
            fld_m64(e, unsigned(rng() % 16));
            e.u8v(0xDE); e.modrm_rr(sub, 1);             // f<op>p st(1), st
            c.label = "faddp family";
        } else {
            e.u8v(0x41); e.u8v(0xDC);
            e.modrm_mem32(sub, 15, 64 + 8 * i32(rng() % 16));
            c.label = "fadd m64 family";
        }
        fstp_m64(e, 0);
        c.flag_mask = kNone;
        return c;
    });

    g.emplace_back("x87-unary-const", [x87_cw, fld_m64, fstp_m64](Rng& rng) {
        Case c;
        Enc e{c.code};
        c.scratch_init = ScratchInit::X87;
        x87_cw(e);
        if (rng() & 1) {
            fld_m64(e, unsigned(rng() % 16));
            static const u8 kOps[] = {0xE0, 0xE1, 0xFA, 0xFC};   // fchs fabs fsqrt frndint
            e.u8v(0xD9); e.u8v(kOps[rng() % 4]);
            c.label = "fchs/fabs/fsqrt/frndint";
        } else {
            static const u8 kOps[] = {0xE8, 0xEE, 0xEB, 0xEA, 0xE9, 0xEC, 0xED};
            e.u8v(0xD9); e.u8v(kOps[rng() % 7]);
            c.label = "fld constants";
        }
        fstp_m64(e, 0);
        c.flag_mask = kNone;
        return c;
    });

    g.emplace_back("x87-compare", [x87_cw, fld_m64, fstp_m64](Rng& rng) {
        Case c;
        Enc e{c.code};
        c.scratch_init = ScratchInit::X87;
        x87_cw(e);
        fld_m64(e, unsigned(rng() % 16));
        fld_m64(e, unsigned(rng() % 16));
        unsigned remaining = 2;
        if (rng() & 1) {
            static const u8 kOp[][2] = {{0xDB, 0xF1}, {0xDB, 0xE9},
                                        {0xDF, 0xF1}, {0xDF, 0xE9}};
            const unsigned k = unsigned(rng() % 4);
            e.u8v(kOp[k][0]); e.u8v(kOp[k][1]);
            if (k >= 2) remaining = 1;                   // the ...IP forms pop
            c.flag_mask = kAll;
            c.label = "fcomi family";
        } else {
            if (rng() & 1) { e.u8v(0xD8); e.u8v(0xD1); }   // fcom st(1)
            else { e.u8v(0xDD); e.u8v(0xE1); }             // fucom st(1)
            e.u8v(0xDF); e.u8v(0xE0);                      // fnstsw ax
            c.flag_mask = kNone;
            c.label = "fcom + fnstsw";
        }
        for (unsigned i = 0; i < remaining; ++i) fstp_m64(e, i);
        return c;
    });

    g.emplace_back("x87-convert", [x87_cw, fld_m64, fstp_m64](Rng& rng) {
        Case c;
        Enc e{c.code};
        c.scratch_init = ScratchInit::X87;
        x87_cw(e);
        if (rng() & 1) {
            e.u8v(0x41); e.u8v(0xDB);
            e.modrm_mem32(0, 15, 256 + 4 * i32(rng() % 8));   // fild m32
            fstp_m64(e, 0);
            c.label = "fild m32";
        } else {
            fld_m64(e, unsigned(rng() % 16));
            e.u8v(0x41); e.u8v(0xDB); e.modrm_mem32(3, 15, 512);   // fistp m32
            c.label = "fistp m32";
        }
        c.flag_mask = kNone;
        return c;
    });

    // A longer straight-line sequence: TOP moves several times inside one
    // block, which is what a register-resident x87 model has to get right.
    g.emplace_back("x87-mixed", [x87_cw, fld_m64, fstp_m64](Rng& rng) {
        Case c;
        Enc e{c.code};
        c.scratch_init = ScratchInit::X87;
        x87_cw(e);
        fld_m64(e, unsigned(rng() % 16));
        unsigned depth = 1;
        for (unsigned n = 0; n < 6; ++n) {
            switch (rng() % 4) {
            case 0:
                if (depth < 6) { fld_m64(e, unsigned(rng() % 16)); ++depth; }
                break;
            case 1:
                if (depth >= 2) {
                    e.u8v(0xDE); e.modrm_rr((rng() & 1) ? 0u : 1u, 1);
                    --depth;
                }
                break;
            case 2:
                e.u8v(0x41); e.u8v(0xDC);
                e.modrm_mem32((rng() & 1) ? 0u : 1u, 15, 64 + 8 * i32(rng() % 16));
                break;
            default:
                if (depth >= 2) { e.u8v(0xD9); e.u8v(0xC9); }   // fxch st(1)
                break;
            }
        }
        for (unsigned i = 0; i < depth; ++i) fstp_m64(e, i);
        c.flag_mask = kNone;
        c.label = "x87 mixed sequence";
        return c;
    });

    return g;
}

// Seeds the XMM file for a case.
void seed_xmm(Rng& rng, XmmInit mode, VjRegs& in) {
    for (int x = 0; x < 16; ++x) {
        for (int lane = 0; lane < 4; ++lane) {
            switch (mode) {
            case XmmInit::Random: {
                const u32 v = u32(rng());
                std::memcpy(in.xmm[x] + 4 * lane, &v, 4);
                break;
            }
            case XmmInit::Floats: {
                // Non-zero on purpose: a zero divisor makes DIVPS raise the
                // invalid-operation exception, and the two architectures
                // disagree about the sign of the NaN that produces.
                i32 n = i32(rng() % 2001) - 1000;
                if (n == 0) n = 7;
                const f32 f = f32(n) / f32(1 + (rng() % 16));
                std::memcpy(in.xmm[x] + 4 * lane, &f, 4);
                break;
            }
            case XmmInit::Doubles:
                if ((lane & 1) == 0) {
                    i64 n = i64(rng() % 2001) - 1000;
                    if (n == 0) n = 7;
                    const f64 d = f64(n) / f64(1 + (rng() % 16));
                    std::memcpy(in.xmm[x] + 4 * lane, &d, 8);
                }
                break;
            case XmmInit::Extremes: {
                // Out of integer range, infinite, or NaN: what the float to
                // integer conversions have to answer with x86's integer
                // indefinite rather than AArch64's saturation.
                static const u32 kPool[] = {
                    0x4F000000u,  // 2147483648.0f, first value out of range
                    0x4EFFFFFFu, 0xCF000000u, 0xCF000001u,
                    0x7F800000u,  // +inf
                    0xFF800000u,  // -inf
                    0x7FC00000u,  // NaN
                    0x5F000000u,  // 2^63
                    0x00000000u, 0x3F800000u, 0xC0000000u, 0x4B7FFFFFu,
                };
                const u32 v = kPool[rng() % (sizeof(kPool) / sizeof(kPool[0]))];
                std::memcpy(in.xmm[x] + 4 * lane, &v, 4);
                break;
            }
            }
        }
    }
}

// Lays out the scratch buffer the x87 generators expect: a control word at
// offset 0 selecting 53-bit precision and round-to-nearest, then a bank of
// doubles and one of 32-bit integers to load from.
void seed_x87_scratch(Rng& rng, u8* data) {
    const u16 cw = 0x027F;
    std::memcpy(data, &cw, 2);
    for (int i = 0; i < 16; ++i) {
        i64 n = i64(rng() % 4001) - 2000;
        if (n == 0) n = 3;
        const f64 d = f64(n) / f64(1 + (rng() % 32));
        std::memcpy(data + 64 + 8 * i, &d, 8);
    }
    for (int i = 0; i < 8; ++i) {
        const i32 v = i32(rng() % 200001) - 100000;
        std::memcpy(data + 256 + 4 * i, &v, 4);
    }
}

// Boundary values are where flag bugs live, so bias toward them.
u64 interesting_value(Rng& rng) {
    static const u64 kPool[] = {
        0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF,
        0x7FFFFFFF, 0x80000000ull, 0xFFFFFFFFull,
        0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, ~0ull,
        0x0101010101010101ull, 0xAAAAAAAAAAAAAAAAull, 0x5555555555555555ull,
    };
    if (rng() % 2) return kPool[rng() % (sizeof(kPool) / sizeof(kPool[0]))];
    return rng();
}

const char* const kRegName[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                  "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                  "r12", "r13", "r14", "r15"};

std::string hex_bytes(const std::vector<u8>& b) {
    std::string s;
    char buf[8];
    for (u8 x : b) { std::snprintf(buf, sizeof(buf), "%02x ", x); s += buf; }
    return s;
}

// ---------------------------------------------------------------------------
// Running a snippet on Vitreo's interpreter
// ---------------------------------------------------------------------------

struct Observed {
    u64 gpr[16]{};
    u8 xmm[16][16]{};
    u32 eflags{0};
    std::vector<u8> data;
    bool completed{false};
    // Why the snippet did not finish. A harness that reports only "did not
    // complete" is hard to tell apart from a real semantic divergence.
    std::string why;
};

Observed run_on_interpreter(const Case& c, const VjRegs& in, const u8* scratch) {
    Observed out;
    GuestMemory memory;
    // The scratch buffer sits at the same address on both sides so that a
    // pointer a snippet computes compares directly.
    if (!memory.map_region(VJ_SCRATCH_ADDR, VJ_SCRATCH, MemoryProtection::ReadWrite)) {
        out.why = "map_region(scratch) failed";
        return out;
    }
    if (!memory.map_region(kCodeBase, kCodeSize, MemoryProtection::ReadWrite)) {
        out.why = "map_region(code) failed";
        return out;
    }
    if (!memory.write_bytes(VJ_SCRATCH_ADDR, reinterpret_cast<const Byte*>(scratch),
                            VJ_SCRATCH)) {
        out.why = "write_bytes(scratch) failed";
        return out;
    }

    // No terminator instruction: HLT is privileged in user mode and Vitreo
    // models that, so the snippet is instead run until RIP reaches the end of
    // it. Snippets contain no branches, so RIP only moves forward.
    if (!memory.write_bytes(kCodeBase, reinterpret_cast<const Byte*>(c.code.data()),
                            c.code.size())) {
        out.why = "write_bytes(code) failed";
        return out;
    }

    CpuState cpu;
    cpu.init_long_mode();
    for (unsigned r = 0; r < 16; ++r) cpu.gpr.set64(u8(r), in.gpr[r]);
    cpu.gpr.set64(u8(VJ_BASE_REG), VJ_SCRATCH_ADDR);
    cpu.gpr.set64(4, VJ_SCRATCH_ADDR + VJ_STACK_OFFSET);   // rsp
    for (unsigned i = 0; i < 16; ++i)
        std::memcpy(cpu.sse.xmm[i].u8_, in.xmm[i], 16);
    cpu.flags.rflags = in.eflags;
    cpu.rip = kCodeBase;

    Interpreter interp(cpu, memory);
    const u64 end = kCodeBase + c.code.size();
    int steps = 0;
    while (cpu.rip < end && steps < 4096) {
        const ExecResult r = interp.step();
        if (r != ExecResult::Continue && r != ExecResult::Branch) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "step returned %d at rip=%#llx",
                          int(r), static_cast<unsigned long long>(cpu.rip));
            out.why = buf;
            break;
        }
        ++steps;
    }
    out.completed = out.why.empty() && cpu.rip == end;
    if (!out.completed && out.why.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "stopped at rip=%#llx, expected %#llx",
                      static_cast<unsigned long long>(cpu.rip),
                      static_cast<unsigned long long>(end));
        out.why = buf;
    }
    for (unsigned i = 0; i < 16; ++i) out.gpr[i] = cpu.gpr.get64(u8(i));
    for (unsigned i = 0; i < 16; ++i)
        std::memcpy(out.xmm[i], cpu.sse.xmm[i].u8_, 16);
    out.eflags = u32(cpu.flags.rflags);
    out.data.resize(VJ_SCRATCH);
    memory.read_bytes(VJ_SCRATCH_ADDR, reinterpret_cast<Byte*>(out.data.data()),
                      VJ_SCRATCH);
    return out;
}

}  // namespace

TEST(HardwareDifferential, InterpreterMatchesRealX86) {
    Oracle oracle;
    if (!oracle.start()) {
        GTEST_SKIP() << "x86 oracle unavailable (needs the host x86 runner and the "
                        "vitreo_x86_oracle target); skipping.";
    }

    const char* env = std::getenv("VITREO_X86_ORACLE_CASES");
    const int per_generator = (env != nullptr) ? std::atoi(env) : 200;

    auto gens = generators();
    Rng rng(0x5DEECE66Dull);
    int total = 0;
    int failures = 0;

    for (auto& entry : gens) {
        int ran = 0;
        int skipped = 0;
        int before = failures;
        for (int i = 0; i < per_generator && failures == before; ++i) {
            Case c = entry.second(rng);
            if (c.code.empty() || c.code.size() > VJ_MAX_CODE) continue;

            VjCase vc;
            std::memset(&vc, 0, sizeof(vc));
            vc.code_len = u32(c.code.size());
            std::memcpy(vc.code, c.code.data(), c.code.size());
            for (unsigned r = 0; r < 16; ++r) vc.in.gpr[r] = interesting_value(rng);
            // IF is forced on: user mode cannot clear it, so a seed with
            // IF=0 would put the two sides in a state no guest can reach and
            // report a divergence that says nothing.
            vc.in.eflags = (u32(rng()) & 0x8D5u) | 2u | (1u << 9);
            seed_xmm(rng, c.xmm_init, vc.in);
            for (int b = 0; b < VJ_SCRATCH; ++b) vc.data[b] = u8(rng());
            if (c.scratch_init == ScratchInit::X87) seed_x87_scratch(rng, vc.data);

            VjResult ref;
            ASSERT_TRUE(oracle.run(vc, ref)) << "x86 oracle stopped responding";
            if (ref.faulted != 0) { ++skipped; continue; }

            VjRegs in = vc.in;
            in.gpr[VJ_BASE_REG] = VJ_SCRATCH_ADDR;
            in.gpr[4] = VJ_SCRATCH_ADDR + VJ_STACK_OFFSET;

            const Observed got = run_on_interpreter(c, in, vc.data);
            ++ran;
            ++total;

            auto fail = [&](const std::string& what) {
                ++failures;
                std::string msg = entry.first + std::string(" / ") + c.label +
                                  ": " + what + "\n  code: " + hex_bytes(c.code) +
                                  "\n  in:   ";
                char buf[64];
                for (unsigned r = 0; r < 16; ++r) {
                    if (r == VJ_BASE_REG || r == 4) continue;
                    std::snprintf(buf, sizeof(buf), "%s=%llx ", kRegName[r],
                                  static_cast<unsigned long long>(in.gpr[r]));
                    msg += buf;
                }
                std::snprintf(buf, sizeof(buf), "\n  flags in: %08x", in.eflags);
                msg += buf;
                ADD_FAILURE() << msg;
            };

            if (!got.completed) {
                fail("interpreter did not complete the snippet: " + got.why);
                continue;
            }

            bool diverged = false;
            for (unsigned r = 0; r < 16 && !diverged; ++r) {
                if (ref.out.gpr[r] == got.gpr[r]) continue;
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s hw=%llx vitreo=%llx",
                              kRegName[r],
                              static_cast<unsigned long long>(ref.out.gpr[r]),
                              static_cast<unsigned long long>(got.gpr[r]));
                fail(buf);
                diverged = true;
            }
            if (diverged) continue;

            const u32 fa = ref.out.eflags & c.flag_mask;
            const u32 fb = got.eflags & c.flag_mask;
            if (fa != fb) {
                const u32 diff = fa ^ fb;
                std::string which;
                if (diff & kCF) which += "CF ";
                if (diff & kPF) which += "PF ";
                if (diff & kAF) which += "AF ";
                if (diff & kZF) which += "ZF ";
                if (diff & kSF) which += "SF ";
                if (diff & kOF) which += "OF ";
                char buf[96];
                std::snprintf(buf, sizeof(buf), "flags differ (%s) hw=%08x vitreo=%08x",
                              which.c_str(), fa, fb);
                fail(buf);
                continue;
            }

            if (c.compare_xmm) {
                bool xmm_diverged = false;
                for (int x = 0; x < 16 && !xmm_diverged; ++x) {
                    if (std::memcmp(ref.out.xmm[x], got.xmm[x], 16) == 0) continue;
                    char hw[64], vt[64];
                    for (int b = 0; b < 16; ++b) {
                        std::snprintf(hw + 3 * b, 4, "%02x ", ref.out.xmm[x][15 - b]);
                        std::snprintf(vt + 3 * b, 4, "%02x ", got.xmm[x][15 - b]);
                    }
                    fail("xmm" + std::to_string(x) + " hw=" + hw + " vitreo=" + vt);
                    xmm_diverged = true;
                }
                if (xmm_diverged) continue;
            }

            if (std::memcmp(ref.data, got.data.data(), VJ_SCRATCH) != 0) {
                for (int b = 0; b < VJ_SCRATCH; ++b) {
                    if (ref.data[b] == got.data[b]) continue;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf),
                                  "scratch[%d] hw=%02x vitreo=%02x", b,
                                  ref.data[b], got.data[b]);
                    fail(buf);
                    break;
                }
            }
        }
        // A case the oracle faulted on is skipped, not compared. Report it, so
        // a generator that mostly emits snippets hardware refuses to run cannot
        // look like success.
        std::printf("    %-22s %5d cases%s %s\n", entry.first, ran,
                    skipped ? (" (" + std::to_string(skipped) + " skipped)").c_str() : "",
                    (failures == before) ? "ok" : "FAILED");
    }
    std::printf("    %d differential cases against hardware\n", total);
    EXPECT_GT(total, 0);
}
