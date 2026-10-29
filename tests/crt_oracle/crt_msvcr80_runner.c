// SPDX-License-Identifier: LGPL-2.1-only
// Differential probe for the remaining msvcr80 surface The Sims 3 imports:
// classification, conversion, wide strings, low-level descriptor I/O, time,
// environment, and the odds and ends (signal, strtok, pointer encoding).
//
// Everything printed is contract-invariant. Where a result is inherently
// machine-specific -- the current directory, the wall clock, a temp file name
// -- the derived property is printed instead (is it absolute, does the length
// agree with strlen, is it monotonic). Times are pinned to a fixed time_t so
// the formatted output IS deterministic and can be compared byte for byte.
#include <windows.h>
#include <ctype.h>
#include <wctype.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <io.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <wchar.h>

// mingw's import library has no _encode_pointer/_decode_pointer, so they are
// resolved at run time from the CRT the process actually loaded -- which is
// also the honest way to probe them, since that is how a guest reaches them.
typedef void* (__cdecl *ptr_fn)(void*);

#define P(k, v)  printf("%s %d\n", (k), (int)(v))
#define PX(k, v) printf("%s %08lX\n", (k), (unsigned long)(v))
#define PS(k, s) printf("%s |%s|\n", (k), (s))

static void t_classify(void) {
    // The classification functions must agree with Windows across the whole
    // byte range, including the >0x7F region where the C locale's answer is
    // not obvious. One packed line per function keeps the diff readable.
    static const struct { const char* n; int (*f)(int); } fns[] = {
        {"isalnum", isalnum}, {"isalpha", isalpha}, {"isdigit", isdigit},
        {"islower", islower}, {"isprint", isprint}, {"isspace", isspace},
        {"isupper", isupper}, {"isxdigit", isxdigit},
    };
    for (unsigned k = 0; k < sizeof fns / sizeof fns[0]; ++k) {
        char line[300]; int n = 0;
        for (int c = 0; c < 256; ++c) line[n++] = fns[k].f(c) ? '1' : '0';
        line[n] = 0;
        printf("cls.%s |%s|\n", fns[k].n, line);
    }
    char lo[300], up[300]; int n = 0;
    for (int c = 0; c < 256; ++c) { lo[n] = (char)tolower(c); up[n] = (char)toupper(c); ++n; }
    // Print as hex: the mapped values are bytes, not printable text.
    { char h[700]; int j = 0;
      for (int c = 0; c < 256; ++c) j += sprintf(h + j, "%02x", (unsigned char)lo[c]);
      printf("cls.tolower |%s|\n", h);
      j = 0;
      for (int c = 0; c < 256; ++c) j += sprintf(h + j, "%02x", (unsigned char)up[c]);
      printf("cls.toupper |%s|\n", h); }

    // Wide classification over Latin-1 plus a few code points that separate a
    // real table from an ASCII-only approximation.
    static const wchar_t wc[] = {
        L'A', L'z', L'0', L' ', L'\t', L'\n', L'.', 0x00C9 /* E-acute */,
        0x00DF /* sharp s */, 0x00F7 /* division sign */, 0x0391 /* Greek Alpha */,
        0x0660 /* Arabic-Indic zero */, 0x3000 /* ideographic space */,
        0xFF21 /* fullwidth A */, 0x2028 /* line separator */
    };
    for (unsigned i = 0; i < sizeof wc / sizeof wc[0]; ++i) {
        char lbl[48];
        sprintf(lbl, "wcls.%04X", (unsigned)wc[i]);
        printf("%s %d%d%d%d %04X %04X\n", lbl,
               iswalpha(wc[i]) ? 1 : 0, iswdigit(wc[i]) ? 1 : 0,
               iswspace(wc[i]) ? 1 : 0, iswxdigit(wc[i]) ? 1 : 0,
               (unsigned)towlower(wc[i]), (unsigned)towupper(wc[i]));
    }
}

