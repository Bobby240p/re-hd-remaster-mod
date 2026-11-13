// Masquage du curseur Windows au-dessus de la fenêtre du jeu (hooks USER32 dans l'IAT de bhd.exe).

#include "common.h"

typedef int(WINAPI* ShowCursor_t)(BOOL);
typedef HCURSOR(WINAPI* SetCursor_t)(HCURSOR);
typedef HCURSOR(WINAPI* LoadCursorA_t)(HINSTANCE, LPCSTR);
typedef ATOM(WINAPI* RegisterClassExA_t)(const WNDCLASSEXA*);
typedef ATOM(WINAPI* RegisterClassExW_t)(const WNDCLASSEXW*);
typedef HWND(WINAPI* CreateWindowExA_t)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef HWND(WINAPI* CreateWindowExW_t)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);

static ShowCursor_t       oShowCursor       = nullptr;
static SetCursor_t        oSetCursor        = nullptr;
static LoadCursorA_t      oLoadCursorA      = nullptr;
static RegisterClassExA_t oRegisterClassExA = nullptr;
static RegisterClassExW_t oRegisterClassExW = nullptr;
static CreateWindowExA_t  oCreateWindowExA  = nullptr;
static CreateWindowExW_t  oCreateWindowExW  = nullptr;

// Le compteur d'affichage du curseur est propre à chaque thread : on le force à < 0.
static int ForceHidden()
{
    int c = oShowCursor(FALSE);
    while (c >= 0) c = oShowCursor(FALSE);
    return c;
}

static int WINAPI Hook_ShowCursor(BOOL)
{
    return ForceHidden(); // toute demande laisse le curseur caché
}

static HCURSOR WINAPI Hook_SetCursor(HCURSOR)
{
    return oSetCursor(nullptr);
}

static HCURSOR WINAPI Hook_LoadCursorA(HINSTANCE, LPCSTR)
{
    return nullptr;
}

static ATOM WINAPI Hook_RegisterClassExA(const WNDCLASSEXA* wc)
{
    if (!wc) return oRegisterClassExA(wc);
    WNDCLASSEXA copy = *wc;
    copy.hCursor = nullptr; // pas de curseur de classe => DefWindowProc ne remet pas la flèche
    return oRegisterClassExA(&copy);
}

static ATOM WINAPI Hook_RegisterClassExW(const WNDCLASSEXW* wc)
{
    if (!wc) return oRegisterClassExW(wc);
    WNDCLASSEXW copy = *wc;
    copy.hCursor = nullptr;
    return oRegisterClassExW(&copy);
}

static HWND WINAPI Hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD st, int x, int y, int w, int h,
                                        HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND hwnd = oCreateWindowExA(ex, cls, name, st, x, y, w, h, parent, menu, inst, param);
    if (hwnd) { ForceHidden(); oSetCursor(nullptr); }
    return hwnd;
}

static HWND WINAPI Hook_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD st, int x, int y, int w, int h,
                                        HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND hwnd = oCreateWindowExW(ex, cls, name, st, x, y, w, h, parent, menu, inst, param);
    if (hwnd) { ForceHidden(); oSetCursor(nullptr); }
    return hwnd;
}

void Cursor_Install()
{
    HookDef hooks[] = {
        { "ShowCursor",       (void*)Hook_ShowCursor,       (void**)&oShowCursor },
        { "SetCursor",        (void*)Hook_SetCursor,        (void**)&oSetCursor },
        { "LoadCursorA",      (void*)Hook_LoadCursorA,      (void**)&oLoadCursorA },
        { "RegisterClassExA", (void*)Hook_RegisterClassExA, (void**)&oRegisterClassExA },
        { "RegisterClassExW", (void*)Hook_RegisterClassExW, (void**)&oRegisterClassExW },
        { "CreateWindowExA",  (void*)Hook_CreateWindowExA,  (void**)&oCreateWindowExA },
        { "CreateWindowExW",  (void*)Hook_CreateWindowExW,  (void**)&oCreateWindowExW },
    };

    // Valeurs de repli si une fonction n'est pas trouvée dans l'IAT.
    HMODULE user32 = GetModuleHandleA("user32.dll");
    oShowCursor = (ShowCursor_t)GetProcAddress(user32, "ShowCursor");
    oSetCursor  = (SetCursor_t)GetProcAddress(user32, "SetCursor");

    PatchIAT(GetModuleHandleA(nullptr), "USER32.dll", hooks, sizeof(hooks) / sizeof(hooks[0]));
}
