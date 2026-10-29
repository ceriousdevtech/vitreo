/// @file fp_runner.c
/// @brief 32-bit x86 floating-point semantics probe, run identically on native
///        Windows and under Vitreo so the two can be diffed exactly.
///
/// Every value is printed as a raw bit pattern, never as a formatted float, so
/// a single-ULP difference is as visible as a catastrophic one and no printf
/// rounding can hide a divergence.
///
/// The emphasis is on the operations whose failure changes *control flow*
/// rather than just a digit: comparison flags (a wrong CF/ZF turns
/// `if (motive <= critical)` into an always-true branch), MIN/MAX NaN
/// asymmetry, and float-to-int conversion including the out-of-range and NaN
/// cases that produce the integer indefinite value. Those are the ones that
/// can drive a simulation into a catastrophic state while ordinary arithmetic
/// still looks correct.
///
/// Build: i686-w64-mingw32-gcc -O1 -msse2 -o fp_runner.exe fp_runner.c
///
/// Usage: fp_runner.exe [section]
///   all | sse_arith | sse_cmp | sse_mask | sse_cvt | x87_arith | x87_cmp | x87_cvt

#include <stdio.h>
#include <string.h>

typedef unsigned int u32;
typedef unsigned long long u64;

static u32 f2b(float f) { u32 b; memcpy(&b, &f, 4); return b; }
static float b2f(u32 b) { float f; memcpy(&f, &b, 4); return f; }
static u64 d2b(double d) { u64 b; memcpy(&b, &d, 8); return b; }
static double b2d(u64 b) { double d; memcpy(&d, &b, 8); return d; }

/* Interesting single-precision bit patterns. Chosen so that every class the
   hardware treats specially is present: signed zeros, denormals, both
   infinities, a quiet and a signalling NaN, and values that sit outside the
   32-bit integer range so conversion has to produce the indefinite value. */
static const u32 kFloats[] = {
    0x00000000u, /* +0    */
    0x80000000u, /* -0    */
    0x3F800000u, /* 1.0   */
    0xBF800000u, /* -1.0  */
    0x3FC00000u, /* 1.5   */
    0x40490FDBu, /* pi    */
    0x00000001u, /* smallest denormal */
    0x807FFFFFu, /* -largest denormal */
    0x7F7FFFFFu, /* FLT_MAX */
    0x7F800000u, /* +inf  */
    0xFF800000u, /* -inf  */
    0x7FC00000u, /* qNaN  */
    0x7F800001u, /* sNaN  */
    0x4B000000u, /* 2^23  */
    0x4F000000u, /* 2^31 — just outside INT32 */
    0xCF000000u, /* -2^31 — exactly INT32_MIN */
    0x477FFF00u, /* 65535.0 */
    0x3F000000u, /* 0.5 — ties for round-to-even */
    0x3FA00000u, /* 1.25 — ties the other way */
    0xC0200000u, /* -2.5 — negative tie, separates round modes */
};
#define FLOAT_COUNT ((int)(sizeof(kFloats) / sizeof(kFloats[0])))

/* --------------------------------------------------------------------------
 * SSE scalar arithmetic
 * ----------------------------------------------------------------------- */

#define SSE_BINOP(name, mnemonic)                                             \
    static u32 name(u32 a_bits, u32 b_bits) {                                 \
        float a = b2f(a_bits), b = b2f(b_bits);                               \
        float r;                                                              \
        __asm__ volatile(mnemonic " %2, %0" : "=x"(r) : "0"(a), "x"(b));      \
        return f2b(r);                                                        \
    }

SSE_BINOP(op_addss, "addss")
SSE_BINOP(op_subss, "subss")
SSE_BINOP(op_mulss, "mulss")
SSE_BINOP(op_divss, "divss")
SSE_BINOP(op_minss, "minss")
SSE_BINOP(op_maxss, "maxss")

static void section_sse_arith(void) {
    int i, j;
    for (i = 0; i < FLOAT_COUNT; ++i) {
        for (j = 0; j < FLOAT_COUNT; ++j) {
            const u32 a = kFloats[i], b = kFloats[j];
            printf("[sse_arith] a=%08X b=%08X add=%08X sub=%08X mul=%08X "
                   "div=%08X min=%08X max=%08X\n",
                   a, b, op_addss(a, b), op_subss(a, b), op_mulss(a, b),
                   op_divss(a, b), op_minss(a, b), op_maxss(a, b));
        }
    }
}

