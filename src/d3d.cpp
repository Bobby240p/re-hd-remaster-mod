// Hooks Direct3D 9 :
//  - IDirect3DDevice9::ShowCursor forcé à FALSE (si HideCursor)
//  - cinématiques letterbox en "rendu original" (4/3) agrandies pour remplir l'écran (si FullscreenCutscenes)
//
// Principe de la détection (à chaque Present, sur l'image finale) :
//  * l'image 4/3 est centrée dans le backbuffer, avec des bandes noires à gauche et à droite ;
//  * pendant une cinématique, le jeu ajoute des bandes noires en haut et en bas DANS la zone 4/3 ;
//  * on copie quelques colonnes de pixels, on cherche des bandes noires horizontales nettes et
//    symétriques ; si elles sont stables plusieurs images de suite, on verrouille le rectangle utile
//    et on l'agrandit à l'écran en conservant son ratio ;
//  * on sort du mode dès que du contenu apparaît dans la bande du haut (ou près du bord de celle du bas)
//    ou que les bandes latérales ne sont plus noires. Une image entièrement noire (fondu) ne change rien.

#include "common.h"
#include <d3d9.h>
#include <stdlib.h>

namespace {

typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* SCPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef BOOL(STDMETHODCALLTYPE* DevShowCursor_t)(IDirect3DDevice9*, BOOL);

Direct3DCreate9_t oDirect3DCreate9 = nullptr;
CreateDevice_t    oCreateDevice    = nullptr;
Reset_t           oReset           = nullptr;
Present_t         oPresent         = nullptr;
SCPresent_t       oSCPresent       = nullptr;
DevShowCursor_t   oDevShowCursor   = nullptr;

// Index dans les vtables
const int kVT_D3D_CreateDevice   = 16;
const int kVT_Dev_ShowCursor     = 12;
const int kVT_Dev_Reset          = 16;
const int kVT_Dev_Present        = 17;
const int kVT_SwapChain_Present  = 3;

// Échantillonnage
const int kPillarCols = 4;   // colonnes dans les bandes latérales
const int kInnerCols  = 24;  // colonnes dans la zone 4/3
const int kCols       = kPillarCols + kInnerCols;
const int kBlackMax   = 8;   // composante <= kBlackMax => pixel considéré noir
const int kEnterFrames = 4;  // images stables nécessaires pour entrer en mode cinématique

struct State {
    IDirect3DDevice9*  dev       = nullptr;
    UINT               w = 0, h = 0;
    D3DFORMAT          fmt       = D3DFMT_UNKNOWN;
    bool               usable    = false;
    IDirect3DTexture9* copyTex   = nullptr;
    IDirect3DSurface9* copySurf  = nullptr;
    IDirect3DSurface9* sampleRT  = nullptr;
    IDirect3DSurface9* sampleSys = nullptr;
    int  x0 = 0, x43 = 0;
    int  colX[kCols] = {};

    bool active   = false;
    int  streak   = 0;
    int  candTop  = -1, candBot = -1;
    int  lockTop  = 0,  lockBot = 0;   // lignes de contenu [lockTop, lockBot]
    unsigned frame = 0;
    bool inPresent = false;
} S;

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

void ReleaseResources()
{
    SafeRelease(S.copySurf);
    SafeRelease(S.copyTex);
    SafeRelease(S.sampleRT);
    SafeRelease(S.sampleSys);
    S.usable = false;
    S.w = S.h = 0;
    S.fmt = D3DFMT_UNKNOWN;
    S.active = false;
    S.streak = 0;
}

bool EnsureResources(IDirect3DDevice9* dev, const D3DSURFACE_DESC& d)
{
    if (S.dev != dev) { ReleaseResources(); S.dev = dev; }
    if (S.usable && S.w == d.Width && S.h == d.Height && S.fmt == d.Format) return true;

    // Configuration déjà jugée inutilisable pour cette taille/format : ne pas réessayer à chaque image.
    static UINT badW = 0, badH = 0; static D3DFORMAT badF = D3DFMT_UNKNOWN;
    if (!S.usable && d.Width == badW && d.Height == badH && d.Format == badF) return false;

    ReleaseResources();
    S.w = d.Width; S.h = d.Height; S.fmt = d.Format;
    Log("Backbuffer %ux%u format=%d msaa=%d", d.Width, d.Height, (int)d.Format, (int)d.MultiSampleType);

    auto fail = [&](const char* why) {
        Log("Cinématiques plein écran inactives : %s", why);
        badW = d.Width; badH = d.Height; badF = d.Format;
        ReleaseResources();
        return false;
    };

    if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8)
        return fail("format du backbuffer non géré");

    S.x43 = (int)((d.Height * 4 + 1) / 3);
    if ((int)d.Width < S.x43 + 16)
        return fail("l'écran n'est pas plus large que 4/3");
    S.x0 = ((int)d.Width - S.x43) / 2;

