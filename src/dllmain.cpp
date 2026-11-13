// bhd_mod - proxy dinput8.dll pour Resident Evil HD Remaster (bhd.exe)
//  - cache le curseur de la souris
//  - en "rendu original" (4/3), agrandit les cinématiques letterbox pour remplir l'écran
// Options : bhd_mod.ini à côté de bhd.exe.

#include "common.h"
#include <unknwn.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

Config g_cfg;

// ---------------------------------------------------------------------------
// Proxy vers la vraie dinput8.dll du système
// ---------------------------------------------------------------------------
static HMODULE g_realDInput = nullptr;

static FARPROC GetReal(const char* name)
{
    if (!g_realDInput) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH); // SysWOW64 pour un processus 32 bits (redirection WOW64)
        if (n == 0 || n >= MAX_PATH - 16) return nullptr;
        lstrcatA(path, "\\dinput8.dll");
        g_realDInput = LoadLibraryA(path);
        if (!g_realDInput) return nullptr;
    }
    return GetProcAddress(g_realDInput, name);
}

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    typedef HRESULT(WINAPI* Fn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    Fn fn = (Fn)GetReal("DirectInput8Create");
    if (!fn) return E_FAIL;
    return fn(hinst, ver, riid, out, outer);
}

extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    typedef HRESULT(WINAPI* Fn)();
    Fn fn = (Fn)GetReal("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}

extern "C" HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
{
    typedef HRESULT(WINAPI* Fn)(REFCLSID, REFIID, LPVOID*);
    Fn fn = (Fn)GetReal("DllGetClassObject");
    return fn ? fn(clsid, riid, out) : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DllRegisterServer()
{
    typedef HRESULT(WINAPI* Fn)();
    Fn fn = (Fn)GetReal("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DllUnregisterServer()
{
    typedef HRESULT(WINAPI* Fn)();
    Fn fn = (Fn)GetReal("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

extern "C" LPCVOID WINAPI Proxy_GetdfDIJoystick()
{
    typedef LPCVOID(WINAPI* Fn)();
    Fn fn = (Fn)GetReal("GetdfDIJoystick");
    return fn ? fn() : nullptr;
}

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------
static char g_logPath[MAX_PATH];

void Log(const char* fmt, ...)
{
    if (!g_cfg.log || !g_logPath[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") != 0 || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Utilitaires de patch
// ---------------------------------------------------------------------------
void PatchIAT(HMODULE mod, const char* dll, HookDef* hooks, size_t count)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;

    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        if (!imp->OriginalFirstThunk) continue;

        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* iat   = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char* fn = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            for (size_t i = 0; i < count; ++i) {
                if (strcmp(fn, hooks[i].name) != 0) continue;
                DWORD old;
                if (VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &old)) {
                    *hooks[i].orig = (void*)iat->u1.Function;
                    iat->u1.Function = (ULONG_PTR)hooks[i].hook;
                    VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), old, &old);
                    Log("IAT %s!%s intercepté", dll, fn);
                }
            }
        }
    }
}

void* PatchVTable(void* object, int index, void* hook)
{
    void** slot = &(*(void***)object)[index];
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* orig = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return orig;
}

// ---------------------------------------------------------------------------
// Configuration (.ini)
// ---------------------------------------------------------------------------
static const char kDefaultIni[] =
    u8"; Options du mod bhd_mod (dinput8.dll) - Resident Evil HD Remaster\r\n"
    u8"; Les changements sont pris en compte au prochain lancement du jeu.\r\n"
    u8"\r\n"
    u8"[Cursor]\r\n"
    u8"; 1 = cacher le curseur de la souris dans le jeu, 0 = laisser le curseur visible\r\n"
    u8"HideCursor=1\r\n"
    u8"\r\n"
    u8"[Cutscenes]\r\n"
    u8"; 1 = en \"rendu original\" (4/3), les cinématiques avec bandes noires sont agrandies\r\n"
    u8";     pour remplir l'écran en conservant leur ratio d'image. 0 = comportement d'origine\r\n"
    u8"FullscreenCutscenes=1\r\n"
    u8"; 1 = le contenu de la bande noire du bas (sous-titres) est conservé en bas de l'image\r\n"
    u8"KeepSubtitles=1\r\n"
    u8"\r\n"
    u8"[Startup]\r\n"
    u8"; 1 = supprime l'écran d'avertissement de sauvegarde automatique au lancement\r\n"
    u8";     (on arrive directement au menu principal)\r\n"
    u8"SkipSaveNotice=1\r\n"
    u8"\r\n"
    u8"[Debug]\r\n"
    u8"; 1 = écrit un journal bhd_mod.log à côté de bhd.exe\r\n"
    u8"Log=0\r\n";

static char g_gameDir[MAX_PATH];

// Écrit la section .text de bhd.exe telle qu'elle est en mémoire (déchiffrée par la protection Steam)
// dans bhd_text.bin, pour analyse hors jeu. Appelé une fois, depuis du code du jeu (donc après déchiffrement).
void DumpCode()
{
    static bool done = false;
    if (done || !g_cfg.dumpCode || !g_gameDir[0]) return;
    done = true;

    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 6) != 0) continue;
        char path[MAX_PATH];
        lstrcpyA(path, g_gameDir);
        lstrcatA(path, "bhd_text.bin");
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(h, base + sec->VirtualAddress, sec->Misc.VirtualSize, &written, nullptr);
        CloseHandle(h);
        Log("DumpCode : base=%p .text VA=0x%08lX taille=0x%lX écrit=0x%lX",
            base, sec->VirtualAddress, sec->Misc.VirtualSize, written);
        return;
    }
}

