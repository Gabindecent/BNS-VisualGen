# BNS VisualGen

Logiciel de VJing et de génération visuelle pour [BNS Sound System](https://github.com/), association loi 1901
dédiée à la promotion et à la diffusion des cultures de musiques électroniques (tekno, scène locale).

Écrit en C++ avec [openFrameworks](https://openframeworks.cc/), esthétique terminal/rétro-tech (noir, vert
phosphorescent, jaune acide fluo).

## Fonctionnalités

- **Onglet Vidéo/Webcam** : mapping vidéo interactif avec effet de feedback ("Larsen vidéo") — zoom, rotation
  et traînée en boucle sur le flux caméra, passé dans un shader de bichromie (couleur claire / couleur sombre,
  seuil réglable). Dérive automatique des couleurs disponible (la couleur sombre suit toujours la teinte
  complémentaire de la couleur claire, pour garder du contraste).
- **Mode Génératif** : shader procédural (moiré, vortex, anneaux concentriques) tricolore, indépendant de la
  caméra.
- Les deux couches sont indépendamment activables et se superposent automatiquement si les deux sont actives.
- **Réactivité audio** : analyse spectrale (FFT) de l'entrée audio (table de mix branchée en ligne sur la carte
  son) — basses/médiums/aigus pilotent la luminosité, le zoom et la vitesse de dérive des couleurs.
- Dashboard de contrôle sur une fenêtre séparée (idéal en second écran), en menus déroulants.

## Prérequis

- [openFrameworks](https://openframeworks.cc/download/) (testé avec la branche compatible OF 0.12).
- Linux (le projet utilise les addons/libs Linux d'OF : GStreamer pour la caméra, ALSA/PulseAudio pour l'audio).
- L'addon `ofxGui` (livré avec openFrameworks).

## Installation

1. Télécharger/cloner openFrameworks et le compiler une première fois (voir la doc officielle OF pour Linux).
2. Cloner ce dépôt **dans `openFrameworks/apps/myApps/`** (ou ajuster `OF_ROOT` dans `config.make` sinon) :
   ```bash
   cd openFrameworks/apps/myApps/
   git clone <url-du-repo> BNS-VisualGen
   cd BNS-VisualGen
   ```
3. Compiler et lancer :
   ```bash
   make -j4
   make RunRelease
   ```

Deux fenêtres s'ouvrent : **BNS VisualGen - Sortie** (le rendu, à envoyer au vidéoprojecteur) et
**BNS VisualGen - Dashboard** (le panneau de contrôle, à garder sur l'écran de contrôle/second écran).

## Utilisation rapide

- Dans le Dashboard, les 2 interrupteurs du haut (`VIDEO / WEBCAM` / `MODE GENERATIF`) activent/désactivent
  chaque couche du rendu.
- Chaque section (`MUSIQUE (AUDIO)`, `VIDEO / WEBCAM`, `MODE GENERATIF`) est un menu déroulant : cliquer sur
  son titre pour l'ouvrir/le replier.
- Pour la réactivité audio : brancher la sortie de la table de mix sur l'entrée ligne/jack de l'ordinateur,
  puis activer `Reagit a la musique` dans `MUSIQUE (AUDIO)`.

## Structure du projet

- `src/main.cpp` — création des deux fenêtres (Sortie / Dashboard).
- `src/ofApp.h` / `ofApp.cpp` — logique principale (mapping vidéo, générateur, audio, GUI).
- `bin/data/shader.vert` / `shader.frag` — shader de bichromie (onglet Vidéo/Webcam).
- `bin/data/generative.frag` — shader procédural du Mode Génératif (moiré / vortex / anneaux).
- `bin/data/fonts/` — police embarquée pour l'interface.

## À propos

Projet développé pour et par BNS Sound System. Bureau : Yan Hourdebaigt--Hiroux (co-président, trésorier),
Gabin Alquier (co-président, secrétaire).
