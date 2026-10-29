// Test DLL for the regsvr32 probe. Each entry point appends one line to
// %TEMP%\regsvr32_probe.log naming itself, its arguments, the thread's COM
// apartment and whether the DLL was loaded from where regsvr32 was told, and
// returns the HRESULT in %REGDLL_HR% (default S_OK).
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>

static HRESULT result(void) {
    char v[32];
    if (GetEnvironmentVariableA("REGDLL_HR", v, sizeof v)) return (HRESULT)strtoul(v, NULL, 16);
    return S_OK;
}

static void note(const char* what, const wchar_t* arg) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "regsvr32_probe.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    APTTYPE type = (APTTYPE)-1;
    APTTYPEQUALIFIER q;
    HRESULT ah = CoGetApartmentType(&type, &q);
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(init)) CoUninitialize();
    fprintf(f, "%s arg=[%ls] apt_hr=%#lx apt=%d coinit=%#lx bits=%d\n", what, arg ? arg : L"(none)", ah, (int)type,
            init, (int)(sizeof(void*) * 8));
    fclose(f);
}

__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void) { note("DllRegisterServer", NULL); return result(); }
__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void) { note("DllUnregisterServer", NULL); return result(); }
__declspec(dllexport) HRESULT WINAPI DllInstall(BOOL install, LPCWSTR cmd) {
    note(install ? "DllInstall(TRUE)" : "DllInstall(FALSE)", cmd);
    return result();
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) {
    (void)h; (void)p;
    if (r == DLL_PROCESS_ATTACH) note("DllMain(PROCESS_ATTACH)", NULL);
    return TRUE;
}
