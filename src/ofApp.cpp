#include "ofApp.h"

//--------------------------------------------------------------
void ofApp::setup(){
    ofSetBackgroundColor(0, 0, 0); // Esthétique sombre tekno

    // Chargement du Shader de Bichromie (il va chercher shader.vert et shader.frag dans bin/data)
    shader.load("shader");
    // Shader procédural du Mode Génératif (moiré / vortex / anneaux), réutilise le même vertex shader
    genShader.load("shader.vert", "generative.frag");
    // Mode 3 : réaction-diffusion de Turing (simulation + colorisation)
    rdShader.load("shader.vert", "reaction_diffusion.frag");
    turingDisplayShader.load("shader.vert", "turing_display.frag");

    // Initialisation de la caméra (640x480 pour des performances optimales)
    vidGrabber.setDesiredFrameRate(30);
    vidGrabber.initGrabber(640, 480);

    // Allocation des FBO à la résolution finale
    fbo.allocate(1920, 1080, GL_RGBA);
    lastFbo.allocate(1920, 1080, GL_RGBA);

    fbo.begin();
    ofClear(0, 0, 0, 255);
    fbo.end();

    lastFbo.begin();
    ofClear(0, 0, 0, 255);
    lastFbo.end();

    ofDirectory::createDirectory("presets", true, true);

    // Simulation de Turing : 2 buffers en ping-pong, à résolution réduite (perf) avec filtrage
    // linéaire pour un agrandissement doux vers la résolution finale.
    ofFboSettings rdSettings;
    rdSettings.width = rdWidth;
    rdSettings.height = rdHeight;
    rdSettings.internalformat = GL_RGBA32F;
    rdSettings.textureTarget = GL_TEXTURE_RECTANGLE_ARB;
    rdBufferA.allocate(rdSettings);
    rdBufferB.allocate(rdSettings);
    rdBufferA.getTexture().setTextureMinMagFilter(GL_LINEAR, GL_LINEAR);
    rdBufferB.getTexture().setTextureMinMagFilter(GL_LINEAR, GL_LINEAR);
    seedTuringPattern();

    // Points de départ des dérives auto : évite un saut au premier activage
    mapLightTargetHue = mapLightHue;
    genLightTargetHue = genLightHue;

    // Entrée audio : capte le retour de la table de mix branché sur le jack de la carte son.
    // FFT réelle (kiss_fftr, déjà linkée) allouée une fois pour la taille de buffer fixe.
    fftCfg = kiss_fftr_alloc(audioBufferSize, 0, nullptr, nullptr);

    ofSoundStreamSettings soundSettings;
    soundSettings.setInListener(this);
    soundSettings.sampleRate = 44100;
    soundSettings.numInputChannels = 2;
    soundSettings.numOutputChannels = 0;
    soundSettings.bufferSize = audioBufferSize;
    soundStream.setup(soundSettings);
}

//--------------------------------------------------------------
void ofApp::update(){
    ofSetFrameRate((int)targetFPS);
    vidGrabber.update();

    float dt = ofGetLastFrameTime();
    if (dt <= 0.0f) dt = 1.0f / 60.0f;

    // Dérive Video/Webcam : PAS une rotation continue autour du cercle chromatique.
    // On saute vers une nouvelle teinte cible tirée au hasard, on transitionne en douceur
    // vers elle, on attend, puis on retire une nouvelle cible -> "passer d'une couleur à
    // une autre" plutôt que "tourner en rond".
    if (mapAutoMode) {
        mapLightHueTimer -= dt;
        if (mapLightHueTimer <= 0.0f) {
            mapLightTargetHue = ofRandom(0.0f, 255.0f);
            mapLightHueTimer = ofRandom(3.0f, 7.0f);
        }
        float diff = mapLightTargetHue - mapLightHue;
        if (diff > 127.5f) diff -= 255.0f;
        if (diff < -127.5f) diff += 255.0f;
        mapLightHue += diff * ofClamp(mapColorSpeed * dt, 0.0f, 1.0f);
        if (mapLightHue >= 255.0f) mapLightHue -= 255.0f;
        if (mapLightHue < 0.0f) mapLightHue += 255.0f;

        // La couleur sombre reste toujours (à peu près) la complémentaire de la claire
        float darkHue = mapLightHue + 128.0f;
        darkHue += (ofNoise(ofGetElapsedTimef() * 0.1f + 100.0f) - 0.5f) * 40.0f;
        if (darkHue > 255.0f) darkHue -= 255.0f;
        if (darkHue < 0.0f) darkHue += 255.0f;
        mapDarkHue = darkHue;
    }

    // Dérive Mode Génératif : même principe de saut + transition
    if (autoMode) {
        genLightHueTimer -= dt;
        if (genLightHueTimer <= 0.0f) {
            genLightTargetHue = ofRandom(0.0f, 255.0f);
            genLightHueTimer = ofRandom(3.0f, 7.0f);
        }
        float diff = genLightTargetHue - genLightHue;
        if (diff > 127.5f) diff -= 255.0f;
        if (diff < -127.5f) diff += 255.0f;
        genLightHue += diff * ofClamp(colorSpeed * dt, 0.0f, 1.0f);
        if (genLightHue >= 255.0f) genLightHue -= 255.0f;
        if (genLightHue < 0.0f) genLightHue += 255.0f;
    }

    // Simulation de Turing : seulement si la couche est active (coûteux pour rien sinon)
    if (showTuringTab) {
        stepTuringSimulation();
    }

    // Lissage (attack rapide / decay doux) des bandes audio calculées sur le thread audio
    if (audioReactive) {
        float bass, mid, treble;
        {
            std::lock_guard<std::mutex> lock(audioMutex);
            bass = bassRaw; mid = midRaw; treble = trebleRaw;
        }
        bassSmooth += (bass - bassSmooth) * (bass > bassSmooth ? 0.6f : 0.1f);
        midSmooth += (mid - midSmooth) * (mid > midSmooth ? 0.6f : 0.1f);
        trebleSmooth += (treble - trebleSmooth) * (treble > trebleSmooth ? 0.6f : 0.1f);
    }
}

