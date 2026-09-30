#pragma once

#include "ofMain.h"
#include "kiss_fftr.h"
#include <mutex>
#include <vector>
#include <functional>

enum class DashTab { AUDIO, VIDEO, GENERATIF, TURING };

// Mini kit d'UI custom : chaque widget se dessine ET s'enregistre pour le hit-test en un seul appel,
// pas de dépendance à ofxGui (plus de police cassée, plus de boîtes imbriquées, plus de taille figée).
struct UISlider {
	float * value;
	float minV, maxV;
	bool isInt;
	ofRectangle rect;
};
struct UIToggle {
	bool * value;
	ofRectangle rect;
};
struct UIButton {
	ofRectangle rect;
	std::function<void()> onClick;
};
struct UIColorWheel {
	float * hue;
	float * sat;
	ofPoint center;
	float radius;
};

class ofApp : public ofBaseApp{

	public:
		void setup() override;
		void update() override;
		void draw() override;
		void exit() override;
		void audioIn(ofSoundBuffer & buffer) override;

		// Souris : seule la fenêtre Dashboard est interactive (voir main.cpp pour le branchement)
		void mouseDragged(int x, int y, int button) override;
		void mousePressed(int x, int y, int button) override;
		void mouseReleased(int x, int y, int button) override;
		void mouseScrolled(int x, int y, float scrollX, float scrollY) override;

		// Dashboard custom : dessiné à la main, layout recalculé chaque frame depuis la taille
		// réelle de la fenêtre -> un vrai espace dynamique (redimensionner la fenêtre reflow tout).
		void drawDashboard(ofEventArgs & args);
		ofAppBaseWindow * dashboardWindowPtr = nullptr;

		void uiText(const std::string & text, float x, float y, ofColor color, float scale = 1.5f);
		float uiTextWidth(const std::string & text, float scale);
		// En-tête de section cliquable : replie/déplie son contenu (menu déroulant), retourne l'état après clic
		bool uiCollapsibleHeader(const std::string & text, float x, float y, float w, ofColor accent, bool & expanded);
		void uiToggleCard(const std::string & label, ofRectangle rect, bool & value, ofColor accent);
		void uiTabButton(const std::string & label, ofRectangle rect, bool active, ofColor accent, std::function<void()> onClick);
		void uiButton(const std::string & label, ofRectangle rect, ofColor accent, std::function<void()> onClick);
		float uiSliderRow(const std::string & label, float x, float y, float w, float & value, float minV, float maxV, bool isInt = false);
		void drawSliderThumb(float x, float y, float size, ofColor color);
		float uiColorEditor(const std::string & label, float x, float y, float w, float & h, float & s, float & b);
		void applySliderDrag(int index, float mouseX);
		void applyWheelDrag(int index, float mouseX, float mouseY);
		void generateColorWheelImage(int size);

		// --- Choix des périphériques (caméra / entrée audio) : listés une fois au démarrage,
		// changement à chaud sans relancer l'appli.
		std::vector<ofVideoDevice> videoDevices;
		int selectedVideoDeviceId = -1;
		void applyVideoDevice(int deviceId);
		std::vector<ofSoundDevice> audioInputDevices;
		int selectedAudioDeviceId = -1;
		void applyAudioDevice(int deviceId);

		// --- Habillage "collage papier découpé" : appliqué aux chips/boutons/cadres du Dashboard
		// uniquement, la fenêtre Sortie garde ses shaders inchangés.
		float collageAngle(float x, float y);          // rotation déterministe et stable (pas de flicker)
		void drawCollageChip(ofRectangle rect, ofColor fillColor, ofColor borderColor, bool filled = true);
		void drawStarShape(float x, float y, float outerR, float innerR, int points, float rotationDeg, ofColor color);
		ofTrueTypeFont dashFont; // Police du dashboard (au lieu de la police bitmap par défaut, trop grossière)

		std::vector<UISlider> liveSliders;
		std::vector<UIToggle> liveToggles;
		std::vector<UIButton> liveButtons;
		std::vector<UIColorWheel> liveWheels;
		int draggingSlider = -1;
		int draggingWheel = -1;
		// Cercle colorimétrique (teinte = angle, saturation = distance au centre) partagé par tous
		// les sélecteurs de couleur du dashboard ; généré une seule fois (luminosité fixée à 255,
		// réglée séparément par le slider Luminosite).
		ofImage colorWheelImg;
		bool colorWheelReady = false;

		// Scroll du dashboard : le contenu peut dépasser la hauteur de la fenêtre (surtout onglet
		// replié/déplié) -> molette pour défiler. Les rects de hit-test restent en coordonnées de
		// contenu (non transformées) ; on ajoute juste le scroll aux coordonnées souris avant de tester.
		float dashScrollY = 0.0f;
		float dashContentHeight = 0.0f;
		DashTab activeTab = DashTab::VIDEO;
		bool presetsExpanded = true; // Menu déroulant : la grille de presets se replie/déplie au clic