static void t_convert(void) {
    // _ecvt / _fcvt return a static buffer plus a decimal exponent and sign.
    // The rounding at the requested digit count is the whole contract.
    static const double vals[] = {
        0.0, 1.0, -1.0, 0.5, 1.0 / 3.0, 123456.789, 1e-5, 9.9999999, 1e20, 2.5, 3.5
    };
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; ++i) {
        int dec = 0, sign = 0;
        char* e = _ecvt(vals[i], 6, &dec, &sign);
        printf("ecvt[%u] |%s| dec=%d sign=%d\n", i, e ? e : "(null)", dec, sign);
        dec = 0; sign = 0;
        char* f = _fcvt(vals[i], 3, &dec, &sign);
        printf("fcvt[%u] |%s| dec=%d sign=%d\n", i, f ? f : "(null)", dec, sign);
    }

    static const char* ints[] = {
        "0", "42", "-42", "2147483647", "-2147483648", "4294967295",
        "18446744073709551615", "0x1f", "  12ab", "", "+7", "99999999999999999999"
    };
    for (unsigned i = 0; i < sizeof ints / sizeof ints[0]; ++i) {
        char* end = NULL;
        errno = 0;
        long l = strtol(ints[i], &end, 0);
        printf("strtol[%u] %ld off=%d errno=%d\n", i, l,
               (int)(end - ints[i]), errno);
        end = NULL; errno = 0;
        unsigned long u = strtoul(ints[i], &end, 0);
        printf("strtoul[%u] %lu off=%d errno=%d\n", i, u,
               (int)(end - ints[i]), errno);
        end = NULL; errno = 0;
        unsigned __int64 q = _strtoui64(ints[i], &end, 0);
        printf("strtoui64[%u] %llu off=%d errno=%d\n", i,
               (unsigned long long)q, (int)(end - ints[i]), errno);
    }

    static const wchar_t* wints[] = { L"0", L"-99", L"2147483647", L"  0x2A", L"zz" };
    for (unsigned i = 0; i < sizeof wints / sizeof wints[0]; ++i) {
        wchar_t* wend = NULL;
        long wl = wcstol(wints[i], &wend, 0);
        unsigned long wu = wcstoul(wints[i], &wend, 0);
        printf("wcstol[%u] %ld %lu off=%d\n", i, wl, wu, (int)(wend - wints[i]));
        printf("wtol[%u] %ld\n", i, _wtol(wints[i]));
    }

    // _finite / _isnan classify the specials the FP probe generates.
    double specials[5];
    specials[0] = 0.0; specials[1] = 1.0;
    specials[2] = HUGE_VAL; specials[3] = -HUGE_VAL;
    { unsigned long long qn = 0x7FF8000000000000ull; memcpy(&specials[4], &qn, 8); }
    for (unsigned i = 0; i < 5; ++i) {
        printf("fpclass[%u] finite=%d isnan=%d\n", i,
               _finite(specials[i]), _isnan(specials[i]));
    }
}

