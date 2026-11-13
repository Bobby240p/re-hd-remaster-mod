#pragma once
#define BHD_MOD_VERSION "1.1.0"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>

// Options lues dans bhd_mod.ini (à côté de bhd.exe)
struct Config {
    bool hideCursor          = true; // [Cursor]    HideCursor
    bool fullscreenCutscenes = true; // [Cutscenes] FullscreenCutscenes
    bool keepSubtitles       = true; // [Cutscenes] KeepSubtitles
    bool wideDoors           = true; // [Cutscenes] WideDoors
    char resolutions[256]    = {};   // [Display]   Resolutions ("auto" = résolution native, ou "2560x1440,...")
    int  sceneScale          = -1;   // [Display]   SceneScale : -1 = auto, 0 = désactivé (rendu d'origine),
                                     //             1 = taille d'origine + réduction de qualité, 2..4 = facteur fixe
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

bool SceneQualityEnabled();      // super-sampling / réduction de qualité actifs (SceneScale != 0)
UINT SceneScaleFor(UINT screenH); // facteur de la scène pour une hauteur d'écran donnée

void Cursor_Install();
void D3D_Install();
