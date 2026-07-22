# BNS-VisualGen

Logiciel VJing / génération visuelle pour BNS Sound System (association loi 1901, musiques électroniques / tekno).
Écrit en C++ avec openFrameworks (ofxGui pour l'interface de contrôle).

## Architecture

- `src/main.cpp` — crée deux fenêtres GLFW : sortie vidéoprojecteur ("BNS Output", 1920x1080) et dashboard de contrôle ("BNS Dashboard").
- `src/ofApp.h` / `ofApp.cpp` — logique principale :
  - Mode 1 (Mapping) : webcam → shader de bichromie → feedback ping-pong FBO (effet Larsen).
  - Mode 2 (Génératif) : formes psychédéliques autonomes (à développer).
  - `gui` (ofxPanel) : panneau de contrôle live, thème terminal tekno (noir/vert/jaune fluo).
- `bin/data/shader.vert` / `shader.frag` — shader de bichromie appliqué au flux vidéo.

## Direction artistique

Fond noir, texte vert phosphorescent (rétro-terminal), accents jaune acide fluo pour les éléments interactifs/actifs,
typographie monospace/pixel. Rendu brut, contrasté, assumé "artisanal".
