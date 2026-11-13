# Resident Evil HD Remaster – petit mod de confort

Un mod léger pour **Resident Evil / biohazard HD REMASTER** (version Steam) qui corrige trois petites gênes :

- 🖱️ **Curseur de la souris caché** pendant le jeu.
- 🎬 **Cinématiques en plein écran** en « rendu original » (4/3) : au lieu d'une petite image entourée de bandes noires sur les quatre côtés, la cinématique est agrandie pour remplir l'écran, sans déformation.
- ⏩ **Démarrage plus rapide** : l'écran d'avertissement sur la sauvegarde automatique (« Suivant ») est supprimé, on arrive directement au menu principal.

Chaque option peut être activée ou désactivée dans un fichier texte.

## Installation

1. Téléchargez le fichier `.zip` de la dernière version dans la page [Releases](../../releases).
2. Ouvrez le dossier du jeu : dans Steam, clic droit sur le jeu → **Gérer** → **Parcourir les fichiers locaux**.
3. Copiez-y `dinput8.dll` et `bhd_mod.ini`, à côté de `bhd.exe`.
4. Lancez le jeu normalement depuis Steam.

**Désinstallation :** supprimez `dinput8.dll` (et `bhd_mod.ini`) du dossier du jeu.

## Réglages

Ouvrez `bhd_mod.ini` avec le Bloc-notes. `1` = activé, `0` = désactivé. Les changements sont pris en compte au prochain lancement.

| Option | Par défaut | Effet |
|---|---|---|
| `HideCursor` | 1 | Cache le curseur de la souris dans la fenêtre du jeu. |
| `FullscreenCutscenes` | 1 | En rendu original (4/3), agrandit les cinématiques à bandes noires pour remplir l'écran. |
| `KeepSubtitles` | 1 | Garde les sous-titres affichés dans la bande noire du bas. |
| `SkipSaveNotice` | 1 | Supprime l'écran d'avertissement de sauvegarde au lancement. |
| `Log` | 0 | Écrit un journal `bhd_mod.log` à côté du jeu (utile en cas de problème). |

## Bon à savoir

- Le fichier `bhd.exe` n'est **pas modifié** : tout se passe en mémoire, au lancement.
- Si une mise à jour du jeu change le code concerné, la suppression de l'écran d'avertissement se désactive d'elle-même (le jeu reste jouable normalement).
- Les cinématiques sont détectées en analysant l'image. Si une scène n'est pas agrandie alors qu'elle devrait (ou l'inverse), mettez `Log=1` et ouvrez une *issue* avec le contenu de `bhd_mod.log`.
- Incompatible avec un autre mod qui utilise lui aussi un `dinput8.dll`.

## Compiler soi-même

Prérequis : Visual Studio 2022 avec les outils C++ (x86). Lancez `build.bat` : la DLL est produite dans `build\`.

---

Mod non officiel, sans lien avec Capcom. Aucun fichier du jeu n'est inclus.