    if (FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &S.copyTex, nullptr)) ||
        FAILED(S.copyTex->GetSurfaceLevel(0, &S.copySurf)) ||
        FAILED(dev->CreateRenderTarget(kCols, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &S.sampleRT, nullptr)) ||
        FAILED(dev->CreateOffscreenPlainSurface(kCols, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &S.sampleSys, nullptr))) {
        // Peut arriver si le périphérique est perdu : on réessaiera plus tard.
        Log("Création des ressources impossible (réessai plus tard)");
        ReleaseResources();
        return false;
    }

    const int w = (int)d.Width;
    const int x1 = S.x0 + S.x43;
    S.colX[0] = S.x0 / 4;
    S.colX[1] = S.x0 * 3 / 4;
    S.colX[2] = x1 + (w - x1) / 4;
    S.colX[3] = x1 + (w - x1) * 3 / 4;
    const int margin = S.x43 * 3 / 100;
    for (int i = 0; i < kInnerCols; ++i)
        S.colX[kPillarCols + i] = S.x0 + margin + (S.x43 - 2 * margin - 1) * i / (kInnerCols - 1);
    for (int i = 0; i < kCols; ++i) {
        if (S.colX[i] < 0) S.colX[i] = 0;
        if (S.colX[i] > w - 1) S.colX[i] = w - 1;
    }

    S.usable = true;
    Log("Zone 4/3 : x=%d largeur=%d", S.x0, S.x43);
    return true;
}

inline bool IsBlack(DWORD p)
{
    return ((p >> 16) & 0xFF) <= kBlackMax && ((p >> 8) & 0xFF) <= kBlackMax && (p & 0xFF) <= kBlackMax;
}

void Analyze(const BYTE* bits, int pitch)
{
    const int h = (int)S.h;
    auto px = [&](int c, int y) { return *(const DWORD*)(bits + y * pitch + c * 4); };

    bool pillarBlack = true;
    for (int c = 0; c < kPillarCols && pillarBlack; ++c)
        for (int y = 0; y < h; ++y)
            if (!IsBlack(px(c, y))) { pillarBlack = false; break; }

    int top[kInnerCols], bot[kInnerCols], nonEmpty = 0;
    for (int i = 0; i < kInnerCols; ++i) {
        const int c = kPillarCols + i;
        int t = -1, b = -1;
        for (int y = 0; y < h; ++y)
            if (!IsBlack(px(c, y))) { t = y; break; }
        if (t >= 0) {
            for (int y = h - 1; y >= t; --y)
                if (!IsBlack(px(c, y))) { b = y; break; }
            ++nonEmpty;
        }
        top[i] = t; bot[i] = b;
    }

    ++S.frame;

    // Nombre de colonnes ayant du contenu dans la bande collée sous l'image (lignes après `contentBot`).
    // Les sous-titres sont plus bas, vers le centre de la bande noire, et ne la touchent pas.
    auto bottomEdgeIntrusions = [&](int contentBot) {
        const int barH  = h - 1 - contentBot;
        const int band0 = contentBot + 2;
        int band1 = band0 + (barH / 5 > 2 ? barH / 5 : 2);
        if (band1 > h) band1 = h;
        int n = 0;
        for (int i = 0; i < kInnerCols; ++i)
            for (int y = band0; y < band1; ++y)
                if (!IsBlack(px(kPillarCols + i, y))) { ++n; break; }
        return n;
    };

    if (S.active) {
        const char* why = nullptr;
        if (!pillarBlack) {
            why = "bandes latérales non noires";
        } else if (nonEmpty > 0) {
            int topIntr = 0;
            for (int i = 0; i < kInnerCols; ++i)
                if (top[i] >= 0 && top[i] < S.lockTop - 1) ++topIntr;

            if (topIntr >= 3) why = "contenu dans la bande du haut";
            else if (bottomEdgeIntrusions(S.lockBot) >= kInnerCols / 4) why = "contenu dans la bande du bas";
        }
        if (why) {
            S.active = false;
            S.streak = 0;
            Log("Fin de cinématique (%s)", why);
        }
        return;
    }

    bool valid = false;
    int T = -1, B = -1;
    if (pillarBlack && nonEmpty >= kInnerCols / 2 && top[0] >= 0 && top[kInnerCols - 1] >= 0) {
        // Bord haut = première ligne non noire ; bord bas attendu = symétrique (letterbox centré).
        // Le bas réel ne sert pas à trouver le bord : des sous-titres peuvent occuper la bande du bas.
        T = h;
        for (int i = 0; i < kInnerCols; ++i)
            if (top[i] >= 0 && top[i] < T) T = top[i];
        B = h - 1 - T;
        const int tol = h / 100 + 2;
        int edgeT = 0, edgeB = 0;
        for (int i = 0; i < kInnerCols; ++i) {
            if (top[i] < 0) continue;
            if (top[i] <= T + 1) ++edgeT;
            if (bot[i] >= B - tol && bot[i] <= B + tol) ++edgeB;
        }
        // Affine B sur le bord réellement observé (colonnes qui s'arrêtent près du bord attendu).
        int bMax = -1;
        for (int i = 0; i < kInnerCols; ++i)
            if (top[i] >= 0 && bot[i] >= B - tol && bot[i] <= B + tol && bot[i] > bMax) bMax = bot[i];
        if (bMax >= 0) B = bMax;
        const int ch = B - T + 1;
        const double aspect = (double)S.x43 / ch;
        valid = edgeT * 2 >= nonEmpty && edgeT >= 6 &&
                edgeB * 3 >= nonEmpty && edgeB >= 6 &&
                T >= h * 4 / 100 &&
                bottomEdgeIntrusions(B) < kInnerCols / 4 &&
                aspect >= 1.45 && aspect <= 2.6;
    }

    if (valid && S.streak > 0 && abs(T - S.candTop) <= 1 && abs(B - S.candBot) <= 1) ++S.streak;
    else S.streak = valid ? 1 : 0;
    if (valid) { S.candTop = T; S.candBot = B; }

    if (S.streak >= kEnterFrames) {
        S.active  = true;
        S.lockTop = T;
        S.lockBot = B;
        Log("Cinématique détectée : contenu lignes %d-%d (%dx%d, ratio %.3f)",
            T, B, S.x43, B - T + 1, (double)S.x43 / (B - T + 1));
    } else if (g_cfg.log && S.frame % 300 == 0) {
        Log("Analyse : bandes latérales noires=%d colonnes non vides=%d haut=%d bas=%d valide=%d",
            pillarBlack, nonEmpty, T, B, valid);
    }
}

