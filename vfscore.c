/*
 * vfscore.dll  --  config-driven filesystem path redirector.
 *
 * Loaded as a dependency of a generated proxy DLL (see vfsproxy.py) that a game
 * imports, so vfscore lands in the target process early. It reads vfsproxy.ini
 * from its own folder and hooks the Win32 path-taking file APIs: any path under
 * a configured "real" prefix is rewritten to the matching "virtual" prefix
 * before the real API sees it. Everything else passes through untouched.
 *
 * INI (next to vfscore.dll):
 *   [settings]
 *   create_dirs = 1          ; mkdir each virtual root at startup
 *   log         = 1          ; write vfsproxy.log next to the ini
 *   [paths]
 *   Path1Real    = C:\Users\Me\Documents\Mount and Blade II Bannerlord
 *   Path1Virtual = E:\...\TalesAge\MyDocs
 *   Path2Real    = C:\ProgramData\Mount and Blade II Bannerlord
 *   Path2Virtual = E:\...\TalesAge\MyProgramData
 *
 * Build (mingw-w64 + MinHook):
 *   x86_64-w64-mingw32-gcc -O2 -shared -s -static-libgcc -w -o vfscore.dll \
 *     vfscore.c minhook/src/*.c minhook/src/hde/hde64.c -Iminhook/include \
 *     -Wl,--export-all-symbols   (so the proxy can import the VfsInit marker)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdarg.h>
#include <wctype.h>
#include "MinHook.h"

#define MAXPAIRS 32
#define PLEN     1024

static int   g_n;
static WCHAR g_real[MAXPAIRS][PLEN];
static int   g_reallen[MAXPAIRS];
static WCHAR g_virt[MAXPAIRS][PLEN];
static WCHAR g_ini[MAX_PATH], g_log[MAX_PATH];
static BOOL  g_dolog, g_createdirs;

/* Exported marker so a proxy can static-import vfscore.dll and thereby load it. */
__declspec(dllexport) void VfsInit(void) {}

/* ----------------------------------------------------------------- logging */
static void vlog(const WCHAR *fmt, ...)
{
    if (!g_dolog) return;
    WCHAR w[2048]; char u[6144]; SYSTEMTIME t; GetLocalTime(&t);
    int n = _snwprintf(w, 2048, L"%04u-%02u-%02u %02u:%02u:%02u [%lu] ",
        t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond, GetCurrentProcessId());
    va_list a; va_start(a,fmt); n += _vsnwprintf(w+n, 2040-n, fmt, a); va_end(a);
    if (n < 0 || n > 2040) n = 2040;
    w[n++] = L'\r'; w[n++] = L'\n';
    int len = WideCharToMultiByte(CP_UTF8,0,w,n,u,sizeof u,NULL,NULL);
    HANDLE f = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr; WriteFile(f, u, len, &wr, NULL); CloseHandle(f);
}

/* ----------------------------------------------------------------- config */
static void rstrip(WCHAR *s){ int n=(int)wcslen(s); while(n>0 && (s[n-1]==L' '||s[n-1]==L'\t'||s[n-1]==L'\r'||s[n-1]==L'\n')) s[--n]=0; }
static WCHAR* lstrip(WCHAR *s){ while(*s==L' '||*s==L'\t') s++; return s; }
static void trim_trailing_sep(WCHAR *s){ int n=(int)wcslen(s); while(n>0 && (s[n-1]==L'\\'||s[n-1]==L'/')) s[--n]=0; }

/* pull the trailing integer out of a key like "Path12Real" -> 12, else -1 */
static int key_index(const WCHAR *k){ int i=(int)wcslen(k),e=-1,v=0,m=1,any=0;
    for(i=i-1;i>=0;i--){ if(iswdigit(k[i])){ e=i; } else if(e>=0) break; }
    if(e<0) return -1; for(i=0;k[i];i++){} /* noop */
    /* scan the digit run */
    for(i=0;k[i];i++) if(iswdigit(k[i])){ int s=i; while(iswdigit(k[i])) i++; for(int j=s;j<i;j++){v=v*10+(k[j]-L'0');} any=1; break; }
    (void)m; return any?v:-1;
}
static int has_ci(const WCHAR*h,const WCHAR*n){ /* case-insensitive substring */
    for(;*h;h++){ const WCHAR*a=h,*b=n; while(*b && towupper(*a)==towupper(*b)){a++;b++;} if(!*b) return 1; }
    return 0;
}

