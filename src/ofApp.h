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

class ofApp : public ofBaseApp{

	public:
		void setup() override;
		void update() override;
		void draw() override;
		void exit() override;
		void audioIn(ofSoundBuffer & buffer) override;

		void keyPressed(int key) override;
		void keyReleased(int key) override;
		void mouseMoved(int x, int y ) override;
		void mouseDragged(int x, int y, int button) override;
		void mousePressed(int x, int y, int button) override;
		void mouseReleased(int x, int y, int button) override;
		void mouseScrolled(int x, int y, float scrollX, float scrollY) override;
		void mouseEntered(int x, int y) override;
		void mouseExited(int x, int y) override;
		void windowResized(int w, int h) override;
		void dragEvent(ofDragInfo dragInfo) override;
		void gotMessage(ofMessage msg) override;

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
		float uiColorEditor(const std::string & label, float x, float y, float w, float & h, float & s, float & b);
		void applySliderDrag(int index, float mouseX);

		std::vector<UISlider> liveSliders;
		std::vector<UIToggle> liveToggles;
		std::vector<UIButton> liveButtons;
		int draggingSlider = -1;

		// Scroll du dashboard : le contenu peut dépasser la hauteur de la fenêtre (surtout onglet
		// replié/déplié) -> molette pour défiler. Les rects de hit-test restent en coordonnées de
		// contenu (non transformées) ; on ajoute juste le scroll aux coordonnées souris avant de tester.
		float dashScrollY = 0.0f;
		float dashContentHeight = 0.0f;
		DashTab activeTab = DashTab::VIDEO;
		bool presetsExpanded = true; // Menu déroulant : la grille de presets se replie/déplie au clic

		// Presets (JSON), et création de motifs
		void savePreset(int slot);
		void loadPreset(int slot);
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
		float audioSpeedAmount = 1.0f; // Aigus -> vitesse de dérive couleur

		// Onglet Video/Webcam : couleurs en HSB (teinte/saturation/luminosité séparées),
		// reconstruites à neuf chaque frame -> plus aucun risque de dérive vers le blanc/gris.
		float mapLightHue = 330, mapLightSat = 255, mapLightBright = 255;
		float mapDarkHue = 100, mapDarkSat = 220, mapDarkBright = 90;
		bool mapAutoMode = false;
		float mapColorSpeed = 1.0f;
		float mapLightTargetHue = 330; // Dérive = saute vers cette cible aléatoire, puis transitionne
		float mapLightHueTimer = 0.0f;

		float zoomLevel = 1.03f;
		float rotationAngle = 0.8f;
		float feedbackOpacity = 248;
		float camOpacity = 60;
		float threshold = 0.45f;
		float targetFPS = 20;

		// Onglet Mode Génératif
		float genLightHue = 45, genLightSat = 255, genLightBright = 255;
		float genMidHue = 270, genMidSat = 200, genMidBright = 230;
		float genDarkHue = 280, genDarkSat = 200, genDarkBright = 70;
		bool autoMode = true;
		float colorSpeed = 0.6f;
		float genLightTargetHue = 45;
		float genLightHueTimer = 0.0f;

		float evolutionSpeed = 1.2f;
		float lineDensity = 45.0f;
		float warpAmount = 1.5f;
		float swirlAmount = 2.0f;
		float ringAmount = 0.35f;

		// Onglet Turing (réaction-diffusion) : ses propres couleurs + les paramètres du modèle
		float turingLightHue = 130, turingLightSat = 220, turingLightBright = 255;
		float turingMidHue = 190, turingMidSat = 200, turingMidBright = 220;
		float turingDarkHue = 250, turingDarkSat = 180, turingDarkBright = 40;
		float turingFeed = 0.037f;   // Taux d'apport du produit U ("Feed") -> type de motif
		float turingKill = 0.060f;   // Taux de disparition du produit V ("Kill") -> type de motif
		float turingDiffV = 0.5f;    // Diffusion du produit V (relative à U, fixée à 1.0)
		float turingSteps = 6;       // Nombre de pas de simulation par frame (vitesse d'évolution)

		ofFbo rdBufferA, rdBufferB; // Ping-pong de la simulation (R=U, G=V), résolution réduite
		bool rdPingPong = false;
		int rdWidth = 480, rdHeight = 270;
		ofShader rdShader;          // Un pas de Gray-Scott
		ofShader turingDisplayShader; // Colorisation du résultat

		// Rendu (inchangé)
		ofVideoGrabber vidGrabber;
		ofFbo fbo;
		ofFbo lastFbo;
		ofShader shader;
		ofShader genShader;

		ofSoundStream soundStream;
		kiss_fftr_cfg fftCfg = nullptr;
		int audioBufferSize = 512;

		std::mutex audioMutex;
		float bassRaw = 0, midRaw = 0, trebleRaw = 0;
		float bassSmooth = 0, midSmooth = 0, trebleSmooth = 0;
};
