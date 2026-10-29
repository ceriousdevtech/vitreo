// Minimal DLL whose only purpose is a VS_VERSIONINFO (versioned_dll.rc) for
// the SetupAPI SP_COPY_NEWER probe cases.
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { (void)h; (void)r; (void)p; return TRUE; }