//--------------------------------------------------------------
void ofApp::audioIn(ofSoundBuffer & buffer){
    int nCh = buffer.getNumChannels();
    int n = buffer.getNumFrames();
    if (n != audioBufferSize || !fftCfg) return;

    // Downmix vers mono + gain, fenêtrage de Hann pour limiter les fuites spectrales
    std::vector<float> samples(n);
    for (int i = 0; i < n; i++) {
        float s = 0;
        for (int c = 0; c < nCh; c++) s += buffer.getSample(i, c);
        float window = 0.5f * (1.0f - cosf(TWO_PI * i / (n - 1)));
        samples[i] = (s / nCh) * audioGain * window;
    }

    std::vector<kiss_fft_cpx> spectrum(n / 2 + 1);
    kiss_fftr(fftCfg, samples.data(), spectrum.data());

    float sampleRate = buffer.getSampleRate() > 0 ? buffer.getSampleRate() : 44100.0f;
    float binHz = sampleRate / n;
    float bass = 0, mid = 0, treble = 0;
    int bassN = 0, midN = 0, trebleN = 0;

    for (int i = 1; i < (int)spectrum.size(); i++) {
        float freq = i * binHz;
        float mag = sqrtf(spectrum[i].r * spectrum[i].r + spectrum[i].i * spectrum[i].i);
        if (freq < 250.0f) { bass += mag; bassN++; }
        else if (freq < 4000.0f) { mid += mag; midN++; }
        else if (freq < 16000.0f) { treble += mag; trebleN++; }
    }
    if (bassN) bass /= bassN;
    if (midN) mid /= midN;
    if (trebleN) treble /= trebleN;

    std::lock_guard<std::mutex> lock(audioMutex);
    bassRaw = bass; midRaw = mid; trebleRaw = treble;
}

//--------------------------------------------------------------
void ofApp::draw(){
    // Traité ici (fenêtre Sortie = bon contexte OpenGL), pas dans le clic du dashboard : voir
    // le commentaire sur turingReseedRequested dans ofApp.h.
    if (turingReseedRequested) {
        seedTuringPattern();
        turingReseedRequested = false;
    }

    // Réactivité audio : les basses font flasher/zoomer la vidéo (s'applique aux deux onglets)
    float currentZoom = zoomLevel;
    int currentCamOpacity = (int)camOpacity;
    if (audioReactive) {
        currentCamOpacity = ofClamp(currentCamOpacity + (int)(bassSmooth * audioColorAmount * 400.0f), 0, 255);
        currentZoom += bassSmooth * audioZoomAmount * 0.15f;
    }

    // 1. ONGLET VIDEO/WEBCAM : caméra + shader bichromie + feedback Larsen (traînées en boucle)
    fbo.begin();
    ofClear(0, 0, 0, 255);

    if (showVideoTab) {
        ofPushMatrix();
        ofTranslate(fbo.getWidth() / 2, fbo.getHeight() / 2);
        ofRotateDeg(rotationAngle);
        ofScale(currentZoom, currentZoom);
        ofSetColor(255, 255, 255, (int)feedbackOpacity);
        lastFbo.draw(-fbo.getWidth() / 2, -fbo.getHeight() / 2);
        ofPopMatrix();

        // Mélange alpha normal (PAS additif) : reste toujours borné, même après un temps infini
        ofEnableAlphaBlending();

        ofColor lc = ofColor::fromHsb(mapLightHue, mapLightSat, mapLightBright);
        ofColor dc = ofColor::fromHsb(mapDarkHue, mapDarkSat, mapDarkBright);

        shader.begin();
        shader.setUniform4f("u_color", lc.r/255.0f, lc.g/255.0f, lc.b/255.0f, currentCamOpacity/255.0f);
        shader.setUniform4f("u_darkColor", dc.r/255.0f, dc.g/255.0f, dc.b/255.0f, 1.0f);
        shader.setUniform1f("u_threshold", threshold);

        ofSetColor(255);
        vidGrabber.draw(0, 0, fbo.getWidth(), fbo.getHeight());

        shader.end();

        // Test de rendu manuel (Souris) : si la webcam est noire ou non détectée,
        // ceci permet de tester l'effet Larsen !
        ofSetColor(lc.r, lc.g, lc.b, currentCamOpacity);
        float mx = ((float)mouseX / ofGetWidth()) * fbo.getWidth();
        float my = ((float)mouseY / ofGetHeight()) * fbo.getHeight();
        ofDrawCircle(mx, my, 80);
    }
    fbo.end();

    // 2. PING-PONG : sauvegarder AVANT de dessiner le Mode Génératif (voir drawGenerativeVisuals)
    lastFbo.begin();
    ofClear(0, 0, 0, 255);
    ofSetColor(255);
    fbo.draw(0, 0);
    lastFbo.end();

    // 3. ONGLET MODE GENERATIF
    if (showGenerativeTab) {
        fbo.begin();
        drawGenerativeVisuals();
        fbo.end();
    }

    // 3bis. ONGLET TURING (réaction-diffusion) : même principe, jamais réinjecté dans le feedback
    if (showTuringTab) {
        fbo.begin();
        drawTuringVisuals();
        fbo.end();
    }

    // 4. AFFICHAGE SUR LA FENÊTRE OUTPUT
    ofSetColor(255);
    fbo.draw(0, 0, ofGetWidth(), ofGetHeight());
}