static void load_config(void)
{
    WCHAR tmpR[MAXPAIRS][PLEN]; WCHAR tmpV[MAXPAIRS][PLEN];
    for(int i=0;i<MAXPAIRS;i++){ tmpR[i][0]=0; tmpV[i][0]=0; }
    g_dolog=TRUE; g_createdirs=TRUE;           /* defaults */

    HANDLE f=CreateFileW(g_ini,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE) return;
    DWORD sz=GetFileSize(f,NULL); if(sz==INVALID_FILE_SIZE||sz==0){CloseHandle(f);return;}
    char *buf=(char*)HeapAlloc(GetProcessHeap(),0,sz+1); DWORD rd=0;
    ReadFile(f,buf,sz,&rd,NULL); CloseHandle(f); buf[rd]=0;
    /* UTF-8 (or ANSI) -> wide */
    int wn=MultiByteToWideChar(CP_UTF8,0,buf,rd,NULL,0);
    WCHAR *wbuf=(WCHAR*)HeapAlloc(GetProcessHeap(),0,(wn+1)*sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8,0,buf,rd,wbuf,wn); wbuf[wn]=0;
    HeapFree(GetProcessHeap(),0,buf);

    WCHAR *p=wbuf;
    while(*p){
        WCHAR *line=p;
        while(*p && *p!=L'\r' && *p!=L'\n') p++;
        if(*p){ *p=0; p++; while(*p==L'\r'||*p==L'\n') p++; }
        WCHAR *s=lstrip(line);
        if(*s==L';'||*s==L'#'||*s==L'['||*s==0) continue;
        WCHAR *eq=wcschr(s,L'='); if(!eq) continue;
        *eq=0; WCHAR *key=s; WCHAR *val=lstrip(eq+1);
        rstrip(key); rstrip(val);
        if(has_ci(key,L"create_dirs")){ g_createdirs = (val[0]==L'1'||towupper(val[0])==L'T'||towupper(val[0])==L'Y'); continue; }
        if(has_ci(key,L"log")){ g_dolog = (val[0]==L'1'||towupper(val[0])==L'T'||towupper(val[0])==L'Y'); continue; }
        int idx=key_index(key); if(idx<1||idx>=MAXPAIRS) continue;
        if(has_ci(key,L"virtual")){ lstrcpynW(tmpV[idx],val,PLEN); trim_trailing_sep(tmpV[idx]); }
        else if(has_ci(key,L"real")){ lstrcpynW(tmpR[idx],val,PLEN); trim_trailing_sep(tmpR[idx]); }
    }
    HeapFree(GetProcessHeap(),0,wbuf);

    /* compact paired entries */
    for(int i=0;i<MAXPAIRS && g_n<MAXPAIRS;i++){
        if(tmpR[i][0] && tmpV[i][0]){
            lstrcpynW(g_real[g_n],tmpR[i],PLEN);
            lstrcpynW(g_virt[g_n],tmpV[i],PLEN);
            g_reallen[g_n]=(int)wcslen(g_real[g_n]);
            if(g_createdirs) SHCreateDirectoryExW(NULL,g_virt[g_n],NULL);
            g_n++;
        }
    }
}