static void LoadConfig()
{
    char dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    char* slash = strrchr(dir, '\\');
    if (!slash || (size_t)(slash - dir) + 16 >= MAX_PATH) return;
    slash[1] = 0;

    char ini[MAX_PATH];
    lstrcpyA(ini, dir);
    lstrcatA(ini, "bhd_mod.ini");

    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileA(ini, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(h, kDefaultIni, sizeof(kDefaultIni) - 1, &written, nullptr);
            CloseHandle(h);
        }
    }

    g_cfg.hideCursor          = GetPrivateProfileIntA("Cursor", "HideCursor", 1, ini) != 0;
    g_cfg.fullscreenCutscenes = GetPrivateProfileIntA("Cutscenes", "FullscreenCutscenes", 1, ini) != 0;
    g_cfg.keepSubtitles       = GetPrivateProfileIntA("Cutscenes", "KeepSubtitles", 1, ini) != 0;
    g_cfg.skipSaveNotice      = GetPrivateProfileIntA("Startup", "SkipSaveNotice", 1, ini) != 0;
    g_cfg.log                 = GetPrivateProfileIntA("Debug", "Log", 0, ini) != 0;
    g_cfg.dumpCode            = GetPrivateProfileIntA("Debug", "DumpCode", 0, ini) != 0;
    lstrcpyA(g_gameDir, dir);

    if (g_cfg.log) {
        lstrcpyA(g_logPath, dir);
        lstrcatA(g_logPath, "bhd_mod.log");
        DeleteFileA(g_logPath);
        Log("bhd_mod chargé - HideCursor=%d FullscreenCutscenes=%d KeepSubtitles=%d SkipSaveNotice=%d",
            g_cfg.hideCursor, g_cfg.fullscreenCutscenes, g_cfg.keepSubtitles, g_cfg.skipSaveNotice);
    }
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        LoadConfig();
        if (g_cfg.hideCursor)
            Cursor_Install();
        D3D_Install(); // sert aussi de point d'entrée pour les patchs de code (voir Hook_Direct3DCreate9)
    }
    return TRUE;
}
