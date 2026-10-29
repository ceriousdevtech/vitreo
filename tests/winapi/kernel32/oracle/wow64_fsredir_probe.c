// WOW64 file-system redirection probe (Learn: "File System Redirector").
// Build: {x86_64,i686}-w64-mingw32-gcc -O1 -o fsr{64,32}.exe wow64_fsredir_probe.c
// Run (elevated winoracle): fsr64.exe setup ; fsr32.exe ; fsr64.exe ; fsr64.exe cleanup
// `setup` writes a marker file "vprobe_fsr.txt" holding "native" under
// %windir%\System32 and each exempt subdirectory, and "wow" under the
// matching SysWOW64 paths; each probe line says which one a path reaches.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static const char* kDirs[] = {"", "\\catroot", "\\catroot2", "\\drivers\\etc", "\\driverstore", "\\logfiles",
                              "\\spool", "\\drivers", "\\config", "\\wbem"};

static void put(const char* path, const char* text) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD done;
    if (h == INVALID_HANDLE_VALUE) { printf("setup-fail %s %lu\n", path, GetLastError()); return; }
    WriteFile(h, text, (DWORD)strlen(text), &done, NULL);
    CloseHandle(h);
}

static const char* reach(const char* path) {
    static char buf[32];
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { sprintf(buf, "err%lu", GetLastError()); return buf; }
    DWORD n = 0;
    memset(buf, 0, sizeof buf);
    ReadFile(h, buf, sizeof buf - 1, &n, NULL);
    CloseHandle(h);
    return buf;
}

static void line(const char* label, const char* path) { printf("%-28s %s -> %s\n", label, path, reach(path)); }

static DWORD WINAPI other_thread(LPVOID unused) {
    (void)unused;
    line("per-thread other", "C:\\Windows\\System32\\vprobe_fsr.txt");
    return 0;
}