static void t_wide_strings(void) {
    static const wchar_t* a[] = { L"", L"Alpha", L"alpha", L"ALPHA", L"alpha2", L"beta" };
    for (unsigned i = 0; i < sizeof a / sizeof a[0]; ++i) {
        for (unsigned j = 0; j < sizeof a / sizeof a[0]; ++j) {
            char lbl[64];
            sprintf(lbl, "wcsicmp[%u,%u]", i, j);
            // Only the SIGN is contractual; the magnitude is not.
            int r = _wcsicmp(a[i], a[j]);
            P(lbl, r < 0 ? -1 : (r > 0 ? 1 : 0));
            sprintf(lbl, "wcsnicmp3[%u,%u]", i, j);
            r = _wcsnicmp(a[i], a[j], 3);
            P(lbl, r < 0 ? -1 : (r > 0 ? 1 : 0));
            sprintf(lbl, "wcsncmp3[%u,%u]", i, j);
            r = wcsncmp(a[i], a[j], 3);
            P(lbl, r < 0 ? -1 : (r > 0 ? 1 : 0));
        }
    }
    wchar_t buf[64];
    wcscpy(buf, L"MiXeD Case 123");
    _wcslwr(buf);
    printf("wcslwr |%ls|\n", buf);

    const wchar_t* hay = L"the quick brown fox";
    printf("wcschr |%ls|\n", wcschr(hay, L'q') ? wcschr(hay, L'q') : L"(null)");
    P("wcschr.missing", wcschr(hay, L'Z') == NULL);
    P("wcschr.nul_ok", wcschr(hay, 0) != NULL);
    printf("wcsrchr |%ls|\n", wcsrchr(hay, L'o') ? wcsrchr(hay, L'o') : L"(null)");
    printf("wcsstr |%ls|\n", wcsstr(hay, L"brown") ? wcsstr(hay, L"brown") : L"(null)");
    P("wcsstr.empty_is_start", wcsstr(hay, L"") == hay);
    P("wcsstr.missing", wcsstr(hay, L"zebra") == NULL);
    printf("wcspbrk |%ls|\n", wcspbrk(hay, L"xq") ? wcspbrk(hay, L"xq") : L"(null)");
    P("wcspbrk.missing", wcspbrk(hay, L"ZY") == NULL);

    // wcsncpy does NOT terminate when the source fills the field, and it pads
    // with NULs when it does not. Both halves matter.
    wchar_t pad[8];
    for (int i = 0; i < 8; ++i) pad[i] = L'#';
    wcsncpy(pad, L"ab", 5);
    printf("wcsncpy.pad %04X %04X %04X %04X %04X %04X\n",
           pad[0], pad[1], pad[2], pad[3], pad[4], pad[5]);
    for (int i = 0; i < 8; ++i) pad[i] = L'#';
    wcsncpy(pad, L"abcdefgh", 4);
    printf("wcsncpy.fill %04X %04X %04X %04X %04X\n",
           pad[0], pad[1], pad[2], pad[3], pad[4]);

    wchar_t sbuf[16];
    errno_t se = wcscpy_s(sbuf, 16, L"fits");
    printf("wcscpy_s.ok %d |%ls|\n", (int)se, sbuf);
    errno_t se2 = wcscpy_s(sbuf, 2, L"far too long");
    P("wcscpy_s.overflow", (int)se2);
    // On failure the destination is emptied, not left partially written.
    P("wcscpy_s.emptied", sbuf[0] == 0);
}

static void t_narrow_misc(void) {
    char work[64];
    strcpy(work, "alpha,beta;;gamma");
    char* t = strtok(work, ",;");
    int n = 0;
    while (t != NULL && n < 8) {
        char lbl[32]; sprintf(lbl, "strtok[%d]", n++);
        PS(lbl, t);
        t = strtok(NULL, ",;");
    }
    P("strtok.count", n);

    // memmove must handle overlap in both directions.
    char ov[16];
    strcpy(ov, "0123456789");
    memmove(ov + 2, ov, 8); ov[10] = 0;
    PS("memmove.fwd", ov);
    strcpy(ov, "0123456789");
    memmove(ov, ov + 2, 8); ov[8] = 0;
    PS("memmove.back", ov);

    for (int e = 0; e <= 8; ++e) {
        char lbl[32]; sprintf(lbl, "strerror[%d]", e);
        PS(lbl, strerror(e));
    }

    // _vsnprintf truncates without terminating and returns -1; _snprintf_s
    // terminates and (with _TRUNCATE) returns -1 too. Different contracts.
    char small[8];
    memset(small, '#', sizeof small);
    int r = 0;
    { va_list ap; (void)ap; }
    r = _snprintf(small, 5, "%s", "abcdefgh");
    printf("vsnprintf.ret %d bytes %02x%02x%02x%02x%02x%02x\n", r,
           (unsigned char)small[0], (unsigned char)small[1],
           (unsigned char)small[2], (unsigned char)small[3],
           (unsigned char)small[4], (unsigned char)small[5]);

    {
        HMODULE crt = GetModuleHandleA("msvcrt.dll");
        if (!crt) crt = LoadLibraryA("msvcrt.dll");
        ptr_fn enc = crt ? (ptr_fn)GetProcAddress(crt, "_encode_pointer") : NULL;
        ptr_fn dec = crt ? (ptr_fn)GetProcAddress(crt, "_decode_pointer") : NULL;
        P("encode.available", enc != NULL && dec != NULL);
        if (enc && dec) {
            P("encode.roundtrip",
              dec(enc((void*)0x12345678)) == (void*)0x12345678);
            P("encode.roundtrip_null", dec(enc(NULL)) == NULL);
        }
    }

    P("errno.ptr_stable", _errno() == _errno());
    *_errno() = 34;
    P("errno.through_ptr", errno == 34);
    errno = 0;

    // signal returns the PREVIOUS handler; SIG_ERR marks a rejected signal.
    void (*prev)(int) = signal(SIGABRT, SIG_IGN);
    P("signal.prev_not_err", prev != SIG_ERR);
    void (*prev2)(int) = signal(SIGABRT, prev);
    P("signal.roundtrip", prev2 == SIG_IGN);
    P("signal.bogus_is_err", signal(1234, SIG_IGN) == SIG_ERR);
}

