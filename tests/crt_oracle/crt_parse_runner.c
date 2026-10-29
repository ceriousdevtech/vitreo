/// @file crt_parse_runner.c
/// @brief 32-bit Win32 CRT numeric-parsing and formatting probe, run
///        identically on native Windows and under Vitreo so the two can be
///        diffed exactly.
///
/// Motivation: a game's *tuning* data — decay rates, age thresholds,
/// interaction weights — arrives as text and becomes numbers through these
/// functions. A parse that silently yields the wrong value produces wrong
/// behaviour while every runtime value downstream looks perfectly well-formed,
/// which is invisible to arithmetic and comparison testing. The Sims 3 layout
/// loader already hit exactly this in Vitreo: `swscanf` had no floating-point
/// conversion at all, so every rectf property failed and the UI came up empty.
///
/// Parsed doubles are printed as raw 64-bit patterns so a low-bit difference
/// is as visible as a catastrophic one, and the item count from each *scanf is
/// printed too — a conversion that silently matches nothing is the exact
/// failure mode that bug had.
///
/// Build: i686-w64-mingw32-gcc -O1 -o crt_parse_runner.exe crt_parse_runner.c
///
/// Usage: crt_parse_runner.exe [section]
///   all | strtod | scanf | wscanf | itoa | fmt

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef unsigned long long u64;
typedef unsigned int u32;

static u64 d2b(double d) { u64 b; memcpy(&b, &d, 8); return b; }

/* Text forms a tuning file realistically contains, plus the encodings that
   separate a correct parser from a permissive one: exponents, leading and
   trailing space, signs, hex, INF/NAN spellings, and values that need more
   than 17 digits to round-trip. */
static const char* kNumberText[] = {
    "0", "1", "-1", "0.5", "-0.5", "1.5", "3.14159265358979",
    "0.00037", "1024", "768.0001", "100.0", "1e3", "1E-3", "1.5e+2",
    "-2.5e-3", "  42.25", "42.25  ", "+7.75", ".5", "5.", "0.1",
    "1e308", "1e-308", "1e400", "1e-400", "0x1p3", "0X10", "010",
    "inf", "-inf", "INF", "nan", "NAN", "1.7976931348623157e308",
    "2.2250738585072014e-308", "4.9406564584124654e-324",
    "0.30000000000000004", "16777217", "9007199254740993",
    "", "abc", "1abc", "- 1", "1.2.3", "1,5",
};
#define NUMBER_COUNT ((int)(sizeof(kNumberText) / sizeof(kNumberText[0])))

/* ---------------------------------------------------------------------- */

static void section_strtod(void) {
    int i;
    for (i = 0; i < NUMBER_COUNT; ++i) {
        const char* s = kNumberText[i];
        char* end = NULL;
        const double v = strtod(s, &end);
        /* The end pointer says how much the parser consumed; a parser that
           accepts too much or too little is as wrong as one that returns a
           bad value. */
        const int consumed = (end == NULL) ? -1 : (int)(end - s);
        printf("[strtod] in=\"%s\" bits=%016llX consumed=%d atof=%016llX\n",
               s, (unsigned long long)d2b(v), consumed,
               (unsigned long long)d2b(atof(s)));
    }
}

static void section_scanf(void) {
    int i;
    for (i = 0; i < NUMBER_COUNT; ++i) {
        const char* s = kNumberText[i];
        double d = -1.0;
        float f = -1.0f;
        int n_d = sscanf(s, "%lf", &d);
        int n_f = sscanf(s, "%f", &f);
        int iv = -1, n_i = sscanf(s, "%d", &iv);
        unsigned xv = 0xFFFFFFFFu;
        int n_x = sscanf(s, "%x", &xv);
        printf("[scanf] in=\"%s\" lf=%d/%016llX f=%d/%016llX d=%d/%d "
               "x=%d/%08X\n",
               s, n_d, (unsigned long long)d2b(d), n_f,
               (unsigned long long)d2b((double)f), n_i, iv, n_x, xv);
    }

    /* The exact shape the Sims 3 layout loader uses: a rectf property is four
       comma-separated floats in one string. A parser missing float support
       returns 0 items here and the whole object is discarded. */
    {
        const char* rect = "0.00037,0.00027,1024,768.0001";
        float a = -1, b = -1, c = -1, e = -1;
        const int n = sscanf(rect, "%f,%f,%f,%f", &a, &b, &c, &e);
        printf("[scanf] rectf n=%d a=%016llX b=%016llX c=%016llX d=%016llX\n",
               n, (unsigned long long)d2b((double)a),
               (unsigned long long)d2b((double)b),
               (unsigned long long)d2b((double)c),
               (unsigned long long)d2b((double)e));
    }
}

