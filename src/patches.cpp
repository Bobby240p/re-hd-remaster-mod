// Patchs du code de bhd.exe, appliqués en mémoire une fois le code déchiffré par la protection Steam.
// Chaque patch vérifie les octets d'origine : si le jeu a été mis à jour, il n'est pas appliqué.

#include "common.h"
#include <string.h>

namespace {

struct CodePatch {
    const char* name;
    DWORD       rva;          // adresse relative à la base du module
    const BYTE* expected;     // octets d'origine attendus
    size_t      expectedLen;
    const BYTE* replacement;
    size_t      replacementLen;
};

bool Apply(const CodePatch& p)
{
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    BYTE* addr = base + p.rva;
    MEMORY_BASIC_INFORMATION mbi;
    if (p.rva + p.expectedLen > nt->OptionalHeader.SizeOfImage ||
        !VirtualQuery(addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        (BYTE*)mbi.BaseAddress + mbi.RegionSize < addr + p.expectedLen) {
        Log("Patch '%s' NON appliqué : adresse hors du code de l'exécutable", p.name);
        return false;
    }
    if (memcmp(addr, p.expected, p.expectedLen) != 0) {
        Log("Patch '%s' NON appliqué : code inattendu (version du jeu différente ?)", p.name);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(addr, p.replacementLen, PAGE_EXECUTE_READWRITE, &old)) {
        Log("Patch '%s' NON appliqué : VirtualProtect a échoué", p.name);
        return false;
    }
    memcpy(addr, p.replacement, p.replacementLen);
    VirtualProtect(addr, p.replacementLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, p.replacementLen);
    Log("Patch '%s' appliqué", p.name);
    return true;
}

// --- Écran d'avertissement de sauvegarde automatique au lancement -------------------------------
// Procédure 3 de l'écran titre (fonction 0x467C00, sous-état dans [esi+0D8h]) :
//   0 : prépare uBhdGUI_NoticeMessage   1 : l'ouvre   2 : attend "Suivant"
//   3-4 : fermeture                    0x64 : passe à la procédure 0xC (menu principal)
// On remplace le cas 0 (0x467C32) par : sous-état = 0x64 -> menu principal, l'avertissement n'est jamais affiché.
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

} // namespace

void Patches_Apply()
{
    static bool done = false;
    if (done) return;
    done = true;

    if (g_cfg.skipSaveNotice) {
        CodePatch p = { "SkipSaveNotice", 0x467C32 - 0x400000,
                        kNoticeOrig, sizeof(kNoticeOrig), kNoticeNew, sizeof(kNoticeNew) };
        Apply(p);
    }
}