		// Presets (JSON), et création de motifs. presetFloats()/presetBools() listent UNE seule
		// fois tous les paramètres sauvegardés (clé JSON -> variable) : save et load partagent la
		// même table, impossible d'oublier un paramètre dans l'un des deux.
		void savePreset(int slot);
		void loadPreset(int slot);
		std::vector<std::pair<const char *, float *>> presetFloats();
		std::vector<std::pair<const char *, bool *>> presetBools();
		void randomizeGenerativePattern();

		// Mode 2 : moiré / vortex / anneaux, réactif à l'audio
		void drawGenerativeVisuals();

		// Mode 3 : réaction-diffusion de Turing (Gray-Scott) : motifs organiques (taches, corail,
		// rayures) simulés sur GPU par ping-pong de FBO, puis colorisés avec la palette de l'onglet
		void seedTuringPattern();
		void stepTuringSimulation();
		void drawTuringVisuals();

		// --- Etat de l'application (piloté directement par le dashboard custom ci-dessus) ---
		bool showVideoTab = true;
		bool showGenerativeTab = true;
		bool showTuringTab = false;
		// Le bouton "Reensemencer" est cliqué depuis la fenêtre Dashboard, qui a son PROPRE
		// contexte OpenGL. Les FBO ne sont pas des objets partagés entre contextes (contrairement
		// aux textures) : appeler rdBufferA/B.begin() directement depuis ce clic échoue en silence.
		// On se contente donc de poser un drapeau ici, et on fait le vrai travail dans draw()
		// (fenêtre Sortie), qui s'exécute dans le bon contexte.
		bool turingReseedRequested = false;

		bool audioReactive = false;
		float audioGain = 3.0f;
		float audioColorAmount = 1.0f; // Basses -> flash de luminosité caméra
		float audioZoomAmount = 1.0f;  // Basses -> coup de zoom (kick)
		float audioSpeedAmount = 1.0f; // Aigus/médiums -> vitesse de dérive des couleurs
		// true : le zoom "tape" uniquement sur un temps détecté (kick net, silence entre les
		// temps) ; false : il suit bassKick en continu (plus organique, moins "calé sur la grille")
		bool audioBeatSync = false;

		// --- Analyse audio : fenêtre d'analyse plus grande que le buffer matériel (accumulée
		// dans un anneau), pour une estimation des basses beaucoup moins bruitée. Voir audioIn().
		static const int audioFftSize = 2048;
		std::vector<float> audioRingBuffer;
		int audioRingWritePos = 0;

		// Enveloppes lissées avec de vraies constantes de temps (secondes), indépendantes du
		// framerate -> fluide à 20 FPS comme à 60. bassKick est une pulsation séparée (attaque
		// quasi instantanée, retombée nette) pour les coups percussifs (zoom/flash), distincte de
		// bassSmooth/midSmooth/trebleSmooth qui restent un niveau d'énergie lissé sur la durée.
		float bassPeak = 0.001f, midPeak = 0.001f, treblePeak = 0.001f; // auto-normalisation (suivent le niveau récent)
		float bassKick = 0.0f;

		// --- Détection de temps (onset) sur les basses : un "temps" est détecté quand le niveau
		// dépasse nettement sa propre moyenne locale récente (détection par flux d'énergie, la
		// technique la plus simple et la plus robuste pour un kick de grosse caisse techno/dub).
		// beatPulse est le déclenchement net (1.0 puis retombée) ; bpmEstimate est une estimation
		// de tempo lissée à partir de l'écart entre les derniers temps détectés (purement informatif,
		// affiché dans le dashboard pour vérifier que la détection "tient" le morceau).
		float bassLocalAvg = 0.0f;
		float lastOnsetTime = -10.0f;
		float beatPulse = 0.0f;
		float bpmEstimate = 0.0f;

		// Onglet Video/Webcam : couleurs en HSB (teinte/saturation/luminosité séparées),
		// reconstruites à neuf chaque frame -> plus aucun risque de dérive vers le blanc/gris.
		// ATTENTION : toutes les teintes sont sur l'échelle d'openFrameworks, 0..255 (PAS 0..360°).
		// Une teinte > ~306 donne du BLANC dans ofColor::fromHsb -> toujours passer par hsb().
		float mapLightHue = 234, mapLightSat = 255, mapLightBright = 255;
		float mapDarkHue = 100, mapDarkSat = 220, mapDarkBright = 90;
		bool mapAutoMode = false;
		float mapColorSpeed = 1.0f;
		float mapLightTargetHue = 234; // Dérive = saute vers cette cible aléatoire, puis transitionne
		float mapLightHueTimer = 0.0f;
		// La couleur sombre dérive aussi, sur son propre timer (indépendant du clair) : cible tirée
		// autour de la complémentaire de la couleur claire (garde le contraste), mais pas figée
		// dessus -> les deux couleurs bougent vraiment, pas juste une qui suit l'autre.
		float mapDarkTargetHue = 100;
		float mapDarkHueTimer = 0.0f;