/* --------------------------------------------------------------------------
 * SSE comparisons
 *
 * COMISS/UCOMISS write ZF, PF and CF and clear OF, SF and AF. The mask keeps
 * all six so that a flag which should have been cleared but was left set is
 * caught too — that is precisely the kind of error that flips a branch.
 * ----------------------------------------------------------------------- */

#define EFLAGS_MASK 0x8D5u /* CF | PF | AF | ZF | SF | OF */

static u32 cmp_flags_ss(u32 a_bits, u32 b_bits, int unordered) {
    float a = b2f(a_bits), b = b2f(b_bits);
    u32 flags;
    if (unordered) {
        __asm__ volatile("ucomiss %2, %1\n\tpushfl\n\tpopl %0"
                         : "=r"(flags) : "x"(a), "x"(b) : "cc");
    } else {
        __asm__ volatile("comiss %2, %1\n\tpushfl\n\tpopl %0"
                         : "=r"(flags) : "x"(a), "x"(b) : "cc");
    }
    return flags & EFLAGS_MASK;
}

static u32 cmp_flags_sd(u64 a_bits, u64 b_bits, int unordered) {
    double a = b2d(a_bits), b = b2d(b_bits);
    u32 flags;
    if (unordered) {
        __asm__ volatile("ucomisd %2, %1\n\tpushfl\n\tpopl %0"
                         : "=r"(flags) : "x"(a), "x"(b) : "cc");
    } else {
        __asm__ volatile("comisd %2, %1\n\tpushfl\n\tpopl %0"
                         : "=r"(flags) : "x"(a), "x"(b) : "cc");
    }
    return flags & EFLAGS_MASK;
}

/* The comparison forms a compiler actually emits for `a < b` style source.
   A correct flag result that the setcc reads wrongly is still a wrong branch,
   so both layers are recorded. */
static u32 predicates_ss(u32 a_bits, u32 b_bits) {
    float a = b2f(a_bits), b = b2f(b_bits);
    u32 result = 0;
    if (a < b) result |= 1u;
    if (a <= b) result |= 2u;
    if (a == b) result |= 4u;
    if (a != b) result |= 8u;
    if (a >= b) result |= 16u;
    if (a > b) result |= 32u;
    return result;
}

static void section_sse_cmp(void) {
    int i, j;
    for (i = 0; i < FLOAT_COUNT; ++i) {
        for (j = 0; j < FLOAT_COUNT; ++j) {
            const u32 a = kFloats[i], b = kFloats[j];
            /* Widen to double through the same bit patterns so the SD forms
               see the identical ordering cases, NaNs included. */
            const u64 ad = d2b((double)b2f(a));
            const u64 bd = d2b((double)b2f(b));
            printf("[sse_cmp] a=%08X b=%08X comiss=%03X ucomiss=%03X "
                   "comisd=%03X ucomisd=%03X pred=%02X\n",
                   a, b, cmp_flags_ss(a, b, 0), cmp_flags_ss(a, b, 1),
                   cmp_flags_sd(ad, bd, 0), cmp_flags_sd(ad, bd, 1),
                   predicates_ss(a, b));
        }
    }
}

/* --------------------------------------------------------------------------
 * SSE conversions
 * ----------------------------------------------------------------------- */

static u32 cvttss2si(u32 bits) {
    float f = b2f(bits);
    u32 r;
    __asm__ volatile("cvttss2si %1, %0" : "=r"(r) : "x"(f));
    return r;
}

static u32 cvtss2si(u32 bits) {
    float f = b2f(bits);
    u32 r;
    __asm__ volatile("cvtss2si %1, %0" : "=r"(r) : "x"(f));
    return r;
}

static u64 cvtss2sd(u32 bits) {
    float f = b2f(bits);
    double d;
    __asm__ volatile("cvtss2sd %1, %0" : "=x"(d) : "x"(f));
    return d2b(d);
}

static u32 cvtsi2ss(u32 value) {
    float f;
    __asm__ volatile("cvtsi2ss %1, %0" : "=x"(f) : "r"(value));
    return f2b(f);
}

