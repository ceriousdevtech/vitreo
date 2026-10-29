/// @file crt_fmt_io_runner.c
/// @brief 32-bit CRT formatting and file-I/O contract probe, run identically
///        on native Windows and under Vitreo and diffed.
///
/// Companion to crt_surface_runner.c. Formatting earned its own probe: the
/// first CRT diff found that %s and %c silently ignored the field width, so
/// printf("%-28s") emitted a bare string. That class of bug corrupts anything
/// the guest lays out in columns while every value involved is "correct", so
/// this sweeps the width/precision/flag matrix across every conversion rather
/// than spot-checking a few.
///
/// Output is hashed per case AND printed, so a diff points at the exact spec.
///
/// Build: i686-w64-mingw32-gcc -O1 -o crt_fmt_io_runner.exe crt_fmt_io_runner.c
/// Usage: crt_fmt_io_runner.exe [all|fmt|fileio]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Print the produced text with explicit delimiters so leading/trailing spaces
   -- exactly what a width bug gets wrong -- survive the diff. */
static void emit(const char* spec, const char* out) {
    printf("%-18s |%s|\n", spec, out);
}

static void section_fmt(void) {
    printf("== fmt ==\n");
    static const char* flags[] = {"", "-", "+", "0", "#", " "};
    static const char* widths[] = {"", "1", "5", "12", "28"};
    static const char* precs[]  = {"", ".0", ".2", ".8"};
    char spec[32], out[512];

    /* signed integers */
    static const int ivals[] = {0, 1, -1, 42, -42, 2147483647, -2147483647-1};
    for (unsigned f = 0; f < sizeof(flags)/sizeof(*flags); ++f)
    for (unsigned w = 0; w < sizeof(widths)/sizeof(*widths); ++w)
    for (unsigned p = 0; p < sizeof(precs)/sizeof(*precs); ++p)
    for (unsigned v = 0; v < sizeof(ivals)/sizeof(*ivals); ++v) {
        sprintf(spec, "%%%s%s%sd", flags[f], widths[w], precs[p]);
        sprintf(out, spec, ivals[v]);
        char lbl[48]; sprintf(lbl, "%s@%d", spec, v);
        emit(lbl, out);
    }
    /* unsigned / hex / octal: # and 0 interact with the base prefix */
    static const unsigned uvals[] = {0u, 1u, 255u, 4294967295u};
    static const char* uconv[] = {"u", "x", "X", "o"};
    for (unsigned c = 0; c < 4; ++c)
    for (unsigned f = 0; f < sizeof(flags)/sizeof(*flags); ++f)
    for (unsigned w = 0; w < sizeof(widths)/sizeof(*widths); ++w)
    for (unsigned v = 0; v < sizeof(uvals)/sizeof(*uvals); ++v) {
        sprintf(spec, "%%%s%s%s", flags[f], widths[w], uconv[c]);
        sprintf(out, spec, uvals[v]);
        char lbl[48]; sprintf(lbl, "%s@%d", spec, v);
        emit(lbl, out);
    }
    /* strings: the case that was broken. precision truncates, width pads. */
    static const char* svals[] = {"", "a", "abc", "hello world", "0123456789ABCDEF"};
    for (unsigned f = 0; f < sizeof(flags)/sizeof(*flags); ++f)
    for (unsigned w = 0; w < sizeof(widths)/sizeof(*widths); ++w)
    for (unsigned p = 0; p < sizeof(precs)/sizeof(*precs); ++p)
    for (unsigned v = 0; v < sizeof(svals)/sizeof(*svals); ++v) {
        sprintf(spec, "%%%s%s%ss", flags[f], widths[w], precs[p]);
        sprintf(out, spec, svals[v]);
        char lbl[48]; sprintf(lbl, "%s@%d", spec, v);
        emit(lbl, out);
    }
    /* chars */
    for (unsigned f = 0; f < sizeof(flags)/sizeof(*flags); ++f)
    for (unsigned w = 0; w < sizeof(widths)/sizeof(*widths); ++w) {
        sprintf(spec, "%%%s%sc", flags[f], widths[w]);
        sprintf(out, spec, 'Z');
        emit(spec, out);
    }
    /* doubles: %f/%e/%g each round differently and MSVC's exponent has at
       least three digits historically -- a classic divergence. */
    static const double dvals[] = {0.0, 1.0, -1.0, 0.5, 3.14159265358979,
                                   1e-5, 1e5, 1e20, 1e-20, 123456.789, 0.1};
    static const char* dconv[] = {"f", "e", "E", "g", "G"};
    for (unsigned c = 0; c < 5; ++c)
    for (unsigned w = 0; w < sizeof(widths)/sizeof(*widths); ++w)
    for (unsigned p = 0; p < sizeof(precs)/sizeof(*precs); ++p)
    for (unsigned v = 0; v < sizeof(dvals)/sizeof(*dvals); ++v) {
        sprintf(spec, "%%%s%s%s", widths[w], precs[p], dconv[c]);
        sprintf(out, spec, dvals[v]);
        char lbl[48]; sprintf(lbl, "%s@%d", spec, v);
        emit(lbl, out);
    }
    /* return value and truncation behaviour of the _snprintf family:
       MSVC's _snprintf returns -1 when the result does not fit and does NOT
       null-terminate, which differs from C99 snprintf. */
    char small[8];
    int r1 = _snprintf(small, sizeof small, "%s", "0123456789");
    printf("_snprintf.ret      %d\n", r1);
    printf("_snprintf.last     %d\n", (int)(unsigned char)small[7]);
    memset(small, '#', sizeof small);
    int r2 = _snprintf(small, sizeof small, "%s", "abc");
    printf("_snprintf.fit.ret  %d\n", r2);
    printf("_snprintf.fit.term %d\n", (int)(unsigned char)small[3]);
    int r3 = sprintf(out, "%d-%s-%.3f", 42, "x", 1.5);
    printf("sprintf.ret        %d\n", r3);
    emit("sprintf.out", out);
}

