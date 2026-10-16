#include <windows.h>
__declspec(dllimport) void VfsInit(void);  /* nop in vfscore; keeps the import */
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x){
    (void)h;(void)x;
    if(r==DLL_PROCESS_ATTACH) VfsInit();     /* trivial; vfscore already loaded as dependency */
    return TRUE;
}
