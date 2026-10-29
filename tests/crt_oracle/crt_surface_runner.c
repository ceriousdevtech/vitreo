/// @file crt_surface_runner.c
/// @brief Broad 32-bit CRT contract probe, run identically on native Windows
///        and under Vitreo so the two can be diffed exactly.
///
/// Motivation: Sims 3 imports 182 msvcr80 functions and, before this runner,
/// NONE of them were validated against Windows -- the largest unvalidated
/// surface in the emulator by a wide margin (517 APIs used, 201 validated).
/// The CRT is where wrong answers are silent: they produce plausible numbers
/// and well-formed strings that fail only much later, which is precisely the
/// failure mode already recorded three times in this codebase (scanf %lf
/// writing 4 bytes into an 8-byte double, fileno() reading a zeroed field,
/// the UCRT api-set gap).
///
/// Every floating-point result is printed as a raw 64-bit pattern so a 1-ULP
/// difference is as visible as a catastrophic one. Every function that returns
/// a count or a pointer relationship prints that too -- "silently matched
/// nothing" must be distinguishable from "matched correctly".
///
/// The _CI* entries matter disproportionately: MSVC passes their arguments and
/// returns their results on the x87 stack, so they exercise the FPU calling
/// convention at the emulator boundary, not just the math.
///
/// Build: i686-w64-mingw32-gcc -O1 -o crt_surface_runner.exe crt_surface_runner.c
/// Usage: crt_surface_runner.exe [all|math|string|conv|alg|heap|time]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

typedef unsigned long long u64;
static u64 d2b(double d) { u64 b; memcpy(&b, &d, 8); return b; }
#define PD(label, expr) printf("%-28s %016llx\n", label, d2b((double)(expr)))
#define PI(label, expr) printf("%-28s %lld\n",    label, (long long)(expr))

static const double kAngles[] = {
    0.0, 1.0, -1.0, 0.5, -0.5, 3.14159265358979, 1.5707963267948966,
    -1.5707963267948966, 6.283185307179586, 1e-8, 1e8, 0.7071067811865476,
    2.0, 10.0, 100.0, 0.1, 1e-300, 1e300, 123456.789
};
enum { NANG = (int)(sizeof(kAngles)/sizeof(kAngles[0])) };

/* MSVC routes these through x87-stack intrinsics, so this section probes the
   FPU calling convention as much as the math. */
