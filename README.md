# Resident Evil HD Remaster – mod de confort et de qualité d'image

Un mod léger pour **Resident Evil / biohazard HD REMASTER** (Steam), pensé pour jouer en « rendu original » (4/3).

**Version actuelle : 1.1.0** – [télécharger](../../releases/latest)

- 🖱️ **Curseur de la souris caché** pendant le jeu.
- 🎬 **Cinématiques en plein écran**, sans déformation ni saut visible.
- 🚪 **Transitions entre pièces en pleine largeur** (portes, escaliers, ascenseurs…).
- ✨ **Image plus fine** : super-sampling automatique, 3D bien moins crénelée, décors aussi nets qu'à l'origine.
- 🖥️ **Résolution native de l'écran ajoutée au menu** si le jeu ne la propose pas (1440p, 4K…).
- ⏩ **Démarrage direct au menu principal**, sans l'écran d'avertissement de sauvegarde.

## Nouveautés de la 1.1.0

- Les cinématiques passent en plein écran dès la première image (le mod suit désormais le jeu au lieu d'analyser l'image).
- Les transitions entre pièces s'affichent sur toute la largeur.
- Super-sampling de la scène en rendu original, adapté automatiquement à votre résolution, avec une réduction de qualité (Lanczos) à la place du filtre bilinéaire du jeu.
- Résolutions supplémentaires dans le menu des options.

## Installation

1. Téléchargez le `.zip` de la [dernière version](../../releases/latest).
2. Dans Steam : clic droit sur le jeu → **Gérer** → **Parcourir les fichiers locaux**.
3. Copiez `dinput8.dll` et `bhd_mod.ini` à côté de `bhd.exe` (remplacez les anciens si vous mettez à jour).
4. Lancez le jeu, et choisissez **la résolution native de votre écran** dans les options vidéo.

**Désinstallation :** supprimez `dinput8.dll` et `bhd_mod.ini`.

## Réglages

Tout se règle dans `bhd_mod.ini` (Bloc-notes), pris en compte au prochain lancement. Les valeurs par défaut conviennent à la plupart des configurations.

| Option | Par défaut | Effet |
|---|---|---|
| `HideCursor` | 1 | Cache le curseur de la souris. |
| `FullscreenCutscenes` | 1 | Cinématiques en plein écran. |
| `KeepSubtitles` | 1 | Garde les sous-titres de la bande noire du bas. |
| `WideDoors` | 1 | Transitions entre pièces en pleine largeur. |
| `Resolutions` | auto | Ajoute la résolution native au menu. Vous pouvez en ajouter d'autres : `auto,3840x2160`. |
| `SceneScale` | auto | Qualité d'image : `auto` (recommandé), `1` à `4` (facteur fixe), `0` (rendu d'origine). |
| `SkipSaveNotice` | 1 | Supprime l'écran d'avertissement au lancement. |
| `Log` | 0 | Journal `bhd_mod.log`, utile en cas de problème. |

💡 Inutile d'activer le DSR/VSR de votre carte graphique : le mod fait déjà le super-sampling, mieux et sans flou supplémentaire.

## Bon à savoir

- `bhd.exe` n'est pas modifié : tout se passe en mémoire au lancement.
- Si une mise à jour du jeu change le code concerné, la fonction touchée se désactive d'elle-même et le jeu reste jouable.
- Incompatible avec un autre mod utilisant lui aussi `dinput8.dll`.

## Compiler soi-même

Visual Studio 2022 (outils C++ x86), puis `build.bat` : la DLL est produite dans `build\`.

---

Mod non officiel, sans lien avec Capcom. Aucun fichier du jeu n'est inclus.
