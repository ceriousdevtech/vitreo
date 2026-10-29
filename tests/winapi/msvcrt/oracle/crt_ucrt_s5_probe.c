// UCRT secure conversion / bsearch_s / fmin-family probe.
//
// Measures, on real Windows ucrtbase.dll (x86 and x64), every documented case
// of mbstowcs_s, mbsrtowcs_s, wcsrtombs_s and bsearch_s -- return value, errno,
// invalid-parameter-handler calls, *pReturnValue, the destination buffer, the
// restart pointer and mbstate -- and fmin/fminf/fminl/fmaxf/fmaxl result bits
// plus _statusfp for the special-value matrix used by crt_compare_probe.
//
//   i686-w64-mingw32-gcc   -O0 -o ucrt_s5_32.exe crt_ucrt_s5_probe.c
//   x86_64-w64-mingw32-gcc -O0 -o ucrt_s5_64.exe crt_ucrt_s5_probe.c
//   ssh winoracle ./ucrt_s5_32.exe > crt_ucrt_s5_probe32.expected
//   ssh winoracle ./ucrt_s5_64.exe > crt_ucrt_s5_probe64.expected
//   python3 gen_crt_ucrt_s5_oracle.py   (writes ../crt_ucrt_s5_oracle.inc)
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

typedef void (__cdecl *IPH)(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t);
typedef IPH (__cdecl *SetIPH)(IPH);
typedef int* (__cdecl *ErrnoFn)(void);
typedef int (__cdecl *MbsToWcsS)(size_t*, wchar_t*, size_t, const char*, size_t);
typedef int (__cdecl *MbsrToWcsS)(size_t*, wchar_t*, size_t, const char**, size_t, void*);
typedef int (__cdecl *WcsrToMbsS)(size_t*, char*, size_t, const wchar_t**, size_t, void*);
typedef int (__cdecl *Cmp3)(void*, const void*, const void*);
typedef void* (__cdecl *BsearchS)(const void*, const void*, size_t, size_t, Cmp3, void*);
typedef double (__cdecl *BinD)(double, double);
typedef float (__cdecl *BinF)(float, float);
typedef unsigned (__cdecl *FpStatus)(void);

static HANDLE out;
static ErrnoFn err;
static unsigned iph_calls, iph_errno;
static void put(const char* s) { DWORD n; WriteFile(out, s, lstrlenA(s), &n, 0); }
static void hex(uint64_t n) { char b[17]; int i = 16; b[i] = 0; do { b[--i] = "0123456789abcdef"[n & 15]; n >>= 4; } while (n); put(b + i); }
static void tab(uint64_t n) { put("\t"); hex(n); }
static void __cdecl handler(const wchar_t* e, const wchar_t* f, const wchar_t* fi, unsigned l, uintptr_t r) {
    (void)e; (void)f; (void)fi; (void)l; (void)r; ++iph_calls; iph_errno = (unsigned)*err();
}
static void reset(void) { iph_calls = 0; iph_errno = 0; *err() = 0x1234; SetLastError(0xdead); }

#define T ((size_t)-1)
// dst: 0 = NULL, 1 = buffer. src: 0..3 string index, 4 = NULL string,
// 5 = NULL indirect pointer (restartable forms only).
// The table is mirrored in gen_crt_ucrt_s5_oracle.py; keep the two in step.
struct Case { unsigned dst; size_t size; unsigned src; size_t count; unsigned null_ret; };
static const struct Case cases[] = {
    {1, 8, 0, T, 0}, {1, 8, 0, 8, 0}, {1, 8, 0, 2, 0}, {1, 5, 0, 4, 0}, {1, 4, 0, 4, 0},
    {1, 4, 0, T, 0}, {1, 4, 0, 3, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 2, 0}, {0, 5, 0, 2, 0},
    {1, 0, 0, 2, 0}, {1, 8, 4, 2, 0}, {1, 8, 1, 8, 0}, {1, 8, 2, 8, 0}, {1, 1, 0, T, 0},
    {1, 1, 1, 0, 0}, {1, 8, 0, 0, 0}, {1, 8, 0, 8, 1}, {1, 8, 3, 8, 0}, {1, 8, 3, T, 0},
    {0, 0, 3, 0, 0}, {1, 8, 5, 8, 0}, {1, 2, 0, 1, 0}, {1, 3, 0, 8, 0}, {0, 0, 4, 0, 0},
};
static const char* narrow[] = {"abcd", "", "\x80\xe9\xff", "a\x81\x62"};
static const wchar_t wide0[] = {'a', 'b', 'c', 'd', 0}, wide1[] = {0},
    wide2[] = {0x80, 0xe9, 0xff, 0}, wide3[] = {'a', 0x100, 'b', 0};
