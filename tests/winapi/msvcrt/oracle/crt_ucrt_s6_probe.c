// UCRT _crt_at_quick_exit / quick_exit, _wcsicoll, strtok_s,
// __stdio_common_vfwscanf and qsort_s probe.
//
// Measures, on real Windows ucrtbase.dll (x86 and x64): return values, errno,
// invalid-parameter-handler calls (and errno as the handler saw it), outputs
// and side effects of each documented case. quick_exit cases run in child
// processes (the probe re-runs itself with a mode argument) and report the
// order the registered functions ran in plus the exit code.
//
//   i686-w64-mingw32-gcc   -O0 -o ucrt_s6_32.exe crt_ucrt_s6_probe.c
//   x86_64-w64-mingw32-gcc -O0 -o ucrt_s6_64.exe crt_ucrt_s6_probe.c
//   ssh winoracle 'C:\vprobe\ucrt_s6_32.exe' > crt_ucrt_s6_probe32.expected
//   ssh winoracle 'C:\vprobe\ucrt_s6_64.exe' > crt_ucrt_s6_probe64.expected
//   python3 gen_crt_ucrt_s6_oracle.py   (writes ../crt_ucrt_s6_oracle.inc)
//
// Row formats (tab-separated, hex):
//   q <mode> <child output> <exit code>
//   i <case> <ret> <errno> <iph_calls> <iph_errno>
//   t <case> <step> <ret offset|ff> <context offset|ffff> <errno> <iph_calls> <iph_errno> <buffer bytes...>
//   w <case> <ret> <errno> <iph_calls> <iph_errno> <v0> <v1> <v2> <v3> <next fgetc>
//   s <case> <errno> <iph_calls> <iph_errno> <cmp_calls> <cmp_bad> <elements...>
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef void (__cdecl *IPH)(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t);
typedef IPH (__cdecl *SetIPH)(IPH);
typedef int* (__cdecl *ErrnoFn)(void);
typedef void (__cdecl *PVFV)(void);
typedef int (__cdecl *RegFn)(PVFV);
typedef void (__cdecl *QuickExit)(int);
typedef void (__cdecl *ExitFn)(int);
typedef int (__cdecl *Wcsicoll)(const wchar_t*, const wchar_t*);
typedef char* (__cdecl *StrtokS)(char*, const char*, char**);
typedef int (__cdecl *VfwscanfFn)(unsigned long long, void*, const wchar_t*, void*, va_list);
typedef void* (__cdecl *FopenFn)(const char*, const char*);
typedef int (__cdecl *FcloseFn)(void*);
typedef int (__cdecl *FgetcFn)(void*);
typedef void* (__cdecl *IobFn)(unsigned);
typedef int (__cdecl *FputsFn)(const char*, void*);
typedef int (__cdecl *SetvbufFn)(void*, char*, int, size_t);
typedef int (__cdecl *Cmp3)(void*, const void*, const void*);
typedef void (__cdecl *QsortS)(void*, size_t, size_t, Cmp3, void*);

static HANDLE out;
static HMODULE ucrt;
static ErrnoFn err;
static unsigned iph_calls, iph_errno;
static void put(const char* s) { DWORD n; WriteFile(out, s, lstrlenA(s), &n, 0); }
static void hex(uint64_t n) { char b[17]; int i = 16; b[i] = 0; do { b[--i] = "0123456789abcdef"[n & 15]; n >>= 4; } while (n); put(b + i); }
static void tab(uint64_t n) { put("\t"); hex(n); }
static void __cdecl handler(const wchar_t* e, const wchar_t* f, const wchar_t* fi, unsigned l, uintptr_t r) {
    (void)e; (void)f; (void)fi; (void)l; (void)r; ++iph_calls; iph_errno = (unsigned)*err();
}
static void reset(void) { iph_calls = 0; iph_errno = 0; *err() = 0x1234; }
static FARPROC fn(const char* name) { return GetProcAddress(ucrt, name); }

// ---- quick_exit children ------------------------------------------------------
static RegFn reg_quick, reg_atexit;
static void __cdecl qa(void) { put("A"); }
static void __cdecl qb(void) { put("B"); }
static void __cdecl qc(void) { put("C"); }
static void __cdecl qd(void) { put("D"); }
static void __cdecl qe(void) { put("E"); }
static void __cdecl qf(void) { put("F"); }
static void __cdecl q_registers_e(void) { put("R"); hex((unsigned)reg_quick(qe)); }

