// SetupAPI INF engine probe: SetupOpenInfFileA, SetupGetInfInformationA,
// SetupInstallFromInfSectionA with the default queue callback,
// SetupIterateCabinetA and the OS-component exports.
//
// Build: {x86_64,i686}-w64-mingw32-gcc -O1 -o sinf.exe setupapi_inf_probe.c -lsetupapi
// Run:   sinf.exe <workdir>   (workdir must hold src\probe.cab from
//        gen_setupapi_fixtures.py); paths in the output are masked as
//        <W> (workdir), <WIN> (%windir%) and <TMP> (%TEMP%).
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <string.h>

static char W[MAX_PATH], WIN[MAX_PATH], TMP[MAX_PATH];
static int bits = (int)(sizeof(void*) * 8);

static const char* mask(const char* s) {
    static char out[8][1024];
    static int slot;
    char* o = out[slot++ & 7];
    if (!s) return "(null)";
    if ((ULONG_PTR)s < 0x10000) { sprintf(o, "#%u", (unsigned)(ULONG_PTR)s); return o; }
    o[0] = 0;
    while (*s) {
        size_t lw = strlen(W), lwin = strlen(WIN), lt = strlen(TMP);
        if (lt && _strnicmp(s, TMP, lt) == 0) { strcat(o, "<TMP>"); s += lt; continue; }
        if (lw && _strnicmp(s, W, lw) == 0) { strcat(o, "<W>"); s += lw; continue; }
        if (lwin && _strnicmp(s, WIN, lwin) == 0) { strcat(o, "<WIN>"); s += lwin; continue; }
        size_t n = strlen(o); o[n] = *s++; o[n + 1] = 0;
    }
    return o;
}

static void write_file(const char* rel, const void* data, DWORD size) {
    char p[MAX_PATH];
    sprintf(p, "%s\\%s", W, rel);
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD done;
    WriteFile(h, data, size, &done, NULL);
    CloseHandle(h);
}

static void write_text(const char* rel, const char* text) { write_file(rel, text, (DWORD)strlen(text)); }