static const wchar_t* wide[] = {wide0, wide1, wide2, wide3};

static int keys[] = {1, 3, 5, 7, 9};
static unsigned cmp_calls, cmp_bad, visited;
static int ctx_obj;
static int __cdecl cmp3(void* ctx, const void* k, const void* e) {
    ++cmp_calls;
    if (ctx != &ctx_obj) cmp_bad |= 1;
    if ((const int*)e < keys || (const int*)e >= keys + 5) cmp_bad |= 2;
    else visited = (visited << 4) | (unsigned)((const int*)e - keys);
    if (!k) { cmp_bad |= 4; return -1; }   // a NULL key reaching the callback
    return *(const int*)k - *(const int*)e;
}

static const uint64_t dinputs[] = {0, 0x8000000000000000ULL, 0x3ff0000000000000ULL, 0xbff0000000000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000123ULL, 0xfff8000000000456ULL, 1, 0x7ff0000000000123ULL};
static const uint32_t finputs[] = {0, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7f800000u, 0xff800000u, 0x7fc00123u, 0xffc00456u, 1, 0x7f800123u};

int main(void) {
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    HMODULE h = LoadLibraryA("ucrtbase.dll");
    err = (ErrnoFn)GetProcAddress(h, "_errno");
    ((SetIPH)GetProcAddress(h, "_set_invalid_parameter_handler"))(handler);
    MbsToWcsS mbstowcs_s_ = (MbsToWcsS)GetProcAddress(h, "mbstowcs_s");
    MbsrToWcsS mbsrtowcs_s_ = (MbsrToWcsS)GetProcAddress(h, "mbsrtowcs_s");
    WcsrToMbsS wcsrtombs_s_ = (WcsrToMbsS)GetProcAddress(h, "wcsrtombs_s");
    BsearchS bsearch_s_ = (BsearchS)GetProcAddress(h, "bsearch_s");
    FpStatus clear = (FpStatus)GetProcAddress(h, "_clearfp"), status = (FpStatus)GetProcAddress(h, "_statusfp");
    unsigned c, i, j, fn;

    // Row: fn case ret errno iph_calls iph_errno retval srcoff state0 state1 lasterr buf[0..7]
    for (fn = 0; fn < 3; ++fn) {
        for (c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
            const struct Case* k = &cases[c];
            if (k->src == 5 && fn == 0) continue;
            // *wcstr == NULL faults inside Windows' wcsrtombs_s (measured): undefined.
            if (k->src == 4 && fn == 2) continue;
            wchar_t wbuf[8]; char nbuf[8]; uint32_t state[2] = {0, 0};
            size_t retval = (size_t)0x5a5a5a5a5a5a5a5aULL;
            for (i = 0; i < 8; ++i) { wbuf[i] = 0xcccc; nbuf[i] = (char)0xcc; }
            const char* np = k->src < 4 ? narrow[k->src] : 0;
            const wchar_t* wp = k->src < 4 ? wide[k->src] : 0;
            const char* nstart = np; const wchar_t* wstart = wp;
            size_t* pret = k->null_ret ? 0 : &retval;
            int r;
            reset();
            if (fn == 0) r = mbstowcs_s_(pret, k->dst ? wbuf : 0, k->size, np, k->count);
            else if (fn == 1) r = mbsrtowcs_s_(pret, k->dst ? wbuf : 0, k->size, k->src == 5 ? 0 : &np, k->count, state);
            else r = wcsrtombs_s_(pret, k->dst ? nbuf : 0, k->size, k->src == 5 ? 0 : &wp, k->count, state);
            unsigned errno_v = (unsigned)*err(), le = GetLastError();
            uint64_t off;
            if (fn == 0 || k->src >= 4) off = 0xeeee;
            else if (fn == 1) off = np ? (uint64_t)(np - nstart) : 0xffff;
            else off = wp ? (uint64_t)(wp - wstart) : 0xffff;
            hex(fn); tab(c); tab((unsigned)r); tab(errno_v); tab(iph_calls); tab(iph_errno);
            tab((uint64_t)retval); tab(off); tab(state[0]); tab(state[1]); tab(le);
            for (i = 0; i < 8; ++i) tab(fn == 2 ? (unsigned char)nbuf[i] : wbuf[i]);
            put("\n");
        }
    }

    // bsearch_s rows: "b" case resultidx errno iph_calls iph_errno cmp_calls cmp_bad visited
    {
        struct B { int key; unsigned null_key, null_base; size_t number, width; unsigned null_cmp; };
        static const struct B bc[] = {
            {5, 0, 0, 5, 4, 0}, {4, 0, 0, 5, 4, 0}, {1, 0, 0, 5, 4, 0}, {9, 0, 0, 5, 4, 0},
            {10, 0, 0, 5, 4, 0}, {0, 0, 0, 5, 4, 0}, {3, 0, 0, 0, 4, 0}, {3, 0, 1, 0, 4, 0},
            {3, 0, 1, 5, 4, 0}, {3, 1, 0, 5, 4, 0}, {3, 0, 0, 5, 0, 0}, {3, 0, 0, 5, 4, 1},
            {7, 0, 0, 4, 4, 0}, {3, 0, 0, 1, 4, 0}, {1, 0, 0, 1, 4, 0}, {3, 0, 0, 0, 0, 0},
            {3, 1, 1, 0, 4, 0},
        };
        for (c = 0; c < sizeof(bc) / sizeof(bc[0]); ++c) {
            int key = bc[c].key;
            cmp_calls = cmp_bad = visited = 0;
            reset();
            void* r = bsearch_s_(bc[c].null_key ? 0 : &key, bc[c].null_base ? 0 : keys, bc[c].number,
                                 bc[c].width, bc[c].null_cmp ? 0 : cmp3, &ctx_obj);
            unsigned errno_v = (unsigned)*err();
            put("b\t"); hex(c); tab(r ? (uint64_t)((int*)r - keys) : 0xff); tab(errno_v);
            tab(iph_calls); tab(iph_errno); tab(cmp_calls); tab(cmp_bad); tab(visited); put("\n");
        }
    }

    // fmin-family rows: "m" name i j resultbits statusfp
    {
        static const char* dnames[] = {"fmin", "fminl", "fmaxl"};
        static const char* fnames[] = {"fminf", "fmaxf"};
        for (fn = 0; fn < 3; ++fn) {
            BinD f = (BinD)GetProcAddress(h, dnames[fn]);
            for (i = 0; i < 10; ++i) for (j = 0; j < 10; ++j) {
                union { uint64_t b; double d; } a, b, r; a.b = dinputs[i]; b.b = dinputs[j];
                clear(); r.d = f(a.d, b.d); unsigned st = status();
                put("m\t"); put(dnames[fn]); tab(i); tab(j); tab(r.b); tab(st); put("\n");
            }
        }
        for (fn = 0; fn < 2; ++fn) {
            BinF f = (BinF)GetProcAddress(h, fnames[fn]);
            for (i = 0; i < 10; ++i) for (j = 0; j < 10; ++j) {
                union { uint32_t b; float d; } a, b, r; a.b = finputs[i]; b.b = finputs[j];
                clear(); r.d = f(a.d, b.d); unsigned st = status();
                put("m\t"); put(fnames[fn]); tab(i); tab(j); tab(r.b); tab(st); put("\n");
            }
        }
    }
    return 0;
}