static int child(const char* mode) {
    QuickExit quick_exit_ = (QuickExit)fn("quick_exit");
    ExitFn exit_ = (ExitFn)fn("exit");
    int i;
    reg_quick = (RegFn)fn("_crt_at_quick_exit");
    reg_atexit = (RegFn)fn("_crt_atexit");
    if (!strcmp(mode, "lifo")) {
        // at_quick_exit functions run LIFO; atexit functions do not run.
        hex((unsigned)reg_quick(qa)); hex((unsigned)reg_quick(qb)); hex((unsigned)reg_quick(qc));
        hex((unsigned)reg_atexit(qd)); put(":");
        quick_exit_(7);
    } else if (!strcmp(mode, "nested")) {
        // A function registered while quick_exit is running.
        hex((unsigned)reg_quick(qa)); hex((unsigned)reg_quick(q_registers_e)); hex((unsigned)reg_quick(qb));
        put(":");
        quick_exit_(9);
    } else if (!strcmp(mode, "null")) {
        // _crt_at_quick_exit(NULL), then a real one, then quick_exit.
        hex((unsigned)reg_quick(0)); hex((unsigned)reg_quick(qa)); put(":");
        quick_exit_(5);
    } else if (!strcmp(mode, "exit")) {
        // exit() runs atexit functions, not at_quick_exit ones.
        hex((unsigned)reg_quick(qa)); hex((unsigned)reg_atexit(qd)); put(":");
        exit_(3);
    } else if (!strcmp(mode, "many")) {
        // More registrations than any initial table size.
        for (i = 0; i < 40; ++i) if (reg_quick(i == 0 ? qf : qe) != 0) put("!");
        put(":");
        quick_exit_(4);
    } else if (!strcmp(mode, "flush")) {
        // quick_exit ends with _Exit: buffered stream output is not flushed.
        void* so = ((IobFn)fn("__acrt_iob_func"))(1);
        ((SetvbufFn)fn("setvbuf"))(so, 0, 0 /*_IOFBF*/, 512);
        ((FputsFn)fn("fputs"))("buffered", so);
        hex((unsigned)reg_quick(qa)); put(":");
        quick_exit_(6);
    } else if (!strcmp(mode, "zero")) {
        put(":");
        quick_exit_(0);
    }
    return 0x77;
}

static void run_child(const char* self, const char* mode) {
    char cmd[MAX_PATH * 2];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), 0, TRUE};
    HANDLE rd, wr;
    DWORD code = 0xFFFFFFFF, n;
    char buf[256];
    DWORD total = 0;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    wsprintfA(cmd, "\"%s\" %s", self, mode);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(wr);
        while (total < sizeof(buf) - 1 && ReadFile(rd, buf + total, sizeof(buf) - 1 - total, &n, 0) && n)
            total += n;
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        CloseHandle(wr);
    }
    CloseHandle(rd);
    buf[total] = 0;
    put("q\t"); put(mode); put("\t"); put(total ? buf : "-"); tab(code); put("\n");
}

// ---- _wcsicoll ---------------------------------------------------------------
static const wchar_t* const coll[][2] = {
    {L"abc", L"ABC"}, {L"a", L"b"}, {L"b", L"a"}, {L"a", L"c"}, {L"A", L"c"},
    {L"[", L"a"}, {L"a", L"["}, {L"_", L"A"}, {L"abc", L"abcd"}, {L"abcd", L"abc"},
    {L"", L""}, {L"", L"a"}, {L"\x00e9", L"\x00c9"}, {L"\x0101", L"\x0100"},
    {L"z", L"\x00e9"}, {L"\x00e9", L"z"}, {L"Z", L"a"}, {L"@", L"`"},
    {L"\x0130", L"i"}, {L"\xff21", L"\xff41"}, {L"\xd800", L"\xffff"}, {L"Hello", L"hELLo"},
    {0, L"a"}, {L"a", 0}, {0, 0},
};

// ---- strtok_s ----------------------------------------------------------------
// A tokenizing sequence: str is the buffer on step 0 and NULL afterwards.
struct TokSeq { const char* text; const char* delims; unsigned steps; };
static const struct TokSeq seqs[] = {
    {"  ab,,cd e ", " ,", 5}, {"", " ", 2}, {",,,", ",", 2}, {"abc", "", 2},
    {"a b", " ", 3}, {"x", "x", 2},
};

// ---- __stdio_common_vfwscanf -----------------------------------------------------
static VfwscanfFn vfwscanf_;
static int scan(unsigned long long opts, void* stream, const wchar_t* format, ...) {
    va_list ap; int r;
    va_start(ap, format);
    r = vfwscanf_(opts, stream, format, 0, ap);
    va_end(ap);
    return r;
}