static void section_math(void) {
    printf("== math ==\n");
    for (int i = 0; i < NANG; ++i) {
        double x = kAngles[i];
        char lbl[40];
        snprintf(lbl, sizeof lbl, "sin[%d]", i);   PD(lbl, sin(x));
        snprintf(lbl, sizeof lbl, "cos[%d]", i);   PD(lbl, cos(x));
        snprintf(lbl, sizeof lbl, "tan[%d]", i);   PD(lbl, tan(x));
        snprintf(lbl, sizeof lbl, "atan[%d]", i);  PD(lbl, atan(x));
        snprintf(lbl, sizeof lbl, "exp[%d]", i);   PD(lbl, exp(x));
        snprintf(lbl, sizeof lbl, "sqrt[%d]", i);  PD(lbl, sqrt(fabs(x)));
        snprintf(lbl, sizeof lbl, "log[%d]", i);   PD(lbl, log(fabs(x) + 1e-320));
        snprintf(lbl, sizeof lbl, "log10[%d]", i); PD(lbl, log10(fabs(x) + 1e-320));
        snprintf(lbl, sizeof lbl, "sinh[%d]", i);  PD(lbl, sinh(x > 700 ? 700 : x));
        snprintf(lbl, sizeof lbl, "cosh[%d]", i);  PD(lbl, cosh(x > 700 ? 700 : x));
        snprintf(lbl, sizeof lbl, "tanh[%d]", i);  PD(lbl, tanh(x));
        snprintf(lbl, sizeof lbl, "ceil[%d]", i);  PD(lbl, ceil(x));
        snprintf(lbl, sizeof lbl, "floor[%d]", i); PD(lbl, floor(x));
        snprintf(lbl, sizeof lbl, "fmod[%d]", i);  PD(lbl, fmod(x, 3.0));
        /* asin/acos are only defined on [-1,1]; feed a clamped value so the
           comparison is about precision, not about who returns which NaN. */
        double c = x > 1.0 ? 1.0 : (x < -1.0 ? -1.0 : x);
        snprintf(lbl, sizeof lbl, "asin[%d]", i);  PD(lbl, asin(c));
        snprintf(lbl, sizeof lbl, "acos[%d]", i);  PD(lbl, acos(c));
        snprintf(lbl, sizeof lbl, "atan2[%d]", i); PD(lbl, atan2(x, 2.0));
        snprintf(lbl, sizeof lbl, "pow[%d]", i);   PD(lbl, pow(fabs(x) + 0.5, 1.5));
        int e = 0; double mant = frexp(x, &e);
        snprintf(lbl, sizeof lbl, "frexp.m[%d]", i); PD(lbl, mant);
        snprintf(lbl, sizeof lbl, "frexp.e[%d]", i); PI(lbl, e);
        snprintf(lbl, sizeof lbl, "ldexp[%d]", i); PD(lbl, ldexp(x, 3));
        double ip = 0; double fp = modf(x, &ip);
        snprintf(lbl, sizeof lbl, "modf.i[%d]", i); PD(lbl, ip);
        snprintf(lbl, sizeof lbl, "modf.f[%d]", i); PD(lbl, fp);
    }
}

static void section_string(void) {
    printf("== string ==\n");
    static const char* a[] = {"", "a", "A", "abc", "ABC", "abcd", "aBcD",
                              "hello world", "Hello World", "\xff\xfe", "123",
                              "  lead", "trail  ", "MiXeD123"};
    enum { NA = (int)(sizeof(a)/sizeof(a[0])) };
    for (int i = 0; i < NA; ++i) {
        for (int j = 0; j < NA; ++j) {
            char lbl[48];
            /* Windows returns the SIGN, not a fixed magnitude; normalise so a
               real ordering difference is what shows, not a libc convention. */
            int c1 = strcmp(a[i], a[j]);
            int c2 = _stricmp(a[i], a[j]);
            int c3 = strncmp(a[i], a[j], 3);
            int c4 = _strnicmp(a[i], a[j], 3);
            snprintf(lbl, sizeof lbl, "strcmp[%d,%d]", i, j);   PI(lbl, (c1>0)-(c1<0));
            snprintf(lbl, sizeof lbl, "stricmp[%d,%d]", i, j);  PI(lbl, (c2>0)-(c2<0));
            snprintf(lbl, sizeof lbl, "strncmp[%d,%d]", i, j);  PI(lbl, (c3>0)-(c3<0));
            snprintf(lbl, sizeof lbl, "strnicmp[%d,%d]", i, j); PI(lbl, (c4>0)-(c4<0));
        }
        char lbl[40], buf[64];
        snprintf(lbl, sizeof lbl, "strlen[%d]", i); PI(lbl, (long long)strlen(a[i]));
        const char* p = strchr(a[i], 'l');
        snprintf(lbl, sizeof lbl, "strchr[%d]", i); PI(lbl, p ? (p - a[i]) : -1);
        p = strrchr(a[i], 'l');
        snprintf(lbl, sizeof lbl, "strrchr[%d]", i); PI(lbl, p ? (p - a[i]) : -1);
        p = strstr(a[i], "lo");
        snprintf(lbl, sizeof lbl, "strstr[%d]", i); PI(lbl, p ? (p - a[i]) : -1);
        snprintf(lbl, sizeof lbl, "strspn[%d]", i);  PI(lbl, (long long)strspn(a[i], "abc "));
        snprintf(lbl, sizeof lbl, "strcspn[%d]", i); PI(lbl, (long long)strcspn(a[i], "lo"));
        /* memcmp sign and memchr offset: both are silent-wrong candidates. */
        snprintf(lbl, sizeof lbl, "memcmp[%d]", i);
        int mc = memcmp(a[i], "abc", 3 < strlen(a[i]) ? 3 : strlen(a[i]) + 1);
        PI(lbl, (mc>0)-(mc<0));
        const void* mp = memchr(a[i], 'c', strlen(a[i]));
        snprintf(lbl, sizeof lbl, "memchr[%d]", i);
        PI(lbl, mp ? ((const char*)mp - a[i]) : -1);
        memset(buf, 0, sizeof buf);
        strncpy(buf, a[i], 8);
        snprintf(lbl, sizeof lbl, "strncpy8[%d]", i);
        unsigned h = 0; for (int k = 0; k < 16; ++k) h = h * 31u + (unsigned char)buf[k];
        PI(lbl, h);
    }
}

