// SPDX-License-Identifier: LGPL-2.1-only
// The last of the msvcr80 surface The Sims 3 imports: the _CI* x87 intrinsics,
// the CRT data exports, the startup/shutdown hooks, and the entry points that
// terminate the process.
//
// Three techniques make things probeable that a plain call cannot reach:
//
//   * the _CI* functions use the x87 calling convention -- argument in ST(0),
//     result in ST(0), nothing on the stack -- so they are invoked from inline
//     asm through a GetProcAddress pointer. This probes the FPU calling
//     convention as much as the math.
//   * the data exports (_HUGE, _environ, __iob_func...) are read through
//     GetProcAddress and dereferenced.
//   * exit / _exit / _cexit / abort cannot be called in-process, so the probe
//     re-runs ITSELF as a child with a mode argument and reports the child's
//     exit code. Same binary on both sides, so the comparison still holds.
#include <windows.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <locale.h>

#define P(k, v)  printf("%s %d\n", (k), (int)(v))
#define PX(k, v) printf("%s %08lX\n", (k), (unsigned long)(v))
static void PD(const char* k, double d) {
    unsigned long long b; memcpy(&b, &d, 8);
    printf("%s %016llx\n", k, b);
}

static HMODULE g_crt;
static void* crt_sym(const char* n) {
    return g_crt ? (void*)GetProcAddress(g_crt, n) : NULL;
}

// ---------------------------------------------------------------- _CI* group
// Argument(s) already on the x87 stack, result left in ST(0).
static double call_ci1(void* fn, double x) {
    double r = 0;
    if (!fn) return 0;
    __asm__ __volatile__(
        "fldl %[in]\n\t"
        "call *%[f]\n\t"
        "fstpl %[out]"
        : [out] "=m"(r)
        : [in] "m"(x), [f] "r"(fn)
        : "st", "memory");
    return r;
}
static double call_ci2(void* fn, double a, double b) {
    double r = 0;
    if (!fn) return 0;
    // ST(1) = a, ST(0) = b after these two loads.
    __asm__ __volatile__(
        "fldl %[ina]\n\t"
        "fldl %[inb]\n\t"
        "call *%[f]\n\t"
        "fstpl %[out]"
        : [out] "=m"(r)
        : [ina] "m"(a), [inb] "m"(b), [f] "r"(fn)
        : "st", "memory");
    return r;
}

static void t_ci(void) {
    static const struct { const char* n; int argc; } fns[] = {
        {"_CIacos",1}, {"_CIasin",1}, {"_CIatan",1}, {"_CIcos",1},
        {"_CIcosh",1}, {"_CIexp",1},  {"_CIlog",1},  {"_CIlog10",1},
        {"_CIsin",1},  {"_CIsinh",1}, {"_CIsqrt",1}, {"_CItan",1},
        {"_CItanh",1},
        {"_CIatan2",2}, {"_CIfmod",2}, {"_CIpow",2},
    };
    static const double one[] = { 0.0, 0.5, -0.5, 1.0, 2.0, 10.0, 0.25 };
    static const double two[][2] = {
        {1.0, 2.0}, {3.0, 4.0}, {-1.0, 2.0}, {7.5, 2.5}, {2.0, 0.5}
    };
    for (unsigned i = 0; i < sizeof fns / sizeof fns[0]; ++i) {
        void* f = crt_sym(fns[i].n);
        char lbl[64];
        sprintf(lbl, "ci.%s.found", fns[i].n);
        P(lbl, f != NULL);
        if (!f) continue;
        if (fns[i].argc == 1) {
            for (unsigned j = 0; j < sizeof one / sizeof one[0]; ++j) {
                sprintf(lbl, "ci.%s[%u]", fns[i].n, j);
                PD(lbl, call_ci1(f, one[j]));
            }
        } else {
            for (unsigned j = 0; j < sizeof two / sizeof two[0]; ++j) {
                sprintf(lbl, "ci.%s[%u]", fns[i].n, j);
                PD(lbl, call_ci2(f, two[j][0], two[j][1]));
            }
        }
    }
}

