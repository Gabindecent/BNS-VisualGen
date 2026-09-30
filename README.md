# BNS VisualGen

Logiciel de VJing génératif et audio-réactif pour les soirées de BNS Sound System, association loi 1901
dédiée aux cultures de musiques électroniques (tekno, scène locale).

C++20 / [openFrameworks 0.12.1](https://openframeworks.cc/) / Linux. Deux fenêtres : une **sortie** sans
bordure pour le vidéoprojecteur (60 FPS, aucune interface) et un **tableau de bord** opérateur (ImGui).

## Démarrage

```bash
git clone https://github.com/Gabindecent/BNS-VisualGen.git
cd BNS-VisualGen
./build_and_run.sh
```

Le script fait tout, et saute les étapes déjà faites :

1. outils de base (git, curl, make, g++, pkg-config) ;
2. openFrameworks 0.12.1, téléchargé dans `../openFrameworks` s'il est introuvable ;
3. dépendances système d'openFrameworks, via son script officiel (mot de passe sudo demandé) ;
4. addon ofxImGui, épinglé sur un commit précis (build reproductible) ;
5. compilation ;
6. lancement.

| Option | Effet |
|---|---|
| `--no-deps` | ne pas vérifier/installer les dépendances |
| `-y`, `--yes` | répondre oui aux questions d'installation |
| `--debug` | compilation Debug |
| `--clean` | nettoyer avant de compiler |
| `--build-only` | compiler sans lancer |
| `--of-root CHEMIN` | openFrameworks installé ailleurs |
| `-- ARGS` | options passées à l'application (ci-dessous) |

Options de l'application : `--monitor N` (écran de sortie, défaut : le 2e écran s'il existe),
`--windowed` (sortie en fenêtre 1280x720 pour répéter sur un seul écran), `--render 1280x720`
(résolution interne, à baisser sur une petite carte graphique).

```bash
./build_and_run.sh -- --windowed          # répétition sur l'ordi portable seul
./build_and_run.sh -- --monitor 1         # soirée : sortie sur le vidéoprojecteur
```

## En live

| Touche (fenêtre de contrôle) | Action |
|---|---|
| `1` à `9` | scène 1 à 9 |
| `n` | scène suivante |
| `espace` | palette suivante |
| `f` | figer l'image (freeze) |
| `c` | couper / rétablir la webcam |

Si l'écran de contrôle est perdu, la fenêtre de sortie accepte aussi `1`-`9` et `n`. `Échap` ne quitte
**pas** l'application (sécurité en plein set).

La barre du bas du tableau de bord affiche en permanence les FPS de la sortie (rouge sous 57) et le coût
mesuré de la dernière bascule de scène.

## Scènes

| Scène | Motif |
|---|---|
| **Morphogen** | réaction-diffusion de Gray-Scott (Turing) : taches, labyrinthes, mycélium ; les kicks plantent des germes |
| **Fluid** | nappes fractales qui coulent (domain warping) ; les basses gonflent les volutes |
| **Mandala** | kaléidoscope polaire, bras en spirale logarithmique ; les kicks font respirer les anneaux |
| **Spore** | spores qui dérivent, cernes de croissance, filaments de mycélium qui les relient |

Le changement de scène est instantané : tous les shaders sont compilés **et préchauffés** au démarrage, la
bascule ne fait que changer un index. Transition au choix : coupe franche, ou trame de blocs (jamais de fondu).

## Couleurs : aplats fluo, sans dégradé ni contour noir

Les scènes ne calculent jamais de couleur : elles produisent un **champ** de valeurs 0..1. Une passe unique
(`composite.frag`) découpe ce champ en bandes et donne à chaque bande une couleur **pleine** de la palette.
Les palettes n'acceptent que des couleurs vives (saturation ≥ 0,55, luminosité ≥ 0,70) : le noir, les
contours sombres et les dégradés sont impossibles par construction, pour toutes les scènes présentes et
futures. Les aplats peuvent défiler en continu et sauter d'un cran à chaque kick.

## Webcam

La caméra passe par une boucle de feedback (zoom, rotation, rémanence : effet "Larsen vidéo") sur sa
luminance, puis est combinée au motif de la scène **avant** la palette, donc elle aussi en aplats :

- **Silhouette** : les aplats se décalent à l'intérieur des zones claires filmées (le public apparaît dans le motif) ;
- **Ajout** : l'image caméra tord le motif ;
- **Caméra seule** : la boucle de feedback caméra, colorisée.

Les futurs effets caméra s'ajoutent dans `bin/data/shaders/webcam_feedback.frag`.

## Audio

Entrée par défaut du système (PulseAudio/PipeWire, ALSA en secours) : brancher la sortie de la table de
mix sur l'entrée ligne. FFT 2048 points : **kick** (40-120 Hz), **mid** (250 Hz-2 kHz), **high**
(4-12 kHz), lissage à constantes de temps réelles, auto-normalisation (pas de gain à retoucher entre les
morceaux), détection de kick et estimation du BPM.

## Structure

```
build_and_run.sh              installation + build + lancement
src/main.cpp                  les deux fenêtres (sortie sans bordure / contrôle)
src/ofApp.h, ofApp.cpp        AudioEngine, PaletteManager, scènes, SceneManager, WebcamLayer, fenêtres
bin/data/shaders/
  passthrough.vert            vertex shader commun
  composite.frag              champ -> aplats de palette, transition tramée, combinaison webcam
  webcam_feedback.frag        boucle de feedback caméra
  scenes/*.frag               un fichier par scène (morphogen = 2 passes)
bin/data/fonts/               police de l'interface
legacy/v1/                    version précédente, archivée (non compilée)
```

## À propos

Projet développé pour et par BNS Sound System. Bureau : Yan Hourdebaigt--Hiroux (co-président, trésorier),
Gabin Alquier (co-président, secrétaire).