struct Vtx { float x, y, z, rhw, u, v; };

void Quad(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
    x0 -= 0.5f; y0 -= 0.5f; x1 -= 0.5f; y1 -= 0.5f; // correspondance pixel/texel D3D9
    Vtx v[4] = {
        { x0, y0, 0.f, 1.f, u0, v0 },
        { x1, y0, 0.f, 1.f, u1, v0 },
        { x0, y1, 0.f, 1.f, u0, v1 },
        { x1, y1, 0.f, 1.f, u1, v1 },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
}

void DrawZoom(IDirect3DDevice9* dev, IDirect3DSurface9* bb)
{
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;

    IDirect3DSurface9* oldRT[4] = {};
    for (DWORD i = 0; i < 4; ++i) dev->GetRenderTarget(i, &oldRT[i]);
    IDirect3DSurface9* oldDS = nullptr;
    dev->GetDepthStencilSurface(&oldDS);

    dev->SetRenderTarget(0, bb);
    for (DWORD i = 1; i < 4; ++i) if (oldRT[i]) dev->SetRenderTarget(i, nullptr);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = { 0, 0, S.w, S.h, 0.f, 1.f };
    dev->SetViewport(&vp);

    if (SUCCEEDED(dev->BeginScene())) {
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.f, 0);

        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

        dev->SetTexture(0, S.copyTex);
        for (DWORD i = 1; i < 8; ++i) dev->SetTexture(i, nullptr);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);

        const float W = (float)S.w, H = (float)S.h;
        const int   srcTop = S.lockTop, srcBot = S.lockBot + 1; // [srcTop, srcBot)
        const float cw = (float)S.x43, ch = (float)(srcBot - srcTop);
        float scale = W / cw;
        if (H / ch < scale) scale = H / ch;
        const float dw = cw * scale, dh = ch * scale;
        const float dx = (W - dw) * 0.5f, dy = (H - dh) * 0.5f;

        // 1) image utile agrandie, ratio conservé
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        Quad(dev, dx, dy, dx + dw, dy + dh,
             S.x0 / W, srcTop / H, (S.x0 + S.x43) / W, srcBot / H);

        // 2) bande du bas (sous-titres) superposée en addition : le noir n'ajoute rien.
        const int barH = (int)S.h - srcBot;
        if (g_cfg.keepSubtitles && barH > 0) {
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            const float sx = (W - cw) * 0.5f;
            Quad(dev, sx, H - barH, sx + cw, H,
                 S.x0 / W, srcBot / H, (S.x0 + S.x43) / W, 1.f);
        }

        dev->EndScene();
    }

    for (DWORD i = 0; i < 4; ++i) {
        if (i > 0 || oldRT[0]) dev->SetRenderTarget(i, oldRT[i]); // la cible 0 ne peut pas être nulle
        SafeRelease(oldRT[i]);
    }
    dev->SetDepthStencilSurface(oldDS);
    SafeRelease(oldDS);

    sb->Apply();
    sb->Release();
}