//--------------------------------------------------------------
void ofApp::drawGenerativeVisuals(){
    ofColor c1 = ofColor::fromHsb(genLightHue, genLightSat, genLightBright);
    ofColor c2 = ofColor::fromHsb(genMidHue, genMidSat, genMidBright);
    ofColor c3 = ofColor::fromHsb(genDarkHue, genDarkSat, genDarkBright);

    ofEnableBlendMode(OF_BLENDMODE_ADD);
    genShader.begin();
    genShader.setUniform1f("u_time", ofGetElapsedTimef() * evolutionSpeed);
    genShader.setUniform1f("u_aspect", fbo.getWidth() / fbo.getHeight());
    genShader.setUniform2f("u_resolution", fbo.getWidth(), fbo.getHeight());
    genShader.setUniform1f("u_lineDensity", lineDensity);
    genShader.setUniform1f("u_warpAmount", warpAmount);
    genShader.setUniform1f("u_swirlAmount", swirlAmount);
    genShader.setUniform1f("u_ringAmount", ringAmount);
    genShader.setUniform3f("u_colorLight", c1.r/255.0f, c1.g/255.0f, c1.b/255.0f);
    genShader.setUniform3f("u_colorMid", c2.r/255.0f, c2.g/255.0f, c2.b/255.0f);
    genShader.setUniform3f("u_colorDark", c3.r/255.0f, c3.g/255.0f, c3.b/255.0f);
    genShader.setUniform1f("u_bass", bassSmooth);
    genShader.setUniform1f("u_mid", midSmooth);
    genShader.setUniform1f("u_treble", trebleSmooth);

    ofSetColor(255);
    ofDrawRectangle(0, 0, fbo.getWidth(), fbo.getHeight());

    genShader.end();
    ofDisableBlendMode();
}

//--------------------------------------------------------------
void ofApp::seedTuringPattern(){
    // U=1, V=0 partout (état stable "au repos"), avec quelques taches V=1 pour amorcer la réaction.
    // Les deux buffers ping-pong sont initialisés pareil : peu importe lequel est lu en premier.
    for (ofFbo * b : { &rdBufferA, &rdBufferB }) {
        b->begin();
        ofClear(255, 0, 0, 255); // R=1 (U=1), G=0 (V=0)
        ofSetColor(255, 255, 0, 255); // R=1 (U=1), G=1 (V=1) : amorce la réaction dans ces taches
        for (int i = 0; i < 6; i++) {
            float rx = ofRandom(rdWidth * 0.25f, rdWidth * 0.75f);
            float ry = ofRandom(rdHeight * 0.25f, rdHeight * 0.75f);
            float size = ofRandom(5.0f, 12.0f);
            ofDrawRectangle(rx - size / 2, ry - size / 2, size, size);
        }
        b->end();
    }
}

//--------------------------------------------------------------
void ofApp::stepTuringSimulation(){
    int steps = std::max(1, (int)turingSteps);
    for (int i = 0; i < steps; i++) {
        ofFbo & src = rdPingPong ? rdBufferB : rdBufferA;
        ofFbo & dst = rdPingPong ? rdBufferA : rdBufferB;

        dst.begin();
        rdShader.begin();
        rdShader.setUniform1f("feed", turingFeed);
        rdShader.setUniform1f("kill", turingKill);
        rdShader.setUniform1f("du", 1.0f);
        rdShader.setUniform1f("dv", turingDiffV);
        ofSetColor(255);
        src.draw(0, 0);
        rdShader.end();
        dst.end();

        rdPingPong = !rdPingPong;
    }
}

//--------------------------------------------------------------
void ofApp::drawTuringVisuals(){
    ofColor c1 = ofColor::fromHsb(turingLightHue, turingLightSat, turingLightBright);
    ofColor c2 = ofColor::fromHsb(turingMidHue, turingMidSat, turingMidBright);
    ofColor c3 = ofColor::fromHsb(turingDarkHue, turingDarkSat, turingDarkBright);

    ofFbo & result = rdPingPong ? rdBufferB : rdBufferA;

    ofEnableBlendMode(OF_BLENDMODE_ADD);
    turingDisplayShader.begin();
    turingDisplayShader.setUniform3f("u_colorLight", c1.r/255.0f, c1.g/255.0f, c1.b/255.0f);
    turingDisplayShader.setUniform3f("u_colorMid", c2.r/255.0f, c2.g/255.0f, c2.b/255.0f);
    turingDisplayShader.setUniform3f("u_colorDark", c3.r/255.0f, c3.g/255.0f, c3.b/255.0f);
    ofSetColor(255);
    result.draw(0, 0, fbo.getWidth(), fbo.getHeight());
    turingDisplayShader.end();
    ofDisableBlendMode();
}

