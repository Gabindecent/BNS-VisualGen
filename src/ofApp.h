#pragma once

// =====================================================================================
//  BNS VisualGen v2 — moteur VJ génératif audio-réactif (openFrameworks 0.12.1, Linux)
//
//  Vue d'ensemble
//  --------------
//    AudioEngine     thread audio : acquisition + FFT  ->  thread principal : lissage kick/mid/high
//    PaletteManager  palettes d'aplats fluo (validées : ni noir, ni couleur terne)
//    Scene           un moteur génératif = un CHAMP scalaire (0..1), jamais de couleur
//    SceneManager    charge / préchauffe toutes les scènes au démarrage, bascule en O(1),
//                    colorise le champ en aplats (passe palette) + transition tramée
//    WebcamLayer     caméra -> luminance -> boucle de feedback ("Larsen vidéo") -> champ,
//                    combiné au champ de la scène AVANT la palette (donc aplats, lui aussi)
//    VisualEngine    état partagé par les deux fenêtres
//    ofApp           fenêtre SORTIE  : rendu seul, 60 FPS, sans bordure, aucune UI
//    GuiApp          fenêtre CONTRÔLE : tableau de bord opérateur (ofxImGui)
//
//  Règles de threads / contextes GL (à respecter partout)
//  ------------------------------------------------------
//  * openFrameworks exécute les deux fenêtres SÉQUENTIELLEMENT sur le thread principal
//    (ofMainLoop::loopOnce) : GUI et sortie ne tournent jamais en parallèle -> pas de mutex
//    entre elles. Seul le thread audio est concurrent (pont par std::atomic).
//  * Les deux fenêtres ont chacune leur contexte GL, partagé (shareContextWith). Les
//    textures et shaders sont communs aux deux contextes, PAS les FBO (objets conteneurs).
//    => toute ressource GL du moteur est créée et utilisée dans le contexte de la SORTIE ;
//       la GUI ne fait que lire des textures (aperçu) et poser des requêtes.
// =====================================================================================

#include "ofMain.h"
#include "ofxImGui.h"
#include "kiss_fftr.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace bns {

// =====================================================================================
//  AUDIO
// =====================================================================================

// Valeurs prêtes à piloter l'image : toutes normalisées 0..1 et lissées dans le temps.
struct AudioBands {
    float kick = 0.f;       // énergie 40-120 Hz (grosse caisse), attaque rapide / retombée nette
    float mid = 0.f;        // 250 Hz - 2 kHz (voix, synthés, caisse claire)
    float high = 0.f;       // 4 - 12 kHz (hi-hats, cymbales, bruit)
    float kickPulse = 0.f;  // 1.0 à chaque kick détecté, puis décroissance exponentielle
    float bpm = 0.f;        // estimation informative (0 = inconnue)
    bool kickOnset = false; // vrai pendant UNE frame quand un kick est détecté
};

// Constantes de temps en SECONDES (et non en % par frame) : le rendu est identique à 30 ou 60 FPS.
struct AudioSettings {
    float gain = 1.f;               // gain numérique avant analyse
    float kickAttack = 0.010f;      // montée du kick
    float kickRelease = 0.180f;     // retombée du kick
    float bodyAttack = 0.060f;      // montée mid/high
    float bodyRelease = 0.450f;     // retombée mid/high
    float peakMemory = 6.f;         // auto-normalisation : durée de mémoire du maximum récent
    float onsetThreshold = 1.35f;   // kick = énergie > moyenne locale * seuil
    float onsetRefractory = 0.18f;  // pas deux kicks à moins de 180 ms (évite les doubles)
};

class AudioEngine : public ofBaseSoundInput {
public:
    static constexpr int kSampleRate = 48000; // fréquence native de PulseAudio / PipeWire
    static constexpr int kBufferSize = 256;   // ~5,3 ms de latence matérielle
    static constexpr int kFftSize = 2048;     // 23,4 Hz par bin : ~4 bins dans la bande du kick
    static constexpr int kSpectrumBands = 48; // spectre log affiché dans la GUI

    AudioEngine() = default;
    ~AudioEngine() override;
    AudioEngine(const AudioEngine &) = delete;
    AudioEngine & operator=(const AudioEngine &) = delete;