static void section_wscanf(void) {
    int i;
    for (i = 0; i < NUMBER_COUNT; ++i) {
        wchar_t ws[64];
        const char* s = kNumberText[i];
        int k = 0;
        while (s[k] != '\0' && k < 63) { ws[k] = (wchar_t)s[k]; ++k; }
        ws[k] = 0;

        double d = -1.0;
        float f = -1.0f;
        int iv = -1;
        const int n_d = swscanf(ws, L"%lf", &d);
        const int n_f = swscanf(ws, L"%f", &f);
        const int n_i = swscanf(ws, L"%d", &iv);
        wchar_t* wend = NULL;
        const double wv = wcstod(ws, &wend);
        const int consumed = (wend == NULL) ? -1 : (int)(wend - ws);
        printf("[wscanf] in=\"%s\" lf=%d/%016llX f=%d/%016llX d=%d/%d "
               "wcstod=%016llX consumed=%d\n",
               s, n_d, (unsigned long long)d2b(d), n_f,
               (unsigned long long)d2b((double)f), n_i, iv,
               (unsigned long long)d2b(wv), consumed);
    }

    {
        const wchar_t* rect = L"0.00037,0.00027,1024,768.0001";
        float a = -1, b = -1, c = -1, e = -1;
        const int n = swscanf(rect, L"%f,%f,%f,%f", &a, &b, &c, &e);
        printf("[wscanf] rectf n=%d a=%016llX b=%016llX c=%016llX d=%016llX\n",
               n, (unsigned long long)d2b((double)a),
               (unsigned long long)d2b((double)b),
               (unsigned long long)d2b((double)c),
               (unsigned long long)d2b((double)e));
    }
}

static void section_itoa(void) {
    static const char* kInts[] = {
        "0", "1", "-1", "2147483647", "-2147483648", "4294967295",
        "9223372036854775807", "-9223372036854775808", "  12", "12  ",
        "+34", "0x1F", "007", "abc", "", "99999999999999999999",
    };
    int i;
    for (i = 0; i < (int)(sizeof(kInts) / sizeof(kInts[0])); ++i) {
        const char* s = kInts[i];
        char* end = NULL;
        const long lv = strtol(s, &end, 10);
        const int lconsumed = (end == NULL) ? -1 : (int)(end - s);
        char* uend = NULL;
        const unsigned long uv = strtoul(s, &uend, 10);
        const long long llv = _strtoi64(s, NULL, 10);
        printf("[itoa] in=\"%s\" atoi=%d strtol=%ld/%d strtoul=%lu "
               "strtoi64=%lld\n",
               s, atoi(s), lv, lconsumed, uv, llv);
    }
}

/* Formatting is the other half of a round trip: tuning written back out, and
   any value the game logs or hashes as text. */
static void section_fmt(void) {
    static const double kValues[] = {
        0.0, -0.0, 1.0, -1.0, 0.5, 3.14159265358979, 0.1,
        1e308, 1e-308, 16777217.0, 0.30000000000000004,
        1.7976931348623157e308,
    };
    int i;
    for (i = 0; i < (int)(sizeof(kValues) / sizeof(kValues[0])); ++i) {
        char g[64], f[64], e[64], seventeen[64];
        sprintf(g, "%g", kValues[i]);
        sprintf(f, "%f", kValues[i]);
        sprintf(e, "%e", kValues[i]);
        sprintf(seventeen, "%.17g", kValues[i]);
        /* Round-trip: text back to a double must reproduce the bits. */
        const double back = strtod(seventeen, NULL);
        printf("[fmt] bits=%016llX g=\"%s\" f=\"%s\" e=\"%s\" .17g=\"%s\" "
               "roundtrip=%s\n",
               (unsigned long long)d2b(kValues[i]), g, f, e, seventeen,
               d2b(back) == d2b(kValues[i]) ? "exact" : "LOSSY");
    }
}

int main(int argc, char** argv) {
    const char* section = (argc > 1) ? argv[1] : "all";
    const int all = (strcmp(section, "all") == 0);

    setvbuf(stdout, NULL, _IONBF, 0);

    if (all || strcmp(section, "strtod") == 0) section_strtod();
    if (all || strcmp(section, "scanf") == 0) section_scanf();
    if (all || strcmp(section, "wscanf") == 0) section_wscanf();
    if (all || strcmp(section, "itoa") == 0) section_itoa();
    if (all || strcmp(section, "fmt") == 0) section_fmt();

    printf("[done] section=%s\n", section);
    return 0;
}
