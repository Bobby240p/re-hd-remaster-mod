// Hooks Direct3D 9 :
//  - IDirect3DDevice9::ShowCursor forcé à FALSE (si HideCursor)
//  - cinématiques en "rendu original" (4/3) agrandies pour remplir l'écran (si FullscreenCutscenes)
//  - super-sampling de la scène 4/3 (si SceneScale >= 2) : décor agrandi sans lissage dans la scène
//    agrandie, puis réduction finale en Lanczos (resample.cpp) à la place de celle du jeu
//
// Les cinématiques ne sont pas détectées en analysant l'image : on lit l'état du jeu (voir patches.cpp,
// Game_LetterboxActive) au moment de l'affichage de chaque image. Quand le jeu affiche ses bandes noires,
// on agrandit la partie entre les bandes, avec la géométrie exacte utilisée par le jeu.

#include "common.h"
#include "resample.h"
#include <d3d9.h>

bool Game_LetterboxActive(); // patches.cpp

namespace {

typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* SCPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef BOOL(STDMETHODCALLTYPE* DevShowCursor_t)(IDirect3DDevice9*, BOOL);
typedef HRESULT(STDMETHODCALLTYPE* CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* SetRenderTarget_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT(STDMETHODCALLTYPE* DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawIndexedPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);

Direct3DCreate9_t oDirect3DCreate9 = nullptr;
CreateDevice_t    oCreateDevice    = nullptr;
Reset_t           oReset           = nullptr;
Present_t         oPresent         = nullptr;
SCPresent_t       oSCPresent       = nullptr;
DevShowCursor_t   oDevShowCursor   = nullptr;
CreateTexture_t      oCreateTexture      = nullptr;
SetRenderTarget_t        oSetRenderTarget        = nullptr;
SetTexture_t             oSetTexture             = nullptr;
DrawPrimitive_t          oDrawPrimitive          = nullptr;
DrawIndexedPrimitive_t   oDrawIndexedPrimitive   = nullptr;
DrawPrimitiveUP_t        oDrawPrimitiveUP        = nullptr;
DrawIndexedPrimitiveUP_t oDrawIndexedPrimitiveUP = nullptr;

// Index dans les vtables
const int kVT_D3D_CreateDevice  = 16;
const int kVT_Dev_ShowCursor    = 12;
const int kVT_Dev_Reset         = 16;
const int kVT_Dev_Present       = 17;
const int kVT_SwapChain_Present = 3;

// Géométrie des bandes du jeu, dans son repère d'interface 1280x720 (voir patches.cpp) :
// l'image utile va de la ligne 56 (58 - 2) à la ligne 662 (720 - 58).
const float kUiHeight     = 720.f;
const float kContentTop   = 56.f;
const float kContentBot   = 662.f;

struct State {
    IDirect3DDevice9*  dev      = nullptr;
    UINT               w = 0, h = 0;
    D3DFORMAT          fmt      = D3DFMT_UNKNOWN;
    IDirect3DTexture9* copyTex  = nullptr;
    IDirect3DSurface9* copySurf = nullptr;
    bool               active   = false;
    bool               inPresent = false;
} S;

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// --- Super-sampling de la scène 4/3 : réduction finale en Lanczos -------------------------------
// Les images de la scène agrandie (SceneScale) sont repérées à leur création. Quand le jeu en dessine
// une sur une cible de la taille de l'écran (composition finale), on la réduit d'abord nous-mêmes en
// Lanczos à la taille exacte de destination, et on la lui substitue avec un échantillonnage au pixel
// près : le jeu la recopie 1:1, ses propres effets (filtre, fondus, couleurs) restent appliqués.
struct Scene {
    IDirect3DBaseTexture9* textures[16] = {};   // images de la scène agrandie (non possédées)
    int                    count = 0, next = 0;
    IDirect3DBaseTexture9* bound[16] = {};      // textures liées par le jeu, par étage
    UINT                   rtW = 0, rtH = 0;    // cible de rendu 0 courante du jeu
    UINT                   bbW = 0, bbH = 0;    // taille de l'écran (backbuffer)
    IDirect3DTexture9*     smallTex = nullptr;     // scène réduite (taille de destination)
    UINT                   smallW = 0, smallH = 0;
    IDirect3DBaseTexture9* lastSrc = nullptr;
    unsigned               lastFrame = ~0u;
    unsigned               frame = 0;
    bool                   inOwn = false;       // nos propres appels : ne pas suivre / intercepter
    UINT                   loggedW = 0, loggedH = 0;
    bool                   loggedBg = false;
} G;

bool IsSceneTexture(IDirect3DBaseTexture9* t)
{
    if (!t) return false;
    for (int i = 0; i < G.count; ++i) if (G.textures[i] == t) return true;
    return false;
}

void SceneForget()
{
    G.count = 0; G.next = 0;
    for (auto& b : G.bound) b = nullptr;
    SafeRelease(G.smallTex);
    G.smallW = G.smallH = 0;
    G.lastSrc = nullptr;
    G.bbW = G.bbH = 0;
}

// Réduit `src` vers G.smallTex (dstW x dstH) avec le filtre donné. Renvoie G.smallTex, ou nullptr.
IDirect3DTexture9* SceneDownsample(IDirect3DDevice9* dev, IDirect3DTexture9* src, UINT dstW, UINT dstH,
                                   ResampleFilter filter)
{
    if (G.lastSrc == src && G.lastFrame == G.frame && G.smallTex && G.smallW == dstW && G.smallH == dstH)
        return G.smallTex;
    if (!G.smallTex || G.smallW != dstW || G.smallH != dstH) {
        SafeRelease(G.smallTex);
        if (FAILED(dev->CreateTexture(dstW, dstH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                      &G.smallTex, nullptr)))
            return nullptr;
        G.smallW = dstW; G.smallH = dstH;
    }
    D3DSURFACE_DESC sd;
    IDirect3DSurface9* dst = nullptr;
    if (FAILED(src->GetLevelDesc(0, &sd)) || FAILED(G.smallTex->GetSurfaceLevel(0, &dst))) return nullptr;

    // Sauvegarde complète de l'état du jeu.
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) { dst->Release(); return nullptr; }
    IDirect3DSurface9* oldRT[4] = {};
    for (DWORD i = 0; i < 4; ++i) dev->GetRenderTarget(i, &oldRT[i]);
    IDirect3DSurface9* oldDS = nullptr;
    dev->GetDepthStencilSurface(&oldDS);
    IDirect3DVertexBuffer9* vb = nullptr; UINT vbOff = 0, vbStride = 0;
    dev->GetStreamSource(0, &vb, &vbOff, &vbStride);
    IDirect3DIndexBuffer9* ib = nullptr;
    dev->GetIndices(&ib);

    const RECT srcRect = { 0, 0, (LONG)sd.Width, (LONG)sd.Height };
    const RECT dstRect = { 0, 0, (LONG)dstW, (LONG)dstH };
    const bool ok = Resample(dev, src, srcRect, dst, dstRect, filter);

    for (DWORD i = 0; i < 4; ++i) {
        if (i > 0 || oldRT[0]) dev->SetRenderTarget(i, oldRT[i]);
        SafeRelease(oldRT[i]);
    }
    dev->SetDepthStencilSurface(oldDS);
    SafeRelease(oldDS);
    sb->Apply();
    sb->Release();
    dev->SetStreamSource(0, vb, vbOff, vbStride);
    dev->SetIndices(ib);
    SafeRelease(vb);
    SafeRelease(ib);
    dst->Release();

    if (!ok) return nullptr;
    G.lastSrc = src;
    G.lastFrame = G.frame;
    if (G.loggedW != sd.Width || G.loggedH != dstH) {
        G.loggedW = sd.Width; G.loggedH = dstH;
        Log("Composition de la scène : %ux%u -> %ux%u (%s)", sd.Width, sd.Height, dstW, dstH,
            filter == ResampleFilter::Box ? "moyenne exacte, rapport entier" : "Lanczos");
    }
    return G.smallTex;
}

// Décor précalculé (ou sa carte de profondeur) : texture 2D ordinaire de 1920x1440.
bool IsBackgroundTexture(IDirect3DBaseTexture9* t)
{
    if (!t || t->GetType() != D3DRTYPE_TEXTURE) return false;
    D3DSURFACE_DESC d;
    return SUCCEEDED(((IDirect3DTexture9*)t)->GetLevelDesc(0, &d)) && d.Width == 1920 && d.Height == 1440 &&
           !(d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
}

// Dessin dans la scène agrandie : le décor (1920x1440) y est agrandi exactement x SceneScale. On force
// l'échantillonnage au plus proche voisin : chaque pixel du décor devient un bloc entier, sans lissage.
template <class F> HRESULT BackgroundDraw(IDirect3DDevice9* dev, F draw)
{
    DWORD saved[16][2];
    bool changed[16] = {};
    bool any = false;
    G.inOwn = true;
    for (DWORD s = 0; s < 16; ++s) {
        if (!IsBackgroundTexture(G.bound[s])) continue;
        dev->GetSamplerState(s, D3DSAMP_MINFILTER, &saved[s][0]);
        dev->GetSamplerState(s, D3DSAMP_MAGFILTER, &saved[s][1]);
        dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        changed[s] = any = true;
    }
    G.inOwn = false;
    if (any && !G.loggedBg) {
        G.loggedBg = true;
        Log("Décor agrandi sans lissage dans la scène x%u", G.rtH / 1440);
    }
    HRESULT hr = draw();
    if (any) {
        G.inOwn = true;
        for (DWORD s = 0; s < 16; ++s)
            if (changed[s]) {
                dev->SetSamplerState(s, D3DSAMP_MINFILTER, saved[s][0]);
                dev->SetSamplerState(s, D3DSAMP_MAGFILTER, saved[s][1]);
            }
        G.inOwn = false;
    }
    return hr;
}

// Enveloppe un appel de dessin du jeu : substitue la scène réduite si c'est la composition finale.
template <class F> HRESULT SceneDraw(IDirect3DDevice9* dev, F draw)
{
    if (G.inOwn || S.inPresent || G.count == 0) return draw();
    // Dessin dans la scène agrandie (multiple entier de 1920x1440) : décor sans lissage.
    if (G.rtH > 1440 && G.rtH % 1440 == 0 && G.rtW * 3 == G.rtH * 4) return BackgroundDraw(dev, draw);
    if (!G.bbW || G.rtW != G.bbW || G.rtH != G.bbH) return draw();
    int stage = -1;
    for (int s = 0; s < 16; ++s)
        if (IsSceneTexture(G.bound[s])) { stage = s; break; }
    if (stage < 0) return draw();

    // Zone 4/3 à l'écran (écran plus large que 4/3 : hauteur pleine ; plus étroit : largeur pleine).
    UINT dstW, dstH;
    if (G.rtW * 3 >= G.rtH * 4) { dstH = G.rtH; dstW = (G.rtH * 4 + 1) / 3; }
    else                        { dstW = G.rtW; dstH = (G.rtW * 3 + 2) / 4; }

    D3DSURFACE_DESC sd;
    if (FAILED(((IDirect3DTexture9*)G.bound[stage])->GetLevelDesc(0, &sd))) return draw();
    if (sd.Width == dstW && sd.Height == dstH) return draw();   // déjà à la bonne taille : rien à faire

    // Rapport entier et décor tombant pile sur les pixels de l'écran (1440p, 5K...) : moyenne exacte,
    // le décor est restitué pixel pour pixel. Sinon : Lanczos.
    const bool exact = dstH % 1440 == 0 && sd.Height % dstH == 0 && sd.Width % dstW == 0;
    const ResampleFilter filter = exact ? ResampleFilter::Box : ResampleFilter::Lanczos;

    G.inOwn = true;
    IDirect3DTexture9* reduced = SceneDownsample(dev, (IDirect3DTexture9*)G.bound[stage], dstW, dstH, filter);
    if (!reduced) { G.inOwn = false; return draw(); }

    IDirect3DBaseTexture9* orig = G.bound[stage];
    DWORD minF = 0, magF = 0;
    dev->GetSamplerState(stage, D3DSAMP_MINFILTER, &minF);
    dev->GetSamplerState(stage, D3DSAMP_MAGFILTER, &magF);
    oSetTexture(dev, stage, reduced);
    dev->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT); // recopie 1:1, aucun lissage
    dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    G.inOwn = false;

    HRESULT hr = draw();

    G.inOwn = true;
    oSetTexture(dev, stage, orig);
    dev->SetSamplerState(stage, D3DSAMP_MINFILTER, minF);
    dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, magF);
    G.inOwn = false;
    return hr;
}

