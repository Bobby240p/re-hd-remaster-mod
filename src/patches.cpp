// Patchs du code de bhd.exe, appliqués en mémoire une fois le code déchiffré par la protection Steam,
// et lecture de l'état du jeu. Chaque patch vérifie les octets d'origine : si le jeu a été mis à jour,
// il n'est pas appliqué et la fonction correspondante est désactivée.

#include "common.h"
#include <string.h>
#include <stdlib.h>

namespace {

const DWORD kImageBase = 0x400000; // adresses ci-dessous = adresses de bhd.exe (base 0x400000)

BYTE* Base() { return (BYTE*)GetModuleHandleA(nullptr); }

// L'adresse est-elle dans le code exécutable de bhd.exe ?
bool InCode(DWORD va, size_t len)
{
    BYTE* base = Base();
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    DWORD rva = va - kImageBase;
    if (rva + len > nt->OptionalHeader.SizeOfImage) return false;
    MEMORY_BASIC_INFORMATION mbi;
    BYTE* addr = base + rva;
    return VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
           (BYTE*)mbi.BaseAddress + mbi.RegionSize >= addr + len;
}

bool Matches(const char* name, DWORD va, const BYTE* bytes, size_t len)
{
    if (!InCode(va, len) || memcmp(Base() + (va - kImageBase), bytes, len) != 0) {
        Log("'%s' désactivé : code inattendu à 0x%08lX (version du jeu différente ?)", name, va);
        return false;
    }
    return true;
}

bool Write(const char* name, DWORD va, const BYTE* bytes, size_t len)
{
    BYTE* addr = Base() + (va - kImageBase);
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) {
        Log("'%s' désactivé : VirtualProtect a échoué", name);
        return false;
    }
    memcpy(addr, bytes, len);
    VirtualProtect(addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    Log("Patch '%s' appliqué", name);
    return true;
}

// --- Écran d'avertissement de sauvegarde automatique au lancement -------------------------------
// Procédure 3 de l'écran titre (fonction 0x467C00, sous-état dans [esi+0D8h]) :
//   0 : prépare uBhdGUI_NoticeMessage   1 : l'ouvre   2 : attend "Suivant"
//   3-4 : fermeture                    0x64 : passe à la procédure 0xC (menu principal)
// On remplace le cas 0 (0x467C32) par : sous-état = 0x64 -> menu principal, l'avertissement n'est jamais affiché.
const DWORD kNoticeVA = 0x467C32;
const BYTE kNoticeOrig[] = {
    0x8B, 0x46, 0x5C,                               // mov  eax,[esi+5Ch]
    0x85, 0xC0,                                     // test eax,eax
    0x74, 0x11,                                     // je   +11h
    0x81, 0x48, 0x0C, 0x00, 0x04, 0x00, 0x00,       // or   dword ptr [eax+0Ch],400h
    0x8B, 0x46, 0x5C,                               // mov  eax,[esi+5Ch]
    0x81, 0x48, 0x0C, 0x00, 0x08, 0x00, 0x00,       // or   dword ptr [eax+0Ch],800h
    0xFF, 0x86, 0xD8, 0x00, 0x00, 0x00,             // inc  dword ptr [esi+0D8h]
    0x5F, 0x5E, 0xC3,                               // pop edi / pop esi / ret
};
const BYTE kNoticeNew[] = {
    0xC7, 0x86, 0xD8, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, // mov dword ptr [esi+0D8h],64h
    0x5F, 0x5E, 0xC3,                                           // pop edi / pop esi / ret
};

// --- Bandes noires des cinématiques (rendu original) ---------------------------------------------
// Fonction 0x537330 (dessin des bandes) :
//   drapeau cinématique = bit 17 de [[0CF2F28h]+2Ch] ; à chaque changement, fondu de 15 images ;
//   bandes dessinées sauf si mDisplayMode ([[0CF2F28h]+511Ch]) == 1 (16/9) ;
//   haut : y = -2 .. hauteur-2, bas : y = 720-hauteur .. 722 (repère 1280x720), hauteur = 3Ah (58).
const DWORD kGamePtrVA = 0xCF2F28;
const DWORD kLetterboxReadVA = 0x53733C;
const BYTE kLetterboxRead[] = {
    0xA1, 0x28, 0x2F, 0xCF, 0x00,                   // mov    eax,[0CF2F28h]
    0xF3, 0x0F, 0x10, 0x0D, 0xD4, 0x14, 0xC3, 0x00, // movss  xmm1,[0C314D4h]
    0x8B, 0x58, 0x2C,                               // mov    ebx,[eax+2Ch]
    0xC1, 0xEB, 0x11,                               // shr    ebx,11h
    0x80, 0xE3, 0x01,                               // and    bl,1
};
const DWORD kLetterboxModeVA = 0x537417;
const BYTE kLetterboxMode[] = {
    0xA1, 0x28, 0x2F, 0xCF, 0x00,                   // mov eax,[0CF2F28h]
    0x83, 0xB8, 0x1C, 0x51, 0x00, 0x00, 0x01,       // cmp dword ptr [eax+511Ch],1
};
const DWORD kLetterboxHeightVA = 0x537035;
const BYTE kLetterboxHeight[] = {
    0xC7, 0x86, 0xAC, 0x60, 0x00, 0x00, 0x3A, 0x00, 0x00, 0x00, // mov dword ptr [esi+60ACh],3Ah
};
// Durée du fondu des bandes : 15.0 -> 0.0 (les bandes apparaissent/disparaissent instantanément,
// en même temps que le recadrage : aucune transition visible).
const DWORD kLetterboxFadeVA = 0x53735C;
const BYTE kLetterboxFadeOrig[] = { 0xC7, 0x87, 0xA0, 0x60, 0x00, 0x00, 0x00, 0x00, 0x70, 0x41 };
const BYTE kLetterboxFadeNew[]  = { 0xC7, 0x87, 0xA0, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

bool g_letterboxOk = false;

// --- Résolutions supplémentaires dans le menu des options -----------------------------------------
// Le menu propose une liste fixe de 12 résolutions (1024x768 ... 2560x1600) :
//   libellés  : table (pointeur, index) en 0C2EE78h, chaînes de 12 octets ;
//   index -> taille : fonction 0x528CF0, "mov dword ptr [esp+4],L" / "mov dword ptr [esp+8],H" par cas ;
//   taille -> index : fonction 0x5283A0, "cmp ecx,L" / "cmp eax,H" en chaîne.
// On remplace les entrées dont la largeur est unique dans la liste (remplacement sans ambiguïté).
struct ResSlot {
    int   index;
    DWORD w, h;          // valeurs d'origine
    DWORD applyW, applyH; // "mov dword ptr [esp+4/8],imm32" (imm à +4)
    DWORD findW, findH;   // "cmp ecx,imm32" (imm à +2) / "cmp eax,imm32" (imm à +1)
};
// Ordre de remplacement : les plus grandes résolutions vont en bas de la liste.
const ResSlot kResSlots[] = {
    { 11, 2560, 1600, 0x528E64, 0x528E6C, 0x5284AC, 0x5284B4 },
    {  8, 1680, 1050, 0x528E2E, 0x528E36, 0x528472, 0x52847A },
    {  5, 1360,  768, 0x528DF8, 0x528E00, 0x528438, 0x528440 },
    {  1, 1152,  864, 0x528DC9, 0x528DD1, 0x5283E4, 0x5283EC },
    {  0, 1024,  768, 0x528DB4, 0x528DBC, 0x5283D1, 0x5283D9 },
};
// Texte affiché dans le menu : message du jeu dont le numéro est lu en 0C370E0h + 12*index,
// via "call 004763C0" (récupération d'un message, __thiscall) en 0x527AB2.
// On redirige cet appel pour renvoyer nos libellés pour les entrées remplacées.
const DWORD kResMsgTableVA   = 0xC370E0;
const DWORD kResMsgCallVA    = 0x527AB2;
const DWORD kGetMessageVA    = 0x4763C0;
const DWORD kResLabelTableVA = 0xC2EE78; // 12 x (const char* libellé, DWORD index)
const DWORD kResLabelSize    = 12;
// Tailles des 12 entrées d'origine (pour refuser les doublons).
const DWORD kResOrig[12][2] = {
    {1024,768},{1152,864},{1280,720},{1280,960},{1280,1024},{1360,768},
    {1600,900},{1600,1200},{1680,1050},{1920,1080},{1920,1200},{2560,1600},
};

BYTE* At(DWORD va) { return Base() + (va - kImageBase); }

bool Readable(DWORD va, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi;
    BYTE* addr = At(va);
    return VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           (BYTE*)mbi.BaseAddress + mbi.RegionSize >= addr + len;
}

bool Poke(DWORD va, const void* data, size_t len)
{
    BYTE* addr = At(va);
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, data, len);
    VirtualProtect(addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return true;
}

bool SlotIsOriginal(const ResSlot& s)
{
    const BYTE movW[] = { 0xC7, 0x44, 0x24, 0x04 }, movH[] = { 0xC7, 0x44, 0x24, 0x08 };
    const BYTE cmpW[] = { 0x81, 0xF9 }, cmpH[] = { 0x3D };
    if (!InCode(s.applyW, 8) || !InCode(s.applyH, 8) || !InCode(s.findW, 6) || !InCode(s.findH, 5)) return false;
    if (memcmp(At(s.applyW), movW, 4) || *(DWORD*)At(s.applyW + 4) != s.w) return false;
    if (memcmp(At(s.applyH), movH, 4) || *(DWORD*)At(s.applyH + 4) != s.h) return false;
    if (memcmp(At(s.findW), cmpW, 2) || *(DWORD*)At(s.findW + 2) != s.w) return false;
    if (memcmp(At(s.findH), cmpH, 1) || *(DWORD*)At(s.findH + 1) != s.h) return false;

    const DWORD entry = kResLabelTableVA + 8 * s.index;
    if (!Readable(entry, 8) || *(DWORD*)At(entry + 4) != (DWORD)s.index) return false;
    const DWORD label = *(DWORD*)At(entry);
    if (!Readable(label, kResLabelSize)) return false;
    char expected[16];
    wsprintfA(expected, "%lux%lu", s.w, s.h);
    return strcmp((const char*)At(label), expected) == 0;
}

// Libellés de nos résolutions, indexés par numéro de message du jeu.
typedef const char*(__fastcall* GetMessage_t)(void* self, void* edx, DWORD id); // __thiscall, 1 argument
GetMessage_t g_getMessage = nullptr;
DWORD        g_resMsgId[5] = {};
char         g_resLabel[5][16] = {};
int          g_resCount = 0;

const char* __fastcall Hook_ResolutionLabel(void* self, void* edx, DWORD id)
{
    for (int i = 0; i < g_resCount; ++i)
        if (g_resMsgId[i] == id) return g_resLabel[i];
    return g_getMessage(self, edx, id);
}

// --- Tri de la liste du menu par taille ---
// La liste des résolutions proposées est construite par 0x4DF7C0 (__thiscall, sans argument), appelée
// en 0x4DF4D0 et 0x4DF6F6 : intersection des modes du pilote et des 12 libellés, rangée dans l'ordre
// des libellés. On la trie par taille après chaque construction, pour que nos résolutions soient à leur
// place. Les entrées sont des paires (libellé "LxH", valeur) ; le menu les retrouve par libellé.
// Seule position mémorisée par le jeu : [this+442208h], remise à jour après le tri.
const DWORD kBuildModesVA     = 0x4DF7C0;
const DWORD kBuildModesCall[] = { 0x4DF4D0, 0x4DF6F6 };
const DWORD kModeListOffset   = 0x442210; // entrées de 8 octets
const DWORD kModeCountOffset  = 0x44220C;
const DWORD kModeDefaultOff   = 0x442208;
const DWORD kModeListMax      = 0x100;    // taille de la liste dans l'objet du jeu (0x442210..0x442A10)

typedef void(__fastcall* BuildModes_t)(void* self, void* edx);
BuildModes_t g_buildModes = nullptr;

unsigned long long LabelPixels(const char* label, DWORD* w)
{
    char* end;
    *w = strtoul(label, &end, 10);
    DWORD h = (*end == 'x') ? strtoul(end + 1, nullptr, 10) : 0;
    return (unsigned long long)*w * h;
}

void __fastcall Hook_BuildModes(void* self, void* edx)
{
    g_buildModes(self, edx);

    BYTE* obj = (BYTE*)self;
    DWORD count = *(DWORD*)(obj + kModeCountOffset);
    if (count < 2 || count > kModeListMax) return;
    DWORD (*list)[2] = (DWORD(*)[2])(obj + kModeListOffset);

    DWORD def = *(DWORD*)(obj + kModeDefaultOff);
    DWORD defLabel = def < count ? list[def][0] : 0;

    for (DWORD i = 1; i < count; ++i) {         // tri par insertion (stable), par nombre de pixels puis largeur
        DWORD e0 = list[i][0], e1 = list[i][1], w;
        unsigned long long p = LabelPixels((const char*)e0, &w);
        DWORD j = i;
        while (j > 0) {
            DWORD pw;
            unsigned long long pp = LabelPixels((const char*)list[j - 1][0], &pw);
            if (pp < p || (pp == p && pw <= w)) break;
            list[j][0] = list[j - 1][0];
            list[j][1] = list[j - 1][1];
            --j;
        }
        list[j][0] = e0;
        list[j][1] = e1;
    }

    if (defLabel)
        for (DWORD i = 0; i < count; ++i)
            if (list[i][0] == defLabel) { *(DWORD*)(obj + kModeDefaultOff) = i; break; }
}

// --- Super-sampling de la scène 4/3 -------------------------------------------------------------
// Une "vue" de rendu (fonction 0x8CF0xx) crée son contexte (0xA706C0 : scène, profondeur, images
// intermédiaires) soit à une taille relative à l'écran (drapeau 100h), soit à une taille fixe lue dans
// les données : largeur [esi+0FCh], hauteur [esi+100h]. La scène 4/3 utilise la taille fixe 1920x1440
// (celle des décors). On multiplie cette taille au moment où elle est lue (0x8CF20D).
const DWORD kSceneSizeVA = 0x8CF20D;
const BYTE  kSceneSizeOrig[] = { 0x8B, 0xBE, 0x00, 0x01, 0x00, 0x00,   // mov edi,[esi+100h] (hauteur)
                                 0x8B, 0x9E, 0xFC, 0x00, 0x00, 0x00 }; // mov ebx,[esi+0FCh] (largeur)
DWORD g_sceneSizeReturn = 0;
DWORD g_sceneScale = 1;   // facteur choisi pour la scène en cours de création (lu par le stub)

// Résolution choisie dans le jeu : objet d'affichage [0D5A048h], hauteur en +433E54h.
const DWORD kDisplayPtrVA   = 0xD5A048;
const DWORD kDisplayHeightOff = 0x433E54;

// Appelé au moment où le jeu crée le contexte de la scène 4/3 (1920x1440) : choisit le facteur.
extern "C" void __cdecl ChooseSceneScale()
{
    UINT screenH = 0;
    BYTE* disp = *(BYTE**)At(kDisplayPtrVA);
    if (disp) screenH = *(DWORD*)(disp + kDisplayHeightOff);
    g_sceneScale = SceneScaleFor(screenH);
    static DWORD lastH = ~0u, lastK = 0;
    if (screenH != lastH || g_sceneScale != lastK) {
        lastH = screenH; lastK = g_sceneScale;
        Log("Scène 4/3 : écran de %u lignes -> 1920x1440 x%lu = %lux%lu",
            screenH, g_sceneScale, 1920 * g_sceneScale, 1440 * g_sceneScale);
    }
}

__declspec(naked) void Stub_SceneSize()
{
    __asm {
        mov  edi, [esi + 100h]         // instructions d'origine
        mov  ebx, [esi + 0FCh]
        cmp  ebx, 1920
        jne  done
        cmp  edi, 1440
        jne  done
        pushfd
        pushad
        call ChooseSceneScale
        popad
        popfd
        imul ebx, dword ptr [g_sceneScale]
        imul edi, dword ptr [g_sceneScale]
    done:
        jmp  dword ptr [g_sceneSizeReturn]
    }
}

void ApplySceneScale()
{
    if (g_cfg.sceneScale == 0 || g_cfg.sceneScale == 1) return; // pas d'agrandissement de la scène
    if (!Matches("SceneScale", kSceneSizeVA, kSceneSizeOrig, sizeof(kSceneSizeOrig))) return;
    if (!Readable(kDisplayPtrVA, 4)) return;
    g_sceneSizeReturn = (DWORD)(ULONG_PTR)At(kSceneSizeVA + sizeof(kSceneSizeOrig));
    BYTE jmp[12] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    *(DWORD*)(jmp + 1) = (DWORD)(ULONG_PTR)Stub_SceneSize - (DWORD)(ULONG_PTR)(At(kSceneSizeVA) + 5);
    if (Poke(kSceneSizeVA, jmp, sizeof(jmp))) {
        if (g_cfg.sceneScale < 0) Log("Patch 'SceneScale' appliqué (auto)");
        else Log("Patch 'SceneScale' appliqué (x%d)", g_cfg.sceneScale);
    }
}

// Lit "2560x1440,3840x2160,..." ; renvoie le nombre de résolutions valides (triées par taille décroissante).
int ParseResolutions(const char* text, DWORD out[][2], int max)
{
    int n = 0;
    const char* p = text;
    while (*p && n < max) {
        while (*p == ' ' || *p == ',' || *p == ';') ++p;
        if (!*p) break;
        char* end;
        DWORD w, h;
        if (_strnicmp(p, "auto", 4) == 0) {          // résolution native de l'écran
            p += 4;
            DEVMODEA dm = {};
            dm.dmSize = sizeof(dm);
            if (!EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) continue;
            w = dm.dmPelsWidth; h = dm.dmPelsHeight;
            Log("Résolution native de l'écran : %lux%lu", w, h);
            goto add;
        }
        w = strtoul(p, &end, 10);
        if (*end != 'x' && *end != 'X') {           // entrée invalide : on passe à la suivante
            while (*p && *p != ',' && *p != ';') ++p;
            continue;
        }
        h = strtoul(end + 1, &end, 10);
        p = end;
        if (*p && *p != ',' && *p != ';' && *p != ' ') { // "1920x1080abc" : invalide
            while (*p && *p != ',' && *p != ';') ++p;
            continue;
        }
    add:
        if (w < 640 || h < 480 || w > 16384 || h > 16384) continue;
        bool dup = false;
        for (int i = 0; i < 12; ++i) dup |= kResOrig[i][0] == w && kResOrig[i][1] == h;
        for (int i = 0; i < n; ++i) dup |= out[i][0] == w && out[i][1] == h;
        if (dup) continue;
        out[n][0] = w; out[n][1] = h; ++n;
    }
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if ((unsigned long long)out[j][0] * out[j][1] > (unsigned long long)out[i][0] * out[i][1]) {
                DWORD tw = out[i][0], th = out[i][1];
                out[i][0] = out[j][0]; out[i][1] = out[j][1];
                out[j][0] = tw; out[j][1] = th;
            }
    return n;
}

void ApplyResolutions()
{
    const int kMax = sizeof(kResSlots) / sizeof(kResSlots[0]);
    DWORD res[kMax][2];
    const int n = ParseResolutions(g_cfg.resolutions, res, kMax);
    if (n == 0) return;

    for (int i = 0; i < n; ++i)
        if (!SlotIsOriginal(kResSlots[i])) {
            Log("'Resolutions' désactivé : code inattendu (version du jeu différente ?)");
            return;
        }

    // Appel du texte du menu : "call 004763C0" attendu en 0x527AB2.
    if (!InCode(kResMsgCallVA, 5) || *At(kResMsgCallVA) != 0xE8 ||
        kResMsgCallVA + 5 + *(DWORD*)At(kResMsgCallVA + 1) != kGetMessageVA ||
        !Readable(kResMsgTableVA, 12 * 12)) {
        Log("'Resolutions' désactivé : code inattendu (version du jeu différente ?)");
        return;
    }

    // Libellés du menu : on remplace le texte des entrées concernées.
    g_getMessage = (GetMessage_t)At(kGetMessageVA);
    for (int i = 0; i < n; ++i) {
        g_resMsgId[i] = *(DWORD*)At(kResMsgTableVA + 12 * kResSlots[i].index);
        wsprintfA(g_resLabel[i], "%lux%lu", res[i][0], res[i][1]);
    }
    g_resCount = n;
    BYTE call[5] = { 0xE8 };
    *(DWORD*)(call + 1) = (DWORD)(ULONG_PTR)Hook_ResolutionLabel - (DWORD)(ULONG_PTR)(At(kResMsgCallVA) + 5);
    if (!Poke(kResMsgCallVA, call, 5)) {
        Log("'Resolutions' : redirection du texte du menu impossible");
        g_resCount = 0;
        return;
    }

    // Tri de la liste du menu (facultatif : sans lui, nos résolutions restent à la place des anciennes).
    bool sortOk = true;
    for (DWORD c : kBuildModesCall)
        sortOk &= InCode(c, 5) && *At(c) == 0xE8 && c + 5 + *(DWORD*)At(c + 1) == kBuildModesVA;
    if (sortOk) {
        g_buildModes = (BuildModes_t)At(kBuildModesVA);
        for (DWORD c : kBuildModesCall) {
            BYTE jmp[5] = { 0xE8 };
            *(DWORD*)(jmp + 1) = (DWORD)(ULONG_PTR)Hook_BuildModes - (DWORD)(ULONG_PTR)(At(c) + 5);
            Poke(c, jmp, 5);
        }
        Log("Liste des résolutions du menu triée par taille");
    } else {
        Log("Tri de la liste des résolutions désactivé : code inattendu");
    }

    for (int i = 0; i < n; ++i) {
        const ResSlot& s = kResSlots[i];
        const DWORD w = res[i][0], h = res[i][1];
        char label[kResLabelSize] = {};
        wsprintfA(label, "%lux%lu", w, h);
        const DWORD labelVA = *(DWORD*)At(kResLabelTableVA + 8 * s.index);
        if (!Poke(s.applyW + 4, &w, 4) || !Poke(s.applyH + 4, &h, 4) ||
            !Poke(s.findW + 2, &w, 4) || !Poke(s.findH + 1, &h, 4) ||
            !Poke(labelVA, label, kResLabelSize)) {
            Log("'Resolutions' : écriture impossible pour %s", label);
            return;
        }
        Log("Résolution %lux%lu ajoutée au menu (à la place de %lux%lu)", w, h, s.w, s.h);
    }
}

// --- Animations de porte en pleine largeur ---------------------------------------------------------
// La porte est toujours calculée en pleine largeur ; en rendu original, cDoorDemoControl (0x47CA50)
// affiche par-dessus uBhdPrim_SidePanel (panneaux noirs à gauche et à droite) si mDisplayMode == 0.
// On remplace "jne masquer" par "jmp masquer" : les panneaux ne sont jamais affichés.
const DWORD kDoorPanelVA = 0x47CC75;
const BYTE kDoorPanelOrig[] = {
    0xA1, 0x28, 0x2F, 0xCF, 0x00,                   // mov  eax,[0CF2F28h]
    0x8B, 0x4A, 0x0C,                               // mov  ecx,[edx+0Ch]
    0x83, 0xB8, 0x1C, 0x51, 0x00, 0x00, 0x00,       // cmp  dword ptr [eax+511Ch],0
    0x75, 0x0C,                                     // jne  masquer
    0x81, 0xC9, 0x00, 0x08, 0x00, 0x00,             // or   ecx,800h (afficher)
};
const BYTE kDoorPanelNew[] = {
    0xA1, 0x28, 0x2F, 0xCF, 0x00,
    0x8B, 0x4A, 0x0C,
    0x83, 0xB8, 0x1C, 0x51, 0x00, 0x00, 0x00,
    0xEB, 0x0C,                                     // jmp  masquer
    0x81, 0xC9, 0x00, 0x08, 0x00, 0x00,
};

} // namespace

void Patches_Apply()
{
    static bool done = false;
    if (done) return;
    done = true;

    if (g_cfg.skipSaveNotice && Matches("SkipSaveNotice", kNoticeVA, kNoticeOrig, sizeof(kNoticeOrig)))
        Write("SkipSaveNotice", kNoticeVA, kNoticeNew, sizeof(kNoticeNew));

    if (g_cfg.fullscreenCutscenes &&
        Matches("FullscreenCutscenes", kLetterboxReadVA, kLetterboxRead, sizeof(kLetterboxRead)) &&
        Matches("FullscreenCutscenes", kLetterboxModeVA, kLetterboxMode, sizeof(kLetterboxMode)) &&
        Matches("FullscreenCutscenes", kLetterboxHeightVA, kLetterboxHeight, sizeof(kLetterboxHeight)) &&
        Matches("FullscreenCutscenes", kLetterboxFadeVA, kLetterboxFadeOrig, sizeof(kLetterboxFadeOrig)) &&
        Write("InstantLetterbox", kLetterboxFadeVA, kLetterboxFadeNew, sizeof(kLetterboxFadeNew))) {
        g_letterboxOk = true;
    }

    if (g_cfg.wideDoors && Matches("WideDoors", kDoorPanelVA, kDoorPanelOrig, sizeof(kDoorPanelOrig)))
        Write("WideDoors", kDoorPanelVA, kDoorPanelNew, sizeof(kDoorPanelNew));

    ApplyResolutions();
    ApplySceneScale();
}

bool Game_LetterboxActive()
{
    if (!g_letterboxOk) return false;
    BYTE* game = *(BYTE**)(Base() + (kGamePtrVA - kImageBase));
    if (!game) return false;
    const DWORD flags = *(volatile DWORD*)(game + 0x2C);
    const DWORD mode  = *(volatile DWORD*)(game + 0x511C);
    return mode == 0 && ((flags >> 17) & 1) != 0;
}