/* MXCSR rounding control lives in bits 13:14. Conversion via CVTSS2SI obeys
   it while CVTTSS2SI always truncates, so both are sampled under each mode. */
static u32 get_mxcsr(void) {
    u32 v;
    __asm__ volatile("stmxcsr %0" : "=m"(v));
    return v;
}

static void set_mxcsr(u32 v) { __asm__ volatile("ldmxcsr %0" : : "m"(v)); }

/* --------------------------------------------------------------------------
 * Mask-producing compares and the boolean algebra built on them.
 *
 * COMISS writes EFLAGS and is what a simple `if (a < b)` compiles to. But a
 * JIT emitting straight-line code — Mono's, for instance — leans on CMPSS/
 * CMPPS instead, which produce all-ones/all-zeros masks that are then combined
 * with ANDPS/ORPS/ANDNPS and collected with MOVMSKPS. A wrong predicate or a
 * wrong mask polarity there yields a wrong *decision* while every input value
 * stays perfectly correct, which is invisible to an arithmetic test.
 * ----------------------------------------------------------------------- */

#define CMPSS_PRED(name, imm)                                                 \
    static u32 name(u32 a_bits, u32 b_bits) {                                 \
        float a = b2f(a_bits), b = b2f(b_bits);                               \
        float r;                                                              \
        __asm__ volatile("cmpss $" #imm ", %2, %0"                            \
                         : "=x"(r) : "0"(a), "x"(b));                         \
        return f2b(r);                                                        \
    }

CMPSS_PRED(cmpss_eq, 0)
CMPSS_PRED(cmpss_lt, 1)
CMPSS_PRED(cmpss_le, 2)
CMPSS_PRED(cmpss_unord, 3)
CMPSS_PRED(cmpss_neq, 4)
CMPSS_PRED(cmpss_nlt, 5)
CMPSS_PRED(cmpss_nle, 6)
CMPSS_PRED(cmpss_ord, 7)

/* MOVMSKPS is how mask lanes become an integer a branch can test. */
static u32 movmskps_of_cmp(u32 a_bits, u32 b_bits) {
    float a = b2f(a_bits), b = b2f(b_bits);
    u32 r;
    __asm__ volatile("cmpps $1, %2, %1\n\tmovmskps %1, %0"
                     : "=r"(r), "+x"(a) : "x"(b));
    return r;
}

/* ANDPS / ANDNPS / ORPS as a select: (a<b) ? a : b, built the way compiled
   code builds it. Wrong mask polarity picks the wrong operand. */
static u32 masked_select(u32 a_bits, u32 b_bits) {
    float a = b2f(a_bits), b = b2f(b_bits), m = b2f(a_bits), r;
    __asm__ volatile(
        "movaps %1, %0\n\t"      /* r = a            */
        "cmpss $1, %2, %3\n\t"   /* m = (a < b)      */
        "andps %3, %0\n\t"       /* r = a & m        */
        "andnps %2, %3\n\t"      /* m = ~m & b       */
        "orps %3, %0"            /* r = (a&m)|(~m&b) */
        : "=&x"(r), "+x"(a), "+x"(b), "+x"(m));
    return f2b(r);
}

/* Packed integer compares — the same decision problem in the integer domain. */
static u32 pcmpgtd_lane0(u32 a, u32 b) {
    u32 r;
    __asm__ volatile("movd %1, %%xmm0\n\t"
                     "movd %2, %%xmm1\n\t"
                     "pcmpgtd %%xmm1, %%xmm0\n\t"
                     "movd %%xmm0, %0"
                     : "=r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1");
    return r;
}

static u32 pcmpeqd_lane0(u32 a, u32 b) {
    u32 r;
    __asm__ volatile("movd %1, %%xmm0\n\t"
                     "movd %2, %%xmm1\n\t"
                     "pcmpeqd %%xmm1, %%xmm0\n\t"
                     "movd %%xmm0, %0"
                     : "=r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1");
    return r;
}