		float zoomLevel = 1.03f;
		float rotationAngle = 0.8f;
		float feedbackOpacity = 248;
		float camOpacity = 60;
		float threshold = 0.45f;
		float targetFPS = 20;
		int appliedFPS = -1; // dernier framerate réellement appliqué (évite ofSetFrameRate à chaque frame)

		// Onglet Mode Génératif
		float genLightHue = 45, genLightSat = 255, genLightBright = 255;
		float genMidHue = 191, genMidSat = 200, genMidBright = 230;
		float genDarkHue = 198, genDarkSat = 200, genDarkBright = 70;
		bool autoMode = true;
		float colorSpeed = 0.6f;
		float genLightTargetHue = 45;
		float genLightHueTimer = 0.0f;
		// La couleur accent dérive aussi, sur son propre timer -> les deux couleurs qui comptent
		// le plus visuellement bougent en permanence, pas juste la claire.
		float genMidTargetHue = 191;
		float genMidHueTimer = 0.0f;

		// Mode Génératif "nébuleuse" : nappes de bruit fractal qui dérivent lentement (domain
		// warping), volontairement sans forme géométrique nette -> effet planant plutôt que
		// moiré/anneaux nerveux (ancienne version).
		float evolutionSpeed = 0.18f;   // Vitesse globale de dérive temporelle (lent par défaut)
		float nebulaScale = 1.4f;       // Échelle du bruit (zoom) -> taille des volutes
		float nebulaWarp = 3.0f;        // Intensité de la déformation de domaine -> à quel point ça "coule"
		float nebulaContrast = 1.6f;    // Netteté des transitions de couleur (doux <-> contrasté)

		// Onglet Turing (réaction-diffusion) : ses propres couleurs + les paramètres du modèle
		float turingLightHue = 130, turingLightSat = 220, turingLightBright = 255;
		float turingMidHue = 190, turingMidSat = 200, turingMidBright = 220;
		float turingDarkHue = 250, turingDarkSat = 180, turingDarkBright = 40;
		// "Fingerprints" : un des rares couples Feed/Kill du modèle de Gray-Scott qui ne se fige
		// JAMAIS, même en plein écran ou en laissant tourner des heures (contrairement à "worms",
		// utilisé avant, qui finit par se caler dans des poches figées une fois l'espace occupé).
		// Le motif change perpétuellement de dessin (empreintes, labyrinthe, vermicelles) sans
		// jamais se répéter à l'identique -> voir aussi turingDriftAmount plus bas.
		float turingFeed = 0.0620f;  // Taux d'apport du produit U ("Feed") -> type de motif
		float turingKill = 0.0609f;  // Taux de disparition du produit V ("Kill") -> type de motif
		float turingDiffV = 0.5f;    // Diffusion du produit V (relative à U, fixée à 1.0) : ratio standard Gray-Scott
		float turingSteps = 10;      // Nombre de pas de simulation par frame (vitesse d'évolution)

		// Dérive automatique de Feed/Kill (en plus des nucléations spontanées côté shader) : balade
		// lentement le point de fonctionnement autour du réglage choisi. Amplitude volontairement
		// resserrée pour rester dans la poche de la carte Gray-Scott qui reste "vivante" en
		// permanence (au-delà, on retombe vite sur des zones qui se figent ou s'éteignent).
		bool turingAutoDrift = true;
		float turingDriftSpeed = 0.07f;
		float turingDriftAmount = 0.006f;
		float turingDriftPhase = 0.0f;

		// Masquage du motif de Turing par la luminosité de l'image en dessous (webcam + génératif) :
		// 0 = plein écran, 1 = zones claires uniquement, 2 = zones sombres uniquement.
		int turingMaskMode = 0;
		float turingMaskThreshold = 0.5f;

		ofFbo rdBufferA, rdBufferB; // Ping-pong de la simulation (R=U, G=V), résolution réduite
		bool rdPingPong = false;
		int rdWidth = 480, rdHeight = 270;
		ofShader rdShader;          // Un pas de Gray-Scott
		ofShader turingDisplayShader; // Colorisation du résultat
		ofFbo turingMaskSrc;        // Copie de l'image composée juste avant le calque Turing (pour le masque)

		// Rendu (inchangé)
		ofVideoGrabber vidGrabber;
		ofFbo fbo;
		ofFbo lastFbo;
		ofShader shader;
		ofShader genShader;

		ofSoundStream soundStream;
		kiss_fftr_cfg fftCfg = nullptr;
		int audioBufferSize = 512;
		// Buffers de travail de la FFT, alloués une fois dans setup() : aucune allocation mémoire
		// dans audioIn() (thread audio temps réel -> une allocation peut provoquer des craquements).
		std::vector<float> fftWindow;   // fenêtre de Hann précalculée
		std::vector<float> fftSamples;
		std::vector<kiss_fft_cpx> fftSpectrum;

		std::mutex audioMutex;
		float bassRaw = 0, midRaw = 0, trebleRaw = 0;
		float bassSmooth = 0, midSmooth = 0, trebleSmooth = 0;
};
