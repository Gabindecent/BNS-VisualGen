# BNS VisualGen v1 (archive)

Version précédente, conservée telle quelle au moment du passage à la v2 (30/09/2026) :
webcam + bichromie + feedback "Larsen", mode génératif nébuleuse, mode Turing, dashboard dessiné
à la main, presets JSON (`bin/data/presets/`).

Ce dossier n'est **pas compilé** (le Makefile d'openFrameworks ne compile que `src/`).

Pour relancer la v1 temporairement :
```bash
cp legacy/v1/src/* src/
cp legacy/v1/data/* bin/data/
./build_and_run.sh
```
(retirer `ofxImGui` de `addons.make` n'est pas nécessaire : il est simplement inutilisé en v1)