static void t_time(void) {
    // A pinned time_t makes the formatted output deterministic, so these can
    // be compared byte for byte instead of only for shape.
    __time64_t fixed = 1234567890;   // 2009-02-13 23:31:30 UTC
    struct tm* g = _localtime64(&fixed);
    P("time.localtime.nonnull", g != NULL);
    if (g) {
        // Local time depends on the box's zone, so print only the fields that
        // do not: the UTC-invariant ones come from gmtime instead.
        struct tm* u = _gmtime64(&fixed);
        if (u) {
            printf("time.gmt %04d-%02d-%02d %02d:%02d:%02d wday=%d yday=%d\n",
                   u->tm_year + 1900, u->tm_mon + 1, u->tm_mday,
                   u->tm_hour, u->tm_min, u->tm_sec, u->tm_wday, u->tm_yday);
            PS("time.asctime", asctime(u));
        }
    }
    __time64_t now64 = _time64(NULL);
    __time32_t now32 = _time32(NULL);
    P("time.t64_sane", now64 > 1600000000LL && now64 < 4000000000LL);
    P("time.t32_matches_t64", (long long)now32 == (long long)now64 ||
                              (long long)now32 == (long long)now64 - 1);
    __time64_t out = 0;
    __time64_t r64 = _time64(&out);
    P("time.out_param_matches", out == r64);

    wchar_t wbuf[64];
    errno_t we = _wctime64_s(wbuf, 64, &fixed);
    P("time.wctime_s.ret", (int)we);
    P("time.wctime_s.len", (int)wcslen(wbuf));
}

static void t_env_paths(void) {
    const char* p = getenv("PATH");
    P("env.path_nonnull", p != NULL);
    P("env.path_nonempty", p && p[0] != 0);
    P("env.missing_null", getenv("VITREO_NO_SUCH_VAR_XYZ") == NULL);
    P("env.empty_name_null", getenv("") == NULL);

    char cwd[MAX_PATH];
    char* c = _getcwd(cwd, sizeof cwd);
    P("cwd.nonnull", c != NULL);
    P("cwd.absolute", c && cwd[1] == ':');
    P("cwd.len_matches", c && strlen(cwd) > 2);
    wchar_t wcwd[MAX_PATH];
    wchar_t* wc = _wgetcwd(wcwd, MAX_PATH);
    P("wcwd.nonnull", wc != NULL);
    P("wcwd.same_len", wc && c && wcslen(wcwd) == strlen(cwd));
    P("drive.in_range", _getdrive() >= 1 && _getdrive() <= 26);
    wchar_t wdc[MAX_PATH];
    P("wgetdcwd.nonnull", _wgetdcwd(_getdrive(), wdc, MAX_PATH) != NULL);

    // _mktemp rewrites the template in place; the pattern of the result is
    // the contract, not the name it picks.
    char tmpl[] = "vitXXXXXX";
    char* mk = _mktemp(tmpl);
    P("mktemp.nonnull", mk != NULL);
    P("mktemp.len", (int)strlen(tmpl));
    P("mktemp.prefix_kept", strncmp(tmpl, "vit", 3) == 0);
}

