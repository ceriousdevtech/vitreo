// vcruntime140 memory/string contract probe (records behaviour only).
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef void* (__cdecl *MemCpy)(void*, const void*, size_t);
typedef void* (__cdecl *MemSet)(void*, int, size_t);
typedef int   (__cdecl *MemCmp)(const void*, const void*, size_t);
typedef void* (__cdecl *MemChr)(const void*, int, size_t);
typedef char* (__cdecl *StrChr)(const char*, int);
typedef char* (__cdecl *StrStr)(const char*, const char*);
typedef wchar_t* (__cdecl *WcsChrI)(const wchar_t*, unsigned);
typedef wchar_t* (__cdecl *WcsStr)(const wchar_t*, const wchar_t*);

static MemCpy p_memcpy, p_memmove; static MemSet p_memset; static MemCmp p_memcmp;
static MemChr p_memchr; static StrChr p_strchr, p_strrchr; static StrStr p_strstr;
static WcsChrI p_wcschr, p_wcsrchr; static WcsStr p_wcsstr;

static volatile LONG g_count; static DWORD g_code; static ULONG_PTR g_i0, g_i1;
static DWORD g_nparams; static BYTE* g_watch; static size_t g_watch_n; static const BYTE* g_ref;
static size_t g_match_at_fault; static BYTE* g_fix; static DWORD g_fix_prot;