static void section_sse_mask(void) {
    int i, j;
    for (i = 0; i < FLOAT_COUNT; ++i) {
        for (j = 0; j < FLOAT_COUNT; ++j) {
            const u32 a = kFloats[i], b = kFloats[j];
            printf("[sse_mask] a=%08X b=%08X eq=%08X lt=%08X le=%08X "
                   "unord=%08X neq=%08X nlt=%08X nle=%08X ord=%08X "
                   "movmsk=%X sel=%08X pcmpgtd=%08X pcmpeqd=%08X\n",
                   a, b, cmpss_eq(a, b), cmpss_lt(a, b), cmpss_le(a, b),
                   cmpss_unord(a, b), cmpss_neq(a, b), cmpss_nlt(a, b),
                   cmpss_nle(a, b), cmpss_ord(a, b), movmskps_of_cmp(a, b),
                   masked_select(a, b), pcmpgtd_lane0(a, b),
                   pcmpeqd_lane0(a, b));
        }
    }
}

static void section_sse_cvt(void) {
    static const u32 kModes[] = {0u, 1u, 2u, 3u}; /* near, down, up, truncate */
    static const char* kModeNames[] = {"near", "down", "up", "trunc"};
    const u32 saved = get_mxcsr();
    int m, i;
    static const int kInts[] = {0, 1, -1, 2147483647, -2147483647 - 1, 12345,
                                -65536, 16777217};

    for (m = 0; m < 4; ++m) {
        set_mxcsr((saved & ~0x6000u) | (kModes[m] << 13));
        for (i = 0; i < FLOAT_COUNT; ++i) {
            const u32 f = kFloats[i];
            printf("[sse_cvt] rc=%s f=%08X cvttss2si=%08X cvtss2si=%08X "
                   "cvtss2sd=%016llX\n",
                   kModeNames[m], f, cvttss2si(f), cvtss2si(f),
                   (unsigned long long)cvtss2sd(f));
        }
    }
    set_mxcsr(saved);

    for (i = 0; i < (int)(sizeof(kInts) / sizeof(kInts[0])); ++i) {
        printf("[sse_cvt] i=%d cvtsi2ss=%08X\n", kInts[i],
               cvtsi2ss((u32)kInts[i]));
    }
    printf("[sse_cvt] mxcsr_default=%08X\n", saved);
}

/* --------------------------------------------------------------------------
 * x87
 * ----------------------------------------------------------------------- */

static u32 x87_control(void) {
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    return cw;
}

static void x87_set_control(unsigned short cw) {
    __asm__ volatile("fldcw %0" : : "m"(cw));
}

/* C3/C2/C0 of the status word are the x87 comparison result. FCOM sets them;
   FCOMI writes EFLAGS instead, so both encodings are sampled. */
static u32 x87_fcom_status(u32 a_bits, u32 b_bits) {
    float a = b2f(a_bits), b = b2f(b_bits);
    unsigned short sw;
    __asm__ volatile(
        "flds %2\n\t"   /* st0 = b */
        "flds %1\n\t"   /* st0 = a, st1 = b */
        "fcomp %%st(1)\n\t"
        "fnstsw %0\n\t"
        "fstp %%st(0)"
        : "=a"(sw) : "m"(a), "m"(b) : "cc");
    return sw & 0x4700u; /* C3 | C2 | C0 */
}

static u32 x87_fcomi_flags(u32 a_bits, u32 b_bits) {
    float a = b2f(a_bits), b = b2f(b_bits);
    u32 flags;
    __asm__ volatile(
        "flds %2\n\t"
        "flds %1\n\t"
        "fcomi %%st(1), %%st\n\t"
        "pushfl\n\t"
        "popl %0\n\t"
        "fstp %%st(0)\n\t"
        "fstp %%st(0)"
        : "=r"(flags) : "m"(a), "m"(b) : "cc", "eax");
    return flags & EFLAGS_MASK;
}

static void section_x87_cmp(void) {
    int i, j;
    for (i = 0; i < FLOAT_COUNT; ++i) {
        for (j = 0; j < FLOAT_COUNT; ++j) {
            printf("[x87_cmp] a=%08X b=%08X fcom_sw=%04X fcomi=%03X\n",
                   kFloats[i], kFloats[j], x87_fcom_status(kFloats[i], kFloats[j]),
                   x87_fcomi_flags(kFloats[i], kFloats[j]));
        }
    }
}

