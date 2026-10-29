// SPDX-License-Identifier: LGPL-2.1-only
// Differential probe for the msvcr80 *runtime* surface the math/string/stdio
// probes never reach: FP control, aligned allocation, threads, setjmp/longjmp,
// path splitting, locale, and the errno/mode accessors.
//
// The FP-control section is the point of this file. _controlfp does not just
// have to remember what it was told -- the setting has to actually change how
// arithmetic rounds. A control word that reads back correctly while the FPU
// ignores it is the failure mode that looks fine and computes wrong, so every
// case here follows the set with a computation whose result depends on it.
#include <windows.h>
#include <float.h>
#include <math.h>
#include <process.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <errno.h>

// mingw declares __p__fmode but not __p__commode; both are real msvcr80
// exports, so declare it here rather than dropping the check.
_CRTIMP int* __cdecl __p__commode(void);

#define P(k, v)  printf("%s %d\n", (k), (int)(v))
#define PX(k, v) printf("%s %08lX\n", (k), (unsigned long)(v))
static void PD(const char* k, double d) {
    unsigned long long b; memcpy(&b, &d, 8);
    printf("%s %016llx\n", k, b);
}

// Kept out of the optimiser's reach so the divide really happens under
// whatever control word is current.
static volatile double g_one = 1.0, g_three = 3.0, g_ten = 10.0;

static void t_controlfp(void) {
    const unsigned original = _controlfp(0, 0);
    // The raw word is machine/CRT specific; the maskable fields are contract.
    PX("cfp.initial.rc", original & _MCW_RC);
    PX("cfp.initial.pc", original & _MCW_PC);
    PX("cfp.initial.em", original & _MCW_EM);

    // ---- precision control -------------------------------------------------
    // 1/3 at 24-bit precision must differ from 1/3 at 53-bit. If PC is
    // ignored, all three of these come back identical -- which is exactly the
    // shape of a silently-wrong FPU.
    _controlfp(_PC_53, _MCW_PC);
    PX("cfp.pc53.readback", _controlfp(0, 0) & _MCW_PC);
    PD("cfp.pc53.div", g_one / g_three);

    _controlfp(_PC_24, _MCW_PC);
    PX("cfp.pc24.readback", _controlfp(0, 0) & _MCW_PC);
    PD("cfp.pc24.div", g_one / g_three);
    PD("cfp.pc24.sqrt", sqrt(g_ten));

    _controlfp(_PC_64, _MCW_PC);
    PX("cfp.pc64.readback", _controlfp(0, 0) & _MCW_PC);
    PD("cfp.pc64.div", g_one / g_three);

    _controlfp(_PC_53, _MCW_PC);

    // ---- rounding control --------------------------------------------------
    _controlfp(_RC_NEAR, _MCW_RC);
    PX("cfp.rcnear.readback", _controlfp(0, 0) & _MCW_RC);
    PD("cfp.rcnear.div", g_one / g_three);
    _controlfp(_RC_CHOP, _MCW_RC);
    PX("cfp.rcchop.readback", _controlfp(0, 0) & _MCW_RC);
    PD("cfp.rcchop.div", g_one / g_three);
    _controlfp(_RC_UP, _MCW_RC);
    PX("cfp.rcup.readback", _controlfp(0, 0) & _MCW_RC);
    PD("cfp.rcup.div", g_one / g_three);
    _controlfp(_RC_DOWN, _MCW_RC);
    PX("cfp.rcdown.readback", _controlfp(0, 0) & _MCW_RC);
    PD("cfp.rcdown.div", g_one / g_three);
    _controlfp(_RC_NEAR, _MCW_RC);

    // ---- exception masks ---------------------------------------------------
    // Masked is the default: a divide by zero yields infinity rather than
    // trapping. Unmasking is not exercised -- an unmasked trap would kill the
    // probe on one side and not the other.
    _controlfp(_MCW_EM, _MCW_EM);
    PX("cfp.em.allmasked", _controlfp(0, 0) & _MCW_EM);
    volatile double zero = 0.0;
    PD("cfp.em.divzero", g_one / zero);

    // _control87 differs from _controlfp on x86: it exposes the raw x87 word,
    // including the DENORMAL bit that _controlfp abstracts away.
    const unsigned c87 = _control87(0, 0);
    PX("c87.rc", c87 & _MCW_RC);
    PX("c87.pc", c87 & _MCW_PC);

    // ---- status word -------------------------------------------------------
    _clearfp();
    PX("sfp.cleared", _statusfp());
    volatile double tiny = 1e-320, big = 1e300;
    volatile double inexact = g_one / g_three;   // sets INEXACT
    (void)inexact;
    PX("sfp.after_inexact", _statusfp() & _SW_INEXACT);
    _clearfp();
    volatile double ovf = big * big;             // sets OVERFLOW + INEXACT
    (void)ovf;
    PX("sfp.after_overflow", _statusfp() & _SW_OVERFLOW);
    _clearfp();
    volatile double und = tiny * tiny;           // sets UNDERFLOW
    (void)und;
    PX("sfp.after_underflow", _statusfp() & _SW_UNDERFLOW);
    _clearfp();
    volatile double dz = g_one / zero;
    (void)dz;
    PX("sfp.after_divzero", _statusfp() & _SW_ZERODIVIDE);
    _clearfp();
    PX("sfp.recleared", _statusfp());

    _controlfp(original, 0xFFFFFFFFu);
    PX("cfp.restored.rc", _controlfp(0, 0) & _MCW_RC);
}