void ReleaseResources()
{
    SafeRelease(S.copySurf);
    SafeRelease(S.copyTex);
    S.w = S.h = 0;
    S.fmt = D3DFMT_UNKNOWN;
}

bool EnsureResources(IDirect3DDevice9* dev, const D3DSURFACE_DESC& d)
{
    if (S.dev != dev) { ReleaseResources(); S.dev = dev; }
    if (S.copyTex && S.w == d.Width && S.h == d.Height && S.fmt == d.Format) return true;
    ReleaseResources();
    if (FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &S.copyTex, nullptr)) ||
        FAILED(S.copyTex->GetSurfaceLevel(0, &S.copySurf))) {
        Log("Création de la texture de copie impossible (%ux%u format=%d)", d.Width, d.Height, (int)d.Format);
        ReleaseResources();
        return false;
    }
    S.w = d.Width; S.h = d.Height; S.fmt = d.Format;
    Log("Backbuffer %ux%u format=%d", d.Width, d.Height, (int)d.Format);
    return true;
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

// Dessine dans le backbuffer la zone [x0, x0+cw) x [top, bot) de la copie, agrandie (ratio conservé),
// puis, si demandé, la bande du bas (sous-titres) par-dessus en mode additif.
void DrawZoom(IDirect3DDevice9* dev, IDirect3DSurface9* bb, int x0, int cw, int top, int bot)
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

    const float W = (float)S.w, H = (float)S.h;
    const float fcw = (float)cw, fch = (float)(bot - top);
    float scale = W / fcw;
    if (H / fch < scale) scale = H / fch;
    const float dw = fcw * scale, dh = fch * scale;
    const float dx = (W - dw) * 0.5f, dy = (H - dh) * 0.5f;

    if (SUCCEEDED(dev->BeginScene())) {
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.f, 0);

        // 1) image utile agrandie, ratio conservé : Lanczos (repli sur bilinéaire si indisponible)
        const RECT srcRect = { x0, top, x0 + cw, bot };
        const RECT dstRect = { (LONG)(dx + 0.5f), (LONG)(dy + 0.5f), (LONG)(dx + dw + 0.5f), (LONG)(dy + dh + 0.5f) };
        const bool lanczos = Resample(dev, S.copyTex, srcRect, bb, dstRect);
        dev->SetRenderTarget(0, bb);
        D3DVIEWPORT9 full = { 0, 0, S.w, S.h, 0.f, 1.f };
        dev->SetViewport(&full);

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

        if (!lanczos) {
            dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            Quad(dev, dx, dy, dx + dw, dy + dh, x0 / W, top / H, (x0 + cw) / W, bot / H);
        }

        // 2) bande du bas (sous-titres sur fond noir) superposée en addition : le noir n'ajoute rien.
        const int barH = (int)S.h - bot;
        if (g_cfg.keepSubtitles && barH > 0) {
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            Quad(dev, (float)x0, H - barH, (float)(x0 + cw), H, x0 / W, bot / H, (x0 + cw) / W, 1.f);
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
    const bool want = Game_LetterboxActive();
    if (want != S.active) {
        S.active = want;
        Log(want ? "Cinématique : plein écran" : "Fin de cinématique");
    }
    if (!want) return;

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;

    D3DSURFACE_DESC d;
    if (SUCCEEDED(bb->GetDesc(&d))) {
        const int w = (int)d.Width, h = (int)d.Height;
        const int cw = (h * 4 + 1) / 3;                         // largeur de l'image 4/3
        const float s = h / kUiHeight;
        const int top = (int)(kContentTop * s + 0.5f);
        const int bot = (int)(kContentBot * s + 0.5f);
        if (w >= cw && bot > top && EnsureResources(dev, d) &&
            SUCCEEDED(dev->StretchRect(bb, nullptr, S.copySurf, nullptr, D3DTEXF_NONE)))
            DrawZoom(dev, bb, (w - cw) / 2, cw, top, bot);
    }
    bb->Release();
}