    // deviceId < 0 : entrée par défaut du système (PulseAudio/PipeWire, sinon ALSA).
    bool setup(int deviceId = -1);
    void close();
    std::vector<ofSoundDevice> listInputDevices();
    int currentDeviceId() const { return deviceId_; }

    // THREAD AUDIO : FFT et énergie brute par bande. Aucune allocation, aucun verrou.
    void audioIn(ofSoundBuffer & buffer) override;

    // THREAD PRINCIPAL (une fois par frame, depuis la sortie) : normalisation, lissage, onsets.
    void update(float dt);

    const AudioBands & bands() const { return bands_; }
    void copySpectrum(std::array<float, kSpectrumBands> & out) const; // pour la GUI
    AudioSettings settings;

private:
    ofSoundStream stream_;
    int deviceId_ = -1;

    // --- Propriété exclusive du thread audio (alloués dans setup, jamais redimensionnés) ---
    kiss_fftr_cfg fft_ = nullptr;
    std::vector<float> ring_;              // fenêtre glissante des kFftSize derniers échantillons
    std::vector<float> hann_;              // fenêtre de Hann précalculée
    std::vector<float> frame_;             // tampon d'entrée FFT
    std::vector<kiss_fft_cpx> spectrum_;   // sortie FFT
    int ringPos_ = 0;

    // --- Pont audio -> principal : sans verrou (un float atomique est lock-free sur x86_64) ---
    static_assert(std::atomic<float>::is_always_lock_free, "std::atomic<float> doit être lock-free");
    std::atomic<float> rawKick_{0.f}, rawMid_{0.f}, rawHigh_{0.f};
    std::array<std::atomic<float>, kSpectrumBands> rawSpectrum_{};

    // --- Propriété exclusive du thread principal ---
    AudioBands bands_;
    std::array<float, 3> peaks_{{1e-4f, 1e-4f, 1e-4f}};
    float kickLocalAvg_ = 0.f;
    float lastOnsetTime_ = -10.f;
};

// =====================================================================================
//  PALETTES — aplats fluo uniquement
// =====================================================================================
//  Les scènes ne produisent JAMAIS de couleur : un champ 0..1 est découpé en N bandes, chaque
//  bande reçoit UNE couleur pleine de la palette (passe composite). Conséquences garanties
//  par construction, pour toutes les scènes présentes et futures :
//    - aucun dégradé (pas de mix() entre couleurs de palette, seulement des marches) ;
//    - aucun contour noir (aucune couleur sombre n'est acceptée dans une palette, et le
//      fond est lui-même une couleur de palette : le noir n'existe nulle part à l'écran).

struct Palette {
    std::string name;
    std::vector<ofFloatColor> colors; // de 2 à kMaxColors aplats, dans l'ordre des bandes
};

class PaletteManager {
public:
    static constexpr int kMaxColors = 8;

    void setupDefaults();                              // palettes intégrées (acid, toxic, ...)
    bool add(const Palette & palette);                 // refuse une palette non conforme
    static bool isVivid(const ofFloatColor & c);       // saturation >= 0.55 ET luminosité >= 0.70

    void select(int index);
    int currentIndex() const { return current_; }
    const Palette & current() const { return palettes_[current_]; }
    const std::vector<Palette> & all() const { return palettes_; }

    // Envoie u_palette[kMaxColors] (vec3), u_paletteSize, u_bands, u_bandOffset au shader.
    void apply(ofShader & shader, float bandOffset) const;

    int bands = 6;           // nombre d'aplats découpés dans le champ (>= taille de palette = répétition)
    float cycleSpeed = 0.f;  // défilement des aplats (bandes/s), par sauts nets, jamais en fondu

private:
    std::vector<Palette> palettes_;
    int current_ = 0;
};

// =====================================================================================
//  SCÈNES
// =====================================================================================

struct SceneContext {
    float time = 0.f;            // horloge de la scène (secondes, pondérée par masterSpeed)
    float dt = 0.f;              // durée de la frame
    glm::vec2 resolution{0.f};   // taille du FBO de champ (pixels)
    const AudioBands * audio = nullptr;
    float audioAmount = 1.f;     // réactivité globale (fader maître de la GUI)
};