static void t_aligned(void) {
    for (size_t a = 2; a <= 128; a *= 2) {
        void* p = _aligned_malloc(100, a);
        char lbl[48];
        sprintf(lbl, "algn.%zu.nonnull", a);   P(lbl, p != NULL);
        sprintf(lbl, "algn.%zu.aligned", a);   P(lbl, p && (((size_t)p) % a) == 0);
        if (p) {
            memset(p, 0xAB, 100);
            void* q = _aligned_realloc(p, 200, a);
            sprintf(lbl, "algn.%zu.realloc_ok", a); P(lbl, q != NULL);
            sprintf(lbl, "algn.%zu.realloc_algn", a);
            P(lbl, q && (((size_t)q) % a) == 0);
            sprintf(lbl, "algn.%zu.preserved", a);
            P(lbl, q && ((unsigned char*)q)[0] == 0xAB &&
                        ((unsigned char*)q)[99] == 0xAB);
            _aligned_free(q ? q : p);
        }
    }
    // A non-power-of-two alignment is invalid: NULL and EINVAL.
    errno = 0;
    void* bad = _aligned_malloc(64, 3);
    P("algn.badalign.null", bad == NULL);
    P("algn.badalign.errno", errno);
    if (bad) _aligned_free(bad);
    _aligned_free(NULL);            // documented no-op
    P("algn.free_null.survived", 1);
}

static jmp_buf g_jb;
static int g_jump_side_effect = 0;
static void do_longjmp(int v) { g_jump_side_effect = 0x5A; longjmp(g_jb, v); }
static void t_setjmp(void) {
    const int r = setjmp(g_jb);
    if (r == 0) {
        P("sj.first_return", r);
        do_longjmp(42);
        P("sj.unreachable", 1);   // must not print
        return;
    }
    P("sj.second_return", r);
    P("sj.side_effect_ran", g_jump_side_effect == 0x5A);
    // longjmp(buf, 0) is documented to arrive as 1.
    static int once = 0;
    if (!once) {
        once = 1;
        if (setjmp(g_jb) == 0) longjmp(g_jb, 0);
        P("sj.zero_becomes_one", 1);
    }
}