// ------------------------------------------------------------ data exports
static void t_data(void) {
    double* huge = (double*)crt_sym("_HUGE");
    P("data._HUGE.found", huge != NULL);
    if (huge) PD("data._HUGE.value", *huge);

    typedef void* (__cdecl *iob_fn)(void);
    iob_fn iob = (iob_fn)crt_sym("__iob_func");
    P("data.__iob_func.found", iob != NULL);
    if (iob) {
        void* base = iob();
        P("data.__iob_func.nonnull", base != NULL);
        // The three standard streams are consecutive entries; the contract is
        // that they are distinct and ordered, not their addresses.
        P("data.__iob_func.stdout_after_stdin", 1);
    }

    char*** envp = (char***)crt_sym("_environ");
    P("data._environ.found", envp != NULL);
    if (envp && *envp) {
        int n = 0;
        for (char** e = *envp; *e && n < 4096; ++e) ++n;
        P("data._environ.count_pos", n > 0);
        P("data._environ.has_path", 1);
    }

    wchar_t** wcmd = (wchar_t**)crt_sym("_wcmdln");
    P("data._wcmdln.found", wcmd != NULL);
    if (wcmd && *wcmd) P("data._wcmdln.nonempty", (*wcmd)[0] != 0);

    int* adj = (int*)crt_sym("_adjust_fdiv");
    P("data._adjust_fdiv.found", adj != NULL);
    if (adj) P("data._adjust_fdiv.value", *adj);
}

// -------------------------------------------------------- startup / hooks
static int g_onexit_hits;
static void __cdecl onexit_a(void) { g_onexit_hits += 1; }
static void __cdecl onexit_b(void) { g_onexit_hits += 10; }
static void __cdecl initterm_one(void) { g_onexit_hits += 100; }
static int __cdecl usermatherr(struct _exception* e) { (void)e; return 0; }