/* ----------------------------------------------------------------- remap */
static const WCHAR* strip_nt(const WCHAR *p){
    if(p && p[0]==L'\\'&&p[1]==L'\\'&&(p[2]==L'?'||p[2]==L'.')&&p[3]==L'\\') return p+4;
    return p;
}
static int ieq_prefix(const WCHAR *s,const WCHAR *pfx,int pl){
    for(int i=0;i<pl;i++){ WCHAR a=s[i],b=pfx[i];
        if(a==L'/')a=L'\\'; if(b==L'/')b=L'\\';
        if(towupper(a)!=towupper(b)) return 0; }
    return 1;
}
/* returns TRUE and fills out[] if 'in' is under a real prefix */
static BOOL remapW(const WCHAR *in, WCHAR *out, size_t outcch, int *which)
{
    if(!in || !*in) return FALSE;
    const WCHAR *s=strip_nt(in);
    for(int i=0;i<g_n;i++){
        int rl=g_reallen[i];
        if(ieq_prefix(s,g_real[i],rl)){
            WCHAR nx=s[rl];
            if(nx==0 || nx==L'\\' || nx==L'/'){
                _snwprintf(out,outcch,L"%s%s",g_virt[i],s+rl);
                out[outcch-1]=0;
                if(which)*which=i;
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* ----------------------------------------------------------------- hooks */
/* Each wrapper: remap first (and where relevant second) path arg, else pass through. */

#define REMAP1(pin) WCHAR _r1[PLEN*2]; const WCHAR* a1=(pin); int _w1=-1; \
    if(remapW(pin,_r1,PLEN*2,&_w1)){ a1=_r1; vlog(L"%S -> %s", __func__, _r1); }

typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *CreateFile2_t)(LPCWSTR,DWORD,DWORD,DWORD,LPVOID);
typedef DWORD  (WINAPI *GetFileAttributesW_t)(LPCWSTR);
typedef BOOL   (WINAPI *GetFileAttributesExW_t)(LPCWSTR,GET_FILEEX_INFO_LEVELS,LPVOID);
typedef BOOL   (WINAPI *SetFileAttributesW_t)(LPCWSTR,DWORD);
typedef BOOL   (WINAPI *CreateDirectoryW_t)(LPCWSTR,LPSECURITY_ATTRIBUTES);
typedef BOOL   (WINAPI *CreateDirectoryExW_t)(LPCWSTR,LPCWSTR,LPSECURITY_ATTRIBUTES);
typedef BOOL   (WINAPI *RemoveDirectoryW_t)(LPCWSTR);
typedef BOOL   (WINAPI *DeleteFileW_t)(LPCWSTR);
typedef HANDLE (WINAPI *FindFirstFileW_t)(LPCWSTR,LPWIN32_FIND_DATAW);
typedef HANDLE (WINAPI *FindFirstFileExW_t)(LPCWSTR,FINDEX_INFO_LEVELS,LPVOID,FINDEX_SEARCH_OPS,LPVOID,DWORD);
typedef BOOL   (WINAPI *MoveFileExW_t)(LPCWSTR,LPCWSTR,DWORD);
typedef BOOL   (WINAPI *MoveFileW_t)(LPCWSTR,LPCWSTR);
typedef BOOL   (WINAPI *CopyFileW_t)(LPCWSTR,LPCWSTR,BOOL);
typedef BOOL   (WINAPI *CopyFileExW_t)(LPCWSTR,LPCWSTR,LPPROGRESS_ROUTINE,LPVOID,LPBOOL,DWORD);

static CreateFileW_t          o_CreateFileW;
static CreateFile2_t          o_CreateFile2;
static GetFileAttributesW_t   o_GetFileAttributesW;
static GetFileAttributesExW_t o_GetFileAttributesExW;
static SetFileAttributesW_t   o_SetFileAttributesW;
static CreateDirectoryW_t     o_CreateDirectoryW;
static CreateDirectoryExW_t   o_CreateDirectoryExW;
static RemoveDirectoryW_t     o_RemoveDirectoryW;
static DeleteFileW_t          o_DeleteFileW;
static FindFirstFileW_t       o_FindFirstFileW;
static FindFirstFileExW_t     o_FindFirstFileExW;
static MoveFileExW_t          o_MoveFileExW;
static MoveFileW_t            o_MoveFileW;
static CopyFileW_t            o_CopyFileW;
static CopyFileExW_t          o_CopyFileExW;

static HANDLE WINAPI h_CreateFileW(LPCWSTR fn,DWORD a,DWORD sh,LPSECURITY_ATTRIBUTES sa,DWORD cd,DWORD fl,HANDLE t){
    REMAP1(fn); return o_CreateFileW(a1,a,sh,sa,cd,fl,t); }
static HANDLE WINAPI h_CreateFile2(LPCWSTR fn,DWORD a,DWORD sh,DWORD cd,LPVOID p){
    REMAP1(fn); return o_CreateFile2(a1,a,sh,cd,p); }
static DWORD WINAPI h_GetFileAttributesW(LPCWSTR fn){ REMAP1(fn); return o_GetFileAttributesW(a1); }
static BOOL WINAPI h_GetFileAttributesExW(LPCWSTR fn,GET_FILEEX_INFO_LEVELS lv,LPVOID p){ REMAP1(fn); return o_GetFileAttributesExW(a1,lv,p); }
static BOOL WINAPI h_SetFileAttributesW(LPCWSTR fn,DWORD at){ REMAP1(fn); return o_SetFileAttributesW(a1,at); }
static BOOL WINAPI h_CreateDirectoryW(LPCWSTR fn,LPSECURITY_ATTRIBUTES sa){ REMAP1(fn); return o_CreateDirectoryW(a1,sa); }
static BOOL WINAPI h_CreateDirectoryExW(LPCWSTR tmpl,LPCWSTR fn,LPSECURITY_ATTRIBUTES sa){ REMAP1(fn); return o_CreateDirectoryExW(tmpl,a1,sa); }
static BOOL WINAPI h_RemoveDirectoryW(LPCWSTR fn){ REMAP1(fn); return o_RemoveDirectoryW(a1); }
static BOOL WINAPI h_DeleteFileW(LPCWSTR fn){ REMAP1(fn); return o_DeleteFileW(a1); }
static HANDLE WINAPI h_FindFirstFileW(LPCWSTR fn,LPWIN32_FIND_DATAW d){ REMAP1(fn); return o_FindFirstFileW(a1,d); }
static HANDLE WINAPI h_FindFirstFileExW(LPCWSTR fn,FINDEX_INFO_LEVELS il,LPVOID fd,FINDEX_SEARCH_OPS op,LPVOID sf,DWORD fl){
    REMAP1(fn); return o_FindFirstFileExW(a1,il,fd,op,sf,fl); }
static BOOL WINAPI h_MoveFileExW(LPCWSTR a,LPCWSTR b,DWORD fl){
    WCHAR ra[PLEN*2],rb[PLEN*2]; const WCHAR *na=a,*nb=b; int w;
    if(remapW(a,ra,PLEN*2,&w)){na=ra; vlog(L"MoveFileExW src -> %s",ra);}
    if(remapW(b,rb,PLEN*2,&w)){nb=rb; vlog(L"MoveFileExW dst -> %s",rb);}
    return o_MoveFileExW(na,nb,fl); }
static BOOL WINAPI h_MoveFileW(LPCWSTR a,LPCWSTR b){
    WCHAR ra[PLEN*2],rb[PLEN*2]; const WCHAR *na=a,*nb=b; int w;
    if(remapW(a,ra,PLEN*2,&w))na=ra; if(remapW(b,rb,PLEN*2,&w))nb=rb;
    return o_MoveFileW(na,nb); }
static BOOL WINAPI h_CopyFileW(LPCWSTR a,LPCWSTR b,BOOL ff){
    WCHAR ra[PLEN*2],rb[PLEN*2]; const WCHAR *na=a,*nb=b; int w;
    if(remapW(a,ra,PLEN*2,&w))na=ra; if(remapW(b,rb,PLEN*2,&w))nb=rb;
    return o_CopyFileW(na,nb,ff); }
static BOOL WINAPI h_CopyFileExW(LPCWSTR a,LPCWSTR b,LPPROGRESS_ROUTINE pr,LPVOID pd,LPBOOL c,DWORD fl){
    WCHAR ra[PLEN*2],rb[PLEN*2]; const WCHAR *na=a,*nb=b; int w;
    if(remapW(a,ra,PLEN*2,&w))na=ra; if(remapW(b,rb,PLEN*2,&w))nb=rb;
    return o_CopyFileExW(na,nb,pr,pd,c,fl); }

/* ---- a few ANSI variants (widen, remap, narrow, call orig A) ---- */
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef DWORD  (WINAPI *GetFileAttributesA_t)(LPCSTR);
typedef BOOL   (WINAPI *CreateDirectoryA_t)(LPCSTR,LPSECURITY_ATTRIBUTES);
typedef BOOL   (WINAPI *DeleteFileA_t)(LPCSTR);
static CreateFileA_t        o_CreateFileA;
static GetFileAttributesA_t o_GetFileAttributesA;
static CreateDirectoryA_t   o_CreateDirectoryA;
static DeleteFileA_t        o_DeleteFileA;

static int remapA(LPCSTR in, char *out, int outsz){
    if(!in) return 0;
    WCHAR w[PLEN]; MultiByteToWideChar(CP_ACP,0,in,-1,w,PLEN);
    WCHAR r[PLEN*2]; int which;
    if(!remapW(w,r,PLEN*2,&which)) return 0;
    WideCharToMultiByte(CP_ACP,0,r,-1,out,outsz,NULL,NULL); return 1;
}
static HANDLE WINAPI h_CreateFileA(LPCSTR fn,DWORD a,DWORD sh,LPSECURITY_ATTRIBUTES sa,DWORD cd,DWORD fl,HANDLE t){
    char r[PLEN*2]; if(remapA(fn,r,sizeof r)){ vlog(L"CreateFileA (remapped)"); return o_CreateFileA(r,a,sh,sa,cd,fl,t);} return o_CreateFileA(fn,a,sh,sa,cd,fl,t); }
static DWORD WINAPI h_GetFileAttributesA(LPCSTR fn){ char r[PLEN*2]; if(remapA(fn,r,sizeof r)) return o_GetFileAttributesA(r); return o_GetFileAttributesA(fn); }
static BOOL WINAPI h_CreateDirectoryA(LPCSTR fn,LPSECURITY_ATTRIBUTES sa){ char r[PLEN*2]; if(remapA(fn,r,sizeof r)) return o_CreateDirectoryA(r,sa); return o_CreateDirectoryA(fn,sa); }
static BOOL WINAPI h_DeleteFileA(LPCSTR fn){ char r[PLEN*2]; if(remapA(fn,r,sizeof r)) return o_DeleteFileA(r); return o_DeleteFileA(fn); }

/* ----------------------------------------------------------------- install */
static HMODULE k32, kbase;
static void* resolve(const char *name){
    void *p = kbase ? (void*)GetProcAddress(kbase,name) : NULL;
    if(!p && k32) p = (void*)GetProcAddress(k32,name);
    return p;
}
#define HOOK(name) do{ void*t=resolve(#name); if(t && MH_CreateHook(t,(void*)h_##name,(void**)&o_##name)==MH_OK) nok++; else nfail++; }while(0)

static DWORD WINAPI install(LPVOID u)
{
    (void)u;
    load_config();
    if(g_n==0){ vlog(L"no path pairs configured in %s - nothing to do", g_ini); return 0; }

    k32   = GetModuleHandleW(L"kernel32.dll");
    kbase = GetModuleHandleW(L"kernelbase.dll");
    if(!kbase) kbase = LoadLibraryW(L"kernelbase.dll");
    if(MH_Initialize()!=MH_OK){ vlog(L"MH_Initialize failed"); return 0; }

    int nok=0,nfail=0;
    HOOK(CreateFileW); HOOK(CreateFile2); HOOK(GetFileAttributesW); HOOK(GetFileAttributesExW);
    HOOK(SetFileAttributesW); HOOK(CreateDirectoryW); HOOK(CreateDirectoryExW); HOOK(RemoveDirectoryW);
    HOOK(DeleteFileW); HOOK(FindFirstFileW); HOOK(FindFirstFileExW); HOOK(MoveFileExW); HOOK(MoveFileW);
    HOOK(CopyFileW); HOOK(CopyFileExW);
    HOOK(CreateFileA); HOOK(GetFileAttributesA); HOOK(CreateDirectoryA); HOOK(DeleteFileA);

    MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
    WCHAR exe[MAX_PATH]; GetModuleFileNameW(NULL,exe,MAX_PATH);
    const WCHAR *en=wcsrchr(exe,L'\\')?wcsrchr(exe,L'\\')+1:exe;
    vlog(L"attach %s: %d hooks (%d failed) %s, %d path pair(s)", en, nok, nfail,
         st==MH_OK?L"enabled":L"ENABLE FAILED", g_n);
    for(int i=0;i<g_n;i++) vlog(L"  [%d] %s  =>  %s", i+1, g_real[i], g_virt[i]);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if(reason==DLL_PROCESS_ATTACH){
        DisableThreadLibraryCalls(h);
        /* ini + log live next to this DLL */
        WCHAR p[MAX_PATH]; GetModuleFileNameW(h,p,MAX_PATH);
        WCHAR *s=wcsrchr(p,L'\\'); if(s) *(s+1)=0;
        _snwprintf(g_ini,MAX_PATH,L"%svfsproxy.ini",p);
        _snwprintf(g_log,MAX_PATH,L"%svfsproxy.log",p);
        /* Install on a worker thread, NOT here: MH_EnableHook suspends every
           other thread to patch, which under the loader lock faults on real
           Windows (a dependent LoadLibrary of this DLL then fails). The thread
           only begins once the loader lock is released. */
        HANDLE th = CreateThread(NULL, 0, install, NULL, 0, NULL);
        if(th) CloseHandle(th);
    }
    return TRUE;
}