//======================================================================
// DASHBOARD CUSTOM : mini kit d'UI (dessin + hit-test), layout 100% dynamique
//======================================================================

//--------------------------------------------------------------
void ofApp::uiText(const std::string & text, float x, float y, ofColor color, float scale){
    ofSetColor(color);
    ofPushMatrix();
    ofTranslate(x, y);
    ofScale(scale, scale);
    ofDrawBitmapString(text, 0, 0);
    ofPopMatrix();
}

//--------------------------------------------------------------
float ofApp::uiTextWidth(const std::string & text, float scale){
    return text.size() * 8.0f * scale;
}

//--------------------------------------------------------------
bool ofApp::uiCollapsibleHeader(const std::string & text, float x, float y, float w, ofColor accent, bool & expanded){
    ofRectangle rect(x, y - 6, w, 32);
    ofSetColor(20, 20, 20);
    ofDrawRectangle(rect);
    ofNoFill();
    ofSetColor(accent);
    ofSetLineWidth(2);
    ofDrawRectangle(rect);
    ofFill();

    std::string arrow = expanded ? "[-] " : "[+] ";
    uiText(arrow + text, x + 10, y + 14, accent, 1.5f);

    liveToggles.push_back({ &expanded, rect });
    return expanded;
}

//--------------------------------------------------------------
void ofApp::uiToggleCard(const std::string & label, ofRectangle rect, bool & value, ofColor accent){
    if (value) {
        ofSetColor(accent);
        ofDrawRectangle(rect);
    } else {
        ofSetColor(20, 20, 20);
        ofDrawRectangle(rect);
        ofNoFill();
        ofSetColor(accent);
        ofSetLineWidth(3);
        ofDrawRectangle(rect);
        ofFill();
    }
    float scale = 1.6f;
    float tw = uiTextWidth(label, scale);
    ofColor textColor = value ? ofColor(0, 0, 0) : accent;
    uiText(label, rect.x + (rect.width - tw) / 2, rect.y + rect.height / 2 + 6, textColor, scale);
    liveToggles.push_back({ &value, rect });
}

//--------------------------------------------------------------
void ofApp::uiTabButton(const std::string & label, ofRectangle rect, bool active, ofColor accent, std::function<void()> onClick){
    ofSetColor(active ? accent : ofColor(18, 18, 18));
    ofDrawRectangle(rect);
    ofNoFill();
    ofSetColor(accent);
    ofSetLineWidth(2);
    ofDrawRectangle(rect);
    ofFill();
    ofColor textColor = active ? ofColor(0, 0, 0) : accent;
    float scale = 1.4f;
    float tw = uiTextWidth(label, scale);
    uiText(label, rect.x + (rect.width - tw) / 2, rect.y + rect.height / 2 + 5, textColor, scale);
    liveButtons.push_back({ rect, onClick });
}

//--------------------------------------------------------------
void ofApp::uiButton(const std::string & label, ofRectangle rect, ofColor accent, std::function<void()> onClick){
    ofSetColor(accent);
    ofDrawRectangle(rect);
    float scale = 1.25f;
    float tw = uiTextWidth(label, scale);
    uiText(label, rect.x + (rect.width - tw) / 2, rect.y + rect.height / 2 + 5, ofColor(0, 0, 0), scale);
    liveButtons.push_back({ rect, onClick });
}

//--------------------------------------------------------------
float ofApp::uiSliderRow(const std::string & label, float x, float y, float w, float & value, float minV, float maxV, bool isInt){
    float scale = 1.25f;
    uiText(label, x, y + 12, ofColor(60, 255, 130), scale);

    std::string valStr = isInt ? ofToString((int)std::round(value)) : ofToString(value, 2);
    float valW = uiTextWidth(valStr, scale);
    uiText(valStr, x + w - valW, y + 12, ofColor(235, 255, 30), scale);

    float trackY = y + 24;
    float trackH = 24;
    ofRectangle track(x, trackY, w, trackH);

    ofSetColor(28, 28, 28);
    ofDrawRectangle(track);

    float t = ofClamp((value - minV) / (maxV - minV), 0.0f, 1.0f);
    ofSetColor(235, 255, 30);
    ofDrawRectangle(x, trackY, w * t, trackH);

    ofNoFill();
    ofSetColor(60, 255, 130);
    ofSetLineWidth(2);
    ofDrawRectangle(track);
    ofFill();

    liveSliders.push_back({ &value, minV, maxV, isInt, track });
    return trackY + trackH + 18;
}

//--------------------------------------------------------------
float ofApp::uiColorEditor(const std::string & label, float x, float y, float w, float & h, float & s, float & b){
    uiText(label, x, y + 12, ofColor(255, 255, 255), 1.4f);

    ofColor preview = ofColor::fromHsb(h, s, b);
    ofRectangle swatch(x + w - 60, y - 6, 50, 32);
    ofSetColor(preview);
    ofDrawRectangle(swatch);
    ofNoFill();
    ofSetColor(255);
    ofDrawRectangle(swatch);
    ofFill();

    y += 36;
    y = uiSliderRow("  Teinte", x, y, w, h, 0.0f, 255.0f);
    y = uiSliderRow("  Saturation", x, y, w, s, 0.0f, 255.0f);
    y = uiSliderRow("  Luminosite", x, y, w, b, 0.0f, 255.0f);
    return y + 8;
}

