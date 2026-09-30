# BNS-VisualGen v2

VJing génératif audio-réactif pour BNS Sound System (tekno / sound system). C++20, openFrameworks 0.12.1,
Linux, addon ofxImGui (épinglé dans build_and_run.sh). GL 2.1 / GLSL 1.20 dans les deux fenêtres.

## Build
`./build_and_run.sh` (deps + OF + addon + make + run). `--build-only`, `--debug`, `-- --windowed`.
openFrameworks attendu dans `../openFrameworks` (ou `--of-root`). Seul `src/` est compilé.

## Architecture (tout dans src/ofApp.h/.cpp, namespace bns)
- AudioEngine : thread audio = FFT kiss_fftr sans allocation, pont std::atomic ; thread principal =
  normalisation, lissage en secondes, onset kick, BPM.
- Scene : produit un CHAMP 0..1 (canal R) dans un FBO RGBA16F à 50 % de la résolution. JAMAIS de couleur.
  ShaderScene (1 passe : fluid, mandala, spore) ; MorphogenScene (Gray-Scott ping-pong, simulé dans update()).
- SceneManager : load + warmup de toutes les scènes au démarrage ; requestScene() = simple index ;
  bascule appliquée en début de frame côté sortie ; transition Cut ou Dither (trame Bayer).
- composite.frag : SEUL endroit où naît la couleur (quantification en bandes -> palette).
- WebcamLayer : luminance caméra -> boucle de feedback (zoom/rotation/rémanence) -> champ combiné dans composite.
- ofApp = fenêtre sortie (crée toutes les ressources GL) ; GuiApp = fenêtre contrôle (ImGui).

## Règles à ne pas casser
- Couleurs : aucune couleur hors palette, aucun mix() entre couleurs, palettes validées (S >= 0.55, B >= 0.70).
  Les mix() dans les shaders de scène portent sur des VALEURS de champ, jamais sur des couleurs.
- Contextes GL : textures/shaders partagés entre fenêtres, PAS les FBO. Toute ressource GL est créée et
  utilisée côté sortie ; la GUI pose des requêtes (flags traités dans update()) et lit des textures.
- Vsync ON sur la sortie seulement, OFF sur la GUI (sinon 30 FPS) ; jamais d'ofSetFrameRate(n > 0).
- Rien de coûteux dans Scene::enter()/leave() (pas d'allocation, pas de compilation).
- Uniforms : chaque setUniform du C++ doit exister dans le shader (échec silencieux sinon).
- sampler2DRect : garder `#extension GL_ARB_texture_rectangle : enable` en tête des shaders.

## Direction artistique
Aplats fluo très vibrants, formes organiques (spirales, fractales, champignons, mycélium), ni dégradé ni
contour noir. Interface : fond noir, vert phosphore, accents jaune acide / rose fluo, police terminal.