static void list_dir(const char* label, const char* dir) {
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    sprintf(pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    printf("  dir %s:", label);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.') continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) printf(" %s/", fd.cFileName);
            else printf(" %s=%lu", fd.cFileName, fd.nFileSizeLow);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    printf("\n");
}

static void clear_dir(const char* dir) {
    char pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAA fd;
    sprintf(pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        sprintf(p, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { clear_dir(p); RemoveDirectoryA(p); }
        else { SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL); DeleteFileA(p); }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ---- queue callback: log, then defer to the default callback --------------
static PVOID g_default;
static int g_skip_copy_of;
static int g_needmedia_doit;  // 1: answer NEEDMEDIA with FILEOP_DOIT whatever the default says  // 1: return FILEOP_SKIP on STARTCOPY instead of the default

static UINT CALLBACK queue_cb(PVOID ctx, UINT note, UINT_PTR p1, UINT_PTR p2) {
    printf("  cb ctx=%s note=%#x", ctx == (PVOID)0x5150 ? "ok" : "BAD", note);
    switch (note) {
    case SPFILENOTIFY_STARTQUEUE: case SPFILENOTIFY_ENDQUEUE:
    case SPFILENOTIFY_STARTSUBQUEUE: case SPFILENOTIFY_ENDSUBQUEUE:
        printf(" p1=%#llx p2=%#llx", (unsigned long long)p1, (unsigned long long)p2);
        break;
    case SPFILENOTIFY_STARTCOPY: case SPFILENOTIFY_ENDCOPY: case SPFILENOTIFY_COPYERROR:
    case SPFILENOTIFY_STARTDELETE: case SPFILENOTIFY_ENDDELETE: case SPFILENOTIFY_DELETEERROR:
    case SPFILENOTIFY_STARTRENAME: case SPFILENOTIFY_ENDRENAME: case SPFILENOTIFY_RENAMEERROR: {
        const FILEPATHS_A* fp = (const FILEPATHS_A*)p1;
        printf(" target=%s source=%s err=%u flags=%#lx p2=%#llx", mask(fp->Target),
               note >= SPFILENOTIFY_STARTCOPY ? mask(fp->Source) : "-", fp->Win32Error, fp->Flags,
               (unsigned long long)p2);
        break;
    }
    case SPFILENOTIFY_NEEDMEDIA: {
        const SOURCE_MEDIA_A* sm = (const SOURCE_MEDIA_A*)p1;
        printf(" tag=%s desc=%s path=%s file=%s flags=%#lx", mask(sm->Tagfile), mask(sm->Description),
               mask(sm->SourcePath), mask(sm->SourceFile), sm->Flags);
        break;
    }
    default:
        printf(" p1=%#llx p2=%#llx", (unsigned long long)p1, (unsigned long long)p2);
    }
    UINT r;
    if (note == SPFILENOTIFY_STARTCOPY && g_skip_copy_of) r = FILEOP_SKIP;
    else r = SetupDefaultQueueCallbackA(g_default, note, p1, p2);
    if (note == SPFILENOTIFY_NEEDMEDIA && g_needmedia_doit) {
        printf(" (default %u)", r);
        r = FILEOP_DOIT;
    }
    printf(" -> %u\n", r);
    return r;
}

static UINT CALLBACK null_ctx_cb(PVOID ctx, UINT note, UINT_PTR p1, UINT_PTR p2) {
    UINT r = SetupDefaultQueueCallbackA(NULL, note, p1, p2);
    printf("  nullctx note=%#x -> %u\n", note, r);
    return r;
}

static void print_version(const char* label, const char* path) {
    DWORD handle = 0, size = GetFileVersionInfoSizeA(path, &handle);
    if (size == 0) { printf("  %s version: none (err %#lx)\n", label, GetLastError()); return; }
    BYTE buf[4096];
    VS_FIXEDFILEINFO* fi = NULL;
    UINT len = 0;
    if (size <= sizeof buf && GetFileVersionInfoA(path, 0, size, buf) && VerQueryValueA(buf, "\\", (void**)&fi, &len))
        printf("  %s version: %lu.%lu\n", label, fi->dwFileVersionMS >> 16, fi->dwFileVersionMS & 0xffff);
}

static void install(HINF inf, const char* section, UINT flags, const char* root, UINT copy) {
    SetLastError(0xdead);
    BOOL ok = SetupInstallFromInfSectionA(NULL, inf, section, flags, NULL, root, copy, queue_cb,
                                          (PVOID)0x5150, NULL, NULL);
    printf("install %s flags=%#x root=%s copy=%#x -> %d lasterr=%#lx\n", section, flags, mask(root),
           copy, ok, GetLastError());
}

// ---- cabinet iteration ------------------------------------------------------
static int g_cab_mode;  // 0: extract 1st, skip 2nd; 1: abort on 1st; 2: return 0xdead on CABINETINFO

static UINT CALLBACK cab_cb(PVOID ctx, UINT note, UINT_PTR p1, UINT_PTR p2) {
    printf("  cab ctx=%s note=%#x", ctx == (PVOID)0x7070 ? "ok" : "BAD", note);
    UINT r = NO_ERROR;
    switch (note) {
    case SPFILENOTIFY_CABINETINFO: case SPFILENOTIFY_NEEDNEWCABINET: {
        const CABINET_INFO_A* ci = (const CABINET_INFO_A*)p1;
        printf(" path=%s file=%s disk=%s set=%u num=%u p2=%#llx", mask(ci->CabinetPath),
               mask(ci->CabinetFile), mask(ci->DiskName), ci->SetId, ci->CabinetNumber,
               (unsigned long long)p2);
        if (g_cab_mode == 2) r = 0xdead;
        break;
    }
    case SPFILENOTIFY_FILEINCABINET: {
        FILE_IN_CABINET_INFO_A* fi = (FILE_IN_CABINET_INFO_A*)p1;
        static int n;
        printf(" name=%s size=%lu err=%lu date=%#x time=%#x attr=%#x full=[%s] p2=%s", fi->NameInCabinet,
               fi->FileSize, fi->Win32Error, fi->DosDate, fi->DosTime, fi->DosAttribs,
               mask(fi->FullTargetName), mask((const char*)p2));
        if (g_cab_mode == 1) { SetLastError(0x1234); r = FILEOP_ABORT; break; }
        if (n++ % 2 == 0) {
            sprintf(fi->FullTargetName, "%s\\it\\%s", W, fi->NameInCabinet);
            r = FILEOP_DOIT;
        } else {
            r = FILEOP_SKIP;
        }
        break;
    }
    case SPFILENOTIFY_FILEEXTRACTED: {
        const FILEPATHS_A* fp = (const FILEPATHS_A*)p1;
        printf(" target=%s source=%s err=%u flags=%#lx p2=%#llx", mask(fp->Target), mask(fp->Source),
               fp->Win32Error, fp->Flags, (unsigned long long)p2);
        break;
    }
    default:
        printf(" p1=%#llx p2=%#llx", (unsigned long long)p1, (unsigned long long)p2);
    }
    printf(" -> %#x\n", r);
    return r;
}

static void iterate(const char* label, const char* cab) {
    SetLastError(0xdead);
    BOOL ok = SetupIterateCabinetA(cab, 0, cab_cb, (PVOID)0x7070);
    printf("iterate %s mode=%d -> %d lasterr=%#lx\n", label, g_cab_mode, ok, GetLastError());
}

// ---- INF texts ---------------------------------------------------------------
static const char kProbeInf[] =
    "; probe INF\r\n"
    "[Version]\r\n"
    "Signature=\"$Windows NT$\"\r\n"
    "Class=VitreoProbe\r\n"
    "AdvancedINF=2.0\r\n"
    "Provider=%Prov%\r\n"
    "DriverVer = 02-04-2010, 5.3.1.0904\r\n"
    "CatalogFile=probe.cat ; comment\r\n"
    "Quoted=\"a;b\",  \"c\"  d , %%Lit%% %Unknown%\r\n"
    "\r\n"
    "[SourceDisksNames]\r\n"
    "1=%Disk%,,,\r\n"
    "2=%Disk%,\"probe.cab\",,\r\n"
    "3=%Disk%,\"absent.cab\",0,,-1\r\n"
    "4=%Disk%,\"flag.cab\",0,,-1\r\n"
    "5=%Disk%,\"flag.cab\",0,,0x10\r\n"
    "6=%Disk%,\"absent.cab\",0,,0x10\r\n"
    "\r\n"
    "[SourceDisksFiles]\r\n"
    "loose.txt=1\r\n"
    "incab1.txt=2\r\n"
    "incab2.txt=2\r\n"
    "missing.txt=1\r\n"
    "ver1.dll=1\r\n"
    "flagloose.txt=3\r\n"
    "flagcab.txt=4\r\n"
    "flag10.txt=5\r\n"
    "flag10loose.txt=6\r\n"
    "ver2.dll=1\r\n"
    "\r\n"
    "[DestinationDirs]\r\n"
    "DefaultDestDir=10,\"Temp\\vprobe%BITS%\"\r\n"
    "Sub_Files=10,Temp\\vprobe%BITS%\\sub\r\n"
    "Sys_Files=11\r\n"
    "Wow_Files=10,SysWOW64\r\n"
    "\r\n"
    "[Loose_Install]\r\nCopyFiles=Loose_Files\r\n"
    "[Loose_Files]\r\nloose.txt\r\nrenamed.txt,loose.txt\r\n"
    "[Cab_Install]\r\nCopyFiles=Cab_Files\r\n"
    "[Cab_Files]\r\nincab1.txt\r\nincab2.txt\r\n"
    "[Missing_Install]\r\nCopyFiles=Missing_Files\r\n"
    "[Missing_Files]\r\nmissing.txt\r\n"
    "[Direct_Install]\r\nCopyFiles=@loose.txt\r\n"
    "[Sub_Install]\r\nCopyFiles=Sub_Files\r\n"
    "[Sub_Files]\r\nloose.txt\r\n"
    "[Both_Install]\r\nCopyFiles=Loose_Files, Sub_Files\r\n"
    "[NoOver_Install]\r\nCopyFiles=NoOver_Files\r\n"
    "[NoOver_Files]\r\nloose.txt,,,0x8\r\n"
    "[Del_Install]\r\nDelFiles=Del_Files\r\n"
    "[Del_Files]\r\nrenamed.txt\r\n"
    "[Reg_Install]\r\nAddReg=Reg_Add\r\n"
    "[Reg_Add]\r\n"
    "HKCU,\"Software\\VitreoSetupProbe\",Str,,\"%Prov%\"\r\n"
    "HKCU,\"Software\\VitreoSetupProbe\",Dw,0x10001,0x1234\r\n"
    "[RegDel_Install]\r\nDelReg=Reg_Del\r\n"
    "[Reg_Del]\r\nHKCU,\"Software\\VitreoSetupProbe\"\r\n"
    "[Empty_Install]\r\n"
    "[FlagLoose_Install]\r\nCopyFiles=FlagLoose_Files\r\n"
    "[FlagLoose_Files]\r\nflagloose.txt\r\n"
    "[FlagCab_Install]\r\nCopyFiles=FlagCab_Files\r\n"
    "[FlagCab_Files]\r\nflagcab.txt\r\n"
    "[Flag10_Install]\r\nCopyFiles=Flag10_Files\r\n"
    "[Flag10_Files]\r\nflag10.txt\r\n"
    "[Flag10Loose_Install]\r\nCopyFiles=Flag10Loose_Files\r\n"
    "[Flag10Loose_Files]\r\nflag10loose.txt\r\n"
    "[V1_Install]\r\nCopyFiles=V1_Files\r\n"
    "[V1_Files]\r\nver.dll,ver1.dll\r\n"
    "[V2_Install]\r\nCopyFiles=V2_Files\r\n"
    "[V2_Files]\r\nver.dll,ver2.dll\r\n"
    "[DxStyle]\r\n"
    "dxdllreg.exe -PATCH\r\n"
    "infinst.exe X3DAudio1_7_x64_xp.inf\r\n"
    "d3dx9_24_x86.inf, x64_Install\r\n"
    "dxdllreg_x86.cab,                  2\r\n"
    "CopyCount=1\r\n"
    "Size=21 ;approximately\r\n"
    "Q = \"a,b\" , c ,, \r\n"
    "%Disk% = %Prov%, \"x%Disk%y\", %%Lit%%, %Unknown%\r\n"
    "Num=0x1F, -3, 12abc, , 077\r\n"
    "%11%\\xactengine2_0.dll\r\n"
    "Dirs=%10%,%11%,%12%,%17%,%1%,%01%,%24%,%16422%,%16425%,%16426%,%-1%,%99999%,\"%11%\"\r\n"
    "[Sys_Install]\r\nCopyFiles=Sys_Files, Wow_Files, NoDest_Files\r\n"
    "[Sys_Files]\r\nloose.txt\r\n"
    "[Wow_Files]\r\nloose.txt\r\n"
    "[NoDest_Files]\r\nloose.txt\r\n"
    "[Cont_Install]\r\nCopyFiles = Loose_Files,\\\r\n   Sub_Files ; trailing comment\r\n"
    "\r\n"
    "[Strings]\r\n"
    "Prov=\"Vitreo \"\"Probe\"\"\"\r\n"
    "Disk=\"Probe Disk\"\r\n";

static void print_line(const char* label, INFCONTEXT* ctx) {
    char text[512];
    DWORD req = 0;
    SetLastError(0xdead);
    BOOL ok = SetupGetLineTextA(ctx, NULL, NULL, NULL, text, sizeof text, &req);
    DWORD n = SetupGetFieldCount(ctx);
    printf("  %s line=%u text-> %d lasterr=%#lx req=%lu [%s] fields=%lu:", label, ctx->Line, ok, GetLastError(), req,
           ok ? text : "", n);
    for (DWORD f = 0; f <= n + 1; ++f) {
        char field[256];
        DWORD fr = 0;
        SetLastError(0xdead);
        BOOL fo = SetupGetStringFieldA(ctx, f, field, sizeof field, &fr);
        if (fo) printf(" %lu=[%s]/%lu", f, field, fr);
        else printf(" %lu=!%#lx", f, GetLastError());
        INT iv = 0x5555;
        SetLastError(0xdead);
        BOOL io = SetupGetIntField(ctx, f, &iv);
        if (io) printf("(int %d)", iv);
        else printf("(int !%#lx)", GetLastError());
    }
    printf("\n");
}

static void find_case(HINF h, const char* section, const char* key) {
    INFCONTEXT ctx;
    memset(&ctx, 0xcc, sizeof ctx);
    SetLastError(0xdead);
    BOOL ok = SetupFindFirstLineA(h, section, key, &ctx);
    printf("find %s/%s -> %d lasterr=%#lx\n", section ? section : "(null)", key ? key : "(null)", ok, GetLastError());
    if (!ok) return;
    int guard = 0;
    do {
        print_line("  ", &ctx);
        SetLastError(0xdead);
    } while (SetupFindNextLine(&ctx, &ctx) && ++guard < 20);
    printf("  next-end lasterr=%#lx\n", GetLastError());
}

static void print_info_buffer(const char* label, PSP_INF_INFORMATION info, DWORD size) {
    printf("%s style=%lu count=%lu bytes=%lu data:", label, info->InfStyle, info->InfCount, size);
    const BYTE* d = (const BYTE*)info;
    for (DWORD i = 8; i < size; ++i) {
        BYTE c = d[i];
        if (c >= 0x20 && c < 0x7f && c != '\\') printf("%c", c);
        else printf("\\x%02x", c);
    }
    printf("\n");
}

static void open_case(const char* label, const char* rel, const char* cls, DWORD style) {
    char p[MAX_PATH];
    UINT line = 0xbeef;
    if (rel[0] == '!') strcpy(p, rel + 1);
    else sprintf(p, "%s\\src\\%s", W, rel);
    SetLastError(0xdead);
    HINF h = SetupOpenInfFileA(p, cls, style, &line);
    DWORD le = GetLastError();
    printf("open %s class=%s style=%#lx -> %s lasterr=%#lx line=%#x\n", label, cls ? cls : "(null)", style,
           h == INVALID_HANDLE_VALUE ? "INVALID" : "ok", le, line);
    if (h != INVALID_HANDLE_VALUE) SetupCloseInfFile(h);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    strcpy(W, argv[1]);
    GetWindowsDirectoryA(WIN, MAX_PATH);
    GetTempPathA(MAX_PATH, TMP);
    if (TMP[0] && TMP[strlen(TMP) - 1] == '\\') TMP[strlen(TMP) - 1] = 0;
    printf("bits=%d\n", bits);

    // Probe INF with %BITS% replaced (the 32- and 64-bit runs use distinct dirs).
    char inf[sizeof(kProbeInf) + 32], *o = inf;
    for (const char* s = kProbeInf; *s;) {
        if (strncmp(s, "%BITS%", 6) == 0) { o += sprintf(o, "%d", bits); s += 6; }
        else *o++ = *s++;
    }
    *o = 0;
    write_text("src\\probe.inf", inf);
    write_text("src\\loose.txt", "loose file\r\n");
    write_text("src\\noversion.inf", "[Foo]\r\nx=1\r\n");
    write_text("src\\badsection.inf", "[Version]\r\nSignature=\"$Windows NT$\"\r\n[Broken\r\nx=1\r\n");
    write_text("src\\chicago.inf", "[Version]\r\nSignature=\"$CHICAGO$\"\r\nAdvancedINF=2.0\r\n");
    write_text("src\\empty.inf", "");
    write_text("src\\oldnt.inf", "[Identification]\r\nOptionType=NetAdapter\r\n");
    {
        static const WCHAR uni[] = L"\xfeff[Version]\r\nSignature=\"$Windows NT$\"\r\nClass=VitreoProbe\r\n";
        write_file("src\\unicode.inf", uni, sizeof(uni) - sizeof(WCHAR));
    }

    // ---- SetupOpenInfFileA ----
    open_case("probe", "probe.inf", NULL, INF_STYLE_WIN4);
    open_case("probe-class", "probe.inf", "VitreoProbe", INF_STYLE_WIN4);
    open_case("probe-class-case", "probe.inf", "vitreoprobe", INF_STYLE_WIN4);
    open_case("probe-wrongclass", "probe.inf", "Other", INF_STYLE_WIN4);
    open_case("probe-oldnt", "probe.inf", NULL, INF_STYLE_OLDNT);
    open_case("probe-both", "probe.inf", NULL, INF_STYLE_OLDNT | INF_STYLE_WIN4);
    open_case("probe-style0", "probe.inf", NULL, 0);
    open_case("missing", "nope.inf", NULL, INF_STYLE_WIN4);
    open_case("relative", "!probe.inf", NULL, INF_STYLE_WIN4);
    open_case("noversion", "noversion.inf", NULL, INF_STYLE_WIN4);
    open_case("badsection", "badsection.inf", NULL, INF_STYLE_WIN4);
    open_case("chicago", "chicago.inf", NULL, INF_STYLE_WIN4);
    open_case("empty", "empty.inf", NULL, INF_STYLE_WIN4);
    open_case("oldnt-file-win4", "oldnt.inf", NULL, INF_STYLE_WIN4);
    open_case("oldnt-file-oldnt", "oldnt.inf", NULL, INF_STYLE_OLDNT);
    open_case("unicode", "unicode.inf", "VitreoProbe", INF_STYLE_WIN4);

    // ---- SetupGetInfInformationA ----
    char path[MAX_PATH];
    sprintf(path, "%s\\src\\probe.inf", W);
    HINF inf_h = SetupOpenInfFileA(path, NULL, INF_STYLE_WIN4, NULL);
    {
        BYTE buf[4096];
        DWORD req = 0xbeef;
        SetLastError(0xdead);
        BOOL ok = SetupGetInfInformationA(inf_h, INFINFO_INF_SPEC_IS_HINF, NULL, 0, &req);
        printf("info hinf size-query -> %d lasterr=%#lx req=%lu\n", ok, GetLastError(), req);
        DWORD need = req;
        SetLastError(0xdead);
        ok = SetupGetInfInformationA(inf_h, INFINFO_INF_SPEC_IS_HINF, (PSP_INF_INFORMATION)buf, need - 1, &req);
        printf("info hinf short -> %d lasterr=%#lx req=%lu\n", ok, GetLastError(), req);
        memset(buf, 0xcc, sizeof buf);
        SetLastError(0xdead);
        ok = SetupGetInfInformationA(inf_h, INFINFO_INF_SPEC_IS_HINF, (PSP_INF_INFORMATION)buf, sizeof buf, &req);
        printf("info hinf full -> %d lasterr=%#lx req=%lu\n", ok, GetLastError(), req);
        print_info_buffer("  hinf", (PSP_INF_INFORMATION)buf, req);
        {
            char ver[256];
            DWORD vr = 0;
            SetLastError(0xdead);
            ok = SetupQueryInfVersionInformationA((PSP_INF_INFORMATION)buf, 0, "Provider", ver, sizeof ver, &vr);
            printf("  query-version Provider -> %d lasterr=%#lx req=%lu value=[%s]\n", ok, GetLastError(), vr,
                   ok ? ver : "");
            ok = SetupQueryInfFileInformationA((PSP_INF_INFORMATION)buf, 0, ver, sizeof ver, &vr);
            printf("  query-file -> %d req=%lu value=[%s]\n", ok, vr, ok ? mask(ver) : "");
        }
        memset(buf, 0xcc, sizeof buf);
        SetLastError(0xdead);
        ok = SetupGetInfInformationA(path, INFINFO_INF_NAME_IS_ABSOLUTE, (PSP_INF_INFORMATION)buf, sizeof buf, &req);
        printf("info name -> %d lasterr=%#lx req=%lu\n", ok, GetLastError(), req);
        if (ok) print_info_buffer("  name", (PSP_INF_INFORMATION)buf, req);
        sprintf(path, "%s\\src\\chicago.inf", W);
        ok = SetupGetInfInformationA(path, INFINFO_INF_NAME_IS_ABSOLUTE, (PSP_INF_INFORMATION)buf, sizeof buf, &req);
        printf("info chicago -> %d lasterr=%#lx req=%lu\n", ok, GetLastError(), req);
        if (ok) print_info_buffer("  chicago", (PSP_INF_INFORMATION)buf, req);
        sprintf(path, "%s\\src\\nope.inf", W);
        SetLastError(0xdead);
        ok = SetupGetInfInformationA(path, INFINFO_INF_NAME_IS_ABSOLUTE, (PSP_INF_INFORMATION)buf, sizeof buf, &req);
        printf("info missing -> %d lasterr=%#lx\n", ok, GetLastError());
        SetLastError(0xdead);
        ok = SetupGetInfInformationA(inf_h, 99, (PSP_INF_INFORMATION)buf, sizeof buf, &req);
        printf("info badsearch -> %d lasterr=%#lx\n", ok, GetLastError());
    }

    // ---- INFCONTEXT readers ----
    find_case(inf_h, "Version", NULL);
    find_case(inf_h, "SourceDisksNames", "2");
    find_case(inf_h, "Loose_Files", NULL);
    find_case(inf_h, "DxStyle", NULL);
    find_case(inf_h, "NoSuch", NULL);
    find_case(inf_h, "Empty_Install", NULL);
    find_case(inf_h, "Version", "NoKey");
    find_case(inf_h, "version", "signature");
    {
        INFCONTEXT ctx, next;
        char text[64];
        DWORD req = 0;
        SetLastError(0xdead);
        BOOL ok = SetupGetLineTextA(NULL, inf_h, "Strings", "Prov", NULL, 0, &req);
        printf("linetext by key size-query -> %d lasterr=%#lx req=%lu\n", ok, GetLastError(), req);
        SetLastError(0xdead);
        ok = SetupGetLineTextA(NULL, inf_h, "Strings", "Prov", text, 4, &req);
        printf("linetext short -> %d lasterr=%#lx req=%lu\n", ok, GetLastError(), req);
        SetLastError(0xdead);
        ok = SetupGetLineTextA(NULL, inf_h, "Strings", "Nope", text, sizeof text, &req);
        printf("linetext missing key -> %d lasterr=%#lx\n", ok, GetLastError());
        SetupFindFirstLineA(inf_h, "Version", NULL, &ctx);
        SetLastError(0xdead);
        ok = SetupFindNextMatchLineA(&ctx, "Provider", &next);
        printf("nextmatch Provider -> %d lasterr=%#lx line=%u\n", ok, GetLastError(), next.Line);
        SetLastError(0xdead);
        ok = SetupFindNextMatchLineA(&ctx, "Nope", &next);
        printf("nextmatch Nope -> %d lasterr=%#lx\n", ok, GetLastError());
        SetLastError(0xdead);
        LONG lc = SetupGetLineCountA(inf_h, "DxStyle");
        printf("linecount DxStyle -> %ld lasterr=%#lx\n", lc, GetLastError());
        SetLastError(0xdead);
        lc = SetupGetLineCountA(inf_h, "NoSuch");
        printf("linecount NoSuch -> %ld lasterr=%#lx\n", lc, GetLastError());
        SetLastError(0xdead);
        ok = SetupGetLineByIndexA(inf_h, "DxStyle", 2, &ctx);
        printf("byindex 2 -> %d lasterr=%#lx line=%u\n", ok, GetLastError(), ctx.Line);
        SetLastError(0xdead);
        ok = SetupGetLineByIndexA(inf_h, "DxStyle", 99, &ctx);
        printf("byindex 99 -> %d lasterr=%#lx\n", ok, GetLastError());
        printf("ctx-size %u\n", (unsigned)sizeof(INFCONTEXT));
    }

    // ---- SetupInstallFromInfSectionA ----
    char dest[MAX_PATH], src[MAX_PATH];
    sprintf(dest, "%s\\Temp\\vprobe%d", WIN, bits);
    sprintf(src, "%s\\src", W);
    CreateDirectoryA(dest, NULL);
    clear_dir(dest);
    g_default = SetupInitDefaultQueueCallbackEx(NULL, INVALID_HANDLE_VALUE, 0, 0, NULL);
    printf("default-ctx %s\n", g_default ? "ok" : "NULL");

    install(inf_h, "Loose_Install", SPINST_FILES, src, 0);
    list_dir("dest", dest);
    install(inf_h, "Loose_Install", SPINST_FILES, NULL, 0);
    list_dir("dest", dest);
    install(inf_h, "NoOver_Install", SPINST_FILES, src, 0);
    install(inf_h, "Sub_Install", SPINST_FILES, src, 0);
    {
        char sub[MAX_PATH];
        sprintf(sub, "%s\\sub", dest);
        list_dir("dest/sub", sub);
    }
    install(inf_h, "Both_Install", SPINST_FILES, src, 0);
    clear_dir(dest);
    install(inf_h, "Direct_Install", SPINST_FILES, src, 0);
    list_dir("dest", dest);
    clear_dir(dest);
    install(inf_h, "Cab_Install", SPINST_FILES, src, 0);
    list_dir("dest", dest);
    list_dir("src", src);
    clear_dir(dest);
    g_skip_copy_of = 1;
    install(inf_h, "Loose_Install", SPINST_FILES, src, 0);
    g_skip_copy_of = 0;
    list_dir("dest", dest);
    install(inf_h, "Loose_Install", SPINST_FILES, src, 0);
    install(inf_h, "Del_Install", SPINST_FILES, src, 0);
    list_dir("dest", dest);
    install(inf_h, "Loose_Install", SPINST_REGISTRY, src, 0);
    list_dir("dest", dest);
    install(inf_h, "Empty_Install", SPINST_ALL, src, 0);
    install(inf_h, "NoSuch_Install", SPINST_FILES, src, 0);
    install(inf_h, "NoSuch_Install", SPINST_REGISTRY, src, 0);
    install(inf_h, "Loose_Install", 0, src, 0);
    install(inf_h, "Reg_Install", SPINST_REGISTRY, src, 0);
    {
        HKEY k;
        char v[64] = "";
        DWORD d = 0, sz = sizeof v, type = 0;
        LONG r = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\VitreoSetupProbe", 0, KEY_READ, &k);
        printf("  reg open -> %ld\n", r);
        if (r == 0) {
            r = RegQueryValueExA(k, "Str", NULL, &type, (BYTE*)v, &sz);
            printf("  reg Str -> %ld type=%lu size=%lu [%s]\n", r, type, sz, v);
            sz = sizeof d;
            r = RegQueryValueExA(k, "Dw", NULL, &type, (BYTE*)&d, &sz);
            printf("  reg Dw -> %ld type=%lu size=%lu %#lx\n", r, type, sz, d);
            RegCloseKey(k);
        }
    }
    install(inf_h, "RegDel_Install", SPINST_REGISTRY, src, 0);
    {
        HKEY k;
        LONG r = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\VitreoSetupProbe", 0, KEY_READ, &k);
        printf("  reg open after DelReg -> %ld\n", r);
        if (r == 0) RegCloseKey(k);
    }
    g_skip_copy_of = 1;
    install(inf_h, "Sys_Install", SPINST_FILES, src, 0);
    clear_dir(dest);
    install(inf_h, "Cont_Install", SPINST_FILES, src, 0);
    g_skip_copy_of = 0;
    SetLastError(0xdead);
    {
        BOOL ok = SetupInstallFromInfSectionA(NULL, inf_h, "Loose_Install", SPINST_FILES, NULL, src, 0,
                                              null_ctx_cb, NULL, NULL, NULL);
        printf("install nullctx -> %d lasterr=%#lx\n", ok, GetLastError());
        list_dir("dest", dest);
    }
    {
        char ver[MAX_PATH];
        sprintf(ver, "%s\\ver.dll", dest);
        static const UINT styles[] = {SP_COPY_NEWER, 0x144, SP_COPY_NEWER_ONLY, SP_COPY_FORCE_NEWER, SP_COPY_NOOVERWRITE,
                                      SP_COPY_SOURCE_ABSOLUTE, SP_COPY_IN_USE_NEEDS_REBOOT, SP_COPY_FORCE_NOOVERWRITE};
        for (int i = 0; i < 8; ++i) {
            DeleteFileA(ver);
            printf("-- style %#x, target absent\n", styles[i]);
            install(inf_h, "V1_Install", SPINST_FILES, src, styles[i]);
            print_version("dest", ver);
            printf("-- style %#x, target v2, source v1\n", styles[i]);
            install(inf_h, "V2_Install", SPINST_FILES, src, 0);
            install(inf_h, "V1_Install", SPINST_FILES, src, styles[i]);
            print_version("dest", ver);
            printf("-- style %#x, target v1, source v1\n", styles[i]);
            DeleteFileA(ver);
            install(inf_h, "V1_Install", SPINST_FILES, src, 0);
            install(inf_h, "V1_Install", SPINST_FILES, src, styles[i]);
            print_version("dest", ver);
        }
        DeleteFileA(ver);
    }
    write_text("src\\flagloose.txt", "flag loose\r\n");
    write_text("src\\flag10loose.txt", "flag10 loose\r\n");
    clear_dir(dest);
    install(inf_h, "FlagLoose_Install", SPINST_FILES, src, 0);
    install(inf_h, "FlagCab_Install", SPINST_FILES, src, 0);
    install(inf_h, "Flag10_Install", SPINST_FILES, src, 0);
    install(inf_h, "Flag10Loose_Install", SPINST_FILES, src, 0);
    list_dir("dest", dest);
    g_needmedia_doit = 1;
    install(inf_h, "FlagLoose_Install", SPINST_FILES, src, 0);
    install(inf_h, "Flag10Loose_Install", SPINST_FILES, src, 0);
    g_needmedia_doit = 0;
    list_dir("dest", dest);
    install(inf_h, "Missing_Install", SPINST_FILES, src, 0);
    list_dir("dest", dest);
    SetupTermDefaultQueueCallback(g_default);
    clear_dir(dest);
    RemoveDirectoryA(dest);
    SetupCloseInfFile(inf_h);

    // ---- SetupIterateCabinetA ----
    char cab[MAX_PATH], it[MAX_PATH];
    sprintf(cab, "%s\\src\\probe.cab", W);
    sprintf(it, "%s\\it", W);
    CreateDirectoryA(it, NULL);
    clear_dir(it);
    g_cab_mode = 0;
    iterate("probe", cab);
    list_dir("it", it);
    g_cab_mode = 1;
    iterate("probe", cab);
    g_cab_mode = 2;
    iterate("probe", cab);
    g_cab_mode = 0;
    iterate("text", path[0] ? (sprintf(path, "%s\\src\\loose.txt", W), path) : path);
    sprintf(path, "%s\\src\\nope.cab", W);
    iterate("missing", path);

    // ---- OS-component exports (absent from the current SDK headers) ----
    HMODULE sa = GetModuleHandleA("setupapi.dll");
    const char* names[] = {"SetupQueryRegisteredOsComponent", "SetupRegisterOsComponent",
                           "SetupUnRegisterOsComponent", "SetupQueryRegisteredOsComponentsOrder",
                           "SetupEnumInfSectionsA"};
    for (int i = 0; i < 5; ++i) {
        const BYTE* f = (const BYTE*)GetProcAddress(sa, names[i]);
        printf("export %s %s", names[i], f ? "present" : "absent");
        if (f) {
            printf(" code:");
            for (int j = 0; j < 24; ++j) printf(" %02x", f[j]);
        }
        printf("\n");
    }
    {
        typedef BOOL (WINAPI *QueryFn)(GUID*, void*, void*);
        QueryFn q = (QueryFn)GetProcAddress(sa, "SetupQueryRegisteredOsComponent");
        if (q) {
            static BYTE a[4096], b[4096];
            GUID g = {0xBF7F2196, 0x4D7C, 0x4AC1, {0x8B, 0x1E, 0x41, 0xD3, 0x15, 0x3E, 0x09, 0xE7}};
            DWORD sizes[] = {0, 0x14c, 0x150, 0x22c, 0x234, 0x1000};
            for (int i = 0; i < 6; ++i) {
                memset(a, 0, sizeof a); memset(b, 0, sizeof b);
                *(DWORD*)a = sizes[i]; *(DWORD*)b = sizes[i];
                SetLastError(0xdead);
                BOOL r = q(&g, a, b);
                printf("query-os size=%#lx -> %d lasterr=%#lx a0=%#lx b0=%#lx\n", sizes[i], r, GetLastError(),
                       *(DWORD*)a, *(DWORD*)b);
            }
        }
    }
    return 0;
}