//--------------------------------------------------------------
void ofApp::applySliderDrag(int index, float mouseX){
    if (index < 0 || index >= (int)liveSliders.size()) return;
    auto & s = liveSliders[index];
    float t = ofClamp((mouseX - s.rect.x) / s.rect.width, 0.0f, 1.0f);
    float v = s.minV + t * (s.maxV - s.minV);
    if (s.isInt) v = std::round(v);
    *(s.value) = v;
}

//--------------------------------------------------------------
void ofApp::drawDashboard(ofEventArgs & args){
    ofBackground(8, 8, 8);
    liveSliders.clear();
    liveToggles.clear();
    liveButtons.clear();

    float W = ofGetWidth();
    float pad = 24;
    float x = pad;
    float w = W - pad * 2;
    float y = 24;

    // Tout le contenu est décalé par le scroll ; les rects poussés dans liveButtons/liveToggles/
    // liveSliders restent en coordonnées de CONTENU (non affectées par cette transformation GL) :
    // le hit-test compense en ajoutant dashScrollY aux coordonnées souris (voir mousePressed).
    ofPushMatrix();
    ofTranslate(0, -dashScrollY);

    uiText("BNS VISUALGEN", x, y + 8, ofColor(60, 255, 130), 2.2f);
    y += 46;

    // Interrupteurs de mode : 3 grandes cartes pleine largeur, dynamiques (recalculées depuis W)
    float modeW = (w - 2 * 16) / 3.0f;
    uiToggleCard("VIDEO / WEBCAM", ofRectangle(x, y, modeW, 64), showVideoTab, ofColor(255, 0, 150));
    uiToggleCard("MODE GENERATIF", ofRectangle(x + modeW + 16, y, modeW, 64), showGenerativeTab, ofColor(150, 60, 220));
    uiToggleCard("TURING", ofRectangle(x + 2 * (modeW + 16), y, modeW, 64), showTuringTab, ofColor(0, 220, 170));
    y += 64 + 28;

    // PRESETS : menu déroulant (clic sur l'en-tête pour replier/déplier la grille de boutons)
    uiCollapsibleHeader("PRESETS", x, y, w, ofColor(255, 200, 40), presetsExpanded);
    y += 44;
    if (presetsExpanded) {
        struct PresetBtn { std::string label; ofColor color; std::function<void()> fn; };
        std::vector<PresetBtn> presets = {
            { "ALEATOIRE", ofColor(255, 200, 40), [this](){ randomizeGenerativePattern(); } },
            { "SAUVER A",  ofColor(90, 90, 90),   [this](){ savePreset(1); } },
            { "RAPPEL A",  ofColor(60, 255, 130), [this](){ loadPreset(1); } },
            { "SAUVER B",  ofColor(90, 90, 90),   [this](){ savePreset(2); } },
            { "RAPPEL B",  ofColor(60, 255, 130), [this](){ loadPreset(2); } },
            { "SAUVER C",  ofColor(90, 90, 90),   [this](){ savePreset(3); } },
            { "RAPPEL C",  ofColor(60, 255, 130), [this](){ loadPreset(3); } },
        };
        float btnW = 150, btnH = 48, gap = 12;
        int cols = std::max(1, (int)((w + gap) / (btnW + gap)));
        for (size_t i = 0; i < presets.size(); i++) {
            int col = i % cols;
            int row = i / cols;
            ofRectangle r(x + col * (btnW + gap), y + row * (btnH + gap), btnW, btnH);
            uiButton(presets[i].label, r, presets[i].color, presets[i].fn);
        }
        int rows = ((int)presets.size() + cols - 1) / cols;
        y += rows * (btnH + gap) + 20;
    } else {
        y += 16;
    }

    // Barre d'onglets à plat : PAS de boîte dans la boîte, un seul contenu visible à la fois
    float tabW = (w - 3 * 12) / 4.0f;
    uiTabButton("MUSIQUE", ofRectangle(x, y, tabW, 48), activeTab == DashTab::AUDIO, ofColor(60, 255, 130), [this](){ activeTab = DashTab::AUDIO; });
    uiTabButton("VIDEO", ofRectangle(x + tabW + 12, y, tabW, 48), activeTab == DashTab::VIDEO, ofColor(255, 0, 150), [this](){ activeTab = DashTab::VIDEO; });
    uiTabButton("GENERATIF", ofRectangle(x + 2 * (tabW + 12), y, tabW, 48), activeTab == DashTab::GENERATIF, ofColor(150, 60, 220), [this](){ activeTab = DashTab::GENERATIF; });
    uiTabButton("TURING", ofRectangle(x + 3 * (tabW + 12), y, tabW, 48), activeTab == DashTab::TURING, ofColor(0, 220, 170), [this](){ activeTab = DashTab::TURING; });
    y += 48 + 28;

    // Contenu de l'onglet actif : pleine largeur, à plat, directement sur le fond -> espace dynamique
    if (activeTab == DashTab::AUDIO) {
        uiToggleCard(audioReactive ? "MUSIQUE : ACTIVE" : "MUSIQUE : COUPEE", ofRectangle(x, y, w, 54), audioReactive, ofColor(60, 255, 130));
        y += 54 + 24;
        y = uiSliderRow("Sensibilite Micro/Ligne", x, y, w, audioGain, 0.5f, 10.0f);
        y = uiSliderRow("Basses -> Flash", x, y, w, audioColorAmount, 0.0f, 3.0f);
        y = uiSliderRow("Basses -> Zoom Kick", x, y, w, audioZoomAmount, 0.0f, 3.0f);
        y = uiSliderRow("Aigus -> Vitesse Derive", x, y, w, audioSpeedAmount, 0.0f, 3.0f);
    } else if (activeTab == DashTab::VIDEO) {
        y = uiColorEditor("COULEUR CLAIRE", x, y, w, mapLightHue, mapLightSat, mapLightBright);
        y = uiColorEditor("COULEUR SOMBRE", x, y, w, mapDarkHue, mapDarkSat, mapDarkBright);
        uiToggleCard(mapAutoMode ? "DERIVE AUTO : ACTIVE" : "DERIVE AUTO : COUPEE", ofRectangle(x, y, w, 54), mapAutoMode, ofColor(255, 0, 150));
        y += 54 + 24;
        y = uiSliderRow("Vitesse Derive", x, y, w, mapColorSpeed, 0.1f, 3.0f);
        y = uiSliderRow("Zoom", x, y, w, zoomLevel, 0.8f, 1.2f);
        y = uiSliderRow("Rotation", x, y, w, rotationAngle, -5.0f, 5.0f);
        y = uiSliderRow("Traine (Feedback)", x, y, w, feedbackOpacity, 0.0f, 255.0f, true);
        y = uiSliderRow("Gain Camera", x, y, w, camOpacity, 0.0f, 255.0f, true);
        y = uiSliderRow("Seuil Clair/Sombre", x, y, w, threshold, 0.0f, 1.0f);
        y = uiSliderRow("Strobe FPS", x, y, w, targetFPS, 5.0f, 60.0f, true);
    } else if (activeTab == DashTab::GENERATIF) {
        y = uiColorEditor("COULEUR CLAIRE", x, y, w, genLightHue, genLightSat, genLightBright);
        y = uiColorEditor("COULEUR ACCENT", x, y, w, genMidHue, genMidSat, genMidBright);
        y = uiColorEditor("COULEUR CREUX/NOIR", x, y, w, genDarkHue, genDarkSat, genDarkBright);
        uiToggleCard(autoMode ? "DERIVE AUTO : ACTIVE" : "DERIVE AUTO : COUPEE", ofRectangle(x, y, w, 54), autoMode, ofColor(150, 60, 220));
        y += 54 + 24;
        y = uiSliderRow("Vitesse Derive", x, y, w, colorSpeed, 0.1f, 3.0f);
        y = uiSliderRow("Vitesse Animation", x, y, w, evolutionSpeed, 0.0f, 5.0f);
        y = uiSliderRow("Densite Lignes", x, y, w, lineDensity, 5.0f, 150.0f);
        y = uiSliderRow("Ondulation", x, y, w, warpAmount, 0.0f, 5.0f);
        y = uiSliderRow("Tourbillon", x, y, w, swirlAmount, 0.0f, 6.0f);
        y = uiSliderRow("Anneaux", x, y, w, ringAmount, 0.0f, 1.0f);
    } else {
        // TURING : réaction-diffusion de Gray-Scott -> motifs organiques (taches, corail, rayures)
        y = uiColorEditor("COULEUR CLAIRE", x, y, w, turingLightHue, turingLightSat, turingLightBright);
        y = uiColorEditor("COULEUR ACCENT", x, y, w, turingMidHue, turingMidSat, turingMidBright);
        y = uiColorEditor("COULEUR CREUX/NOIR", x, y, w, turingDarkHue, turingDarkSat, turingDarkBright);
        uiButton("REENSEMENCER", ofRectangle(x, y, w, 50), ofColor(0, 220, 170), [this](){ turingReseedRequested = true; });
        y += 50 + 24;
        y = uiSliderRow("Feed (apport)", x, y, w, turingFeed, 0.01f, 0.09f);
        y = uiSliderRow("Kill (disparition)", x, y, w, turingKill, 0.03f, 0.08f);
        y = uiSliderRow("Diffusion V", x, y, w, turingDiffV, 0.2f, 1.0f);
        y = uiSliderRow("Vitesse (pas/frame)", x, y, w, turingSteps, 1.0f, 20.0f, true);
    }

    y += 20; // marge de respiration en bas de page
    dashContentHeight = y;

    ofPopMatrix();

    // Recalage du scroll si le contenu a changé (onglet différent, section repliée...) : on ne
    // veut jamais rester bloqué au-delà de ce qu'il y a réellement à afficher.
    float maxScroll = std::max(0.0f, dashContentHeight - (float)ofGetHeight());
    dashScrollY = ofClamp(dashScrollY, 0.0f, maxScroll);

    // Petite indication visuelle qu'il y a du contenu à faire défiler (barre à droite)
    if (maxScroll > 0.0f) {
        float trackH = ofGetHeight() - 20.0f;
        float thumbH = std::max(30.0f, trackH * (ofGetHeight() / dashContentHeight));
        float thumbY = 10.0f + (trackH - thumbH) * (dashScrollY / maxScroll);
        ofSetColor(60, 255, 130, 120);
        ofDrawRectangle(ofGetWidth() - 8, thumbY, 5, thumbH);
    }
}