int main(int argc, char** argv) {
    char win[MAX_PATH], p[MAX_PATH], q[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);
    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    PVOID old = NULL;
    if (argc > 1 && strcmp(argv[1], "setup") == 0) {
        for (int i = 0; i < 10; ++i) {
            sprintf(p, "%s\\System32%s\\vprobe_fsr.txt", win, kDirs[i]);
            put(p, "native");
            sprintf(p, "%s\\SysWOW64%s", win, kDirs[i]);
            CreateDirectoryA(p, NULL);
            sprintf(p, "%s\\SysWOW64%s\\vprobe_fsr.txt", win, kDirs[i]);
            put(p, "wow");
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "cleanup") == 0) {
        for (int i = 0; i < 10; ++i) {
            sprintf(p, "%s\\System32%s\\vprobe_fsr.txt", win, kDirs[i]);
            DeleteFileA(p);
            sprintf(p, "%s\\SysWOW64%s\\vprobe_fsr.txt", win, kDirs[i]);
            DeleteFileA(p);
        }
        sprintf(p, "%s\\SysWOW64\\vprobe_fsr_dir", win);
        RemoveDirectoryA(p);
        sprintf(p, "%s\\System32\\vprobe_fsr_dir", win);
        RemoveDirectoryA(p);
        return 0;
    }
    printf("bits=%d wow64=%d\n", (int)(sizeof(void*) * 8), wow);
    for (int i = 0; i < 10; ++i) {
        sprintf(p, "C:\\Windows\\System32%s\\vprobe_fsr.txt", kDirs[i]);
        line(i == 0 ? "system32" : kDirs[i] + 1, p);
    }
    line("sysnative", "C:\\Windows\\Sysnative\\vprobe_fsr.txt");
    line("sysnative-etc", "C:\\Windows\\Sysnative\\drivers\\etc\\vprobe_fsr.txt");
    line("syswow64", "C:\\Windows\\SysWOW64\\vprobe_fsr.txt");
    line("upper", "C:\\WINDOWS\\SYSTEM32\\VPROBE_FSR.TXT");
    line("slash", "C:/Windows/System32/vprobe_fsr.txt");
    line("dotdot", "C:\\Windows\\Temp\\..\\System32\\vprobe_fsr.txt");
    line("long-prefix", "\\\\?\\C:\\Windows\\System32\\vprobe_fsr.txt");
    line("nt-global", "\\\\.\\C:\\Windows\\System32\\vprobe_fsr.txt");
    {
        DWORD a = GetFileAttributesA("C:\\Windows\\Sysnative");
        printf("%-28s attrs(Sysnative) -> %#lx err=%lu\n", "sysnative-dir", a, a == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0);
    }
    // Current directory inside System32.
    SetCurrentDirectoryA("C:\\Windows\\System32");
    GetCurrentDirectoryA(MAX_PATH, q);
    printf("%-28s cwd=%s\n", "cwd", q);
    line("relative", "vprobe_fsr.txt");
    line("relative-etc", "drivers\\etc\\vprobe_fsr.txt");
    SetCurrentDirectoryA("C:\\");
    // FindFirstFile sees the redirected directory.
    {
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA("C:\\Windows\\System32\\vprobe_fsr.txt", &fd);
        printf("%-28s find -> %s size=%lu\n", "find", f == INVALID_HANDLE_VALUE ? "none" : fd.cFileName,
               f == INVALID_HANDLE_VALUE ? 0 : fd.nFileSizeLow);
        if (f != INVALID_HANDLE_VALUE) FindClose(f);
    }
    // CreateDirectory lands in the redirected view.
    {
        BOOL c = CreateDirectoryA("C:\\Windows\\System32\\vprobe_fsr_dir", NULL);
        sprintf(p, "%s\\SysWOW64\\vprobe_fsr_dir", win);
        sprintf(q, "%s\\System32\\vprobe_fsr_dir", win);
        printf("%-28s create=%d wow-exists=%d", "mkdir", c, GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES);
        PVOID o2;
        Wow64DisableWow64FsRedirection(&o2);
        printf(" native-exists=%d\n", GetFileAttributesA(q) != INVALID_FILE_ATTRIBUTES);
        Wow64RevertWow64FsRedirection(o2);
        RemoveDirectoryA("C:\\Windows\\System32\\vprobe_fsr_dir");
    }
    // GetFullPathName / GetSystemDirectory are not redirected.
    GetFullPathNameA("C:\\Windows\\System32\\x.dll", MAX_PATH, p, NULL);
    printf("%-28s %s\n", "fullpath", p);
    GetSystemDirectoryA(p, MAX_PATH);
    printf("%-28s %s\n", "sysdir", p);
    UINT n = GetSystemWow64DirectoryA(p, MAX_PATH);
    printf("%-28s ret=%u %s err=%lu\n", "syswow64dir", n, n ? p : "", n ? 0 : GetLastError());

    // Disable / revert / enable.
    old = (PVOID)0x1234;
    SetLastError(0xdead);
    BOOL d = Wow64DisableWow64FsRedirection(&old);
    printf("%-28s ret=%d err=%#lx old=%p\n", "disable", d, GetLastError(), old);
    line("disabled-system32", "C:\\Windows\\System32\\vprobe_fsr.txt");
    line("disabled-sysnative", "C:\\Windows\\Sysnative\\vprobe_fsr.txt");
    PVOID old2 = (PVOID)0x5678;
    SetLastError(0xdead);
    d = Wow64DisableWow64FsRedirection(&old2);
    printf("%-28s ret=%d err=%#lx old=%p\n", "disable-again", d, GetLastError(), old2);
    SetLastError(0xdead);
    BOOL r = Wow64RevertWow64FsRedirection(old2);
    printf("%-28s ret=%d err=%#lx\n", "revert-inner", r, GetLastError());
    line("after-inner", "C:\\Windows\\System32\\vprobe_fsr.txt");
    SetLastError(0xdead);
    r = Wow64RevertWow64FsRedirection(old);
    printf("%-28s ret=%d err=%#lx\n", "revert", r, GetLastError());
    line("reverted", "C:\\Windows\\System32\\vprobe_fsr.txt");
    SetLastError(0xdead);
    BOOLEAN e = Wow64EnableWow64FsRedirection(FALSE);
    printf("%-28s ret=%d err=%#lx\n", "enable(FALSE)", e, GetLastError());
    line("enable-false", "C:\\Windows\\System32\\vprobe_fsr.txt");
    SetLastError(0xdead);
    e = Wow64EnableWow64FsRedirection(TRUE);
    printf("%-28s ret=%d err=%#lx\n", "enable(TRUE)", e, GetLastError());
    line("enable-true", "C:\\Windows\\System32\\vprobe_fsr.txt");
    SetLastError(0xdead);
    d = Wow64DisableWow64FsRedirection(NULL);
    printf("%-28s ret=%d err=%#lx\n", "disable(NULL)", d, GetLastError());
    if (d) Wow64RevertWow64FsRedirection(NULL);
    // Redirection is per thread.
    {
        PVOID o3;
        Wow64DisableWow64FsRedirection(&o3);
        HANDLE t = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        printf("%-28s main-thread -> %s\n", "per-thread", reach("C:\\Windows\\System32\\vprobe_fsr.txt"));
        Wow64RevertWow64FsRedirection(o3);
    }
    return 0;
}
