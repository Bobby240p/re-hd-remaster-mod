#pragma once
#include <d3d9.h>

enum class ResampleFilter {
    Lanczos, // Lanczos-3 séparable (noyau élargi en réduction) : rapports quelconques
    Box,     // moyenne exacte par zone : réduction d'un rapport entier (restitution pixel pour pixel)
};

// Copie la zone `srcRect` de `src` dans la zone `dstRect` de `dst` (cible de rendu) avec le filtre donné.
// Modifie l'état du périphérique : l'appelant le sauvegarde/restaure.
// Renvoie false si la mise à l'échelle de qualité est indisponible.
bool Resample(IDirect3DDevice9* dev, IDirect3DTexture9* src, const RECT& srcRect,
              IDirect3DSurface9* dst, const RECT& dstRect, ResampleFilter filter = ResampleFilter::Lanczos);

void Resampler_OnReset();   // avant IDirect3DDevice9::Reset
void Resampler_Shutdown();