//--------------------------------------------------------------
void ofApp::savePreset(int slot){
    ofJson j;
    j["showVideoTab"] = showVideoTab; j["showGenerativeTab"] = showGenerativeTab;
    j["audioReactive"] = audioReactive; j["audioGain"] = audioGain;
    j["audioColorAmount"] = audioColorAmount; j["audioZoomAmount"] = audioZoomAmount; j["audioSpeedAmount"] = audioSpeedAmount;
    j["mapLightHue"] = mapLightHue; j["mapLightSat"] = mapLightSat; j["mapLightBright"] = mapLightBright;
    j["mapDarkHue"] = mapDarkHue; j["mapDarkSat"] = mapDarkSat; j["mapDarkBright"] = mapDarkBright;
    j["mapAutoMode"] = mapAutoMode; j["mapColorSpeed"] = mapColorSpeed;
    j["zoomLevel"] = zoomLevel; j["rotationAngle"] = rotationAngle; j["feedbackOpacity"] = feedbackOpacity;
    j["camOpacity"] = camOpacity; j["threshold"] = threshold; j["targetFPS"] = targetFPS;
    j["genLightHue"] = genLightHue; j["genLightSat"] = genLightSat; j["genLightBright"] = genLightBright;
    j["genMidHue"] = genMidHue; j["genMidSat"] = genMidSat; j["genMidBright"] = genMidBright;
    j["genDarkHue"] = genDarkHue; j["genDarkSat"] = genDarkSat; j["genDarkBright"] = genDarkBright;
    j["autoMode"] = autoMode; j["colorSpeed"] = colorSpeed;
    j["evolutionSpeed"] = evolutionSpeed; j["lineDensity"] = lineDensity; j["warpAmount"] = warpAmount;
    j["swirlAmount"] = swirlAmount; j["ringAmount"] = ringAmount;
    j["showTuringTab"] = showTuringTab;
    j["turingLightHue"] = turingLightHue; j["turingLightSat"] = turingLightSat; j["turingLightBright"] = turingLightBright;
    j["turingMidHue"] = turingMidHue; j["turingMidSat"] = turingMidSat; j["turingMidBright"] = turingMidBright;
    j["turingDarkHue"] = turingDarkHue; j["turingDarkSat"] = turingDarkSat; j["turingDarkBright"] = turingDarkBright;
    j["turingFeed"] = turingFeed; j["turingKill"] = turingKill; j["turingDiffV"] = turingDiffV; j["turingSteps"] = turingSteps;

    ofSavePrettyJson("presets/preset_" + ofToString(slot) + ".json", j);
    ofLogNotice("BNS-VisualGen") << "Preset " << slot << " sauvegarde.";
}