static char tmpdir[MAX_PATH];
static void write_file(const char* name, const char* bytes, DWORD n) {
    char path[MAX_PATH]; DWORD w;
    HANDLE f;
    wsprintfA(path, "%s%s", tmpdir, name);
    f = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    WriteFile(f, bytes, n, &w, 0);
    CloseHandle(f);
}

// ---- qsort_s -----------------------------------------------------------------
static int ctx_obj;
static int* sort_base;
static size_t sort_count, sort_width;
static unsigned cmp_calls, cmp_bad;
static int __cdecl cmp3(void* ctx, const void* a, const void* b) {
    ++cmp_calls;
    if (ctx != &ctx_obj) cmp_bad |= 1;
    if ((const char*)a < (const char*)sort_base || (const char*)a >= (const char*)sort_base + sort_count * sort_width ||
        (const char*)b < (const char*)sort_base || (const char*)b >= (const char*)sort_base + sort_count * sort_width)
        cmp_bad |= 2;
    if (sort_width == 3) return memcmp(a, b, 3);
    return *(const int*)a < *(const int*)b ? -1 : *(const int*)a > *(const int*)b;
}

int main(int argc, char** argv) {
    unsigned c, s, i;
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    ucrt = LoadLibraryA("ucrtbase.dll");
    err = (ErrnoFn)fn("_errno");
    ((SetIPH)fn("_set_invalid_parameter_handler"))(handler);
    if (argc > 1) return child(argv[1]);

    run_child(argv[0], "lifo");
    run_child(argv[0], "nested");
    run_child(argv[0], "null");
    run_child(argv[0], "exit");
    run_child(argv[0], "many");
    run_child(argv[0], "flush");
    run_child(argv[0], "zero");

    {
        Wcsicoll wcsicoll_ = (Wcsicoll)fn("_wcsicoll");
        for (c = 0; c < sizeof(coll) / sizeof(coll[0]); ++c) {
            int r;
            reset();
            r = wcsicoll_(coll[c][0], coll[c][1]);
            put("i"); tab(c); tab((unsigned)r); tab((unsigned)*err()); tab(iph_calls); tab(iph_errno); put("\n");
        }
    }

    {
        StrtokS strtok_s_ = (StrtokS)fn("strtok_s");
        for (c = 0; c < sizeof(seqs) / sizeof(seqs[0]); ++c) {
            char buf[16];
            char* ctx = 0;
            size_t len = strlen(seqs[c].text);
            memset(buf, 0xcc, sizeof(buf));
            memcpy(buf, seqs[c].text, len + 1);
            for (s = 0; s < seqs[c].steps; ++s) {
                char* r;
                reset();
                r = strtok_s_(s == 0 ? buf : 0, seqs[c].delims, &ctx);
                put("t"); tab(c); tab(s); tab(r ? (uint64_t)(r - buf) : 0xff);
                tab(ctx ? (uint64_t)(ctx - buf) : 0xffff); tab((unsigned)*err()); tab(iph_calls); tab(iph_errno);
                for (i = 0; i < len + 1; ++i) tab((unsigned char)buf[i]);
                put("\n");
            }
        }
        // Error cases: 0 (NULL, d, &NULL), 1 (s, NULL, &ctx), 2 (s, d, NULL),
        // 3 (NULL, d, NULL), 4 (NULL, NULL, &ctx valid), 5 (NULL, d, &ctx valid),
        // 6 (s, d, &ctx valid: str restarts).
        for (c = 0; c < 7; ++c) {
            char buf[8] = "p q";
            char other[8] = "x y";
            char* ctx = c >= 4 ? other + 2 : 0;
            char* r;
            reset();
            switch (c) {
            case 0: r = strtok_s_(0, " ", &ctx); break;
            case 1: r = strtok_s_(buf, 0, &ctx); break;
            case 2: r = strtok_s_(buf, " ", 0); break;
            case 3: r = strtok_s_(0, " ", 0); break;
            case 4: r = strtok_s_(0, 0, &ctx); break;
            case 5: r = strtok_s_(0, " ", &ctx); break;
            default: r = strtok_s_(buf, " ", &ctx); break;
            }
            put("t"); tab(0x10 + c); tab(0);
            tab(!r ? 0xff : (r >= buf && r < buf + 8) ? (uint64_t)(r - buf) : 0x100 + (uint64_t)(r - other));
            tab(!ctx ? 0xffff : (ctx >= buf && ctx < buf + 8) ? (uint64_t)(ctx - buf) : 0x100 + (uint64_t)(ctx - other));
            tab((unsigned)*err()); tab(iph_calls); tab(iph_errno);
            for (i = 0; i < 4; ++i) tab((unsigned char)buf[i]);
            put("\n");
        }
    }

    {
        FopenFn fopen_ = (FopenFn)fn("fopen");
        FcloseFn fclose_ = (FcloseFn)fn("fclose");
        FgetcFn fgetc_ = (FgetcFn)fn("fgetc");
        struct W { const char* bytes; unsigned n; const char* mode; unsigned long long opts; const wchar_t* format; unsigned kind; };
        // kind: 0 = %d %ls %lc, 1 = three %lc, 2 = %lc %d, 3 = %d %lc, 4 = %d, 5 = %s (buffer),
        // 6 = %s secure (size 2), 7 = %ls%n, 8 = %d%d
        static const struct W wc[] = {
            {"12 abc x\r\n34", 12, "r", 0, L"%d %ls %lc", 0},
            {"\x80\xe9z", 3, "r", 0, L"%lc%lc%lc", 1},
            {"ab12", 4, "rb", 0, L"%lc%d", 2},
            {"\xc3\xa9 5", 4, "r, ccs=UTF-8", 0, L"%lc %d", 2},
            {"5\0 \0x\0", 6, "r, ccs=UTF-16LE", 0, L"%d %lc", 3},
            {"42abc", 5, "r", 0, L"%d", 4},
            {"abc", 3, "r", 0, L"%s", 5},
            {"abc", 3, "r", 2, L"%s", 5},
            {"abc", 3, "r", 1, L"%s", 6},
            {"", 0, "r", 0, L"%d", 4},
            {"   ", 3, "r", 0, L"%d", 4},
            {"ab cd", 5, "r", 0, L"%ls%n", 7},
            {"1\r\n2", 4, "r", 0, L"%d%d", 8},
            {"1\r\n2", 4, "rb", 0, L"%d%d", 8},
            {"\x1a" "5", 2, "r", 0, L"%d", 4},
            {"7\r", 2, "r", 0, L"%d%lc", 2},
        };
        vfwscanf_ = (VfwscanfFn)fn("__stdio_common_vfwscanf");
        GetTempPathA(MAX_PATH, tmpdir);
        for (c = 0; c < sizeof(wc) / sizeof(wc[0]); ++c) {
            char path[MAX_PATH];
            void* f;
            int r, next;
            uint64_t v[4] = {0, 0, 0, 0};
            unsigned short ws[8]; unsigned char nb[8];
            int iv = 0x5a5a, iv2 = 0x5a5a; unsigned short wch = 0x5a5a; int n = 0x5a5a;
            memset(ws, 0xcc, sizeof(ws)); memset(nb, 0xcc, sizeof(nb));
            write_file("vprobe_s6.txt", wc[c].bytes, wc[c].n);
            wsprintfA(path, "%svprobe_s6.txt", tmpdir);
            f = fopen_(path, wc[c].mode);
            reset();
            switch (wc[c].kind) {
            case 0: r = scan(wc[c].opts, f, wc[c].format, &iv, ws, &wch);
                v[0] = (unsigned)iv; v[1] = ws[0] | (uint64_t)ws[1] << 16 | (uint64_t)ws[2] << 32 | (uint64_t)ws[3] << 48; v[2] = wch; break;
            case 1: { unsigned short a = 0x5a5a, b = 0x5a5a, d = 0x5a5a;
                r = scan(wc[c].opts, f, wc[c].format, &a, &b, &d); v[0] = a; v[1] = b; v[2] = d; break; }
            case 2: r = scan(wc[c].opts, f, wc[c].format, &wch, &iv); v[0] = wch; v[1] = (unsigned)iv; break;
            case 3: r = scan(wc[c].opts, f, wc[c].format, &iv, &wch); v[0] = (unsigned)iv; v[1] = wch; break;
            case 4: r = scan(wc[c].opts, f, wc[c].format, &iv); v[0] = (unsigned)iv; break;
            case 5: r = scan(wc[c].opts, f, wc[c].format, nb);
                for (i = 0; i < 8; ++i) v[i / 4] |= (uint64_t)nb[i] << (16 * (i % 4));
                break;
            case 6: r = scan(wc[c].opts, f, wc[c].format, ws, (unsigned)2);
                v[0] = ws[0] | (uint64_t)ws[1] << 16 | (uint64_t)ws[2] << 32 | (uint64_t)ws[3] << 48; break;
            case 7: r = scan(wc[c].opts, f, wc[c].format, ws, &n);
                v[0] = ws[0] | (uint64_t)ws[1] << 16 | (uint64_t)ws[2] << 32 | (uint64_t)ws[3] << 48; v[1] = (unsigned)n; break;
            default: r = scan(wc[c].opts, f, wc[c].format, &iv, &iv2); v[0] = (unsigned)iv; v[1] = (unsigned)iv2; break;
            }
            {
                unsigned e = (unsigned)*err(), calls = iph_calls, ie = iph_errno;
                next = fgetc_(f);
                fclose_(f);
                put("w"); tab(c); tab((unsigned)r); tab(e); tab(calls); tab(ie);
                tab(v[0]); tab(v[1]); tab(v[2]); tab(v[3]); tab((unsigned)next); put("\n");
            }
        }
        // NULL stream, NULL format.
        reset();
        {
            int iv = 0;
            int r = scan(0, 0, L"%d", &iv);
            put("w"); tab(0x20); tab((unsigned)r); tab((unsigned)*err()); tab(iph_calls); tab(iph_errno);
            tab(0); tab(0); tab(0); tab(0); tab(0); put("\n");
        }
        {
            char path[MAX_PATH];
            void* f;
            int r;
            write_file("vprobe_s6.txt", "1", 1);
            wsprintfA(path, "%svprobe_s6.txt", tmpdir);
            f = fopen_(path, "r");
            reset();
            r = scan(0, f, 0);
            put("w"); tab(0x21); tab((unsigned)r); tab((unsigned)*err()); tab(iph_calls); tab(iph_errno);
            tab(0); tab(0); tab(0); tab(0); tab((unsigned)fgetc_(f)); put("\n");
            fclose_(f);
        }
    }

    {
        QsortS qsort_s_ = (QsortS)fn("qsort_s");
        // Validation: base NULL?, number, width, compare NULL?
        struct V { unsigned null_base; size_t number, width; unsigned null_cmp; };
        static const struct V vc[] = {
            {1, 3, 4, 0}, {1, 0, 4, 0}, {0, 3, 0, 0}, {0, 3, 4, 1}, {0, 0, 0, 0},
            {0, 1, 4, 1}, {0, 0, 4, 1}, {1, 0, 0, 1}, {0, 1, 0, 0}, {0, 0, 0, 1},
        };
        int arr[8];
        for (c = 0; c < sizeof(vc) / sizeof(vc[0]); ++c) {
            static const int init[] = {3, 1, 2};
            memcpy(arr, init, sizeof(init));
            sort_base = arr; sort_count = vc[c].number; sort_width = vc[c].width ? vc[c].width : 4;
            cmp_calls = cmp_bad = 0;
            reset();
            qsort_s_(vc[c].null_base ? 0 : arr, vc[c].number, vc[c].width, vc[c].null_cmp ? 0 : cmp3, &ctx_obj);
            put("s"); tab(c); tab((unsigned)*err()); tab(iph_calls); tab(iph_errno); tab(cmp_calls); tab(cmp_bad);
            for (i = 0; i < 3; ++i) tab((unsigned)arr[i]);
            put("\n");
        }
        // Sorting: ints (with duplicates), 3-byte records, one element.
        {
            int ints[] = {5, 3, 9, 1, 7, 3, -4, 0x7fffffff, (int)0x80000000};
            sort_base = ints; sort_count = 9; sort_width = 4;
            cmp_calls = cmp_bad = 0;
            reset();
            qsort_s_(ints, 9, 4, cmp3, &ctx_obj);
            put("s"); tab(0x20); tab((unsigned)*err()); tab(iph_calls); tab(iph_errno); tab(cmp_calls != 0); tab(cmp_bad);
            for (i = 0; i < 9; ++i) tab((unsigned)ints[i]);
            put("\n");
        }
        {
            unsigned char rec[15] = {'m','o','p', 'a','b','c', 'z','z','z', 'a','b','b', 'q','r','s'};
            sort_base = (int*)rec; sort_count = 5; sort_width = 3;
            cmp_calls = cmp_bad = 0;
            reset();
            qsort_s_(rec, 5, 3, cmp3, &ctx_obj);
            put("s"); tab(0x21); tab((unsigned)*err()); tab(iph_calls); tab(iph_errno); tab(cmp_calls != 0); tab(cmp_bad);
            for (i = 0; i < 15; ++i) tab(rec[i]);
            put("\n");
        }
        {
            int one[] = {42};
            sort_base = one; sort_count = 1; sort_width = 4;
            cmp_calls = cmp_bad = 0;
            reset();
            qsort_s_(one, 1, 4, cmp3, &ctx_obj);
            put("s"); tab(0x22); tab((unsigned)*err()); tab(iph_calls); tab(iph_errno); tab(cmp_calls); tab(cmp_bad);
            tab((unsigned)one[0]);
            put("\n");
        }
    }
    return 0;
}