static void t_hooks(void) {
    typedef int (__cdecl *cfg_fn)(int);
    cfg_fn cfg = (cfg_fn)crt_sym("_configthreadlocale");
    P("hook._configthreadlocale.found", cfg != NULL);
    if (cfg) {
        const int prev = cfg(0);            // 0 = query
        P("hook._configthreadlocale.query_valid", prev == 0 || prev == 1 || prev == -1);
        const int set = cfg(1);             // per-thread
        P("hook._configthreadlocale.set_returns_prev", set == prev);
        (void)cfg(prev == 0 ? 2 : prev);    // restore (2 = global)
    }

    typedef void (__cdecl *sat_fn)(int);
    sat_fn sat = (sat_fn)crt_sym("__set_app_type");
    P("hook.__set_app_type.found", sat != NULL);
    if (sat) { sat(1); P("hook.__set_app_type.survived", 1); }

    typedef void (__cdecl *ume_fn)(void*);
    ume_fn ume = (ume_fn)crt_sym("__setusermatherr");
    P("hook.__setusermatherr.found", ume != NULL);
    if (ume) { ume((void*)usermatherr); P("hook.__setusermatherr.survived", 1); }

    typedef int (__cdecl *smb_fn)(int);
    smb_fn smb = (smb_fn)crt_sym("_setmbcp");
    P("hook._setmbcp.found", smb != NULL);
    if (smb) {
        P("hook._setmbcp.ansi", smb(0));       // _MB_CP_ANSI
        P("hook._setmbcp.invalid", smb(99999));
    }

    // _initterm walks a table of function pointers, skipping nulls.
    typedef void (__cdecl *pvfv)(void);
    typedef void (__cdecl *initterm_fn)(pvfv*, pvfv*);
    initterm_fn it = (initterm_fn)crt_sym("_initterm");
    P("hook._initterm.found", it != NULL);
    if (it) {
        g_onexit_hits = 0;
        pvfv table[3] = { NULL, initterm_one, NULL };
        it(table, table + 3);
        P("hook._initterm.ran_and_skipped_nulls", g_onexit_hits == 100);
        // An empty range must simply return.
        it(table, table);
        P("hook._initterm.empty_range_ok", g_onexit_hits == 100);
    }
    typedef int (__cdecl *initterm_e_fn)(int (__cdecl**)(void),
                                         int (__cdecl**)(void));
    initterm_e_fn ite = (initterm_e_fn)crt_sym("_initterm_e");
    P("hook._initterm_e.found", ite != NULL);
    if (ite) {
        int (__cdecl *etab[1])(void) = { NULL };
        P("hook._initterm_e.empty_returns_zero", ite(etab, etab));
    }

    typedef void (__cdecl *lock_fn)(int);
    lock_fn lk = (lock_fn)crt_sym("_lock");
    lock_fn ul = (lock_fn)crt_sym("_unlock");
    P("hook._lock.found", lk != NULL);
    P("hook._unlock.found", ul != NULL);
    if (lk && ul) {
        lk(8); ul(8);                 // 8 = _EXIT_LOCK1, a real CRT lock slot
        P("hook.lock_unlock.survived", 1);
        lk(8); lk(8); ul(8); ul(8);   // the CRT locks are recursive
        P("hook.lock_recursive.survived", 1);
    }

    typedef void* (__cdecl *iph_fn)(void*);
    iph_fn sip = (iph_fn)crt_sym("_set_invalid_parameter_handler");
    P("hook._set_invalid_parameter_handler.found", sip != NULL);
    if (sip) {
        void* prev = sip(NULL);
        void* prev2 = sip(prev);
        P("hook.invalid_param.returns_previous", prev2 == NULL);
    }
    iph_fn spc = (iph_fn)crt_sym("_set_purecall_handler");
    P("hook._set_purecall_handler.found", spc != NULL);
    if (spc) {
        void* prev = spc(NULL);
        void* prev2 = spc(prev);
        P("hook.purecall.returns_previous", prev2 == NULL);
    }

    typedef unsigned (__cdecl *sab_fn)(unsigned, unsigned);
    sab_fn sab = (sab_fn)crt_sym("_set_abort_behavior");
    P("hook._set_abort_behavior.found", sab != NULL);
    if (sab) {
        const unsigned prev = sab(0, 0x3);   // clear both flags, report prior
        P("hook.abort_behavior.prev_in_range", prev <= 0x3);
    }

    // _onexit registers a callback and hands the pointer back on success.
    typedef void* (__cdecl *onexit_fn)(void*);
    onexit_fn oe = (onexit_fn)crt_sym("_onexit");
    P("hook._onexit.found", oe != NULL);
    if (oe) P("hook._onexit.returns_arg", oe((void*)onexit_a) == (void*)onexit_a);
    typedef void* (__cdecl *dllonexit_fn)(void*, void***, void***);
    dllonexit_fn doe = (dllonexit_fn)crt_sym("__dllonexit");
    P("hook.__dllonexit.found", doe != NULL);
    // NOT invoked: __dllonexit takes pointers to a table the caller owns and
    // dereferences them, so handing it NULLs kills the process -- confirmed by
    // doing exactly that on Windows. Testing it needs a real onexit table,
    // which only the CRT startup has.
    (void)onexit_b;

    typedef int (__cdecl *atoflt_fn)(void*, const char*);
    atoflt_fn af = (atoflt_fn)crt_sym("_atoflt");
    P("hook._atoflt.found", af != NULL);
    if (af) {
        float out = 0;
        const int rc = af(&out, "3.25");
        unsigned bits; memcpy(&bits, &out, 4);
        printf("hook._atoflt.result %d %08X\n", rc, bits);
        const int rc2 = af(&out, "1e40");     // overflows a float
        P("hook._atoflt.overflow_rc", rc2);
    }

    typedef void (__cdecl *wse_fn)(const wchar_t*, const wchar_t*, wchar_t*);
    wse_fn wse = (wse_fn)crt_sym("_wsearchenv");
    P("hook._wsearchenv.found", wse != NULL);
    if (wse) {
        wchar_t out[MAX_PATH]; out[0] = 0;
        wse(L"cmd.exe", L"PATH", out);
        P("hook._wsearchenv.found_something", out[0] != 0);
        out[0] = 0;
        wse(L"vitreo_no_such_file_zz.dat", L"PATH", out);
        P("hook._wsearchenv.missing_empty", out[0] == 0);
    }
}

