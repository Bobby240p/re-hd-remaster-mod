// Mise à l'échelle de qualité (Lanczos-3 séparable) sur le GPU, en deux passes :
//   1) horizontale : source -> image intermédiaire en flottant 16 bits (largeur de destination) ;
//   2) verticale   : image intermédiaire -> destination.
// En réduction, le noyau est élargi selon le rapport (chaque pixel de sortie couvre toute sa zone
// de source) : pas de crénelage ni de scintillement. En agrandissement, Lanczos-3 classique.
// Les shaders sont compilés au premier usage avec d3dcompiler_47.dll (fourni par Windows).

#include "common.h"
#include "resample.h"
#include <d3dcompiler.h>

namespace {

const char kShaderSource[] = R"(
struct VSO { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VSO vs_main(float4 pos : POSITION, float2 uv : TEXCOORD0) { VSO o; o.pos = pos; o.uv = uv; return o; }

sampler2D src : register(s0);
float4 cTex  : register(c0); // largeur, hauteur, 1/largeur, 1/hauteur de la texture source
float4 cRect : register(c1); // zone source : x, y, largeur, hauteur (en pixels)
float4 cAxis : register(c2); // x : 1 = horizontal, 0 = vertical ; y : pixels source par pixel de sortie ;
                             // z : 1 = moyenne exacte par zone (rapport entier), 0 = Lanczos-3

static const float PI = 3.14159265;

float lanczos3(float x)
{
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 3.0) return 0.0;
    float a = PI * x;
    return 3.0 * sin(a) * sin(a / 3.0) / (a * a);
}

float4 ps_main(float2 uv : TEXCOORD0) : COLOR
{
    float2 p = cRect.xy + uv * cRect.zw;          // position dans la source (pixels)
    bool horiz = cAxis.x > 0.5;
    float c = horiz ? p.x : p.y;
    float other = horiz ? p.y : p.x;
    float s = max(cAxis.y, 1.0);                  // élargissement du noyau en réduction
    bool box = cAxis.z > 0.5;
    float support = box ? 0.5 * s : 3.0 * s;
    float lo = horiz ? cRect.x : cRect.y;
    float hi = lo + (horiz ? cRect.z : cRect.w);
    float first = floor(c - support) + 0.5;       // centre du premier pixel source
    float4 acc = 0;
    float wsum = 0;
    [loop] for (int i = 0; i < 64; ++i) {
        float q = first + i;
        if (q > c + support) break;
        float w = box ? (abs(q - c) < support ? 1.0 : 0.0) : lanczos3((q - c) / s);
        float qc = clamp(q, lo + 0.5, hi - 0.5);  // bords : on répète le dernier pixel de la zone
        float2 t = horiz ? float2(qc, other) : float2(other, qc);
        acc += tex2Dlod(src, float4(t * cTex.zw, 0, 0)) * w;
        wsum += w;
    }
    return acc / wsum;
}
)";

typedef HRESULT(WINAPI* D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);

IDirect3DDevice9*            g_dev  = nullptr;
IDirect3DVertexShader9*      g_vs   = nullptr;
IDirect3DPixelShader9*       g_ps   = nullptr;
IDirect3DVertexDeclaration9* g_decl = nullptr;
bool                         g_failed = false;

// Image intermédiaire (D3DPOOL_DEFAULT : libérée avant chaque Reset).
IDirect3DTexture9* g_mid = nullptr;
UINT               g_midW = 0, g_midH = 0;

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool Compile(D3DCompile_t compile, const char* entry, const char* profile, ID3DBlob** out)
{
    ID3DBlob* err = nullptr;
    HRESULT hr = compile(kShaderSource, sizeof(kShaderSource) - 1, "bhd_mod_resample", nullptr, nullptr,
                         entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) {
        Log("Compilation du shader %s impossible : %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err);
        return false;
    }
    SafeRelease(err);
    return true;
}

bool Init(IDirect3DDevice9* dev)
{
    if (g_dev == dev && g_ps) return true;
    if (g_failed) return false;
    Resampler_Shutdown();

    HMODULE lib = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompile_t compile = lib ? (D3DCompile_t)GetProcAddress(lib, "D3DCompile") : nullptr;
    if (!compile) {
        Log("d3dcompiler_47.dll introuvable : mise à l'échelle de qualité indisponible");
        g_failed = true;
        return false;
    }
    ID3DBlob *vsb = nullptr, *psb = nullptr;
    bool ok = Compile(compile, "vs_main", "vs_3_0", &vsb) && Compile(compile, "ps_main", "ps_3_0", &psb) &&
              SUCCEEDED(dev->CreateVertexShader((const DWORD*)vsb->GetBufferPointer(), &g_vs)) &&
              SUCCEEDED(dev->CreatePixelShader((const DWORD*)psb->GetBufferPointer(), &g_ps));
    SafeRelease(vsb);
    SafeRelease(psb);
    const D3DVERTEXELEMENT9 elems[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    ok = ok && SUCCEEDED(dev->CreateVertexDeclaration(elems, &g_decl));
    if (!ok) {
        Resampler_Shutdown();
        g_failed = true;
        Log("Mise à l'échelle de qualité indisponible");
        return false;
    }
    g_dev = dev;
    Log("Mise à l'échelle Lanczos prête");
    return true;
}

bool EnsureMid(IDirect3DDevice9* dev, UINT w, UINT h)
{
    if (g_mid && g_midW == w && g_midH == h) return true;
    SafeRelease(g_mid);
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_mid, nullptr))) {
        Log("Création de l'image intermédiaire %ux%u impossible", w, h);
        return false;
    }
    g_midW = w; g_midH = h;
    return true;
}