static u64 x87_binop(u32 a_bits, u32 b_bits, int which) {
    float a = b2f(a_bits), b = b2f(b_bits);
    double r;
    switch (which) {
    case 0: __asm__ volatile("flds %1\n\tflds %2\n\tfaddp %%st, %%st(1)\n\t"
                             "fstpl %0" : "=m"(r) : "m"(a), "m"(b)); break;
    case 1: __asm__ volatile("flds %1\n\tflds %2\n\tfsubrp %%st, %%st(1)\n\t"
                             "fstpl %0" : "=m"(r) : "m"(a), "m"(b)); break;
    case 2: __asm__ volatile("flds %1\n\tflds %2\n\tfmulp %%st, %%st(1)\n\t"
                             "fstpl %0" : "=m"(r) : "m"(a), "m"(b)); break;
    default: __asm__ volatile("flds %1\n\tflds %2\n\tfdivrp %%st, %%st(1)\n\t"
                              "fstpl %0" : "=m"(r) : "m"(a), "m"(b)); break;
    }
    return d2b(r);
}

static void section_x87_arith(void) {
    /* Precision control: 24-bit, 53-bit and 64-bit significands. A translator
       that models x87 as plain double silently ignores this field. */
    static const unsigned short kPc[] = {0x003Fu, 0x023Fu, 0x033Fu};
    static const char* kPcNames[] = {"pc24", "pc53", "pc64"};
    const u32 saved = x87_control();
    int p, i, j;

    for (p = 0; p < 3; ++p) {
        x87_set_control(kPc[p]);
        for (i = 0; i < FLOAT_COUNT; ++i) {
            for (j = 0; j < FLOAT_COUNT; ++j) {
                printf("[x87_arith] %s a=%08X b=%08X add=%016llX sub=%016llX "
                       "mul=%016llX div=%016llX\n",
                       kPcNames[p], kFloats[i], kFloats[j],
                       (unsigned long long)x87_binop(kFloats[i], kFloats[j], 0),
                       (unsigned long long)x87_binop(kFloats[i], kFloats[j], 1),
                       (unsigned long long)x87_binop(kFloats[i], kFloats[j], 2),
                       (unsigned long long)x87_binop(kFloats[i], kFloats[j], 3));
            }
        }
    }
    x87_set_control((unsigned short)saved);
    printf("[x87_arith] cw_default=%04X\n", saved);
}

/* FISTP under each rounding mode. This is the classic _ftol path, and a wrong
   result here corrupts any integer derived from a float — frame counts,
   indices, thresholds. */
static u32 x87_fistp(u32 bits) {
    float f = b2f(bits);
    int r;
    __asm__ volatile("flds %1\n\tfistpl %0" : "=m"(r) : "m"(f));
    return (u32)r;
}

static void section_x87_cvt(void) {
    static const unsigned short kRc[] = {0x037Fu, 0x077Fu, 0x0B7Fu, 0x0F7Fu};
    static const char* kRcNames[] = {"near", "down", "up", "trunc"};
    const u32 saved = x87_control();
    int m, i;
    for (m = 0; m < 4; ++m) {
        x87_set_control(kRc[m]);
        for (i = 0; i < FLOAT_COUNT; ++i) {
            printf("[x87_cvt] rc=%s f=%08X fistp=%08X\n", kRcNames[m],
                   kFloats[i], x87_fistp(kFloats[i]));
        }
    }
    x87_set_control((unsigned short)saved);
}

int main(int argc, char** argv) {
    const char* section = (argc > 1) ? argv[1] : "all";
    const int all = (strcmp(section, "all") == 0);

    setvbuf(stdout, NULL, _IONBF, 0);

    if (all || strcmp(section, "sse_arith") == 0) section_sse_arith();
    if (all || strcmp(section, "sse_cmp") == 0) section_sse_cmp();
    if (all || strcmp(section, "sse_mask") == 0) section_sse_mask();
    if (all || strcmp(section, "sse_cvt") == 0) section_sse_cvt();
    if (all || strcmp(section, "x87_arith") == 0) section_x87_arith();
    if (all || strcmp(section, "x87_cmp") == 0) section_x87_cmp();
    if (all || strcmp(section, "x87_cvt") == 0) section_x87_cvt();

    printf("[done] section=%s\n", section);
    return 0;
}