class Scene {
public:
    explicit Scene(std::string name) : name_(std::move(name)) {}
    virtual ~Scene() = default;
    Scene(const Scene &) = delete;
    Scene & operator=(const Scene &) = delete;

    const std::string & name() const { return name_; }
    bool isReady() const { return ready_; }

    // Tout ce qui est coûteux (lecture disque, compilation GLSL, allocation FBO) : ICI
    // uniquement, une seule fois au démarrage. Retourne false si la scène est inutilisable
    // (shader absent ou en erreur) : elle sera grisée dans la GUI au lieu de planter le live.
    virtual bool load(int fieldWidth, int fieldHeight) = 0;

    // Appelées au moment de la bascule : DOIVENT rester en O(1) (ni allocation, ni compilation).
    virtual void enter() {}
    virtual void leave() {}

    // Avance l'état interne (simulations). Appelée seulement quand la scène est visible.
    virtual void update(const SceneContext & ctx) { (void)ctx; }

    // Dessine le champ scalaire (canal R, 0..1) en plein cadre. Le SceneManager a déjà bindé
    // le FBO de champ (begin()/end()) dans le contexte de la sortie : ne PAS binder d'autre
    // FBO ici. Les simulations multi-passes (ping-pong) se font dans update(), pas ici.
    virtual void renderField(const SceneContext & ctx) = 0;

    // Paramètres propres à la scène, dessinés dans la fenêtre GUI (widgets ImGui uniquement,
    // AUCUN appel GL ici : on est dans le contexte de la GUI).
    virtual void drawGui() {}

protected:
    std::string name_;
    bool ready_ = false;
};

// Scène mono-passe : un fragment shader calcule directement le champ (Fluid, Mandala, Spore).
// Uniforms fournis : u_time, u_resolution, u_kick, u_mid, u_high, u_pulse, u_speed, u_scale,
// u_complexity, u_audio.
class ShaderScene : public Scene {
public:
    ShaderScene(std::string name, std::string fragmentPath);
    bool load(int fieldWidth, int fieldHeight) override;
    void update(const SceneContext & ctx) override;
    void renderField(const SceneContext & ctx) override;
    void drawGui() override;

protected:
    std::string fragmentPath_;
    ofShader shader_;
    float localTime_ = 0.f;

    float speed_ = 1.f;       // vitesse d'évolution propre à la scène
    float scale_ = 1.f;       // zoom du motif
    float complexity_ = 0.5f; // densité (bras de spirale, octaves fractales, ramifications...)
};

// Morphogen : réaction-diffusion de Gray-Scott (motifs de Turing : taches, mycélium, corail).
// État persistant simulé par ping-pong de deux FBO à résolution réduite.
class MorphogenScene : public Scene {
public:
    MorphogenScene();
    bool load(int fieldWidth, int fieldHeight) override;
    void enter() override;
    void update(const SceneContext & ctx) override;
    void renderField(const SceneContext & ctx) override;
    void drawGui() override;

private:
    void seed();                  // réinitialise la simulation (appelé dans le contexte de sortie)

    ofShader stepShader_;         // un pas de Gray-Scott (bin/data/reaction_diffusion.frag)
    ofShader fieldShader_;        // conversion concentration V -> champ 0..1
    std::array<ofFbo, 2> sim_;    // R = U, G = V
    int src_ = 0;
    int simWidth_ = 480, simHeight_ = 270;

    float feed_ = 0.0620f;        // "fingerprints" : ne se fige jamais
    float kill_ = 0.0609f;
    float diffusionV_ = 0.5f;
    int stepsPerFrame_ = 8;
    float kickInjection_ = 0.5f;  // les kicks relancent des germes de croissance
    bool reseedOnEnter_ = false;
    bool reseedRequested_ = false; // posé par la GUI, traité dans update() (bon contexte GL)
};

// =====================================================================================
//  WEBCAM — couche de feedback
// =====================================================================================
//  La caméra n'est jamais affichée telle quelle : sa luminance devient un champ 0..1, réinjecté
//  dans une boucle ping-pong (zoom + rotation + rémanence = traînées "Larsen"), puis combiné au
//  champ de la scène dans la passe composite. Elle hérite donc des aplats de la palette.
//  Les futurs effets caméra s'insèrent dans webcam_feedback.frag (ou en passes supplémentaires
//  dans update()), sans rien changer au reste du pipeline.

