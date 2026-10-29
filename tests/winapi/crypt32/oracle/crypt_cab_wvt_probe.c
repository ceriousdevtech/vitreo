// Cabinet WinVerifyTrust oracle probe: prints the HRESULT of
// WINTRUST_ACTION_GENERIC_VERIFY_V2 (no UI, no revocation) for each file.
//   x86_64-w64-mingw32-gcc -O1 -municode -o wvt64.exe crypt_cab_wvt_probe.c -lwintrust
//   i686-w64-mingw32-gcc   -O1 -municode -o wvt32.exe crypt_cab_wvt_probe.c -lwintrust
// Run on winoracle in a folder holding ../fixtures/cab/*.cab; both bitnesses
// printed crypt_cab_wvt.expected exactly.
// WinVerifyTrust(WINTRUST_ACTION_GENERIC_VERIFY_V2) on each argv file; prints HRESULT.
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <stdio.h>
int wmain(int argc, wchar_t** argv) {
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    for (int i = 1; i < argc; ++i) {
        WINTRUST_FILE_INFO fi = {sizeof(fi), argv[i], NULL, NULL};
        WINTRUST_DATA wd = {0};
        wd.cbStruct = sizeof(wd); wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE;
        wd.dwUnionChoice = WTD_CHOICE_FILE; wd.pFile = &fi; wd.dwStateAction = WTD_STATEACTION_VERIFY;
        LONG r = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
        wd.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
        wprintf(L"%ls\t%08lx\n", argv[i], (unsigned long)r);
    }
    return 0;
}