static LONG CALLBACK veh(PEXCEPTION_POINTERS e) {
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_ACCESS_VIOLATION && c != STATUS_GUARD_PAGE_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (g_fix == NULL) return EXCEPTION_CONTINUE_SEARCH;
    BYTE* fa = (BYTE*)e->ExceptionRecord->ExceptionInformation[1];
    if (fa < g_fix || fa >= g_fix + 4096) { printf("outside_fault\t%p\n", fa); fflush(stdout); ExitProcess(3); }
    if (InterlockedIncrement(&g_count) == 1) {
        g_code = c; g_nparams = e->ExceptionRecord->NumberParameters;
        g_i0 = e->ExceptionRecord->ExceptionInformation[0];
        g_i1 = e->ExceptionRecord->ExceptionInformation[1];
        size_t m = 0;
        if (g_watch) for (size_t i = 0; i < g_watch_n; ++i) if (g_watch[i] == g_ref[i]) ++m;
        g_match_at_fault = m;
    }
    DWORD old; VirtualProtect(g_fix, 4096, g_fix_prot, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static BYTE* region;  // 3 pages: [0] RW, [1] RW, [2] RW (page index 2 gets faulted)
static void reset_region(void) {
    DWORD old; VirtualProtect(region, 3 * 4096, PAGE_READWRITE, &old);
    for (int i = 0; i < 3 * 4096; ++i) region[i] = (BYTE)(i * 7 + 3);
    region[3 * 4096 - 1] = region[3 * 4096 - 2] = region[3 * 4096 - 3] = region[3 * 4096 - 4] = 0;
}
static void arm(BYTE* page, DWORD prot) {
    DWORD old; VirtualProtect(page, 4096, prot, &old);
    g_fix = page; g_fix_prot = PAGE_READWRITE; g_count = 0; g_code = 0; g_i0 = g_i1 = 0; g_match_at_fault = 0;
}
static void report(const char* tag, BYTE* page) {
    long long off = g_count ? (long long)((BYTE*)g_i1 - page) : 0;
    printf("fault\t%s\tcount=%ld\tcode=%08lx\tn=%lu\trw=%lu\toff_in_page=%lld\tmatched_before=%zu\n",
           tag, (long)g_count, (unsigned long)g_code, (unsigned long)g_nparams, (unsigned long)g_i0, off, g_match_at_fault);
    g_fix = NULL; g_watch = NULL;
}

static uint32_t fnv(const BYTE* p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; } return h; }
static void ref_memmove(BYTE* d, const BYTE* s, size_t n) {
    if (d < s) for (size_t i = 0; i < n; ++i) d[i] = s[i];
    else for (size_t i = n; i-- > 0;) d[i] = s[i];
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE h = LoadLibraryA("vcruntime140.dll");
    if (!h) { printf("no vcruntime140\n"); return 1; }
    p_memcpy = (MemCpy)GetProcAddress(h, "memcpy"); p_memmove = (MemCpy)GetProcAddress(h, "memmove");
    p_memset = (MemSet)GetProcAddress(h, "memset"); p_memcmp = (MemCmp)GetProcAddress(h, "memcmp");
    p_memchr = (MemChr)GetProcAddress(h, "memchr"); p_strchr = (StrChr)GetProcAddress(h, "strchr");
    p_strrchr = (StrChr)GetProcAddress(h, "strrchr"); p_strstr = (StrStr)GetProcAddress(h, "strstr");
    p_wcschr = (WcsChrI)GetProcAddress(h, "wcschr"); p_wcsrchr = (WcsChrI)GetProcAddress(h, "wcsrchr");
    p_wcsstr = (WcsStr)GetProcAddress(h, "wcsstr");
    printf("bits\t%d\n", (int)(sizeof(void*) * 8));

    // 1. memcmp return values
    static BYTE a[9000], b[9000];
    size_t sizes[] = {1, 2, 7, 8, 15, 16, 31, 32, 33, 63, 64, 100, 4096, 8192};
    size_t poss[] = {0, 1, 5, 15, 17, 31, 33, 100, 4100};
    BYTE pairs[][2] = {{0x01, 0x80}, {0x80, 0x01}, {0x00, 0xFF}, {0x41, 0x42}, {0x42, 0x41}};
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; ++si)
        for (size_t pi = 0; pi < sizeof poss / sizeof *poss; ++pi) {
            if (poss[pi] >= sizes[si]) continue;
            for (size_t k = 0; k < 5; ++k) {
                memset(a, 0x33, sizeof a); memset(b, 0x33, sizeof b);
                a[poss[pi]] = pairs[k][0]; b[poss[pi]] = pairs[k][1];
                printf("memcmp\t%zu\t%zu\t%02x\t%02x\t%d\n", sizes[si], poss[pi], pairs[k][0], pairs[k][1], p_memcmp(a, b, sizes[si]));
            }
        }
    printf("memcmp\tequal\t%d\tzero_len_null\t%d\n", p_memcmp(a, a, 64), p_memcmp(NULL, NULL, 0));

    // 2. memcpy / memmove overlap
    static BYTE buf[12000], refb[12000];
    long deltas[] = {1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 1000, -1, -2, -3, -4, -7, -8, -15, -16, -17, -31, -32, -33, -63, -64, -65, -1000};
    size_t ns[] = {1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 100, 127, 128, 129, 255, 256, 257, 1000, 4096, 5000};
    for (int f = 0; f < 2; ++f)
        for (size_t di = 0; di < sizeof deltas / sizeof *deltas; ++di)
            for (size_t ni = 0; ni < sizeof ns / sizeof *ns; ++ni) {
                for (int i = 0; i < 12000; ++i) buf[i] = refb[i] = (BYTE)(i * 13 + 5);
                BYTE* s = buf + 3000 + 1; BYTE* d = s + deltas[di];
                void* r = (f ? p_memmove : p_memcpy)(d, s, ns[ni]);
                ref_memmove(refb + (d - buf), refb + (s - buf), ns[ni]);
                printf("%s\t%ld\t%zu\tmemmove_equal=%d\tret_ok=%d\n", f ? "memmove" : "memcpy", deltas[di], ns[ni],
                       memcmp(buf, refb, sizeof buf) == 0, r == d);
            }
    printf("memcpy\tzero_len_null_ret=%p\n", p_memcpy(NULL, NULL, 0));

    // 3. memset / chr semantics
    BYTE ms[16]; p_memset(ms, 0x1A5, 16); printf("memset\tc=0x1A5\tbyte=%02x\n", ms[0]);
    p_memset(ms, -1, 16); printf("memset\tc=-1\tbyte=%02x\n", ms[0]);
    const char* str = "abcabc\x80xyz";
    int cs[] = {'a', 'c', 0, 'a' + 256, 'a' - 256, 0x80, -128, 0x180, 'q'};
    for (size_t i = 0; i < sizeof cs / sizeof *cs; ++i) {
        char* r1 = p_strchr(str, cs[i]); char* r2 = p_strrchr(str, cs[i]);
        void* r3 = p_memchr(str, cs[i], 11);
        printf("chr\tc=%d\tstrchr=%ld\tstrrchr=%ld\tmemchr=%ld\n", cs[i], r1 ? (long)(r1 - str) : -1L,
               r2 ? (long)(r2 - str) : -1L, r3 ? (long)((const char*)r3 - str) : -1L);
    }
    printf("memchr\tzero_len=%p\n", p_memchr(NULL, 0, 0));
    printf("strstr\tempty=%ld\tfound=%ld\tmissing=%p\tsame=%ld\tlonger=%p\n", (long)(p_strstr(str, "") - str), (long)(p_strstr(str, "cab") - str),
           (void*)p_strstr(str, "abd"), (long)(p_strstr(str, str) - str), (void*)p_strstr("ab", "abc"));
    const wchar_t* ws = L"abcab\x8000xy";
    unsigned wcs[] = {'a', 'b', 0, 'a' + 0x10000, 0x8000, 0x18000, 'q'};
    for (size_t i = 0; i < sizeof wcs / sizeof *wcs; ++i) {
        wchar_t* r1 = p_wcschr(ws, wcs[i]); wchar_t* r2 = p_wcsrchr(ws, wcs[i]);
        printf("wchr\tc=%x\twcschr=%ld\twcsrchr=%ld\n", wcs[i], r1 ? (long)(r1 - ws) : -1L, r2 ? (long)(r2 - ws) : -1L);
    }
    printf("wcsstr\tempty=%ld\tfound=%ld\tmissing=%p\n", (long)(p_wcsstr(ws, L"") - ws), (long)(p_wcsstr(ws, L"cab") - ws), (void*)p_wcsstr(ws, L"abd"));

    // 4. faults. region pages: P0 P1 P2; faulting page = P1 (index 1).
    AddVectoredExceptionHandler(1, veh);
    region = (BYTE*)VirtualAlloc(NULL, 3 * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BYTE* P1 = region + 4096;
    static BYTE src[20000], dstb[20000];
    size_t lens[] = {200, 300, 1000, 5000};
    DWORD prots[] = {PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE | PAGE_GUARD};
    const char* pn[] = {"noaccess", "readonly", "guard"};
    for (int pk = 0; pk < 3; ++pk)
        for (size_t li = 0; li < 4; ++li) {
            size_t n = lens[li]; char tag[128];
            for (size_t i = 0; i < sizeof src; ++i) src[i] = (BYTE)(i * 5 + 1);
            // write fault: memcpy into P0 tail, crossing into P1
            reset_region(); g_watch = P1 - 100; g_watch_n = 100; g_ref = src; arm(P1, prots[pk]);
            p_memcpy(P1 - 100, src, n); snprintf(tag, sizeof tag, "memcpy_write_%s_%zu", pn[pk], n); report(tag, P1);
            reset_region(); g_watch = P1 - 100; g_watch_n = 100; g_ref = src; arm(P1, prots[pk]);
            p_memmove(P1 - 100, src, n); snprintf(tag, sizeof tag, "memmove_write_%s_%zu", pn[pk], n); report(tag, P1);
            static BYTE setref[20000]; memset(setref, 0x5A, sizeof setref);
            reset_region(); g_watch = P1 - 100; g_watch_n = 100; g_ref = setref; arm(P1, prots[pk]);
            p_memset(P1 - 100, 0x5A, n); snprintf(tag, sizeof tag, "memset_write_%s_%zu", pn[pk], n); report(tag, P1);
            if (pk == 1) continue;  // readonly is readable
            // read fault: memcpy from P0 tail crossing into P1
            reset_region(); memset(dstb, 0xEE, sizeof dstb); g_watch = dstb; g_watch_n = 100; g_ref = P1 - 100; arm(P1, prots[pk]);
            p_memcpy(dstb, P1 - 100, n); snprintf(tag, sizeof tag, "memcpy_read_%s_%zu", pn[pk], n); report(tag, P1);
            // backward memmove: dst above src, dst range crosses into P1
            reset_region(); g_watch = NULL; arm(P1, prots[pk]);
            BYTE* bs = P1 - 150; BYTE* bd = P1 - 100;
            p_memmove(bd, bs, n); snprintf(tag, sizeof tag, "memmove_back_%s_%zu", pn[pk], n); report(tag, P1);
            // backward memmove reading: src range crosses into P1 (from above): dst = P1+4096+..? use P2 as dst
            // memcmp: equal prefix, region crosses into P1
            reset_region(); memcpy(dstb, P1 - 100, 100); arm(P1, prots[pk]);
            int rc = p_memcmp(P1 - 100, dstb, n); snprintf(tag, sizeof tag, "memcmp_eq_%s_%zu", pn[pk], n); printf("memcmp_ret\t%s\t%d\n", tag, rc); report(tag, P1);
            reset_region(); memcpy(dstb, P1 - 100, 100); dstb[90] ^= 1; arm(P1, prots[pk]);
            rc = p_memcmp(P1 - 100, dstb, n); snprintf(tag, sizeof tag, "memcmp_diff90_%s_%zu", pn[pk], n); printf("memcmp_ret\t%s\t%d\n", tag, rc); report(tag, P1);
            reset_region(); memcpy(dstb, P1 - 100, 100); dstb[2] ^= 1; arm(P1, prots[pk]);
            rc = p_memcmp(P1 - 100, dstb, n); snprintf(tag, sizeof tag, "memcmp_diff2_%s_%zu", pn[pk], n); printf("memcmp_ret\t%s\t%d\n", tag, rc); report(tag, P1);
            // memchr: match before boundary / no match
            reset_region(); memset(P1 - 100, 0x11, 100); (P1 - 100)[40] = 0x77; arm(P1, prots[pk]);
            void* mr = p_memchr(P1 - 100, 0x77, n); snprintf(tag, sizeof tag, "memchr_hit40_%s_%zu", pn[pk], n); printf("memchr_ret\t%s\t%ld\n", tag, mr ? (long)((BYTE*)mr - (P1 - 100)) : -1L); report(tag, P1);
            reset_region(); memset(P1 - 100, 0x11, 100); (P1 - 100)[97] = 0x77; arm(P1, prots[pk]);
            mr = p_memchr(P1 - 100, 0x77, n); snprintf(tag, sizeof tag, "memchr_hit97_%s_%zu", pn[pk], n); printf("memchr_ret\t%s\t%ld\n", tag, mr ? (long)((BYTE*)mr - (P1 - 100)) : -1L); report(tag, P1);
            reset_region(); memset(P1 - 100, 0x11, 100); arm(P1, prots[pk]);
            mr = p_memchr(P1 - 100, 0x77, n); snprintf(tag, sizeof tag, "memchr_miss_%s_%zu", pn[pk], n); printf("memchr_ret\t%s\t%ld\n", tag, mr ? (long)((BYTE*)mr - (P1 - 100)) : -1L); report(tag, P1);
        }
    // strings ending exactly at a page end before an inaccessible page; and running into it
    for (int pk = 0; pk < 3; pk += 2) {
        char tag[128];
        reset_region(); memset(P1 - 64, 'x', 63); (P1 - 1)[0] = 0; arm(P1, prots[pk]);
        char* r = p_strchr((char*)(P1 - 64), 'q'); snprintf(tag, sizeof tag, "strchr_term_at_end_%s", pn[pk]); printf("str_ret\t%s\t%p\n", tag, (void*)r); report(tag, P1);
        reset_region(); memset(P1 - 64, 'x', 63); (P1 - 1)[0] = 0; arm(P1, prots[pk]);
        r = p_strrchr((char*)(P1 - 64), 'x'); snprintf(tag, sizeof tag, "strrchr_term_at_end_%s", pn[pk]); printf("str_ret\t%s\t%ld\n", tag, r ? (long)(r - (char*)(P1 - 64)) : -1L); report(tag, P1);
        reset_region(); memset(P1 - 64, 'x', 63); (P1 - 1)[0] = 0; arm(P1, prots[pk]);
        r = p_strstr((char*)(P1 - 64), "xy"); snprintf(tag, sizeof tag, "strstr_term_at_end_%s", pn[pk]); printf("str_ret\t%s\t%p\n", tag, (void*)r); report(tag, P1);
        reset_region(); memset(P1 - 64, 'x', 64); arm(P1, prots[pk]);
        r = p_strchr((char*)(P1 - 64), 'q'); snprintf(tag, sizeof tag, "strchr_runs_in_%s", pn[pk]); printf("str_ret\t%s\t%p\n", tag, (void*)r); report(tag, P1);
        reset_region(); memset(P1 - 64, 'x', 64); arm(P1, prots[pk]);
        r = p_strchr((char*)(P1 - 64), 'x'); snprintf(tag, sizeof tag, "strchr_hit0_runs_in_%s", pn[pk]); printf("str_ret\t%s\t%ld\n", tag, r ? (long)(r - (char*)(P1 - 64)) : -1L); report(tag, P1);
        wchar_t* wp = (wchar_t*)(P1 - 64);
        reset_region(); for (int i = 0; i < 31; ++i) wp[i] = L'x'; wp[31] = 0; arm(P1, prots[pk]);
        wchar_t* wr = p_wcschr(wp, L'q'); snprintf(tag, sizeof tag, "wcschr_term_at_end_%s", pn[pk]); printf("str_ret\t%s\t%p\n", tag, (void*)wr); report(tag, P1);
        reset_region(); for (int i = 0; i < 31; ++i) wp[i] = L'x'; wp[31] = 0; arm(P1, prots[pk]);
        wr = p_wcsstr(wp, L"xy"); snprintf(tag, sizeof tag, "wcsstr_term_at_end_%s", pn[pk]); printf("str_ret\t%s\t%p\n", tag, (void*)wr); report(tag, P1);
        reset_region(); for (int i = 0; i < 32; ++i) wp[i] = L'x'; arm(P1, prots[pk]);
        wr = p_wcsrchr(wp, L'x'); snprintf(tag, sizeof tag, "wcsrchr_runs_in_%s", pn[pk]); printf("str_ret\t%s\t%p\n", tag, (void*)wr); report(tag, P1);
        // odd-aligned wide string ending at the page end
        wchar_t* wo = (wchar_t*)(P1 - 63);
        reset_region(); memset(P1 - 63, 'x', 63); memset(P1 - 3, 0, 3); arm(P1, prots[pk]);
        wr = p_wcschr(wo, L'q'); snprintf(tag, sizeof tag, "wcschr_odd_%s", pn[pk]); printf("str_ret\t%s\t%p\n", tag, (void*)wr); report(tag, P1);
    }
    return 0;
}