static void section_conv(void) {
    printf("== conv ==\n");
    static const char* t[] = {"0","1","-1","42","-42","2147483647","-2147483648",
                              "4294967295","0.5","-0.5","3.14159","1e10","1e-10",
                              " 12","12 ","+12","0x1A","007","abc","","1e400"};
    enum { NT = (int)(sizeof(t)/sizeof(t[0])) };
    for (int i = 0; i < NT; ++i) {
        char lbl[40];
        snprintf(lbl, sizeof lbl, "atoi[%d]", i); PI(lbl, atoi(t[i]));
        snprintf(lbl, sizeof lbl, "atol[%d]", i); PI(lbl, atol(t[i]));
        snprintf(lbl, sizeof lbl, "atof[%d]", i); PD(lbl, atof(t[i]));
        char buf[40];
        _ltoa(atol(t[i]), buf, 10);
        unsigned h = 0; for (const char* s = buf; *s; ++s) h = h*31u + (unsigned char)*s;
        snprintf(lbl, sizeof lbl, "ltoa10[%d]", i); PI(lbl, h);
        _ltoa(atol(t[i]), buf, 16);
        h = 0; for (const char* s = buf; *s; ++s) h = h*31u + (unsigned char)*s;
        snprintf(lbl, sizeof lbl, "ltoa16[%d]", i); PI(lbl, h);
    }
}

static int cmp_int(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}
static void section_alg(void) {
    printf("== alg ==\n");
    int v[32];
    for (int i = 0; i < 32; ++i) v[i] = ((i * 2654435761u) >> 8) % 1000;
    qsort(v, 32, sizeof(int), cmp_int);
    for (int i = 0; i < 32; ++i) { char l[24]; snprintf(l,sizeof l,"qsort[%d]",i); PI(l, v[i]); }
    for (int k = 0; k < 1000; k += 97) {
        int key = k;
        const int* f = (const int*)bsearch(&key, v, 32, sizeof(int), cmp_int);
        char l[24]; snprintf(l,sizeof l,"bsearch[%d]",k);
        PI(l, f ? (f - v) : -1);
    }
    /* rand() is a documented LCG in MSVC; the sequence must match exactly. */
    srand(12345);
    for (int i = 0; i < 24; ++i) { char l[24]; snprintf(l,sizeof l,"rand[%d]",i); PI(l, rand()); }
    srand(1);
    for (int i = 0; i < 8; ++i) { char l[24]; snprintf(l,sizeof l,"rand1[%d]",i); PI(l, rand()); }
    PI("abs", abs(-2147483647));
    PI("labs", labs(-123456789L));
    div_t d = div(-7, 2);   PI("div.q", d.quot); PI("div.rem", d.rem);
    ldiv_t l2 = ldiv(-7L, 2L); PI("ldiv.q", l2.quot); PI("ldiv.rem", l2.rem);
}

