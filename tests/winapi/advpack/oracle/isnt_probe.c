// advpack!IsNTAdmin probe (elevated winoracle session). Built with
// {x86_64,i686}-w64-mingw32-gcc -O1 -o isnt.exe isnt_probe.c -lshell32; both
// bitnesses printed isnt_probe.expected.
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
typedef BOOL (WINAPI *IsNTAdminFn)(DWORD, DWORD*);
int main(void) {
    HMODULE h = LoadLibraryA("advpack.dll");
    IsNTAdminFn f = (IsNTAdminFn)GetProcAddress(h, "IsNTAdmin");
    FARPROC by_ord = GetProcAddress(h, (LPCSTR)43);
    DWORD reserved = 0x77;
    SetLastError(0xdead);
    BOOL r = f(0x1234, &reserved);
    DWORD le = GetLastError();
    printf("IsNTAdmin=%d lasterr=%lx reserved=%lx ord43_same=%d IsUserAnAdmin=%d\n", r, le, reserved, by_ord == (FARPROC)f, IsUserAnAdmin());
    return 0;
}