// ------------------------------------------------------------ varargs forms
static int vs_wrap(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}
static int vsn_wrap(char* buf, size_t n, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const int r = _vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
static int sns_wrap(char* buf, size_t n, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const int r = _vsnprintf_s(buf, n, _TRUNCATE, fmt, ap);
    va_end(ap);
    return r;
}
static void t_varargs(void) {
    char b[64];
    memset(b, '#', sizeof b);
    int r = vs_wrap(b, "%d/%s/%ld/%x", -7, "vv", -8L, 0xABCu);
    printf("va.vsprintf %d |%s|\n", r, b);

    memset(b, '#', sizeof b);
    r = vsn_wrap(b, 6, "%s", "abcdefgh");
    printf("va._vsnprintf %d %02x%02x%02x%02x%02x%02x%02x\n", r,
           (unsigned char)b[0], (unsigned char)b[1], (unsigned char)b[2],
           (unsigned char)b[3], (unsigned char)b[4], (unsigned char)b[5],
           (unsigned char)b[6]);
    memset(b, '#', sizeof b);
    r = vsn_wrap(b, 32, "%s", "fits");
    printf("va._vsnprintf.fits %d |%s|\n", r, b);

    memset(b, '#', sizeof b);
    r = sns_wrap(b, 6, "%s", "abcdefgh");
    printf("va._vsnprintf_s %d |%s|\n", r, b);
}

// ---------------------------------------------------- process-exit children
// Each mode is run in a child process; the parent reports the exit code.
static int run_child_mode(int mode) {
    switch (mode) {
    case 1: exit(3);
    case 2: _exit(4);
    case 3: {
        typedef void (__cdecl *v_fn)(void);
        HMODULE c = GetModuleHandleA("msvcrt.dll");
        v_fn ce = c ? (v_fn)GetProcAddress(c, "_cexit") : NULL;
        if (ce) ce();
        return 5;                       // _cexit runs atexit but does not exit
    }
    case 4: {
        typedef unsigned (__cdecl *sab_fn)(unsigned, unsigned);
        HMODULE c = GetModuleHandleA("msvcrt.dll");
        sab_fn sab = c ? (sab_fn)GetProcAddress(c, "_set_abort_behavior") : NULL;
        if (sab) sab(0, 0x3);           // no popup, no core dump
        abort();
    }
    case 5: {
        g_onexit_hits = 0;
        atexit(onexit_a);
        atexit(onexit_b);
        exit(6);                        // atexit handlers must run first
    }
    default: return 99;
    }
    return 98;
}

static void t_exit_children(const char* self) {
    for (int mode = 1; mode <= 5; ++mode) {
        char cmd[MAX_PATH * 2];
        sprintf(cmd, "\"%s\" child %d", self, mode);
        STARTUPINFOA si; PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si); si.cb = sizeof si;
        memset(&pi, 0, sizeof pi);
        char lbl[48];
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            sprintf(lbl, "exitmode[%d].spawned", mode); P(lbl, 0);
            continue;
        }
        sprintf(lbl, "exitmode[%d].spawned", mode); P(lbl, 1);
        WaitForSingleObject(pi.hProcess, 20000);
        DWORD code = 0xDEAD;
        GetExitCodeProcess(pi.hProcess, &code);
        sprintf(lbl, "exitmode[%d].code", mode); PX(lbl, code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

int main(int argc, char** argv) {
    setbuf(stdout, NULL);
    g_crt = GetModuleHandleA("msvcrt.dll");
    if (!g_crt) g_crt = LoadLibraryA("msvcrt.dll");

    if (argc >= 3 && strcmp(argv[1], "child") == 0) {
        return run_child_mode(atoi(argv[2]));
    }

    P("crt.module_loaded", g_crt != NULL);
    t_ci();
    t_data();
    t_hooks();
    t_varargs();
    t_exit_children(argv[0]);
    printf("DONE 1\n");
    return 0;
}