static void section_heap(void) {
    printf("== heap ==\n");
    /* Allocation ADDRESSES legitimately differ; alignment and the ability to
       round-trip data do not. Probe the contract, not the addresses. */
    static const size_t sizes[] = {1,2,3,4,7,8,15,16,17,31,32,64,100,255,256,
                                   1000,4096,65536};
    enum { NS = (int)(sizeof(sizes)/sizeof(sizes[0])) };
    for (int i = 0; i < NS; ++i) {
        void* p = malloc(sizes[i]);
        char l[32];
        snprintf(l,sizeof l,"malloc.nonnull[%d]",i); PI(l, p != NULL);
        snprintf(l,sizeof l,"malloc.align8[%d]",i);  PI(l, p ? (((size_t)p & 7u) == 0) : -1);
        if (p) { memset(p, 0xAB, sizes[i]); PI("", ((unsigned char*)p)[sizes[i]-1]); }
        void* q = realloc(p, sizes[i] * 2 + 1);
        snprintf(l,sizeof l,"realloc.keep[%d]",i);
        PI(l, q ? ((unsigned char*)q)[sizes[i]-1] : -1);
        free(q);
    }
    void* c = calloc(64, 4);
    int zero = 1;
    if (c) for (int i = 0; i < 256; ++i) if (((unsigned char*)c)[i]) { zero = 0; break; }
    PI("calloc.zeroed", c ? zero : -1);
    free(c);
    PI("free.null.ok", (free(NULL), 1));
}

static void section_time(void) {
    printf("== time ==\n");
    /* Wall-clock values differ per run; the CONVERSIONS must not. Feed fixed
       epochs so the output is deterministic on both sides. */
    static const long long epochs[] = {0, 1, 1000000000LL, 1234567890LL,
                                       2147483647LL, 946684800LL, 1600000000LL};
    enum { NE = (int)(sizeof(epochs)/sizeof(epochs[0])) };
    for (int i = 0; i < NE; ++i) {
        __time64_t t = (__time64_t)epochs[i];
        struct tm* g = _gmtime64(&t);
        char l[32];
        if (!g) { snprintf(l,sizeof l,"gmtime.null[%d]",i); PI(l,-1); continue; }
        snprintf(l,sizeof l,"gm.year[%d]",i); PI(l, g->tm_year);
        snprintf(l,sizeof l,"gm.mon[%d]",i);  PI(l, g->tm_mon);
        snprintf(l,sizeof l,"gm.mday[%d]",i); PI(l, g->tm_mday);
        snprintf(l,sizeof l,"gm.hour[%d]",i); PI(l, g->tm_hour);
        snprintf(l,sizeof l,"gm.min[%d]",i);  PI(l, g->tm_min);
        snprintf(l,sizeof l,"gm.sec[%d]",i);  PI(l, g->tm_sec);
        snprintf(l,sizeof l,"gm.wday[%d]",i); PI(l, g->tm_wday);
        snprintf(l,sizeof l,"gm.yday[%d]",i); PI(l, g->tm_yday);
        char sbuf[64];
        strftime(sbuf, sizeof sbuf, "%Y-%m-%d %H:%M:%S", g);
        unsigned h = 0; for (const char* s = sbuf; *s; ++s) h = h*31u + (unsigned char)*s;
        snprintf(l,sizeof l,"strftime[%d]",i); PI(l, h);
        snprintf(l,sizeof l,"difftime[%d]",i);
        __time64_t t2 = t + 3600;
        PD(l, _difftime64(t2, t));
    }
}

int main(int argc, char** argv) {
    const char* s = (argc > 1) ? argv[1] : "all";
    int all = (strcmp(s, "all") == 0);
    if (all || !strcmp(s,"math"))   section_math();
    if (all || !strcmp(s,"string")) section_string();
    if (all || !strcmp(s,"conv"))   section_conv();
    if (all || !strcmp(s,"alg"))    section_alg();
    if (all || !strcmp(s,"heap"))   section_heap();
    if (all || !strcmp(s,"time"))   section_time();
    fflush(stdout);
    return 0;
}