//--------------------------------------------------------------
void ofApp::loadPreset(int slot){
    std::string path = "presets/preset_" + ofToString(slot) + ".json";
    if (!ofFile::doesFileExist(path)) {
        ofLogNotice("BNS-VisualGen") << "Preset " << slot << " : aucun fichier trouve.";
        return;
    }
    ofJson j = ofLoadJson(path);

    showVideoTab = j.value("showVideoTab", showVideoTab);
    showGenerativeTab = j.value("showGenerativeTab", showGenerativeTab);
    audioReactive = j.value("audioReactive", audioReactive);
    audioGain = j.value("audioGain", audioGain);
    audioColorAmount = j.value("audioColorAmount", audioColorAmount);
    audioZoomAmount = j.value("audioZoomAmount", audioZoomAmount);
    audioSpeedAmount = j.value("audioSpeedAmount", audioSpeedAmount);
    mapLightHue = j.value("mapLightHue", mapLightHue);
    mapLightSat = j.value("mapLightSat", mapLightSat);
    mapLightBright = j.value("mapLightBright", mapLightBright);
    mapDarkHue = j.value("mapDarkHue", mapDarkHue);
    mapDarkSat = j.value("mapDarkSat", mapDarkSat);
    mapDarkBright = j.value("mapDarkBright", mapDarkBright);
    mapAutoMode = j.value("mapAutoMode", mapAutoMode);
    mapColorSpeed = j.value("mapColorSpeed", mapColorSpeed);
    zoomLevel = j.value("zoomLevel", zoomLevel);
    rotationAngle = j.value("rotationAngle", rotationAngle);
    feedbackOpacity = j.value("feedbackOpacity", feedbackOpacity);
    camOpacity = j.value("camOpacity", camOpacity);
    threshold = j.value("threshold", threshold);
    targetFPS = j.value("targetFPS", targetFPS);
    genLightHue = j.value("genLightHue", genLightHue);
    genLightSat = j.value("genLightSat", genLightSat);
    genLightBright = j.value("genLightBright", genLightBright);
    genMidHue = j.value("genMidHue", genMidHue);
    genMidSat = j.value("genMidSat", genMidSat);
    genMidBright = j.value("genMidBright", genMidBright);
    genDarkHue = j.value("genDarkHue", genDarkHue);
    genDarkSat = j.value("genDarkSat", genDarkSat);
    genDarkBright = j.value("genDarkBright", genDarkBright);
    autoMode = j.value("autoMode", autoMode);
    colorSpeed = j.value("colorSpeed", colorSpeed);
    evolutionSpeed = j.value("evolutionSpeed", evolutionSpeed);
    lineDensity = j.value("lineDensity", lineDensity);
    warpAmount = j.value("warpAmount", warpAmount);
    swirlAmount = j.value("swirlAmount", swirlAmount);
    ringAmount = j.value("ringAmount", ringAmount);
    showTuringTab = j.value("showTuringTab", showTuringTab);
    turingLightHue = j.value("turingLightHue", turingLightHue);
    turingLightSat = j.value("turingLightSat", turingLightSat);
    turingLightBright = j.value("turingLightBright", turingLightBright);
    turingMidHue = j.value("turingMidHue", turingMidHue);
    turingMidSat = j.value("turingMidSat", turingMidSat);
    turingMidBright = j.value("turingMidBright", turingMidBright);
    turingDarkHue = j.value("turingDarkHue", turingDarkHue);
    turingDarkSat = j.value("turingDarkSat", turingDarkSat);
    turingDarkBright = j.value("turingDarkBright", turingDarkBright);
    turingFeed = j.value("turingFeed", turingFeed);
    turingKill = j.value("turingKill", turingKill);
    turingDiffV = j.value("turingDiffV", turingDiffV);
    turingSteps = j.value("turingSteps", turingSteps);

    mapLightTargetHue = mapLightHue;
    genLightTargetHue = genLightHue;
    ofLogNotice("BNS-VisualGen") << "Preset " << slot << " rappele.";
}