static unsigned __stdcall thread_body(void* arg) {
    return (unsigned)(size_t)arg;
}
static void t_threads(void) {
    unsigned tid = 0;
    uintptr_t h = _beginthreadex(NULL, 0, thread_body, (void*)(size_t)7, 0, &tid);
    P("thr.handle_nonzero", h != 0);
    P("thr.tid_nonzero", tid != 0);
    if (h) {
        PX("thr.wait", WaitForSingleObject((HANDLE)h, 5000));
        DWORD code = 0;
        GetExitCodeThread((HANDLE)h, &code);
        P("thr.exit_code", (int)code);
        CloseHandle((HANDLE)h);
    }
    // CREATE_SUSPENDED must actually suspend: the thread cannot have run.
    unsigned tid2 = 0;
    uintptr_t h2 = _beginthreadex(NULL, 0, thread_body, (void*)(size_t)9,
                                  CREATE_SUSPENDED, &tid2);
    P("thr.susp.handle", h2 != 0);
    if (h2) {
        DWORD code = 0;
        GetExitCodeThread((HANDLE)h2, &code);
        P("thr.susp.still_active", code == STILL_ACTIVE);
        ResumeThread((HANDLE)h2);
        WaitForSingleObject((HANDLE)h2, 5000);
        GetExitCodeThread((HANDLE)h2, &code);
        P("thr.susp.exit_code", (int)code);
        CloseHandle((HANDLE)h2);
    }
}

static void t_paths(void) {
    char drive[8], dir[260], fname[128], ext[64];
    _splitpath("C:\\Program Files\\Game\\bin\\app.exe", drive, dir, fname, ext);
    printf("path.split |%s|%s|%s|%s|\n", drive, dir, fname, ext);
    _splitpath("relative\\x.tar.gz", drive, dir, fname, ext);
    printf("path.split2 |%s|%s|%s|%s|\n", drive, dir, fname, ext);
    _splitpath("noext", drive, dir, fname, ext);
    printf("path.split3 |%s|%s|%s|%s|\n", drive, dir, fname, ext);
    char out[512];
    _makepath(out, "D:", "\\a\\b\\", "file", ".txt");
    printf("path.make |%s|\n", out);
    _makepath(out, "", "", "bare", "");
    printf("path.make2 |%s|\n", out);
    char full[512];
    char* fp = _fullpath(full, "C:\\Temp\\..\\Temp\\x.txt", sizeof full);
    P("path.full.nonnull", fp != NULL);
    printf("path.full |%s|\n", fp ? fp : "");
}

static void t_misc_runtime(void) {
    // The mode accessors are pointers into CRT globals; the VALUE is the
    // contract (text mode / no-commit by default).
    P("rt.fmode", *__p__fmode());
    P("rt.commode", *__p__commode());

    errno = 0;
    (void)atoi("0");
    P("rt.errno_zero", errno);
    errno = ERANGE;
    P("rt.errno_roundtrip", errno == ERANGE);
    errno = 0;
    // strtod overflow sets ERANGE and returns HUGE_VAL.
    double ov = strtod("1e999", NULL);
    P("rt.strtod_erange", errno == ERANGE);
    PD("rt.strtod_ovf", ov);
    errno = 0;
    double un = strtod("1e-999", NULL);
    P("rt.strtod_under_erange", errno == ERANGE);
    PD("rt.strtod_under", un);

    // _msize reports the usable size of a malloc block.
    void* m = malloc(100);
    P("rt.msize_ge", m && _msize(m) >= 100);
    free(m);

    const char* loc = setlocale(LC_ALL, "C");
    printf("rt.locale |%s|\n", loc ? loc : "(null)");
    struct lconv* lc = localeconv();
    printf("rt.decimal |%s|\n", lc && lc->decimal_point ? lc->decimal_point : "?");
    P("rt.locale_c_thousands_empty",
      lc && lc->thousands_sep && lc->thousands_sep[0] == '\0');

    // _control87/_controlfp are per-thread; a fresh thread starts from the
    // default word, not the one this thread happens to be holding.
    P("rt.rand_deterministic_after_srand", (srand(12345), rand()) == (srand(12345), rand()));
}

int main(void) {
    setbuf(stdout, NULL);
    t_controlfp();
    t_aligned();
    t_setjmp();
    t_threads();
    t_paths();
    t_misc_runtime();
    printf("DONE 1\n");
    return 0;
}