static void section_fileio(void) {
    printf("== fileio ==\n");
    const char* path = "crt_io_probe.tmp";
    remove(path);
    FILE* f = fopen(path, "wb");
    printf("fopen.wb.nonnull   %d\n", f != NULL);
    if (!f) return;
    static const char payload[] = "line1\nline2\nline3\n";
    size_t w = fwrite(payload, 1, sizeof(payload) - 1, f);
    printf("fwrite.count       %d\n", (int)w);
    printf("ftell.afterwrite   %ld\n", ftell(f));
    printf("fflush.ret         %d\n", fflush(f));
    printf("fclose.ret         %d\n", fclose(f));

    f = fopen(path, "rb");
    printf("fopen.rb.nonnull   %d\n", f != NULL);
    if (!f) return;
    char buf[64];
    size_t r = fread(buf, 1, 5, f);
    buf[5] = 0;
    printf("fread.count        %d\n", (int)r);
    emit("fread.data", buf);
    printf("ftell.after5       %ld\n", ftell(f));
    printf("fseek.set.ret      %d\n", fseek(f, 6, SEEK_SET));
    printf("ftell.afterseek    %ld\n", ftell(f));
    printf("fseek.end.ret      %d\n", fseek(f, 0, SEEK_END));
    printf("ftell.end          %ld\n", ftell(f));
    printf("feof.beforeread    %d\n", feof(f) != 0);
    int c = fgetc(f);
    printf("fgetc.at.eof       %d\n", c);
    printf("feof.afterread     %d\n", feof(f) != 0);
    printf("ferror.clean       %d\n", ferror(f) != 0);
    rewind(f);
    printf("feof.afterrewind   %d\n", feof(f) != 0);
    char* g = fgets(buf, sizeof buf, f);
    printf("fgets.nonnull      %d\n", g != NULL);
    if (g) { size_t n = strlen(buf); if (n && buf[n-1]=='\n') buf[n-1]='@'; emit("fgets.line1", buf); }
    g = fgets(buf, 3, f);   /* short buffer: must stop at size-1 and terminate */
    if (g) emit("fgets.short", buf);
    printf("fclose2.ret        %d\n", fclose(f));

    /* text mode translates \n on Windows; the byte count must reflect it. */
    f = fopen(path, "w");
    if (f) { fputs("a\nb\n", f); fclose(f); }
    f = fopen(path, "rb");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf, f);
        printf("textmode.bytes     %d\n", (int)n);
        unsigned h = 0; for (size_t i = 0; i < n; ++i) h = h*31u + (unsigned char)buf[i];
        printf("textmode.hash      %u\n", h);
        fclose(f);
    }
    remove(path);
    printf("remove.gone        %d\n", fopen(path, "rb") == NULL);
}

int main(int argc, char** argv) {
    const char* s = (argc > 1) ? argv[1] : "all";
    int all = !strcmp(s, "all");
    if (all || !strcmp(s, "fmt"))    section_fmt();
    if (all || !strcmp(s, "fileio")) section_fileio();
    fflush(stdout);
    return 0;
}