// --- Hooks ------------------------------------------------------------------

// Fin d'image : compteur d'images et taille de l'écran (pour repérer la composition finale).
void NewFrame(IDirect3DDevice9* dev)
{
    ++G.frame;
    if (!SceneQualityEnabled()) return;
    IDirect3DSurface9* bb = nullptr;
    D3DSURFACE_DESC d;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb && SUCCEEDED(bb->GetDesc(&d))) {
        G.bbW = d.Width; G.bbH = d.Height;
    }
    SafeRelease(bb);
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* rgn)
{
    if (!S.inPresent) NewFrame(dev);
    if (S.inPresent || !g_cfg.fullscreenCutscenes) return oPresent(dev, src, dst, wnd, rgn);
    S.inPresent = true;
    ProcessFrame(dev);
    HRESULT hr = oPresent(dev, src, dst, wnd, rgn);
    S.inPresent = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_SCPresent(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* rgn, DWORD flags)
{
    if (!S.inPresent) {
        IDirect3DDevice9* d = nullptr;
        if (SUCCEEDED(sc->GetDevice(&d)) && d) { NewFrame(d); d->Release(); }
    }
    if (S.inPresent || !g_cfg.fullscreenCutscenes) return oSCPresent(sc, src, dst, wnd, rgn, flags);
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
    SceneForget();
    Resampler_OnReset();
    HRESULT hr = oReset(dev, pp);
    Log("Reset -> 0x%08lX (%ux%u, fenêtré=%d)", (unsigned long)hr,
        pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT lv, DWORD usage, D3DFORMAT f,
                                             D3DPOOL pool, IDirect3DTexture9** out, HANDLE* sh)
{
    HRESULT hr = oCreateTexture(dev, w, h, lv, usage, f, pool, out, sh);

    // Images de la scène 4/3 (1920x1440 ou multiple) : à réduire avec un filtre de qualité à la composition.
    // Liste circulaire : les plus anciennes (scènes recréées depuis) sont remplacées en premier.
    if (SUCCEEDED(hr) && out && *out && !G.inOwn && !S.inPresent && (usage & D3DUSAGE_RENDERTARGET) &&
        h >= 1440 && h % 1440 == 0 && w * 3 == h * 4) {
        G.textures[G.next] = *out;
        G.next = (G.next + 1) % 16;
        if (G.count < 16) ++G.count;
    }
    return hr;
}

// --- Suivi de l'état du jeu et interception des dessins (super-sampling) ---
HRESULT STDMETHODCALLTYPE Hook_SetRenderTarget(IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* s)
{
    if (idx == 0 && !G.inOwn && !S.inPresent) {
        D3DSURFACE_DESC d;
        if (s && SUCCEEDED(s->GetDesc(&d))) { G.rtW = d.Width; G.rtH = d.Height; }
        else { G.rtW = G.rtH = 0; }
    }
    return oSetRenderTarget(dev, idx, s);
}

HRESULT STDMETHODCALLTYPE Hook_SetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* t)
{
    if (stage < 16 && !G.inOwn && !S.inPresent) G.bound[stage] = t;
    return oSetTexture(dev, stage, t);
}

HRESULT STDMETHODCALLTYPE Hook_DrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT start, UINT n)
{
    return SceneDraw(dev, [&] { return oDrawPrimitive(dev, t, start, n); });
}

HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT minIdx,
                                                    UINT nv, UINT start, UINT n)
{
    return SceneDraw(dev, [&] { return oDrawIndexedPrimitive(dev, t, base, minIdx, nv, start, n); });
}

HRESULT STDMETHODCALLTYPE Hook_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT n, const void* data, UINT stride)
{
    return SceneDraw(dev, [&] { return oDrawPrimitiveUP(dev, t, n, data, stride); });
}

HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT minIdx, UINT nv, UINT n,
                                                      const void* idx, D3DFORMAT fmt, const void* data, UINT stride)
{
    return SceneDraw(dev, [&] { return oDrawIndexedPrimitiveUP(dev, t, minIdx, nv, n, idx, fmt, data, stride); });
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
    if (SceneQualityEnabled() && !oSetTexture) { // super-sampling : composition finale en Lanczos
        oCreateTexture          = (CreateTexture_t)PatchVTable(dev, 23, (void*)Hook_CreateTexture);
        oSetRenderTarget        = (SetRenderTarget_t)PatchVTable(dev, 37, (void*)Hook_SetRenderTarget);
        oSetTexture             = (SetTexture_t)PatchVTable(dev, 65, (void*)Hook_SetTexture);
        oDrawPrimitive          = (DrawPrimitive_t)PatchVTable(dev, 81, (void*)Hook_DrawPrimitive);
        oDrawIndexedPrimitive   = (DrawIndexedPrimitive_t)PatchVTable(dev, 82, (void*)Hook_DrawIndexedPrimitive);
        oDrawPrimitiveUP        = (DrawPrimitiveUP_t)PatchVTable(dev, 83, (void*)Hook_DrawPrimitiveUP);
        oDrawIndexedPrimitiveUP = (DrawIndexedPrimitiveUP_t)PatchVTable(dev, 84, (void*)Hook_DrawIndexedPrimitiveUP);
    }

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
    if (g_cfg.fullscreenCutscenes || SceneQualityEnabled()) flags &= ~D3DCREATE_PUREDEVICE;
    HRESULT hr = oCreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    Log("CreateDevice -> 0x%08lX (%ux%u, fenêtré=%d)", (unsigned long)hr,
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