//--------------------------------------------------------------
void ofApp::randomizeGenerativePattern(){
    lineDensity = ofRandom(15.0f, 90.0f);
    warpAmount = ofRandom(0.5f, 4.0f);
    swirlAmount = ofRandom(0.5f, 5.0f);
    ringAmount = ofRandom(0.0f, 0.8f);
    evolutionSpeed = ofRandom(0.6f, 2.5f);

    genLightHue = ofRandom(0.0f, 255.0f);
    genLightTargetHue = genLightHue;
    genLightSat = 255; genLightBright = 255;

    float accentHue = genLightHue + ofRandom(60.0f, 180.0f);
    float darkHue = genLightHue + 128.0f + ofRandom(-25.0f, 25.0f);
    if (accentHue > 255) accentHue -= 255;
    if (darkHue > 255) darkHue -= 255;
    if (darkHue < 0) darkHue += 255;

    genMidHue = accentHue; genMidSat = 200; genMidBright = 230;
    genDarkHue = darkHue; genDarkSat = 200; genDarkBright = 70;
}

//--------------------------------------------------------------
void ofApp::exit(){
    soundStream.close();
    if (fftCfg) {
        kiss_fftr_free(fftCfg);
        fftCfg = nullptr;
    }
}

//--------------------------------------------------------------
void ofApp::keyPressed(int key){

}

//--------------------------------------------------------------
void ofApp::keyReleased(int key){

}

//--------------------------------------------------------------
void ofApp::mouseMoved(int x, int y ){

}

//--------------------------------------------------------------
void ofApp::mouseDragged(int x, int y, int button){
    if (ofGetWindowPtr() != dashboardWindowPtr) return;
    if (draggingSlider >= 0) applySliderDrag(draggingSlider, (float)x);
}

//--------------------------------------------------------------
void ofApp::mousePressed(int x, int y, int button){
    if (ofGetWindowPtr() != dashboardWindowPtr) return;

    // Le contenu est dessiné décalé de -dashScrollY (voir drawDashboard) mais les rects stockés
    // restent en coordonnées de contenu -> on ré-ajoute le scroll aux coordonnées souris ici.
    int cy = y + (int)dashScrollY;

    for (auto & b : liveButtons) {
        if (b.rect.inside(x, cy)) { b.onClick(); return; }
    }
    for (auto & t : liveToggles) {
        if (t.rect.inside(x, cy)) { *(t.value) = !(*(t.value)); return; }
    }
    for (size_t i = 0; i < liveSliders.size(); i++) {
        if (liveSliders[i].rect.inside(x, cy)) {
            draggingSlider = (int)i;
            applySliderDrag((int)i, (float)x);
            return;
        }
    }
}

//--------------------------------------------------------------
void ofApp::mouseReleased(int x, int y, int button){
    draggingSlider = -1;
}

//--------------------------------------------------------------
void ofApp::mouseScrolled(int x, int y, float scrollX, float scrollY){
    if (ofGetWindowPtr() != dashboardWindowPtr) return;
    float maxScroll = std::max(0.0f, dashContentHeight - (float)ofGetHeight());
    dashScrollY = ofClamp(dashScrollY - scrollY * 45.0f, 0.0f, maxScroll);
}

//--------------------------------------------------------------
void ofApp::mouseEntered(int x, int y){

}

//--------------------------------------------------------------
void ofApp::mouseExited(int x, int y){

}

//--------------------------------------------------------------
void ofApp::windowResized(int w, int h){

}

//--------------------------------------------------------------
void ofApp::gotMessage(ofMessage msg){

}

//--------------------------------------------------------------
void ofApp::dragEvent(ofDragInfo dragInfo){

}
