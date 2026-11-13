#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>

// Options lues dans bhd_mod.ini (à côté de bhd.exe)
struct Config {
    bool hideCursor          = true; // [Cursor]    HideCursor
    bool fullscreenCutscenes = true; // [Cutscenes] FullscreenCutscenes
    bool keepSubtitles       = true; // [Cutscenes] KeepSubtitles
    bool skipSaveNotice      = true; // [Startup]   SkipSaveNotice
    bool log                 = false; // [Debug]    Log
    bool dumpCode            = false; // [Debug]    DumpCode (non documenté : copie le code déchiffré de bhd.exe)
};
extern Config g_cfg;

void Log(const char* fmt, ...);

struct HookDef { const char* name; void* hook; void** orig; };
void  PatchIAT(HMODULE mod, const char* dll, HookDef* hooks, size_t count);
void* PatchVTable(void* object, int index, void* hook); // renvoie l'ancienne entrée

void DumpCode();
void Patches_Apply(); // à appeler une fois le code du jeu déchiffré

void Cursor_Install();
void D3D_Install();