void ProcessFrame(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;

    D3DSURFACE_DESC d;
    if (SUCCEEDED(bb->GetDesc(&d)) && EnsureResources(dev, d) &&
        SUCCEEDED(dev->StretchRect(bb, nullptr, S.copySurf, nullptr, D3DTEXF_NONE))) {
        bool ok = true;
        for (int i = 0; i < kCols && ok; ++i) {
            RECT src = { S.colX[i], 0, S.colX[i] + 1, (LONG)S.h };
            RECT dst = { i, 0, i + 1, (LONG)S.h };
            ok = SUCCEEDED(dev->StretchRect(S.copySurf, &src, S.sampleRT, &dst, D3DTEXF_NONE));
        }
        if (ok && SUCCEEDED(dev->GetRenderTargetData(S.sampleRT, S.sampleSys))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(S.sampleSys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                Analyze((const BYTE*)lr.pBits, lr.Pitch);
                S.sampleSys->UnlockRect();
            }
        }
        if (S.active) DrawZoom(dev, bb);
    }
    bb->Release();
}

// --- Hooks ------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* rgn)
{
    if (S.inPresent || !g_cfg.fullscreenCutscenes) return oPresent(dev, src, dst, wnd, rgn);
    static bool logged = false;
    if (!logged) { logged = true; Log("Present (device) src=%p dst=%p", src, dst); }
    S.inPresent = true;
    ProcessFrame(dev);
    HRESULT hr = oPresent(dev, src, dst, wnd, rgn);
    S.inPresent = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_SCPresent(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* rgn, DWORD flags)
{
    if (S.inPresent || !g_cfg.fullscreenCutscenes) return oSCPresent(sc, src, dst, wnd, rgn, flags);
    static bool logged = false;
    if (!logged) { logged = true; Log("Present (swapchain) src=%p dst=%p", src, dst); }
    S.inPresent = true;
    IDirect3DDevice9* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(&dev)) && dev) {
        ProcessFrame(dev);
        dev->Release();
    }
    HRESULT hr = oSCPresent(sc, src, dst, wnd, rgn, flags);
    S.inPresent = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    ReleaseResources(); // les ressources D3DPOOL_DEFAULT doivent être libérées avant Reset
    HRESULT hr = oReset(dev, pp);
    Log("Reset -> 0x%08lX", (unsigned long)hr);
    return hr;
}

BOOL STDMETHODCALLTYPE Hook_DevShowCursor(IDirect3DDevice9* dev, BOOL)
{
    return oDevShowCursor(dev, FALSE);
}

void HookDevice(IDirect3DDevice9* dev)
{
    if (!oReset)   oReset   = (Reset_t)PatchVTable(dev, kVT_Dev_Reset, (void*)Hook_Reset);
    if (!oPresent) oPresent = (Present_t)PatchVTable(dev, kVT_Dev_Present, (void*)Hook_Present);
    if (g_cfg.hideCursor && !oDevShowCursor)
        oDevShowCursor = (DevShowCursor_t)PatchVTable(dev, kVT_Dev_ShowCursor, (void*)Hook_DevShowCursor);

    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        if (!oSCPresent) oSCPresent = (SCPresent_t)PatchVTable(sc, kVT_SwapChain_Present, (void*)Hook_SCPresent);
        sc->Release();
    }
    if (g_cfg.hideCursor) dev->ShowCursor(FALSE);
}

HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                            D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    // Un périphérique "pur" ne permet pas de sauvegarder/restaurer l'état : on retire ce drapeau.
    if (g_cfg.fullscreenCutscenes) flags &= ~D3DCREATE_PUREDEVICE;
    HRESULT hr = oCreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    Log("CreateDevice -> 0x%08lX (flags=0x%lX, %ux%u, fenêtré=%d)", (unsigned long)hr, flags,
        pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1);
    if (SUCCEEDED(hr) && out && *out) HookDevice(*out);
    return hr;
}

IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdk)
{
    // Appelé depuis le code du jeu : il est déjà déchiffré par la protection Steam.
    DumpCode();
    Patches_Apply();
    IDirect3D9* d3d = oDirect3DCreate9(sdk);
    if (d3d && !oCreateDevice)
        oCreateDevice = (CreateDevice_t)PatchVTable(d3d, kVT_D3D_CreateDevice, (void*)Hook_CreateDevice);
    return d3d;
}

} // namespace

void D3D_Install()
{
    HookDef hooks[] = {
        { "Direct3DCreate9", (void*)Hook_Direct3DCreate9, (void**)&oDirect3DCreate9 },
    };
    PatchIAT(GetModuleHandleA(nullptr), "d3d9.dll", hooks, 1);
}