enum class CamMode { Solo = 1, Mask = 2, Add = 3 }; // valeurs = u_camMode dans composite.frag

struct WebcamParams {
    bool enabled = true;
    CamMode mode = CamMode::Mask;
    float amount = 0.5f;       // Mask : décalage de bandes dans la silhouette ; Add : poids
    float threshold = 0.45f;   // seuil de silhouette (mode Mask)
    float gain = 1.2f;         // gain de luminance caméra
    bool mirror = true;        // effet miroir (le public se reconnaît)
    float feedback = 0.93f;    // rémanence de la boucle (0 = aucune traînée, 0.99 = infinie)
    float zoom = 1.012f;       // zoom par frame (à 60 FPS) de la boucle : > 1 = aspiration
    float rotation = 12.f;     // rotation de la boucle, degrés par seconde
    float kickZoom = 0.04f;    // coup de zoom supplémentaire sur chaque kick
};

class WebcamLayer {
public:
    // Contexte de SORTIE. Ne jamais échouer "fort" : sans caméra, la couche est simplement inactive.
    void setup(int fieldWidth, int fieldHeight);
    void close();

    // GUI : pose une requête (l'ouverture GStreamer se fait dans update(), bon contexte GL).
    // Ouvrir une caméra bloque ~100-500 ms : à faire entre deux morceaux, pas sur un drop.
    void requestDevice(int deviceId) { pendingDevice_ = deviceId; }

    // Contexte de SORTIE, une fois par frame : lecture caméra + une passe de feedback.
    void update(float dt, const AudioBands & audio, float audioAmount);

    bool isActive() const { return params.enabled && opened_; }
    const ofTexture & field() const { return feedback_[src_].getTexture(); }
    const std::vector<ofVideoDevice> & devices() const { return devices_; }
    int currentDeviceId() const { return deviceId_; }

    WebcamParams params;

private:
    void openDevice(int deviceId);

    ofVideoGrabber grabber_;
    std::vector<ofVideoDevice> devices_;
    int deviceId_ = -1;
    int pendingDevice_ = -1;
    bool opened_ = false;

    ofShader feedbackShader_;
    std::array<ofFbo, 2> feedback_; // R = champ caméra rémanent
    int src_ = 0;
    int width_ = 0, height_ = 0;
};

// =====================================================================================
//  SCENE MANAGER — bascule instantanée
// =====================================================================================

enum class TransitionMode { Cut, Dither };

class SceneManager {
public:
    void add(std::unique_ptr<Scene> scene);

    // Contexte de SORTIE, au démarrage (APRÈS palettes.setupDefaults()) : alloue les FBO, charge et compile toutes les scènes,
    // puis les PRÉCHAUFFE (un rendu hors écran chacune) pour que le pilote termine la
    // compilation réelle maintenant, et pas au premier affichage en plein set.
    bool setup(int outputWidth, int outputHeight, float fieldScale, const PaletteManager & palettes);

    // Appelable depuis la GUI : ne touche à AUCUNE ressource GL, pose seulement une requête.
    void requestScene(int index);
    void requestNext();

    // Contexte de SORTIE, début de frame : applique la requête (O(1)) et fait avancer la
    // transition puis la scène active (et la précédente tant que la transition dure).
    void update(float dt, const SceneContext & ctx);

    // Contexte de SORTIE : champ(s) (+ champ caméra) -> palette d'aplats -> output().
    void render(const SceneContext & ctx, const PaletteManager & palettes, float bandOffset,
                const WebcamLayer * webcam);

    // Image finale. Texture GL_TEXTURE_2D (et non RECTANGLE) : partageable avec la fenêtre GUI
    // et directement affichable par ImGui::Image() pour l'aperçu.
    const ofFbo & output() const { return output_; }

    int activeIndex() const { return active_; }
    int sceneCount() const { return static_cast<int>(scenes_.size()); }
    Scene & scene(int index) { return *scenes_[index]; }
    bool isTransitioning() const { return previous_ >= 0; }
    glm::vec2 fieldSize() const { return { fieldWidth_, fieldHeight_ }; }
    float lastSwitchCostMs() const { return lastSwitchCostMs_; } // instrumentation live