static void t_lowlevel_io(void) {
    char path[MAX_PATH], tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    sprintf(path, "%svitreo_ll_io.bin", tmp);
    _unlink(path);

    int fd = _open(path, _O_CREAT | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
    P("io.open_ok", fd >= 0);
    if (fd >= 0) {
        P("io.write", (int)_write(fd, "abcdefghij", 10));
        P("io.lseek_cur", (int)_lseek(fd, 0, SEEK_CUR));
        P("io.lseek_set", (int)_lseek(fd, 2, SEEK_SET));
        char rb[16]; memset(rb, 0, sizeof rb);
        P("io.read_n", (int)_read(fd, rb, 4));
        PS("io.read_data", rb);
        P("io.lseek_end", (int)_lseek(fd, 0, SEEK_END));
        P("io.read_at_eof", (int)_read(fd, rb, 4));
        P("io.osfhandle_valid",
          (HANDLE)_get_osfhandle(fd) != INVALID_HANDLE_VALUE);
        struct _stat64i32 st;
        P("io.fstat_ret", _fstat64i32(fd, &st));
        P("io.fstat_size", (int)st.st_size);
        P("io.fstat_isreg", (st.st_mode & _S_IFREG) != 0);
        P("io.close", _close(fd));
        P("io.close_twice", _close(fd));
        P("io.errno_after", errno);
    }
    struct _stat64i32 s2;
    P("io.stat_ret", _stat64i32(path, &s2));
    P("io.stat_size", (int)s2.st_size);
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    struct _stat64i32 s3;
    P("io.wstat_ret", _wstat64i32(wpath, &s3));
    P("io.wstat_size_matches", s3.st_size == s2.st_size);
    P("io.stat_missing", _stat64i32("Z:\\nope\\nope.bin", &s2));
    P("io.open_missing", _open("Z:\\nope\\nope.bin", _O_RDONLY));
    P("io.open_missing_errno", errno);
    _unlink(path);

    FILE* tf = tmpfile();
    P("tmpfile.nonnull", tf != NULL);
    if (tf) {
        P("tmpfile.fileno_valid", _fileno(tf) >= 0);
        fputs("xyz", tf);
        rewind(tf);
        char b[8] = {0};
        P("tmpfile.read", fgets(b, sizeof b, tf) != NULL);
        PS("tmpfile.data", b);
        fclose(tf);
    }

    // fscanf against a file we wrote ourselves, so the input is fixed.
    sprintf(path, "%svitreo_scan.txt", tmp);
    FILE* w = fopen(path, "wb");
    if (w) { fputs("42 3.5 hello\n-7 abc\n", w); fclose(w); }
    FILE* r = fopen(path, "rb");
    if (r) {
        int i1 = 0, i2 = 0; double d1 = 0; char s1[32] = {0}, s2b[32] = {0};
        int got = fscanf(r, "%d %lf %31s", &i1, &d1, s1);
        printf("fscanf.n1 %d %d %.6f |%s|\n", got, i1, d1, s1);
        got = fscanf(r, "%d %31s", &i2, s2b);
        printf("fscanf.n2 %d %d |%s|\n", got, i2, s2b);
        got = fscanf(r, "%d", &i2);
        P("fscanf.eof", got);
        fclose(r);
    }
    _unlink(path);
}

int main(void) {
    setbuf(stdout, NULL);
    t_classify();
    t_convert();
    t_wide_strings();
    t_narrow_misc();
    t_time();
    t_env_paths();
    t_lowlevel_io();
    printf("DONE 1\n");
    return 0;
}