struct Vtx { float x, y, z, w, u, v; };

// Une passe : dessine dans la cible courante, sur la zone `vp`, la zone `rect` de `src`.
void Pass(IDirect3DDevice9* dev, IDirect3DTexture9* src, UINT srcW, UINT srcH, const float rect[4],
          bool horizontal, float ratio, bool box, const D3DVIEWPORT9& vp)
{
    dev->SetViewport(&vp);
    dev->SetTexture(0, src);
    const float cTex[4]  = { (float)srcW, (float)srcH, 1.f / srcW, 1.f / srcH };
    const float cAxis[4] = { horizontal ? 1.f : 0.f, ratio, box ? 1.f : 0.f, 0.f };
    dev->SetPixelShaderConstantF(0, cTex, 1);
    dev->SetPixelShaderConstantF(1, rect, 1);
    dev->SetPixelShaderConstantF(2, cAxis, 1);

    // Quadrilatère plein écran (dans la zone du viewport), décalé d'un demi-pixel (règle D3D9).
    const float hx = 1.f / vp.Width, hy = 1.f / vp.Height;
    Vtx v[4] = {
        { -1.f - hx,  1.f + hy, 0.5f, 1.f, 0.f, 0.f },
        {  1.f - hx,  1.f + hy, 0.5f, 1.f, 1.f, 0.f },
        { -1.f - hx, -1.f + hy, 0.5f, 1.f, 0.f, 1.f },
        {  1.f - hx, -1.f + hy, 0.5f, 1.f, 1.f, 1.f },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
}

} // namespace

void Resampler_OnReset()
{
    SafeRelease(g_mid);
    g_midW = g_midH = 0;
}

void Resampler_Shutdown()
{
    Resampler_OnReset();
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    SafeRelease(g_decl);
    g_dev = nullptr;
}

bool Resample(IDirect3DDevice9* dev, IDirect3DTexture9* src, const RECT& srcRect,
              IDirect3DSurface9* dst, const RECT& dstRect, ResampleFilter filter)
{
    const bool box = filter == ResampleFilter::Box;
    if (!src || !dst || !Init(dev)) return false;

    D3DSURFACE_DESC sd;
    if (FAILED(src->GetLevelDesc(0, &sd))) return false;
    const UINT rw = srcRect.right - srcRect.left, rh = srcRect.bottom - srcRect.top;
    const UINT dw = dstRect.right - dstRect.left, dh = dstRect.bottom - dstRect.top;
    if (!rw || !rh || !dw || !dh || !EnsureMid(dev, dw, rh)) return false;

    IDirect3DSurface9* mid = nullptr;
    if (FAILED(g_mid->GetSurfaceLevel(0, &mid))) return false;

    // État commun aux deux passes.
    dev->SetVertexShader(g_vs);
    dev->SetPixelShader(g_ps);
    dev->SetVertexDeclaration(g_decl);
    dev->SetDepthStencilSurface(nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
    for (DWORD i = 1; i < 4; ++i) dev->SetRenderTarget(i, nullptr);

    // Si le jeu a déjà une scène ouverte (appel pendant son rendu), on dessine dedans.
    const bool ownScene = SUCCEEDED(dev->BeginScene());
    const bool ok = true;
    {
        // 1) horizontale : zone source -> intermédiaire (dw x rh)
        dev->SetRenderTarget(0, mid);
        const float rect1[4] = { (float)srcRect.left, (float)srcRect.top, (float)rw, (float)rh };
        const D3DVIEWPORT9 vp1 = { 0, 0, dw, rh, 0.f, 1.f };
        Pass(dev, src, sd.Width, sd.Height, rect1, true, (float)rw / dw, box, vp1);

        // 2) verticale : intermédiaire -> zone de destination (dw x dh)
        dev->SetRenderTarget(0, dst);
        const float rect2[4] = { 0.f, 0.f, (float)dw, (float)rh };
        const D3DVIEWPORT9 vp2 = { (DWORD)dstRect.left, (DWORD)dstRect.top, dw, dh, 0.f, 1.f };
        Pass(dev, g_mid, dw, rh, rect2, false, (float)rh / dh, box, vp2);

        dev->SetTexture(0, nullptr);
        if (ownScene) dev->EndScene();
    }
    mid->Release();
    return ok;
}