    TransitionMode transition = TransitionMode::Dither;
    float transitionDuration = 0.6f; // secondes
    int ditherBlockSize = 8;         // taille des blocs de la trame (pixels de sortie)

private:
    void warmup(int index, const SceneContext & ctx, const PaletteManager & palettes);
    void applyPendingSwitch();

    std::vector<std::unique_ptr<Scene>> scenes_;
    int active_ = 0;
    int previous_ = -1;           // scène sortante pendant une transition, -1 sinon
    int pending_ = -1;            // requête de la GUI, -1 = aucune
    float transitionT_ = 1.f;     // 0 -> 1
    float lastSwitchCostMs_ = 0.f;

    int fieldWidth_ = 0, fieldHeight_ = 0;
    int outputWidth_ = 0, outputHeight_ = 0;
    ofFbo fieldActive_;           // champ de la scène active (RGBA16F, filtrage linéaire)
    ofFbo fieldPrevious_;         // champ de la scène sortante (transition)
    ofFbo output_;                // image finale en aplats (GL_TEXTURE_2D)
    ofShader compositeShader_;    // bin/data/composite.frag : quantification palette + trame
};

// =====================================================================================
//  ÉTAT PARTAGÉ PAR LES DEUX FENÊTRES
// =====================================================================================

struct LiveParams {
    float masterSpeed = 1.f;     // multiplie l'horloge de toutes les scènes
    float audioAmount = 1.f;     // réactivité audio globale
    bool freeze = false;         // fige l'image (le temps des scènes s'arrête)
};

struct VisualEngine {
    AudioEngine audio;
    PaletteManager palettes;
    SceneManager scenes;
    WebcamLayer webcam;
    LiveParams live;
    bool paletteStepOnKick = true; // chaque kick décale les aplats d'une bande (saut net)

    int renderWidth = 1920;      // résolution interne de rendu, indépendante de la fenêtre
    int renderHeight = 1080;
    float fieldScale = 0.5f;     // champs calculés à 50 % puis quantifiés à 100 % (voir .cpp)

    float sceneTime = 0.f;
    float bandPhase = 0.f;       // décalage courant des aplats (0..1)
    float outputFps = 0.f;       // mesuré dans la fenêtre de sortie, affiché par la GUI
    float outputFrameMs = 0.f;
    std::shared_ptr<ofAppBaseWindow> outputWindow;
};

} // namespace bns

// =====================================================================================
//  FENÊTRE SORTIE : rendu seul
// =====================================================================================
class ofApp : public ofBaseApp {
public:
    explicit ofApp(std::shared_ptr<bns::VisualEngine> engine) : engine_(std::move(engine)) {}

    void setup() override;       // vsync ON, crée toutes les ressources GL du moteur
    void update() override;      // audio.update(dt) puis scenes.update() : UNE fois par frame
    void draw() override;        // scenes.render() puis output() étiré plein cadre
    void exit() override;
    void keyPressed(int key) override; // secours si la GUI est perdue : F plein écran, 1-9 scènes

private:
    std::shared_ptr<bns::VisualEngine> engine_;
    bns::SceneContext ctx_;      // construit dans update(), consommé par draw()
};

// =====================================================================================
//  FENÊTRE CONTRÔLE : tableau de bord opérateur
// =====================================================================================
class GuiApp : public ofBaseApp {
public:
    explicit GuiApp(std::shared_ptr<bns::VisualEngine> engine) : engine_(std::move(engine)) {}

    void setup() override;       // vsync OFF (sinon 2 attentes d'écran par boucle -> 30 FPS)
    void draw() override;
    void exit() override;
    void keyPressed(int key) override; // raccourcis live : 1-9 scènes, espace = palette suivante

private:
    void drawSceneSwitcher();
    void drawPalettePanel();
    void drawAudioPanel();
    void drawPreview();
    void drawStatusBar();

    void drawWebcamPanel();
    void drawMasterPanel();
    void applyTheme();

    std::shared_ptr<bns::VisualEngine> engine_;
    ofxImGui::Gui gui_;
    std::array<float, bns::AudioEngine::kSpectrumBands> spectrum_{};
    // Liste des entrées audio mise en cache : interroger RtAudio à chaque frame coûte des ms
    std::vector<ofSoundDevice> audioDevices_;
};
